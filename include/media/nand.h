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
    NAND_PAGE_PENDING = 3,   /* 正在编程，暂时不可读 */
    /*
     * 掉电撕裂页（S5）：编程被断电打断，介质上的电荷只写进去一部分。
     * 真实盘读这种页是 UECC —— 数据不可信，重建时必须当成"没有这一页"。
     * 它不是 INVALID：INVALID 说的是"数据是对的，但已经过时"；
     * CORRUPT 说的是"这一页根本不存在"。区分两者很重要，
     * 否则恢复时会把一个半写的页当成有效数据映射给 host。
     */
    NAND_PAGE_CORRUPT = 4
} nand_page_state_t;

typedef enum nand_block_state {
    NAND_BLK_FREE   = 0,     /* 已擦除，可作为 open block */
    NAND_BLK_OPEN   = 1,     /* 正在写入 */
    NAND_BLK_CLOSED = 2,     /* 已写满 */
    NAND_BLK_BAD    = 3,     /* 坏块，不可使用 */
    /*
     * 上电未知（S5）：块表和页状态都住在 FTL 的 DRAM 里，掉电即失。
     * 上电后这类块必须逐页扫 spare 才能判定它到底是空闲、半满还是写满。
     * BAD 相反 —— 坏块信息是持久的（真实盘靠出厂标记 + 固件坏块表记录），
     * 所以掉电不会把 BAD 洗成 UNKNOWN。
     */
    NAND_BLK_UNKNOWN = 4
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

    /* 已提交但尚未完成的页编程数（S4）。
     *
     * 必须有这个计数：异步提交之后，页在介质上还处于 PENDING 状态，
     * 而它的"有效/失效"要等完成回调才知道。GC 若不看这个计数，
     * 就可能把还有页在飞的块擦掉，随后这块被重新分配，
     * 那个迟到的编程会把 host 数据写进别人的块里 —— 实测会静默丢数据。
     * 真实固件同样要跟踪 in-flight 操作，回收前必须等它们落地。 */
    uint32_t pending_pages;

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

/*
 * CE（Chip Enable）级资源（S4）。
 *
 * 一个 NAND 操作的时序被拆成两段：
 *   1. t_xfer —— 数据经通道传输，期间通道被独占
 *   2. t_prog / t_read / t_bers —— 介质内部操作，通道已释放
 *
 * 关键在于：介质操作期间通道是空闲的，可以去服务另一个 CE。
 * 所以"并发度"由 CE 数决定，而"通道"只是传输期的共享瓶颈。
 * 同一时刻同一个 CE 上只能有一个操作 —— 这就是 free_at 的作用。
 */
typedef struct nand_ce {
    uint64_t free_at;    /* 该 CE 再次可用的时刻 */
    uint64_t busy_ns;    /* 累计忙时间，用于算介质利用率 */
} nand_ce_t;

struct nand_dev {
    nand_geometry_t geo;

    nand_page_meta_t *pages;      /* [total_pages] */
    nand_block_info_t *blocks;    /* [total_blocks] */
    nand_channel_t  *channels;    /* [channels] */
    nand_ce_t       *ces;         /* [channels * ces_per_ch] */
    uint32_t         n_ces;
    uint64_t         start_ns;    /* 仿真起点，作为利用率的时间分母 */

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
    uint32_t page_bytes;      /* data 区字节数：system 区元数据按页计费要用 */

    /* 可靠性参数 */
    uint32_t pe_limit;
    uint32_t fault_inject;        /* 0/1：是否注入随机故障 */
    uint32_t fault_rate_permille;
    uint32_t factory_bb_permille;

    uint32_t factory_bad_blocks;  /* 初始化时生成的出厂坏块数 */

    /*
     * S5：掉电后依然存在的东西。
     *
     * 这两项模拟真实盘里的 system 区（superblock / NVRAM 元数据）：
     * 它们不在"每页的 spare"里，而是固件单独维护、按低频下刷的持久化状态。
     *
     * persist_seq —— 已持久化的写序号。
     *   它是 FTL 的"写到第几笔了"这个计数，掉电后必须从介质上取回来：
     *   块年龄（cost-benefit 判冷热用的 blk_invalid_seq）是以它为基线算的，
     *   基线一旦回退，一批块会被误判成"很久没动过"而遭到过度回收。
     *   只从页里取 max seq 是不够的 —— 盘被 TRIM + GC 清空后 max 归零，
     *   基线就跟着塌了。
     *  （真实盘上这个序号往往还要承担判定页版本的职责，本模型里
     *    页版本由 host 传入的 seq 决定，所以它只服务块年龄基线。）
     *
     * nv_trim —— 已持久化的 TRIM 位图。
     *   TRIM 只是把映射断开，页里的数据还在。若不持久化这条信息，
     *   掉电重建时会把 host 已经删掉的数据重新映射回来 ——
     *   TRIM 语义被破坏，而且数据"死而复生"是安全问题。
     */
    uint64_t persist_seq;
    uint32_t *nv_trim;        /* [total_pages] 位，按 lba 索引 */
    uint32_t  nv_trim_words;

    /*
     * S6：checkpoint（掉电保留的固件元数据快照）。
     *
     * S5 的重建要逐块读介质，成本 O(块数) —— 盘一大，上电延迟就不可接受。
     * 真实固件靠"持久化检查点"把这条路径砍掉：
     * 每隔一段时间，把 DRAM 里的映射表与块表整份下刷到 system 区；
     * 上电时直接读回来，只需补扫"检查点之后动过"的那几个块。
     *
     * 快照存的是 FTL 的概念（L2P），放在介质层看似越界，但这就是
     * system 区的物理含义：固件自己管的、掉电不丢的元数据区
     *（真实盘写在 system block 或 EEPROM/NVRAM 上）。
     */
    bool      cp_valid;
    uint64_t  cp_seq;           /* 快照对应的写序号 */
    ppn_t    *cp_l2p;           /* [cp_lbas] L2P 快照 */
    lba_t     cp_lbas;
    uint16_t *cp_blk_state;     /* [total_blocks] 块表快照 */
    uint32_t *cp_blk_next;
    uint32_t *cp_blk_valid;
    uint32_t *cp_blk_invalid;
    pbn_t    *cp_open_blk;      /* [cp_n_open] 写入槽位快照 */
    uint32_t *cp_open_next;
    uint32_t  cp_n_open;

    /*
     * S6：活跃块位图（掉电保留）。
     *
     * 记录"自上次 checkpoint 以来，哪些块从空闲池里被取出来过"。
     * 上电时只扫这些块 —— 因为 checkpoint 之后所有的写，
     * 只可能落在"当时就开着的块"或"之后新取出来的块"上，
     * 其余块在快照里是什么样，掉电后就是什么样。
     *
     * 为什么不用"每页写都记一条日志"：日志能彻底免掉扫描，但每次 host 写
     * 都要额外写一笔元数据，那是一笔实打实的写放大，而且要保证
     * "日志先落盘、再给 host 回执"（否则回执过的数据掉电后会丢）。
     * 位图方案只在"块出池"这个低频事件上动一次元数据，代价小一个量级。
     */
    uint32_t *nv_active;        /* [total_blocks] 位，按 pbn 索引 */
    uint32_t  nv_active_words;
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
/* 该块上还有多少页编程未完成：GC 必须跳过这些块 */
uint32_t nand_block_pending_pages(const nand_dev_t *dev, pbn_t pbn);
uint32_t nand_block_next_page(const nand_dev_t *dev, pbn_t pbn);
uint16_t nand_page_state(const nand_dev_t *dev, ppn_t ppn);
uint64_t nand_block_last_prog_ns(const nand_dev_t *dev, pbn_t pbn);
uint64_t nand_block_last_invalid_ns(const nand_dev_t *dev, pbn_t pbn);

/* ---------------- 并发与利用率（S4） ---------------- */

uint32_t nand_ce_count(const nand_dev_t *dev);
uint64_t nand_ce_busy_ns(const nand_dev_t *dev, uint32_t ce_idx);
uint64_t nand_channel_busy_ns(const nand_dev_t *dev, uint32_t ch);

/* 通道利用率：通道忙时间 / (通道数 × 仿真时长)。
 * 这是"还能不能再加盘"的直接指标。 */
double   nand_channel_utilization(const nand_dev_t *dev);
/* 介质利用率：所有 CE 忙时间 / (CE 数 × 仿真时长) */
double   nand_ce_utilization(const nand_dev_t *dev);
/* 平均并行度：仿真期间平均有多少个 CE 同时在工作。
 * 单线程同步模型下这个值恒为 1/CE数，并发调度做对了才会接近 CE 数。 */
double   nand_parallelism(const nand_dev_t *dev);
/* 仿真时长基准（利用率分母） */
uint64_t nand_sim_start_ns(const nand_dev_t *dev);

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

/* ---------------- S5：掉电与上电恢复 ---------------- */

/*
 * 掉电记账。真实固件上电时靠这些信息决定"这次要不要走完整重建"，
 * 这里同时用它给测试提供可断言的结果。
 */
typedef struct nand_power_loss {
    uint64_t at_ns;         /* 掉电时刻（虚拟时钟） */
    uint32_t torn_progs;    /* 被断电打断的页编程：这些页数据不可信 */
    uint32_t torn_erases;   /* 被断电打断的块擦除：整块内容不可信 */
    uint32_t dropped_ops;   /* 丢失的 in-flight 操作数（含读，读丢了无害） */
    uint32_t ops_busy;      /* 掉电瞬间盘上还挂着多少操作 */
} nand_power_loss_t;

/*
 * 模拟一次突然掉电。
 *
 * 只做"物理上必然发生"的事，绝不帮忙收拾残局：
 *   1. 在飞的页编程 -> CORRUPT（半写的页不可信）
 *   2. 在飞的擦除   -> 整块页内容不清，但整块判为不可信
 *   3. 页的"失效"标记 -> 丢失。失效是 FTL 在 DRAM 里记的账，
 *      介质上旧页的 spare 里仍然写着它的 lba —— 这正是重建要靠 seq
 *      而不是靠 INVALID 标记判断版本的原因。
 *   4. 块状态 / 有效页计数 / 写指针 -> 丢失（都在 DRAM 块表里）
 *   5. 事件队列与控制器（通道/CE 占用）-> 复位
 * 坏块标记和 system 区（persist_seq / nv_trim）保留。
 */
void nand_power_cut(nand_dev_t *dev, nand_power_loss_t *out);

/* system 区：写序号 */
uint64_t nand_persistent_seq(const nand_dev_t *dev);
void     nand_set_persistent_seq(nand_dev_t *dev, uint64_t seq);

/* system 区：TRIM 位图 */
void nand_nv_trim_set(nand_dev_t *dev, lba_t lba);
void nand_nv_trim_clear(nand_dev_t *dev, lba_t lba);
bool nand_nv_trim_get(const nand_dev_t *dev, lba_t lba);

/*
 * 上电扫描的耗时。
 * 真实盘上电要逐块读 spare 才能重建映射，这一步是实打实要花时间的
 *（几 GB 的盘扫描几十毫秒到几百毫秒）。各 CE 可以并行扫自己那部分，
 * 所以除以 CE 数。这笔时间计入虚拟时钟，但不计入 host IOPS。
 */
uint64_t nand_scan_time_ns(const nand_dev_t *dev);
/* 只扫 n 个块的时间：checkpoint 之后只需扫活跃块，恢复时间据此下降 */
uint64_t nand_scan_blocks_time_ns(const nand_dev_t *dev, uint64_t blocks);

/* ---------------- S6：checkpoint ---------------- */

/*
 * 下刷一份快照。块表由介质层自己拷（它本来就住在 nand 里），
 * FTL 只需交出 L2P 与写入槽位 —— 这两样是 FTL 的概念。
 */
void     nand_cp_store(nand_dev_t *dev, uint64_t seq, const ppn_t *l2p,
                       lba_t n_lbas, const pbn_t *open_blk,
                       const uint32_t *open_next, uint32_t n_open);

/* 上电时读回快照。返回 false 表示没有可用快照，必须走全盘重建 */
bool     nand_cp_load(nand_dev_t *dev, uint64_t *seq, ppn_t *l2p,
                      lba_t n_lbas, pbn_t *open_blk,
                      uint32_t *open_next, uint32_t n_open);

void     nand_cp_invalidate(nand_dev_t *dev);
bool     nand_cp_is_valid(const nand_dev_t *dev);

/* 快照下刷 / 读回的耗时：元数据也要占真实的编程与传输时间 */
uint64_t nand_cp_store_time_ns(const nand_dev_t *dev);
uint64_t nand_cp_load_time_ns(const nand_dev_t *dev);

/* 活跃块位图：块一从池里取出来就置位，checkpoint 时清空 */
void     nand_nv_active_set(nand_dev_t *dev, pbn_t pbn);
bool     nand_nv_active_get(const nand_dev_t *dev, pbn_t pbn);
void     nand_nv_active_clear_all(nand_dev_t *dev);
uint64_t nand_nv_active_count(const nand_dev_t *dev);

/* 重建结果回写：由 FTL 扫描完整个块之后一次性改，避免统计量与页状态脱节 */
void nand_page_set_state(nand_dev_t *dev, ppn_t ppn, uint16_t state);
void nand_block_rebuild(nand_dev_t *dev, pbn_t pbn, uint16_t state,
                        uint32_t next_page, uint32_t valid, uint32_t invalid,
                        uint64_t last_prog_ns);

#endif /* SSD_MEDIA_NAND_H */
