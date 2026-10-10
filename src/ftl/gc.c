#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

#include <string.h>

/* 搬一页最多试几次编程：与 host 写路径一致，单次失败换页重来。
 * 连续失败才说明目标块真的不行了，那时宁可不擦 victim，也不能丢数据。 */
#define GC_MOVE_MAX_TRIES 3u

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

    nand_set_block_next_page(nd, dest, dest_wp);

    if (reused_open) {
        uint32_t idx = ftl_open_find(f, dest);

        if (idx < f->n_open) {
            f->open_next[idx] = dest_wp;
        }
        return;
    }

    /* 还有剩余空间就登记成某个 CE 的 open block，否则关闭等下次回收 */
    ftl_open_adopt(f, dest, dest_wp);
}

/* 已选列表里是否已经有这个块（批量选 victim 时要排除） */
static bool already_picked(const pbn_t *list, uint32_t n, pbn_t pbn)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (list[i] == pbn) {
            return true;
        }
    }
    return false;
}

/* 批量挑 victim：want 个有效页最少的块（S4 一次回收一批） */
static uint32_t gc_pick_victims_greedy(ftl_dev_t *f, pbn_t *out, uint32_t want)
{
    nand_dev_t *nd = f->nand;
    uint32_t found = 0u;

    while (found < want) {
        pbn_t    victim = SSD_INVALID_PBN;
        uint32_t min_valid = 0xFFFFFFFFu;
        pbn_t pbn;

        for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint32_t v;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
        /* 还有页在飞的块不能回收：擦掉之后这块会被重新分配，
         * 那个迟到的编程就会把 host 数据写进别人的块里 */
        if (nand_block_pending_pages(nd, pbn) != 0u) {
            continue;
        }
            if (already_picked(out, found, pbn)) {
                continue;
            }
            v = nand_block_valid_pages(nd, pbn);
            if (v < min_valid) {
                min_valid = v;
                victim    = pbn;
            }
        }

        if (victim == SSD_INVALID_PBN) {
            break;
        }
        out[found++] = victim;
    }
    return found;
}

static uint32_t gc_pick_victims_cb(ftl_dev_t *f, pbn_t *out, uint32_t want)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    uint32_t found = 0u;

    /* 写满一遍用户容量所需的 host 写次数：作为年龄的尺度基准 */
    double w = (double)((f->user_lbas > 0u) ? f->user_lbas : 1u);

    while (found < want) {
        pbn_t victim = SSD_INVALID_PBN;
        double best = -1.0;
        pbn_t pbn;

        for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint64_t age;
        uint32_t valid;
        double age_norm;
        double score;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
            if (nand_block_pending_pages(nd, pbn) != 0u) {
                continue;   /* 同上：有操作在飞的块不能碰 */
            }
            if (already_picked(out, found, pbn)) {
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

        if (victim == SSD_INVALID_PBN) {
            break;
        }
        out[found++] = victim;
    }
    return found;
}

/*
 * 一次挑一批 victim：返回实际挑到的块数。
 *
 * allow_force=false 时绝不为了凑数去强关 host 正在写的块 ——
 * 批量回收用它挑"种子"，否则一次回收可能把当前 superblock 整组关掉，
 * 剩下几个半满块变成碎片。
 */
static uint32_t gc_pick_victims_ex(ftl_dev_t *f, pbn_t *out, uint32_t want,
                                   bool allow_force)
{
    uint32_t n;

    if (f->gc_policy == (uint32_t)FTL_GC_COST_BENEFIT) {
        n = gc_pick_victims_cb(f, out, want);
    } else {
        n = gc_pick_victims_greedy(f, out, want);
    }

    if (n > 0u) {
        return n;
    }
    if (!allow_force) {
        return 0u;
    }
    {
        /* 没有 closed 块：强制关掉一个 open block 作为 victim */
        pbn_t v;
        if (ftl_open_take_victim(f, &v)) {
            out[0] = v;
            n = 1u;
        } else {
            nand_dev_t *nd = f->nand;
            uint32_t nb_closed = 0u, nb_pend = 0u, nb_open = 0u;
            pbn_t p;

            for (p = 0; p < nd->geo.total_blocks; p++) {
                uint16_t st = nand_block_state(nd, p);
                if (st == (uint16_t)NAND_BLK_CLOSED) {
                    nb_closed++;
                    if (nand_block_pending_pages(nd, p) != 0u) {
                        nb_pend++;
                    }
                } else if (st == (uint16_t)NAND_BLK_OPEN) {
                    nb_open++;
                }
            }
            SSD_ERR("gc: no victim (free=%u closed=%u pending=%u open=%u)",
                    ftl_free_blocks(f), nb_closed, nb_pend, nb_open);
        }
    }
    return n;
}

/* 一次挑一批 victim（老接口：允许在实在没得收时强关一个写入块） */
static uint32_t gc_pick_victims(ftl_dev_t *f, pbn_t *out, uint32_t want)
{
    return gc_pick_victims_ex(f, out, want, true);
}

/*
 * S6：凑一个 superblock 出来一起回收。
 *
 * 先按原策略挑出"最该回收的那一块"当种子，再把与它同批次的其余块
 * 一并收进来。同批次意味着它们是一起开写、一起写满的，
 * 有效页数也大致相当 —— 一次搬完，读阶段可以整批并行提交。
 *
 * 凑不齐就返回 1（退化成单块回收）：这是常态，不该为了凑数去碰别的块。
 */
static uint32_t gc_pick_victims_batch(ftl_dev_t *f, pbn_t *out, uint32_t want)
{
    nand_dev_t *nd = f->nand;
    uint32_t n;
    uint64_t sb;
    pbn_t pbn;

    n = gc_pick_victims_ex(f, out, 1u, false);
    if (n == 0u) {
        return 0u;
    }
    sb = f->blk_sb[out[0]];
    if (sb == 0u || want <= 1u) {
        return n;   /* 不属于任何批次（多半是刚恢复过），不凑批 */
    }

    for (pbn = 0; pbn < nd->geo.total_blocks && n < want; pbn++) {
        if (f->blk_sb[pbn] != sb) {
            continue;
        }
        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_CLOSED) {
            continue;
        }
        if (nand_block_pending_pages(nd, pbn) != 0u) {
            continue;   /* 有页在飞的块不能碰 */
        }
        if (already_picked(out, n, pbn)) {
            continue;
        }
        out[n++] = pbn;
    }
    return n;
}

/* 流水线暂存区的完成回调：只记成功与否，数据在 ftl_dev 的缓冲区里 */
static void move_read_done(void *arg, int status)
{
    ftl_move_slot_t *s = (ftl_move_slot_t *)arg;

    s->ok = (status == SSD_OK) ? 1 : 0;
}

static void move_write_done(void *arg, int status)
{
    ftl_move_slot_t *s = (ftl_move_slot_t *)arg;

    s->ok = (status == SSD_OK) ? 1 : 0;
}

/*
 * 搬移的"写"阶段有两种落盘策略：
 *   单块版写进一个 destination（磨损均衡 / 坏块抢救用）
 *   条带版轮转写进每个 CE 的写入槽位（GC 用，见 move_program_stripe）
 * 读阶段（move_collect）和映射更新（move_commit）是两者共用的。
 */
typedef bool (*move_prog_fn)(ftl_dev_t *f, uint32_t k, void *ctx);

typedef struct move_dest_ctx {
    pbn_t    *dest;
    uint32_t *dest_wp;
    bool     *reused_open;
} move_dest_ctx_t;

/* 条带化搬移的游标：轮到哪个 CE、本次回收已经补了几块、最多能补几块 */
typedef struct move_stripe_ctx {
    uint32_t rr;
    uint32_t took;
    uint32_t max_take;
} move_stripe_ctx_t;

/* 写一页到单个 destination：写满就换块。返回 false 表示这一页没写出去 */
static bool move_program_one(ftl_dev_t *f, uint32_t k, void *ctx)
{
    move_dest_ctx_t *c = (move_dest_ctx_t *)ctx;
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    ppn_t dppn;

    /* destination 写满，换新块 */
    if (*c->dest_wp >= ppb) {
        ftl_release_dest(f, *c->dest, *c->dest_wp, *c->reused_open);
        *c->dest = ftl_take_gc_block(f);
        if (*c->dest == SSD_INVALID_PBN) {
            return false;
        }
        *c->dest_wp     = 0u;
        *c->reused_open = false;
        nand_set_block_state(nd, *c->dest, (uint16_t)NAND_BLK_OPEN);
    }

    dppn = nand_geo_block_base(&nd->geo, *c->dest) + *c->dest_wp;
    f->move_slots[k].ok = 0;
    if (nand_prog_async(nd, dppn, &f->move_buf[k],
                        move_write_done, &f->move_slots[k]) != SSD_OK) {
        (*c->dest_wp)++;   /* 这一页作废 */
        nand_set_block_next_page(nd, *c->dest, *c->dest_wp);
        return false;
    }
    f->move_dst[k] = dppn;
    (*c->dest_wp)++;
    nand_set_block_next_page(nd, *c->dest, *c->dest_wp);
    return true;
}

/*
 * 写一页到轮转到的写入槽位（S4）。
 *
 * 这是 GC 能吃满多 CE 的关键：一个块物理上只属于一个 CE，
 * 把整块的有效页都写进同一块，等于让它们在同一颗 die 上排队编程。
 * 实测一次回收 = 193 页 × 2ms ≈ 390ms，而盘上有 4 个 CE ——
 * 总虚拟时间里 83% 都耗在这段串行编程上，p99.9 也是被它顶到 350ms 的。
 * 页按轮转摊到各槽位后，四颗 die 同时编程，这段停顿直接除以 CE 数。
 *
 * 空间上不会多占块：目标始终是"host 正在写的块"，
 * 槽位空着或写满了才从池里补一块并接管该槽位。
 * 只有写满的块才会被关闭 —— 半满的块绝不提前关，
 * 那是碎片、也是之前批量回收把池子饿死的根因。
 */
static bool move_program_stripe(ftl_dev_t *f, uint32_t k, void *ctx)
{
    move_stripe_ctx_t *c = (move_stripe_ctx_t *)ctx;
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    uint32_t nslots = (f->n_open > 0u) ? f->n_open : 1u;
    uint32_t s = nslots;   /* nslots = 没找到可用槽位 */
    uint32_t j;
    pbn_t blk;
    ppn_t dppn;

    /*
     * 只往"还有空间的槽位"写，槽位满了才从池里补 —— 而且一次回收最多补一块。
     *
     * 不设上限会是自杀：一次回收只归还 1 块（擦掉的 victim），而槽位是
     * 轮转使用的，池子一紧往往三四个槽位同时是空的 —— 那样一次回收要补
     * 3~4 块，净支出 2~3 块，十几轮就把池子抽干（实测 free 从 7 掉到 0，
     * 盘再也写不动）。有了"最多补一块"，回收对池子永远是净持平或净增。
     */
    for (j = 0; j < nslots; j++) {
        uint32_t t = (c->rr + j) % nslots;

        if (f->open_blocks[t] != SSD_INVALID_PBN && f->open_next[t] < ppb) {
            s = t;
            break;
        }
    }
    if (s == nslots) {
        /*
         * 补块额度。
         *
         * 一次收 N 块时允许补 N 块 —— 收进来的（擦掉 victim 归还的）和
         * 借出去的一样多，池子净变化为 0，绝不会越收越紧。
         * 单块回收沿用"最多补一块"的老规矩：那时净变化是 +0/-1，
         * 多补一块就真的在吃老本。
         */
        if (c->took >= c->max_take) {
            return false;
        }
        s   = c->rr % nslots;
        blk = ftl_take_gc_block_ce(f, s);
        if (blk == SSD_INVALID_PBN) {
            return false;   /* 一块都拿不到：这次回收做不成 */
        }
        if (f->open_blocks[s] != SSD_INVALID_PBN) {
            nand_set_block_state(nd, f->open_blocks[s],
                                 (uint16_t)NAND_BLK_CLOSED);
        }
        ftl_open_set(f, s, blk, 0u);   /* host 之后也从这个块继续写 */
        c->took++;
    }

    c->rr = (s + 1u) % nslots;   /* 下一页换一个 CE */
    blk = f->open_blocks[s];

    dppn = nand_geo_block_base(&nd->geo, blk) + f->open_next[s];
    f->move_slots[k].ok = 0;
    if (nand_prog_async(nd, dppn, &f->move_buf[k],
                        move_write_done, &f->move_slots[k]) != SSD_OK) {
        f->open_next[s]++;   /* 这一页作废 */
        nand_set_block_next_page(nd, blk, f->open_next[s]);
        return false;
    }
    f->move_dst[k] = dppn;
    f->open_next[s]++;
    nand_set_block_next_page(nd, blk, f->open_next[s]);
    return true;
}

/*
 * 阶段 1 的单块部分：把 src 里的有效页挂进流水线（从 n 号槽位开始存）。
 * 调用方决定何时 nand_run_until_idle —— 一次回收整个 superblock 时，
 * 所有 victim 的读一起提交、只等一次，读阶段才能真正并行起来。
 */
static uint32_t move_collect_into(ftl_dev_t *f, pbn_t src, uint32_t n)
{
    nand_dev_t *nd = f->nand;
    uint32_t ppb = nd->geo.pages_per_block;
    ppn_t base = nand_geo_block_base(&nd->geo, src);
    uint32_t i;

    for (i = 0; i < ppb && n < f->move_cap; i++) {
        ppn_t sppn = base + i;

        if (nand_page_state(nd, sppn) != (uint16_t)NAND_PAGE_VALID) {
            continue;   /* 只有有效页才需要搬，失效页随擦除一起消失 */
        }

        f->move_src[n]        = sppn;
        f->move_slots[n].f    = f;
        f->move_slots[n].idx  = n;
        f->move_slots[n].ok   = 0;
        f->move_slots[n].live  = 0u;
        f->move_slots[n].tries = 0u;
        memset(&f->move_buf[n], 0, sizeof(f->move_buf[n]));

        if (nand_read_async(nd, sppn, &f->move_buf[n],
                            move_read_done, &f->move_slots[n]) != SSD_OK) {
            continue;   /* 提交失败：这一页本次搬不了 */
        }
        n++;
    }
    return n;
}

/* --- 阶段 1：把 src 块里的有效页并行读上来，返回收集到的页数 --- */
static uint32_t move_collect(ftl_dev_t *f, pbn_t src)
{
    uint32_t n = move_collect_into(f, src, 0u);

    /* 等读落地。这段时间 host 的请求也在其他 CE 上推进 —— 不会被白等 */
    nand_run_until_idle(f->nand);
    return n;
}

/* --- 阶段 2 + 2b：并行写出去，写失败的换页重试。返回最终没写出去的页数 --- */
static uint32_t move_write_phase(ftl_dev_t *f, uint32_t n,
                                 move_prog_fn prog, void *ctx)
{
    uint32_t k;
    uint32_t round;
    uint32_t dead = 0u;

    for (k = 0; k < n; k++) {
        if (!f->move_slots[k].ok) {
            /* 读不出来 = UECC 不可纠，这一页数据真的丢了。
             * 真实盘也是如此，固件能做的只有把错误上报给 host。 */
            SSD_WARN("gc: read failed on ppn=%llu, page lost",
                     (unsigned long long)f->move_src[k]);
            continue;
        }
        if (f->move_buf[k].lba == SSD_INVALID_LBA ||
            f->move_buf[k].lba >= f->user_lbas) {
            continue;   /* 元数据异常，跳过 */
        }

        /*
         * 搬之前必须确认这一页还是最新的那一版。
         *
         * 读阶段是异步的，期间 host 完全可能又写了同一个 lba：
         * 我们手里这份是旧数据，而 L2P 已经指向新页了。
         * 若照搬不误，阶段 3 会把 L2P 改回旧位置 —— host 读到的值
         * 就静默退回上一个版本（实测 50 万请求里会错 2 个页）。
         * 旧的那页随块擦除一起消失即可，不必单独失效。
         */
        if (f->l2p[f->move_buf[k].lba] != f->move_src[k]) {
            continue;
        }
        if (!prog(f, k, ctx)) {
            dead++;
            continue;
        }
        f->move_slots[k].live  = 1u;
        f->move_slots[k].tries = 1u;
    }
    nand_run_until_idle(f->nand);

    /*
     * 阶段 2b：写失败的页换页重试。
     *
     * 这一步不是锦上添花 —— 编程失败是注入率 1% 量级的常见故障，
     * 一次回收要搬几百页，几乎必然碰上几页。若不重试就把 victim 擦了，
     * 这些页的数据就随块一起消失，host 读回来是错的
     *（实测 2000 次写里有 11 个 lba 校验失败，全部来自这里）。
     * 与 host 写路径一致：单次失败换页重来，连着失败才认命。
     */
    for (round = 0u; round + 1u < GC_MOVE_MAX_TRIES; round++) {
        uint32_t again = 0u;

        for (k = 0; k < n; k++) {
            if (!f->move_slots[k].live || f->move_slots[k].ok) {
                continue;   /* 已经成功，或者压根没提交 */
            }
            if (f->move_slots[k].tries >= GC_MOVE_MAX_TRIES) {
                continue;   /* 试够了，认命 */
            }
            if (!prog(f, k, ctx)) {
                continue;
            }
            f->move_slots[k].tries++;
            again++;
        }
        if (again == 0u) {
            break;   /* 没有可重试的了 */
        }
        nand_run_until_idle(f->nand);
    }

    /* 统计认命的那些页：它们的数据还在 src 里，调用方不能擦 src */
    for (k = 0; k < n; k++) {
        if (f->move_slots[k].live && !f->move_slots[k].ok) {
            dead++;
            SSD_ERR("gc: page ppn=%llu lost after %u tries",
                    (unsigned long long)f->move_src[k],
                    (unsigned)f->move_slots[k].tries);
        }
    }
    return dead;
}

/* --- 阶段 3：只有真正写成功、且仍是最新的页才更新映射，返回搬成的页数 ---
 * 写失败时不更新 L2P：旧位置还在，host 至少还能读到旧数据，
 * 比"映射指向一个没写进去的页"要好。 */
static uint32_t move_commit(ftl_dev_t *f, uint32_t n)
{
    nand_dev_t *nd = f->nand;
    uint32_t k;
    uint32_t moved = 0u;

    for (k = 0; k < n; k++) {
        /*
         * 只认"确实提交过写、且写成功了"的页。
         *
         * 光看 ok 不够：ok 在阶段 1 就被读回调置过 1，
         * 那些被跳过的页（元数据异常、期间被覆盖）带着 ok=1 走到这里，
         * 而它们的 move_dst 是本轮没写过的旧值 —— 照搬就会把映射指到
         * 别人家的物理页上（实测 lba=1391 -> ppn=0 holds lba=151880）。
         */
        if (!f->move_slots[k].ok || !f->move_slots[k].live) {
            continue;
        }
        if (f->move_buf[k].lba == SSD_INVALID_LBA ||
            f->move_buf[k].lba >= f->user_lbas) {
            continue;
        }
        if (f->l2p[f->move_buf[k].lba] != f->move_src[k]) {
            /* 写期间又被覆盖写了：刚写出去的这页已经是垃圾，
             * 留给下一轮 GC 回收，绝不能把映射指回旧版本 */
            continue;
        }
        f->l2p[f->move_buf[k].lba] = f->move_dst[k];
        nand_invalidate_page(nd, f->move_src[k]);
        moved++;
        ssd_stats_inc(ST_GC_COPY_PAGES);
    }
    return moved;
}

/*
 * 把一个块里的有效页搬进一个 destination 并更新 L2P。
 * 磨损均衡和坏块抢救用这条路径 —— 它们的 destination 有自己的讲究
 *（WL 要挑高 P/E 块），不适合条带化。
 */
int ftl_move_valid_pages(ftl_dev_t *f, pbn_t src, pbn_t *dest,
                         uint32_t *dest_wp, bool *reused_open,
                         uint32_t *copied, uint32_t *lost)
{
    move_dest_ctx_t c;
    uint32_t n;

    c.dest        = dest;
    c.dest_wp     = dest_wp;
    c.reused_open = reused_open;

    n = move_collect(f, src);
    if (lost != NULL) {
        *lost += move_write_phase(f, n, move_program_one, &c);
    } else {
        (void)move_write_phase(f, n, move_program_one, &c);
    }
    if (copied != NULL) {
        *copied += move_commit(f, n);
    } else {
        (void)move_commit(f, n);
    }
    return SSD_OK;
}

/*
 * GC 的搬移（S4）：有效页轮转写进每个 CE 的写入槽位，多颗 die 同时编程。
 *
 * 搬完之后槽位的写指针已经前移，host 接着往下写 —— 不需要"交还"目标块，
 * 因为目标块本来就是 host 的写入块，这也是它不会破坏空间守恒的原因。
 */
int ftl_move_striped(ftl_dev_t *f, pbn_t src, uint32_t *copied, uint32_t *lost)
{
    /* 从 host 正在写的槽位开始转：轮转一圈正好覆盖所有 CE */
    nand_dev_t *nd = f->nand;
    move_stripe_ctx_t c;
    uint32_t n;
    uint32_t dead;
    uint32_t moved;

    c.rr       = (f->n_open > 0u) ? (f->open_rr % f->n_open) : 0u;
    c.took     = 0u;
    c.max_take = 1u;   /* 单块回收：只借一块，池子必须净增 */

    /*
     * host 正在等的那个槽位若是空的/满的，优先给它补一块（占用本次唯一的额度）。
     *
     * 一举两得：host 回收完立刻能接着写，不必再触发一轮回收；
     * 搬移也能用满全部 CE —— 少一个槽位就少一颗 die 参与编程。
     */
    if (f->open_blocks[c.rr] == SSD_INVALID_PBN ||
        f->open_next[c.rr] >= nd->geo.pages_per_block) {
        pbn_t nb;

        if (f->open_blocks[c.rr] != SSD_INVALID_PBN) {
            nand_set_block_state(nd, f->open_blocks[c.rr],
                                 (uint16_t)NAND_BLK_CLOSED);
        }
        nb = ftl_take_gc_block_ce(f, c.rr);
        if (nb != SSD_INVALID_PBN) {
            ftl_open_set(f, c.rr, nb, 0u);
            c.took = 1u;
        } else {
            /* 池子空了：把槽位还原成"空着"，别让 host 以为这里有块 */
            f->open_blocks[c.rr] = SSD_INVALID_PBN;
            f->open_next[c.rr]   = 0u;
        }
    }

    n     = move_collect(f, src);
    dead  = move_write_phase(f, n, move_program_stripe, &c);
    moved = move_commit(f, n);

    if (copied != NULL) {
        *copied += moved;
    }
    if (lost != NULL) {
        *lost += dead;
    }
    return SSD_OK;
}

/*
 * S6：一次回收一整批（一个 superblock 的若干块）。
 *
 * 与单块回收的差别只在"批次有多大"：
 *   - 读阶段：所有 victim 的有效页一次性提交，只等一次落地。
 *     单块回收是收一块、等一次、再收下一块，N 次回收就有 N 段等待，
 *     每段结束时盘上只剩下最后几页在读 —— 并行度是被这么摊薄的。
 *   - 补块额度 = 本批块数：擦掉几块就最多借几块，池子净变化为 0。
 *     这是批量回收能不能成立的关键，单块回收时的"最多补一块"
 *     放到批量上会把回收活活饿死。
 */
static int ftl_move_batch(ftl_dev_t *f, const pbn_t *victims, uint32_t nv,
                          uint32_t *copied, uint32_t *lost)
{
    nand_dev_t *nd = f->nand;
    move_stripe_ctx_t c;
    uint32_t n = 0u;
    uint32_t i;
    uint32_t dead;
    uint32_t moved;

    c.rr       = (f->n_open > 0u) ? (f->open_rr % f->n_open) : 0u;
    c.took     = 0u;
    c.max_take = (nv > 0u) ? nv : 1u;

    /* host 正在等的那个槽位空着/满了，先给它补一块（占用一个额度） */
    if (f->open_blocks[c.rr] == SSD_INVALID_PBN ||
        f->open_next[c.rr] >= nd->geo.pages_per_block) {
        pbn_t nb2;

        if (f->open_blocks[c.rr] != SSD_INVALID_PBN) {
            nand_set_block_state(nd, f->open_blocks[c.rr],
                                 (uint16_t)NAND_BLK_CLOSED);
        }
        nb2 = ftl_take_gc_block_ce(f, c.rr);
        if (nb2 != SSD_INVALID_PBN) {
            ftl_open_set(f, c.rr, nb2, 0u);
            f->blk_sb[nb2] = ++f->sb_seq;
            c.took = 1u;
        } else {
            f->open_blocks[c.rr] = SSD_INVALID_PBN;
            f->open_next[c.rr]   = 0u;
        }
    }

    for (i = 0; i < nv; i++) {
        n = move_collect_into(f, victims[i], n);
    }
    nand_run_until_idle(nd);   /* 整批一起等：读阶段才吃得满多 CE */

    dead  = move_write_phase(f, n, move_program_stripe, &c);
    moved = move_commit(f, n);

    if (copied != NULL) {
        *copied += moved;
    }
    if (lost != NULL) {
        *lost += dead;
    }
    return SSD_OK;
}

/* 擦除一批 victim 并归还（擦除失败即运行时坏块，数据已搬走，不丢） */
static void gc_erase_and_return(ftl_dev_t *f, const pbn_t *victims, uint32_t nv)
{
    nand_dev_t *nd = f->nand;
    uint32_t i;

    for (i = 0; i < nv; i++) {
        if (nand_erase_sync(nd, victims[i]) == SSD_OK) {
            ftl_return_block(f, victims[i]);
        } else {
            SSD_WARN("gc: erase failed on pbn=%llu, dropped",
                     (unsigned long long)victims[i]);
            nand_set_block_state(nd, victims[i], (uint16_t)NAND_BLK_BAD);
            f->bad_blocks++;
            ssd_stats_inc(ST_BADBLOCK_RUNTIME);
        }
        ssd_stats_inc(ST_GC_ERASES);
    }
}

/* 静态磨损均衡的触发检查：每 16 次回收才看一次，避免扫描开销影响主路径 */
static void gc_maybe_wl(ftl_dev_t *f)
{
    ftl_pe_stat_t pe;

    if (f->wl_enable == 0u || (f->gc_count % 16u) != 0u) {
        return;
    }
    ftl_pe_stats(f, &pe);
    if (pe.max_pe > pe.min_pe + f->wl_pe_thresh) {
        (void)ftl_wl_static_once(f);
    }
}

int ftl_gc_one(ftl_dev_t *f)
{
    uint32_t nv;
    pbn_t victim;
    uint32_t copied = 0u;
    uint32_t lost = 0u;
    int rc;

    if (f == NULL || !f->valid) {
        return SSD_ERR_INVAL;
    }

    /*
     * S6：一次收一整批 —— 但只在后台 GC 时这么做。
     *
     * 实测教训：前台也批量回收的话，一次回收的停顿会变成 N 倍
     *（20 万请求下 max 从 322ms 涨到 933ms、p99.9 从 119ms 涨到 140ms），
     * 而 WAF 与并行度一点没变好 —— 因为搬移本来就条带化写进所有 CE 了，
     * 批量带来的只是"同一段时间里干更多的活"，正好撞在 host 在等的那一刻。
     *
     * 前台 GC 的目标是"尽快让 host 能接着写"，所以一次只收一块；
     * 后台 GC 发生在 host 的空闲时段，没人等，那时才该一次收一整批，
     * 把空闲时间换成更多可用块。
     */
    if (f->striping != 0u && f->gc_background) {
        uint32_t nb = gc_pick_victims_batch(f, f->gc_victims, f->n_open);

        if (nb > 1u) {
            uint32_t bcopied = 0u;
            uint32_t blost   = 0u;

            (void)ftl_move_batch(f, f->gc_victims, nb, &bcopied, &blost);
            if (blost != 0u) {
                /*
                 * 这批里有页没搬出去，数据还在 victim 里 ——
                 * 一块都不能擦。已经搬走的那些页会在下一轮再搬一次，
                 * 代价是重复搬移，换来的是不丢数据。
                 */
                SSD_ERR("gc: abort batch of %u, %u page(s) still inside",
                        (unsigned)nb, (unsigned)blost);
                return SSD_ERR_IO;
            }

            gc_erase_and_return(f, f->gc_victims, nb);
            f->gc_count += nb;
            if (f->gc_background) {
                f->bg_gc_count += nb;
            }
            f->gc_copied += bcopied;
            gc_maybe_wl(f);
            return SSD_OK;
        }
    }

    /*
     * 单块回收：有效页仍然条带化写进所有 CE（ftl_move_striped）。
     *
     * 为什么必须条带：整块的页写进同一块，等于在同一颗 die 上排队编程，
     * 一次回收 ~390ms，盘上其余 3 个 CE 全程围观 —— 实测 100 万请求里
     * 83% 的虚拟时间都耗在这段串行编程上，p99.9 也是被它顶到 350ms。
     *
     * （S4 这里原本记着"一次收 N 块会把池子抽干，得先做 superblock 再谈"，
     *   S6 补上了 superblock 与"借 N 块还 N 块"的额度，批量回收已可用。）
     */
    nv = gc_pick_victims(f, f->gc_victims, 1u);
    if (nv == 0u) {
        return SSD_ERR_NO_SPACE;   /* 没有任何可回收的块 */
    }
    victim = f->gc_victims[0];

    rc = ftl_move_striped(f, victim, &copied, &lost);
    if (rc != SSD_OK) {
        return rc;
    }
    if (lost != 0u) {
        /*
         * 有页没能搬出去（重试到头仍然编程失败）。
         * 它的数据还在 victim 里 —— 这时候擦 victim 就是实打实的丢数据，
         * host 下一次读会拿到空页/旧页。宁可这次回收白做，
         * 也不能用"腾出空间"换"数据没了"。
         */
        SSD_ERR("gc: abort, %u page(s) still in pbn=%llu",
                (unsigned)lost, (unsigned long long)victim);
        return SSD_ERR_IO;
    }

    /* 擦除 victim 并归还 */
    {
        pbn_t one[1];

        one[0] = victim;
        gc_erase_and_return(f, one, 1u);
    }

    f->gc_count++;
    if (f->gc_background) {
        f->bg_gc_count++;
    }
    f->gc_copied += copied;
    gc_maybe_wl(f);
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
     * 提前干活的好处是 host 写时不必被迫等 GC，延迟更平稳。
     *
     * gc_background 必须在这里置上：S6 的批量回收只对后台生效
     *（前台一次收 N 块会把单次停顿拉长，实测见上面的说明），
     * 而"这次回收算前台还是后台"就是靠这个标记区分的。 */
    f->gc_background = true;
    while (f->free_top < f->bg_target) {
        if (ftl_gc_one(f) != SSD_OK) {
            break;
        }
        if (++guard > f->free_cap) {
            break;   /* 转了一圈还没补上，盘上确实没垃圾可收了 */
        }
    }
    f->gc_background = false;
    return SSD_OK;
}
