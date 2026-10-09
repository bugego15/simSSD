#ifndef SSD_MEDIA_NAND_H
#define SSD_MEDIA_NAND_H

#include "core/config.h"
#include "core/eventq.h"
#include "core/rng.h"
#include "media/geometry.h"
#include "ssd_types.h"

/*
 * NAND 介质模型。
 *
 * 三条建模原则：
 *
 * 1) 不保存 payload。真实盘的 data 区有 TB 级，内存放不下也没有必要。
 *    每个 page 只保留元数据（lba / seq / state / crc），读回时用
 *    (lba, seq) 复算 crc 校验 —— 这足以捕获地址映射错乱、重复写入、
 *    读到未写页等 FTL 逻辑错误，而这正是我们要验证的东西。
 *
 * 2) 通道与介质分离。数据经通道传输耗时 t_xfer（期间通道被占用），
 *    随后介质编程 t_prog（期间通道已释放，可服务其他 CE）。
 *    这是 NAND 能做多 CE 并发的根本原因，必须建模对。
 *
 * 3) 操作是异步的。提交时返回，完成时间由事件队列按虚拟时钟触发。
 */

/* ---------------- 状态定义 ---------------- */

typedef enum nand_page_state {
    NAND_PAGE_FREE    = 0,   /* 已擦除，可写入 */
    NAND_PAGE_VALID   = 1,   /* 有效数据 */
    NAND_PAGE_INVALID = 2,   /* 已失效，等待 GC 回收 */
    NAND_PAGE_PENDING = 3    /* 正在编程，暂时不可读 */
} nand_page_state_t;

typedef enum nand_block_state {
    NAND_BLK_FREE   = 0,     /* 已擦除，可作为 open block */
    NAND_BLK_OPEN   = 1,     /* 正在写入 */
    NAND_BLK_CLOSED = 2,     /* 已写满 */
    NAND_BLK_BAD    = 3      /* 坏块，不可使用 */
} nand_block_state_t;

/* ---------------- 元数据 ---------------- */

typedef struct nand_page_meta {
    lba_t    lba;       /* 该页承载的逻辑地址 */
    uint32_t seq;       /* 写入序号，用于校验与断电恢复排序 */
    uint16_t state;     /* nand_page_state_t */
    uint16_t crc;       /* 由 (lba, seq, state) 计算，检测元数据错乱 */
} nand_page_meta_t;

typedef struct nand_block_info {
    uint32_t erase_count;
    uint16_t state;         /* nand_block_state_t */
    uint16_t reserved;
    uint32_t next_page;     /* 写指针：本 block 内下一个可写页偏移 */
    uint32_t valid_pages;
    uint32_t invalid_pages;

    /* 时间戳（虚拟时钟，ns）：GC 的冷热判定与 WL 都要用。
     * last_prog_ns    —— 最后一次页编程完成时刻，代表"块有多新"
     * last_invalid_ns —— 最后一次页失效时刻，代表"多久没产生新垃圾"
     * cost-benefit 用二者的差值近似"块里的数据有多冷"。 */
    uint64_t last_prog_ns;
    uint64_t last_invalid_ns;
} nand_block_info_t;

/* ---------------- 完成回调 ---------------- */

typedef void (*nand_done_cb)(void *arg, int status);

/* ---------------- 设备 ---------------- */

#define NAND_MAX_PENDING_OPS 1024u

typedef struct nand_dev nand_dev_t;   /* 前向声明，op 需要回指设备 */

typedef enum nand_op_type {
    NAND_OP_PROG  = 0,
    NAND_OP_READ  = 1,
    NAND_OP_ERASE = 2
} nand_op_type_t;

typedef struct nand_op {
    uint32_t          in_use;
    nand_dev_t       *dev;
    nand_op_type_t    type;
    ppn_t             ppn;          /* PROG/READ: 目标页；ERASE: block 首页 */
    nand_page_meta_t  meta;         /* PROG: 待写入内容 */
    nand_page_meta_t *out;          /* READ: 输出缓冲 */
    int               status;       /* 完成状态 */
    nand_done_cb      cb;
    void             *arg;
} nand_op_t;

typedef struct nand_channel {
    uint64_t free_at;    /* 该通道再次可用的时刻 */
    uint64_t busy_ns;    /* 累计占用时间，用于算通道利用率 */
} nand_channel_t;

struct nand_dev {
    nand_geometry_t geo;

    nand_page_meta_t *pages;      /* [total_pages] */
    nand_block_info_t *blocks;    /* [total_blocks] */
    nand_channel_t  *channels;    /* [channels] */

    nand_op_t       ops[NAND_MAX_PENDING_OPS];

    ssd_evtq_t      evtq;
    ssd_event_t    *evtq_heap;
    uint32_t        evtq_cap;

    ssd_rng_t       rng;

    /* 时序参数 */
    uint32_t t_prog;
    uint32_t t_read;
    uint32_t t_bers;
    uint32_t t_xfer;

    /* 可靠性参数 */
    uint32_t pe_limit;
    uint32_t fault_inject;        /* 0/1：是否注入随机故障 */
    uint32_t fault_rate_permille;
    uint32_t factory_bb_permille;

    uint32_t factory_bad_blocks;  /* 初始化时生成的出厂坏块数 */
};

/* ---------------- 生命周期 ---------------- */

int  nand_init(nand_dev_t *dev, const ssd_config_t *cfg, uint32_t evtq_cap);
void nand_deinit(nand_dev_t *dev);

/* ---------------- 异步操作 ---------------- */

int nand_prog_async(nand_dev_t *dev, ppn_t ppn, const nand_page_meta_t *meta,
                    nand_done_cb cb, void *arg);
int nand_read_async(nand_dev_t *dev, ppn_t ppn, nand_page_meta_t *out,
                    nand_done_cb cb, void *arg);
int nand_erase_async(nand_dev_t *dev, pbn_t pbn, nand_done_cb cb, void *arg);

/* ---------------- 同步封装（测试 / 初始化用） ---------------- */

int nand_prog_sync(nand_dev_t *dev, ppn_t ppn, const nand_page_meta_t *meta);
int nand_read_sync(nand_dev_t *dev, ppn_t ppn, nand_page_meta_t *out);
int nand_erase_sync(nand_dev_t *dev, pbn_t pbn);

/* ---------------- 事件驱动 ---------------- */

/* 推进一个事件；队列空则什么都不做并返回 SSD_ERR_NO_SPACE */
int  nand_step(nand_dev_t *dev);
void nand_run_until_idle(nand_dev_t *dev);
uint32_t nand_pending(const nand_dev_t *dev);

/* ---------------- 查询 ---------------- */

uint32_t nand_pe_count(const nand_dev_t *dev, pbn_t pbn);
uint16_t nand_block_state(const nand_dev_t *dev, pbn_t pbn);
uint32_t nand_block_valid_pages(const nand_dev_t *dev, pbn_t pbn);
uint32_t nand_block_invalid_pages(const nand_dev_t *dev, pbn_t pbn);
uint32_t nand_block_next_page(const nand_dev_t *dev, pbn_t pbn);
uint16_t nand_page_state(const nand_dev_t *dev, ppn_t ppn);
uint64_t nand_block_last_prog_ns(const nand_dev_t *dev, pbn_t pbn);
uint64_t nand_block_last_invalid_ns(const nand_dev_t *dev, pbn_t pbn);

/* 元数据 CRC：只覆盖数据身份 (lba, seq)。
 * 注意不要把 state 算进去 —— state 会随"页失效"被介质层改写，
 * 若纳入 CRC，失效后的页再读就会出现假性校验失败。 */
uint16_t nand_meta_crc(lba_t lba, uint32_t seq);
void     nand_meta_seal(nand_page_meta_t *m);

/* ---------------- 状态维护（由 FTL 层调用） ---------------- */

/* 把某个有效页标记为失效：host 覆盖写旧页、TRIM、GC 搬移后都需要 */
void     nand_invalidate_page(nand_dev_t *dev, ppn_t ppn);
void     nand_set_block_state(nand_dev_t *dev, pbn_t pbn, uint16_t state);
void     nand_set_block_next_page(nand_dev_t *dev, pbn_t pbn, uint32_t next);

#endif /* SSD_MEDIA_NAND_H */
