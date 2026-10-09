#include "core/mempool.h"
#include "core/assert.h"
#include "ssd_types.h"

#include <string.h>

void ssd_pool_init(ssd_mempool_t *pool, void *buf, size_t capacity)
{
    SSD_ASSERT(pool != NULL);
    SSD_ASSERT(buf != NULL);
    SSD_ASSERT(capacity > 0);

    pool->base     = (uint8_t *)buf;
    pool->capacity = capacity;
    pool->used     = 0;
    pool->peak     = 0;
}

void *ssd_pool_alloc(ssd_mempool_t *pool, size_t size, size_t align)
{
    size_t aligned_used;
    uint8_t *ptr;

    SSD_ASSERT(pool != NULL);
    SSD_ASSERT(pool->base != NULL);
    SSD_ASSERT(size > 0);
    SSD_ASSERT(align > 0);
    /* align 必须是 2 的幂 */
    SSD_ASSERT((align & (align - 1u)) == 0u);

    aligned_used = (size_t)SSD_ALIGN_UP(pool->used, align);
    if (aligned_used + size > pool->capacity) {
        SSD_BUG("mempool exhausted: static buffer too small");
    }

    ptr = pool->base + aligned_used;
    pool->used = aligned_used + size;
    if (pool->used > pool->peak) {
        pool->peak = pool->used;
    }

    memset(ptr, 0, size);
    return ptr;
}

void ssd_pool_reset(ssd_mempool_t *pool)
{
    SSD_ASSERT(pool != NULL);
    pool->used = 0;
}

size_t ssd_pool_used(const ssd_mempool_t *pool)
{
    SSD_ASSERT(pool != NULL);
    return pool->used;
}

size_t ssd_pool_peak(const ssd_mempool_t *pool)
{
    SSD_ASSERT(pool != NULL);
    return pool->peak;
}

size_t ssd_pool_capacity(const ssd_mempool_t *pool)
{
    SSD_ASSERT(pool != NULL);
    return pool->capacity;
}
