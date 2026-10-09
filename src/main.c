#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/log.h"
#include "core/rng.h"
#include "core/stats.h"
#include "ftl/ftl.h"
#include "media/nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nand_dump_summary(const nand_dev_t *dev)
{
    printf("================ nand media ===================\n");
    printf("  channels        %u\n", dev->geo.channels);
    printf("  blocks          %llu\n", (unsigned long long)dev->geo.total_blocks);
    printf("  pages           %llu\n", (unsigned long long)dev->geo.total_pages);
    printf("  pages/block     %u\n", dev->geo.pages_per_block);
    printf("  factory bad     %u\n", dev->factory_bad_blocks);
    printf("  good blocks     %llu\n",
           (unsigned long long)(dev->geo.total_blocks - dev->factory_bad_blocks));
    printf("  fault inject    %s (rate %u/1000)\n",
           dev->fault_inject ? "on" : "off", dev->fault_rate_permille);
    printf("================================================\n");
}

static void ftl_dump_summary(const ftl_dev_t *ftl)
{
    printf("================ ftl ==========================\n");
    printf("  user lbas       %llu\n", (unsigned long long)ftl->user_lbas);
    printf("  l2p entries     %llu (%.2f MiB)\n",
           (unsigned long long)ftl->user_lbas,
           (double)(ftl->user_lbas * sizeof(ppn_t)) / (1024.0 * 1024.0));
    printf("  free blocks     %u (gc reserve %u)\n",
           ftl->free_top, ftl->rsv_min);
    printf("================================================\n");
}

/*
 * S2 验收基准：随机写 N 次（远超物理容量，必然触发 GC），
 * 然后全量读出校验。host 侧维护期望 seq 表，比对 FTL 读回的内容。
 */
static int run_bench(const ssd_config_t *cfg)
{
    nand_dev_t nand;
    ftl_dev_t ftl;
    uint32_t *exp;
    ssd_rng_t rng;
    lba_t n;
    lba_t lba;
    uint64_t i;
    uint64_t nwrites = cfg->bench_writes;
    uint64_t bad = 0;
    uint64_t t_start;
    uint64_t vtime;
    clock_t wall0;
    double wall;

    if (nand_init(&nand, cfg, 4096u) != SSD_OK) {
        SSD_ERR("nand_init failed");
        return 1;
    }
    if (ftl_init(&ftl, &nand, cfg) != SSD_OK) {
        SSD_ERR("ftl_init failed");
        nand_deinit(&nand);
        return 1;
    }

    n = ftl_user_lbas(&ftl);
    exp = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    if (exp == NULL) {
        SSD_ERR("out of memory for verifier table");
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return 1;
    }

    ssd_rng_seed(&rng, cfg->seed);
    t_start = ssd_clock_now();
    wall0   = clock();

    for (i = 0; i < nwrites; i++) {
        uint32_t seq = (uint32_t)((i + 1u) & 0x7FFFFFFFu);

        lba = (lba_t)ssd_rng_below(&rng, (uint32_t)n);
        if (ftl_write(&ftl, lba, seq) != SSD_OK) {
            SSD_ERR("write failed at i=%llu lba=%llu", i, (unsigned long long)lba);
            bad++;
            break;
        }
        exp[lba] = seq;
    }

    /* 全量校验：写过的 lba 必须能读到最后一次写入的内容 */
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
            if (bad < 5u) {
                printf("  MISMATCH lba=%llu expect_seq=%u got_seq=%u got_lba=%llu\n",
                       (unsigned long long)lba, exp[lba], m.seq,
                       (unsigned long long)m.lba);
            }
            bad++;
        }
    }

    vtime = ssd_clock_now() - t_start;
    wall  = (double)(clock() - wall0) / (double)CLOCKS_PER_SEC;

    printf("================ benchmark ====================\n");
    printf("  host writes     %llu\n", (unsigned long long)nwrites);
    printf("  nand prog       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_PROG_PAGES));
    printf("  nand read       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_READ_PAGES));
    printf("  nand erase      %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_ERASE_BLOCKS));
    printf("  WAF             %.3f\n", ssd_stats_waf());
    printf("  GC count        %llu\n", (unsigned long long)ftl.gc_count);
    printf("  GC copied pages %llu (overhead %.3f)\n",
           (unsigned long long)ftl.gc_copied, ssd_stats_gc_overhead());
    printf("  free blocks     %u\n", ftl_free_blocks(&ftl));
    printf("  virtual time    %.3f ms\n", (double)vtime / 1e6);
    printf("  IOPS            %.0f\n",
           (double)nwrites / ((double)vtime / 1e9));
    printf("  wall time       %.2f s\n", wall);
    printf("  verify          %s (mismatch %llu)\n",
           (bad == 0u) ? "PASS" : "FAIL", (unsigned long long)bad);
    printf("================================================\n");

    free(exp);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
    return (bad == 0u) ? 0 : 1;
}

int main(int argc, char **argv)
{
    ssd_config_t cfg;
    nand_dev_t dev;
    ftl_dev_t ftl;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            ssd_config_usage(argv[0]);
            return 0;
        }
    }

    ssd_config_set_defaults(&cfg);
    if (ssd_config_parse_args(&cfg, argc, argv) != SSD_OK) {
        ssd_config_usage(argv[0]);
        return 1;
    }
    if (ssd_config_derive(&cfg) != SSD_OK) {
        return 1;
    }

    ssd_clock_init();
    ssd_log_init(cfg.log_level);
    ssd_stats_init();

    ssd_config_dump(&cfg);

    if (nand_init(&dev, &cfg, 4096u) != SSD_OK) {
        SSD_ERR("nand_init failed");
        return 1;
    }
    nand_dump_summary(&dev);

    if (ftl_init(&ftl, &dev, &cfg) != SSD_OK) {
        SSD_ERR("ftl_init failed");
        nand_deinit(&dev);
        return 1;
    }
    ftl_dump_summary(&ftl);

    if (cfg.bench_writes > 0u) {
        int rc;
        ftl_deinit(&ftl);
        nand_deinit(&dev);
        ssd_stats_reset();
        rc = run_bench(&cfg);
        return rc;
    }

    ssd_stats_dump();

    ftl_deinit(&ftl);
    nand_deinit(&dev);
    return 0;
}
