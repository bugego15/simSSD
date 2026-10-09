#ifndef SSD_TYPES_H
#define SSD_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 返回码：固件风格，0 成功，负值失败                                   */
/* ------------------------------------------------------------------ */
typedef enum ssd_err {
    SSD_OK          =  0,
    SSD_ERR_INVAL   = -1,   /* 参数非法 */
    SSD_ERR_IO      = -2,   /* 介质操作失败 */
    SSD_ERR_BADBLK  = -3,   /* 坏块 */
    SSD_ERR_UECC    = -4,   /* 不可纠正错误 */
    SSD_ERR_NO_SPACE= -5,   /* 无空闲资源 */
    SSD_ERR_NO_MEM  = -6,   /* 静态池耗尽（属于设计缺陷，不应发生） */
    SSD_ERR_TIMEOUT = -7,
    SSD_ERR_BUSY    = -8    /* 资源暂时不可用，需重试 */
} ssd_err_t;

/* ------------------------------------------------------------------ */
/* 地址类型                                                            */
/* ------------------------------------------------------------------ */
typedef uint64_t lba_t;     /* 主机逻辑块地址（以 page_data_size 为单位） */
typedef uint64_t ppn_t;     /* 线性物理页号 */
typedef uint64_t pbn_t;     /* 线性物理块号 */

#define SSD_INVALID_PPN  ((ppn_t)~0ULL)
#define SSD_INVALID_PBN  ((pbn_t)~0ULL)
#define SSD_INVALID_LBA  ((lba_t)~0ULL)

/* ------------------------------------------------------------------ */
/* 常用宏                                                              */
/* ------------------------------------------------------------------ */
#define SSD_UNUSED(x)       ((void)(x))
#define SSD_ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

#define SSD_KB (1024ULL)
#define SSD_MB (1024ULL * 1024ULL)
#define SSD_GB (1024ULL * 1024ULL * 1024ULL)

/* 要求 align 为 2 的幂；注意提升为 uint64 避免高位被截断 */
#define SSD_ALIGN_UP(x, a) \
    (((uint64_t)(x) + (uint64_t)((a) - 1u)) & ~((uint64_t)((a) - 1u)))

#define SSD_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define SSD_MAX(a, b) (((a) > (b)) ? (a) : (b))

#endif /* SSD_TYPES_H */
