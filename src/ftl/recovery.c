/*
 * S5：上电重建（SPOR, Sudden Power-Off Recovery）。
 *
 * 掉电之后 FTL 手里什么都不剩：映射表、空闲块池、写入块、有效页计数、
 * 写序号——全在 DRAM 里，一断电就没了。唯一还站着的是介质本身：
 * 每个已编程页的 spare 里写着 (lba, seq)。
 *
 * 重建做的事，就是把 DRAM 里那套状态从介质上重新推出来：
 *
 *   1. 逐块逐页扫 spare，把每个 lba 的"最新版本"找出来 -> 映射表
 *   2. 版本判定只能靠 seq，不能靠页的 INVALID 标记
 *   3. 顺带把写指针、有效/失效页数、块状态算回来 -> 块表
 *   4. 空闲块池、写入槽位重新组织
 *   5. 写序号从介质上恢复，保证它继续单调递增
 *
 * 第 2 点值得展开：覆盖写是"新页写进去之后才让旧页失效"，
 * 而失效这个动作只改 DRAM，不会去动旧页的 spare
 *（要改就得先擦整块，真实盘不可能这么干）。
 * 于是掉电后新旧两页在介质上看起来都正常，都写着同一个 lba ——
 * 谁新谁旧只有 seq 知道。把 INVALID 标记当成"版本依据"是错的：
 * 它会让"新页撕裂、旧页还在"这种情形直接丢数据。
 */

#include "ftl/ftl.h"

#include "core/assert.h"
#include "core/bitmap.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/geometry.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 页能否参与重建                                                      */
/* ------------------------------------------------------------------ */

/*
 * 一页要能被映射回来，必须同时满足：
 *   数据是完整写进去的（不是撕裂页）
 *   spare 里的 lba 落在用户地址空间内
 *   元数据 CRC 自洽（否则这一页的身份本身就不可信）
 *   这个 lba 没有被 TRIM 掉
 *
 * 注意这里**不看** INVALID 标记 —— 见文件头的说明。
 */
static bool page_usable(const nand_dev_t *nd, const nand_page_meta_t *m,
                        lba_t user_lbas)
{
    if (m->lba == SSD_INVALID_LBA || m->lba >= user_lbas) {
        return false;
    }
    if (nand_meta_crc(m->lba, m->seq) != m->crc) {
        return false;   /* 元数据本身坏了，这一页的身份不可信 */
    }
    if (nand_nv_trim_get(nd, m->lba)) {
        return false;   /* host 已经删掉了，不能让它死而复生 */
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 第一遍：选出每个 lba 的最新版本                                      */
/* ------------------------------------------------------------------ */

static void scan_block_pick(ftl_dev_t *f, pbn_t pbn, uint32_t ppb,
                            uint64_t *torn_pages, uint64_t *orphan_pages,
                            uint64_t *max_seq, uint64_t *scanned_pages)
{
    nand_dev_t *nd = f->nand;
    ppn_t base = nand_geo_block_base(&nd->geo, pbn);
    uint32_t i;

    for (i = 0; i < ppb; i++) {
        ppn_t ppn = base + i;
        const nand_page_meta_t *m = &nd->pages[ppn];

        (*scanned_pages)++;

        if (m->state == (uint16_t)NAND_PAGE_FREE) {
            continue;
        }
        /* 撕裂页：半写的内容一律不采信，但它仍占着这个位置 */
        if (m->state == (uint16_t)NAND_PAGE_CORRUPT ||
            m->state == (uint16_t)NAND_PAGE_PENDING) {
            (*torn_pages)++;
            continue;
        }
        if (m->seq > *max_seq) {
            *max_seq = (uint64_t)m->seq;
        }
        if (!page_usable(nd, m, f->user_lbas)) {
            (*orphan_pages)++;
            continue;
        }

        /*
         * 同 lba 的多个副本：seq 大的那个最新。
         *
         * seq 相同只出现在 GC 搬移（搬移保留原 seq，src 与 dst 内容一致），
         * 此时选谁都不影响数据正确性，但必须**确定**:
         * 若两次上电选出不同的页，盘的行为就不可复现了。
         * 这里按物理页号大的取胜（GC 的目的地通常是后分配的块）。
         */
        {
            ppn_t cur = f->l2p[m->lba];

            if (cur == SSD_INVALID_PPN) {
                f->l2p[m->lba] = ppn;
            } else if (m->seq > nd->pages[cur].seq ||
                       (m->seq == nd->pages[cur].seq && ppn > cur)) {
                f->l2p[m->lba] = ppn;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 第二遍：定下每页的生死，并把块表算回来                                */
/* ------------------------------------------------------------------ */

/*
 * 块内页是顺序编程的，所以写指针 = 最后一个非空闲页之后。
 * 一旦中间出现撕裂页，这个块就不能再往下写了 ——
 * 真实 NAND 不允许跳页编程，而且撕裂页后面那些页的电荷状态也不可信。
 * 固件只能把它关掉，剩下的空间留给 GC。
 */
static void scan_block_commit(ftl_dev_t *f, pbn_t pbn, uint32_t ppb,
                              uint64_t *stale_pages, uint64_t *mapped_lbas)
{
    nand_dev_t *nd = f->nand;
    ppn_t base = nand_geo_block_base(&nd->geo, pbn);
    uint32_t i;
    uint32_t valid = 0u;
    uint32_t invalid = 0u;
    uint32_t next_page = 0u;
    bool torn = false;
    uint16_t blk_state;

    for (i = 0; i < ppb; i++) {
        ppn_t ppn = base + i;
        const nand_page_meta_t *m = &nd->pages[ppn];

        if (m->state == (uint16_t)NAND_PAGE_FREE) {
            continue;
        }
        if (m->state == (uint16_t)NAND_PAGE_CORRUPT ||
            m->state == (uint16_t)NAND_PAGE_PENDING) {
            torn = true;
            next_page = i + 1u;
            /* 撕裂页判成垃圾：它占着位置，但里面的东西不能用 */
            nand_page_set_state(nd, ppn, (uint16_t)NAND_PAGE_INVALID);
            invalid++;
            continue;
        }
        next_page = i + 1u;

        if (page_usable(nd, m, f->user_lbas) && f->l2p[m->lba] == ppn) {
            nand_page_set_state(nd, ppn, (uint16_t)NAND_PAGE_VALID);
            valid++;
            (*mapped_lbas)++;
        } else {
            /* 落选的旧版本 / 越界 / TRIM 过：都是垃圾，等 GC 收走 */
            nand_page_set_state(nd, ppn, (uint16_t)NAND_PAGE_INVALID);
            invalid++;
            (*stale_pages)++;
        }
    }

    if (torn || next_page >= ppb) {
        /*
         * 有撕裂页的块即使没写满也必须关闭。
         * 这不是保守过头：继续往里写等于把 host 数据放在一颗
         * 刚被断电打断过的 die 上，那批页的可靠性没有任何保证。
         */
        next_page = ppb;
        blk_state = (uint16_t)NAND_BLK_CLOSED;
    } else if (next_page == 0u) {
        blk_state = (uint16_t)NAND_BLK_FREE;
    } else {
        blk_state = (uint16_t)NAND_BLK_OPEN;
    }

    nand_block_rebuild(nd, pbn, blk_state, next_page, valid, invalid,
                       (valid > 0u) ? ssd_clock_now() : 0u);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

/*
 * 快照里的映射可能"指空"（S6）。
 *
 * 检查点是在异步写还飞着的时候拍下的：那一刻 L2P 已经指向那些页，
 * 但页在介质上还是 PENDING。掉电之后它们变成撕裂页（CORRUPT）；
 * 另一种情形是快照里记着"这块有数据"，而它在快照之后被 GC 擦掉了。
 *
 * 这些映射一律作废。语义上完全站得住：指向撕裂页的那一笔写，
 * host 根本没收到完成回执，掉电丢掉是合法的；
 * 真正不能丢的是已经 ack 过的写，而它们要么已落在介质上、
 * 要么身处活跃块、会在扫描里被如实找回来。
 */
static uint64_t cp_drop_dangling(ftl_dev_t *f)
{
    nand_dev_t *nd = f->nand;
    lba_t lba;
    uint64_t dropped = 0u;

    for (lba = 0; lba < f->user_lbas; lba++) {
        ppn_t ppn = f->l2p[lba];
        pbn_t pbn;

        if (ppn == SSD_INVALID_PPN) {
            continue;
        }
        /*
         * TRIM 过的地址必须重新断开。
         * 全盘重建时 page_usable() 会挡住它们，但快照是"整份照搬"的，
         * 没有经过那道过滤 —— 不补这一刀，host 已经删掉的数据会死而复生。
         */
        if (nand_nv_trim_get(nd, lba)) {
            f->l2p[lba] = SSD_INVALID_PPN;
            dropped++;
            continue;
        }

        /* 只审"这次扫过"的块：没扫的块与快照一致，无从也无需怀疑 */
        pbn = nand_geo_pbn_of_ppn(&nd->geo, ppn);
        if (nand_nv_active_get(nd, pbn) &&
            nd->pages[ppn].state != (uint16_t)NAND_PAGE_VALID) {
            f->l2p[lba] = SSD_INVALID_PPN;
            dropped++;
        }
    }
    return dropped;
}

int ftl_recover(ftl_dev_t *f, ftl_recovery_t *out)
{
    nand_dev_t *nd;
    uint32_t ppb;
    pbn_t pbn;
    lba_t lba;
    uint32_t i;
    uint32_t kept = 0u;
    uint64_t max_seq = 0u;
    uint64_t bad = 0u;
    uint64_t scan_ns;
    uint64_t cp_seq = 0u;
    bool     use_cp = false;
    ftl_recovery_t rec;

    if (f == NULL || !f->valid || f->nand == NULL) {
        return SSD_ERR_INVAL;
    }
    nd  = f->nand;
    ppb = nd->geo.pages_per_block;
    memset(&rec, 0, sizeof(rec));

    /*
     * ftl_init 是按"新盘"填的：映射全空、所有非坏块都算空闲、
     * 写入槽位全空。这些默认值在恢复场景下全是错的，先推倒。
     */
    for (lba = 0; lba < f->user_lbas; lba++) {
        f->l2p[lba] = SSD_INVALID_PPN;
    }
    for (i = 0; i < f->n_open; i++) {
        f->open_blocks[i] = SSD_INVALID_PBN;
        f->open_next[i]   = 0u;
    }
    f->open_rr = 0u;

    /*
     * --- S6：先试快照 ---
     *
     * 有快照就把映射与块表整体读回来，只补扫"快照之后动过"的块。
     * 没有（首次上电 / 检查点被关掉 / 几何变了）就退回 S5 的全盘重建。
     */
    if (f->cp_interval != 0u && nand_cp_is_valid(nd)) {
        use_cp = nand_cp_load(nd, &cp_seq, f->l2p, f->user_lbas, NULL, NULL, 0u);
    }
    if (use_cp) {
        rec.from_checkpoint = 1u;
        rec.cp_load_ns      = nand_cp_load_time_ns(nd);
    } else {
        nand_cp_invalidate(nd);   /* 这份快照用不了，别让下次上电还信它 */
    }

    /* --- 第一遍：选版本 --- */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
            bad++;
            continue;
        }
        /* 快照里没动过的块：介质上的样子与快照一致，不必读 */
        if (use_cp && !nand_nv_active_get(nd, pbn)) {
            continue;
        }
        rec.scanned_blocks++;
        scan_block_pick(f, pbn, ppb, &rec.torn_pages, &rec.orphan_pages,
                        &max_seq, &rec.scanned_pages);
    }

    /* --- 第二遍：定生死 + 重建块表 --- */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        if (nand_block_state(nd, pbn) == (uint16_t)NAND_BLK_BAD) {
            continue;
        }
        if (use_cp && !nand_nv_active_get(nd, pbn)) {
            continue;
        }
        scan_block_commit(f, pbn, ppb, &rec.stale_pages, &rec.mapped_lbas);
    }

    /* --- S6：作废快照里那些指空的映射 --- */
    if (use_cp) {
        rec.cp_lbas_fixed = cp_drop_dangling(f);
    }

    /*
     * --- 写入槽位 ---
     *
     * 半满的块是"掉电前正在写"的，恢复后应当接着往下写。
     * 但每个 CE 只有一个槽位，所以同 CE 上多出来的半满块只能关闭等回收 ——
     * 剩余空间会浪费一点，这比把一块半满的块混进空闲池要安全得多
     *（池里的块必须是干净的，否则新数据会写进别人的旧页上）。
     */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        uint32_t next;
        uint32_t ce;

        if (nand_block_state(nd, pbn) != (uint16_t)NAND_BLK_OPEN) {
            continue;
        }
        next = nand_block_next_page(nd, pbn);
        if (next == 0u || next >= ppb) {
            continue;
        }
        ce = nand_geo_ce_index_of_pbn(&nd->geo, pbn);
        if (ce >= f->n_open) {
            ce = ce % f->n_open;
        }
        if (f->open_blocks[ce] != SSD_INVALID_PBN) {
            nand_set_block_state(nd, pbn, (uint16_t)NAND_BLK_CLOSED);
            continue;
        }
        ftl_open_set(f, ce, pbn, next);
        rec.open_blocks++;
    }

    /* --- 空闲块池：只收真正干净的块 --- */
    for (i = 0; i < f->free_top; i++) {
        pbn_t p = f->free_stack[i];

        if (nand_block_state(nd, p) == (uint16_t)NAND_BLK_FREE) {
            f->free_stack[kept++] = p;
        }
    }
    f->free_top = kept;
    rec.free_blocks = kept;
    rec.bad_blocks  = bad;

    /*
     * --- 写序号 ---
     *
     * 块年龄（cost-benefit 判冷热）是以这个计数为基线算的，
     * 所以它必须比盘上任何一页都新，且不能因为掉电而倒退。
     * 只取页面里的 max seq 是不够的：盘被 TRIM + GC 清空后 max 归零，
     * 基线就塌了 —— 上电后所有块都会被当成"很久没变脏"，
     * 于是一口气回收一批其实刚写过的块。
     * 所以还要并上 system 区里持久化的那个序号。
     */
    {
        uint64_t persisted = nand_persistent_seq(nd);
        uint64_t base = max_seq;

        if (cp_seq > base) {
            base = cp_seq;   /* 快照序号：没扫到的块里可能有更晚的写 */
        }
        if (persisted > base) {
            base = persisted;
        }
        f->write_seq = base + 1u;
    }
    rec.write_seq = f->write_seq;

    /* 冷热信息（cost-benefit 用）随掉电一起没了：一律当作"刚变脏"，
     * 免得一上电就因为 age 虚高把一批块当成冷数据狂回收。 */
    for (pbn = 0; pbn < nd->geo.total_blocks; pbn++) {
        f->blk_invalid_seq[pbn] = (uint32_t)f->write_seq;
    }

    /* 坏块：介质上看到的减去出厂的，就是运行期新增的 */
    f->bad_blocks = (bad > (uint64_t)nd->factory_bad_blocks)
                    ? (uint32_t)(bad - (uint64_t)nd->factory_bad_blocks)
                    : 0u;

    /*
     * 上电开销：读快照 + 扫（活跃）块，都是实打实要读介质的，
     * 一并记进虚拟时钟 —— 但不计入 host IOPS。
     */
    scan_ns = use_cp ? (rec.cp_load_ns +
                        nand_scan_blocks_time_ns(nd, rec.scanned_blocks))
                     : nand_scan_time_ns(nd);
    ssd_clock_advance(scan_ns);
    rec.scan_ns = scan_ns;

    ssd_stats_inc(ST_SPOR_RECOVERIES);
    ssd_stats_add(ST_SPOR_TORN_PAGES, (uint64_t)rec.torn_pages);
    ssd_stats_add(ST_SPOR_STALE_PAGES, (uint64_t)rec.stale_pages);

    SSD_INFO("ftl recovered: lbas=%llu free=%u open=%llu bad=%llu "
             "torn=%llu stale=%llu write_seq=%llu scan=%.3f ms (%s, %llu blocks)",
             (unsigned long long)rec.mapped_lbas, f->free_top,
             (unsigned long long)rec.open_blocks,
             (unsigned long long)rec.bad_blocks,
             (unsigned long long)rec.torn_pages,
             (unsigned long long)rec.stale_pages,
             (unsigned long long)rec.write_seq,
             (double)scan_ns / 1e6,
             use_cp ? "checkpoint" : "full scan",
             (unsigned long long)rec.scanned_blocks);

    if (out != NULL) {
        *out = rec;
    }
    return SSD_OK;
}
