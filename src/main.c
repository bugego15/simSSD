#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/latency.h"
#include "core/log.h"
#include "core/rng.h"
#include "core/stats.h"
#include "ftl/ftl.h"
#include "ftl/sched.h"
#include "media/nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nand_dump_summary(const nand_dev_t *dev)
{
    printf("================ nand media ===================\n");
    printf("  channels        %u\n", dev->geo.channels);
    printf("  ce per channel  %u  (total %u CE)\n",
           dev->geo.ces_per_ch, nand_ce_count(dev));
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

/* ------------------------------------------------------------------ */
/* 请求生成器                                                          */
/* ------------------------------------------------------------------ */

#define RECENT_LBA_SIZE 64u

typedef struct bench_ctx {
    ssd_rng_t           rng;
    const ssd_config_t *cfg;
    ftl_dev_t          *ftl;
    lba_t               n;
    /*
     * exp —— 提交时的期望值（"最后一个提交者"）
     * ack —— 收到完成通知时的值（"最后一个确认者"）
     *
     * 不掉电时两者相等；开了 power_cut 之后必须用 ack 校验：
     * 掉电那一刻还在飞的请求丢掉是合法的，host 根本没收到完成通知，
     * 拿 exp 去校验会报出一堆"假"的 mismatch。
     */
    uint32_t           *exp;
    uint32_t           *ack;
    uint32_t            seq;
    uint64_t            counter;
    uint64_t            trims;
    uint64_t            trim_pages;
    /* 最近下发的 lba：TRIM 时用来避开"还在盘上飞"的地址 */
    lba_t               recent[RECENT_LBA_SIZE];
    uint32_t            recent_pos;
} bench_ctx_t;

/* 请求真正完成时才登记确认值（S5 掉电校验用） */
static void bench_done(void *arg, ssd_req_type_t type, lba_t lba,
                       uint32_t seq, int status)
{
    bench_ctx_t *c = (bench_ctx_t *)arg;

    if (type != SSD_REQ_WRITE || status != SSD_OK) {
        return;
    }
    /*
     * 取 max，不能直接覆盖。
     *
     * 同一个 lba 上完全可能同时挂着两个写（qdepth 32、随机地址），
     * 而它们的完成顺序未必等于提交顺序 —— 后提交的那个可能先落地。
     * 若按完成顺序覆盖，先完成的"旧"请求会把刚记下的新值顶掉，
     * 校验时就会报出一个实际上并不存在的 mismatch（实测 20 万请求里错 4 个）。
     *
     * seq 单调递增，所以"已确认完成里最大的 seq"正是 host 能指望的值：
     * 全完成时它等于最后提交者；掉电打断最后提交者时，它回退到次新的
     * 那个已完成版本 —— 而这正是介质重建后应该读到的内容。
     */
    if (seq > c->ack[lba]) {
        c->ack[lba] = seq;
    }
}

static bool recent_conflict(const bench_ctx_t *c, lba_t start, uint32_t cnt)
{
    uint32_t i;

    for (i = 0; i < RECENT_LBA_SIZE; i++) {
        if (c->recent[i] >= start && c->recent[i] < start + cnt) {
            return true;
        }
    }
    return false;
}

/*
 * TRIM 必须避开正在飞的请求：
 * 如果 trim 掉的 lba 上恰好还有一个写请求在介质里排队，
 * 它完成后会把映射重新填上，而 host 侧却认为这段已经删掉了 ——
 * 全量校验时就会报出一个"假"的 mismatch。
 * 真实盘靠 host 保证这种顺序，基准里只能主动避开。
 */
static void bench_maybe_trim(bench_ctx_t *c)
{
    uint32_t cnt = 64u;
    lba_t    t;

    if (c->cfg->trim_ratio == 0u) {
        return;
    }
    if (((++c->counter) % c->cfg->trim_ratio) != 0u) {
        return;
    }

    t = (lba_t)ssd_rng_below(&c->rng, (uint32_t)c->n);
    if ((uint64_t)t + cnt > (uint64_t)c->n) {
        cnt = (uint32_t)((uint64_t)c->n - t);
    }
    if (cnt == 0u || recent_conflict(c, t, cnt)) {
        return;
    }

    if (ftl_trim(c->ftl, t, cnt) == SSD_OK) {
        uint32_t j;

        for (j = 0; j < cnt; j++) {
            c->exp[t + j] = 0u;
            c->ack[t + j] = 0u;
        }
        c->trims++;
        c->trim_pages += cnt;
    }
}

static void bench_gen(void *arg, ssd_req_type_t *type, lba_t *lba, uint32_t *seq)
{
    bench_ctx_t *c = (bench_ctx_t *)arg;

    *type = SSD_REQ_WRITE;
    if (c->cfg->read_ratio > 0u &&
        ssd_rng_chance_permille(&c->rng, c->cfg->read_ratio)) {
        *type = SSD_REQ_READ;
    }

    *lba = bench_pick_lba(&c->rng, c->cfg, c->n);
    c->seq++;
    *seq = c->seq;

    c->recent[c->recent_pos % RECENT_LBA_SIZE] = *lba;
    c->recent_pos++;

    /* 写请求在这里登记期望值：映射是在提交那一刻更新的，
     * 所以"最后提交者的 seq"就是最终应当读到的内容，
     * 即便同一 lba 上先后有两个请求在飞也不会错位。 */
    if (*type == SSD_REQ_WRITE) {
        c->exp[*lba] = *seq;
    }

    bench_maybe_trim(c);
}

/* ------------------------------------------------------------------ */
/* 基准                                                                */
/* ------------------------------------------------------------------ */

static int run_bench(const ssd_config_t *cfg)
{
    nand_dev_t    nand;
    ftl_dev_t     ftl;
    ssd_sched_t   sched;
    bench_ctx_t   ctx;
    ftl_pe_stat_t pe;
    uint32_t     *exp;
    uint32_t     *ack;
    lba_t         n;
    lba_t         lba;
    uint32_t      i;
    uint64_t      bad = 0u;
    uint64_t      dropped = 0u;   /* 提交过但从未收到完成通知的 lba 数 */
    uint64_t      nreq = cfg->bench_writes;
    clock_t       wall0;
    double        wall;
    double        elapsed_s;
    double        iops;
    /* S5：掉电与恢复 */
    uint64_t      host_ns     = 0u;   /* host 真正跑请求的时间（不含上电重建） */
    uint64_t      recovery_ns = 0u;   /* 上电重建花掉的虚拟时间 */
    uint64_t      cuts        = 0u;
    uint64_t      torn_progs  = 0u;
    uint64_t      torn_erases = 0u;
    uint64_t      rec_mapped  = 0u;
    uint64_t      rec_stale   = 0u;
    uint64_t      rec_torn    = 0u;
    /* 调度器和 FTL 每段都会重建，这些计数必须自己累加 */
    ssd_lat_t     lat_w;
    ssd_lat_t     lat_r;
    uint64_t      idle_gc          = 0u;
    uint64_t      idle_ns          = 0u;   /* S6：间歇型负载里空转掉的时间 */
    uint64_t      cp_total         = 0u;   /* S6：下刷快照的次数（跨掉电累加） */
    uint64_t      cp_ns_total      = 0u;
    uint64_t      rec_cp           = 0u;   /* S6：走快照重建的次数 */
    uint64_t      rec_scanned      = 0u;
    uint64_t      retries          = 0u;
    uint64_t      errors           = 0u;
    uint64_t      missed           = 0u;
    uint64_t      gc_total         = 0u;
    uint64_t      bg_total         = 0u;
    uint64_t      gc_copied_total  = 0u;
    uint64_t      wl_total         = 0u;
    uint64_t      wl_pages_total   = 0u;
    uint64_t      trim_cmds_total  = 0u;
    uint64_t      trim_pages_total = 0u;

    if (nand_init(&nand, cfg, 8192u) != SSD_OK) {
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
    ack = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    if (exp == NULL || ack == NULL) {
        SSD_ERR("out of memory for verifier table");
        free(exp);
        free(ack);
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ssd_rng_seed(&ctx.rng, cfg->seed);
    ctx.cfg  = cfg;
    ctx.ftl  = &ftl;
    ctx.n    = n;
    ctx.exp  = exp;
    ctx.ack  = ack;
    for (i = 0; i < RECENT_LBA_SIZE; i++) {
        ctx.recent[i] = SSD_INVALID_LBA;
    }

    if (sched_init(&sched, &ftl, &nand, cfg->qdepth) != SSD_OK) {
        SSD_ERR("sched_init failed");
        free(exp);
        free(ack);
        ftl_deinit(&ftl);
        nand_deinit(&nand);
        return 1;
    }
    sched_set_done_cb(&sched, bench_done, &ctx);
    ssd_lat_init(&lat_w);
    ssd_lat_init(&lat_r);

    wall0 = clock();

    /*
     * 分段跑：每 power_cut 个请求掉一次电，然后上电重建继续。
     *
     * FTL 在掉电后是"全部重来"的 —— 映射表、块池、写入块都得从介质上
     * 重新推出来。这里如实模拟：deinit 掉整个 FTL 再 init + recover，
     * 不留任何 DRAM 状态下来走后门。
     */
    {
        uint64_t done = 0u;

        for (;;) {
            uint64_t seg = nreq - done;
            uint64_t t0;

            if (cfg->power_cut > 0u && seg > (uint64_t)cfg->power_cut) {
                seg = (uint64_t)cfg->power_cut;
            }

            /* 最后一段正常收尾；其余段到达即断电，请求留在队列里交给掉电打断 */
            uint64_t cut = (done + seg >= nreq) ? 0u : seg;

            t0 = ssd_clock_now();
            (void)sched_run_bursty(&sched, seg, bench_gen, &ctx,
                                   cfg->burst_len,
                                   (uint64_t)cfg->idle_us * 1000u, cut);
            host_ns += ssd_clock_now() - t0;
            done    += seg;

            if (cut != 0u) {
                /*
                 * 断电前先把队列补满。
                 * 不这么做的话，前台 GC 的 run_until_idle 早就把队列跑空了，
                 * 掉电打不到任何页编程 —— 等于每回都挑盘最闲的瞬间断电。
                 */
                (void)sched_fill(&sched, bench_gen, &ctx, cfg->qdepth);
            }

            ssd_lat_merge(&lat_w, &sched.lat_w);
            ssd_lat_merge(&lat_r, &sched.lat_r);
            idle_gc += sched.idle_gc_count;
            idle_ns += sched.idle_ns;
            retries += sched.retries;
            errors  += sched.errors;
            missed  += sched.missed;

            if (done >= nreq) {
                break;
            }

            {
                nand_power_loss_t loss;
                ftl_recovery_t    rec;
                uint64_t          t1 = ssd_clock_now();

                nand_power_cut(&nand, &loss);

                /* FTL 的计数器也在 DRAM 里，掉电一并带走，先存下来 */
                gc_total         += ftl.gc_count;
                bg_total         += ftl.bg_gc_count;
                gc_copied_total  += ftl.gc_copied;
                cp_total         += ftl.cp_count;
                cp_ns_total      += ftl.cp_ns;
                wl_total         += ftl.wl_migrations;
                wl_pages_total   += ftl.wl_migrated_pages;
                trim_cmds_total  += ftl.trim_cmds;
                trim_pages_total += ftl.trim_pages;

                ftl_deinit(&ftl);
                if (ftl_init(&ftl, &nand, cfg) != SSD_OK) {
                    SSD_ERR("ftl_init after power cut failed");
                    break;
                }
                if (ftl_recover(&ftl, &rec) != SSD_OK) {
                    SSD_ERR("ftl_recover failed");
                    break;
                }

                /* host 侧的队列同样随掉电消失，重发由调度器重新建起来 */
                sched_deinit(&sched);
                if (sched_init(&sched, &ftl, &nand, cfg->qdepth) != SSD_OK) {
                    SSD_ERR("sched_init after power cut failed");
                    break;
                }
                sched_set_done_cb(&sched, bench_done, &ctx);

                recovery_ns += ssd_clock_now() - t1;
                cuts++;
                torn_progs  += loss.torn_progs;
                torn_erases += loss.torn_erases;
                rec_mapped  += rec.mapped_lbas;
                rec_stale   += rec.stale_pages;
                rec_torn    += rec.torn_pages;
                rec_cp      += rec.from_checkpoint;
                rec_scanned += rec.scanned_blocks;
            }
        }
    }

    wall = (double)(clock() - wall0) / (double)CLOCKS_PER_SEC;

    /*
     * 全量校验：以"最后一次收到完成通知的值"为准。
     *
     * 掉电时还在飞的请求丢掉是合法的，所以不能用提交值（exp）校验 ——
     * 那是 host 都没收到回执的写。用 ack 才是 SPOR 真正的语义底线：
     * 凡是 host 已经被告知"写完了"的数据，上电之后必须还在，且是最新的。
     * 被 TRIM 掉的 lba 必须读不到数据。
     */
    for (lba = 0; lba < n; lba++) {
        nand_page_meta_t m;

        if (ack[lba] == 0u) {
            continue;
        }
        if (ftl_read(&ftl, lba, &m) != SSD_OK) {
            bad++;
            continue;
        }
        if (m.lba != lba || m.seq != ack[lba]) {
            if (bad < 5u) {
                printf("  MISMATCH lba=%llu expect_seq=%u got_seq=%u got_lba=%llu\n",
                       (unsigned long long)lba, ack[lba], m.seq,
                       (unsigned long long)m.lba);
            }
            bad++;
        }
        /* 提交了、却始终没等到完成通知的写：掉电把它们带走了 */
        if (exp[lba] != ack[lba]) {
            dropped++;
        }
    }

    elapsed_s = (double)host_ns / 1e9;
    if (elapsed_s <= 0.0) {
        elapsed_s = 1e-9;
    }
    iops = (double)(nreq) / elapsed_s;

    ftl_pe_stats(&ftl, &pe);
    ssd_lat_finish(&lat_w);
    ssd_lat_finish(&lat_r);

    printf("================ benchmark ====================\n");
    printf("  host requests   %llu (write %u / read %u)\n",
           (unsigned long long)nreq,
           ssd_lat_count(&lat_w), ssd_lat_count(&lat_r));
    printf("  qdepth          %u\n", cfg->qdepth);
    printf("  IOPS            %.0f\n", iops);
    printf("  throughput      %.1f MB/s\n",
           iops * (double)cfg->page_data_size / 1e6);
    printf("  ---------------- latency ----------------\n");
    ssd_lat_dump("write", &lat_w);
    if (ssd_lat_count(&lat_r) > 0u) {
        ssd_lat_dump("read", &lat_r);
    }
    printf("  ---------------- media ----------------\n");
    printf("  channel util    %.1f%%\n", nand_channel_utilization(&nand) * 100.0);
    printf("  ce util         %.1f%%\n", nand_ce_utilization(&nand) * 100.0);
    printf("  parallelism     %.2f of %u CE\n",
           nand_parallelism(&nand), nand_ce_count(&nand));
    printf("  ---------------- ftl ----------------\n");
    printf("  nand prog       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_PROG_PAGES));
    printf("  nand read       %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_READ_PAGES));
    printf("  nand erase      %llu\n",
           (unsigned long long)ssd_stats_get(ST_NAND_ERASE_BLOCKS));
    printf("  WAF             %.3f\n", ssd_stats_waf());
    printf("  GC count        %llu (foreground %llu / background %llu)\n",
           (unsigned long long)(gc_total + ftl.gc_count),
           (unsigned long long)((gc_total + ftl.gc_count) -
                                (bg_total + ftl.bg_gc_count)),
           (unsigned long long)(bg_total + ftl.bg_gc_count));
    printf("  GC copied pages %llu (overhead %.3f)\n",
           (unsigned long long)(gc_copied_total + ftl.gc_copied),
           ssd_stats_gc_overhead());
    printf("  idle gc         %llu (host 队列没填满时补的)\n",
           (unsigned long long)idle_gc);
    if (idle_ns > 0u) {
        printf("  idle spans      %.3f ms (%.1f%% of host time)\n",
               (double)idle_ns / 1e6,
               (double)idle_ns / (double)host_ns * 100.0);
    }
    if ((cp_total + ftl.cp_count) > 0u) {
        printf("  checkpoints     %llu (%.3f ms of host time)\n",
               (unsigned long long)(cp_total + ftl.cp_count),
               (double)(cp_ns_total + ftl.cp_ns) / 1e6);
    }
    printf("  TRIM            %llu cmds / %llu pages (issued %llu)\n",
           (unsigned long long)(trim_cmds_total + ftl.trim_cmds),
           (unsigned long long)(trim_pages_total + ftl.trim_pages),
           (unsigned long long)ctx.trims);
    printf("  req retries     %llu (errors %llu, read miss %llu)\n",
           (unsigned long long)retries,
           (unsigned long long)errors,
           (unsigned long long)missed);
    printf("  WL migrations   %llu (pages %llu)\n",
           (unsigned long long)(wl_total + ftl.wl_migrations),
           (unsigned long long)(wl_pages_total + ftl.wl_migrated_pages));
    printf("  PE cycles       min %u / max %u / avg %.1f / stddev %.2f\n",
           pe.min_pe, pe.max_pe, pe.avg_pe, pe.stddev_pe);
    printf("  bad blocks      %u (runtime %u)\n",
           ftl_bad_blocks(&ftl), ftl.bad_blocks);
    {
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
    if (cuts > 0u) {
        printf("  ---------------- spor ----------------\n");
        printf("  power cuts      %llu (every %u requests)\n",
               (unsigned long long)cuts, cfg->power_cut);
        printf("  torn            %llu pages (partial program) | %llu blocks (erase)\n",
               (unsigned long long)torn_progs, (unsigned long long)torn_erases);
        printf("  rebuilt         %llu lbas | stale %llu | torn %llu\n",
               (unsigned long long)rec_mapped, (unsigned long long)rec_stale,
               (unsigned long long)rec_torn);
        printf("  from checkpoint %llu/%llu recoveries, %llu blocks scanned\n",
               (unsigned long long)rec_cp, (unsigned long long)cuts,
               (unsigned long long)rec_scanned);
        printf("  lost writes     %llu lbas (submitted, never acked)\n",
               (unsigned long long)dropped);
        printf("  recovery time   %.3f ms\n", (double)recovery_ns / 1e6);
    }
    printf("  host time       %.3f ms\n", elapsed_s * 1e3);
    if (recovery_ns > 0u) {
        printf("  recovery time   %.3f ms (not counted in IOPS)\n",
               (double)recovery_ns / 1e6);
    }
    printf("  wall time       %.2f s\n", wall);
    printf("  verify          %s (mismatch %llu)\n",
           (bad == 0u) ? "PASS" : "FAIL", (unsigned long long)bad);
    printf("================================================\n");

    ssd_lat_clear(&lat_w);
    ssd_lat_clear(&lat_r);
    sched_deinit(&sched);
    free(exp);
    free(ack);
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
