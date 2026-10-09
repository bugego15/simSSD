#include "media/nand.h"

#include "core/assert.h"
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

uint16_t nand_meta_crc(lba_t lba, uint32_t seq, uint16_t state)
{
    uint32_t h = FNV_OFFSET;

    h = fnv_update(h, &lba, sizeof(lba));
    h = fnv_update(h, &seq, sizeof(seq));
    h = fnv_update(h, &state, sizeof(state));
    return (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
}

void nand_meta_seal(nand_page_meta_t *m)
{
    if (m == NULL) {
        return;
    }
    m->crc = nand_meta_crc(m->lba, m->seq, m->state);
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
        }
        ssd_stats_inc(ST_NAND_PROG_PAGES);
        ssd_stats_add(ST_MEDIA_BUSY_NS, dev->t_prog);
        break;

    case NAND_OP_READ:
        meta = &dev->pages[op->ppn];
        if (dev->fault_inject != 0u &&
            ssd_rng_chance_permille(&dev->rng, dev->fault_rate_permille)) {
            status = SSD_ERR_UECC;
        } else if (meta->state != (uint16_t)NAND_PAGE_FREE) {
            /* 已写过的页必须能通过 CRC 自检，否则说明元数据被写坏了 */
            if (nand_meta_crc(meta->lba, meta->seq, meta->state) != meta->crc) {
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

static uint64_t nand_chan_acquire(nand_dev_t *dev, uint32_t ch)
{
    uint64_t start = dev->channels[ch].free_at;
    uint64_t now   = ssd_clock_now();

    if (start < now) {
        start = now;
    }
    dev->channels[ch].free_at = start + dev->t_xfer;
    dev->channels[ch].busy_ns += dev->t_xfer;
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

    /* 提交即占位，避免同一页被重复提交 */
    dev->pages[ppn].state = (uint16_t)NAND_PAGE_PENDING;

    ch    = nand_geo_channel_of_ppn(&dev->geo, ppn);
    start = nand_chan_acquire(dev, ch);
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
    start = nand_chan_acquire(dev, ch);
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

    /* 擦除命令本身不占用通道传输带宽 */
    done = ssd_clock_now() + dev->t_bers;

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
    dev->evtq_heap = (ssd_event_t *)calloc((size_t)evtq_cap, sizeof(ssd_event_t));

    if (dev->pages == NULL || dev->blocks == NULL ||
        dev->channels == NULL || dev->evtq_heap == NULL) {
        nand_deinit(dev);
        return SSD_ERR_NO_MEM;
    }

    ssd_evtq_init(&dev->evtq, dev->evtq_heap, evtq_cap);
    ssd_rng_seed(&dev->rng, cfg->seed);

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
    free(dev->evtq_heap);
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
