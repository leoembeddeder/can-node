/* SPDX-License-Identifier: MIT */
#include "posix_platform.h"

#include <time.h>

static uint64_t s_init_ns;

uint64_t posix_monotonic_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

void posix_clock_init(void)
{
    s_init_ns = posix_monotonic_ns();
}

uint64_t plat_time_ns(void)
{
    return posix_monotonic_ns();
}

uint64_t plat_run_time_counter_us(void)
{
    return (posix_monotonic_ns() - s_init_ns) / 1000ULL;
}
