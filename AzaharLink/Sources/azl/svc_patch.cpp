#include "azl/svc_patch.hpp"

namespace azl {

namespace {
constexpr u32 kPushLr = 0xE52DE004; // str lr, [sp, #-4]!
constexpr u32 kPopLr = 0xE49DE004;  // ldr lr, [sp], #4
} // namespace

bool BuildSvcTrampoline(u32 site, u32 next, u32 tramp_address, u32 entry, ReadWordFn read,
                        void* context, u32 words[kWordsPerTrampoline], const char** reason) {
    const RelocResult r = RelocateArm(next, site + 4, site + 8, read, context);
    if (!r.ok) {
        *reason = r.reason;
        return false;
    }

    // bl at tramp_address + 4
    const s32 offset = (static_cast<s32>(entry) - static_cast<s32>(tramp_address + 4 + 8)) / 4;
    if (offset < -0x800000 || offset >= 0x800000) {
        *reason = "entry point out of bl range";
        return false;
    }

    for (u32 i = 0; i < kWordsPerTrampoline; ++i) {
        words[i] = 0;
    }
    words[0] = kPushLr;
    words[1] = 0xEB000000 | (static_cast<u32>(offset) & 0x00FFFFFF);
    words[2] = kPopLr;
    // The relocated block is self-contained: its pc-relative offsets assume it starts at words[3].
    for (int i = 0; i < r.count; ++i) {
        words[3 + i] = r.words[i];
    }
    return true;
}

} // namespace azl
