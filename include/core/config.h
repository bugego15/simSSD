#ifndef SSD_CORE_CONFIG_H
#define SSD_CORE_CONFIG_H

#include <stdint.h>

/*
 * 全参数化配置：几何、时序、可靠性、FTL 策略、运行参数。
 *
 * 配置来源优先级（后者覆盖前者）：
 *   内置默认值  ->  --config=<file>  ->  命令行 --key=value
 *
 * 所有随机行为由 seed 决定，保证实验可复现，这是做 A/B 对比的前提。
 */

typedef struct ssd_config {
    /* ---------- 介质几何 ---------- */
    uint32_t channels;
    uint32_t ces_per_ch;
    uint32_t dies_per_ce;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_data_size;    /* data 区字节数 */
    uint32_t page_spare_size;   /* spare 区字节数（放 P2L 等元数据） */

    /* ---------- 时序（ns） ---------- */
    uint32_t t_prog_ns;         /* page program */
    uint32_t t_read_ns;         /* page read */
    uint32_t t_bers_ns;         /* block erase */
    uint32_t t_xfer_ns;         /* 通道传输一个 data page */

    /* ---------- 可靠性 ---------- */
    uint32_t pe_limit;          /* 标称 P/E 次数 */
    uint32_t ecc_bits_per_1kb;  /* ECC 纠错能力 */
    uint32_t factory_bb_permille; /* 出厂坏块比例，千分比 */
    uint32_t fault_inject;        /* 0/1：是否注入随机故障 */
    uint32_t fault_rate_permille; /* 单次操作故障概率，千分比 */

    /* ---------- FTL ---------- */
    uint32_t op_percent;        /* 过度供给 OP，百分比 */

    /* ---------- 运行 ---------- */
    uint32_t seed;
    int      log_level;

    /* ---------- 派生量（由 ssd_config_derive 计算） ---------- */
    uint64_t total_planes;
    uint64_t total_blocks;
    uint64_t total_pages;
    uint64_t reserved_blocks;
    uint64_t user_blocks;
    uint64_t user_pages;
    uint64_t capacity_bytes;
} ssd_config_t;

void ssd_config_set_defaults(ssd_config_t *cfg);
int  ssd_config_load_file(ssd_config_t *cfg, const char *path);
int  ssd_config_parse_args(ssd_config_t *cfg, int argc, char **argv);
int  ssd_config_derive(ssd_config_t *cfg);
void ssd_config_dump(const ssd_config_t *cfg);
void ssd_config_usage(const char *prog);

#endif /* SSD_CORE_CONFIG_H */
