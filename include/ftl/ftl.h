#ifndef SSD_FTL_FTL_H
#define SSD_FTL_FTL_H

#include "core/config.h"
#include "media/nand.h"
#include "ssd_types.h"

/*
 * FTL：页级映射 + 写/读/TRIM 路径 + 空间回收 + 磨损均衡 + 坏块管理
 *
 * 映射方案选型：page-level L2P，DRAM 驻留数组 [lba] -> ppn。
 *   - block-level 映射随机写放大极高，不做；
 *   - hybrid/log-structured 复杂且收益主要在元数据量，不引入。
 *
 * 反向映射 P2L 不单独建表：每个 page 的元数据里已经存了 lba + seq，
 * GC 搬移和断电重建时直接扫 block 内页的元数据就能反推，
 * 这与真实固件"元数据写在 spare 区"的做法一致。
 *
 * S3 新增三块：
 *   1. 回收策略：greedy / cost-benefit，以及后台 GC 水位
 *   2. 磨损均衡：分配时挑低 PE 块（动态），PE 差过大时迁移冷数据（静态）
 *   3. 坏块管理：program/erase 失败 -> 隔离坏块 + 抢救有效页 + 重映射
 */

typedef enum ftl_gc_policy {
    FTL_GC_GREEDY       = 0,   /* 只挑有效页最少的块 */
    FTL_GC_COST_BENEFIT = 1    /* (可回收页数 × 块年龄) / 搬移代价 */
} ftl_gc_policy_t;

typedef struct ftl_pe_stat {
    uint32_t min_pe;
    uint32_t max_pe;
    double   avg_pe;
    double   var_pe;       /* 方差 */
    double   stddev_pe;    /* 标准差：衡量磨损是否均衡 */
    uint32_t samples;
} ftl_pe_stat_t;

/* 搬移流水线的一格（S4）：一次块的搬移最多 pages_per_block 页 */
typedef struct ftl_dev ftl_dev_t;   /* 前向声明：槽位里要回指设备 */

typedef struct ftl_move_slot {
    ftl_dev_t *f;
    uint32_t   idx;
    int        ok;
    uint8_t    live;   /* 这一格是否真的提交过写（区分"跳过"与"写失败"） */
    uint8_t    tries;  /* 已尝试的编程次数，写失败要换页重试 */
} ftl_move_slot_t;

typedef struct ftl_dev {
    nand_dev_t         *nand;
    const ssd_config_t *cfg;

    /* 正向映射 L2P（页级，DRAM 驻留） */
    ppn_t *l2p;
    lba_t  user_lbas;

    /* 块级"最后变脏时刻"，单位是 host 写次数（不是 ns）。
     * cost-benefit 要用块年龄，而年龄必须和"页数"同量纲才能和搬移代价相比；
     * 直接用纳秒会让 age 项大出好几个数量级，把策略退化成"专挑最冷的块"。
     * 真实固件里同样维护块表（block table），这部分内存开销是必要的。 */
    uint32_t *blk_invalid_seq;
    uint64_t  write_seq;

    /* 空闲块池：所有可用块都在 free_stack 里。
     *
     * rsv_min 是前台 GC 的触发线：host 写不允许把池子取到 rsv_min 以下，
     * 这些块是留给 GC 的"工作空间"。GC 必须先有地方写，才能擦除 victim，
     * 否则就是死锁 —— OP 的物理意义就在这里。
     *
     * bg_target 是后台 GC 的目标水位：空闲块低于它时，在 host 间隙提前回收，
     * 避免 host 撞上前台 GC 产生延迟尖峰（S4 会把它移到真正的 idle 时段）。 */
    pbn_t    *free_stack;
    uint32_t  free_top;
    uint32_t  free_cap;
    uint32_t  rsv_min;
    uint32_t  bg_target;

    /*
     * 写入块（S4）：每个 CE 一个 open block。
     *
     * 为什么不能只维护一个 open block：块在物理上属于某个固定的 CE，
     * 单一 open block 意味着所有写都落在同一个 CE 上排队编程，
     * 盘上就算有 16 个 CE 也只能跑出 1 个 CE 的 IOPS ——
     * 实测并行度就是 1.00/4，多出来的 CE 完全是摆设。
     *
     * 真实固件同样同时开着一批块（superblock 会横跨所有 CE），
     * 写请求在它们之间轮转，NAND 的并行度才吃得满。
     * 这里让 open block 与 CE 一一对应，分配时按 open_rr 轮转。
     */
    pbn_t    *open_blocks;   /* [n_open]，INVALID 表示该槽位空着 */
    uint32_t *open_next;     /* [n_open] 各块的写指针 */
    uint32_t  n_open;        /* = CE 数 */
    uint32_t  open_rr;       /* 轮转指针：下一个写用哪个 open block */

    /*
     * S6：superblock。
     *
     * 一组"同时分配、横跨所有 CE"的块构成一个 superblock，
     * 条带写就是往这组块的同一页偏移上依次落页 —— 一次分配整组，
     * 组内块的写指针同步推进，因此它们会一起写满、一起变成回收候选。
     *
     * blk_sb[pbn] 记的是"这块属于哪一批"，只用于 GC 挑 victim 时
     * 优先凑同批的块，不参与正确性判定（取模、失效、归零都不影响数据）。
     * 它是纯 DRAM 状态，掉电即失 —— 恢复后新分配的块会重新建立起批次。
     */
    uint32_t  striping;      /* 0=逐个槽位补块（S4 行为），1=按 superblock 分配 */
    uint64_t *blk_sb;        /* [total_blocks]，0 = 未参与任何批次 */
    uint64_t  sb_seq;        /* 批次号分配器 */

    /*
     * 在飞的异步写（按提交顺序排列）。
     *
     * 快照必须是一个"一致点"，而异步写在提交那一刻就把映射改了 ——
     * 快照若照抄当前映射，就会记下"指向一个还没写进去的页"的条目，
     * 连旧版本一起丢掉：掉电后新页是撕裂页，旧页又因为不在扫描范围里
     * 而找不回来。所以下刷快照时要把这些写按 infl_old 临时回滚，
     * 刷完再按 infl_new 还原。
     *
     * 表满就少记几条（退化为乐观快照），
     * 正确性由恢复时的悬空映射清理兜底。
     */
    lba_t    *infl_lba;      /* [infl_cap] */
    ppn_t    *infl_old;      /* [infl_cap] 提交前的映射 */
    ppn_t    *infl_new;      /* [infl_cap] 提交后的映射 */
    uint32_t  infl_n;
    uint32_t  infl_cap;

    /* S3 策略参数 */
    uint32_t gc_policy;
    uint32_t wl_enable;
    uint32_t wl_pe_thresh;

    uint64_t gc_count;
    uint64_t gc_copied;
    uint64_t bg_gc_count;

    uint64_t wl_migrations;
    uint64_t wl_migrated_pages;

    /* 坏块：出厂坏块在介质层，运行期新增的在这里统计 */
    uint32_t bad_blocks;        /* 运行期隔离的坏块数 */
    uint32_t bb_saved_pages;    /* 隔离时抢救出来的有效页数 */

    uint64_t trim_cmds;
    uint64_t trim_pages;

    /* S6：checkpoint。cp_interval 来自配置（0=关闭，退化成 S5 的全盘重建） */
    uint32_t cp_interval;
    uint64_t cp_count;      /* 下刷了多少次快照 */
    uint64_t cp_ns;         /* 下刷快照花掉的虚拟时间 */

    /*
     * 搬移流水线暂存区（S4）：容量 = pages_per_block，一次性分配。
     *
     * GC 以前是一页一页 read_sync + prog_sync，整段回收期间盘上只有一个 CE
     * 在动，host 也被完全挡住 —— 实测 p99 完全由这段停顿决定（371ms）。
     * 改成"先把有效页并行读上来，再并行写出去"之后，
     * 搬移也能吃满多 CE 的带宽。
     */
    ftl_move_slot_t  *move_slots;
    nand_page_meta_t *move_buf;
    ppn_t            *move_src;
    ppn_t            *move_dst;
    uint32_t          move_cap;

    /* 一次回收的 victim 列表（最多 n_open 个，当前只用第 0 个） */
    pbn_t            *gc_victims;      /* [n_open] */

    /* 本次回收是否发生在 host 的空闲时段（用于前后台计数） */
    bool              gc_background;

    bool     valid;
} ftl_dev_t;

/* ---------------- 生命周期 ---------------- */

int  ftl_init(ftl_dev_t *f, nand_dev_t *nand, const ssd_config_t *cfg);
void ftl_deinit(ftl_dev_t *f);

/* ---------------- 对外 IO ---------------- */

int ftl_write(ftl_dev_t *f, lba_t lba, uint32_t seq);
int ftl_read (ftl_dev_t *f, lba_t lba, nand_page_meta_t *out);

/* ---------------- S4：异步 IO（调度器使用） ---------------- */

/*
 * 异步写。哪些部分同步、哪些异步是这个接口的核心：
 *
 *   - FTL 处理（查映射、分配物理页）是同步的。真实固件里这些是 DRAM/CPU 操作，
 *     相比介质操作可以忽略；而一旦分配撞到水位线触发前台 GC，就必须同步等待
 *     —— 这正是"前台 GC 停顿"的来源，不能藏起来，否则延迟分布是假的。
 *   - 页编程是异步的：提交后立刻返回，完成时回调通知。
 *     多个请求因此在通道与 CE 上真正重叠起来。
 */
int ftl_write_async(ftl_dev_t *f, lba_t lba, uint32_t seq, ppn_t *out_ppn,
                    nand_done_cb cb, void *arg);

/*
 * 页编程完成后的对账（S4 并发的关键一步）。
 *
 * 异步提交之后，同一个 lba 完全可能又被写了第二次：
 * 第一次的页此时在介质上还是 PENDING，等它落地时，映射早就指向新页了。
 * 若不处理，介质层会把这一页标成 VALID 留在块里，
 * 之后 GC 会把它当成"有效数据"搬走并更新 L2P —— 映射退回旧版本，
 * host 读到一个过时的值。这种错误不会报任何错，只会静默回退。
 *
 * 所以每一页编程完成时都要问一句：它还是不是最新的那个？
 * 不是就立刻判失效。
 */
void ftl_prog_settled(ftl_dev_t *f, lba_t lba, ppn_t ppn, int status);

/*
 * 一笔异步写彻底失败（重试到头）之后的收尾。
 *
 * 提交即更新映射的代价就在这里：页没写进去，映射却已经指向它了。
 * 不回滚的话，这个 lba 既读不到新数据、也读不到旧数据 ——
 * 一次可恢复的编程故障被放大成"数据没了"。
 * 回滚到提交前的位置，旧数据就还在；那个没写进去的页判成垃圾即可。
 */
void ftl_write_abandon(ftl_dev_t *f, lba_t lba, ppn_t ppn, ppn_t old_ppn);

/* 异步读。lba 从未写过时不惊动介质，直接返回 SSD_ERR_NO_SPACE（"没数据"）。 */
int ftl_read_async (ftl_dev_t *f, lba_t lba, nand_page_meta_t *out,
                    nand_done_cb cb, void *arg);

/* TRIM（S3）：告诉 FTL 这段逻辑地址不再需要，回收时不搬这些页 */
int ftl_trim (ftl_dev_t *f, lba_t lba, uint32_t nr_pages);

/*
 * flush（S5 才有实质内容）。
 *
 * S2~S4 没有写缓存，页编程都是"提交即落介质"，所以这里没有数据要下刷。
 * 真正需要刷的是 system 区的元数据：写序号和 TRIM 位图。
 * host 发 flush 的语义是"此前完成的写必须能扛住掉电"——
 * 对页数据我们本来就满足，但元数据不落下去，掉电后重建就会选错版本。
 */
int ftl_flush(ftl_dev_t *f);

/*
 * 下刷一份检查点快照（S6）。
 *
 * 快照是"某个时刻的一致性镜像"，但 FTL 是异步的：提交的写在介质上还处于
 * PENDING，而映射表在提交那一刻就已经指向它们了。
 * 于是快照里可能包含"指向一个还没真正写进去的页"的条目。
 *
 * 这不是 bug，但恢复时必须处理：这类映射一律作废（见 recovery.c 的
 * 悬空映射清理）。语义上完全站得住 —— 那一笔写 host 根本没收到回执，
 * 掉电丢掉是合法的；真正不能丢的是已经 ack 过的写，而它们要么
 * 已经在介质上（页已编程完成），要么落在活跃块里等着被扫出来。
 */
int ftl_checkpoint(ftl_dev_t *f);

/* ---------------- 内部：块池与页分配（GC / WL 也用） ---------------- */

/* host 写分配：受 rsv_min 水位保护，取不到就返回 INVALID（调用方去触发 GC）。
 * 开启 WL 时，会在栈顶若干候选里挑 P/E 次数最小的块（动态磨损均衡）。 */
pbn_t ftl_take_free_block(ftl_dev_t *f);
/* GC destination：可以取到池底，保证 GC 永远有地方可写 */
pbn_t ftl_take_gc_block(ftl_dev_t *f);
void  ftl_return_block(ftl_dev_t *f, pbn_t pbn);
int   ftl_alloc_ppn(ftl_dev_t *f, ppn_t *out);

/* 找一个剩余空间装得下 need_pages 的 open block，用于 GC/隔离时复用。
 *
 * 为什么必须先问这一句：如果复用 open block 装到一半才发现地方不够，
 * 就得中途再取一个块；万一那时池子空了，victim 擦不掉、新块也没拿到，
 * 就凭空少一个可用块 —— 坏块一多，盘会被这样活活耗死。 */
bool     ftl_open_reuse(ftl_dev_t *f, uint32_t need_pages,
                        pbn_t *dest, uint32_t *dest_wp);

/* open block 槽位的增删改查（GC / 坏块隔离 / 分配路径共用） */
uint32_t ftl_open_find(const ftl_dev_t *f, pbn_t pbn);   /* 找不到返回 n_open */
void     ftl_open_set(ftl_dev_t *f, uint32_t idx, pbn_t pbn, uint32_t wp);
void     ftl_open_adopt(ftl_dev_t *f, pbn_t pbn, uint32_t wp);   /* 放进空槽，否则关闭 */
void     ftl_open_drop(ftl_dev_t *f, pbn_t pbn);                 /* 坏块退出写入轮换 */
bool     ftl_open_take_victim(ftl_dev_t *f, pbn_t *out);         /* 强关一个当 GC victim */

/* 内部共享：搬移与 destination 收尾由 gc.c 实现，磨损均衡复用同一套逻辑。
 *
 * copied 是搬成的页数；lost 是"重试后仍然写不出去"的页数。
 * lost > 0 意味着这一页的数据还在 src 块里 —— 调用方此时绝对不能擦除 src，
 * 否则就是实打实的丢数据（1% 编程故障下这一路径必现，见 gc.c 的说明）。 */
int  ftl_move_valid_pages(ftl_dev_t *f, pbn_t src, pbn_t *dest,
                          uint32_t *dest_wp, bool *reused_open,
                          uint32_t *copied, uint32_t *lost);
void ftl_release_dest(ftl_dev_t *f, pbn_t dest, uint32_t dest_wp, bool reused_open);

/* GC 用的条带搬移（S4）：有效页轮转写进每个 CE 的写入槽位，
 * 让一次回收的编程操作摊到多颗 die 上。
 * 目标块就是 host 的写入块，搬完写指针前移即可，无需交还。 */
int  ftl_move_striped(ftl_dev_t *f, pbn_t src, uint32_t *copied, uint32_t *lost);

/* 从池里取块，优先取属于指定 CE 的（不分水位，GC 用） */
pbn_t ftl_take_gc_block_ce(ftl_dev_t *f, uint32_t ce_idx);

/* ---------------- S5：上电恢复（SPOR） ---------------- */

/*
 * 掉电之后 FTL 在 DRAM 里的东西全部消失：
 * 映射表、空闲块池、写入块、有效页计数、写序号、以及所有计数器。
 *
 * 唯一还站着的是介质：每个已编程页的 spare 里写着 (lba, seq)。
 * 重建就是靠这份信息把上面的东西全部算回来 —— 这也是为什么
 * 从 S2 起就把 P2L 写进页元数据，而不是只在 DRAM 里维护反向映射表。
 *
 * 恢复结果记账：既是诊断信息，也是测试可以断言的量化指标。
 */
typedef struct ftl_recovery {
    uint64_t scanned_blocks;
    uint64_t scanned_pages;
    uint64_t torn_pages;    /* 撕裂页：半写的页，一律丢弃 */
    uint64_t stale_pages;   /* 同一 lba 的旧版本页，判为失效 */
    uint64_t orphan_pages;  /* lba 越界 / 元数据 CRC 错 / 已被 TRIM：不参与重建 */
    uint64_t mapped_lbas;   /* 重建出来的映射条目数 */
    uint64_t open_blocks;   /* 恢复出多少个可以继续往下写的块 */
    uint64_t free_blocks;
    uint64_t bad_blocks;    /* 介质上的坏块总数（含出厂） */
    uint64_t write_seq;     /* 恢复后的写序号 */
    uint64_t scan_ns;       /* 上电扫描花掉的虚拟时间 */

    /* S6：走 checkpoint 时的额外信息 */
    uint64_t from_checkpoint;  /* 1=本次重建以快照为基线（只扫活跃块） */
    uint64_t cp_lbas_fixed;    /* 快照里指向无效页、被作废的映射条目数 */
    uint64_t cp_load_ns;       /* 读回快照花掉的时间 */
} ftl_recovery_t;

/*
 * 上电重建。调用前的状态是：nand_power_cut 已经执行过，
 * ftl_init 也已经重新分配好 DRAM 结构（但里面的内容是"新盘"那套）。
 *
 * 与 ftl_init 的区别只在"池子怎么来"：init 认为所有非坏块都空闲，
 * recover 必须逐块扫过介质才能知道谁空着、谁在写、谁写满了。
 */
int ftl_recover(ftl_dev_t *f, ftl_recovery_t *out);

/* ---------------- 空间回收 ---------------- */

/* 前台 GC：回收一个块（host 写撞到水位线时被迫调用） */
int   ftl_gc_one(ftl_dev_t *f);
/* 后台 GC：把空闲块补到 bg_target，提前回收换取更平稳的延迟 */
int   ftl_bg_gc(ftl_dev_t *f);

/* ---------------- 磨损均衡 ---------------- */

/* 静态 WL：把低 PE 块里的冷数据搬到高 PE 块，让低 PE 块回到写入轮换中 */
int   ftl_wl_static_once(ftl_dev_t *f);
void  ftl_pe_stats(const ftl_dev_t *f, ftl_pe_stat_t *st);

/* ---------------- 坏块管理 ---------------- */

/* 隔离坏块：抢救块内有效页 -> 标记 BAD -> 永不归还池 */
int   ftl_isolate_bad_block(ftl_dev_t *f, pbn_t pbn);

/* ---------------- 查询 ---------------- */

/* 当前映射（调度器用它记下"提交前的旧位置"，写失败时好回滚） */
ppn_t    ftl_l2p_get(const ftl_dev_t *f, lba_t lba);
lba_t    ftl_user_lbas(const ftl_dev_t *f);
uint32_t ftl_free_blocks(const ftl_dev_t *f);
uint32_t ftl_bad_blocks(const ftl_dev_t *f);

#endif /* SSD_FTL_FTL_H */
