#include "core/eventq.h"
#include "core/assert.h"
#include "ssd_types.h"

#include <stddef.h>

/* 按 (time, seq) 比较，返回 true 表示 a 应排在 b 之前 */
static bool evt_earlier(const ssd_event_t *a, const ssd_event_t *b)
{
    if (a->time != b->time) {
        return a->time < b->time;
    }
    return a->seq < b->seq;
}

static void evt_swap(ssd_event_t *a, ssd_event_t *b)
{
    ssd_event_t tmp = *a;
    *a = *b;
    *b = tmp;
}

void ssd_evtq_init(ssd_evtq_t *q, ssd_event_t *heap, uint32_t cap)
{
    SSD_ASSERT(q != NULL);
    SSD_ASSERT(heap != NULL);
    SSD_ASSERT(cap > 0);

    q->heap = heap;
    q->cap  = cap;
    q->size = 0;
    q->seq  = 0;
}

void ssd_evtq_clear(ssd_evtq_t *q)
{
    SSD_ASSERT(q != NULL);
    q->size = 0;
    q->seq  = 0;
}

int ssd_evtq_push(ssd_evtq_t *q, uint64_t time, ssd_evt_cb cb, void *arg)
{
    uint32_t i;

    SSD_ASSERT(q != NULL);
    if (q->size >= q->cap) {
        return SSD_ERR_NO_SPACE;
    }

    i = q->size++;
    q->heap[i].time = time;
    q->heap[i].cb   = cb;
    q->heap[i].arg  = arg;
    q->heap[i].seq  = q->seq++;

    /* 上浮 */
    while (i > 0u) {
        uint32_t parent = (i - 1u) / 2u;
        if (!evt_earlier(&q->heap[i], &q->heap[parent])) {
            break;
        }
        evt_swap(&q->heap[i], &q->heap[parent]);
        i = parent;
    }
    return SSD_OK;
}

int ssd_evtq_pop(ssd_evtq_t *q, ssd_event_t *out)
{
    uint32_t i;

    SSD_ASSERT(q != NULL);
    SSD_ASSERT(out != NULL);

    if (q->size == 0u) {
        return SSD_ERR_NO_SPACE;
    }

    *out = q->heap[0];
    q->size--;
    if (q->size > 0u) {
        q->heap[0] = q->heap[q->size];

        /* 下沉 */
        i = 0u;
        for (;;) {
            uint32_t l = 2u * i + 1u;
            uint32_t r = l + 1u;
            uint32_t m = i;

            if (l < q->size && evt_earlier(&q->heap[l], &q->heap[m])) {
                m = l;
            }
            if (r < q->size && evt_earlier(&q->heap[r], &q->heap[m])) {
                m = r;
            }
            if (m == i) {
                break;
            }
            evt_swap(&q->heap[i], &q->heap[m]);
            i = m;
        }
    }
    return SSD_OK;
}

uint32_t ssd_evtq_size(const ssd_evtq_t *q)
{
    SSD_ASSERT(q != NULL);
    return q->size;
}

uint64_t ssd_evtq_next_time(const ssd_evtq_t *q)
{
    SSD_ASSERT(q != NULL);
    if (q->size == 0u) {
        return UINT64_MAX;
    }
    return q->heap[0].time;
}
