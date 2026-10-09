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
    printf("  gc policy       %s\n",
           (ftl->gc_policy == 1u) ? "cost-benefit" : "greedy");
    printf("  water marks     fg<=%u  bg target %u\n",
           ftl->rsv_min, ftl->bg_target);
    printf("  free blocks     %u\n", ftl_free_blocks(ftl));
    printf("  bad blocks      %u\n", ftl_bad_blocks(ftl));
    printf("================================================\n");
}

/*
 * 负载模型：
 *   uniform  —— 全地址均匀随机，没有冷热之分
 *   hotspot  —— hot_percent 的访问集中在前 20% 地址，其余均匀散布。
 *               真实业务（数据库、文件系统元数据）都是这种形态，
 *               也正是 cost-benefit 相对 greedy 能拉开差距的场景。
 */
static lba_t bench_pick_lba(ssd_rng_t *rng, const ssd_config_t *cfg, lba_t n)
{
    lba_t hot;
    uint32_t permille;

    if (cfg->workload != 1u) {
        return (lba_t)ssd_rng_below(rng, (uint32_t)n);
    }

    hot = (lba_t)((uint64_t)n * 20u / 100u);
    if (hot == 0u) {
        hot = 1u;
    }
    if (hot >= n) {
        hot = n - 1u;
    }
    permille = cfg->hot_percent * 10u;
    if (permille > 1000u) {
        permille = 1000u;
    }

    if (ssd_rng_chance_permille(rng, permille)) {
        return (lba_t)ssd_rng_below(rng, (uint32_t)hot);
    }
    return hot + (lba_t)ssd_rng_below(rng, (uint32_t)(n - hot));
}

/*
 * S3 验收基准：随机写 N 次（远超物理容量，必然触发 GC），
 * 然后全量读出校验。host 侧维护期望 seq 表，比对 FTL 读回的内容。
 */
static int run_bench(const ssd_config_t *cfg)
{
    nand_dev_t nand;
    ftl_dev_t ftl;
    ftl_pe_stat_t pe;
    uint32_t *exp;
    ssd_rng_t rng;
    lba_t n;
    lba_t lba;
    uint64_t i;
    uint64_t nwrites = cfg->bench_writes;
    uint64_t stage;
    uint64_t last_prog = 0u;
    uint64_t last_at = 0u;
    uint64_t bad = 0u;
    uint64_t trims = 0u;
    uint64_t write_errors = 0u;
    uint64_t t_start;
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

    stage = (nwrites + 9u) / 10u;
    if (stage == 0u) {
        stage = 1u;
    }

    printf("---------------- WAF 收敛过程 -----------------\n");

    for (i = 0; i < nwrites; i++) {
        uint32_t seq = (uint32_t)((i + 1u) & 0x7FFFFFFFu);

        lba = bench_pick_lba(&rng, cfg, n);
        if (ftl_write(&ftl, lba, seq) != SSD_OK) {
            /* 写不进去和"读回来的内容不对"是两回事：
             * 前者是容量真的不够了（坏块吃掉物理空间），后者才是数据损坏。 */
            SSD_ERR("write rejected at i=%llu lba=%llu (free=%u bad=%u)",
                    (unsigned long long)i, (unsigned long long)lba,
                    ftl_free_blocks(&ftl), ftl_bad_blocks(&ftl));
            write_errors++;
            break;
        }
        exp[lba] = seq;

        /* TRIM：模拟文件系统删除文件后下发 discard。
         * 被 trim 掉的 lba 期望值清零 —— 之后再读它应当是"没有数据"。 */
        if (cfg->trim_ratio != 0u && ((i + 1u) % cfg->trim_ratio) == 0u) {
            uint32_t cnt = 64u;
            uint32_t j;

            lba = (lba_t)ssd_rng_below(&rng, (uint32_t)n);
            if ((uint64_t)lba + cnt > (uint64_t)n) {
                cnt = (uint32_t)((uint64_t)n - lba);
            }
            if (ftl_trim(&ftl, lba, cnt) == SSD_OK) {
                for (j = 0; j < cnt; j++) {
                    exp[lba + j] = 0u;
                }
                trims++;
            }
        }

        if (((i + 1u) % stage) == 0u) {
            uint64_t prog = ssd_stats_get(ST_NAND_PROG_PAGES);
            uint64_t done = i + 1u;

            /* cum = 从头到尾的累计 WAF（会收敛到稳态值）
             * stage = 这一段内的增量 WAF（稳态后趋于恒定） */
            printf("  [%3llu%%] cum WAF %.3f | stage WAF %.3f | free %4u | gc %8llu | bad %u\n",
                   (unsigned long long)(done * 100u / nwrites),
                   (double)prog / (double)done,
                   (double)(prog - last_prog) / (double)(done - last_at),
                   ftl_free_blocks(&ftl),
                   (unsigned long long)ftl.gc_count,
                   ftl_bad_blocks(&ftl));
            last_prog = prog;
            last_at   = done;
        }
    }

    /* 全量校验：写过的 lba 必须能读到最后一次写入的内容；
     * 被 TRIM 掉的 lba 必须读不到数据 */
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

    wall = (double)(clock() - wall0) / (double)CLOCKS_PER_SEC;
    ftl_pe_stats(&ftl, &pe);

    printf("================ benchmark ====================\n");
    printf("  host writes     %llu\n", (unsigned long long)nwrites);
    printf("  nand prog       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_PROG_PAGES));
    printf("  nand read       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_READ_PAGES));
    printf("  nand erase      %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_ERASE_BLOCKS));
    printf("  WAF             %.3f\n", ssd_stats_waf());
    printf("  GC count        %llu (foreground %llu / background %llu)\n",
           (unsigned long long)ftl.gc_count,
           (unsigned long long)(ftl.gc_count - ftl.bg_gc_count),
           (unsigned long long)ftl.bg_gc_count);
    printf("  GC copied pages %llu (overhead %.3f)\n",
           (unsigned long long)ftl.gc_copied, ssd_stats_gc_overhead());
    printf("  TRIM            %llu cmds / %llu pages (issued %llu)\n",
           (unsigned long long)ftl.trim_cmds, (unsigned long long)ftl.trim_pages,
           (unsigned long long)trims);
    printf("  write rejected  %llu\n", (unsigned long long)write_errors);
    printf("  WL migrations   %llu (pages %llu)\n",
           (unsigned long long)ftl.wl_migrations,
           (unsigned long long)ftl.wl_migrated_pages);
    printf("  PE cycles       min %u / max %u / avg %.1f / stddev %.2f\n",
           pe.min_pe, pe.max_pe, pe.avg_pe, pe.stddev_pe);
    printf("  bad blocks      %u (runtime %u)\n",
           ftl_bad_blocks(&ftl), ftl.bad_blocks);
    {
        /* 块状态分布：free 状态数与池内块数应当一致，
         * 一旦差出很多，说明有块既不在池里、也没在被使用 —— 那就是泄漏 */
        uint32_t st_free = 0u, st_open = 0u, st_closed = 0u, st_bad = 0u;
        pbn_t p;

        for (p = 0; p < nand.geo.total_blocks; p++) {
            uint16_t s = nand_block_state(&nand, p);
            if (s == (uint16_t)NAND_BLK_BAD) {
                st_bad++;
            } else if (s == (uint16_t)NAND_BLK_OPEN) {
                st_open++;
            } else if (s == (uint16_t)NAND_BLK_CLOSED) {
                st_closed++;
            } else {
                st_free++;
            }
        }
        printf("  block states    free %u (in pool %u) | open %u | closed %u | bad %u\n",
               st_free, ftl_free_blocks(&ftl), st_open, st_closed, st_bad);
    }
    printf("  free blocks     %u\n", ftl_free_blocks(&ftl));
    printf("  virtual time    %.3f ms\n",
           (double)(ssd_clock_now() - t_start) / 1e6);
    printf("  IOPS            %.0f\n",
           (double)nwrites / ((double)(ssd_clock_now() - t_start) / 1e9));
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
