#ifndef SSD_CORE_MEMPOOL_H
#define SSD_CORE_MEMPOOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * 静态内存池（bump allocator）。
 *
 * 设计取向：固件里不存在"运行时 malloc 失败"这种优雅降级路径，
 * 元数据内存全部在初始化阶段一次性预留。因此这里只提供 alloc / reset，
 * 不提供单块 free；池耗尽直接 panic，提示把容量调大。
 */

typedef struct ssd_mempool {
    uint8_t *base;
    size_t   capacity;
    size_t   used;
    size_t   peak;
} ssd_mempool_t;

void   ssd_pool_init(ssd_mempool_t *pool, void *buf, size_t capacity);
void  *ssd_pool_alloc(ssd_mempool_t *pool, size_t size, size_t align);
void   ssd_pool_reset(ssd_mempool_t *pool);

size_t ssd_pool_used(const ssd_mempool_t *pool);
size_t ssd_pool_peak(const ssd_mempool_t *pool);
size_t ssd_pool_capacity(const ssd_mempool_t *pool);

/* C99 没有 _Alignof，用 offsetof 技巧取类型对齐要求 */
#define SSD_ALIGNOF(type) offsetof(struct { char _c; type _x; }, _x)

/* 便捷封装：按类型分配并对齐到类型自身对齐要求 */
#define SSD_POOL_ALLOC_T(pool, type, count) \
    ((type *)ssd_pool_alloc((pool), sizeof(type) * (count), SSD_ALIGNOF(type)))

#endif /* SSD_CORE_MEMPOOL_H */
