/*
 * S3 FTL 测试：TRIM / 磨损均衡 / 坏块管理 / GC 策略
 *
 * 每个用例都围绕一个"可验证的行为"，而不是复述实现：
 *   - TRIM：被丢弃的 lba 读不到、页在介质上失效、且能降低 GC 搬移量
 *   - 动态 WL：分配时确实挑了 P/E 次数低的块
 *   - 静态 WL：冷数据迁移之后，数据仍然读得出来
 *   - 坏块：隔离后块退出服务，但有效数据被抢救出来
 *   - 故障注入：program 失败可自愈，已落盘数据不丢
 */

#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/rng.h"
#include "core/stats.h"
#include "ftl/ftl.h"
#include "media/nand.h"

#include <stdio.h>
#include <stdlib.h>

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

/* 小规模拓扑：64 blocks x 16 pages = 1024 物理页 */
static ssd_config_t make_cfg(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    c.channels            = 1;
    c.ces_per_ch          = 1;
    c.dies_per_ce         = 1;
    c.planes_per_die      = 1;
    c.blocks_per_plane    = 64;
    c.pages_per_block     = 16;
    c.op_percent          = 7;
    c.factory_bb_permille = 0;
    c.fault_inject        = 0;
    c.seed                = 4242u;
    c.t_prog_ns           = 1000;
    c.t_read_ns           = 100;
    c.t_bers_ns           = 2000;
    c.t_xfer_ns           = 50;
    /* 单测要求确定性：关掉后台 GC，WL 打开 */
    c.bg_gc_percent       = 0;
    c.wl_enable           = 1;
    c.wl_pe_thresh        = 30u;
    (void)ssd_config_derive(&c);
    return c;
}

/* ---------------- TRIM ---------------- */

static void test_trim_basic(void)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t lba;
    ppn_t victim_ppn;
    int rc;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    for (lba = 0; lba < 100u; lba++) {
        CHECK(ftl_write(&ftl, lba, (uint32_t)(lba + 1u)) == SSD_OK);
    }

    victim_ppn = ftl.l2p[20u];
    CHECK(victim_ppn != SSD_INVALID_PPN);

    /* 丢弃 lba 20..59 */
    rc = ftl_trim(&ftl, 20u, 40u);
    CHECK(rc == SSD_OK);
    CHECK(ftl.trim_cmds == 1u);
    CHECK(ftl.trim_pages == 40u);
    /* 介质上这些页应立刻变成失效页，GC 不必再搬它们 */
    CHECK(nand_page_state(&nand, victim_ppn) == (uint16_t)NAND_PAGE_INVALID);

    /* 被丢弃的范围读不到数据 */
    for (lba = 20u; lba < 60u; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) != SSD_OK);
    }
    /* 范围之外完好无损 */
    for (lba = 0; lba < 20u; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }
    for (lba = 60u; lba < 100u; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    /* 越界 trim 应被拒绝而不是写坏别的地址 */
    CHECK(ftl_trim(&ftl, ftl_user_lbas(&ftl), 8u) != SSD_OK);
    CHECK(ftl_trim(&ftl, 0u, 0u) != SSD_OK);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static uint64_t run_mixed(bool use_trim)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    ssd_rng_t rng;
    lba_t n;
    uint32_t i;
    uint64_t copied;

    ssd_clock_init();
    ssd_stats_init();
    if (nand_init(&nand, &cfg, 256u) != SSD_OK) {
        return 0;
    }
    if (ftl_init(&ftl, &nand, &cfg) != SSD_OK) {
        nand_deinit(&nand);
        return 0;
    }
    n = ftl_user_lbas(&ftl);
    ssd_rng_seed(&rng, 777u);

    for (i = 0; i < 3000u; i++) {
        lba_t lba = (lba_t)ssd_rng_below(&rng, (uint32_t)n);

        (void)ftl_write(&ftl, lba, i + 1u);

        /* 每 20 次写丢弃一小段，模拟文件系统删除文件 */
        if (use_trim && ((i % 20u) == 19u)) {
            lba_t t = (lba_t)ssd_rng_below(&rng, (uint32_t)n);
            uint32_t cnt = 32u;

            if ((uint64_t)t + cnt > (uint64_t)n) {
                cnt = (uint32_t)((uint64_t)n - t);
            }
            (void)ftl_trim(&ftl, t, cnt);
        }
    }

    copied = ftl.gc_copied;
    ftl_deinit(&ftl);
    nand_deinit(&nand);
    return copied;
}

static void test_trim_reduces_gc(void)
{
    uint64_t without_trim = run_mixed(false);
    uint64_t with_trim    = run_mixed(true);

    /* TRIM 让 GC 少搬一堆"host 早就不想要"的数据，搬移量必须下降 */
    CHECK(with_trim < without_trim);
    printf("  [trim] gc copied: no-trim=%llu  with-trim=%llu\n",
           (unsigned long long)without_trim, (unsigned long long)with_trim);
}

/* ---------------- 磨损均衡 ---------------- */

/*
 * 人为把池里每个块的 P/E 拉开：pbn i 擦 i 次，于是 PE[i] = i，池平均 31.5。
 * 取若干块，看拿到手的块平均磨损是多少。
 * 开 WL 应该明显偏向低 P/E 的块；关 WL 就只会拿栈顶那几个（磨损最高）。
 */
static double take_avg_pe(bool wl_enable, uint32_t count)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    uint32_t i;
    uint32_t k;
    double sum = 0.0;
    uint32_t got = 0u;

    cfg.wl_enable = wl_enable ? 1u : 0u;

    ssd_clock_init();
    ssd_stats_init();
    if (nand_init(&nand, &cfg, 256u) != SSD_OK) {
        return -1.0;
    }
    if (ftl_init(&ftl, &nand, &cfg) != SSD_OK) {
        nand_deinit(&nand);
        return -1.0;
    }

    for (i = 0; i < nand.geo.total_blocks; i++) {
        for (k = 0; k < i; k++) {
            (void)nand_erase_sync(&nand, (pbn_t)i);
        }
    }

    for (k = 0; k < count; k++) {
        pbn_t pbn = ftl_take_free_block(&ftl);
        if (pbn == SSD_INVALID_PBN) {
            break;
        }
        sum += (double)nand_pe_count(&nand, pbn);
        got++;
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
    return (got > 0u) ? (sum / (double)got) : -1.0;
}

static void test_dynamic_wl(void)
{
    double on  = take_avg_pe(true, 8u);
    double off = take_avg_pe(false, 8u);

    CHECK(on >= 0.0);
    CHECK(off >= 0.0);
    /* 池平均约 31.5：开了 WL，拿到手的块必须比"随便拿"更年轻 */
    CHECK(on < off);
    CHECK(on < 31.5);

    printf("  [wl] dynamic: avg pe of taken blocks  wl_on=%.1f  wl_off=%.1f (pool avg 31.5)\n",
           on, off);
}

static void test_static_wl(void)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t n;
    lba_t lba;
    uint32_t k;
    uint32_t e;
    int rc;

    cfg.wl_pe_thresh = 5u;   /* PE 差超过 5 就搬冷数据 */

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
    n = ftl_user_lbas(&ftl);

    /* 写满一遍用户容量，制造大量有效数据 */
    for (lba = 0; lba < n; lba++) {
        CHECK(ftl_write(&ftl, lba, (uint32_t)(lba + 1u)) == SSD_OK);
    }

    /* 把池里剩下的块擦很多次，人为拉开磨损差距：
     * 这就是"冷数据占着新块、热数据反复擦旧块"的缩影 */
    for (k = 0; k < ftl.free_top; k++) {
        for (e = 0; e < 10u; e++) {
            CHECK(nand_erase_sync(&nand, ftl.free_stack[k]) == SSD_OK);
        }
    }

    rc = ftl_wl_static_once(&ftl);
    CHECK(rc == SSD_OK);
    CHECK(ftl.wl_migrations == 1u);
    CHECK(ftl.wl_migrated_pages > 0u);

    /* 迁移之后，所有数据必须仍然读得出来 */
    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    printf("  [wl] static migration: %llu pages\n",
           (unsigned long long)ftl.wl_migrated_pages);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ---------------- 坏块管理 ---------------- */

static void test_badblock_isolate(void)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    pbn_t pbn;
    uint32_t i;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    for (i = 0; i < 100u; i++) {
        CHECK(ftl_write(&ftl, (lba_t)i, (uint32_t)(i + 1u)) == SSD_OK);
    }

    pbn = nand_geo_pbn_of_ppn(&nand.geo, ftl.l2p[0]);
    CHECK(ftl_isolate_bad_block(&ftl, pbn) == SSD_OK);

    /* 坏块必须退出服务：状态为 BAD，且绝不出现在空闲池里 */
    CHECK(nand_block_state(&nand, pbn) == (uint16_t)NAND_BLK_BAD);
    CHECK(ftl.bad_blocks == 1u);
    for (i = 0; i < ftl.free_top; i++) {
        CHECK(ftl.free_stack[i] != pbn);
    }

    /* 块里的有效数据必须被抢救出来 */
    for (i = 0; i < 100u; i++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, (lba_t)i, &m) == SSD_OK);
        CHECK(m.seq == (uint32_t)(i + 1u));
    }

    printf("  [bb] isolated pbn=%u, saved %u pages\n",
           (unsigned)pbn, ftl.bb_saved_pages);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static void test_fault_no_data_loss(void)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    ssd_rng_t rng;
    uint32_t *exp;
    lba_t n;
    lba_t lba;
    uint32_t i;
    uint64_t bad = 0u;

    /* 只注入 program/erase 故障：这是固件必须自愈的路径 */
    cfg.fault_inject        = 1u;
    cfg.fault_rate_permille = 10u;   /* 1% */

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    n = ftl_user_lbas(&ftl);
    exp = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    CHECK(exp != NULL);
    if (exp == NULL) {
        return;
    }

    ssd_rng_seed(&rng, 31337u);
    for (i = 0; i < 2000u; i++) {
        lba_t l = (lba_t)ssd_rng_below(&rng, (uint32_t)n);

        if (ftl_write(&ftl, l, i + 1u) != SSD_OK) {
            bad++;
            break;
        }
        exp[l] = i + 1u;
    }
    CHECK(bad == 0u);   /* 编程失败必须被重映射消化掉，不能让 host 写失败 */

    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;

        if (exp[lba] == 0u) {
            continue;
        }
        if (ftl_read(&ftl, lba, &m) != SSD_OK) {
            bad++;
            continue;
        }
        if (m.lba != lba || m.seq != exp[lba]) {
            bad++;
        }
    }
    CHECK(bad == 0u);   /* 已落盘的数据一个都不能错 */

    printf("  [fault] inject=1 rate=10/1000: bad_blocks=%u, verify_bad=%llu\n",
           ftl.bad_blocks, (unsigned long long)bad);

    free(exp);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ---------------- GC 策略 ---------------- */

static uint64_t run_hotspot(uint32_t policy)
{
    ssd_config_t cfg = make_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    ssd_rng_t rng;
    uint32_t *exp;
    lba_t n;
    lba_t hot;
    lba_t lba;
    uint32_t i;
    uint64_t bad = 0u;

    cfg.gc_policy = policy;

    ssd_clock_init();
    ssd_stats_init();
    if (nand_init(&nand, &cfg, 256u) != SSD_OK) {
        return 1u;
    }
    if (ftl_init(&ftl, &nand, &cfg) != SSD_OK) {
        nand_deinit(&nand);
        return 1u;
    }

    n = ftl_user_lbas(&ftl);
    exp = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    if (exp == NULL) {
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return 1u;
    }

    /* 80% 的访问打在前 20% 地址上，其余均匀散布 */
    hot = (lba_t)((uint64_t)n * 20u / 100u);
    ssd_rng_seed(&rng, 909u);
    for (i = 0; i < 4000u; i++) {
        lba_t l;

        if (ssd_rng_chance_permille(&rng, 800u)) {
            l = (lba_t)ssd_rng_below(&rng, (uint32_t)hot);
        } else {
            l = hot + (lba_t)ssd_rng_below(&rng, (uint32_t)(n - hot));
        }
        if (ftl_write(&ftl, l, i + 1u) != SSD_OK) {
            bad++;
            break;
        }
        exp[l] = i + 1u;
    }

    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;

        if (exp[lba] == 0u) {
            continue;
        }
        if (ftl_read(&ftl, lba, &m) != SSD_OK) {
            bad++;
            continue;
        }
        if (m.lba != lba || m.seq != exp[lba]) {
            bad++;
        }
    }

    printf("  [gc] %-13s copied=%llu gc=%llu bad=%llu\n",
           (policy == 1u) ? "cost-benefit" : "greedy",
           (unsigned long long)ftl.gc_copied,
           (unsigned long long)ftl.gc_count,
           (unsigned long long)bad);

    free(exp);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
    return bad;
}

static void test_gc_policy_hotspot(void)
{
    CHECK(run_hotspot(0u) == 0u);
    CHECK(run_hotspot(1u) == 0u);
}

/* 返回失败项数量，由 tests/test_main.c 汇总 */
int ftl_s3_run(void)
{
    printf("[ftl-s3] trim / wear leveling / bad block / gc policy\n");

    test_trim_basic();
    test_trim_reduces_gc();
    test_dynamic_wl();
    test_static_wl();
    test_badblock_isolate();
    test_fault_no_data_loss();
    test_gc_policy_hotspot();

    printf("[ftl-s3] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
