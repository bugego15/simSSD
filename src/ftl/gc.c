#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

/*
 * Greedy GC（S2 最小实现）
 *
 * 流程：
 *   1. 选 victim：有效页最少的 CLOSED 块
 *   2. 选 destination：优先复用当前 open block 的剩余空间，否则取一个新块
 *   3. 搬移 victim 中所有有效页（读 -> 写到 destination -> 更新 L2P）
 *   4. 擦除 victim 并归还块池
 *
 * 为什么 destination 要"优先复用 open block"：
 *   如果每次 GC 都新开一个块，而 victim 的有效页又很少，
 *   就会产生大量"半满块"，白白吃掉 OP。复用写指针是最朴素也最有效的做法。
 *
 * 为什么必须有预留池（rsv_stack）：
 *   victim 只有在搬完数据后才能擦除。若此时一个空闲块都没有，
 *   GC 就死锁了。预留池保证任何时刻 GC 都有地方可写。
 */

static pbn_t gc_pick_victim(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    pbn_t victim = SSD_INVALID_PBN;
    uint32_t min_valid = 0xFFFFFFFFu;
    pbn_t pbn;

    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint16_t st = nand_block_state(nd, pbn);
        uint32_t v;

        if (st != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
        v = nand_block_valid_pages(nd, pbn);
        if (v < min_valid) {
            min_valid = v;
            victim    = pbn;
        }
    }

    if (victim == SSD_INVALID_PBN) {
        /* 没有 closed 块：把当前 open block 强制关闭作为 victim */
        if (f->open_block != SSD_INVALID_PBN) {
            victim = f->open_block;
            nand_set_block_state(nd, victim, (uint16_t)NAND_BLK_CLOSED);
            f->open_block     = SSD_INVALID_PBN;
            f->open_next_page = 0u;
        }
    }
    return victim;
}

/* 释放 destination：把它变成 open block，或者关闭它 */
static void gc_release_dest(ftl_dev_t *f, pbn_t dest, uint32_t dest_wp, bool reused_open)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;

    nand_set_block_next_page(nd, dest, dest_wp);

    if (reused_open) {
        f->open_next_page = dest_wp;
        return;
    }

    if (f->open_block == SSD_INVALID_PBN && dest_wp < ppb) {
        /* 继续当作写入块使用，避免半满块浪费 */
        f->open_block     = dest;
        f->open_next_page = dest_wp;
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);
    } else {
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_CLOSED);
    }
}

int ftl_gc_one(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    pbn_t victim;
    pbn_t dest;
    uint32_t dest_wp;
    bool reused_open;
    ppn_t base;
    uint32_t i;
    uint32_t copied = 0u;
    int rc;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }

    victim = gc_pick_victim(f);
    if (victim == SSD_INVALID_PBN) {
        return SSD_ERR_NO_SPACE;   /* 没有任何可回收的块 */
    }

    /* destination：优先复用当前 open block 的剩余空间 */
    if (f->open_block != SSD_INVALID_PBN &&
        f->open_next_page < ppb) {
        dest        = f->open_block;
        dest_wp     = f->open_next_page;
        reused_open = true;
    } else {
        dest = ftl_take_gc_block(f);
        if (dest == SSD_INVALID_PBN) {
            return SSD_ERR_NO_SPACE;
        }
        dest_wp     = 0u;
        reused_open = false;
    }

    base = nand_geo_block_base(&nd->geo, victim);

    for (i = 0; i < ppb; i++) {
        ppn_t src = base + i;
        ppn_t dst;
        nand_page_meta_t m;

        if (nand_page_state(nd, src) != (uint16_t)NAND_PAGE_VALID) {
            continue;   /* 只有有效页才需要搬，失效页随擦除一起消失 */
        }

        rc = nand_read_sync(nd, src, &m);
        if (rc != SSD_OK) {
            SSD_ERR("gc: read failed on ppn=%llu", (unsigned long long)src);
            gc_release_dest(f, dest, dest_wp, reused_open);
            return rc;
        }
        if (m.lba == SSD_INVALID_LBA || m.lba >= f->user_lbas) {
            continue;   /* 元数据异常，跳过 */
        }

        /* destination 写满，换新块 */
        if (dest_wp >= ppb) {
            gc_release_dest(f, dest, dest_wp, reused_open);
            dest = ftl_take_gc_block(f);
            if (dest == SSD_INVALID_PBN) {
                return SSD_ERR_NO_SPACE;
            }
            dest_wp     = 0u;
            reused_open = false;
            nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);
        }

        dst = nand_geo_block_base(&nd->geo, dest) + dest_wp;
        rc = nand_prog_sync(nd, dst, &m);
        if (rc != SSD_OK) {
            SSD_ERR("gc: program failed on ppn=%llu", (unsigned long long)dst);
            dest_wp++;   /* 坏的页作废，继续用后面的页 */
            nand_set_block_next_page(nd, dest, dest_wp);
            continue;
        }

        dest_wp++;
        nand_set_block_next_page(nd, dest, dest_wp);

        /* 关键：搬移后必须更新 L2P，否则 host 会读到旧位置 */
        f->l2p[m.lba] = dst;
        copied++;
        ssd_stats_inc(ST_GC_COPY_PAGES);
    }

    gc_release_dest(f, dest, dest_wp, reused_open);

    /* 擦除 victim 并归还 */
    rc = nand_erase_sync(nd, victim);
    ssd_stats_inc(ST_GC_ERASES);
    if (rc == SSD_OK) {
        ftl_return_block(f, victim);
    } else {
        /* 擦除失败 -> 运行时坏块，不再归还池子 */
        SSD_WARN("gc: erase failed on pbn=%llu, dropped", (unsigned long long)victim);
        nand_set_block_state(nd, victim, (uint16_t)NAND_BLK_BAD);
    }

    f->gc_count++;
    f->gc_copied += copied;
    return SSD_OK;
}
