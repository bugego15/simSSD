/*
 * S0 冒烟测试：验证公共库（bitmap / mempool / config / clock / stats）。
 * 不依赖任何外部测试框架，返回 0 表示全部通过。
 */

#include "ssd_types.h"
#include "core/bitmap.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/mempool.h"
#include "core/stats.h"

#include <stdio.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            g_failed++;                                                  \
        }                                                                \
    } while (0)

static void test_bitmap(void)
{
    uint32_t bm[4];   /* 128 bits */

    ssd_bitmap_zero(bm, 128);
    CHECK(ssd_bitmap_count_set(bm, 128) == 0);

    ssd_bitmap_set(bm, 5);
    ssd_bitmap_set(bm, 100);
    CHECK(ssd_bitmap_get(bm, 5));
    CHECK(ssd_bitmap_get(bm, 100));
    CHECK(!ssd_bitmap_get(bm, 6));
    CHECK(ssd_bitmap_count_set(bm, 128) == 2);

    CHECK(ssd_bitmap_find_zero(bm, 128, 0) == 0);
    CHECK(ssd_bitmap_find_zero(bm, 128, 5) == 6);

    ssd_bitmap_clear(bm, 100);
    CHECK(ssd_bitmap_count_set(bm, 128) == 1);

    ssd_bitmap_fill(bm, 128);
    CHECK(ssd_bitmap_count_set(bm, 128) == 128);
    CHECK(ssd_bitmap_find_zero(bm, 128, 0) == -1);
}

static void test_mempool(void)
{
    static uint8_t buf[1024];
    ssd_mempool_t pool;
    uint8_t *p1;
    uint8_t *p2;
    size_t off;

    ssd_pool_init(&pool, buf, sizeof(buf));

    p1 = (uint8_t *)ssd_pool_alloc(&pool, 100, 8);
    CHECK(p1 != NULL);
    off = (size_t)(p1 - buf);
    CHECK((off % 8u) == 0u);

    p2 = (uint8_t *)ssd_pool_alloc(&pool, 8, 64);
    CHECK(p2 != NULL);
    off = (size_t)(p2 - buf);
    CHECK((off % 64u) == 0u);

    CHECK(ssd_pool_used(&pool) > 100u);
    CHECK(ssd_pool_peak(&pool) == ssd_pool_used(&pool));

    ssd_pool_reset(&pool);
    CHECK(ssd_pool_used(&pool) == 0u);
    CHECK(ssd_pool_peak(&pool) > 0u);
}

static void test_config(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    CHECK(ssd_config_derive(&c) == SSD_OK);

    CHECK(c.total_blocks == (uint64_t)c.channels * c.ces_per_ch *
                            c.dies_per_ce * c.planes_per_die *
                            c.blocks_per_plane);
    CHECK(c.total_pages == c.total_blocks * c.pages_per_block);
    CHECK(c.user_blocks == c.total_blocks - c.reserved_blocks);
    CHECK(c.user_pages == c.user_blocks * c.pages_per_block);
    CHECK(c.capacity_bytes == c.user_pages * c.page_data_size);
    CHECK(c.user_pages < c.total_pages);

    /* OP >= 100% 属于非法配置 */
    c.op_percent = 100u;
    CHECK(ssd_config_derive(&c) == SSD_ERR_INVAL);

    /* 几何参数非法 */
    ssd_config_set_defaults(&c);
    c.pages_per_block = 0u;
    CHECK(ssd_config_derive(&c) == SSD_ERR_INVAL);
}

static void test_clock_stats(void)
{
    double waf;

    ssd_clock_init();
    CHECK(ssd_clock_now() == 0u);
    ssd_clock_advance(1234u);
    CHECK(ssd_clock_now() == 1234u);

    ssd_stats_init();
    ssd_stats_inc(ST_HOST_WRITE_PAGES);
    ssd_stats_add(ST_NAND_PROG_PAGES, 3u);
    CHECK(ssd_stats_get(ST_NAND_PROG_PAGES) == 3u);

    waf = ssd_stats_waf();
    CHECK(waf > 2.99 && waf < 3.01);

    ssd_stats_add(ST_MEDIA_BUSY_NS, 617u);
    CHECK(ssd_stats_media_utilization() > 0.49 &&
          ssd_stats_media_utilization() < 0.51);
}

int main(void)
{
    printf("[smoke] ssd-sim unit tests\n");

    test_bitmap();
    test_mempool();
    test_config();
    test_clock_stats();

    printf("[smoke] %d checks, %d failed\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
