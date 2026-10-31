/* Stand-in for libctru's <3ds.h> for the virtual-time model (Tests/vsim): the one call enet_3ds.c makes is the ARM11 system tick, here
   the model's own clock at the same rate. */
#pragma once
#include <stdint.h>
#define SYSCLOCK_ARM11 268111856ULL
extern uint64_t vsim_now_us(void);
static inline unsigned long long svcGetSystemTick(void) {
    return (unsigned long long)((double)vsim_now_us() * 268.111856);
}
