#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

#include <math.h>
#include <string.h>

/*
 * 磨损均衡（S3）
 *
 * 为什么要做：NAND 每个块的 P/E 次数有上限（TLC 约 1k~3k 次）。
 * 如果某些块被反复擦写，它们会先于其他块失效，盘的寿命由"最差的块"决定。
 *
 * 动态 WL（在 ftl.c 的分配路径里）：
 *   每次从空闲池取块时，在栈顶若干候选里挑 P/E 次数最小的那个。
 *   它解决的是"写入机会不均"——让所有块轮流承担写入。
 *   但它救不了冷数据：一个块一旦装了不再改写的数据，就再也不参与轮换了。
 *
 * 静态 WL（本文件）：
 *   主动把低 P/E 块里的冷数据搬到高 P/E 块上，把低 P/E 块腾出来
 *   重新交给热数据去写。这是"冷数据占着新块"这个结构性问题的唯一解法。
 */

/* 从空闲池里取 P/E 次数最高（high=true）或最低的块。
 * 全池扫描 O(n)，但只在触发静态迁移时调用，不在主路径上。 */
static pbn_t take_extreme_pe_block(ftl_dev_t *f, bool high)
{
    uint32_t i;
    uint32_t best = 0u;
    uint32_t best_pe = 0u;
    pbn_t pbn;

    if (f->free_top == 0u) {
        return SSD_INVALID_PBN;
    }
    for (i = 0; i < f->free_top; i++) {
        uint32_t pe = nand_pe_count(f->nand, f->free_stack[i]);
        if (i == 0u || (high ? (pe > best_pe) : (pe < best_pe))) {
            best    = i;
            best_pe = pe;
        }
    }
    pbn = f->free_stack[best];
    f->free_stack[best] = f->free_stack[f->free_top - 1u];
    f->free_top--;
    return pbn;
}

int ftl_wl_static_once(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    pbn_t cold = SSD_INVALID_PBN;
    uint32_t cold_pe = 0xFFFFFFFFu;
    pbn_t dest;
    uint32_t dest_wp;
    bool reused_open;
    uint32_t copied = 0u;
    uint32_t lost = 0u;
    pbn_t pbn;
    int rc;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }

    /* 冷块 = P/E 次数最低、且还带着有效数据的 CLOSED 块 */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint32_t pe;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
        if (nand_block_valid_pages(nd, pbn) == 0u) {
            continue;   /* 没数据，交给普通 GC 收就行 */
        }
        pe = nand_pe_count(nd, pbn);
        if (pe < cold_pe) {
            cold_pe = pe;
            cold    = pbn;
        }
    }
    if (cold == SSD_INVALID_PBN) {
        return SSD_ERR_NO_SPACE;
    }

    /* destination 取池里 P/E 最高的块：
     * 冷数据搬到"已经磨损较多"的块上，被腾出来的低 P/E 块回到轮换中。 */
    dest = take_extreme_pe_block(f, true);
    if (dest == SSD_INVALID_PBN) {
        return SSD_ERR_NO_SPACE;
    }
    dest_wp     = 0u;
    reused_open = false;
    nand_set_block_state(nd, dest, (uint16_t)NAND_BLK_OPEN);

    rc = ftl_move_valid_pages(f, cold, &dest, &dest_wp, &reused_open,
                              &copied, &lost);
    ftl_release_dest(f, dest, dest_wp, reused_open);
    if (rc != SSD_OK) {
        return rc;
    }
    if (lost != 0u) {
        /* 与 GC 同理：还有页留在 cold 里，擦掉就是丢数据 */
        SSD_ERR("wl: abort, %u page(s) still in pbn=%llu",
                (unsigned)lost, (unsigned long long)cold);
        return SSD_ERR_IO;
    }

    rc = nand_erase_sync(nd, cold);
    ssd_stats_inc(ST_GC_ERASES);
    if (rc == SSD_OK) {
        ftl_return_block(f, cold);
    } else {
        nand_set_block_state(nd, cold, (uint16_t)NAND_BLK_BAD);
        f->bad_blocks++;
        ssd_stats_inc(ST_BADBLOCK_RUNTIME);
    }

    f->wl_migrations++;
    f->wl_migrated_pages += copied;

    SSD_INFO("wl: migrate cold pbn=%llu pe=%u -> dest pe=%u, copied=%u",
             (unsigned long long)cold, cold_pe,
             nand_pe_count(nd, dest), copied);
    return SSD_OK;
}

void ftl_pe_stats(const ftl_dev_t *f, ftl_pe_stat_t *st)
{
    const nand_dev_t *nd;
    pbn_t pbn;
    uint64_t sum = 0u;
    double avg;
    double var = 0.0;

    if (f == NULL || st == NULL) {
        return;
    }
    nd = f->nand;
    memset(st, 0, sizeof(*st));
    st->min_pe = 0xFFFFFFFFu;

    /* 只统计可用块：坏块已经退出服务，不该拉低"磨损均衡"的评价 */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint32_t pe;

        if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
            continue;
        }
        pe = nand_pe_count(nd, pbn);
        sum += pe;
        if (pe < st->min_pe) {
            st->min_pe = pe;
        }
        if (pe > st->max_pe) {
            st->max_pe = pe;
        }
        st->samples++;
    }
    if (st->samples == 0u) {
        st->min_pe = 0u;
        return;
    }

    avg = (double)sum / (double)st->samples;
    st->avg_pe = avg;

    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        double d;

        if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
            continue;
        }
        d = (double)nand_pe_count(nd, pbn) - avg;
        var += d * d;
    }
    var /= (double)st->samples;
    st->var_pe    = var;
    st->stddev_pe = sqrt(var);
}
