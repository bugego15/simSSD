/*
 * S4 调度与并发测试
 *
 * 每个用例都对着一个可验证的结论：
 *   - 并发正确性：qdepth>1 时，最后提交的内容必须还能读出来
 *   - 并行度：队列深度足够时，多个 CE 必须真的同时工作
 *   - 可复现：同样的 seed 两次跑出的延迟分位数必须一致（A/B 对比的前提）
 *   - 读写混合：读请求插在写中间，数据仍然正确
 *   - idle 后台 GC：host 队列没填满时确实补了回收
 */

#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/latency.h"
#include "core/rng.h"
#include "core/stats.h"
#include "ftl/ftl.h"
#include "ftl/sched.h"
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
 * 并发测试必须用多 CE 拓扑：单 CE 的盘无论怎么调度，并行度都只能是 1，
 * 那就测不出 S4 想验证的东西。
 * 256 blocks x 64 pages = 16384 物理页，8 个 CE（4 通道 x 2 CE）。
 */
static ssd_config_t make_cfg_concurrent(void)
{
    ssd_config_t c;

    ssd_config_set_defaults(&c);
    c.channels            = 4;
    c.ces_per_ch          = 2;
    c.dies_per_ce         = 1;
    c.planes_per_die      = 1;
    c.blocks_per_plane    = 32;
    c.pages_per_block     = 64;
    c.op_percent          = 7;
    c.factory_bb_permille = 0;
    c.fault_inject        = 0;
    c.seed                = 777u;
    c.t_prog_ns           = 100000;   /* 100 us */
    c.t_read_ns           = 20000;
    c.t_bers_ns           = 300000;
    c.t_xfer_ns           = 5000;
    c.bg_gc_percent       = 0;
    c.wl_enable           = 1;
    (void)ssd_config_derive(&c);
    return c;
}

/* ---------------- 负载与校验 ---------------- */

typedef struct gen_ctx {
    ssd_rng_t  rng;
    lba_t      n;
    uint32_t  *exp;
    uint32_t   seq;
    uint32_t   read_permille;
} gen_ctx_t;

static void gen_cb(void *arg, ssd_req_type_t *type, lba_t *lba, uint32_t *seq)
{
    gen_ctx_t *c = (gen_ctx_t *)arg;

    *type = SSD_REQ_WRITE;
    if (c->read_permille > 0u &&
        ssd_rng_chance_permille(&c->rng, c->read_permille)) {
        *type = SSD_REQ_READ;
    }

    *lba = (lba_t)ssd_rng_below(&c->rng, (uint32_t)c->n);
    c->seq++;
    *seq = c->seq;

    /* 映射在提交那一刻更新，所以"最后提交者的 seq"就是最终应读到的内容 */
    if (*type == SSD_REQ_WRITE) {
        c->exp[*lba] = *seq;
    }
}

typedef struct run_result {
    uint64_t completed;
    uint64_t bad;
    double   iops;
    double   parallelism;
    uint64_t idle_gc;
    uint64_t gc_count;
    uint64_t p50_ns;
    uint64_t p99_ns;
} run_result_t;

static void run_once(const ssd_config_t *cfg, uint32_t qdepth, uint64_t nreq,
                     uint32_t read_permille, run_result_t *out)
{
    nand_dev_t    nand;
    ftl_dev_t     ftl;
    ssd_sched_t   sched;
    gen_ctx_t     ctx;
    uint32_t     *exp;
    lba_t         n;
    lba_t         lba;
    uint64_t      t0;
    uint64_t      bad = 0u;

    ssd_clock_init();
    ssd_stats_init();

    CHECK(nand_init(&nand, cfg, 8192u) == SSD_OK);
    CHECK(ftl_init(&ftl, &nand, cfg) == SSD_OK);

    n   = ftl_user_lbas(&ftl);
    exp = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    CHECK(exp != NULL);

    memset(&ctx, 0, sizeof(ctx));
    ssd_rng_seed(&ctx.rng, cfg->seed);
    ctx.n             = n;
    ctx.exp           = exp;
    ctx.read_permille = read_permille;

    CHECK(sched_init(&sched, &ftl, &nand, qdepth) == SSD_OK);

    t0 = ssd_clock_now();
    (void)sched_run(&sched, nreq, gen_cb, &ctx, 0u);

    /* 全量校验：写过的 lba 必须能读到最后一次提交的内容 */
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

    out->completed   = sched.completed;
    out->bad         = bad;
    out->iops        = (double)sched.completed /
                       ((double)(ssd_clock_now() - t0) / 1e9);
    out->parallelism = nand_parallelism(&nand);
    out->idle_gc     = sched.idle_gc_count;
    out->gc_count    = ftl.gc_count;
    out->p50_ns      = ssd_lat_percentile(&sched.lat_w, 0.5);
    out->p99_ns      = ssd_lat_percentile(&sched.lat_w, 0.99);

    sched_deinit(&sched);
    free(exp);
    ftl_deinit(&ftl);
    nand_deinit(&nand);
}

/* ---------------- 用例 ---------------- */

static void test_concurrent_correctness(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t r;

    run_once(&cfg, 8u, 10000u, 0u, &r);

    CHECK(r.completed == 10000u);
    CHECK(r.bad == 0u);

    printf("  [sched] concurrent: qd=8 io=%llu iops=%.0f par=%.2f bad=%llu\n",
           (unsigned long long)r.completed, r.iops, r.parallelism,
           (unsigned long long)r.bad);
}

static void test_parallelism_scales(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t r1;
    run_result_t r16;

    /* 队列深度为 1 时盘上永远只有一个请求，并行度必然接近 1 */
    run_once(&cfg, 1u,  4000u, 0u, &r1);
    run_once(&cfg, 16u, 4000u, 0u, &r16);

    CHECK(r1.parallelism <= 1.2);
    CHECK(r16.parallelism > 2.0);       /* 8 个 CE，喂饱了就该多 CE 同时工作 */
    CHECK(r16.iops > r1.iops * 1.5);    /* 并发必须换来吞吐 */

    printf("  [sched] qd=1  iops=%.0f par=%.2f | qd=16 iops=%.0f par=%.2f\n",
           r1.iops, r1.parallelism, r16.iops, r16.parallelism);
}

static void test_latency_repeatable(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t a;
    run_result_t b;

    run_once(&cfg, 8u, 5000u, 0u, &a);
    run_once(&cfg, 8u, 5000u, 0u, &b);

    /* A/B 对比全部建立在"可复现"之上，同 seed 必须逐位相同 */
    CHECK(a.p50_ns == b.p50_ns);
    CHECK(a.p99_ns == b.p99_ns);
    CHECK(a.completed == b.completed);

    printf("  [sched] repeatable: p50=%llu p99=%llu (run2 p50=%llu p99=%llu)\n",
           (unsigned long long)a.p50_ns, (unsigned long long)a.p99_ns,
           (unsigned long long)b.p50_ns, (unsigned long long)b.p99_ns);
}

static void test_read_write_mix(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t r;

    /* 30% 读插在写中间：读请求不能破坏映射，也不能影响最终数据 */
    run_once(&cfg, 8u, 8000u, 300u, &r);

    CHECK(r.completed == 8000u);
    CHECK(r.bad == 0u);

    printf("  [sched] mixed r/w: io=%llu bad=%llu\n",
           (unsigned long long)r.completed, (unsigned long long)r.bad);
}

static void test_idle_bg_gc(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t r;

    /* 打开后台 GC，并让写入量超过容量，逼出回收 */
    cfg.bg_gc_percent = 10u;
    run_once(&cfg, 4u, 24000u, 0u, &r);

    /* host 队列没填满时应当补过回收 —— 那才是真正的 idle 时段 */
    CHECK(r.idle_gc > 0u);
    CHECK(r.bad == 0u);

    printf("  [sched] idle bg gc: %llu rounds, bad=%llu\n",
           (unsigned long long)r.idle_gc, (unsigned long long)r.bad);
}

/*
 * 回收阶段也必须吃满多 CE。
 *
 * 这条最容易退化：搬移要是把整块的有效页都写进同一个块，
 * 那就是在同一颗 die 上排队编程 —— 盘上其余 CE 全程围观。
 * 实测退化时 100 万请求要跑 5490s（并行度 1.25），
 * 条带化之后 2430s（并行度 2.84）。
 */
static void test_gc_runs_in_parallel(void)
{
    ssd_config_t cfg = make_cfg_concurrent();
    run_result_t r;

    /* 写入量远超容量：后半程一直在回收，并行度由搬移决定 */
    run_once(&cfg, 8u, 24000u, 0u, &r);

    CHECK(r.gc_count > 0u);
    CHECK(r.parallelism > 2.0);
    CHECK(r.bad == 0u);

    printf("  [sched] gc pressure: gc=%llu par=%.2f bad=%llu\n",
           (unsigned long long)r.gc_count, r.parallelism,
           (unsigned long long)r.bad);
}

int sched_tests_run(void)
{
    g_checks = 0;
    g_failed = 0;

    printf("[sched] concurrency / parallelism / latency\n");
    test_concurrent_correctness();
    test_parallelism_scales();
    test_latency_repeatable();
    test_read_write_mix();
    test_idle_bg_gc();
    test_gc_runs_in_parallel();

    printf("[sched] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
