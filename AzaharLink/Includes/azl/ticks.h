/* Conversion of the ARM11 system tick (svcGetSystemTick, 268111856 Hz) to milliseconds / microseconds
   WITHOUT a 64-bit division. `ticks / 268111` costs ~130 instructions on the ARM11 (libgcc's
   __udivmoddi4: there is no hardware divider and the compiler does not turn a 64-bit division by a
   constant into a multiplication), and the plugin reads the clock a dozen times per network round and
   twice per IPC call it handles.

   ms = ticks * R_MS >> 48, us = ticks * R_US >> 40, with R = 2^k * 1000 (resp. 10^6) / 268111856 rounded,
   which fits in 32 bits, so the 64x32 product is two UMULLs. Relative error < 1e-9 (the previous
   `ticks / (SYSCLOCK_ARM11 / 1000)` used the truncated divisor 268111: 3 ppm fast). Plain C so that the
   ENet platform layer can use it too. */
#ifndef AZL_TICKS_H
#define AZL_TICKS_H

#include <stdint.h>

#define AZL_TICKS_PER_MS 268112u /* 268111.856 rounded: for thresholds and statistics, not for the clock */
#define AZL_TICKS_R_MS 1049841588u /* round(2^48 * 1000     / 268111856) */
#define AZL_TICKS_R_US 4100943704u /* round(2^40 * 1000000  / 268111856) */

/* (ticks * r) >> shift for shift >= 32, exact for ticks < 2^63 and r < 2^32 */
static inline uint64_t azl_ticks_mulshift(uint64_t ticks, uint32_t r, unsigned shift) {
    const uint32_t lo = (uint32_t)ticks;
    const uint32_t hi = (uint32_t)(ticks >> 32);
    const uint64_t a = (uint64_t)hi * r;
    const uint64_t b = (uint64_t)lo * r;
    const unsigned s = shift - 32;
    return (a >> s) + ((((a & ((1ull << s) - 1)) << 32) + b) >> shift);
}
static inline uint64_t azl_ticks_to_ms(uint64_t ticks) {
    return azl_ticks_mulshift(ticks, AZL_TICKS_R_MS, 48);
}
static inline uint64_t azl_ticks_to_us(uint64_t ticks) {
    return azl_ticks_mulshift(ticks, AZL_TICKS_R_US, 40);
}

#endif
