/*
 * S6 收尾阶段的测试：checkpoint 增量恢复 / superblock 条带写 / 间歇型负载。
 *
 * 这一阶段的每个特性都有一个"不做会怎样"的对应故障，测试就对着它们写：
 *
 *   1. checkpoint：上电只扫快照之后动过的块（不扫全盘），数据仍要完整
 *   2. checkpoint 关掉时必须退回全盘扫，结果完全一致
 *   3. 快照里指向撕裂页的映射必须作废 —— 否则 host 会读到半页的垃圾
 *   4. 快照不能让 TRIM 掉的数据死而复生（TRIM 后没来得及下刷快照也要成立）
 *   5. superblock：一次分配整组块，组内块分属不同 CE 且批次相同
 *   6. 批量回收（后台一次收一批）不能丢数据，也不能把块池抽干
 *   7. 间歇型负载必须真的产生空闲时段，后台 GC 才有用武之地
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

/*
 * 4 个 CE（2 通道 x 2 CE），128 块 x 16 页 = 2048 物理页，用户可见 1536 页。
 * 条带化必须有多个 CE 才有意义，所以这里不能用单 CE 拓扑。
 */
static ssd_config_t make_s6_cfg(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    c.channels            = 2;
    c.ces_per_ch          = 2;
    c.dies_per_ce         = 1;
    c.planes_per_die      = 1;
    c.blocks_per_plane    = 32;
    c.pages_per_block     = 16;
    c.op_percent          = 25;
    c.factory_bb_permille = 0;
    c.fault_inject        = 0;
    c.seed                = 6060;
    c.t_prog_ns           = 1000;
    c.t_read_ns           = 100;
    c.t_bers_ns           = 2000;
    c.t_xfer_ns           = 50;
    c.cp_interval         = 64;     /* 写 64 笔下刷一次，便于在测试里触发 */
    c.striping            = 1;
    (void)ssd_config_derive(&c);
    return c;
}

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

/* 异步写的空回调：本文件只关心"提交后页还在飞"这个状态 */
static void noop_done(void *arg, int status)
{
    (void)arg;
    (void)status;
}

/* ------------------------------------------------------------------ */
/* 1. checkpoint：上电只扫活跃块                                        */
/* ------------------------------------------------------------------ */

static void test_cp_partial_scan(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t n;
    lba_t lba;
    uint32_t seq = 0u;
    uint64_t active;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 512u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    n = ftl_user_lbas(&ftl);
    for (lba = 0; lba < 600u && lba < n; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(ftl.cp_count > 0u);       /* 写够 64 笔就该下刷过快照 */

    active = nand_nv_active_count(&nand);
    CHECK(active > 0u);
    CHECK(active < nand.geo.total_blocks);

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(rec.from_checkpoint == 1u);
    CHECK(rec.scanned_blocks <= active);
    CHECK(rec.scanned_blocks < nand.geo.total_blocks);

    for (lba = 0; lba < 600u && lba < n; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.lba == lba);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 2. 关掉 checkpoint：退回全盘扫，结果必须一致                          */
/* ------------------------------------------------------------------ */

static void test_cp_disabled_full_scan(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t n;
    lba_t lba;
    uint32_t seq = 0u;

    cfg.cp_interval = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 512u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    n = ftl_user_lbas(&ftl);
    for (lba = 0; lba < 600u && lba < n; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }
    CHECK(ftl.cp_count == 0u);      /* 关掉之后一次都不该下刷 */

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(rec.from_checkpoint == 0u);
    CHECK(rec.scanned_blocks == nand.geo.total_blocks);

    for (lba = 0; lba < 600u && lba < n; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 3. 快照指向撕裂页：这条映射必须作废                                   */
/* ------------------------------------------------------------------ */

/*
 * 异步写把映射更新了、页却还在介质上飞。
 *
 * 快照必须把它当成"还没发生"（回滚到提交前的映射）：
 * 否则快照会记下"指向一个没写进去的页"的条目，掉电后那页是撕裂页，
 * 而旧版本又因为不在扫描范围里找不回来 —— 新旧两版一起丢。
 * 回滚之后掉电重建正好回到"最后一次真正落盘的版本"。
 */
static void test_cp_dangling_mapping(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;
    ppn_t ppn;
    nand_page_meta_t m;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 512u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    /* 先落一批同步写：它们必须扛得住这次掉电 */
    for (lba = 0; lba < 40u; lba++) {
        CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
    }

    /* 再提交一笔异步写，故意不推进事件，让它停在 PENDING */
    CHECK(ftl_write_async(&ftl, 100u, ++seq, &ppn, noop_done, NULL) == SSD_OK);
    CHECK(nand_page_state(&nand, ppn) == (uint16_t)NAND_PAGE_PENDING);

    CHECK(ftl_checkpoint(&ftl) == SSD_OK);   /* 快照里必然带着这条悬空映射 */

    CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);
    CHECK(rec.from_checkpoint == 1u);
    CHECK(rec.torn_pages >= 1u);
    /* 快照本身就该是干净的一致点，不该留下需要兜底清理的悬空条目 */
    CHECK(rec.cp_lbas_fixed == 0u);

    CHECK(ftl_read(&ftl, 100u, &m) != SSD_OK);   /* 没 ack 的写丢了是合法的 */

    for (lba = 0; lba < 40u; lba++) {
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 4. 快照不能让 TRIM 掉的数据死而复生                                   */
/* ------------------------------------------------------------------ */

/*
 * 两种时序都要成立：
 *   a) TRIM 之后又下刷了快照  —— 快照里本就没有这些条目
 *   b) TRIM 之后没来得及下刷  —— 快照里还有，只能靠持久化的 TRIM 位图挡住
 * (b) 才是真正危险的那一种：页里的数据实实在在躺在介质上。
 */
static void test_cp_trim_not_resurrected(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_power_loss_t loss;
    ftl_recovery_t rec;
    lba_t lba;
    uint32_t seq = 0u;
    nand_page_meta_t m;
    int i;

    for (i = 0; i < 2; i++) {
        bool checkpoint_after_trim = (i == 0);

        ssd_clock_init();
        ssd_stats_init();
        CHECK(nand_init(&nand, &cfg, 512u) == SSD_OK);
        CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

        seq = 0u;
        for (lba = 0; lba < 100u; lba++) {
            CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
        }
        CHECK(ftl_trim(&ftl, 0u, 100u) == SSD_OK);
        if (checkpoint_after_trim) {
            CHECK(ftl_checkpoint(&ftl) == SSD_OK);
        }

        CHECK(power_cycle(&nand, &ftl, &cfg, &loss, &rec) == 0);

        for (lba = 0; lba < 100u; lba++) {
            CHECK(ftl_read(&ftl, lba, &m) != SSD_OK);   /* 删掉的不能回来 */
        }

        ftl_deinit(&ftl);
        nand_deinit(&nand);
    }
}

/* ------------------------------------------------------------------ */
/* 5. superblock：一次分配整组块，组内分属不同 CE                        */
/* ------------------------------------------------------------------ */

static void test_superblock_allocation(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    uint32_t i;
    uint32_t j;
    uint32_t seq = 0u;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 512u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
    CHECK(ftl.n_open == nand_geo_ce_count(&nand.geo));

    /* 写到所有槽位都开起来（每个槽位至少分到一次） */
    for (i = 0; i < ftl.n_open * 4u; i++) {
        CHECK(ftl_write(&ftl, (lba_t)i, ++seq) == SSD_OK);
    }

    for (i = 0; i < ftl.n_open; i++) {
        CHECK(ftl.open_blocks[i] != SSD_INVALID_PBN);
    }
    /* 同一批的块必须分属不同 CE：条带写的前提就是每颗 die 都有份 */
    for (i = 0; i < ftl.n_open; i++) {
        for (j = i + 1u; j < ftl.n_open; j++) {
            CHECK(nand_geo_ce_index_of_pbn(&nand.geo, ftl.open_blocks[i]) !=
                  nand_geo_ce_index_of_pbn(&nand.geo, ftl.open_blocks[j]));
        }
    }
    /* 一次分配的块打同一个批次号，GC 才知道谁该跟谁一起收 */
    for (i = 1; i < ftl.n_open; i++) {
        CHECK(ftl.blk_sb[ftl.open_blocks[i]] ==
              ftl.blk_sb[ftl.open_blocks[0]]);
    }
    CHECK(ftl.blk_sb[ftl.open_blocks[0]] != 0u);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 6. 批量回收：不丢数据，也不抽干块池                                   */
/* ------------------------------------------------------------------ */

static void test_batch_reclaim(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t n;
    lba_t lba;
    uint32_t seq = 0u;
    uint32_t *expect;
    uint32_t round;

    cfg.bg_gc_percent = 25;     /* 空闲块低于 25% 就在后台补 */
    cfg.striping      = 1;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 4096u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    n = ftl_user_lbas(&ftl);
    expect = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    CHECK(expect != NULL);
    if (expect == NULL) {
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return;
    }

    /* 写满三遍以上，让 GC（含批量回收）反复发生 */
    for (round = 0; round < 3u; round++) {
        for (lba = 0; lba < n; lba++) {
            CHECK(ftl_write(&ftl, lba, ++seq) == SSD_OK);
            expect[lba] = seq;
        }
    }

    CHECK(ftl.gc_count > 0u);
    CHECK(ftl.bg_gc_count > 0u);        /* 确实走过后台（批量）路径 */
    CHECK(ftl_free_blocks(&ftl) >= ftl.rsv_min);   /* 池子没被抽干 */

    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;

        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.lba == lba);
        CHECK(m.seq == expect[lba]);
    }

    free(expect);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ------------------------------------------------------------------ */
/* 7. 间歇型负载：真的有空闲时段，后台 GC 才有机会                       */
/* ------------------------------------------------------------------ */

typedef struct burst_ctx {
    ssd_rng_t rng;
    lba_t     n;
    uint32_t  seq;
} burst_ctx_t;

static void burst_gen(void *arg, ssd_req_type_t *type, lba_t *lba, uint32_t *seq)
{
    burst_ctx_t *c = (burst_ctx_t *)arg;

    *type = SSD_REQ_WRITE;
    *lba  = (lba_t)ssd_rng_below(&c->rng, (uint32_t)c->n);
    *seq  = ++c->seq;
}

static void test_bursty_idle(void)
{
    ssd_config_t cfg = make_s6_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    ssd_sched_t sched;
    burst_ctx_t ctx;
    ftl_recovery_t rec;
    nand_power_loss_t loss;

    cfg.bg_gc_percent = 25;
    cfg.qdepth        = 8;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 4096u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
    CHECK(sched_init(&sched, &ftl, &nand, cfg.qdepth) == SSD_OK);

    memset(&ctx, 0, sizeof(ctx));
    ssd_rng_seed(&ctx.rng, 6061u);
    ctx.n = ftl_user_lbas(&ftl);

    /*
     * 每轮 64 个请求、然后空转 2ms。
     * 空转期间队列是空的 —— 这就是后台 GC 唯一能干活的窗口。
     */
    CHECK(sched_run_bursty(&sched, 2000u, burst_gen, &ctx,
                           64u, 2000000u, 0u) == SSD_OK);
    CHECK(sched.completed == 2000u);
    CHECK(sched.idle_ns > 0u);          /* 确实空转过 */
    CHECK(sched.idle_gc_count > 0u);    /* 空转时确实回收过 */
    CHECK(ftl.bg_gc_count > 0u);

    /* 空转时段做的回收不能影响数据正确性 */
    {
        lba_t lba;
        uint32_t bad = 0u;

        for (lba = 0; lba < ctx.n; lba++) {
            nand_page_meta_t m;

            if (ftl_read(&ftl, lba, &m) != SSD_OK) {
                continue;   /* 没写过 */
            }
            if (m.lba != lba) {
                bad++;
            }
        }
        CHECK(bad == 0u);
    }

    sched_deinit(&sched);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
    (void)rec;
    (void)loss;
}

/* ------------------------------------------------------------------ */

int s6_tests_run(void)
{
    printf("[s6] checkpoint / superblock / bursty workload\n");

    test_cp_partial_scan();
    test_cp_disabled_full_scan();
    test_cp_dangling_mapping();
    test_cp_trim_not_resurrected();
    test_superblock_allocation();
    test_batch_reclaim();
    test_bursty_idle();

    printf("[s6] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
