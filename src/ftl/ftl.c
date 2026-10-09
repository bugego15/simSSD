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

    f->blk_invalid_seq = (uint32_t *)calloc((size_t)nand->geo.total_blocks,
                                            sizeof(uint32_t));
    if (f->blk_invalid_seq == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    /* S3：回收与磨损策略 */
    f->gc_policy    = cfg->gc_policy;
    f->wl_enable    = cfg->wl_enable;
    f->wl_pe_thresh = cfg->wl_pe_thresh;

    /* 块池：所有非坏块入池 */
    f->free_cap = (uint32_t)nand->geo.total_blocks;
    /* 前台 GC 触发线（= GC 的工作空间）。
     *
     * 为什么不能写死成 2 块：每个运行时坏块会吃掉两个块的额度 ——
     * 坏块本身退出服务，还得再拿一块出来顶替它当前的写入位置。
     * 工作空间只有 2~3 块时，出几个坏块盘就再也写不动了（实测 free 掉到 0），
     * 而此时盘上其实还有大量垃圾可回收。按容量取 1% 更稳妥。 */
    f->rsv_min = (uint32_t)(nand->geo.total_blocks / 100u);
    if (f->rsv_min < 4u) {
        f->rsv_min = 4u;
    }
    if (f->rsv_min >= f->free_cap) {
        f->rsv_min = (f->free_cap > 1u) ? (f->free_cap - 1u) : 0u;
    }

    /* 后台 GC 目标水位：空闲块低于此值时提前回收（0 表示关闭后台 GC） */
    f->bg_target = (uint32_t)((nand->geo.total_blocks * cfg->bg_gc_percent) / 100u);
    if (f->bg_target > f->free_cap) {
        f->bg_target = f->free_cap;
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
    free(f->blk_invalid_seq);
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* 块池                                                                */
/* ------------------------------------------------------------------ */

/*
 * 动态磨损均衡（S3）：
 * 空闲池是一个栈，如果每次都从栈顶取，栈顶那几个块会被反复擦写。
 * 这里改为"在栈顶若干候选里挑 P/E 次数最小的那个"——
 * 只比较有限个候选，开销 O(1)，却能让写入机会在所有块之间摊开。
 */
#define WL_CANDIDATES 8u

static pbn_t take_low_pe_from_top(ftl_dev_t *f)
{
    uint32_t k = WL_CANDIDATES;
    uint32_t best = 0u;
    uint32_t t;
    pbn_t pbn;

    if (k > f->free_top) {
        k = f->free_top;
    }

    /* 在池中随机采 K 个位置（由 write_seq 驱动，保证可复现），挑 P/E 最小的。
     * 只比较栈顶几个是不够的：栈底的块长期取不到，永远是"新块"，
     * 实测 PE stddev 会从 0.56 恶化到 0.74。随机采样让每个块都有出场机会。 */
    for (t = 0; t < k; t++) {
        uint64_t mix = (uint64_t)f->write_seq * 2654435761u + (uint64_t)(t * 40503u);
        uint32_t idx = (uint32_t)(mix % (uint64_t)f->free_top);

        if (nand_pe_count(f->nand, f->free_stack[idx]) <
            nand_pe_count(f->nand, f->free_stack[best])) {
            best = idx;
        }
    }

    pbn = f->free_stack[best];
    f->free_stack[best] = f->free_stack[f->free_top - 1u];
    f->free_top--;
    return pbn;
}

pbn_t ftl_take_free_block(ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);

    /* 水位保护：低于 rsv_min 就不再给 host 用，剩下的留给 GC */
    if (f->free_top <= f->rsv_min) {
        return SSD_INVALID_PBN;
    }
    if (f->wl_enable != 0u) {
        return take_low_pe_from_top(f);
    }
    return f->free_stack[--f->free_top];
}

pbn_t ftl_take_gc_block(ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);

    if (f->free_top == 0u) {
        return SSD_INVALID_PBN;
    }
    if (f->wl_enable != 0u) {
        return take_low_pe_from_top(f);
    }
    return f->free_stack[--f->free_top];
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
        if (ftl_gc_one(f) == SSD_OK && ++guard <= (nd->geo.total_blocks + 4u)) {
            continue;   /* GC 成功：回到循环开头看 destination 是否变成了 open block */
        }

        /* GC 转不出可用块了：动用最后的家底。
         *
         * 为什么不能直接返回 ENOSPC：坏块隔离会永久吃掉块，池子可能长期停在
         * rsv_min 上，而 GC 是"借一块、还一块"、净变化为 0。若坚持"池必须多于
         * rsv_min 才能分配"，盘上明明还有空间也会提前死锁。
         * 真实盘的语义是：只有真的一块都没有了才拒绝写入。 */
        {
            pbn_t last;

            /* 至少给 GC 留一块：GC 必须先有 destination 才能擦 victim，
             * 把池子取到 0 等于把 GC 也一起饿死，盘就真的僵住了。
             * （free 状态数与池内块数一致 —— 这时确实是"没有空闲块"，不是泄漏） */
            if (f->free_top <= 1u) {
                SSD_ERR("alloc_ppn: out of space (free=%u, bad=%u)",
                        ftl_free_blocks(f), ftl_bad_blocks(f));
                return SSD_ERR_NO_SPACE;
            }
            last = ftl_take_gc_block(f);
            if (last == SSD_INVALID_PBN) {
                return SSD_ERR_NO_SPACE;
            }
            f->open_block     = last;
            f->open_next_page = 0u;
            nand_set_block_state(nd, last, (uint16_t)NAND_BLK_OPEN);
            guard = 0u;
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 读写路径                                                            */
/* ------------------------------------------------------------------ */

/* 页失效：除了介质层的状态，还要记下"这块最后一次变脏"的时刻，
 * cost-benefit 靠它区分冷热。 */
bool ftl_can_reuse_open(const ftl_dev_t *f, uint32_t need_pages)
{
    uint32_t ppb;

    if (f == NULL || !f->valid) {
        return false;
    }
    ppb = f->nand->geo.pages_per_block;
    if (f->open_block == SSD_INVALID_PBN || f->open_next_page >= ppb) {
        return false;
    }
    return (ppb - f->open_next_page) >= need_pages;
}

static void ftl_mark_invalid(ftl_dev_t *f, ppn_t ppn)
{
    pbn_t pbn;

    nand_invalidate_page(f->nand, ppn);
    pbn = nand_geo_pbn_of_ppn(&f->nand->geo, ppn);
    f->blk_invalid_seq[pbn] = (uint32_t)f->write_seq;
}

int ftl_write(ftl_dev_t *f, lba_t lba, uint32_t seq)
{
    nand_page_meta_t m;
    ppn_t old;
    ppn_t ppn;
    uint32_t retry;
    int rc = SSD_ERR_IO;

    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }

    m.lba   = lba;
    m.seq   = seq;
    m.state = (uint16_t)NAND_PAGE_VALID;
    m.crc   = 0u;
    nand_meta_seal(&m);

    /* 编程失败属于"可恢复故障"：换块重写，并把坏块隔离掉。
     * 注意顺序 —— 必须等新页写成功之后再让旧页失效。
     * 反过来做会出现一个数据丢失窗口：新页没写进去，旧页却已经失效了。 */
    for (retry = 0; retry < 3u; retry++) {
        rc = ftl_alloc_ppn(f, &ppn);
        if (rc != SSD_OK) {
            return rc;
        }
        rc = nand_prog_sync(f->nand, ppn, &m);
        if (rc == SSD_OK) {
            break;
        }
        /* 单次失败先换页重试：偶发编程失败很常见，
         * 连续失败才说明这块真的不行了，那时再隔离它。 */
        if (retry >= 1u) {
            SSD_WARN("program failed twice on ppn=%llu, isolate block",
                     (unsigned long long)ppn);
            (void)ftl_isolate_bad_block(f,
                     nand_geo_pbn_of_ppn(&f->nand->geo, ppn));
        }
    }
    if (rc != SSD_OK) {
        return rc;   /* 旧映射未动，旧数据仍然可读 */
    }

    old = f->l2p[lba];
    if (old != SSD_INVALID_PPN) {
        ftl_mark_invalid(f, old);
    }
    f->l2p[lba] = ppn;
    f->write_seq++;
    ssd_stats_inc(ST_HOST_WRITE_PAGES);

    /* 后台 GC：借 host 命令之间的空隙提前回收，
     * 免得 host 写撞到水位线被迫同步等 GC（S4 会把它移到真正的 idle 时段） */
    if (f->bg_target > 0u && ftl_free_blocks(f) < f->bg_target) {
        (void)ftl_bg_gc(f);
    }
    return SSD_OK;
}

/* TRIM（S3）：这段逻辑地址 host 不再需要了。
 * 关键价值：mapping 直接断开 + 物理页失效，GC 再遇到这些页就不用搬，
 * 写放大因此下降。没有 TRIM 的盘，GC 只能傻乎乎地搬一堆"host 早就不想要"的数据。 */
int ftl_trim(ftl_dev_t *f, lba_t lba, uint32_t nr_pages)
{
    uint32_t i;
    uint32_t trimmed = 0u;

    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }
    if (nr_pages == 0u) {
        return SSD_ERR_INVAL;
    }
    if (lba + nr_pages > f->user_lbas) {
        nr_pages = (uint32_t)(f->user_lbas - lba);   /* 越界部分截断 */
    }

    for (i = 0; i < nr_pages; i++) {
        ppn_t ppn = f->l2p[lba + i];

        if (ppn == SSD_INVALID_PPN) {
            continue;   /* 本来就没数据 */
        }
        ftl_mark_invalid(f, ppn);
        f->l2p[lba + i] = SSD_INVALID_PPN;
        trimmed++;
    }

    f->write_seq++;
    f->trim_cmds++;
    f->trim_pages += trimmed;
    ssd_stats_inc(ST_HOST_TRIM_CMDS);
    return SSD_OK;
}

/* 坏块管理（S3）：
 *   1. 抢救：把块里还没失效的有效页搬到一个好块上（搬不动就放弃，并如实记录）
 *   2. 隔离：标记 BAD，永不归还空闲池
 *   3. 记账：坏块数 +1，容量靠 OP 的保留区吸收
 * 运行时坏块只会减少可用物理块，不能改变 user_lbas —— 这正是 OP 的用途之一。 */
int ftl_isolate_bad_block(ftl_dev_t *f, pbn_t pbn)
{
    nand_dev_t *nd = f->nand;
    pbn_t dest;
    uint32_t dest_wp;
    bool reused_open;
    uint32_t saved = 0u;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }
    if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
        return SSD_OK;   /* 已经隔离过了，幂等 */
    }

    /* 先把坏块从 open block 位置上摘下来，免得抢救时又往坏块里写 */
    if (f->open_block == pbn) {
        f->open_block     = SSD_INVALID_PBN;
        f->open_next_page = 0u;
    }

    /* 抢救目的地优先复用当前 open block 的剩余空间。
     * 这一点很关键：如果每次隔离坏块都新开一整块，坏块一多，
     * 大量"只装了几页"的块会把盘上空间活活耗光（实测 free 会掉到 0）。 */
    if (ftl_can_reuse_open(f, nand_block_valid_pages(nd, pbn))) {
        dest        = f->open_block;
        dest_wp     = f->open_next_page;
        reused_open = true;
    } else {
        dest = ftl_take_gc_block(f);
        if (dest == SSD_INVALID_PBN) {
            /* 一块都拿不出来：只能隔离，数据救不回来了 */
            nand_set_block_state(nd, pbn, (uint16_t)NAND_BLK_BAD);
            f->bad_blocks++;
            ssd_stats_inc(ST_BADBLOCK_RUNTIME);
            return SSD_OK;
        }
        dest_wp     = 0u;
        reused_open = false;
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);
    }

    /* 复用 GC 的搬移逻辑：它内部自带"写失败换页重试、写满换块"，
     * 且不会反过来调用 isolate，不存在递归风险。 */
    (void)ftl_move_valid_pages(f, pbn, &dest, &dest_wp, &reused_open, &saved);
    ftl_release_dest(f, dest, dest_wp, reused_open);

    nand_set_block_state(nd, pbn, (uint16_t)NAND_BLK_BAD);
    f->bad_blocks++;
    f->bb_saved_pages += saved;
    ssd_stats_inc(ST_BADBLOCK_RUNTIME);

    SSD_WARN("bad block isolated: pbn=%llu saved_pages=%u",
             (unsigned long long)pbn, saved);
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

uint32_t ftl_bad_blocks(const ftl_dev_t *f)
{
    SSD_ASSERT(f != NULL);
    /* 出厂坏块 + 运行期新增：总坏块数决定了还剩多少物理容量可用 */
    return f->nand->factory_bad_blocks + f->bad_blocks;
}
