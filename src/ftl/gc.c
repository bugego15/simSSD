#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

/*
 * 空间回收（S3）
 *
 * 两种 victim 选择策略：
 *
 *   1) greedy：挑有效页最少的块。
 *      代价最小，但它不区分冷热 —— 假如一个块里装的是冷数据，
 *      有效页再多也不会再变成垃圾，回收它纯属白搬；
 *      而装热数据的块虽然当前有效页多，却会很快烂掉。
 *      均匀随机负载下两者无差别，有冷热之分时 greedy 会明显吃亏。
 *
 *   2) cost-benefit：score = 可回收页数 × 块年龄 / 搬移代价
 *      （Rosenblum & Ousterhout 的经典公式，这里用离散形式）
 *      分子奖励"能腾出更多空间、且已经很久没产生新垃圾"的块，
 *      分母惩罚"要搬的页多"的块。冷热混合负载下 WAF 明显低于 greedy。
 *
 * destination 优先复用当前 open block 的剩余空间：
 *   如果每次 GC 都新开块，而 victim 有效页又很少，就会产生大量半满块，
 *   白白吃掉 OP。复用写指针是最朴素也最有效的做法。
 */

/* 释放 destination：能继续写就变成 open block，否则关闭等下次回收 */
void ftl_release_dest(ftl_dev_t *f, pbn_t dest, uint32_t dest_wp, bool reused_open)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;

    nand_set_block_next_page(nd, dest, dest_wp);

    if (reused_open) {
        f->open_next_page = dest_wp;
        return;
    }

    if (f->open_block == SSD_INVALID_PBN && dest_wp < ppb) {
        f->open_block     = dest;
        f->open_next_page = dest_wp;
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);
    } else {
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_CLOSED);
    }
}

static pbn_t gc_pick_victim_greedy(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    pbn_t victim = SSD_INVALID_PBN;
    uint32_t min_valid = 0xFFFFFFFFu;
    pbn_t pbn;

    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint32_t v;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
        v = nand_block_valid_pages(nd, pbn);
        if (v < min_valid) {
            min_valid = v;
            victim    = pbn;
        }
    }
    return victim;
}

static pbn_t gc_pick_victim_cb(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    pbn_t victim = SSD_INVALID_PBN;
    double best = -1.0;
    pbn_t pbn;

    /* 写满一遍用户容量所需的 host 写次数：作为年龄的尺度基准 */
    double w = (double)((f->user_lbas > 0u) ? f->user_lbas : 1u);

    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint64_t age;
        uint32_t valid;
        double age_norm;
        double score;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }

        valid = nand_block_valid_pages(nd, pbn);

        /* 块年龄 = 距最后一次"页失效"过去了多少次 host 写。
         * 越久没产生新垃圾，剩下的越可能是冷数据。 */
        age = f->write_seq - (uint64_t)f->blk_invalid_seq[pbn];

        /* 为什么 age 要做饱和归一化，而不是直接乘上去：
         * 教科书里的 (1-u)*age/(2u) 里 age 是无界时间戳，放到这里会累积到
         * 几千，比页数大两三个数量级，于是公式被年龄项完全主导 ——
         * 实测下来它会去回收"几乎全满的冷块"，搬 15 页只换回 1 页，
         * WAF 反而比 greedy 更差（1595 vs 1435）。
         *
         * 这里改成：主判据仍然是"能腾多少页 / 要搬多少页"，
         * 年龄只作为 [1,2) 的调节因子，在收益接近时偏向更冷的块。
         * 均匀负载下所有块年龄相近，两者退化为等价。 */
        age_norm = (double)age / ((double)age + w);

        score = ((double)(ppb - valid) / (double)(valid + 1u)) * (1.0 + age_norm);
        if (score > best) {
            best   = score;
            victim = pbn;
        }
    }
    return victim;
}

static pbn_t gc_pick_victim(ftl_dev_t *f)
{
    pbn_t victim;

    if (f->gc_policy == (uint32_t)FTL_GC_COST_BENEFIT) {
        victim = gc_pick_victim_cb(f);
    } else {
        victim = gc_pick_victim_greedy(f);
    }

    if (victim == SSD_INVALID_PBN) {
        /* 没有 closed 块：把当前 open block 强制关闭作为 victim */
        if (f->open_block != SSD_INVALID_PBN) {
            victim = f->open_block;
            nand_set_block_state(f->nand, victim, (uint16_t)NAND_BLK_CLOSED);
            f->open_block     = SSD_INVALID_PBN;
            f->open_next_page = 0u;
        }
    }
    return victim;
}

/*
 * 把一个块里的有效页搬到 destination 并更新 L2P。
 * GC 和静态磨损均衡共用这段逻辑 —— 两者的差别只在于"为什么搬"。
 */
int ftl_move_valid_pages(ftl_dev_t *f, pbn_t src, pbn_t *dest,
                         uint32_t *dest_wp, bool *reused_open, uint32_t *copied)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    ppn_t base = nand_geo_block_base(&nd->geo, src);
    uint32_t i;
    uint32_t attempt;
    uint32_t n = 0u;
    int rc = SSD_OK;

    for (i = 0; i < ppb; i++) {
        ppn_t sppn = base + i;
        ppn_t dppn;
        nand_page_meta_t m;

        if (nand_page_state(nd, sppn) != (uint16_t)NAND_PAGE_VALID) {
            continue;   /* 只有有效页才需要搬，失效页随擦除一起消失 */
        }

        rc = nand_read_sync(nd, sppn, &m);
        if (rc != SSD_OK) {
            /* 读不出来 = UECC 不可纠，这一页数据真的丢了。
             * 真实盘也是如此，固件能做的只有把错误上报给 host。 */
            SSD_WARN("gc: read failed on ppn=%llu, page lost",
                     (unsigned long long)sppn);
            rc = SSD_OK;
            continue;
        }
        if (m.lba == SSD_INVALID_LBA || m.lba >= f->user_lbas) {
            continue;   /* 元数据异常，跳过 */
        }

        /* destination 写满，换新块 */
        if (*dest_wp >= ppb) {
            ftl_release_dest(f, *dest, *dest_wp, *reused_open);
            *dest = ftl_take_gc_block(f);
            if (*dest == SSD_INVALID_PBN) {
                return SSD_ERR_NO_SPACE;
            }
            *dest_wp     = 0u;
            *reused_open = false;
            nand_set_block_state(nd, *dest, (uint16_t)NAND_BLK_OPEN);
        }

        /* 编程失败不能跳过这一页：被搬的页是 host 数据的唯一一份，
         * 放弃它就等于静默丢数据。作废当前页、换下一页重搬。 */
        rc = SSD_ERR_IO;
        for (attempt = 0u; attempt < 3u && *dest_wp < ppb; attempt++) {
            dppn = nand_geo_block_base(&nd->geo, *dest) + *dest_wp;
            rc = nand_prog_sync(nd, dppn, &m);
            if (rc == SSD_OK) {
                break;
            }
            (*dest_wp)++;   /* 这一页作废 */
            nand_set_block_next_page(nd, *dest, *dest_wp);
        }

        if (rc != SSD_OK) {
            /* 整个 destination 都不对劲，换一块再试最后一次 */
            ftl_release_dest(f, *dest, *dest_wp, *reused_open);
            *dest = ftl_take_gc_block(f);
            if (*dest == SSD_INVALID_PBN) {
                return SSD_ERR_NO_SPACE;
            }
            *dest_wp     = 0u;
            *reused_open = false;
            nand_set_block_state(nd, *dest, (uint16_t)NAND_BLK_OPEN);

            dppn = nand_geo_block_base(&nd->geo, *dest);
            if (nand_prog_sync(nd, dppn, &m) == SSD_OK) {
                *dest_wp = 1u;
                nand_set_block_next_page(nd, *dest, *dest_wp);
            } else {
                /* 实在写不进去，这一页只能丢 —— 真实盘会向 host 上报 */
                *dest_wp = 1u;
                nand_set_block_next_page(nd, *dest, *dest_wp);
                rc = SSD_OK;
                continue;
            }
        } else {
            (*dest_wp)++;
            nand_set_block_next_page(nd, *dest, *dest_wp);
        }
        rc = SSD_OK;

        /* 关键：搬移后必须更新 L2P，否则 host 会读到旧位置 */
        f->l2p[m.lba] = dppn;
        n++;
        ssd_stats_inc(ST_GC_COPY_PAGES);
    }

    if (copied != NULL) {
        *copied += n;
    }
    return rc;
}

int ftl_gc_one(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    pbn_t victim;
    pbn_t dest;
    uint32_t dest_wp;
    bool reused_open;
    uint32_t copied = 0u;
    ftl_pe_stat_t pe;
    int rc;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }

    victim = gc_pick_victim(f);
    if (victim == SSD_INVALID_PBN) {
        return SSD_ERR_NO_SPACE;   /* 没有任何可回收的块 */
    }

    /* destination：优先复用当前 open block 的剩余空间，
     * 但前提是它装得下 victim 的全部有效页 —— 装不下就老实取一个新块，
     * 避免搬到一半再换块（那时池子可能已经空了）。 */
    if (ftl_can_reuse_open(f, nand_block_valid_pages(nd, victim))) {
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
        nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);
    }

    rc = ftl_move_valid_pages(f, victim, &dest, &dest_wp, &reused_open, &copied);
    ftl_release_dest(f, dest, dest_wp, reused_open);
    if (rc != SSD_OK) {
        return rc;
    }

    /* 擦除 victim 并归还 */
    rc = nand_erase_sync(nd, victim);
    ssd_stats_inc(ST_GC_ERASES);
    if (rc == SSD_OK) {
        ftl_return_block(f, victim);
    } else {
        /* 擦除失败 -> 运行时坏块。此时块内有效数据已经搬完，不会丢数据 */
        SSD_WARN("gc: erase failed on pbn=%llu, dropped",
                 (unsigned long long)victim);
        nand_set_block_state(nd, victim, (uint16_t)NAND_BLK_BAD);
        f->bad_blocks++;
        ssd_stats_inc(ST_BADBLOCK_RUNTIME);
    }

    f->gc_count++;
    f->gc_copied += copied;

    /* 静态磨损均衡：磨损差拉开到阈值就搬一次冷数据。
     * 每 16 次回收才检查一次，避免扫描开销影响主路径。 */
    if (f->wl_enable != 0u && (f->gc_count % 16u) == 0u) {
        ftl_pe_stats(f, &pe);
        if (pe.max_pe > pe.min_pe + f->wl_pe_thresh) {
            (void)ftl_wl_static_once(f);
        }
    }
    return SSD_OK;
}

int ftl_bg_gc(ftl_dev_t *f)
{
    uint32_t guard = 0u;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }

    /* 后台 GC：空闲块低于目标水位就提前回收。
     * 它和前台 GC 用的是同一套回收逻辑，区别只在触发时机 ——
     * 提前干活的好处是 host 写时不必被迫等 GC，延迟更平稳。 */
    while (f->free_top < f->bg_target) {
        if (ftl_gc_one(f) != SSD_OK) {
            break;
        }
        f->bg_gc_count++;
        if (++guard > f->free_cap) {
            break;   /* 转了一圈还没补上，盘上确实没垃圾可收了 */
        }
    }
    return SSD_OK;
}
