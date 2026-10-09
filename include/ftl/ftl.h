#ifndef SSD_FTL_FTL_H
#define SSD_FTL_FTL_H

#include "core/config.h"
#include "media/nand.h"
#include "ssd_types.h"

/*
 * FTL：页级映射 + 写路径 + 读路径（S2）
 *
 * 映射方案选型：page-level L2P，DRAM 驻留数组 [lba] -> ppn。
 *   - block-level 映射随机写放大极高，不做；
 *   - hybrid/log-structured 复杂且收益主要在元数据量，S2 不引入。
 *
 * 反向映射 P2L 不单独建表：每个 page 的元数据里已经存了 lba + seq，
 * GC 搬移和断电重建时直接扫 block 内页的元数据就能反推，
 * 这与真实固件"元数据写在 spare 区"的做法一致。
 *
 * 空间回收：S2 只做"按需触发的 greedy GC"，保证能持续写入；
 * 策略对比（cost-benefit）、前后台 GC、WL、坏块替换在 S3 展开。
 */

typedef struct ftl_dev {
    nand_dev_t         *nand;
    const ssd_config_t *cfg;

    /* 正向映射 L2P（页级，DRAM 驻留） */
    ppn_t *l2p;
    lba_t  user_lbas;

    /* 空闲块池：所有可用块都在 free_stack 里。
     *
     * rsv_min 是水位保护线：host 写分配时不允许把池子取到 rsv_min 以下，
     * 这些块是留给 GC 的"工作空间"。
     * 原因：GC 必须先有地方写，才能擦除 victim —— 如果盘上没有空闲块，
     * 就会出现 victim 擦不掉、新数据没处写的死锁。OP 的物理意义就在这里。 */
    pbn_t    *free_stack;
    uint32_t  free_top;
    uint32_t  free_cap;
    uint32_t  rsv_min;

    /* 当前写入块及其写指针 */
    pbn_t    open_block;
    uint32_t open_next_page;

    uint64_t gc_count;
    uint64_t gc_copied;

    bool     valid;
} ftl_dev_t;

/* ---------------- 生命周期 ---------------- */

int  ftl_init(ftl_dev_t *f, nand_dev_t *nand, const ssd_config_t *cfg);
void ftl_deinit(ftl_dev_t *f);

/* ---------------- 对外 IO ---------------- */

int ftl_write(ftl_dev_t *f, lba_t lba, uint32_t seq);
int ftl_read (ftl_dev_t *f, lba_t lba, nand_page_meta_t *out);

/* S2 没有写缓存，flush 是空操作；语义在 S5（SPOR）才真正有意义 */
int ftl_flush(ftl_dev_t *f);

/* ---------------- 内部：块池与页分配（GC 也用） ---------------- */

/* host 写分配：受 rsv_min 水位保护，取不到就返回 INVALID（调用方去触发 GC） */
pbn_t ftl_take_free_block(ftl_dev_t *f);
/* GC destination：可以取到池底，保证 GC 永远有地方可写 */
pbn_t ftl_take_gc_block(ftl_dev_t *f);
void  ftl_return_block(ftl_dev_t *f, pbn_t pbn);
int   ftl_alloc_ppn(ftl_dev_t *f, ppn_t *out);

/* greedy GC：回收一个块，返回 SSD_OK 表示确实回收了一个块 */
int   ftl_gc_one(ftl_dev_t *f);

lba_t    ftl_user_lbas(const ftl_dev_t *f);
uint32_t ftl_free_blocks(const ftl_dev_t *f);

#endif /* SSD_FTL_FTL_H */
