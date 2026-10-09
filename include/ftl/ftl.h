#ifndef SSD_FTL_FTL_H
#define SSD_FTL_FTL_H

#include "core/config.h"
#include "media/nand.h"
#include "ssd_types.h"

/*
 * FTL：页级映射 + 写/读/TRIM 路径 + 空间回收 + 磨损均衡 + 坏块管理
 *
 * 映射方案选型：page-level L2P，DRAM 驻留数组 [lba] -> ppn。
 *   - block-level 映射随机写放大极高，不做；
 *   - hybrid/log-structured 复杂且收益主要在元数据量，不引入。
 *
 * 反向映射 P2L 不单独建表：每个 page 的元数据里已经存了 lba + seq，
 * GC 搬移和断电重建时直接扫 block 内页的元数据就能反推，
 * 这与真实固件"元数据写在 spare 区"的做法一致。
 *
 * S3 新增三块：
 *   1. 回收策略：greedy / cost-benefit，以及后台 GC 水位
 *   2. 磨损均衡：分配时挑低 PE 块（动态），PE 差过大时迁移冷数据（静态）
 *   3. 坏块管理：program/erase 失败 -> 隔离坏块 + 抢救有效页 + 重映射
 */

typedef enum ftl_gc_policy {
    FTL_GC_GREEDY       = 0,   /* 只挑有效页最少的块 */
    FTL_GC_COST_BENEFIT = 1    /* (可回收页数 × 块年龄) / 搬移代价 */
} ftl_gc_policy_t;

typedef struct ftl_pe_stat {
    uint32_t min_pe;
    uint32_t max_pe;
    double   avg_pe;
    double   var_pe;       /* 方差 */
    double   stddev_pe;    /* 标准差：衡量磨损是否均衡 */
    uint32_t samples;
} ftl_pe_stat_t;

typedef struct ftl_dev {
    nand_dev_t         *nand;
    const ssd_config_t *cfg;

    /* 正向映射 L2P（页级，DRAM 驻留） */
    ppn_t *l2p;
    lba_t  user_lbas;

    /* 块级"最后变脏时刻"，单位是 host 写次数（不是 ns）。
     * cost-benefit 要用块年龄，而年龄必须和"页数"同量纲才能和搬移代价相比；
     * 直接用纳秒会让 age 项大出好几个数量级，把策略退化成"专挑最冷的块"。
     * 真实固件里同样维护块表（block table），这部分内存开销是必要的。 */
    uint32_t *blk_invalid_seq;
    uint64_t  write_seq;

    /* 空闲块池：所有可用块都在 free_stack 里。
     *
     * rsv_min 是前台 GC 的触发线：host 写不允许把池子取到 rsv_min 以下，
     * 这些块是留给 GC 的"工作空间"。GC 必须先有地方写，才能擦除 victim，
     * 否则就是死锁 —— OP 的物理意义就在这里。
     *
     * bg_target 是后台 GC 的目标水位：空闲块低于它时，在 host 间隙提前回收，
     * 避免 host 撞上前台 GC 产生延迟尖峰（S4 会把它移到真正的 idle 时段）。 */
    pbn_t    *free_stack;
    uint32_t  free_top;
    uint32_t  free_cap;
    uint32_t  rsv_min;
    uint32_t  bg_target;

    /* 当前写入块及其写指针 */
    pbn_t    open_block;
    uint32_t open_next_page;

    /* S3 策略参数 */
    uint32_t gc_policy;
    uint32_t wl_enable;
    uint32_t wl_pe_thresh;

    uint64_t gc_count;
    uint64_t gc_copied;
    uint64_t bg_gc_count;

    uint64_t wl_migrations;
    uint64_t wl_migrated_pages;

    /* 坏块：出厂坏块在介质层，运行期新增的在这里统计 */
    uint32_t bad_blocks;        /* 运行期隔离的坏块数 */
    uint32_t bb_saved_pages;    /* 隔离时抢救出来的有效页数 */

    uint64_t trim_cmds;
    uint64_t trim_pages;

    bool     valid;
} ftl_dev_t;

/* ---------------- 生命周期 ---------------- */

int  ftl_init(ftl_dev_t *f, nand_dev_t *nand, const ssd_config_t *cfg);
void ftl_deinit(ftl_dev_t *f);

/* ---------------- 对外 IO ---------------- */

int ftl_write(ftl_dev_t *f, lba_t lba, uint32_t seq);
int ftl_read (ftl_dev_t *f, lba_t lba, nand_page_meta_t *out);

/* TRIM（S3）：告诉 FTL 这段逻辑地址不再需要，回收时不搬这些页 */
int ftl_trim (ftl_dev_t *f, lba_t lba, uint32_t nr_pages);

/* S2/S3 没有写缓存，flush 是空操作；语义在 S5（SPOR）才真正有意义 */
int ftl_flush(ftl_dev_t *f);

/* ---------------- 内部：块池与页分配（GC / WL 也用） ---------------- */

/* host 写分配：受 rsv_min 水位保护，取不到就返回 INVALID（调用方去触发 GC）。
 * 开启 WL 时，会在栈顶若干候选里挑 P/E 次数最小的块（动态磨损均衡）。 */
pbn_t ftl_take_free_block(ftl_dev_t *f);
/* GC destination：可以取到池底，保证 GC 永远有地方可写 */
pbn_t ftl_take_gc_block(ftl_dev_t *f);
void  ftl_return_block(ftl_dev_t *f, pbn_t pbn);
int   ftl_alloc_ppn(ftl_dev_t *f, ppn_t *out);

/* 当前 open block 的剩余空间是否装得下 need_pages 页。
 *
 * 为什么必须先问这一句：如果复用 open block 装到一半才发现地方不够，
 * 就得中途再取一个块；万一那时池子空了，victim 擦不掉、新块也没拿到，
 * 就凭空少一个可用块 —— 坏块一多，盘会被这样活活耗死。 */
bool ftl_can_reuse_open(const ftl_dev_t *f, uint32_t need_pages);

/* 内部共享：搬移与 destination 收尾由 gc.c 实现，磨损均衡复用同一套逻辑 */
int  ftl_move_valid_pages(ftl_dev_t *f, pbn_t src, pbn_t *dest,
                          uint32_t *dest_wp, bool *reused_open, uint32_t *copied);
void ftl_release_dest(ftl_dev_t *f, pbn_t dest, uint32_t dest_wp, bool reused_open);

/* ---------------- 空间回收 ---------------- */

/* 前台 GC：回收一个块（host 写撞到水位线时被迫调用） */
int   ftl_gc_one(ftl_dev_t *f);
/* 后台 GC：把空闲块补到 bg_target，提前回收换取更平稳的延迟 */
int   ftl_bg_gc(ftl_dev_t *f);

/* ---------------- 磨损均衡 ---------------- */

/* 静态 WL：把低 PE 块里的冷数据搬到高 PE 块，让低 PE 块回到写入轮换中 */
int   ftl_wl_static_once(ftl_dev_t *f);
void  ftl_pe_stats(const ftl_dev_t *f, ftl_pe_stat_t *st);

/* ---------------- 坏块管理 ---------------- */

/* 隔离坏块：抢救块内有效页 -> 标记 BAD -> 永不归还池 */
int   ftl_isolate_bad_block(ftl_dev_t *f, pbn_t pbn);

/* ---------------- 查询 ---------------- */

lba_t    ftl_user_lbas(const ftl_dev_t *f);
uint32_t ftl_free_blocks(const ftl_dev_t *f);
uint32_t ftl_bad_blocks(const ftl_dev_t *f);

#endif /* SSD_FTL_FTL_H */
