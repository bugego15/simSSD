#ifndef SSD_CORE_LOG_H
#define SSD_CORE_LOG_H

/*
 * 分级日志。默认输出 stderr，带虚拟时钟前缀，便于把日志和
 * NAND 操作时序对齐分析。
 */

enum ssd_log_level {
    SSD_LOG_ERR   = 0,
    SSD_LOG_WARN  = 1,
    SSD_LOG_INFO  = 2,
    SSD_LOG_DEBUG = 3,
    SSD_LOG_TRACE = 4,
    SSD_LOG_LEVEL_MAX
};

void ssd_log_init(int level);
void ssd_log_set_level(int level);
int  ssd_log_level(void);

void ssd_log_write(int level, const char *file, int line, const char *fmt, ...);

#define SSD_LOG(level, ...) ssd_log_write((level), __FILE__, __LINE__, __VA_ARGS__)

#define SSD_ERR(...)   SSD_LOG(SSD_LOG_ERR,   __VA_ARGS__)
#define SSD_WARN(...)  SSD_LOG(SSD_LOG_WARN,  __VA_ARGS__)
#define SSD_INFO(...)  SSD_LOG(SSD_LOG_INFO,  __VA_ARGS__)
#define SSD_DBG(...)   SSD_LOG(SSD_LOG_DEBUG, __VA_ARGS__)
#define SSD_TRACE(...) SSD_LOG(SSD_LOG_TRACE, __VA_ARGS__)

#endif /* SSD_CORE_LOG_H */
