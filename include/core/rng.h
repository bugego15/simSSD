#ifndef SSD_CORE_RNG_H
#define SSD_CORE_RNG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 可复现伪随机数发生器（xorshift64*）。
 *
 * 为什么不用 rand()：
 *   1. rand() 的实现与平台相关，同一 seed 在不同机器上结果不同；
 *   2. 仿真实验必须可复现（坏块分布、故障注入），否则 A/B 对比没有意义。
 */

typedef struct ssd_rng {
    uint64_t state;
} ssd_rng_t;

void     ssd_rng_seed(ssd_rng_t *r, uint64_t seed);
uint32_t ssd_rng_u32(ssd_rng_t *r);
uint64_t ssd_rng_u64(ssd_rng_t *r);

/* 返回 [0, n) 内的随机数；n 为 0 时返回 0 */
uint32_t ssd_rng_below(ssd_rng_t *r, uint32_t n);

/* 千分比概率判定：permille=5 表示 0.5% 的概率返回 true */
bool     ssd_rng_chance_permille(ssd_rng_t *r, uint32_t permille);

#endif /* SSD_CORE_RNG_H */
