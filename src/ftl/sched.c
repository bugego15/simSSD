#include "ftl/sched.h"
#include "core/assert.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/stats.h"

#include <stdlib.h>
#include <string.h>

#define SCHED_MAX_ATTEMPTS 3u

static void sched_io_done(void *arg, int status);

int sched_init(ssd_sched_t *s, ftl_dev_t *f, nand_dev_t *nd, uint32_t qdepth)
{
    if (s == NULL || f == NULL || nd == NULL) {
        return SSD_ERR_INVAL;
    }
    if (qdepth == 0u) {
        qdepth = 1u;
    }

    memset(s, 0, sizeof(*s));
    s->ftl    = f;
    s->nand   = nd;
    s->qdepth = qdepth;
    s->reqs   = (ssd_req_t *)calloc((size_t)qdepth, sizeof(ssd_req_t));
    if (s->reqs == NULL) {
        return SSD_ERR_NO_MEM;
    }

    ssd_lat_init(&s->lat_w);
    ssd_lat_init(&s->lat_r);
    return SSD_OK;
}

void sched_deinit(ssd_sched_t *s)
{
    if (s == NULL) {
        return;
    }
    ssd_lat_clear(&s->lat_w);
    ssd_lat_clear(&s->lat_r);
    free(s->reqs);
    memset(s, 0, sizeof(*s));
}

void sched_set_done_cb(ssd_sched_t *s, sched_done_cb cb, void *arg)
{
    if (s == NULL) {
        return;
    }
    s->done_cb  = cb;
    s->done_arg = arg;
}

uint32_t sched_inflight(const ssd_sched_t *s)
{
    SSD_ASSERT(s != NULL);
    return s->inflight;
}

static ssd_req_t *sched_alloc_req(ssd_sched_t *s)
{
    uint32_t i;

    for (i = 0; i < s->qdepth; i++) {
        if (!s->reqs[i].in_flight) {
            return &s->reqs[i];
        }
    }
    return NULL;
}

/*
 * retry=true 表示这是同一个请求的重发，不是新请求。
 *
 * 必须区分开：attempts 是"这个请求试了几次"，而不是"这个槽位用过几次"。
 * 槽位是从对象池里循环取的，若不区分，计数会一路累积 ——
 * 队列跑几轮之后每个槽位的 attempts 都超过上限，
 * 于是此后所有编程失败都被直接判死，重试形同虚设
 *（实测故障注入下 retries=0 而 errors=198，就是这个原因）。
 */
static int sched_submit_ex(ssd_sched_t *s, ssd_req_type_t type, lba_t lba,
                           uint32_t seq, bool retry)
{
    ssd_req_t *r;
    int        rc;

    if (s == NULL || s->ftl == NULL) {
        return SSD_ERR_INVAL;
    }

    r = sched_alloc_req(s);
    if (r == NULL) {
        return SSD_ERR_BUSY;   /* 队列已满 */
    }

    r->sched     = s;
    r->type      = type;
    r->lba       = lba;
    r->seq       = seq;
    r->issue_ns  = ssd_clock_now();
    r->done_ns   = 0u;
    r->status    = SSD_OK;
    r->need_retry = false;
    r->attempts   = retry ? (r->attempts + 1u) : 1u;
    memset(&r->meta, 0, sizeof(r->meta));

    if (type == SSD_REQ_WRITE) {
        /* 旧映射只在首次提交时记一次：重发不能把它覆盖成"上一次失败的位置" */
        if (!retry) {
            r->old_ppn = ftl_l2p_get(s->ftl, lba);
        }
        rc = ftl_write_async(s->ftl, lba, seq, &r->ppn, sched_io_done, r);
    } else {
        rc = ftl_read_async(s->ftl, lba, &r->meta, sched_io_done, r);
        if (rc == SSD_ERR_NO_SPACE) {
            /* lba 从未写过：不必访问介质，请求直接完成。
             * 真实盘对未映射地址的读也是立即返回，延迟只含 FTL 查表。 */
            r->done_ns = ssd_clock_now();
            ssd_lat_add(&s->lat_r, r->done_ns - r->issue_ns);
            s->completed++;
            s->missed++;
            return SSD_OK;
        }
    }
    if (rc != SSD_OK) {
        return rc;   /* 提交失败：槽位没有被占用，调用方可以稍后重试 */
    }

    r->in_flight = true;
    s->inflight++;
    return SSD_OK;
}

int sched_submit(ssd_sched_t *s, ssd_req_type_t type, lba_t lba, uint32_t seq)
{
    return sched_submit_ex(s, type, lba, seq, false);
}

/*
 * 介质操作完成回调。
 *
 * 这里只做记账，绝不再提交新请求：本函数是在 nand_step() 里被调用的，
 * 若在此提交，而提交又因分配触发了同步 GC，就会递归回到 nand_step。
 * 重发交给主循环处理。
 */
static void sched_io_done(void *arg, int status)
{
    ssd_req_t   *r = (ssd_req_t *)arg;
    ssd_sched_t *s = r->sched;
    uint64_t     lat;

    r->in_flight = false;
    if (s->inflight > 0u) {
        s->inflight--;
    }
    r->status  = status;
    r->done_ns = ssd_clock_now();
    lat        = (r->done_ns > r->issue_ns) ? (r->done_ns - r->issue_ns) : 0u;

    /* 对账：这一页在编程期间若已被更新的写覆盖，必须立刻判成垃圾，
     * 否则 GC 会把它当有效数据搬回去，让映射静默退回旧版本 */
    if (r->type == SSD_REQ_WRITE) {
        ftl_prog_settled(s->ftl, r->lba, r->ppn, status);
    }

    if (status != SSD_OK) {
        if (r->attempts < SCHED_MAX_ATTEMPTS) {
            r->need_retry = true;
            s->retries++;
            return;
        }
        s->errors++;
        /*
         * 重试到头：这一页始终没写进去。
         * 映射却已经在提交那一刻指向它了 —— 必须回滚，
         * 否则这个 lba 新旧两版都读不到，一次可恢复的编程故障
         * 就变成了实打实的数据丢失。
         */
        if (r->type == SSD_REQ_WRITE) {
            ftl_write_abandon(s->ftl, r->lba, r->ppn, r->old_ppn);
        }
    }

    /* 只有真正完成的命令才算数：掉电时重发中的请求不能算作"已确认" */
    if (status == SSD_OK && s->done_cb != NULL) {
        s->done_cb(s->done_arg, r->type, r->lba, r->seq, status);
    }

    ssd_lat_add((r->type == SSD_REQ_WRITE) ? &s->lat_w : &s->lat_r, lat);
    s->completed++;
}

/*
 * 只下发、不推进事件：把队列补到有 n 个请求在飞。
 *
 * 掉电模拟必须靠它 —— 真实断电不会挑"盘刚好闲下来"的瞬间，
 * 而队列跑完一段之后往往是空的：前台 GC 内部要 run_until_idle，
 * 顺手就把在飞的操作全跑完了（实测 sched_run 直接返回时 inflight 恒为 0，
 * 一个撕裂页都造不出来）。这里补进来的请求不会完成，
 * 于是掉电必然打断一批页编程。
 */
uint32_t sched_fill(ssd_sched_t *s, sched_gen_cb gen, void *arg, uint32_t n)
{
    uint32_t done = 0u;

    if (s == NULL || gen == NULL) {
        return 0u;
    }
    while (done < n && s->inflight < s->qdepth) {
        ssd_req_type_t type = SSD_REQ_WRITE;
        lba_t          lba  = 0u;
        uint32_t       seq  = 0u;

        gen(arg, &type, &lba, &seq);
        if (sched_submit(s, type, lba, seq) != SSD_OK) {
            break;   /* 盘不收了：有多少算多少 */
        }
        s->issued++;
        done++;
    }
    return done;
}

void sched_idle_gc(ssd_sched_t *s)
{
    if (s == NULL || s->ftl == NULL) {
        return;
    }
    /* 后台 GC 关掉（bg_target=0）就不做 */
    if (s->ftl->bg_target == 0u) {
        return;
    }
    if (ftl_free_blocks(s->ftl) >= s->ftl->bg_target) {
        return;
    }

    /*
     * host 队列没填满，说明盘上还有富余带宽 —— 这才是真正的 idle 时段。
     * 在这里提前回收，host 写时就不必被迫等前台 GC，尾延迟因此下降。
     * 一次只收一个块：慢慢补，避免一口气做太多反而把 host 挡住。
     * （S3 里"每次 host 写都补齐水位"的做法实测会把 WAF 抬高 25%，
     *   问题就出在它不是 idle 时段 —— 那其实是在抢 host 的带宽。）
     */
    s->ftl->gc_background = true;
    if (ftl_gc_one(s->ftl) == SSD_OK) {
        s->idle_gc_count++;
    }
    s->ftl->gc_background = false;
}

/*
 * S6：一段真实的空闲时间。
 *
 * 空闲不等于"什么都不做"：盘会趁这段没人等的时候补后台 GC。
 * 但也不能一直收到水位线为止 —— 空闲时间是有限的，
 * 收一次花掉的时间要从预算里扣，扣完了就真的空转。
 */
static void sched_idle_span(ssd_sched_t *s, uint64_t idle_ns)
{
    uint64_t left = idle_ns;

    while (left > 0u) {
        uint64_t t0 = ssd_clock_now();
        uint64_t spent;

        sched_idle_gc(s);
        spent = ssd_clock_now() - t0;
        if (spent == 0u) {
            break;   /* 水位够了、或者根本没垃圾可收：剩下的时间纯空转 */
        }
        if (spent >= left) {
            break;
        }
        left -= spent;
    }
    ssd_clock_advance(left);
    s->idle_ns += idle_ns;
}

int sched_run_bursty(ssd_sched_t *s, uint64_t total, sched_gen_cb gen,
                     void *arg, uint64_t burst, uint64_t idle_ns,
                     uint64_t cut_at)
{
    if (s == NULL || gen == NULL) {
        return SSD_ERR_INVAL;
    }

    while (s->completed < total) {
        uint64_t seg = total - s->completed;
        uint64_t stop_at;

        if (burst > 0u && seg > burst) {
            seg = burst;
        }
        stop_at = s->completed + seg;

        (void)sched_run(s, stop_at, gen, arg, cut_at);

        if (cut_at != 0u && s->completed >= cut_at) {
            break;   /* 掉电点：在飞的请求要留给断电去打断 */
        }
        if (s->completed >= total) {
            break;
        }
        if (burst == 0u || idle_ns == 0u) {
            break;   /* 闭环模式（或没有空闲时段）：一次跑完 */
        }
        sched_idle_span(s, idle_ns);
    }
    return SSD_OK;
}

static void sched_flush_retries(ssd_sched_t *s)
{
    uint32_t i;

    for (i = 0; i < s->qdepth; i++) {
        ssd_req_t *r = &s->reqs[i];

        if (!r->need_retry) {
            continue;
        }
        r->need_retry = false;
        if (sched_submit_ex(s, r->type, r->lba, r->seq, true) != SSD_OK) {
            /* 重发不进去（盘真满了）：记为失败，避免主循环空转 */
            s->errors++;
            s->completed++;
        }
    }
}

int sched_run(ssd_sched_t *s, uint64_t total, sched_gen_cb gen, void *arg,
              uint64_t cut_at)
{
    uint32_t guard = 0u;
    uint64_t stop;
    bool     stop_early;

    if (s == NULL || gen == NULL) {
        return SSD_ERR_INVAL;
    }
    stop_early = (cut_at != 0u);
    stop       = (cut_at == 0u || cut_at > total) ? total : cut_at;

    while (s->completed < total) {
        /* 断点：到达 cut_at 就收手，且不排空在飞的请求（见 sched_fill） */
        if (stop_early && s->completed >= stop) {
            break;
        }

        /* 1. 重发上一次失败的 */
        sched_flush_retries(s);

        /* 2. 尽量把队列填满 */
        while (s->inflight < s->qdepth && s->issued < total) {
            ssd_req_type_t type = SSD_REQ_WRITE;
            lba_t          lba  = 0u;
            uint32_t       seq  = 0u;

            gen(arg, &type, &lba, &seq);
            if (sched_submit(s, type, lba, seq) != SSD_OK) {
                break;   /* 盘拒绝接收（空间不足）：停手，避免空转 */
            }
            s->issued++;
        }

        /* 3. 队列没填满 = 有富余带宽 = idle 时段，补一点后台 GC */
        if (s->inflight < s->qdepth) {
            sched_idle_gc(s);
        }

        /* 4. 推进一个事件 */
        if (s->inflight == 0u) {
            break;
        }
        if (nand_step(s->nand) != SSD_OK) {
            break;   /* 事件队列空了但还有请求在飞 —— 模型不自洽，停手 */
        }

        if (++guard > (total * 4u + 1024u)) {
            SSD_ERR("sched: guard tripped (completed=%llu/%llu)",
                    (unsigned long long)s->completed,
                    (unsigned long long)total);
            break;
        }
    }

    /*
     * 收尾：把还在飞的请求跑完。
     * cut_at 触发的提前返回不做这一步 —— 队列里的请求要留给掉电去打断。
     */
    if (!stop_early) {
        while (s->inflight > 0u) {
            sched_flush_retries(s);
            if (nand_step(s->nand) != SSD_OK) {
                break;
            }
        }
    }

    ssd_lat_finish(&s->lat_w);
    ssd_lat_finish(&s->lat_r);
    return SSD_OK;
}
