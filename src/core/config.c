#include "core/config.h"
#include "core/log.h"
#include "ssd_types.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 字段表：用于配置文件与命令行的统一映射                              */
/* ------------------------------------------------------------------ */
typedef enum cfg_type {
    CFG_T_U32,
    CFG_T_LEVEL
} cfg_type_t;

typedef struct cfg_field {
    const char *name;
    cfg_type_t  type;
    size_t      off;
} cfg_field_t;

static const cfg_field_t kFields[] = {
    { "channels",   CFG_T_U32,   offsetof(ssd_config_t, channels)          },
    { "ce",         CFG_T_U32,   offsetof(ssd_config_t, ces_per_ch)        },
    { "dies",       CFG_T_U32,   offsetof(ssd_config_t, dies_per_ce)       },
    { "planes",     CFG_T_U32,   offsetof(ssd_config_t, planes_per_die)    },
    { "blocks",     CFG_T_U32,   offsetof(ssd_config_t, blocks_per_plane)  },
    { "pages",      CFG_T_U32,   offsetof(ssd_config_t, pages_per_block)   },
    { "page_size",  CFG_T_U32,   offsetof(ssd_config_t, page_data_size)    },
    { "spare_size", CFG_T_U32,   offsetof(ssd_config_t, page_spare_size)   },
    { "t_prog",     CFG_T_U32,   offsetof(ssd_config_t, t_prog_ns)         },
    { "t_read",     CFG_T_U32,   offsetof(ssd_config_t, t_read_ns)         },
    { "t_bers",     CFG_T_U32,   offsetof(ssd_config_t, t_bers_ns)         },
    { "t_xfer",     CFG_T_U32,   offsetof(ssd_config_t, t_xfer_ns)         },
    { "pe_limit",   CFG_T_U32,   offsetof(ssd_config_t, pe_limit)          },
    { "ecc",        CFG_T_U32,   offsetof(ssd_config_t, ecc_bits_per_1kb)  },
    { "factory_bb", CFG_T_U32,   offsetof(ssd_config_t, factory_bb_permille) },
    { "fault_inject", CFG_T_U32, offsetof(ssd_config_t, fault_inject)       },
    { "fault_rate",   CFG_T_U32, offsetof(ssd_config_t, fault_rate_permille) },
    { "op",         CFG_T_U32,   offsetof(ssd_config_t, op_percent)        },
    { "seed",       CFG_T_U32,   offsetof(ssd_config_t, seed)              },
    { "log_level",  CFG_T_LEVEL, offsetof(ssd_config_t, log_level)         }
};

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */
static char *cfg_trim(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static int cfg_parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || s[0] == '\0' || s[0] == '-') {
        return SSD_ERR_INVAL;
    }
    v = strtoul(s, &end, 0);
    if (end == s || (end != NULL && *end != '\0')) {
        return SSD_ERR_INVAL;
    }
    if (v > 0xFFFFFFFFUL) {
        return SSD_ERR_INVAL;
    }
    *out = (uint32_t)v;
    return SSD_OK;
}

static int cfg_parse_level(const char *s, int *out)
{
    static const char *const kNames[SSD_LOG_LEVEL_MAX] = {
        "err", "warn", "info", "debug", "trace"
    };
    uint32_t v;
    int i;

    if (cfg_parse_u32(s, &v) == SSD_OK) {
        if (v >= (uint32_t)SSD_LOG_LEVEL_MAX) {
            return SSD_ERR_INVAL;
        }
        *out = (int)v;
        return SSD_OK;
    }
    for (i = 0; i < SSD_LOG_LEVEL_MAX; i++) {
        if (strcmp(s, kNames[i]) == 0) {
            *out = i;
            return SSD_OK;
        }
    }
    return SSD_ERR_INVAL;
}

static const cfg_field_t *cfg_find(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) {
        if (strcmp(kFields[i].name, name) == 0) {
            return &kFields[i];
        }
    }
    return NULL;
}

static int cfg_set_by_name(ssd_config_t *cfg, const char *name, const char *val)
{
    const cfg_field_t *f = cfg_find(name);

    if (f == NULL) {
        return SSD_ERR_INVAL;
    }

    if (f->type == CFG_T_U32) {
        uint32_t v = 0;
        if (cfg_parse_u32(val, &v) != SSD_OK) {
            return SSD_ERR_INVAL;
        }
        *(uint32_t *)((char *)cfg + f->off) = v;
    } else {
        int v = 0;
        if (cfg_parse_level(val, &v) != SSD_OK) {
            return SSD_ERR_INVAL;
        }
        *(int *)((char *)cfg + f->off) = v;
    }
    return SSD_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
void ssd_config_set_defaults(ssd_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    /* 小规模拓扑：便于快速仿真，同时保留完整层次结构 */
    cfg->channels         = 2;
    cfg->ces_per_ch       = 2;
    cfg->dies_per_ce      = 1;
    cfg->planes_per_die   = 2;
    cfg->blocks_per_plane = 128;
    cfg->pages_per_block  = 256;

    cfg->page_data_size   = 16u * 1024u;
    cfg->page_spare_size  = 2048u;

    /* TLC 典型数量级 */
    cfg->t_prog_ns        = 2000000u;   /* 2.0 ms */
    cfg->t_read_ns        = 60000u;     /* 60 us  */
    cfg->t_bers_ns        = 5000000u;   /* 5.0 ms */
    cfg->t_xfer_ns        = 20000u;     /* 16KB @ ~800MB/s per channel */

    cfg->pe_limit         = 3000u;
    cfg->ecc_bits_per_1kb = 60u;
    cfg->factory_bb_permille = 5u;      /* 0.5% */

    /* 默认关闭故障注入：常规实验不应被随机故障污染 */
    cfg->fault_inject        = 0u;
    cfg->fault_rate_permille = 1u;      /* 开启时 0.1% */

    cfg->op_percent       = 7u;

    cfg->seed             = 1u;
    cfg->log_level        = SSD_LOG_INFO;

    cfg->total_planes     = 0;
    cfg->total_blocks     = 0;
    cfg->total_pages      = 0;
    cfg->reserved_blocks  = 0;
    cfg->user_blocks      = 0;
    cfg->user_pages       = 0;
    cfg->capacity_bytes   = 0;
}

int ssd_config_load_file(ssd_config_t *cfg, const char *path)
{
    FILE *fp;
    char line[256];

    if (cfg == NULL || path == NULL) {
        return SSD_ERR_INVAL;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        return SSD_ERR_INVAL;
    }

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *key;
        char *val;
        char *hash;
        char *eq;

        hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = cfg_trim(line);
        val = cfg_trim(eq + 1);
        if (key[0] == '\0' || val[0] == '\0') {
            continue;
        }
        if (cfg_set_by_name(cfg, key, val) != SSD_OK) {
            fprintf(stderr, "[cfg] skip unknown/invalid entry: %s = %s\n", key, val);
        }
    }

    fclose(fp);
    return SSD_OK;
}

int ssd_config_parse_args(ssd_config_t *cfg, int argc, char **argv)
{
    int i;

    if (cfg == NULL || argv == NULL) {
        return SSD_ERR_INVAL;
    }

    /* 第一遍：配置文件（先加载，后续命令行可覆盖） */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *path = NULL;

        if (strncmp(a, "--config=", 9) == 0) {
            path = a + 9;
        } else if (strcmp(a, "--config") == 0 && (i + 1) < argc) {
            path = argv[++i];
        } else {
            continue;
        }
        if (ssd_config_load_file(cfg, path) != SSD_OK) {
            fprintf(stderr, "[cfg] cannot open config file: %s\n", path);
            return SSD_ERR_INVAL;
        }
    }

    /* 第二遍：其余选项 */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        char key[64];
        const char *val = NULL;
        const char *eq;
        size_t klen;

        if (strncmp(a, "--", 2) != 0) {
            fprintf(stderr, "[cfg] unexpected argument: %s\n", a);
            return SSD_ERR_INVAL;
        }

        a += 2;  /* 跳过 -- */

        eq = strchr(a, '=');
        if (eq != NULL) {
            klen = (size_t)(eq - a);
            val  = eq + 1;
        } else {
            klen = strlen(a);
            if ((i + 1) < argc) {
                val = argv[++i];   /* 值以独立参数给出，注意同步消耗 */
            }
        }
        if (klen == 0 || klen >= sizeof(key) || val == NULL) {
            fprintf(stderr, "[cfg] malformed option: --%s\n", a);
            return SSD_ERR_INVAL;
        }
        memcpy(key, a, klen);
        key[klen] = '\0';

        if (strcmp(key, "config") == 0) {
            continue;   /* 已在第一遍加载，这里只跳过（值也已消耗） */
        }

        if (cfg_set_by_name(cfg, key, val) != SSD_OK) {
            fprintf(stderr, "[cfg] unknown or invalid option: --%s=%s\n", key, val);
            return SSD_ERR_INVAL;
        }
    }

    return SSD_OK;
}

int ssd_config_derive(ssd_config_t *c)
{
    if (c == NULL) {
        return SSD_ERR_INVAL;
    }

    if (c->channels == 0 || c->ces_per_ch == 0 || c->dies_per_ce == 0 ||
        c->planes_per_die == 0 || c->blocks_per_plane == 0 ||
        c->pages_per_block == 0 || c->page_data_size == 0) {
        fprintf(stderr, "[cfg] geometry parameters must be greater than 0\n");
        return SSD_ERR_INVAL;
    }
    if ((c->planes_per_die & (c->planes_per_die - 1u)) != 0u) {
        fprintf(stderr, "[cfg] warning: planes is usually a power of two (got %u)\n",
                c->planes_per_die);
    }
    if (c->op_percent >= 100u) {
        fprintf(stderr, "[cfg] op must be < 100\n");
        return SSD_ERR_INVAL;
    }
    if (c->t_prog_ns == 0u || c->t_read_ns == 0u || c->t_bers_ns == 0u) {
        fprintf(stderr, "[cfg] timing parameters must be greater than 0\n");
        return SSD_ERR_INVAL;
    }
    if (c->page_spare_size < 16u) {
        fprintf(stderr, "[cfg] spare area too small to hold page metadata\n");
        return SSD_ERR_INVAL;
    }

    c->total_planes = (uint64_t)c->channels * c->ces_per_ch *
                      c->dies_per_ce * c->planes_per_die;
    c->total_blocks = c->total_planes * c->blocks_per_plane;
    c->total_pages  = c->total_blocks * c->pages_per_block;

    /* OP 以块为粒度保留，避免出现"半个块"的 OP 空间 */
    c->reserved_blocks = c->total_blocks * c->op_percent / 100u;
    if (c->op_percent > 0u && c->reserved_blocks == 0u) {
        c->reserved_blocks = 1u;
    }
    c->user_blocks    = c->total_blocks - c->reserved_blocks;
    c->user_pages     = c->user_blocks * c->pages_per_block;
    c->capacity_bytes = c->user_pages * c->page_data_size;

    return SSD_OK;
}

void ssd_config_dump(const ssd_config_t *c)
{
    double gib;

    if (c == NULL) {
        return;
    }
    gib = (double)c->capacity_bytes / (1024.0 * 1024.0 * 1024.0);

    printf("================ configuration ================\n");
    printf("  topology        %u ch x %u ce x %u die x %u plane\n",
           c->channels, c->ces_per_ch, c->dies_per_ce, c->planes_per_die);
    printf("  blocks/plane    %u\n", c->blocks_per_plane);
    printf("  pages/block     %u\n", c->pages_per_block);
    printf("  page            %u B data + %u B spare\n",
           c->page_data_size, c->page_spare_size);
    printf("  total blocks    %llu\n", (unsigned long long)c->total_blocks);
    printf("  total pages     %llu\n", (unsigned long long)c->total_pages);
    printf("  OP              %u%%  (%llu reserved blocks)\n",
           c->op_percent, (unsigned long long)c->reserved_blocks);
    printf("  user capacity   %llu pages  (%.2f GiB)\n",
           (unsigned long long)c->user_pages, gib);
    printf("  timing          prog %u ns | read %u ns | erase %u ns | xfer %u ns\n",
           c->t_prog_ns, c->t_read_ns, c->t_bers_ns, c->t_xfer_ns);
    printf("  reliability     pe_limit %u | ecc %u bit/1KB | factory_bb %u/1000\n",
           c->pe_limit, c->ecc_bits_per_1kb, c->factory_bb_permille);
    printf("  run             seed %u | log_level %d\n", c->seed, c->log_level);
    printf("================================================\n");
}

void ssd_config_usage(const char *prog)
{
    const char *name = (prog != NULL) ? prog : "ssd-sim";

    printf("usage: %s [--config=<file>] [--key=value ...]\n\n", name);
    printf("geometry:\n");
    printf("  --channels=N --ce=N --dies=N --planes=N --blocks=N --pages=N\n");
    printf("  --page_size=N --spare_size=N\n");
    printf("timing (ns):\n");
    printf("  --t_prog=N --t_read=N --t_bers=N --t_xfer=N\n");
    printf("reliability:\n");
    printf("  --pe_limit=N --ecc=N --factory_bb=N\n");
    printf("  --fault_inject=N --fault_rate=N\n");
    printf("ftl:\n");
    printf("  --op=N\n");
    printf("run:\n");
    printf("  --seed=N --log_level=err|warn|info|debug|trace\n");
}
