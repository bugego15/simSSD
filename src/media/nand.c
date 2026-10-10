#include "media/nand.h"

#include "core/assert.h"
#include "core/bitmap.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/stats.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 元数据校验：FNV-1a 折叠成 16bit                                     */
/* ------------------------------------------------------------------ */

#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static uint32_t fnv_update(uint32_t h, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

uint16_t nand_meta_crc(lba_t lba, uint32_t seq)
{
    uint32_t h = FNV_OFFSET;

    h = fnv_update(h, &lba, sizeof(lba));
    h = fnv_update(h, &seq, sizeof(seq));
    return (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
}

void nand_meta_seal(nand_page_meta_t *m)
{
    if (m == NULL) {
        return;
    }
    m->crc = nand_meta_crc(m->lba, m->seq);
}

/* ------------------------------------------------------------------ */
/* op 池                                                               */
/* ------------------------------------------------------------------ */

static nand_op_t *nand_op_alloc(nand_dev_t *dev)
{
    uint32_t i;

    for (i = 0; i < NAND_MAX_PENDING_OPS; i++) {
        if (dev->ops[i].in_use == 0u) {
            memset(&dev->ops[i], 0, sizeof(nand_op_t));
            dev->ops[i].in_use = 1u;
            dev->ops[i].dev    = dev;
            return &dev->ops[i];
        }
    }
    return NULL;
}

static void nand_op_release(nand_op_t *op)
{
    op->in_use = 0u;
}

/* ------------------------------------------------------------------ */
/* 操作完成：真正的"状态落地"发生在这里                                 */
/* ------------------------------------------------------------------ */

static void nand_op_complete(void *arg)
{
    nand_op_t *op = (nand_op_t *)arg;
    nand_dev_t *dev = op->dev;
    nand_block_info_t *bi;
    nand_page_meta_t *meta;
    pbn_t pbn;
    int status = SSD_OK;

    pbn = nand_geo_pbn_of_ppn(&dev->geo, op->ppn);
    bi  = &dev->blocks[pbn];

    /* 提交时加的 pending 在这里销账（无论成功还是失败） */
    if (op->type == NAND_OP_PROG && bi->pending_pages > 0u) {
        bi->pending_pages--;
    }

    switch (op->type) {
    case NAND_OP_PROG:
        meta = &dev->pages[op->ppn];
        if (dev->fault_inject != 0u &&
            ssd_rng_chance_permille(&dev->rng, dev->fault_rate_permille)) {
            /* 编程失败：该页作废，只能随 block 一起擦除 */
            status = SSD_ERR_IO;
            meta->state = (uint16_t)NAND_PAGE_INVALID;
            bi->invalid_pages++;
            SSD_WARN("program failed: ppn=%llu", (unsigned long long)op->ppn);
        } else {
            *meta = op->meta;
            meta->state = (uint16_t)NAND_PAGE_VALID;
            nand_meta_seal(meta);
            bi->valid_pages++;
            bi->last_prog_ns = ssd_clock_now();
        }
        ssd_stats_inc(ST_NAND_PROG_PAGES);
        ssd_stats_add(ST_MEDIA_BUSY_NS, dev->t_prog);
        break;

    case NAND_OP_READ:
        meta = &dev->pages[op->ppn];
        /* 只有 fault_inject=2 才注入不可纠的读错误（UECC 必然丢数据）。
         * fault_inject=1 只注入 program/erase 故障 —— 那是固件必须能自愈的
         * 路径（重映射 + 坏块替换），用它来验收"坏块出现后不丢数据"。 */
        if (dev->fault_inject == 2u &&
            ssd_rng_chance_permille(&dev->rng, dev->fault_rate_permille)) {
            status = SSD_ERR_UECC;
        } else if (meta->state != (uint16_t)NAND_PAGE_FREE) {
            /* 已写过的页必须能通过 CRC 自检，否则说明元数据被写坏了 */
            if (nand_meta_crc(meta->lba, meta->seq) != meta->crc) {
                status = SSD_ERR_UECC;
                SSD_ERR("meta crc mismatch: ppn=%llu", (unsigned long long)op->ppn);
            }
        }
        if (op->out != NULL) {
            *op->out = *meta;
        }
        ssd_stats_inc(ST_NAND_READ_PAGES);
        ssd_stats_add(ST_MEDIA_BUSY_NS, dev->t_read);
        break;

    case NAND_OP_ERASE:
        {
            ppn_t base = nand_geo_block_base(&dev->geo, pbn);
            uint32_t i;

            bi->erase_count++;
            for (i = 0; i < dev->geo.pages_per_block; i++) {
                nand_page_meta_t *m = &dev->pages[base + i];
                m->lba   = SSD_INVALID_LBA;
                m->seq   = 0u;
                m->state = (uint16_t)NAND_PAGE_FREE;
                m->crc   = 0u;
            }
            bi->state         = (uint16_t)NAND_BLK_FREE;
            bi->next_page     = 0u;
            bi->valid_pages   = 0u;
            bi->invalid_pages = 0u;
            bi->pending_pages = 0u;
            bi->last_prog_ns    = 0u;
            bi->last_invalid_ns = 0u;

            /* P/E 次数越界后，擦除失败概率上升 -> 运行时坏块 */
            if (dev->fault_inject != 0u &&
                bi->erase_count >= dev->pe_limit &&
                ssd_rng_chance_permille(&dev->rng, dev->fault_rate_permille)) {
                bi->state = (uint16_t)NAND_BLK_BAD;
                status = SSD_ERR_BADBLK;
                ssd_stats_inc(ST_BADBLOCK_RUNTIME);
                SSD_WARN("runtime bad block: pbn=%llu pe=%u",
                         (unsigned long long)pbn, bi->erase_count);
            }
        }
        ssd_stats_inc(ST_NAND_ERASE_BLOCKS);
        ssd_stats_add(ST_MEDIA_BUSY_NS, dev->t_bers);
        break;

    default:
        SSD_BUG("unknown nand op type");
        break;
    }

    op->status = status;
    if (op->cb != NULL) {
        op->cb(op->arg, status);
    }
    nand_op_release(op);
}

/* ------------------------------------------------------------------ */
/* 通道占用                                                            */
/* ------------------------------------------------------------------ */

/*
 * 介质操作排程（S4 并发模型的核心）。
 *
 *   通道只在传输那一段被独占（xfer_ns），传完立刻释放去服务别的 CE；
 *   CE 则从传输开始一直忙到介质操作结束（xfer_ns + media_ns）。
 *
 *   开始时刻 = max(现在, 通道可用时刻, CE 可用时刻)
 *   完成时刻 = 开始 + 传输 + 介质操作
 *
 * 同一个 CE 上的操作被 free_at 串化，不同 CE 之间互不阻塞 ——
 * 多 CE 之所以能换来接近线性的 IOPS 提升，根源就在这里。
 */
static uint64_t nand_schedule(nand_dev_t *dev, uint32_t ch, uint32_t ce,
                              uint32_t media_ns, uint32_t xfer_ns)
{
    uint64_t        now   = ssd_clock_now();
    uint64_t        start = now;
    nand_channel_t *c     = &dev->channels[ch];
    nand_ce_t      *e     = &dev->ces[ce];

    if (c->free_at > start) {
        start = c->free_at;
    }
    if (e->free_at > start) {
        start = e->free_at;
    }

    /* 通道：只占用传输那一段 */
    c->free_at  = start + xfer_ns;
    c->busy_ns += xfer_ns;

    /* CE：从传输开始忙到介质操作结束 */
    e->free_at  = start + xfer_ns + media_ns;
    e->busy_ns += xfer_ns + media_ns;

    return start;
}

/* ------------------------------------------------------------------ */
/* 异步接口                                                            */
/* ------------------------------------------------------------------ */

int nand_prog_async(nand_dev_t *dev, ppn_t ppn, const nand_page_meta_t *meta,
                    nand_done_cb cb, void *arg)
{
    nand_block_info_t *bi;
    nand_op_t *op;
    pbn_t pbn;
    uint32_t ch;
    uint32_t ce;
    uint64_t start;
    uint64_t done;

    if (dev == NULL || meta == NULL) {
        return SSD_ERR_INVAL;
    }
    if (ppn >= dev->geo.total_pages) {
        return SSD_ERR_INVAL;
    }

    pbn = nand_geo_pbn_of_ppn(&dev->geo, ppn);
    bi  = &dev->blocks[pbn];
    if (bi->state == (uint16_t)NAND_BLK_BAD) {
        return SSD_ERR_BADBLK;
    }
    if (dev->pages[ppn].state != (uint16_t)NAND_PAGE_FREE) {
        /* NAND 不允许原地改写：必须先擦除 */
        return SSD_ERR_IO;
    }

    op = nand_op_alloc(dev);
    if (op == NULL) {
        return SSD_ERR_NO_SPACE;
    }
    op->type = NAND_OP_PROG;
    op->ppn  = ppn;
    op->meta = *meta;
    op->cb   = cb;
    op->arg  = arg;

    /* 提交即占位，避免同一页被重复提交。
     * pending 计数同时告诉 GC：这块上还有操作没落地，先别动它。 */
    dev->pages[ppn].state = (uint16_t)NAND_PAGE_PENDING;
    bi->pending_pages++;

    ch    = nand_geo_channel_of_ppn(&dev->geo, ppn);
    ce    = nand_geo_ce_index_of_ppn(&dev->geo, ppn);
    start = nand_schedule(dev, ch, ce, dev->t_prog, dev->t_xfer);
    done  = start + dev->t_xfer + dev->t_prog;

    if (ssd_evtq_push(&dev->evtq, done, nand_op_complete, op) != SSD_OK) {
        dev->pages[ppn].state = (uint16_t)NAND_PAGE_FREE;
        nand_op_release(op);
        return SSD_ERR_NO_SPACE;
    }
    return SSD_OK;
}

int nand_read_async(nand_dev_t *dev, ppn_t ppn, nand_page_meta_t *out,
                    nand_done_cb cb, void *arg)
{
    nand_op_t *op;
    uint32_t ch;
    uint32_t ce;
    uint64_t start;
    uint64_t done;

    if (dev == NULL) {
        return SSD_ERR_INVAL;
    }
    if (ppn >= dev->geo.total_pages) {
        return SSD_ERR_INVAL;
    }
    if (dev->pages[ppn].state == (uint16_t)NAND_PAGE_PENDING) {
        return SSD_ERR_BUSY;
    }

    op = nand_op_alloc(dev);
    if (op == NULL) {
        return SSD_ERR_NO_SPACE;
    }
    op->type = NAND_OP_READ;
    op->ppn  = ppn;
    op->out  = out;
    op->cb   = cb;
    op->arg  = arg;

    ch    = nand_geo_channel_of_ppn(&dev->geo, ppn);
    ce    = nand_geo_ce_index_of_ppn(&dev->geo, ppn);
    start = nand_schedule(dev, ch, ce, dev->t_read, dev->t_xfer);
    done  = start + dev->t_xfer + dev->t_read;

    if (ssd_evtq_push(&dev->evtq, done, nand_op_complete, op) != SSD_OK) {
        nand_op_release(op);
        return SSD_ERR_NO_SPACE;
    }
    return SSD_OK;
}

int nand_erase_async(nand_dev_t *dev, pbn_t pbn, nand_done_cb cb, void *arg)
{
    nand_op_t *op;
    uint32_t ch;
    uint32_t ce;
    uint64_t start;
    uint64_t done;

    if (dev == NULL) {
        return SSD_ERR_INVAL;
    }
    if (pbn >= dev->geo.total_blocks) {
        return SSD_ERR_INVAL;
    }
    if (dev->blocks[pbn].state == (uint16_t)NAND_BLK_BAD) {
        return SSD_ERR_BADBLK;
    }

    op = nand_op_alloc(dev);
    if (op == NULL) {
        return SSD_ERR_NO_SPACE;
    }
    op->type = NAND_OP_ERASE;
    op->ppn  = nand_geo_block_base(&dev->geo, pbn);
    op->cb   = cb;
    op->arg  = arg;

    /* 擦除命令不在通道上搬数据（xfer=0），但要独占该 CE 直到擦完 */
    ch    = nand_geo_channel_of_pbn(&dev->geo, pbn);
    ce    = nand_geo_ce_index_of_pbn(&dev->geo, pbn);
    start = nand_schedule(dev, ch, ce, dev->t_bers, 0u);
    done  = start + dev->t_bers;

    if (ssd_evtq_push(&dev->evtq, done, nand_op_complete, op) != SSD_OK) {
        nand_op_release(op);
        return SSD_ERR_NO_SPACE;
    }
    return SSD_OK;
}

/* ------------------------------------------------------------------ */
/* 同步封装                                                            */
/* ------------------------------------------------------------------ */

typedef struct nand_sync_ctx {
    int done;
    int status;
} nand_sync_ctx_t;

static void nand_sync_cb(void *arg, int status)
{
    nand_sync_ctx_t *ctx = (nand_sync_ctx_t *)arg;

    ctx->done   = 1;
    ctx->status = status;
}

static int nand_wait(nand_dev_t *dev, nand_sync_ctx_t *ctx)
{
    while (ctx->done == 0) {
        if (nand_step(dev) != SSD_OK) {
            /* 事件队列已空但操作仍未完成，属于模型内部错误 */
            return SSD_ERR_IO;
        }
    }
    return ctx->status;
}

int nand_prog_sync(nand_dev_t *dev, ppn_t ppn, const nand_page_meta_t *meta)
{
    nand_sync_ctx_t ctx;
    int rc;

    ctx.done   = 0;
    ctx.status = SSD_OK;
    rc = nand_prog_async(dev, ppn, meta, nand_sync_cb, &ctx);
    if (rc != SSD_OK) {
        return rc;
    }
    return nand_wait(dev, &ctx);
}

int nand_read_sync(nand_dev_t *dev, ppn_t ppn, nand_page_meta_t *out)
{
    nand_sync_ctx_t ctx;
    int rc;

    ctx.done   = 0;
    ctx.status = SSD_OK;
    rc = nand_read_async(dev, ppn, out, nand_sync_cb, &ctx);
    if (rc != SSD_OK) {
        return rc;
    }
    return nand_wait(dev, &ctx);
}

int nand_erase_sync(nand_dev_t *dev, pbn_t pbn)
{
    nand_sync_ctx_t ctx;
    int rc;

    ctx.done   = 0;
    ctx.status = SSD_OK;
    rc = nand_erase_async(dev, pbn, nand_sync_cb, &ctx);
    if (rc != SSD_OK) {
        return rc;
    }
    return nand_wait(dev, &ctx);
}

/* ------------------------------------------------------------------ */
/* 事件驱动                                                            */
/* ------------------------------------------------------------------ */

int nand_step(nand_dev_t *dev)
{
    ssd_event_t e;
    int rc;

    if (dev == NULL) {
        return SSD_ERR_INVAL;
    }
    rc = ssd_evtq_pop(&dev->evtq, &e);
    if (rc != SSD_OK) {
        return rc;
    }
    if (e.time > ssd_clock_now()) {
        ssd_clock_set(e.time);
    }
    if (e.cb != NULL) {
        e.cb(e.arg);
    }
    return SSD_OK;
}

void nand_run_until_idle(nand_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    while (ssd_evtq_size(&dev->evtq) > 0u) {
        (void)nand_step(dev);
    }
}

uint32_t nand_pending(const nand_dev_t *dev)
{
    if (dev == NULL) {
        return 0u;
    }
    return ssd_evtq_size(&dev->evtq);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

int nand_init(nand_dev_t *dev, const ssd_config_t *cfg, uint32_t evtq_cap)
{
    uint64_t i;

    if (dev == NULL || cfg == NULL) {
        return SSD_ERR_INVAL;
    }
    memset(dev, 0, sizeof(*dev));

    nand_geo_init(&dev->geo, cfg);

    dev->t_prog = cfg->t_prog_ns;
    dev->t_read = cfg->t_read_ns;
    dev->t_bers = cfg->t_bers_ns;
    dev->t_xfer = cfg->t_xfer_ns;
    dev->page_bytes = (cfg->page_data_size > 0u) ? cfg->page_data_size : 4096u;

    dev->pe_limit            = cfg->pe_limit;
    dev->fault_inject        = cfg->fault_inject;
    dev->fault_rate_permille = cfg->fault_rate_permille;
    dev->factory_bb_permille = cfg->factory_bb_permille;

    if (evtq_cap == 0u) {
        evtq_cap = 256u;
    }
    dev->evtq_cap = evtq_cap;

    /* 上电时的 DRAM 预留：一次性分配，运行期不再分配任何内存 */
    dev->pages     = (nand_page_meta_t *)calloc((size_t)dev->geo.total_pages,
                                                sizeof(nand_page_meta_t));
    dev->blocks    = (nand_block_info_t *)calloc((size_t)dev->geo.total_blocks,
                                                 sizeof(nand_block_info_t));
    dev->channels  = (nand_channel_t *)calloc((size_t)cfg->channels,
                                              sizeof(nand_channel_t));
    dev->n_ces     = nand_geo_ce_count(&dev->geo);
    dev->ces       = (nand_ce_t *)calloc((size_t)dev->n_ces, sizeof(nand_ce_t));
    dev->evtq_heap = (ssd_event_t *)calloc((size_t)evtq_cap, sizeof(ssd_event_t));

    /* S5：system 区的 TRIM 位图，掉电保留，所以和 pages 一样是"持久"存储 */
    dev->nv_trim_words = SSD_BITMAP_WORDS((uint32_t)dev->geo.total_pages);
    dev->nv_trim = (uint32_t *)calloc((size_t)dev->nv_trim_words, sizeof(uint32_t));

    /* S6：活跃块位图 + 快照缓冲区（同样是掉电保留的 system 区） */
    dev->nv_active_words = SSD_BITMAP_WORDS((uint32_t)dev->geo.total_blocks);
    dev->nv_active = (uint32_t *)calloc((size_t)dev->nv_active_words,
                                        sizeof(uint32_t));
    dev->cp_lbas = cfg->user_pages;
    dev->cp_n_open = nand_geo_ce_count(&dev->geo);
    if (dev->cp_n_open == 0u) {
        dev->cp_n_open = 1u;
    }
    if (dev->cp_lbas > 0u) {
        dev->cp_l2p = (ppn_t *)calloc((size_t)dev->cp_lbas, sizeof(ppn_t));
    }
    dev->cp_blk_state = (uint16_t *)calloc((size_t)dev->geo.total_blocks,
                                           sizeof(uint16_t));
    dev->cp_blk_next = (uint32_t *)calloc((size_t)dev->geo.total_blocks,
                                          sizeof(uint32_t));
    dev->cp_blk_valid = (uint32_t *)calloc((size_t)dev->geo.total_blocks,
                                           sizeof(uint32_t));
    dev->cp_blk_invalid = (uint32_t *)calloc((size_t)dev->geo.total_blocks,
                                             sizeof(uint32_t));
    dev->cp_open_blk = (pbn_t *)calloc((size_t)dev->cp_n_open, sizeof(pbn_t));
    dev->cp_open_next = (uint32_t *)calloc((size_t)dev->cp_n_open,
                                           sizeof(uint32_t));

    if (dev->pages == NULL || dev->blocks == NULL ||
        dev->channels == NULL || dev->ces == NULL || dev->evtq_heap == NULL ||
        dev->nv_trim == NULL || dev->nv_active == NULL ||
        (dev->cp_lbas > 0u && dev->cp_l2p == NULL) ||
        dev->cp_blk_state == NULL || dev->cp_blk_next == NULL ||
        dev->cp_blk_valid == NULL || dev->cp_blk_invalid == NULL ||
        dev->cp_open_blk == NULL || dev->cp_open_next == NULL) {
        nand_deinit(dev);
        return SSD_ERR_NO_MEM;
    }

    ssd_evtq_init(&dev->evtq, dev->evtq_heap, evtq_cap);
    ssd_rng_seed(&dev->rng, cfg->seed);
    dev->start_ns = ssd_clock_now();

    for (i = 0; i < dev->geo.total_pages; i++) {
        dev->pages[i].lba   = SSD_INVALID_LBA;
        dev->pages[i].seq   = 0u;
        dev->pages[i].state = (uint16_t)NAND_PAGE_FREE;
        dev->pages[i].crc   = 0u;
    }
    for (i = 0; i < dev->geo.total_blocks; i++) {
        dev->blocks[i].state = (uint16_t)NAND_BLK_FREE;
    }

    /* 出厂坏块：由 seed 决定，可复现 */
    for (i = 0; i < dev->geo.total_blocks; i++) {
        if (ssd_rng_chance_permille(&dev->rng, dev->factory_bb_permille)) {
            dev->blocks[i].state = (uint16_t)NAND_BLK_BAD;
            dev->factory_bad_blocks++;
        }
    }

    SSD_INFO("nand initialized: %llu blocks / %llu pages, factory bad blocks %u",
             (unsigned long long)dev->geo.total_blocks,
             (unsigned long long)dev->geo.total_pages,
             dev->factory_bad_blocks);
    return SSD_OK;
}

void nand_deinit(nand_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    free(dev->pages);
    free(dev->blocks);
    free(dev->channels);
    free(dev->ces);
    free(dev->evtq_heap);
    free(dev->nv_trim);
    free(dev->nv_active);
    free(dev->cp_l2p);
    free(dev->cp_blk_state);
    free(dev->cp_blk_next);
    free(dev->cp_blk_valid);
    free(dev->cp_blk_invalid);
    free(dev->cp_open_blk);
    free(dev->cp_open_next);
    memset(dev, 0, sizeof(*dev));
}

/* ------------------------------------------------------------------ */
/* 查询                                                                */
/* ------------------------------------------------------------------ */

uint32_t nand_pe_count(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].erase_count;
}

uint16_t nand_block_state(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].state;
}

uint32_t nand_block_valid_pages(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].valid_pages;
}

uint32_t nand_block_invalid_pages(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].invalid_pages;
}

uint32_t nand_block_pending_pages(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].pending_pages;
}

uint32_t nand_block_next_page(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].next_page;
}

uint16_t nand_page_state(const nand_dev_t *dev, ppn_t ppn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(ppn < dev->geo.total_pages);
    return dev->pages[ppn].state;
}

uint64_t nand_block_last_prog_ns(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].last_prog_ns;
}

uint64_t nand_block_last_invalid_ns(const nand_dev_t *dev, pbn_t pbn)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    return dev->blocks[pbn].last_invalid_ns;
}

/* ------------------------------------------------------------------ */
/* 并发与利用率（S4）                                                   */
/* ------------------------------------------------------------------ */

uint32_t nand_ce_count(const nand_dev_t *dev)
{
    SSD_ASSERT(dev != NULL);
    return dev->n_ces;
}

uint64_t nand_ce_busy_ns(const nand_dev_t *dev, uint32_t ce_idx)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(ce_idx < dev->n_ces);
    return dev->ces[ce_idx].busy_ns;
}

uint64_t nand_channel_busy_ns(const nand_dev_t *dev, uint32_t ch)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(ch < dev->geo.channels);
    return dev->channels[ch].busy_ns;
}

/* 利用率的分母：仿真走过的虚拟时间 */
static uint64_t nand_elapsed_ns(const nand_dev_t *dev)
{
    uint64_t now = ssd_clock_now();

    if (now <= dev->start_ns) {
        return 1u;   /* 避免除零 */
    }
    return now - dev->start_ns;
}

uint64_t nand_sim_start_ns(const nand_dev_t *dev)
{
    SSD_ASSERT(dev != NULL);
    return dev->start_ns;
}

double nand_channel_utilization(const nand_dev_t *dev)
{
    uint64_t sum = 0u;
    uint32_t i;

    SSD_ASSERT(dev != NULL);
    for (i = 0; i < dev->geo.channels; i++) {
        sum += dev->channels[i].busy_ns;
    }
    return (double)sum /
           ((double)nand_elapsed_ns(dev) * (double)dev->geo.channels);
}

double nand_ce_utilization(const nand_dev_t *dev)
{
    uint64_t sum = 0u;
    uint32_t i;

    SSD_ASSERT(dev != NULL);
    for (i = 0; i < dev->n_ces; i++) {
        sum += dev->ces[i].busy_ns;
    }
    return (double)sum / ((double)nand_elapsed_ns(dev) * (double)dev->n_ces);
}

/* 平均并行度：仿真期间平均有几个 CE 同时在工作。
 * 串行模型下它 ≈ 1，并发调度做对了才接近 CE 总数。 */
double nand_parallelism(const nand_dev_t *dev)
{
    uint64_t sum = 0u;
    uint32_t i;

    SSD_ASSERT(dev != NULL);
    for (i = 0; i < dev->n_ces; i++) {
        sum += dev->ces[i].busy_ns;
    }
    return (double)sum / (double)nand_elapsed_ns(dev);
}

/* ------------------------------------------------------------------ */
/* 状态维护：由 FTL 层驱动                                             */
/* ------------------------------------------------------------------ */

void nand_invalidate_page(nand_dev_t *dev, ppn_t ppn)
{
    nand_block_info_t *bi;
    pbn_t pbn;

    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(ppn < dev->geo.total_pages);

    if (dev->pages[ppn].state != (uint16_t)NAND_PAGE_VALID) {
        return;   /* 已失效或未写过，幂等 */
    }
    dev->pages[ppn].state = (uint16_t)NAND_PAGE_INVALID;

    pbn = nand_geo_pbn_of_ppn(&dev->geo, ppn);
    bi  = &dev->blocks[pbn];
    if (bi->valid_pages > 0u) {
        bi->valid_pages--;
    }
    bi->invalid_pages++;
    bi->last_invalid_ns = ssd_clock_now();
}

void nand_set_block_state(nand_dev_t *dev, pbn_t pbn, uint16_t state)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    dev->blocks[pbn].state = state;
}

void nand_set_block_next_page(nand_dev_t *dev, pbn_t pbn, uint32_t next)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    SSD_ASSERT(next <= dev->geo.pages_per_block);
    dev->blocks[pbn].next_page = next;
}

/* ------------------------------------------------------------------ */
/* S5：掉电                                                            */
/* ------------------------------------------------------------------ */

/* 把一页变成"撕裂页"：半写进去的内容一律当没写，lba 也一并清掉 */
static void nand_tear_page(nand_dev_t *dev, ppn_t ppn)
{
    nand_page_meta_t *m = &dev->pages[ppn];

    m->lba   = SSD_INVALID_LBA;
    m->seq   = 0u;
    m->state = (uint16_t)NAND_PAGE_CORRUPT;
    nand_meta_seal(m);
}

void nand_power_cut(nand_dev_t *dev, nand_power_loss_t *out)
{
    nand_power_loss_t loss;
    uint64_t ppn;
    pbn_t pbn;
    uint32_t i;

    if (dev == NULL) {
        return;
    }
    memset(&loss, 0, sizeof(loss));
    loss.at_ns = ssd_clock_now();
    loss.ops_busy = ssd_evtq_size(&dev->evtq);

    /*
     * 1. 在飞的操作。
     *
     * 编程被打断的页是"半页"：真实 NAND 上它读出来就是 UECC，
     * 谁也不知道里面是旧数据、新数据还是两者的混合体 —— 只能丢弃。
     * 正在擦除的块同理：擦到一半的块，页可能处于任意中间态，
     * 所以整块都不可信（不是只有"擦到那一页"不可信）。
     */
    for (i = 0; i < NAND_MAX_PENDING_OPS; i++) {
        nand_op_t *op = &dev->ops[i];

        if (op->in_use == 0u) {
            continue;
        }
        pbn = nand_geo_pbn_of_ppn(&dev->geo, op->ppn);

        if (op->type == NAND_OP_PROG) {
            if (dev->pages[op->ppn].state == (uint16_t)NAND_PAGE_PENDING) {
                nand_tear_page(dev, op->ppn);
                if (dev->blocks[pbn].pending_pages > 0u) {
                    dev->blocks[pbn].pending_pages--;
                }
                loss.torn_progs++;
            }
        } else if (op->type == NAND_OP_ERASE) {
            uint32_t j;
            ppn_t base = nand_geo_block_base(&dev->geo, pbn);

            for (j = 0; j < dev->geo.pages_per_block; j++) {
                nand_tear_page(dev, base + j);
            }
            loss.torn_erases++;
        }
        loss.dropped_ops++;
        nand_op_release(op);
    }

    /* 2. 控制器复位：事件队列与通道/CE 的占用时刻都随断电消失。
     *    busy_ns 是累计统计，不属于设备状态，保留。 */
    ssd_evtq_init(&dev->evtq, dev->evtq_heap, dev->evtq_cap);
    for (i = 0; i < dev->geo.channels; i++) {
        dev->channels[i].free_at = 0u;
    }
    for (i = 0; i < dev->n_ces; i++) {
        dev->ces[i].free_at = 0u;
    }

    /*
     * 3. 页的失效标记丢失。
     *
     * "这一页失效了"是 FTL 在 DRAM 里的记账 —— 介质上旧页的 spare
     * 里依然写着它的 lba 和 seq，数据也还在。
     * 真实盘绝不可能为了标失效去改写已编程页的 spare（那要先擦整块）。
     *
     * 这一步是重建必须靠 seq 排序的根本原因：
     * 覆盖写之后，新旧两页在介质上看起来都是"写着同一个 lba 的正常页"。
     */
    for (ppn = 0; ppn < dev->geo.total_pages; ppn++) {
        if (dev->pages[ppn].state == (uint16_t)NAND_PAGE_INVALID) {
            dev->pages[ppn].state = (uint16_t)NAND_PAGE_VALID;
        }
    }

    /* 4. 块表丢失：状态、有效/失效页数、写指针、时间戳全部归零待重建。
     *    坏块标记是持久的，不在这里清洗。 */
    for (pbn = 0; pbn < dev->geo.total_blocks; pbn++) {
        nand_block_info_t *bi = &dev->blocks[pbn];

        if (bi->state != (uint16_t)NAND_BLK_BAD) {
            bi->state = (uint16_t)NAND_BLK_UNKNOWN;
        }
        bi->next_page        = 0u;
        bi->valid_pages      = 0u;
        bi->invalid_pages    = 0u;
        bi->pending_pages    = 0u;
        bi->last_prog_ns     = 0u;
        bi->last_invalid_ns  = 0u;
    }

    if (out != NULL) {
        *out = loss;
    }

    SSD_WARN("power cut at %llu ns: dropped %u/%u ops "
             "(torn prog %u, torn erase %u)",
             (unsigned long long)loss.at_ns, loss.dropped_ops, loss.ops_busy,
             loss.torn_progs, loss.torn_erases);
}

/* ------------------------------------------------------------------ */
/* S5：system 区（掉电保留）                                            */
/* ------------------------------------------------------------------ */

uint64_t nand_persistent_seq(const nand_dev_t *dev)
{
    if (dev == NULL) {
        return 0u;
    }
    return dev->persist_seq;
}

void nand_set_persistent_seq(nand_dev_t *dev, uint64_t seq)
{
    if (dev == NULL) {
        return;
    }
    /* 只增不减：回退写序号等于让未来的重建去选旧版本 */
    if (seq > dev->persist_seq) {
        dev->persist_seq = seq;
    }
}

void nand_nv_trim_set(nand_dev_t *dev, lba_t lba)
{
    if (dev == NULL || dev->nv_trim == NULL) {
        return;
    }
    if (lba >= dev->geo.total_pages) {
        return;
    }
    ssd_bitmap_set(dev->nv_trim, (uint32_t)lba);
}

void nand_nv_trim_clear(nand_dev_t *dev, lba_t lba)
{
    if (dev == NULL || dev->nv_trim == NULL) {
        return;
    }
    if (lba >= dev->geo.total_pages) {
        return;
    }
    ssd_bitmap_clear(dev->nv_trim, (uint32_t)lba);
}

bool nand_nv_trim_get(const nand_dev_t *dev, lba_t lba)
{
    if (dev == NULL || dev->nv_trim == NULL) {
        return false;
    }
    if (lba >= dev->geo.total_pages) {
        return false;
    }
    return ssd_bitmap_get(dev->nv_trim, (uint32_t)lba);
}

uint64_t nand_scan_time_ns(const nand_dev_t *dev)
{
    uint64_t blocks;
    uint32_t lanes;

    if (dev == NULL || dev->geo.total_blocks == 0u) {
        return 0u;
    }
    /*
     * 按块读摘要，而不是逐页读 spare。
     *
     * 逐页扫的代价是 total_pages × t_read / CE 数 —— 按默认拓扑算是 3.9 秒，
     * 比跑完两万个请求本身还久。真实盘承受不起这个上电延迟，
     * 所以固件一定会在每个块的末尾维护一份"块摘要页"（记录块内每页的
     * lba/seq），上电时每块读一次即可。这里按这种实现等效建模。
     *
     * 顺带说明为什么真实固件还要做 checkpoint：块摘要把成本从 O(页数)
     * 降到 O(块数)，但盘变大之后仍然需要"只扫上次之后的块"，
     * 那一步就靠持久化的检查点 + 日志，本模型暂未实现。
     */
    lanes  = (dev->n_ces > 0u) ? dev->n_ces : 1u;
    blocks = dev->geo.total_blocks;
    return (blocks * (uint64_t)dev->t_read) / (uint64_t)lanes;
}

uint64_t nand_scan_blocks_time_ns(const nand_dev_t *dev, uint64_t blocks)
{
    uint32_t lanes;

    if (dev == NULL || blocks == 0u) {
        return 0u;
    }
    lanes = (dev->n_ces > 0u) ? dev->n_ces : 1u;
    return (blocks * (uint64_t)dev->t_read) / (uint64_t)lanes;
}

/* ------------------------------------------------------------------ */
/* S6：checkpoint                                                      */
/* ------------------------------------------------------------------ */

/*
 * 快照要占多少字节。
 *
 * 这一项必须算清楚，因为 checkpoint 不是免费的：它自己也要写进 system 区，
 * 占用通道传输与介质编程的时间。真实固件里"多久做一次检查点"就是
 * 拿这笔开销去换"上电时要扫多久"，本模型的取舍因此可以被量化。
 */
static uint64_t cp_bytes(const nand_dev_t *dev)
{
    uint64_t b;

    if (dev == NULL) {
        return 0u;
    }
    /*
     * L2P 条目按 4 字节计：真实盘的映射条目就是一个 32 位物理页号，
     * 4GB 级的盘、4KB 页也才 2^20 个条目，4 字节足够。
     * 这里如果按宿主的 ppn_t（8 字节）算，快照凭空大一倍，
     * 上电读回的固定开销会盖掉"少扫几百个块"省下的时间。
     */
    b  = (uint64_t)dev->cp_lbas * 4u;
    b += (uint64_t)dev->geo.total_blocks *
         (uint64_t)(sizeof(uint16_t) + 3u * sizeof(uint32_t));
    b += (uint64_t)dev->cp_n_open * (uint64_t)(sizeof(pbn_t) + sizeof(uint32_t));
    return b;
}

static uint64_t cp_pages(const nand_dev_t *dev)
{
    uint64_t b = cp_bytes(dev);
    uint64_t ps = (dev->page_bytes > 0u) ? (uint64_t)dev->page_bytes : 4096u;

    return (b + ps - 1u) / ps;
}

uint64_t nand_cp_store_time_ns(const nand_dev_t *dev)
{
    if (dev == NULL) {
        return 0u;
    }
    return cp_pages(dev) *
           ((uint64_t)dev->t_xfer + (uint64_t)dev->t_prog);
}

uint64_t nand_cp_load_time_ns(const nand_dev_t *dev)
{
    if (dev == NULL) {
        return 0u;
    }
    return cp_pages(dev) *
           ((uint64_t)dev->t_xfer + (uint64_t)dev->t_read);
}

void nand_cp_store(nand_dev_t *dev, uint64_t seq, const ppn_t *l2p,
                   lba_t n_lbas, const pbn_t *open_blk,
                   const uint32_t *open_next, uint32_t n_open)
{
    pbn_t pbn;
    uint32_t i;

    if (dev == NULL || dev->cp_blk_state == NULL) {
        return;
    }
    if (dev->cp_l2p == NULL || dev->cp_lbas != n_lbas || l2p == NULL) {
        return;   /* 容量对不上：这份快照宁可不写，也不能写一半 */
    }

    for (i = 0; i < (uint32_t)n_lbas; i++) {
        dev->cp_l2p[i] = l2p[i];
    }
    for (pbn = 0; pbn < dev->geo.total_blocks; pbn++) {
        dev->cp_blk_state[pbn]   = dev->blocks[pbn].state;
        dev->cp_blk_next[pbn]    = dev->blocks[pbn].next_page;
        dev->cp_blk_valid[pbn]   = dev->blocks[pbn].valid_pages;
        dev->cp_blk_invalid[pbn] = dev->blocks[pbn].invalid_pages;
    }
    for (i = 0; i < dev->cp_n_open; i++) {
        dev->cp_open_blk[i]  = (i < n_open && open_blk != NULL)
                               ? open_blk[i] : SSD_INVALID_PBN;
        dev->cp_open_next[i] = (i < n_open && open_next != NULL)
                               ? open_next[i] : 0u;
    }

    dev->cp_seq   = seq;
    dev->cp_valid = true;
}

bool nand_cp_load(nand_dev_t *dev, uint64_t *seq, ppn_t *l2p,
                  lba_t n_lbas, pbn_t *open_blk,
                  uint32_t *open_next, uint32_t n_open)
{
    pbn_t pbn;
    uint32_t i;

    if (dev == NULL || !dev->cp_valid || dev->cp_l2p == NULL ||
        dev->cp_blk_state == NULL) {
        return false;
    }
    if (dev->cp_lbas != n_lbas) {
        return false;   /* 几何变了：旧快照一律不认 */
    }

    if (l2p != NULL) {
        for (i = 0; i < (uint32_t)n_lbas; i++) {
            l2p[i] = dev->cp_l2p[i];
        }
    }
    for (pbn = 0; pbn < dev->geo.total_blocks; pbn++) {
        if (dev->blocks[pbn].state == (uint16_t)NAND_BLK_BAD) {
            continue;   /* 坏块标记是持久的，比快照更权威 */
        }
        dev->blocks[pbn].state         = dev->cp_blk_state[pbn];
        dev->blocks[pbn].next_page     = dev->cp_blk_next[pbn];
        dev->blocks[pbn].valid_pages   = dev->cp_blk_valid[pbn];
        dev->blocks[pbn].invalid_pages = dev->cp_blk_invalid[pbn];
        dev->blocks[pbn].pending_pages = 0u;
    }
    if (open_blk != NULL && open_next != NULL) {
        for (i = 0; i < n_open && i < dev->cp_n_open; i++) {
            open_blk[i]  = dev->cp_open_blk[i];
            open_next[i] = dev->cp_open_next[i];
        }
    }
    if (seq != NULL) {
        *seq = dev->cp_seq;
    }
    return true;
}

void nand_cp_invalidate(nand_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    dev->cp_valid = false;
    dev->cp_seq   = 0u;
}

bool nand_cp_is_valid(const nand_dev_t *dev)
{
    return (dev != NULL) && dev->cp_valid;
}

void nand_nv_active_set(nand_dev_t *dev, pbn_t pbn)
{
    if (dev == NULL || dev->nv_active == NULL) {
        return;
    }
    if (pbn >= dev->geo.total_blocks) {
        return;
    }
    ssd_bitmap_set(dev->nv_active, (uint32_t)pbn);
}

bool nand_nv_active_get(const nand_dev_t *dev, pbn_t pbn)
{
    if (dev == NULL || dev->nv_active == NULL) {
        return false;
    }
    if (pbn >= dev->geo.total_blocks) {
        return false;
    }
    return ssd_bitmap_get(dev->nv_active, (uint32_t)pbn);
}

void nand_nv_active_clear_all(nand_dev_t *dev)
{
    if (dev == NULL || dev->nv_active == NULL) {
        return;
    }
    memset(dev->nv_active, 0, (size_t)dev->nv_active_words * sizeof(uint32_t));
}

uint64_t nand_nv_active_count(const nand_dev_t *dev)
{
    uint64_t n = 0u;
    pbn_t pbn;

    if (dev == NULL || dev->nv_active == NULL) {
        return 0u;
    }
    for (pbn = 0; pbn < dev->geo.total_blocks; pbn++) {
        if (ssd_bitmap_get(dev->nv_active, (uint32_t)pbn)) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* S5：重建回写                                                        */
/* ------------------------------------------------------------------ */

void nand_page_set_state(nand_dev_t *dev, ppn_t ppn, uint16_t state)
{
    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(ppn < dev->geo.total_pages);
    dev->pages[ppn].state = state;
}

void nand_block_rebuild(nand_dev_t *dev, pbn_t pbn, uint16_t state,
                        uint32_t next_page, uint32_t valid, uint32_t invalid,
                        uint64_t last_prog_ns)
{
    nand_block_info_t *bi;

    SSD_ASSERT(dev != NULL);
    SSD_ASSERT(pbn < dev->geo.total_blocks);
    SSD_ASSERT(next_page <= dev->geo.pages_per_block);

    bi = &dev->blocks[pbn];
    bi->state            = state;
    bi->next_page        = next_page;
    bi->valid_pages      = valid;
    bi->invalid_pages    = invalid;
    bi->pending_pages    = 0u;
    bi->last_prog_ns     = last_prog_ns;
    bi->last_invalid_ns  = 0u;
}
