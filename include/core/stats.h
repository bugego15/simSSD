#ifndef SSD_CORE_STATS_H
#define SSD_CORE_STATS_H

#include <stdint.h>

/*
 * 全局计数器。所有模块的量化指标都汇总到这里，
 * WAF、介质利用率、GC 开销等均由这些基础计数派生。
 */

enum ssd_stat_id {
    /* --- 主机侧 --- */
    ST_HOST_READ_CMDS = 0,
    ST_HOST_WRITE_CMDS,
    ST_HOST_TRIM_CMDS,
    ST_HOST_FLUSH_CMDS,
    ST_HOST_READ_PAGES,
    ST_HOST_WRITE_PAGES,

    /* --- NAND 侧 --- */
    ST_NAND_PROG_PAGES,
    ST_NAND_READ_PAGES,
    ST_NAND_ERASE_BLOCKS,

    /* --- FTL 内部 --- */
    ST_GC_ERASES,
    ST_GC_COPY_PAGES,
    ST_WL_MIGRATE_PAGES,
    ST_BADBLOCK_RUNTIME,

    /* --- S5：掉电恢复 --- */
    ST_SPOR_RECOVERIES, /* 上电重建次数 */
    ST_SPOR_TORN_PAGES, /* 掉电撕裂的页数（半写的页） */
    ST_SPOR_STALE_PAGES,/* 重建时判为旧版本/垃圾的页数 */

    /* --- 时间 --- */
    ST_MEDIA_BUSY_NS,   /* 介质处于忙状态的累计时间，用于算利用率 */

    ST_COUNT
};

void     ssd_stats_init(void);
void     ssd_stats_reset(void);
void     ssd_stats_inc(enum ssd_stat_id id);
void     ssd_stats_add(enum ssd_stat_id id, uint64_t delta);
uint64_t ssd_stats_get(enum ssd_stat_id id);

/* 派生指标 */
double   ssd_stats_waf(void);              /* NAND 写页数 / 主机写页数 */
double   ssd_stats_media_utilization(void);/* 介质忙时间 / 仿真总时间 */
double   ssd_stats_gc_overhead(void);      /* GC 搬移页 / 主机写页 */

void     ssd_stats_dump(void);

#endif /* SSD_CORE_STATS_H */
