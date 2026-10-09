#include "core/clock.h"

static uint64_t g_now_ns = 0;

void ssd_clock_init(void)
{
    g_now_ns = 0;
}

uint64_t ssd_clock_now(void)
{
    return g_now_ns;
}

void ssd_clock_set(uint64_t abs_ns)
{
    g_now_ns = abs_ns;
}

void ssd_clock_advance(uint64_t delta_ns)
{
    g_now_ns += delta_ns;
}
