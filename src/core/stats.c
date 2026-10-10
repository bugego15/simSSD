#include "core/stats.h"
#include "core/assert.h"
#include "core/clock.h"

#include <stdio.h>

static uint64_t g_stats[ST_COUNT];

static const char *const kStatName[ST_COUNT] = {
    "host.read_cmds",
    "host.write_cmds",
    "host.trim_cmds",
    "host.flush_cmds",
    "host.read_pages",
    "host.write_pages",
    "nand.prog_pages",
    "nand.read_pages",
    "nand.erase_blocks",
    "ftl.gc_erases",
    "ftl.gc_copy_pages",
    "ftl.wl_migrate_pages",
    "ftl.badblock_runtime",
    "spor.recoveries",
    "spor.torn_pages",
    "spor.stale_pages",
    "sim.media_busy_ns"
};

void ssd_stats_init(void)
{
    ssd_stats_reset();
}

void ssd_stats_reset(void)
{
    int i;
    for (i = 0; i < ST_COUNT; i++) {
        g_stats[i] = 0;
    }
}

void ssd_stats_inc(enum ssd_stat_id id)
{
    SSD_ASSERT(id >= 0 && id < ST_COUNT);
    g_stats[id]++;
}

void ssd_stats_add(enum ssd_stat_id id, uint64_t delta)
{
    SSD_ASSERT(id >= 0 && id < ST_COUNT);
    g_stats[id] += delta;
}

uint64_t ssd_stats_get(enum ssd_stat_id id)
{
    SSD_ASSERT(id >= 0 && id < ST_COUNT);
    return g_stats[id];
}

double ssd_stats_waf(void)
{
    uint64_t host = g_stats[ST_HOST_WRITE_PAGES];
    if (host == 0) {
        return 0.0;
    }
    return (double)g_stats[ST_NAND_PROG_PAGES] / (double)host;
}

double ssd_stats_media_utilization(void)
{
    uint64_t now = ssd_clock_now();
    if (now == 0) {
        return 0.0;
    }
    return (double)g_stats[ST_MEDIA_BUSY_NS] / (double)now;
}

double ssd_stats_gc_overhead(void)
{
    uint64_t host = g_stats[ST_HOST_WRITE_PAGES];
    if (host == 0) {
        return 0.0;
    }
    return (double)g_stats[ST_GC_COPY_PAGES] / (double)host;
}

void ssd_stats_dump(void)
{
    int i;

    printf("================ statistics ================\n");
    for (i = 0; i < ST_COUNT; i++) {
        printf("  %-24s %20llu\n", kStatName[i], (unsigned long long)g_stats[i]);
    }
    printf("---------------- derived -------------------\n");
    printf("  %-24s %20.3f\n", "write_amplification", ssd_stats_waf());
    printf("  %-24s %20.3f\n", "gc_overhead", ssd_stats_gc_overhead());
    printf("  %-24s %20.2f%%\n", "media_utilization",
           ssd_stats_media_utilization() * 100.0);
    printf("  %-24s %20llu ns\n", "sim_time", (unsigned long long)ssd_clock_now());
    printf("============================================\n");
}
