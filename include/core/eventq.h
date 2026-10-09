#ifndef SSD_CORE_EVENTQ_H
#define SSD_CORE_EVENTQ_H

#include <stdint.h>

/*
 * 离散事件队列（最小堆），按 (time, seq) 排序。
 *
 * 模拟器的时间推进完全由它驱动：
 *   提交 NAND 操作 -> 计算完成时间 -> 入队
 *   出队 -> 把虚拟时钟拨到该事件时刻 -> 执行回调
 * 因此"仿真几个月盘寿命"只需要几秒真实时间。
 */

typedef void (*ssd_evt_cb)(void *arg);

typedef struct ssd_event {
    uint64_t   time;
    ssd_evt_cb cb;
    void      *arg;
    uint64_t   seq;      /* 同一时刻事件的稳定次序（先提交先执行） */
} ssd_event_t;

typedef struct ssd_evtq {
    ssd_event_t *heap;
    uint32_t     cap;
    uint32_t     size;
    uint64_t     seq;
} ssd_evtq_t;

void     ssd_evtq_init(ssd_evtq_t *q, ssd_event_t *heap, uint32_t cap);
void     ssd_evtq_clear(ssd_evtq_t *q);
int      ssd_evtq_push(ssd_evtq_t *q, uint64_t time, ssd_evt_cb cb, void *arg);
int      ssd_evtq_pop(ssd_evtq_t *q, ssd_event_t *out);

uint32_t ssd_evtq_size(const ssd_evtq_t *q);

/* 队首事件时刻；队列为空返回 UINT64_MAX */
uint64_t ssd_evtq_next_time(const ssd_evtq_t *q);

#endif /* SSD_CORE_EVENTQ_H */
