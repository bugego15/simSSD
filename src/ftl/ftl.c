#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

int ftl_init(ftl_dev_t *f, nand_dev_t *nand, const ssd_config_t *cfg)
{
    lba_t i;
    pbn_t pbn;

    if (f == NULL || nand == NULL || cfg == NULL) {
        return SSD_ERR_INVAL;
    }
    memset(f, 0, sizeof(*f));

    f->nand = nand;
    f->cfg  = cfg;

    /* 用户可见逻辑页数：由 OP 决定（derive 时已算好） */
    f->user_lbas = cfg->user_pages;
    if (f->user_lbas == 0u || f->user_lbas > nand->geo.total_pages) {
        SSD_ERR("invalid user_pages=%llu (total=%llu)",
                (unsigned long long)f->user_lbas,
                (unsigned long long)nand->geo.total_pages);
        return SSD_ERR_INVAL;
    }

    f->l2p = (ppn_t *)calloc((size_t)f->user_lbas, sizeof(ppn_t));
    if (f->l2p == NULL) {
        return SSD_ERR_NO_MEM;
    }
    for (i = 0; i < f->user_lbas; i++) {
        f->l2p[i] = SSD_INVALID_PPN;
    }

    /* 块池：所有非坏块入池 */
    f->free_cap = (uint32_t)nand->geo.total_blocks;
    f->rsv_min  = 2u;   /* GC 工作空间：至少保底 2 块，S3 会按 OP 调整 */
    if (f->rsv_min >= f->free_cap) {
        f->rsv_min = (f->free_cap > 1u) ? (f->free_cap - 1u) : 0u;
    }
    f->free_stack = (pbn_t *)calloc(f->free_cap, sizeof(pbn_t));
    if (f->free_stack == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    for (pbn = 0; pbn < nand->geo.total_blocks; pbn++) {
        if (nand_block_state(nand, pbn) == (uint16_t)NAND_BLK_BAD) {
            continue;   /* 出厂坏块不进池 */
        }
        f->free_stack[f->free_top++] = pbn;
    }

    f->open_block     = SSD_INVALID_PBN;
    f->open_next_page = 0u;
    f->valid          = true;

    SSD_INFO("ftl initialized: user_lbas=%llu, free=%u, rsv_min=%u",
             (unsigned long long)f->user_lbas, f->free_top, f->rsv_min);
    return SSD_OK;
}

void ftl_deinit(ftl_dev_t *f)
{
    if (f == NULL) {
        return;
    }
    free(f->l2p);
    free(f->free_stack);
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* 块池                                                                */
/* ------------------------------------------------------------------ */

pbn_t ftl_take_free_block(ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);

    /* 水位保护：低于 rsv_min 就不再给 host 用，剩下的留给 GC */
    if (f->free_top > f->rsv_min) {
        return f->free_stack[--f->free_top];
    }
    return SSD_INVALID_PBN;
}

pbn_t ftl_take_gc_block(ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);

    if (f->free_top > 0u) {
        return f->free_stack[--f->free_top];
    }
    return SSD_INVALID_PBN;
}

void ftl_return_block(ftl_dev_t *f, pbn_t pbn)
{
    SSD_ASSERT(f != NULL);
    SSD_ASSERT(pbn < f->nand->geo.total_blocks);

    if (f->free_top >= f->free_cap) {
        SSD_BUG("block pool overflow");
        return;
    }
    f->free_stack[f->free_top++] = pbn;
}

/* ------------------------------------------------------------------ */
/* 页分配                                                              */
/* ------------------------------------------------------------------ */

int ftl_alloc_ppn(ftl_dev_t *f, ppn_t *out)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    uint32_t guard = 0u;

    for (;;) {
        /* 当前 open block 还有空间，直接顺序分配（NAND 要求块内顺序写） */
        if (f->open_block != SSD_INVALID_PBN && f->open_next_page < ppb) {
            *out = nand_geo_block_base(&nd->geo, f->open_block) + f->open_next_page;
            f->open_next_page++;
            nand_set_block_next_page(nd, f->open_block, f->open_next_page);
            return SSD_OK;
        }

        /* 当前块写满：关闭它 */
        if (f->open_block != SSD_INVALID_PBN) {
            nand_set_block_state(nd, f->open_block, (uint16_t)NAND_BLK_CLOSED);
            f->open_block     = SSD_INVALID_PBN;
            f->open_next_page = 0u;
        }

        {
            pbn_t nb = ftl_take_free_block(f);
            if (nb != SSD_INVALID_PBN) {
                f->open_block     = nb;
                f->open_next_page = 0u;
                nand_set_block_state(nd, nb, (uint16_t)NAND_BLK_OPEN);
                continue;
            }
        }

        /* 空闲块触及水位线：触发 GC。
         * 注意 GC 内部可能会把一个 destination 块设成 open block，
         * 所以回到循环开头重新判断，而不是在这里直接取块覆盖它。 */
        if (ftl_gc_one(f) != SSD_OK) {
            return SSD_ERR_NO_SPACE;
        }
        if (++guard > (nd->geo.total_blocks + 4u)) {
            /* GC 转了一圈还是腾不出空间，说明盘真的写不下了 */
            SSD_ERR("alloc_ppn: out of space after %u gc rounds", guard);
            return SSD_ERR_NO_SPACE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 读写路径                                                            */
/* ------------------------------------------------------------------ */

int ftl_write(ftl_dev_t *f, lba_t lba, uint32_t seq)
{
    nand_page_meta_t m;
    ppn_t old;
    ppn_t ppn;
    int rc;

    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }

    /* 覆盖写：旧物理页失效（不能原地改写，这是 NAND 的物理约束） */
    old = f->l2p[lba];
    if (old != SSD_INVALID_PPN) {
        nand_invalidate_page(f->nand, old);
    }

    rc = ftl_alloc_ppn(f, &ppn);
    if (rc != SSD_OK) {
        return rc;
    }

    m.lba   = lba;
    m.seq   = seq;
    m.state = (uint16_t)NAND_PAGE_VALID;
    m.crc   = 0u;
    nand_meta_seal(&m);

    rc = nand_prog_sync(f->nand, ppn, &m);
    if (rc != SSD_OK) {
        /* 编程失败（故障注入或坏块）：该页已被介质层标为 INVALID，换新页重试一次 */
        SSD_WARN("program failed on ppn=%llu, retry", (unsigned long long)ppn);
        rc = ftl_alloc_ppn(f, &ppn);
        if (rc != SSD_OK) {
            return rc;
        }
        rc = nand_prog_sync(f->nand, ppn, &m);
        if (rc != SSD_OK) {
            f->l2p[lba] = SSD_INVALID_PPN;
            return rc;
        }
    }

    f->l2p[lba] = ppn;
    ssd_stats_inc(ST_HOST_WRITE_PAGES);
    return SSD_OK;
}

int ftl_read(ftl_dev_t *f, lba_t lba, nand_page_meta_t *out)
{
    ppn_t ppn;
    int rc;

    if (f == NULL || out == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }

    ppn = f->l2p[lba];
    if (ppn == SSD_INVALID_PPN) {
        out->lba   = SSD_INVALID_LBA;
        out->seq   = 0u;
        out->state = (uint16_t)NAND_PAGE_FREE;
        out->crc   = 0u;
        return SSD_ERR_NO_SPACE;   /* 从未写过 */
    }

    rc = nand_read_sync(f->nand, ppn, out);
    if (rc != SSD_OK) {
        return rc;
    }
    /* 映射错乱检测：物理页里存的 lba 必须就是要读的 lba */
    if (out->lba != lba) {
        SSD_ERR("mapping corruption: lba=%llu -> ppn=%llu holds lba=%llu",
                (unsigned long long)lba, (unsigned long long)ppn,
                (unsigned long long)out->lba);
        return SSD_ERR_IO;
    }

    ssd_stats_inc(ST_HOST_READ_PAGES);
    return SSD_OK;
}

int ftl_flush(ftl_dev_t *f)
{
    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }
    /* S2 所有写都是同步落盘的，没有缓存需要下刷 */
    return SSD_OK;
}

lba_t ftl_user_lbas(const ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);
    return f->user_lbas;
}

uint32_t ftl_free_blocks(const ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);
    return f->free_top;
}
