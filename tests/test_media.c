/*
 * S1 介质层测试：
 *   1. 地址编解码 round-trip
 *   2. program -> read 数据一致且 CRC 自检通过
 *   3. 虚拟时钟推进量与时序参数误差为 0
 *   4. NAND 不允许原地改写
 *   5. erase 后页回到 FREE 且 PE 计数递增
 *   6. 坏块拒绝 program / erase
 *   7. 编程中的页不可读
 *   8. 出厂坏块比例符合配置
 */

#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/eventq.h"
#include "media/geometry.h"
#include "media/nand.h"

#include <stdio.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            g_failed++;                                               \
        }                                                             \
    } while (0)

/* 小规模确定性拓扑 */
static ssd_config_t make_test_cfg(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    c.channels           = 1;
    c.ces_per_ch         = 1;
    c.dies_per_ce        = 1;
    c.planes_per_die     = 1;
    c.blocks_per_plane   = 8;
    c.pages_per_block    = 4;
    c.t_prog_ns          = 1000;
    c.t_read_ns          = 100;
    c.t_bers_ns          = 2000;
    c.t_xfer_ns          = 50;
    c.factory_bb_permille = 0;
    c.fault_inject       = 0;
    c.seed               = 42;
    (void)ssd_config_derive(&c);
    return c;
}

static nand_page_meta_t make_meta(lba_t lba, uint32_t seq)
{
    nand_page_meta_t m;

    m.lba   = lba;
    m.seq   = seq;
    m.state = (uint16_t)NAND_PAGE_VALID;
    m.crc   = 0;
    nand_meta_seal(&m);
    return m;
}

static void test_geometry(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_geometry_t g;
    ppn_t ppn;

    nand_geo_init(&g, &cfg);
    CHECK(g.total_blocks == 8u);
    CHECK(g.total_pages == 32u);

    for (ppn = 0; ppn < g.total_pages; ppn++) {
        nand_addr_t a;
        nand_geo_decode(&g, ppn, &a);
        CHECK(nand_geo_ppn(&g, &a) == ppn);
        CHECK(nand_geo_pbn_of_ppn(&g, ppn) == ppn / cfg.pages_per_block);
        CHECK(nand_geo_channel_of_ppn(&g, ppn) == 0u);
    }
}

static void test_prog_read_timing(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_dev_t dev;
    nand_page_meta_t m = make_meta(7u, 1u);
    nand_page_meta_t r;
    uint64_t t0;
    uint64_t dt;

    ssd_clock_init();
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);

    /* 编程：时钟推进量必须精确等于 t_xfer + t_prog */
    t0 = ssd_clock_now();
    CHECK(nand_prog_sync(&dev, 5u, &m) == SSD_OK);
    dt = ssd_clock_now() - t0;
    CHECK(dt == (uint64_t)cfg.t_xfer_ns + cfg.t_prog_ns);

    /* 读回：元数据一致且自检通过 */
    CHECK(nand_read_sync(&dev, 5u, &r) == SSD_OK);
    CHECK(r.lba == 7u);
    CHECK(r.seq == 1u);
    CHECK(r.state == (uint16_t)NAND_PAGE_VALID);
    CHECK(nand_meta_crc(r.lba, r.seq, r.state) == r.crc);

    /* 读：t_xfer + t_read */
    t0 = ssd_clock_now();
    CHECK(nand_read_sync(&dev, 5u, &r) == SSD_OK);
    dt = ssd_clock_now() - t0;
    CHECK(dt == (uint64_t)cfg.t_xfer_ns + cfg.t_read_ns);

    /* 原地改写必须被拒绝 */
    CHECK(nand_prog_sync(&dev, 5u, &m) == SSD_ERR_IO);

    /* 未写过的页可读，状态为 FREE */
    CHECK(nand_read_sync(&dev, 6u, &r) == SSD_OK);
    CHECK(r.state == (uint16_t)NAND_PAGE_FREE);
    CHECK(r.lba == SSD_INVALID_LBA);

    nand_deinit(&dev);
}

static void test_erase(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_dev_t dev;
    nand_page_meta_t m = make_meta(11u, 2u);
    pbn_t pbn = 1u;   /* ppn 5 属于 block 1（每块 4 页） */

    ssd_clock_init();
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);
    CHECK(nand_prog_sync(&dev, 5u, &m) == SSD_OK);
    CHECK(nand_block_valid_pages(&dev, pbn) == 1u);

    {
        uint64_t t0 = ssd_clock_now();
        CHECK(nand_erase_sync(&dev, pbn) == SSD_OK);
        CHECK(ssd_clock_now() - t0 == (uint64_t)cfg.t_bers_ns);
    }

    CHECK(nand_pe_count(&dev, pbn) == 1u);
    CHECK(nand_page_state(&dev, 5u) == (uint16_t)NAND_PAGE_FREE);
    CHECK(nand_block_state(&dev, pbn) == (uint16_t)NAND_BLK_FREE);
    CHECK(nand_block_valid_pages(&dev, pbn) == 0u);

    /* 擦除之后可以重新写入 */
    CHECK(nand_prog_sync(&dev, 5u, &m) == SSD_OK);

    nand_deinit(&dev);
}

static void test_bad_block(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_dev_t dev;
    nand_page_meta_t m = make_meta(3u, 1u);
    pbn_t pbn = 2u;

    ssd_clock_init();
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);

    dev.blocks[pbn].state = (uint16_t)NAND_BLK_BAD;
    CHECK(nand_prog_sync(&dev, pbn * cfg.pages_per_block, &m) == SSD_ERR_BADBLK);
    CHECK(nand_erase_sync(&dev, pbn) == SSD_ERR_BADBLK);

    nand_deinit(&dev);
}

static void test_pending_page(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_dev_t dev;
    nand_page_meta_t m = make_meta(21u, 5u);
    nand_page_meta_t r;

    ssd_clock_init();
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);

    /* 异步提交后立刻读：页处于编程中，应返回 BUSY */
    CHECK(nand_prog_async(&dev, 20u, &m, NULL, NULL) == SSD_OK);
    CHECK(nand_read_async(&dev, 20u, &r, NULL, NULL) == SSD_ERR_BUSY);

    nand_run_until_idle(&dev);
    CHECK(nand_pending(&dev) == 0u);
    CHECK(nand_read_sync(&dev, 20u, &r) == SSD_OK);
    CHECK(r.lba == 21u);

    nand_deinit(&dev);
}

static void test_factory_bad_blocks(void)
{
    ssd_config_t cfg = make_test_cfg();
    nand_dev_t dev;

    ssd_clock_init();

    cfg.factory_bb_permille = 0u;
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);
    CHECK(dev.factory_bad_blocks == 0u);
    nand_deinit(&dev);

    cfg.factory_bb_permille = 1000u;   /* 100% */
    CHECK(nand_init(&dev, &cfg, 64u) == SSD_OK);
    CHECK(dev.factory_bad_blocks == dev.geo.total_blocks);
    nand_deinit(&dev);
}

static void test_evtq_order(void)
{
    static ssd_event_t heap[16];
    ssd_evtq_t q;
    ssd_event_t e;

    ssd_evtq_init(&q, heap, 16u);
    CHECK(ssd_evtq_push(&q, 300u, NULL, NULL) == SSD_OK);
    CHECK(ssd_evtq_push(&q, 100u, NULL, NULL) == SSD_OK);
    CHECK(ssd_evtq_push(&q, 200u, NULL, NULL) == SSD_OK);
    CHECK(ssd_evtq_size(&q) == 3u);
    CHECK(ssd_evtq_next_time(&q) == 100u);

    CHECK(ssd_evtq_pop(&q, &e) == SSD_OK);
    CHECK(e.time == 100u);
    CHECK(ssd_evtq_pop(&q, &e) == SSD_OK);
    CHECK(e.time == 200u);
    CHECK(ssd_evtq_pop(&q, &e) == SSD_OK);
    CHECK(e.time == 300u);
    CHECK(ssd_evtq_pop(&q, &e) != SSD_OK);
}

/* 返回失败项数量，由 tests/test_main.c 汇总 */
int media_run(void)
{
    printf("[media] nand model tests\n");

    test_geometry();
    test_prog_read_timing();
    test_erase();
    test_bad_block();
    test_pending_page();
    test_factory_bad_blocks();
    test_evtq_order();

    printf("[media] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
