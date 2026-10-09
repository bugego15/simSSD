#include "core/log.h"
#include "core/clock.h"

#include <stdarg.h>
#include <stdio.h>

static int g_log_level = SSD_LOG_INFO;

void ssd_log_init(int level)
{
    if (level < SSD_LOG_ERR) {
        level = SSD_LOG_ERR;
    } else if (level >= SSD_LOG_LEVEL_MAX) {
        level = SSD_LOG_LEVEL_MAX - 1;
    }
    g_log_level = level;
}

void ssd_log_set_level(int level)
{
    ssd_log_init(level);
}

int ssd_log_level(void)
{
    return g_log_level;
}

void ssd_log_write(int level, const char *file, int line, const char *fmt, ...)
{
    static const char kTag[SSD_LOG_LEVEL_MAX] = { 'E', 'W', 'I', 'D', 'T' };
    va_list ap;

    if (level < SSD_LOG_ERR || level >= SSD_LOG_LEVEL_MAX) {
        return;
    }
    if (level > g_log_level) {
        return;
    }

    fprintf(stderr, "[%c][%14llu] %s:%d: ",
            kTag[level], (unsigned long long)ssd_clock_now(), file, line);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
}
