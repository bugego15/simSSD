#ifndef SSD_FTL_SCHED_H
#define SSD_FTL_SCHED_H

#include "core/config.h"
#include "core/latency.h"
#include "ftl/ftl.h"
#include "media/nand.h"
#include "ssd_types.h"

/*
 * host 请求调度器（S4）。
 *
 * 采用闭环（closed-loop）模型：始终维持 qdepth 个请求在盘上，
 * 完成一个立刻补一个。这是量测 IOPS 与延迟的标准做法 ——
 * 盘的繁忙程度由队列深度决定，而不是由"多久发一个请求"决定。
 *
 * 并发是怎么来的：
 *   请求 A 提交页编程后立刻返回，不等它完成就提交请求 B。
 *   A 占用 CE0 编程时，B 可以走通道去 CE1。
 *   所以并发度 = min(qdepth, CE 数)，前提是 FTL 处理本身不阻塞。
 *
 * 唯一的阻塞点是前台 GC：分配不到物理页时必须同步回收，
 * 这段时间 host 请求被挡在后面 —— 它就是尾延迟（p99/p99.9）的主要来源。
 */

typedef enum ssd_req_type {
    SSD_REQ_WRITE = 0,
    SSD_REQ_READ  = 1
} ssd_req_type_t;

typedef struct ssd_sched ssd_sched_t;

typedef struct ssd_req {
    ssd_sched_t      *sched;
    ssd_req_type_t    type;
    lba_t             lba;
    uint32_t          seq;
    uint64_t          issue_ns;
    uint64_t          done_ns;
    ppn_t             ppn;       /* 写请求落到的物理页（用于对账） */
    ppn_t             old_ppn;   /* S6：提交前该 lba 的映射，写彻底失败时要回滚到这里 */
    nand_page_meta_t  meta;      /* 读请求的输出 */
    int               status;
    uint32_t          attempts;
    bool              in_flight;
    bool              need_retry;
} ssd_req_t;

/* 请求生成器：负载模型由基准提供（uniform / hotspot / 读写比例） */
typedef void (*sched_gen_cb)(void *arg, ssd_req_type_t *type,
                             lba_t *lba, uint32_t *seq);

/*
 * 请求完成通知（S5 用）。
 *
 * 掉电校验必须区分"提交了"和"确认完成了"：
 * 还在盘上飞的请求，掉电后丢掉是合法的（host 也不会收到完成通知）；
 * 已经收到完成通知的写，掉电后必须还在 —— 这是 SPOR 的底线。
 * 基准程序靠这个回调把"最后确认值"单独记一份。
 */
typedef void (*sched_done_cb)(void *arg, ssd_req_type_t type,
                              lba_t lba, uint32_t seq, int status);

struct ssd_sched {
    ftl_dev_t  *ftl;
    nand_dev_t *nand;

    sched_done_cb done_cb;
    void         *done_arg;

    ssd_req_t  *reqs;        /* 请求对象池 */
    uint32_t    qdepth;
    uint32_t    inflight;

    uint64_t    issued;
    uint64_t    completed;
    uint64_t    retries;     /* 编程失败后重发的次数 */
    uint64_t    errors;      /* 最终失败 */
    uint64_t    missed;      /* 读未命中（lba 从未写过，不必访问介质） */

    ssd_lat_t   lat_w;
    ssd_lat_t   lat_r;

    uint64_t    idle_gc_count;   /* 在 idle 时段做的后台 GC 次数 */
    uint64_t    idle_ns;         /* S6：间歇型负载里真正空转掉的虚拟时间 */
};

int      sched_init(ssd_sched_t *s, ftl_dev_t *f, nand_dev_t *nd, uint32_t qdepth);
void     sched_deinit(ssd_sched_t *s);
void     sched_set_done_cb(ssd_sched_t *s, sched_done_cb cb, void *arg);

int      sched_submit(ssd_sched_t *s, ssd_req_type_t type, lba_t lba, uint32_t seq);
uint32_t sched_inflight(const ssd_sched_t *s);

/*
 * 跑一轮：填队列 -> idle 时补 GC -> 推进事件，直到 total 个请求都完成。
 *
 * cut_at != 0 时，一旦完成数达到 cut_at 就立刻返回，**不排空在飞的请求**。
 * 注意这并不保证返回时队列非空 —— 前台 GC 会顺手把队列跑空（见 sched_fill）。
 */
int      sched_run(ssd_sched_t *s, uint64_t total, sched_gen_cb gen,
                   void *arg, uint64_t cut_at);

/*
 * S6：间歇型（bursty）负载。
 *
 * 闭环恒满的队列没有"空闲"这回事 —— 队列里始终有请求在飞，
 * 于是"后台 GC"永远等不到它的 idle 时段，开与不开跑出来的数字一模一样。
 * 真实业务却是忙一阵歇一阵的：这轮请求发完，host 要算一会儿才发下一轮。
 *
 * burst=0 表示退回闭环模式（一次跑完）；否则每轮发 burst 个请求、
 * 等它们落地，然后空转 idle_ns —— 空转期间盘不是闲着，
 * 而是拿去做后台 GC，把 host 的等待换成可用块。
 */
int      sched_run_bursty(ssd_sched_t *s, uint64_t total, sched_gen_cb gen,
                          void *arg, uint64_t burst, uint64_t idle_ns,
                          uint64_t cut_at);

/* 只下发不推进事件，把队列补到有 n 个请求在飞（掉电模拟用） */
uint32_t sched_fill(ssd_sched_t *s, sched_gen_cb gen, void *arg, uint32_t n);

/* host 队列没填满时补一次后台 GC —— 这才是真正的 idle 时段 */
void     sched_idle_gc(ssd_sched_t *s);

#endif /* SSD_FTL_SCHED_H */
