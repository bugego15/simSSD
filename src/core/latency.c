#include "core/latency.h"
#include "core/assert.h"
#include "ssd_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ssd_lat_init(ssd_lat_t *l)
{
    SSD_ASSERT(l != NULL);
    memset(l, 0, sizeof(*l));
    l->min_ns = UINT64_MAX;
}

void ssd_lat_clear(ssd_lat_t *l)
{
    SSD_ASSERT(l != NULL);
    free(l->s);
    memset(l, 0, sizeof(*l));
    l->min_ns = UINT64_MAX;
}

int ssd_lat_reserve(ssd_lat_t *l, uint32_t cap)
{
    uint64_t *p;

    SSD_ASSERT(l != NULL);
    if (cap <= l->cap) {
        return SSD_OK;
    }

    p = (uint64_t *)realloc(l->s, (size_t)cap * sizeof(uint64_t));
    if (p == NULL) {
        return SSD_ERR_NO_MEM;
    }
    l->s   = p;
    l->cap = cap;
    return SSD_OK;
}

static int lat_grow(ssd_lat_t *l)
{
    uint32_t want = (l->cap == 0u) ? 1024u : (l->cap * 2u);

    /* 溢出保护：样本量到千万级已经远超需求 */
    if (l->cap > (UINT32_MAX / 2u)) {
        return SSD_ERR_NO_SPACE;
    }
    return ssd_lat_reserve(l, want);
}

void ssd_lat_add(ssd_lat_t *l, uint64_t ns)
{
    SSD_ASSERT(l != NULL);

    if (l->n == l->cap && lat_grow(l) != SSD_OK) {
        return;   /* 内存不够就丢弃该样本，不能让基准跑不下去 */
    }

    l->s[l->n++] = ns;
    l->sum += ns;
    if (ns < l->min_ns) {
        l->min_ns = ns;
    }
    if (ns > l->max_ns) {
        l->max_ns = ns;
    }
    l->sorted = false;
}

static int lat_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

void ssd_lat_finish(ssd_lat_t *l)
{
    SSD_ASSERT(l != NULL);
    if (l->n > 1u) {
        qsort(l->s, (size_t)l->n, sizeof(uint64_t), lat_cmp);
    }
    l->sorted = true;
}

void ssd_lat_merge(ssd_lat_t *dst, const ssd_lat_t *src)
{
    uint32_t i;

    if (dst == NULL || src == NULL) {
        return;
    }
    /* 掉电只发生在段与段之间，合并次数很少，逐个追加足够 */
    for (i = 0; i < src->n; i++) {
        ssd_lat_add(dst, src->s[i]);
    }
}

uint32_t ssd_lat_count(const ssd_lat_t *l)
{
    SSD_ASSERT(l != NULL);
    return l->n;
}

double ssd_lat_mean(const ssd_lat_t *l)
{
    SSD_ASSERT(l != NULL);
    if (l->n == 0u) {
        return 0.0;
    }
    return (double)l->sum / (double)l->n;
}

uint64_t ssd_lat_percentile(const ssd_lat_t *l, double p)
{
    double   rank;
    uint32_t idx;

    SSD_ASSERT(l != NULL);
    if (l->n == 0u) {
        return 0u;
    }
    if (!l->sorted) {
        /* 未排序就取分位数是没有意义的，返回 0 让问题立刻暴露 */
        return 0u;
    }

    if (p <= 0.0) {
        p = 0.0;
    }
    if (p > 1.0) {
        p = 1.0;
    }

    /* nearest-rank：第 ceil(p*n) 个样本 */
    rank = p * (double)l->n;
    idx  = (uint32_t)(rank + 0.999999);
    if (idx == 0u) {
        idx = 1u;
    }
    if (idx > l->n) {
        idx = l->n;
    }
    return l->s[idx - 1u];
}

void ssd_lat_fmt(uint64_t ns, char *buf, uint32_t buflen)
{
    double      v;
    const char *u;

    if (buflen == 0u) {
        return;
    }
    if (ns < 1000u) {
        v = (double)ns;
        u = "ns";
    } else if (ns < 1000000u) {
        v = (double)ns / 1e3;
        u = "us";
    } else if (ns < 1000000000u) {
        v = (double)ns / 1e6;
        u = "ms";
    } else {
        v = (double)ns / 1e9;
        u = "s";
    }
    (void)snprintf(buf, (size_t)buflen, "%.1f%s", v, u);
}

void ssd_lat_dump(const char *name, const ssd_lat_t *l)
{
    char p50[32], p90[32], p99[32], p999[32], mean[32], max[32];

    SSD_ASSERT(l != NULL);
    if (l->n == 0u) {
        printf("  %-14s no samples\n", (name != NULL) ? name : "latency");
        return;
    }

    ssd_lat_fmt(ssd_lat_percentile(l, 0.5),   p50,  sizeof(p50));
    ssd_lat_fmt(ssd_lat_percentile(l, 0.9),   p90,  sizeof(p90));
    ssd_lat_fmt(ssd_lat_percentile(l, 0.99),  p99,  sizeof(p99));
    ssd_lat_fmt(ssd_lat_percentile(l, 0.999), p999, sizeof(p999));
    ssd_lat_fmt((uint64_t)ssd_lat_mean(l),    mean, sizeof(mean));
    ssd_lat_fmt(l->max_ns,                    max,  sizeof(max));

    printf("  %-14s n=%u  p50 %s | p90 %s | p99 %s | p99.9 %s | mean %s | max %s\n",
           (name != NULL) ? name : "latency",
           l->n, p50, p90, p99, p999, mean, max);
}
