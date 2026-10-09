#ifndef SSD_CORE_ASSERT_H
#define SSD_CORE_ASSERT_H

/*
 * 固件风格断言：命中即 panic，不做"忽略继续"的容错。
 * 模拟器里断言命中意味着模型本身有 bug，必须停下来查。
 */

void ssd_panic(const char *file, int line, const char *expr);

#define SSD_ASSERT(expr) \
    do { if (!(expr)) { ssd_panic(__FILE__, __LINE__, #expr); } } while (0)

#define SSD_BUG(msg) \
    do { ssd_panic(__FILE__, __LINE__, (msg)); } while (0)

#define SSD_NOT_REACHED() SSD_BUG("unreachable code")

#endif /* SSD_CORE_ASSERT_H */
