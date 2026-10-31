// The game's GSPGPU::RegisterInterruptRelayQueue wrapper, as CTRPluginFramework (libctrpf 0.8.0, Services::GSP::Initialize) looks for it.
//
// The framework finds the wrapper by instruction pattern in the game's .text and, if it does not find it, calls abort() before the game
// has started ("Fatal plugin error ... abort() called", then a Luma crash dump with R6 = 0xFFFFFFFF, the -1 of GSP::Initialize). Two
// patterns exist: the libctru-compiled wrapper, which has `svc 0x32` inline, and the Nintendo SDK one, which is recognised by its prologue and
// by the command header 0x00130042 in the following 0x50 bytes.
//
// LEGO Batman 2 (0004000000 05A400) has the libctru wrapper with ONE word different: the `svc 0x32` is not inline but `bl <stub>`, where the
// stub is the game's single `svc 0x32 ; bx lr`. The two are the same for the CPU (the wrapper pops pc, so the lr that `bl` clobbers is not used
// and `svc` keeps the flags), so the plugin writes the `svc 0x32` in place of the `bl` before the framework searches, and the framework finds a
// wrapper it knows. Portable on purpose: the host tests run the very same code over the real image of the game.
#pragma once

#include "azl/arm_reloc.hpp"
#include "azl/common.hpp"

namespace azl {

constexpr u32 kSvcSendSyncRequestWord = 0xEF000032; // svc 0x32
constexpr u32 kBxLrWord = 0xE12FFF1E;               // bx lr

constexpr u32 kGspRegisterInterruptHeader = 0x00130042;

// The wrapper compiled by libctru: the framework hooks the `svc 0x32` at +0x2C.
constexpr u32 kGspWrapperCtru[] = {
    0xE92D4070, 0xE1A06003, 0xE59D5010, 0xEE1D3F70, 0xE2834080, 0xE59F3034, 0xE3A0C000,
    0xE584100C, 0xE9841004, 0xE5843000, 0xE5900000, 0xEF000032, 0xE2101102, 0x4A000004,
    0xE5940008, 0xE5850000, 0xE5940010, 0xE5860000, 0xE5940004, 0xE8BD8070, 0x00130042};
constexpr u32 kGspWrapperCtruWords = sizeof(kGspWrapperCtru) / sizeof(kGspWrapperCtru[0]);
constexpr u32 kGspWrapperCtruSvcIndex = 11;
constexpr u32 kGspWrapperCtruSvcOffset = 4 * kGspWrapperCtruSvcIndex; // 0x2C

// The wrapper compiled by the Nintendo SDK: the framework hooks the `svc 0x32` at +0x28.
constexpr u32 kGspWrapperSdk[] = {0xE92D4070, 0xE1A05003, 0xE59D6010, 0xEE1D4F70, 0xE59FC034,
                                  0xE3A03000, 0xE5A4C080, 0xE584100C, 0xE1C420F4};
constexpr u32 kGspWrapperSdkWords = sizeof(kGspWrapperSdk) / sizeof(kGspWrapperSdk[0]);
constexpr u32 kGspWrapperSdkSvcOffset = 0x28;
constexpr u32 kGspWrapperSdkHeaderWindow = 0x50;

/// The target of an ARM `bl` (condition always) found at `address`; false if `insn` is not one.
inline bool ArmBlTarget(u32 insn, u32 address, u32* target) {
    if ((insn >> 24) != 0xEB) {
        return false;
    }
    u32 offset = insn & 0x00FFFFFF;
    if (offset & 0x00800000) {
        offset |= 0xFF000000; // sign extension: the offset counts words
    }
    *target = address + 8 + (offset << 2);
    return true;
}

/// Looks, in [first, end), for the libctru GSP wrapper whose `svc 0x32` is NOT inline but behind a shared stub, and stores the address of
/// the `bl` that has to become the `svc` (wrapper + 0x2C) in `sites`. Everything else in the 21 words must be the pattern's, the target of
/// the `bl` must be exactly `svc 0x32 ; bx lr` and must lie in [first, end). Returns how many it found (at most `max`).
inline u32 FindStubbedGspWrappers(ReadWordFn read, void* context, u32 first, u32 end, u32* sites, u32 max) {
    u32 found = 0;
    for (u32 addr = first; found < max && addr + 4 * kGspWrapperCtruWords <= end; addr += 4) {
        if (read(context, addr) != kGspWrapperCtru[0]) {
            continue; // push {r4-r6, lr}
        }
        bool same = true;
        for (u32 i = 1; i < kGspWrapperCtruWords && same; ++i) {
            if (i != kGspWrapperCtruSvcIndex && read(context, addr + 4 * i) != kGspWrapperCtru[i]) {
                same = false;
            }
        }
        if (!same) {
            continue;
        }
        const u32 site = addr + kGspWrapperCtruSvcOffset;
        u32 stub = 0;
        if (!ArmBlTarget(read(context, site), site, &stub) || stub < first || stub + 8 > end || (stub & 3) != 0) {
            continue;
        }
        if (read(context, stub) != kSvcSendSyncRequestWord || read(context, stub + 4) != kBxLrWord) {
            continue;
        }
        sites[found++] = site;
    }
    return found;
}

} // namespace azl
