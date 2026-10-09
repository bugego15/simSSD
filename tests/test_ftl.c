/*
 * S2 FTL 测试：
 *   1. 顺序写 -> 读回，lba / seq 一致
 *   2. 覆盖写：读到的是最新 seq，旧页被标记失效
 *   3. 未写过的 lba 读不到数据
 *   4. 随机写超过物理容量 -> 触发 GC -> 全量读出 100% 校验通过
 *   5. GC 后 WAF > 1 且映射自洽（搬移过的页仍能被读到）
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
static ssd_config_t make_ftl_cfg(void)
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
    c.seed                = 12345;
    c.t_prog_ns           = 1000;
    c.t_read_ns           = 100;
    c.t_bers_ns           = 2000;
    c.t_xfer_ns           = 50;
    (void)ssd_config_derive(&c);
    return c;
}

static void test_seq_write_read(void)
{
    ssd_config_t cfg = make_ftl_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t lba;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
    CHECK(ftl_user_lbas(&ftl) == (lba_t)cfg.user_pages);

    for (lba = 0; lba < 100u; lba++) {
        CHECK(ftl_write(&ftl, lba, (uint32_t)(lba + 1u)) == SSD_OK);
    }
    for (lba = 0; lba < 100u; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.lba == lba);
        CHECK(m.seq == (uint32_t)(lba + 1u));
        CHECK(m.state == (uint16_t)NAND_PAGE_VALID);
        CHECK(nand_meta_crc(m.lba, m.seq) == m.crc);
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static void test_overwrite_invalidates_old(void)
{
    ssd_config_t cfg = make_ftl_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_page_meta_t m;
    ppn_t old_ppn;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    CHECK(ftl_write(&ftl, 5u, 10u) == SSD_OK);
    old_ppn = ftl.l2p[5u];
    CHECK(old_ppn != SSD_INVALID_PPN);

    CHECK(ftl_write(&ftl, 5u, 20u) == SSD_OK);
    CHECK(ftl.l2p[5u] != old_ppn);                     /* 写到新物理页 */
    CHECK(nand_page_state(&nand, old_ppn) == (uint16_t)NAND_PAGE_INVALID);  /* 旧页失效 */

    CHECK(ftl_read(&ftl, 5u, &m) == SSD_OK);
    CHECK(m.seq == 20u);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static void test_read_unwritten(void)
{
    ssd_config_t cfg = make_ftl_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    nand_page_meta_t m;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);

    CHECK(ftl_read(&ftl, 500u, &m) != SSD_OK);
    CHECK(m.lba == SSD_INVALID_LBA);

    /* 越界 lba 必须被拒绝 */
    CHECK(ftl_write(&ftl, ftl_user_lbas(&ftl), 1u) != SSD_OK);
    CHECK(ftl_read(&ftl, ftl_user_lbas(&ftl), &m) != SSD_OK);

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static void test_random_write_with_gc(void)
{
    ssd_config_t cfg = make_ftl_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t n = 0;
    uint32_t *exp;
    ssd_rng_t rng;
    uint32_t i;
    const uint32_t writes = 5000u;
    uint32_t bad = 0u;

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

    ssd_rng_seed(&rng, 999u);

    /* 随机写：总量远超物理页数，必然触发 GC */
    for (i = 0; i < writes; i++) {
        lba_t lba = (lba_t)ssd_rng_below(&rng, (uint32_t)n);
        uint32_t seq = i + 1u;

        if (ftl_write(&ftl, lba, seq) != SSD_OK) {
            bad++;
            break;
        }
        exp[lba] = seq;
    }
    CHECK(bad == 0u);
    CHECK(ftl.gc_count > 0u);      /* 确实发生过 GC */

    /* 全量校验：写过的 lba 必须能读到最后一次写入的内容 */
    for (i = 0; i < (uint32_t)n; i++) {
        nand_page_meta_t m;

        if (exp[i] == 0u) {
            continue;
        }
        if (ftl_read(&ftl, (lba_t)i, &m) != SSD_OK) {
            bad++;
            continue;
        }
        if (m.lba != (lba_t)i || m.seq != exp[i]) {
            bad++;
        }
    }
    CHECK(bad == 0u);

    /* WAF 必须大于 1（GC 搬移带来了额外写） */
    CHECK(ssd_stats_waf() > 1.0);

    printf("  [gc] host_write=%llu nand_prog=%llu waf=%.2f gc_count=%llu copied=%llu\n",
           (unsigned long long)ssd_stats_get(ST_HOST_WRITE_PAGES),
           (unsigned long long)ssd_stats_get(ST_NAND_PROG_PAGES),
           ssd_stats_waf(),
           (unsigned long long)ftl.gc_count,
           (unsigned long long)ftl.gc_copied);

    free(exp);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

static void test_gc_mapping_consistency(void)
{
    ssd_config_t cfg = make_ftl_cfg();
    nand_dev_t nand;
    ftl_dev_t ftl;
    lba_t n;
    lba_t lba;

    ssd_clock_init();
    ssd_stats_init();
    CHECK(nand_init(&nand, &cfg, 256u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, &cfg) == SSD_OK);
    n = ftl_user_lbas(&ftl);

    /* 先把盘写满一遍，制造大量有效页 */
    for (lba = 0; lba < n; lba++) {
        CHECK(ftl_write(&ftl, lba, (uint32_t)(lba + 1u)) == SSD_OK);
    }

    /* 手工触发一次 GC，检查搬移后的映射仍然自洽 */
    CHECK(ftl_gc_one(&ftl) == SSD_OK);

    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;
        CHECK(ftl_read(&ftl, lba, &m) == SSD_OK);
        CHECK(m.lba == lba);
        CHECK(m.seq == (uint32_t)(lba + 1u));
    }

    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* 返回失败项数量，由 tests/test_main.c 汇总 */
int ftl_run(void)
{
    printf("[ftl] mapping / write path / gc tests\n");

    test_seq_write_read();
    test_overwrite_invalidates_old();
    test_read_unwritten();
    test_random_write_with_gc();
    test_gc_mapping_consistency();

    printf("[ftl] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
