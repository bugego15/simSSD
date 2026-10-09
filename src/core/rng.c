#include "core/rng.h"

#include <stddef.h>

#define RNG_MAGIC_CONST 2685821657736338717ULL
#define RNG_DEFAULT_SEED 0x9E3779B97F4A7C15ULL

void ssd_rng_seed(ssd_rng_t *r, uint64_t seed)
{
    if (r == NULL) {
        return;
    }
    /* xorshift 的状态不能为 0，否则序列恒为 0 */
    r->state = (seed == 0) ? RNG_DEFAULT_SEED : seed;
}

uint64_t ssd_rng_u64(ssd_rng_t *r)
{
    uint64_t x;

    if (r == NULL) {
        return 0;
    }
    x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    return x * RNG_MAGIC_CONST;
}

uint32_t ssd_rng_u32(ssd_rng_t *r)
{
    return (uint32_t)(ssd_rng_u64(r) >> 32);
}

uint32_t ssd_rng_below(ssd_rng_t *r, uint32_t n)
{
    if (n == 0u) {
        return 0u;
    }
    return (uint32_t)(ssd_rng_u64(r) % (uint64_t)n);
}

bool ssd_rng_chance_permille(ssd_rng_t *r, uint32_t permille)
{
    if (permille == 0u) {
        return false;
    }
    if (permille >= 1000u) {
        return true;
    }
    return ssd_rng_below(r, 1000u) < permille;
}
