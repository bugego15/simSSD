#ifndef SSD_CORE_BITMAP_H
#define SSD_CORE_BITMAP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 位图：用于块分配表、有效页位图、坏块表等。
 * 以 uint32_t 为字，位序为小端（bit0 = word0 的最低位）。
 */

#define SSD_BITMAP_WORDS(nbits) (((uint32_t)(nbits) + 31u) / 32u)

void      ssd_bitmap_zero(uint32_t *bm, uint32_t nbits);
void      ssd_bitmap_fill(uint32_t *bm, uint32_t nbits);
bool      ssd_bitmap_get(const uint32_t *bm, uint32_t idx);
void      ssd_bitmap_set(uint32_t *bm, uint32_t idx);
void      ssd_bitmap_clear(uint32_t *bm, uint32_t idx);
void      ssd_bitmap_assign(uint32_t *bm, uint32_t idx, bool val);

/* 从 start 开始找第一个 0 位，找不到返回 -1 */
int32_t   ssd_bitmap_find_zero(const uint32_t *bm, uint32_t nbits, uint32_t start);

uint32_t  ssd_bitmap_count_set(const uint32_t *bm, uint32_t nbits);

#endif /* SSD_CORE_BITMAP_H */
