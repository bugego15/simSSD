#include "core/bitmap.h"
#include "core/assert.h"

#include <stddef.h>

void ssd_bitmap_zero(uint32_t *bm, uint32_t nbits)
{
    uint32_t w;
    uint32_t words = SSD_BITMAP_WORDS(nbits);

    SSD_ASSERT(bm != NULL);
    for (w = 0; w < words; w++) {
        bm[w] = 0u;
    }
}

void ssd_bitmap_fill(uint32_t *bm, uint32_t nbits)
{
    uint32_t w;
    uint32_t words = SSD_BITMAP_WORDS(nbits);
    uint32_t tail  = nbits & 31u;

    SSD_ASSERT(bm != NULL);
    for (w = 0; w < words; w++) {
        bm[w] = 0xFFFFFFFFu;
    }
    if (tail != 0u) {
        /* 高位补齐位不属于本图，清零避免污染统计 */
        bm[words - 1u] &= (tail == 32u) ? 0xFFFFFFFFu : ((1u << tail) - 1u);
    }
}

bool ssd_bitmap_get(const uint32_t *bm, uint32_t idx)
{
    SSD_ASSERT(bm != NULL);
    return ((bm[idx >> 5] >> (idx & 31u)) & 1u) != 0u;
}

void ssd_bitmap_set(uint32_t *bm, uint32_t idx)
{
    SSD_ASSERT(bm != NULL);
    bm[idx >> 5] |= (1u << (idx & 31u));
}

void ssd_bitmap_clear(uint32_t *bm, uint32_t idx)
{
    SSD_ASSERT(bm != NULL);
    bm[idx >> 5] &= ~(1u << (idx & 31u));
}

void ssd_bitmap_assign(uint32_t *bm, uint32_t idx, bool val)
{
    if (val) {
        ssd_bitmap_set(bm, idx);
    } else {
        ssd_bitmap_clear(bm, idx);
    }
}

int32_t ssd_bitmap_find_zero(const uint32_t *bm, uint32_t nbits, uint32_t start)
{
    uint32_t words = SSD_BITMAP_WORDS(nbits);
    uint32_t w;

    SSD_ASSERT(bm != NULL);
    if (start >= nbits) {
        return -1;
    }

    w = start >> 5;
    /* 首个字需要屏蔽 start 之前的位 */
    {
        uint32_t v = bm[w];
        uint32_t off = start & 31u;
        if (off != 0u) {
            v |= ((1u << off) - 1u);
        }
        if (v != 0xFFFFFFFFu) {
            uint32_t bit = 0;
            uint32_t t = ~v;
            while ((t & 1u) == 0u) {
                t >>= 1;
                bit++;
            }
            return (int32_t)((w << 5) + bit);
        }
    }

    for (w = w + 1u; w < words; w++) {
        if (bm[w] != 0xFFFFFFFFu) {
            uint32_t bit = 0;
            uint32_t t = ~bm[w];
            while ((t & 1u) == 0u) {
                t >>= 1;
                bit++;
            }
            return (int32_t)((w << 5) + bit);
        }
    }
    return -1;
}

uint32_t ssd_bitmap_count_set(const uint32_t *bm, uint32_t nbits)
{
    uint32_t words = SSD_BITMAP_WORDS(nbits);
    uint32_t tail  = nbits & 31u;
    uint32_t w;
    uint32_t cnt = 0;

    SSD_ASSERT(bm != NULL);
    for (w = 0; w < words; w++) {
        uint32_t v = bm[w];
        /* Brian Kernighan 位计数 */
        while (v != 0u) {
            v &= (v - 1u);
            cnt++;
        }
    }
    if (tail != 0u) {
        /* 扣除补齐位中多算的部分 */
        uint32_t v = bm[words - 1u] >> tail;
        while (v != 0u) {
            v &= (v - 1u);
            cnt--;
        }
    }
    return cnt;
}
