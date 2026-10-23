/* Stand-in for libctru's <3ds.h> so that Sources/enet/enet_3ds.c (the plugin's ENet platform layer) builds and
   runs on a PC: the one call it makes is the ARM11 system tick, here a monotonic clock at the same rate. */
#pragma once
#include <stdint.h>
#include <time.h>
#define SYSCLOCK_ARM11 268111856ULL
static inline unsigned long long svcGetSystemTick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * SYSCLOCK_ARM11 +
           (unsigned long long)ts.tv_nsec * SYSCLOCK_ARM11 / 1000000000ULL;
}
