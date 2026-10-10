/*
 * S5 SPOR（Sudden Power-Off Recovery）测试。
 *
 * 断电恢复的正确性底线只有一条：
 *   凡是 host 已经收到完成通知的写，上电之后必须还在，而且是最新的那一版。
 * 反过来，还在盘上飞的写丢掉是合法的 —— host 根本没收到回执，
 * 真实盘也是这样（未完成命令会被 host 重发）。
 *
 * 围绕这条底线，这里覆盖的是"重建容易做错"的几处：
 *   1. 干净断电：什么都没断在半路，重建必须原样还原
 *   2. 撕裂页：半写的页不能当成有效数据，也不能把旧版本一起带走
 *   3. 擦除中断：擦到一半的块整块不可信，必须能被重新回收
 *   4. TRIM 语义：持久化不到位，掉电后删掉的数据会死而复生
 *   5. 空间守恒：重建不能凭空多吃块、也不能把半满的块混进空闲池
 *   6. GC 中途断电：搬移进行到一半时断电，映射不能出现两个有效版本
 *   7. 反复断电：恢复出来的状态必须能继续被恢复
 */

#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/rng.h"
#include "core/stats.h"
#include "ftl/ftl.h"
#include "ftl/sched.h"
#include "media/geometry.h"
#include "media/nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                              \
        }                                                            \
    } while (0)

/* 64 blocks x 16 pages = 1024 物理页，用户可见 768 页 */
static ssd_config_t make_spor_cfg(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    c.channels            = 1;
    c.ces_per_ch          = 1;
    c.dies_per_ce         = 1;
    c.planes_per_die      = 1;
    c.blocks_per_plane    = 64;
    c.pages_per_block     = 16;
    c.op_percent          = 25;
    c.factory_bb_permille = 0;
    c.fault_inject        = 0;
    c.seed                = 20240;
    c.t_prog_ns           = 1000;
    c.t_read_ns           = 100;
    c.t_bers_ns           = 2000;
    c.t_xfer_ns           = 50;
    (void)ssd_config_derive(&c);
    return c;
}

/*
 * 一次完整的掉电 + 上电。
 * 刻意走 deinit + init：FTL 的 DRAM 状态必须真的全部丢掉，
 * 否则测试是被"残留状态"帮着过的。
 */
static int power_cycle(nand_dev_t *nd, ftl_dev_t *ftl,
                       const ssd_config_t *cfg,
                       nand_power_loss_t *loss, ftl_recovery_t *rec)
{
    nand_power_cut(nd, loss);
    ftl_deinit(ftl);
    if (ftl_init(ftl, nd, cfg) != SSD_OK) {
        return -1;
    }
    if (ftl_recover(ftl, rec) != SSD_OK) {
        return -2;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 1. 干净断电                                                         */
/* ------------------------------------------------------------------ */

static void test_spor_clean_cut(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    for (lba = 0; lba < 200u; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(ftl_flush(&ftl) == SSD_OK);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(loss.torn_progs == 0u);       /* 同步写早就落地了 */
    CHECK(loss.dropped_ops == 0u);
    CHECK(rec.torn_pages == 0u);
    CHECK(rec.mapped_lbas == 200u);
    CHECK(rec.write_seq > 200u);

    for (lba = 0; lba < 200u; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.lba == lba);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 2. 撕裂页：新页写了半截，旧页必须还在                                */
/* ------------------------------------------------------------------ */

/*
 * 这是整个 S5 最关键的一条。
 *
 * 覆盖写的顺序是"新页写进去之后，旧页才失效"，而失效只在 DRAM 里记。
 * 掉电时如果新页写了一半，介质上实际是：旧页完好、新页一塌糊涂。
 * 重建必须回到旧版本。
 *
 * 若把介质上的 INVALID 标记当成"这一页作废"来用，旧页就会被跳过，
 * 于是这个 lba 凭空消失 —— 上一版数据明明好好地躺在盘上。
 */
static void test_spor_torn_page(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    nand_page_meta_t m;
    ppn_t ppn = SSD_INVALID_PPN;
    pbn_t pbn;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    /* 第一版：完整落地 */
    CHECK(ftl_write(&ftl, 7u, 100u) == SSD_OK);

    /* 第二版：只提交，不推进时钟，让它死在半路 */
    CHECK(ftl_write_async(&ftl, 7u, 200u, &ppn, NULL, NULL) == SSD_OK);
    CHECK(ppn != SSD_INVALID_PPN);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(loss.torn_progs == 1u);
    CHECK(rec.torn_pages == 1u);

    /* 回到上一个已经确认的版本，而不是"没有数据" */
    CHECK(ftl_read(&ftl, 7u, &m) == SSD_OK);
    CHECK(m.lba == 7u);
    CHECK(m.seq == 100u);

    /*
     * 撕裂页所在的块必须关闭：那颗 die 刚被断电打断过，
     * 继续往里写等于把 host 数据放在可靠性没有保证的位置上。
     */
    pbn = nand_geo_pbn_of_ppn(&nand.geo, ppn);
    CHECK(nand_block_next_page(&nand, pbn) == cfg.pages_per_block);
    CHECK(nand_block_state(&nand, pbn) == (uint16_t)NAND_BLK_CLOSED);

    /* 盘还得能继续用 */
    CHECK(ftl_write(&ftl, 8u, 300u) == SSD_OK);
    CHECK(ftl_read(&ftl, 8u, &m) == SSD_OK);
    CHECK(m.seq == 300u);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 3. 写序号不回退                                                     */
/* ------------------------------------------------------------------ */

static void test_spor_seq_monotonic(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;
    uint64_t seq_after_first;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    for (lba = 0; lba < 300u; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    seq_after_first = rec.write_seq;
    CHECK(seq_after_first > 300u);

    /* 继续写，再掉一次电：序号基线必须继续往前走 */
    for (lba = 0; lba < 100u; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(rec.write_seq > seq_after_first);
    CHECK(rec.write_seq > 400u);

    /* 数据仍然是第二次写的版本 */
    for (lba = 0; lba < 100u; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(300u + lba + 1u));
    }
    /* 没被覆盖的后半段保持第一次的值 */
    for (lba = 100u; lba < 300u; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 4. TRIM 不能死而复生                                                */
/* ------------------------------------------------------------------ */

static void test_spor_trim_survives(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    for (lba = 0; lba < 128u; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(ftl_trim(&ftl, 0u, 64u) == SSD_OK);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);

    /*
     * TRIM 过的地址必须仍然读不到数据。
     * 页里的内容其实还在介质上，重建是"扫介质建映射"——
     * 若没有持久化这条删除记录，这些页会被原样映射回来。
     */
    for (lba = 0; lba < 64u; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_ERR_NO_SPACE);
    }
    for (lba = 64u; lba < 128u; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    /* TRIM 过的地址可以重新写，写了就必须读得到 */
    CHECK(ftl_write(&ftl, 3u, 999u) == SSD_OK);
    {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, 3u, &m) == SSD_OK);
        CHECK(m.seq == 999u);
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 5. 空间守恒                                                         */
/* ------------------------------------------------------------------ */

static void test_spor_space_accounting(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;
    pbn_t pbn;
    uint32_t free_b = 0u, open_b = 0u, closed_b = 0u, bad_b = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    /* 写到超出用户容量，逼出前台 GC */
    for (lba = 0; lba < 900u; lba++) {
        CHECK(ftl_write(&ftl, (lba_t)((lba * 37u) % ftl_user_lbas(&ftl)),
                        ++seq) == SSD_OK);
    }

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);

    for (pbn = 0; pbn < nand.geo.total_blocks; pbn++) {
        uint16_t st = nand_block_state(&nand, pbn);

        if (st == (uint16_t)NAND_BLK_BAD) {
            bad_b++;
        } else if (st == (uint16_t)NAND_BLK_OPEN) {
            open_b++;
        } else if (st == (uint16_t)NAND_BLK_CLOSED) {
            closed_b++;
        } else {
            free_b++;
            /* 池里的块必须是真干净的，否则新数据会写进别人的旧页上 */
            CHECK(nand_block_valid_pages(&nand, pbn) == 0u);
            CHECK(nand_block_next_page(&nand, pbn) == 0u);
        }
    }
    CHECK(free_b + open_b + closed_b + bad_b == (uint32_t)nand.geo.total_blocks);
    CHECK(free_b == ftl_free_blocks(&ftl));

    /* 每个 CE 最多一个写入块，且块内有效页计数必须与介质一致 */
    CHECK(open_b <= nand_ce_count(&nand));
    for (pbn = 0; pbn < nand.geo.total_blocks; pbn++) {
        uint32_t v = 0u;
        uint32_t i;
        ppn_t base = nand_geo_block_base(&nand.geo, pbn);

        for (i = 0; i < cfg.pages_per_block; i++) {
            if (nand_page_state(&nand, base + i) == (uint16_t)NAND_PAGE_VALID) {
                v++;
            }
        }
        CHECK(nand_block_valid_pages(&nand, pbn) == v);
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 6. 擦除中断                                                         */
/* ------------------------------------------------------------------ */

static void test_spor_torn_erase(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    pbn_t victim;
    nand_page_meta_t m;
    uint32_t guard;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    /* 先写点数据，让 GC 有活可干 */
    {
        lba_t lba;
        uint32_t seq = 0u;

        for (lba = 0; lba < 200u; lba++) {
            CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
        }
    }

    /*
     * 从池里取一块发起异步擦除，但不推进时钟 —— 让擦除死在半路。
     * 真实盘上这块的内容处于任意中间态，固件唯一能做的是重新擦一遍。
     */
    victim = ftl_take_gc_block(&ftl);
    CHECK(victim != SSD_INVALID_PBN);
    CHECK(nand_erase_async(&nand, victim, NULL, NULL) == SSD_OK);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(loss.torn_erases == 1u);

    /* 整块被判成垃圾：不能进空闲池，得等 GC 重新擦一遍 */
    CHECK(nand_block_state(&nand, victim) == (uint16_t)NAND_BLK_CLOSED);
    CHECK(nand_block_valid_pages(&nand, victim) == 0u);
    {
        uint32_t i;
        ppn_t base = nand_geo_block_base(&nand.geo, victim);

        for (i = 0; i < cfg.pages_per_block; i++) {
            CHECK(nand_page_state(&nand, base + i) == (uint16_t)NAND_PAGE_INVALID);
        }
    }

    /* 数据不能因为一次擦除中断就少一块可用空间：继续写应当仍然顺畅 */
    for (guard = 0u; guard < 100u; guard++) {
        if (ftl_write(&ftl, (lba_t)(300u + guard), 5000u + guard) != SSD_OK) {
            break;
        }
    }
    CHECK(guard == 100u);
    CHECK(ftl_read(&ftl, 300u, &m) == SSD_OK);
    CHECK(m.seq == 5000u);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 7. 同一 lba 的两个副本：只能留最新的那个                              */
/* ------------------------------------------------------------------ */

/*
 * GC 搬移到一半断电时，同一 lba 会在介质上留下两份副本
 *（src 还没擦、dst 已经写进去），而且它们的 seq 可能完全相同。
 * 重建必须为每一份都做出**同一个**选择：
 * 选错内容不会报错，但映射会静默退回旧版本；选得不确定则盘的行为不可复现。
 */
static void test_spor_version_selection(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    nand_page_meta_t m;
    pbn_t b0;
    pbn_t b1;
    ppn_t p0;
    ppn_t p1;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    b0 = ftl_take_free_block(&ftl);
    b1 = ftl_take_free_block(&ftl);
    CHECK(b0 != SSD_INVALID_PBN);
    CHECK(b1 != SSD_INVALID_PBN);
    CHECK(b0 != b1);

    p0 = nand_geo_block_base(&nand.geo, b0);
    p1 = nand_geo_block_base(&nand.geo, b1);

    /* 旧版本 */
    m.lba   = 1u;
    m.seq   = 10u;
    m.state = (uint16_t)NAND_PAGE_VALID;
    nand_meta_seal(&m);
    CHECK(nand_prog_sync(&nand, p0, &m) == SSD_OK);

    /* 新版本：落在另一个块上（GC 搬移就是这个形态） */
    m.lba   = 1u;
    m.seq   = 20u;
    m.state = (uint16_t)NAND_PAGE_VALID;
    nand_meta_seal(&m);
    CHECK(nand_prog_sync(&nand, p1, &m) == SSD_OK);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);

    CHECK(ftl_read(&ftl, 1u, &m) == SSD_OK);
    CHECK(m.lba == 1u);
    CHECK(m.seq == 20u);

    /* 旧副本判成垃圾，计数也要跟着走 */
    CHECK(nand_page_state(&nand, p0) == (uint16_t)NAND_PAGE_INVALID);
    CHECK(nand_page_state(&nand, p1) == (uint16_t)NAND_PAGE_VALID);
    CHECK(nand_block_valid_pages(&nand, b0) == 0u);
    CHECK(nand_block_valid_pages(&nand, b1) == 1u);

    /* 再掉一次电：选择必须是同一个（重写一遍也不许换页） */
    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(ftl_read(&ftl, 1u, &m) == SSD_OK);
    CHECK(m.seq == 20u);
    CHECK(nand_page_state(&nand, p1) == (uint16_t)NAND_PAGE_VALID);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 8. GC 中途断电 + 反复断电                                            */
/* ------------------------------------------------------------------ */

typedef struct spor_ctx {
    uint32_t  seq;
    lba_t     n;
    uint32_t *ack;
} spor_ctx_t;

static void spor_gen(void *arg, ssd_req_type_t *type, lba_t *lba, uint32_t *seq)
{
    spor_ctx_t *c = (spor_ctx_t *)arg;

    *type = SSD_REQ_WRITE;
    *lba  = (lba_t)((c->seq * 37u + 11u) % (uint32_t)c->n);
    c->seq++;
    *seq  = c->seq;
}

/* 与 main.c 的 bench 同一套语义：只认"已确认完成"里最大的 seq */
static void spor_done(void *arg, ssd_req_type_t type, lba_t lba,
                      uint32_t seq, int status)
{
    spor_ctx_t *c = (spor_ctx_t *)arg;

    if (type != SSD_REQ_WRITE || status != SSD_OK) {
        return;
    }
    if (seq > c->ack[lba]) {
        c->ack[lba] = seq;
    }
}

static uint64_t spor_verify(const ftl_dev_t *ftl, const uint32_t *ack, lba_t n)
{
    lba_t lba;
    uint64_t bad = 0u;

    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;

        if (ack[lba] == 0u) {
            continue;
        }
        if (ftl_read((ftl_dev_t *)ftl, lba, &m) != SSD_OK) {
            bad++;
            continue;
        }
        if (m.lba != lba || m.seq != ack[lba]) {
            bad++;
        }
    }
    return bad;
}

static void test_spor_repeated_cuts(void)
{
    ssd_config_t cfg = make_spor_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    ssd_sched_t sched;
    spor_ctx_t ctx;
    lba_t n;
    uint32_t *ack;
    uint32_t round;
    uint64_t cuts = 0u;
    uint64_t torn = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    n = ftl_user_lbas(&ftl);
    ack = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    CHECK(ack != NULL);
    if (ack == NULL) {
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.n   = n;
    ctx.ack = ack;

    CHECK(sched_init(&sched, &ftl, &nand, 8u) == SSD_OK);
    sched_set_done_cb(&sched, spor_done, &ctx);

    /*
     * 反复"跑一段 -> 断电 -> 重建 -> 继续"，每段都在队列还没排空时断电，
     * 逼着它每次都要处理撕裂页，而且 GC 很可能正搬移到一半。
     */
    for (round = 0u; round < 12u; round++) {
        nand_power_loss_t loss;
        ftl_recovery_t rec;

        (void)sched_run(&sched, 120u, spor_gen, &ctx, 112u);
        CHECK(spor_verify(&ftl, ack, n) == 0u);
        /* 断电前把队列补满，保证这次断电真能打断页编程 */
        CHECK(sched_fill(&sched, spor_gen, &ctx, 8u) > 0u);
        CHECK(sched_inflight(&sched) > 0u);

        nand_power_cut(&nand, &loss);
        ftl_deinit(&ftl);
        CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
        CHECK(ftl_recover(&ftl, &rec) == SSD_OK);

        /* 恢复出来的映射必须立刻可用 */
        CHECK(spor_verify(&ftl, ack, n) == 0u);

        sched_deinit(&sched);
        CHECK(sched_init(&sched, &ftl, &nand, 8u) == SSD_OK);
        sched_set_done_cb(&sched, spor_done, &ctx);

        cuts++;
        torn += loss.torn_progs;
    }

    /* 最后一段正常收尾 */
    (void)sched_run(&sched, 120u, spor_gen, &ctx, 0u);

    CHECK(cuts == 12u);
    CHECK(torn > 0u);   /* 确实打断了页编程，不是每次都挑空闲时刻断电 */
    CHECK(spor_verify(&ftl, ack, n) == 0u);

    /* 恢复多次之后，盘仍然健康：空闲块没有耗尽 */
    CHECK(ftl_free_blocks(&ftl) > 0u);

    sched_deinit(&sched);
    free(ack);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */

int spor_tests_run(void)
{
    printf("[spor] S5 power-loss recovery\n");

    test_spor_clean_cut();
    test_spor_torn_page();
    test_spor_seq_monotonic();
    test_spor_trim_survives();
    test_spor_space_accounting();
    test_spor_torn_erase();
    test_spor_version_selection();
    test_spor_repeated_cuts();

    printf("  %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
