#include "ssd_types.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/log.h"
#include "core/stats.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    ssd_config_t cfg;
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
    SSD_INFO("skeleton ready: media model is not attached yet (S1)");

    return 0;
}
