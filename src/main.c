#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/log.h"
#include "core/stats.h"
#include "media/nand.h"

#include <stdio.h>
#include <string.h>

static void nand_dump_summary(const nand_dev_t *dev)
{
    printf("================ nand media ===================\n");
    printf("  channels        %u\n", dev->geo.channels);
    printf("  blocks          %llu\n", (unsigned long long)dev->geo.total_blocks);
    printf("  pages           %llu\n", (unsigned long long)dev->geo.total_pages);
    printf("  pages/block     %u\n", dev->geo.pages_per_block);
    printf("  factory bad     %u\n", dev->factory_bad_blocks);
    printf("  good blocks     %llu\n",
           (unsigned long long)(dev->geo.total_blocks - dev->factory_bad_blocks));
    printf("  fault inject    %s (rate %u/1000)\n",
           dev->fault_inject ? "on" : "off", dev->fault_rate_permille);
    printf("================================================\n");
}

int main(int argc, char **argv)
{
    ssd_config_t cfg;
    nand_dev_t dev;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            ssd_config_usage(argv[0]);
            return 0;
        }
    }

    ssd_config_set_defaults(&cfg);
    if (ssd_config_parse_args(&cfg, argc, argv) != SSD_OK) {
        ssd_config_usage(argv[0]);
        return 1;
    }
    if (ssd_config_derive(&cfg) != SSD_OK) {
        return 1;
    }

    ssd_clock_init();
    ssd_log_init(cfg.log_level);
    ssd_stats_init();

    ssd_config_dump(&cfg);

    if (nand_init(&dev, &cfg, 4096u) != SSD_OK) {
        SSD_ERR("nand_init failed");
        return 1;
    }
    nand_dump_summary(&dev);

    /* S1 阶段还没有 FTL，介质刚上电即空闲，这里只输出统计基线 */
    ssd_stats_dump();

    nand_deinit(&dev);
    return 0;
}
