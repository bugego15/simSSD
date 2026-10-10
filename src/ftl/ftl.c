#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/clock.h"
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

    /* S6：superblock 批次标记（纯 DRAM，掉电即失） */
    f->striping = cfg->striping;
    f->blk_sb = (uint64_t *)calloc((size_t)nand->geo.total_blocks,
                                   sizeof(uint64_t));
    if (f->blk_sb == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    /* 在飞的写：容量按队列深度给，表满只是少记几条，不影响正确性 */
    f->infl_cap = (cfg->qdepth > 0u) ? (cfg->qdepth + 16u) : 64u;
    f->infl_lba = (lba_t *)calloc((size_t)f->infl_cap, sizeof(lba_t));
    f->infl_old = (ppn_t *)calloc((size_t)f->infl_cap, sizeof(ppn_t));
    f->infl_new = (ppn_t *)calloc((size_t)f->infl_cap, sizeof(ppn_t));
    if (f->infl_lba == NULL || f->infl_old == NULL || f->infl_new == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    /* S3：回收与磨损策略 */
    f->gc_policy    = cfg->gc_policy;
    f->wl_enable    = cfg->wl_enable;
    f->wl_pe_thresh = cfg->wl_pe_thresh;

    /* S6：checkpoint 间隔（0=关闭，上电重建退回 S5 的全盘扫描） */
    f->cp_interval = cfg->cp_interval;

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

    /*
     * 入池顺序必须打乱。
     *
     * 地址是连续编码的（每 pages_per_block×blocks_per_ce 个块属于同一个 CE），
     * 按 pbn 顺序入栈会让"栈顶连续一段"全部落在同一个 CE 上。
     * 于是"每个 CE 一个 open block"的轮转拿不到不同 CE 的块，
     * 4 个写入块会挤在同一个 CE 里 —— 实测并行度就是 1.00/4，白搭。
     */
    {
        uint64_t st = (uint64_t)cfg->seed * 2654435761u + 12345u;
        uint32_t k;

        for (k = (uint32_t)f->free_top; k > 1u; k--) {
            uint32_t j;
            pbn_t    tmp;

            st = st * 6364136223846793005ULL + 1442695040888963407ULL;
            j  = (uint32_t)((st >> 33) % (uint64_t)k);
            tmp                  = f->free_stack[k - 1u];
            f->free_stack[k - 1u] = f->free_stack[j];
            f->free_stack[j]     = tmp;
        }
    }

    /* 每个 CE 一个 open block：写请求在它们之间轮转，
     * 连续的请求才能落到不同 CE 上并行编程（S4 的并行度就来自这里） */
    f->n_open = nand_geo_ce_count(&nand->geo);
    if (f->n_open == 0u) {
        f->n_open = 1u;
    }
    f->open_blocks = (pbn_t *)calloc((size_t)f->n_open, sizeof(pbn_t));
    f->open_next   = (uint32_t *)calloc((size_t)f->n_open, sizeof(uint32_t));
    if (f->open_blocks == NULL || f->open_next == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    /*
     * 搬移流水线暂存：一次块的搬移最多 pages_per_block 页。
     * S6 的批量回收一次要搬整个 superblock 的有效页，所以按 CE 数放大，
     * 让"一次回收 N 块"能在同一批流水线里完成（读阶段因此能全并行）。
     */
    f->move_cap   = nand->geo.pages_per_block * f->n_open;
    f->move_slots = (ftl_move_slot_t *)calloc((size_t)f->move_cap,
                                              sizeof(ftl_move_slot_t));
    f->move_buf   = (nand_page_meta_t *)calloc((size_t)f->move_cap,
                                               sizeof(nand_page_meta_t));
    f->move_src   = (ppn_t *)calloc((size_t)f->move_cap, sizeof(ppn_t));
    f->move_dst   = (ppn_t *)calloc((size_t)f->move_cap, sizeof(ppn_t));
    if (f->move_slots == NULL || f->move_buf == NULL ||
        f->move_src == NULL || f->move_dst == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }

    f->gc_victims     = (pbn_t *)calloc((size_t)f->n_open, sizeof(pbn_t));
    if (f->gc_victims == NULL) {
        ftl_deinit(f);
        return SSD_ERR_NO_MEM;
    }
    for (i = 0; i < f->n_open; i++) {
        f->open_blocks[i] = SSD_INVALID_PBN;
    }
    f->open_rr = 0u;
    f->valid   = true;

    SSD_INFO("ftl initialized: user_lbas=%llu, free=%u, rsv_min=%u, open=%u",
             (unsigned long long)f->user_lbas, f->free_top, f->rsv_min, f->n_open);
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
    free(f->blk_sb);
    free(f->infl_lba);
    free(f->infl_old);
    free(f->infl_new);
    free(f->open_blocks);
    free(f->open_next);
    free(f->move_slots);
    free(f->move_buf);
    free(f->move_src);
    free(f->move_dst);
    free(f->gc_victims);
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
    /* S6：出池即标活跃 —— 之后对它的写都必须进入上电重扫范围 */
    nand_nv_active_set(f->nand, pbn);
    return pbn;
}

/*
 * 从池里取块，优先取属于指定 CE 的那个（S4）。
 *
 * 目的是让每个 open block 落在不同 CE 上：写请求在它们之间轮转时，
 * 页编程才能真正并行。盘上 CE 再多，若所有写入块都挤在同一个 CE 上，
 * 介质并行度还是 1。
 * 找不到同 CE 的块就退回普通取块，那一次写入会临时落到别的 CE —— 无害。
 */
static pbn_t take_block_prefer_ce_ex(ftl_dev_t *f, uint32_t ce_idx,
                                     bool respect_watermark)
{
    uint32_t k = 32u;
    uint32_t i;
    bool     found = false;
    uint32_t best_pos = 0u;

    /* host 分配要留水位给 GC；GC 自己取块则可以用到最后一刻 */
    if (respect_watermark && f->free_top <= f->rsv_min) {
        return SSD_INVALID_PBN;
    }
    if (f->free_top == 0u) {
        return SSD_INVALID_PBN;
    }
    if (k > f->free_top) {
        k = f->free_top;
    }

    for (i = 0; i < k; i++) {
        uint32_t pos = f->free_top - 1u - i;
        pbn_t    pbn = f->free_stack[pos];

        if (nand_geo_ce_index_of_pbn(&f->nand->geo, pbn) != ce_idx) {
            continue;
        }
        /* 同 CE 的候选里还是挑 P/E 最小的，"按 CE 取块"不能破坏磨损均衡 */
        if (!found ||
            nand_pe_count(f->nand, pbn) <
            nand_pe_count(f->nand, f->free_stack[best_pos])) {
            found    = true;
            best_pos = pos;
        }
    }

    if (found) {
        pbn_t pbn = f->free_stack[best_pos];

        f->free_stack[best_pos] = f->free_stack[f->free_top - 1u];
        f->free_top--;
        nand_nv_active_set(f->nand, pbn);
        return pbn;
    }

    /*
     * 没找到同 CE 的块就退回"随便拿一块"。
     *
     * 注意 GC 路径必须退回 ftl_take_gc_block（不看水位），
     * 不能退回 ftl_take_free_block —— 后者一到水位线就拒绝，
     * 于是 GC 在 free<=rsv_min（恰恰是它该干活的时刻）一块都拿不到，
     * 搬移写不出去、victim 不敢擦，池子被 host 一路取空直到 free=0 卡死。
     */
    return respect_watermark ? ftl_take_free_block(f) : ftl_take_gc_block(f);
}

/* GC / 搬移路径取块：不受 host 水位限制，优先取目标 CE 的块 */
pbn_t ftl_take_gc_block_ce(ftl_dev_t *f, uint32_t ce_idx)
{
    return take_block_prefer_ce_ex(f, ce_idx, false);
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
    {
        pbn_t pbn = f->free_stack[--f->free_top];

        nand_nv_active_set(f->nand, pbn);
        return pbn;
    }
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
    {
        pbn_t pbn = f->free_stack[--f->free_top];

        nand_nv_active_set(f->nand, pbn);
        return pbn;
    }
}

void ftl_return_block(ftl_dev_t *f, pbn_t pbn)
{
    SSD_ASSERT(f != NULL);
    SSD_ASSERT(pbn < f->nand->geo.total_blocks);

    if (f->free_top >= f->free_cap) {
        SSD_BUG("block pool overflow");
        return;
    }
    /*
     * S6：归还的块同样要标活跃。
     *
     * 它被回收擦掉了，而快照里还记着"这块装满数据" —— 不重扫的话，
     * 上电后 FTL 会拿着一份已经不存在的块表去算空闲池与有效页数，
     * 凭空少掉一批可用块。规则因此是：凡状态在快照之后变过的块都要扫。
     */
    nand_nv_active_set(f->nand, pbn);
    f->free_stack[f->free_top++] = pbn;
}

/* ------------------------------------------------------------------ */
/* 页分配                                                              */
/* ------------------------------------------------------------------ */

/*
 * S6：按 superblock 一次性把空槽位填满。
 *
 * 与"缺哪块补哪块"的区别不在能不能写，而在组的生命周期：
 * 逐个补块时，各块的写指针起点不同，写满时刻错开，GC 永远凑不齐
 * 一组"同时该回收"的块；整组一起开写之后，它们一起写满，
 * "一次回收 N 块"才真正成立（见 gc.c 的批量回收）。
 *
 * 凑不齐一整组是常态（池子可能不够、某些 CE 暂时没有块），
 * 少几块并不影响正确性，只是这一批的并行度少一点。
 * 但 host 正等着用的那个槽位必须拿到块 —— 拿不到就返回 false，
 * 交给调用方走"单块分配 / 触发 GC"的老路。
 */
static bool ftl_open_alloc_superblock(ftl_dev_t *f, uint32_t start_idx)
{
    uint64_t sb = 0u;
    uint32_t i;

    if (f == NULL || f->n_open == 0u) {
        return false;
    }

    for (i = 0; i < f->n_open; i++) {
        uint32_t idx = (start_idx + i) % f->n_open;
        pbn_t    blk;

        if (f->open_blocks[idx] != SSD_INVALID_PBN) {
            continue;   /* 这个槽位还在写，属于上一批 */
        }
        blk = take_block_prefer_ce_ex(f, idx, true);
        if (blk == SSD_INVALID_PBN) {
            if (i == 0u) {
                return false;   /* 连 host 要的那个槽位都拿不到 */
            }
            break;              /* 其余槽位凑不齐：少几块也能跑 */
        }
        if (sb == 0u) {
            sb = ++f->sb_seq;
        }
        ftl_open_set(f, idx, blk, 0u);
        f->blk_sb[blk] = sb;
    }
    return true;
}

int ftl_alloc_ppn(ftl_dev_t *f, ppn_t *out)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    uint32_t guard = 0u;

    for (;;) {
        uint32_t idx;
        pbn_t    ob;

        /*
         * 严格轮转：每次都从 open_rr 指向的槽位分配，用完就把指针推到下一个。
         *
         * 这是 S4 并行度的来源 —— 连续的请求落到不同 CE 的块上，
         * 它们的页编程才能在介质上真正重叠起来。
         * 只有一个 open block 时，所有写都排队在同一个 CE 上，
         * 盘上有再多 CE 也只能跑出 1 个 CE 的 IOPS（实测并行度 1.00/4）。
         *
         * 注意不能写成"找一个还有空间的 open block"：那样会一直沿用
         * 第一个还没写满的块，几个 open block 变成依次写满，等于还是串行。
         */
        idx = f->open_rr % f->n_open;
        ob  = f->open_blocks[idx];

        if (ob != SSD_INVALID_PBN && f->open_next[idx] < ppb) {
            *out = nand_geo_block_base(&nd->geo, ob) + f->open_next[idx];
            f->open_next[idx]++;
            nand_set_block_next_page(nd, ob, f->open_next[idx]);
            f->open_rr = (idx + 1u) % f->n_open;
            return SSD_OK;
        }

        /* 该槽位写满了（或还空着）：关掉旧块，补一个新的进来 */
        if (ob != SSD_INVALID_PBN) {
            nand_set_block_state(nd, ob, (uint16_t)NAND_BLK_CLOSED);
            f->open_blocks[idx] = SSD_INVALID_PBN;
            f->open_next[idx]   = 0u;
        }

        /*
         * S6：优先整组补块；组分配拿不到块时退回"只补这一个槽位"。
         * 两者都会回到循环开头重新判断，绝不在这里硬取一块覆盖槽位。
         */
        if (f->striping != 0u && ftl_open_alloc_superblock(f, idx)) {
            continue;
        }
        {
            pbn_t nb = take_block_prefer_ce_ex(f, idx, true);

            if (nb != SSD_INVALID_PBN) {
                ftl_open_set(f, idx, nb, 0u);
                continue;
            }
        }

        /* 2. 空闲块触及水位线：触发 GC。
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

            /*
             * 这里必须取到池子的最后一块。
             *
             * 水位线是"该去回收了"的信号，不是"不许分配"的禁令。
             * 写入槽位写满时换块是刚需 —— 不换就一个页也写不进去；
             * 而槽位是轮转使用的，四个槽位完全可能同时写满。
             * 若此时还坚持"池子要多于 rsv_min"，盘会在还有大量垃圾
             * 可回收的情况下僵住（实测 free 停在 1 就再也写不动了）。
             *
             * 取到最后一块会不会把 GC 饿死？不会：S4 的搬移目标复用写入
             * 槽位，回收不再依赖"池里还有块"，只要能擦 victim 就能腾出空间。
             */
            if (f->free_top == 0u) {
                SSD_ERR("alloc_ppn: out of space (free=%u, bad=%u)",
                        ftl_free_blocks(f), ftl_bad_blocks(f));
                return SSD_ERR_NO_SPACE;
            }
            last = ftl_take_gc_block(f);
            if (last == SSD_INVALID_PBN) {
                return SSD_ERR_NO_SPACE;
            }
            {
                uint32_t idx = f->open_rr % f->n_open;

                if (f->open_blocks[idx] != SSD_INVALID_PBN) {
                    nand_set_block_state(nd, f->open_blocks[idx],
                                         (uint16_t)NAND_BLK_CLOSED);
                }
                ftl_open_set(f, idx, last, 0u);
            }
            guard = 0u;
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 读写路径                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* open block 槽位（S4：每个 CE 一个写入块）                           */
/* ------------------------------------------------------------------ */

uint32_t ftl_open_find(const ftl_dev_t *f, pbn_t pbn)
{
    uint32_t i;

    SSD_ASSERT(f != NULL);
    for (i = 0; i < f->n_open; i++) {
        if (f->open_blocks[i] == pbn) {
            return i;
        }
    }
    return f->n_open;
}

void ftl_open_set(ftl_dev_t *f, uint32_t idx, pbn_t pbn, uint32_t wp)
{
    SSD_ASSERT(f != NULL);
    SSD_ASSERT(idx < f->n_open);

    f->open_blocks[idx] = pbn;
    f->open_next[idx]   = wp;
    if (pbn != SSD_INVALID_PBN) {
        nand_set_block_state(f->nand, pbn, (uint16_t)NAND_BLK_OPEN);
    }
}

/* 把一块登记为 open block：有空槽且还有剩余空间就继续写，否则关闭等 GC */
void ftl_open_adopt(ftl_dev_t *f, pbn_t pbn, uint32_t wp)
{
    uint32_t ppb = f->nand->geo.pages_per_block;
    uint32_t i;

    if (wp < ppb) {
        for (i = 0; i < f->n_open; i++) {
            if (f->open_blocks[i] == SSD_INVALID_PBN) {
                ftl_open_set(f, i, pbn, wp);
                return;
            }
        }
    }
    nand_set_block_state(f->nand, pbn, (uint16_t)NAND_BLK_CLOSED);
}

void ftl_open_drop(ftl_dev_t *f, pbn_t pbn)
{
    uint32_t idx = ftl_open_find(f, pbn);

    if (idx < f->n_open) {
        f->open_blocks[idx] = SSD_INVALID_PBN;
        f->open_next[idx]   = 0u;
    }
}

/* 盘上一个 CLOSED 块都没有时，强制关掉一个 open block 当 GC 的 victim */
bool ftl_open_take_victim(ftl_dev_t *f, pbn_t *out)
{
    uint32_t i;

    for (i = 0; i < f->n_open; i++) {
        if (f->open_blocks[i] != SSD_INVALID_PBN) {
            pbn_t p = f->open_blocks[i];

            /* 还有页在飞的写入块同样不能拿来当 victim */
            if (nand_block_pending_pages(f->nand, p) != 0u) {
                continue;
            }
            nand_set_block_state(f->nand, p, (uint16_t)NAND_BLK_CLOSED);
            f->open_blocks[i] = SSD_INVALID_PBN;
            f->open_next[i]   = 0u;
            *out = p;
            return true;
        }
    }
    return false;
}

bool ftl_open_reuse(ftl_dev_t *f, uint32_t need_pages,
                    pbn_t *dest, uint32_t *dest_wp)
{
    uint32_t ppb = f->nand->geo.pages_per_block;
    uint32_t i;

    SSD_ASSERT(f != NULL);
    for (i = 0; i < f->n_open; i++) {
        uint32_t idx = (f->open_rr + i) % f->n_open;

        if (f->open_blocks[idx] == SSD_INVALID_PBN || f->open_next[idx] >= ppb) {
            continue;
        }
        if ((ppb - f->open_next[idx]) < need_pages) {
            continue;   /* 装不下：宁可新开一块，也不要搬到一半再换块 */
        }
        *dest    = f->open_blocks[idx];
        *dest_wp = f->open_next[idx];
        return true;
    }
    return false;
}

/*
 * 下刷 system 区的写序号（S5）。
 *
 * 必须持久化，但不能每次 host 写都刷：system 区自己也要吃 P/E 次数，
 * 真实固件是攒一批再下刷。代价是最多回退一个间隔 —— 无所谓，
 * 重建时取 max(持久化的序号, 介质上最大的 seq)，后者永远不落后。
 */
#define FTL_SEQ_PERSIST_INTERVAL 1024u

static void ftl_persist_seq_maybe(ftl_dev_t *f)
{
    if ((f->write_seq & (uint64_t)(FTL_SEQ_PERSIST_INTERVAL - 1u)) == 0u) {
        nand_set_persistent_seq(f->nand, f->write_seq);
    }
}

/* ------------------------------------------------------------------ */
/* S6：checkpoint                                                      */
/* ------------------------------------------------------------------ */

int ftl_checkpoint(ftl_dev_t *f)
{
    uint64_t ns;
    uint32_t i;

    if (f == NULL || !f->valid || f->cp_interval == 0u) {
        return SSD_ERR_INVAL;
    }

    /*
     * 快照要拍的是"一致点"，所以在飞的写必须先回滚掉。
     *
     * 不这么做会静默丢数据：假设 lba=5 旧版本在块 A、新版本正在块 B 里编程。
     * 若照抄当前映射，快照记的是块 B；掉电后块 B 那一页是撕裂页，
     * 而块 A 不在扫描范围（快照说它"已经过时"）—— 于是 lba=5 两个版本都找不到。
     * 回滚之后快照记的是块 A，掉电重建时块 B 被扫出来判为撕裂、块 A 正常复活，
     * 正好回到"最后一次真正落盘的版本"。
     */
    for (i = f->infl_n; i > 0u; i--) {
        f->l2p[f->infl_lba[i - 1u]] = f->infl_old[i - 1u];
    }

    nand_cp_store(f->nand, f->write_seq, f->l2p, f->user_lbas,
                  f->open_blocks, f->open_next, f->n_open);

    for (i = 0; i < f->infl_n; i++) {
        f->l2p[f->infl_lba[i]] = f->infl_new[i];
    }

    /*
     * 快照只负责"这一刻之前"的事。
     *
     * 之后可能被写的块有两类，都必须重新标活跃：
     *   - 现在开着的块：写指针还要往前走，快照记的是旧写指针
     *   - 之后从池里取出来的块：出池时会被标（见各 take 函数）
     * 漏掉第一类是最危险的一种：上电时拿快照里的写指针接着写，
     * 等于把 host 的新数据写进别人的旧页上。
     */
    nand_nv_active_clear_all(f->nand);
    for (i = 0; i < f->n_open; i++) {
        if (f->open_blocks[i] != SSD_INVALID_PBN) {
            nand_nv_active_set(f->nand, f->open_blocks[i]);
        }
    }

    /* 下刷是要真写介质的：这笔时间必须从 host 的预算里扣 */
    ns = nand_cp_store_time_ns(f->nand);
    ssd_clock_advance(ns);
    f->cp_count++;
    f->cp_ns += ns;

    SSD_DBG("checkpoint #%llu at seq=%llu (%.3f ms, active blocks kept %u)",
              (unsigned long long)f->cp_count,
              (unsigned long long)f->write_seq,
              (double)ns / 1e6, (unsigned)nand_nv_active_count(f->nand));
    return SSD_OK;
}

static void ftl_checkpoint_maybe(ftl_dev_t *f)
{
    if (f->cp_interval == 0u || f->write_seq == 0u) {
        return;
    }
    if ((f->write_seq % (uint64_t)f->cp_interval) != 0u) {
        return;
    }
    (void)ftl_checkpoint(f);
}

/* 页失效：除了介质层的状态，还要记下"这块最后一次变脏"的时刻，
 * cost-benefit 靠它区分冷热。 */

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

    /* 这个 lba 重新有数据了，持久化的 TRIM 标记必须撤掉，
     * 否则掉电重建会把它当"已删除"，把刚写进去的内容丢掉 */
    nand_nv_trim_clear(f->nand, lba);

    old = f->l2p[lba];
    if (old != SSD_INVALID_PPN) {
        ftl_mark_invalid(f, old);
    }
    f->l2p[lba] = ppn;
    f->write_seq++;
    ftl_persist_seq_maybe(f);
    ftl_checkpoint_maybe(f);
    ssd_stats_inc(ST_HOST_WRITE_PAGES);

    /* 后台 GC：借 host 命令之间的空隙提前回收，
     * 免得 host 写撞到水位线被迫同步等 GC（S4 会把它移到真正的 idle 时段） */
    if (f->bg_target > 0u && ftl_free_blocks(f) < f->bg_target) {
        (void)ftl_bg_gc(f);
    }
    return SSD_OK;
}

/*
 * 异步写（S4）：分配 + 提交页编程，立即返回，完成由回调通知。
 *
 * 映射更新的时机值得说明：在"提交成功之后、回调之前"更新。
 *   - 不能等编程成功再更新：并发写同一个 lba 时两次提交都还在飞，
 *     谁先完成不确定，按完成顺序更新会让先提交的写覆盖后提交的。
 *     提交即更新保证了映射顺序与提交顺序一致。
 *   - 也不能在提交之前更新：万一提交失败，旧页已经被判失效，
 *     就出现了"新旧都没有"的丢失窗口。
 */
int ftl_write_async(ftl_dev_t *f, lba_t lba, uint32_t seq, ppn_t *out_ppn,
                    nand_done_cb cb, void *arg)
{
    nand_page_meta_t m;
    ppn_t old;
    ppn_t ppn;
    int rc;

    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }

    m.lba   = lba;
    m.seq   = seq;
    m.state = (uint16_t)NAND_PAGE_VALID;
    m.crc   = 0u;
    nand_meta_seal(&m);

    rc = ftl_alloc_ppn(f, &ppn);
    if (rc != SSD_OK) {
        return rc;
    }

    rc = nand_prog_async(f->nand, ppn, &m, cb, arg);
    if (rc != SSD_OK) {
        return rc;   /* 提交失败：映射未动，旧数据仍然可读 */
    }
    if (out_ppn != NULL) {
        *out_ppn = ppn;
    }

    /* 与同步写同理：提交即清除 TRIM 标记 */
    nand_nv_trim_clear(f->nand, lba);

    old = f->l2p[lba];
    if (old != SSD_INVALID_PPN) {
        ftl_mark_invalid(f, old);
    }
    f->l2p[lba] = ppn;
    /* 记下"这笔写还在飞"：下刷快照时要把映射临时回滚到 old */
    if (f->infl_n < f->infl_cap) {
        f->infl_lba[f->infl_n] = lba;
        f->infl_old[f->infl_n] = old;
        f->infl_new[f->infl_n] = ppn;
        f->infl_n++;
    }
    f->write_seq++;
    ftl_persist_seq_maybe(f);
    ftl_checkpoint_maybe(f);
    ssd_stats_inc(ST_HOST_WRITE_PAGES);
    return SSD_OK;
}

/* 一笔在飞的写落地（或失败）之后，把它从表里摘掉。保持提交顺序。 */
static void infl_remove(ftl_dev_t *f, lba_t lba, ppn_t ppn)
{
    uint32_t i;
    uint32_t j;

    for (i = 0; i < f->infl_n; i++) {
        if (f->infl_lba[i] != lba || f->infl_new[i] != ppn) {
            continue;
        }
        for (j = i + 1u; j < f->infl_n; j++) {
            f->infl_lba[j - 1u] = f->infl_lba[j];
            f->infl_old[j - 1u] = f->infl_old[j];
            f->infl_new[j - 1u] = f->infl_new[j];
        }
        f->infl_n--;
        return;
    }
}

void ftl_write_abandon(ftl_dev_t *f, lba_t lba, ppn_t ppn, ppn_t old_ppn)
{
    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return;
    }
    if (f->l2p[lba] != ppn) {
        return;   /* 期间已被别的写接管：那一版才是有效的，不能动 */
    }
    f->l2p[lba] = old_ppn;
    if (ppn != SSD_INVALID_PPN) {
        nand_invalidate_page(f->nand, ppn);
    }
}

void ftl_prog_settled(ftl_dev_t *f, lba_t lba, ppn_t ppn, int status)
{
    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return;
    }
    infl_remove(f, lba, ppn);
    if (status != SSD_OK) {
        return;   /* 编程失败：数据没进去，等上层重发 */
    }
    if (f->l2p[lba] == ppn) {
        return;   /* 仍是最新的那一页，保持有效 */
    }
    /* 编程期间这个 lba 又被写了：这一页已经过时，立刻判成垃圾，
     * 免得 GC 之后把它当有效数据搬回去 */
    ftl_mark_invalid(f, ppn);
}

int ftl_read_async(ftl_dev_t *f, lba_t lba, nand_page_meta_t *out,
                   nand_done_cb cb, void *arg)
{
    ppn_t ppn;

    if (f == NULL || out == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_ERR_INVAL;
    }

    ppn = f->l2p[lba];
    if (ppn == SSD_INVALID_PPN) {
        out->lba   = SSD_INVALID_LBA;
        out->seq   = 0u;
        out->state = (uint16_t)NAND_PAGE_FREE;
        out->crc   = 0u;
        return SSD_ERR_NO_SPACE;   /* 从未写过，不必惊动介质 */
    }

    ssd_stats_inc(ST_HOST_READ_PAGES);
    return nand_read_async(f->nand, ppn, out, cb, arg);
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

        /*
         * TRIM 必须落进 system 区的持久化位图。
         *
         * 光把映射断开是不够的：页里的数据还实实在在躺在介质上，
         * spare 里也还写着它的 lba。掉电重建是"扫介质重建映射"，
         * 如果不知道这段地址被删过，那些页会被原样映射回来 ——
         * 数据死而复生。这不只是语义问题：host 认为删掉的数据
         *（密钥、日志、用户文件）又变回可读，是实打实的安全问题。
         */
        nand_nv_trim_set(f->nand, lba + i);

        if (ppn == SSD_INVALID_PPN) {
            continue;   /* 本来就没数据 */
        }
        ftl_mark_invalid(f, ppn);
        f->l2p[lba + i] = SSD_INVALID_PPN;
        trimmed++;
    }

    f->write_seq++;
    /* TRIM 是低频命令，下刷不必攒批 */
    nand_set_persistent_seq(f->nand, f->write_seq);
    /* TRIM 改的是映射本身，同样必须进快照，否则被删的数据会死而复生 */
    ftl_checkpoint_maybe(f);
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
    uint32_t lost = 0u;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }
    if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
        return SSD_OK;   /* 已经隔离过了，幂等 */
    }

    /* 先把坏块从写入轮换里摘下来，免得抢救时又往坏块里写 */
    ftl_open_drop(f, pbn);

    /* 抢救目的地优先复用当前 open block 的剩余空间。
     * 这一点很关键：如果每次隔离坏块都新开一整块，坏块一多，
     * 大量"只装了几页"的块会把盘上空间活活耗光（实测 free 会掉到 0）。 */
    if (ftl_open_reuse(f, nand_block_valid_pages(nd, pbn), &dest, &dest_wp)) {
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
     * 且不会反过来调用 isolate，不存在递归风险。
     * 坏块必须隔离，所以搬不走的页只能认账（lost 计出来只用于告警）。 */
    (void)ftl_move_valid_pages(f, pbn, &dest, &dest_wp, &reused_open,
                               &saved, &lost);
    ftl_release_dest(f, dest, dest_wp, reused_open);

    nand_set_block_state(nd, pbn, (uint16_t)NAND_BLK_BAD);
    f->bad_blocks++;
    f->bb_saved_pages += saved;
    ssd_stats_inc(ST_BADBLOCK_RUNTIME);

    SSD_WARN("bad block isolated: pbn=%llu saved_pages=%u lost=%u",
             (unsigned long long)pbn, saved, lost);
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
    /*
     * 没有写缓存，页数据在提交那一刻就已经在介质上了（异步写也在飞），
     * 所以没有 payload 要下刷。真正要刷的是 system 区的元数据：
     * 写序号必须落下去，否则掉电重建时"最新一屏写"可能因为 seq
     * 没被记住而在版本比较里输给旧数据。
     *
     * 在飞的异步写不在这里等待 —— flush 的语义是"此前**已完成**的命令
     * 必须能扛住掉电"，而不是"帮我等所有请求完成"。
     */
    nand_set_persistent_seq(f->nand, f->write_seq);
    ssd_stats_inc(ST_HOST_FLUSH_CMDS);
    return SSD_OK;
}

ppn_t ftl_l2p_get(const ftl_dev_t *f, lba_t lba)
{
    if (f == NULL || !f->valid || lba >= f->user_lbas) {
        return SSD_INVALID_PPN;
    }
    return f->l2p[lba];
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
