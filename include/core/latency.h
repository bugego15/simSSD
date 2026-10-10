#ifndef SSD_CORE_LATENCY_H
#define SSD_CORE_LATENCY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * IO 延迟样本采集（S4）。
 *
 * 每个请求从"下发到盘"到"完成回调"记一个样本，最终给出分位数。
 *
 * 为什么存全量样本而不是直方图：
 *   百万级样本 × 8 字节 ≈ 8 MB，完全可以接受；
 *   换成直方图就得先猜延迟分布范围，p999 这种尾部指标很容易被分桶抹平。
 *   分位数要用来做验收结论，宁可多花几 MB 也要保证精确。
 */

typedef struct ssd_lat {
    uint64_t *s;        /* 样本（ns） */
    uint32_t  n;        /* 已采集样本数 */
    uint32_t  cap;      /* 数组容量 */
    uint64_t  sum;      /* 总和，用于算均值 */
    uint64_t  min_ns;
    uint64_t  max_ns;
    bool      sorted;   /* 取分位数前必须先 finish */
} ssd_lat_t;

void     ssd_lat_init(ssd_lat_t *l);
void     ssd_lat_clear(ssd_lat_t *l);
int      ssd_lat_reserve(ssd_lat_t *l, uint32_t cap);

void     ssd_lat_add(ssd_lat_t *l, uint64_t ns);
void     ssd_lat_finish(ssd_lat_t *l);   /* 排序；之后才能取分位数 */

/* 把另一组样本并进来（S5：掉电会重建调度器，延迟样本必须跨段累加） */
void     ssd_lat_merge(ssd_lat_t *dst, const ssd_lat_t *src);

uint32_t ssd_lat_count(const ssd_lat_t *l);
double   ssd_lat_mean(const ssd_lat_t *l);
/* p ∈ (0,1]，例如 0.99；采用 nearest-rank 定义 */
uint64_t ssd_lat_percentile(const ssd_lat_t *l, double p);

/* 打印 p50 / p90 / p99 / p99.9 / mean / max，单位自动在 ns/us/ms 间切换 */
void     ssd_lat_dump(const char *name, const ssd_lat_t *l);
void     ssd_lat_fmt(uint64_t ns, char *buf, uint32_t buflen);

#endif /* SSD_CORE_LATENCY_H */
