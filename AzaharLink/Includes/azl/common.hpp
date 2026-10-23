// AzaharLink - common types and byte helpers.
// Licensed under GPLv2 or any later version (the UDS logic is a port of Azahar/Citra's nwm_uds).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace azl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

/// A 64-bit sum of system ticks that threads add to atomically (azl/stats.hpp). 8-byte aligned on every host (a 32-bit PC
/// build aligns a plain u64 to 4), which the atomic add and exchange need.
using Ticks64 = u64 __attribute__((aligned(8)));

using Mac = std::array<u8, 6>;

constexpr Mac kBroadcastMac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr Mac kNoPreferredMac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// All multi-byte accessors work on raw bytes so that they are safe on unaligned pointers and
// independent of the host endianness.
inline u16 LoadBE16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}
inline u32 LoadBE32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
inline u64 LoadBE64(const u8* p) {
    return (static_cast<u64>(LoadBE32(p)) << 32) | LoadBE32(p + 4);
}
inline u16 LoadLE16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
inline u32 LoadLE32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
inline u64 LoadLE64(const u8* p) {
    return static_cast<u64>(LoadLE32(p)) | (static_cast<u64>(LoadLE32(p + 4)) << 32);
}

inline void StoreBE16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
inline void StoreBE32(u8* p, u32 v) {
    StoreBE16(p, static_cast<u16>(v >> 16));
    StoreBE16(p + 2, static_cast<u16>(v));
}
inline void StoreBE64(u8* p, u64 v) {
    StoreBE32(p, static_cast<u32>(v >> 32));
    StoreBE32(p + 4, static_cast<u32>(v));
}
inline void StoreLE16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
inline void StoreLE32(u8* p, u32 v) {
    StoreLE16(p, static_cast<u16>(v));
    StoreLE16(p + 2, static_cast<u16>(v >> 16));
}
inline void StoreLE64(u8* p, u64 v) {
    StoreLE32(p, static_cast<u32>(v));
    StoreLE32(p + 4, static_cast<u32>(v >> 32));
}

} // namespace azl
