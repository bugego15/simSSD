#ifndef SSD_MEDIA_GEOMETRY_H
#define SSD_MEDIA_GEOMETRY_H

#include "core/config.h"
#include "ssd_types.h"

/*
 * NAND 拓扑与地址编解码。
 *
 * 层次：channel -> ce -> die -> plane -> block -> page
 *
 * 线性地址编码顺序（page 在最内层）：
 *   ppn = ((((ch * CE + ce) * DIE + die) * PLANE + plane) * BLK + block) * PAGE + page
 *   pbn = ppn / pages_per_block
 *
 * 之所以让 page 处于最内层，是为了让"同一 block 的页连续"，
 * 顺序写时 ppn 单调递增，后续 GC 与元数据扫描都依赖这个性质。
 */

typedef struct nand_addr {
    uint32_t channel;
    uint32_t ce;
    uint32_t die;
    uint32_t plane;
    uint32_t block;
    uint32_t page;
} nand_addr_t;

typedef struct nand_geometry {
    uint32_t channels;
    uint32_t ces_per_ch;
    uint32_t dies_per_ce;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;

    /* 派生：每级包含的 block 数，用于解码 */
    uint32_t blocks_per_die;
    uint32_t blocks_per_ce;
    uint32_t blocks_per_ch;

    uint64_t total_blocks;
    uint64_t total_pages;
} nand_geometry_t;

void     nand_geo_init(nand_geometry_t *g, const ssd_config_t *cfg);

ppn_t    nand_geo_ppn(const nand_geometry_t *g, const nand_addr_t *a);
void     nand_geo_decode(const nand_geometry_t *g, ppn_t ppn, nand_addr_t *out);

pbn_t    nand_geo_pbn(const nand_geometry_t *g, const nand_addr_t *a);
void     nand_geo_decode_block(const nand_geometry_t *g, pbn_t pbn, nand_addr_t *out);

/* block 的首个 ppn */
ppn_t    nand_geo_block_base(const nand_geometry_t *g, pbn_t pbn);
pbn_t    nand_geo_pbn_of_ppn(const nand_geometry_t *g, ppn_t ppn);

uint32_t nand_geo_channel_of_pbn(const nand_geometry_t *g, pbn_t pbn);
uint32_t nand_geo_channel_of_ppn(const nand_geometry_t *g, ppn_t ppn);

/* 全局 CE 索引 = channel * ces_per_ch + ce（S4 用于 CE 级排程）。
 * CE 是并发的基本单位：不同 CE 上的操作可以真正同时进行，
 * 同一个 CE 上则必须串行 —— 介质层靠它决定操作何时开始。 */
uint32_t nand_geo_ce_index_of_pbn(const nand_geometry_t *g, pbn_t pbn);
uint32_t nand_geo_ce_index_of_ppn(const nand_geometry_t *g, ppn_t ppn);
uint32_t nand_geo_ce_count(const nand_geometry_t *g);

#endif /* SSD_MEDIA_GEOMETRY_H */
