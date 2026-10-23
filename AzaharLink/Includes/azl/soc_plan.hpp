// Where the socket layer's work buffer goes in the game's address space (pure arithmetic, no
// system calls, so it is unit-tested on the PC: Tests/soc_plan_test.cpp).
//
// What the documented memory map of the system, the kernel and the console say:
//   * Heap area [0x08000000, 0x0E000000) = 96 MiB, the same for CTR and SNAKE (New 3DS). The kernel
//     enforces the end: an ALLOC that starts in the area and ends above 0x0E000000 is refused
//     (0xE0E01BFD); one that starts at or above 0x0E000000 is accepted if the span is free.
//   * main program + heap + device memory <= 64 MB (CTR / standard mode) or 124 MB (SNAKE extended
//     application): the kernel enforces it as RESLIMIT_COMMIT, so it is read, never assumed.
//   * The SDK grows the heap contiguously from 0x08000000, so the highest address a heap can reach is
//         0x08000000 + min(96 MiB, commit_limit - commit_now - our_block).
//   * Library work buffers (UDS, socket, HTTP, ...) must be non-device memory (table 3-2, p16): an
//     ordinary MEMOP_ALLOC block of the heap region is exactly that (state 0xBB05, what
//     svcCreateMemoryBlock wants).
//   * 0x0E000000..0x10000000 is the documented "Reserved Region" (a game's SDK registers an address
//     range allocator over it); the main thread's stack sits just below 0x10000000.
//
// Placement, in order (the first candidate the kernel accepts wins):
//   1. HeapAreaTop    the top of the heap area, only while the game's heap provably cannot get there
//                     (every CTR/standard title). Hardware-proven on a New 3DS.
//   2. LimitTop       the top of the room the commit limit leaves the heap (0x08000000 + limit): for a
//                     64 MiB title that is 0x0C000000, inside the area of every kernel. Goes FIRST on a
//                     console whose application region is below 100 MiB (Old 3DS / 2DS), where nothing
//                     has run on hardware yet.
//   3. ReserveTop     just under the main thread's stack (extended-memory titles, whose heap could in
//                     theory reach the top of the heap area).
//   4. HeapAreaMargin the top 4 MiB of the heap area: last resort, not provable.
#pragma once

#include <cstdint>
#include <string>

#include "azl/rcvbuf.h"
#include "azl/screen_text.hpp"

namespace azl {
namespace ctr {
namespace socplan {

constexpr uint32_t kRegionBase = 0x08000000;   // start of the heap area (documented memory map)
constexpr uint32_t kHeapAreaEnd = 0x0E000000;  // end of the heap area, 96 MiB cap (documented memory map, kernel)
constexpr uint32_t kRegionEnd = 0x10000000;    // the main thread's stack ends here
constexpr uint32_t kHeapCap = kHeapAreaEnd - kRegionBase;
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kStep = 0x100000;
constexpr uint32_t kSafety = 0x100000;         // slack between the heap's reach and the block
constexpr uint32_t kDefaultTopEnd = 0x0FF00000; // used when the free span under the stack is unknown
constexpr uint32_t kNewConsoleRegion = 100u << 20; // application region >= this = New 3DS layout
constexpr int kProvableSlots = 16;
constexpr int kReserveSlots = 16;
constexpr int kMarginSlots = 4;
constexpr int kMaxCandidates = 2 * kProvableSlots + kReserveSlots + kMarginSlots;

// The block given to socInit(): ONE UDP socket with a 64 KB receive buffer. The soc service
// cuts the block into a buddy pool of 4 KiB pages in 64 KiB chunks and keeps only socket
// ring buffers in it: Initialize wants a multiple of 0x1000 of at least 0x10000 (else 0xD8E073EC), and
// 0x20000 is the least that holds one 64 KiB UDP ring (chunk 0: metadata and the default ring, chunk 1:
// the ring; a new ring is taken before the old one is freed). 0x10000 only holds a 32 KiB ring.
// Luma's minisoc uses 0x60000 (what 0.1.12-0.1.14 took: 256 KB idle); 0x20000 is 0.195% of a 64 MiB
// game's budget. A second socket with a 64 KiB ring would not fit, and there is none (one ENetHost).
constexpr uint32_t kSocBlockSize = 0x20000;
// Sizes tried in this order (the kernel refuses when the commit room or the physical memory is short).
constexpr uint32_t kBlockSizes[] = {0x20000, 0x10000};

// What the start-up says on screen about the block that was reserved (the sizes of kBlockSizes); "" = nothing to say.
// The smaller block holds only a 32 KiB ring, half the buffer time of a 4-player session, and a player who loses frames
// should be told why. The text carries the reason itself (there may be no log to read: it is written only with debug=1) and,
// while the log is off, says how to get the details. (There used to be a second text, about a low memory margin, for a game that
// may be promised more memory than is free: it fired for every game that is given its whole budget and never uses it, Tri
// Force Heroes among them. How much memory the game has left is told by azl/screen_text.hpp, from what it really uses.)
inline std::string StartupIssue(uint32_t block_size, bool log_on) {
    const bool reduced = block_size != 0 && block_size < kSocBlockSize;
    if (!reduced) {
        return "";
    }
    return std::string("Socket buffer reduced to 64 KB: not enough free memory for 128 KB") + DebugHint(log_on);
}

// The reasons the soc service gives for refusing Initialize that need an explanation in the log.
inline const char* InitFailureName(uint32_t rc) {
    switch (rc) {
    case 0xC8A0700B: return "this process is already registered with soc:U: the game initialised the sockets itself first";
    case 0xC8A07004: return "this soc:U session is already initialised";
    default: return nullptr;
    }
}

enum class Window : uint8_t { HeapAreaTop, LimitTop, ReserveTop, HeapAreaMargin };

inline const char* WindowName(Window w) {
    switch (w) {
    case Window::HeapAreaTop: return "top of the heap area, out of the game's heap reach";
    case Window::LimitTop: return "top of the room the commit limit leaves the heap";
    case Window::ReserveTop: return "reserved range under the main stack";
    default: return "top of the heap area, heap margin only";
    }
}

struct Inputs {
    uint32_t commit_limit; // RESLIMIT_COMMIT limit in bytes, 0 = unknown
    uint32_t commit_used;  // RESLIMIT_COMMIT current value in bytes, before our block
    uint32_t free_end;     // end of the FREE span that contains 0x0E000000 (= start of the stack), 0 = unknown
    uint32_t region_bytes; // size of the application memory region (64 MiB Old 3DS, 124 MiB New 3DS), 0 = unknown
};

struct Candidate {
    uint32_t base;
    Window window;
};

struct PlanInfo {
    bool known;           // commit numbers usable
    bool provable;        // the game's heap cannot reach the top of the heap area
    uint32_t room;        // limit - used: what the game can still commit before our block
    uint32_t heap_reach;  // highest address the game's heap can reach once our block is charged
    uint32_t proof_floor; // lowest base that is still provably out of reach (heap_reach + kSafety)
};

inline uint32_t AlignUp(uint64_t v, uint32_t unit = kPage) {
    return static_cast<uint32_t>((v + (unit - 1)) & ~static_cast<uint64_t>(unit - 1));
}

// Pure arithmetic (all 64-bit intermediates, no unsigned wrap): returns the ordered candidate bases.
inline int Build(const Inputs& in, uint32_t size, Candidate* out, int max, PlanInfo* info) {
    PlanInfo pi{};
    int n = 0;
    auto add = [&](uint32_t base, Window w) {
        if (base < kRegionBase) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (out[i].base == base) {
                return;
            }
        }
        if (n < max) {
            out[n].base = base;
            out[n].window = w;
            ++n;
        }
    };
    if (size == 0 || (size & (kPage - 1)) != 0 || size > kHeapCap) {
        if (info) {
            *info = pi;
        }
        return 0;
    }
    pi.known = in.commit_limit != 0 && in.commit_used <= in.commit_limit;
    if (pi.known) {
        pi.room = in.commit_limit - in.commit_used;
        const uint64_t after = pi.room > size ? pi.room - size : 0; // the game's share once our block is charged
        const uint64_t heap = AlignUp(after) < kHeapCap ? AlignUp(after) : kHeapCap;
        pi.heap_reach = kRegionBase + static_cast<uint32_t>(heap);
        pi.proof_floor = AlignUp(static_cast<uint64_t>(pi.heap_reach) + kSafety);
        pi.provable = static_cast<uint64_t>(pi.proof_floor) + size <= kHeapAreaEnd;
    }

    // `end` = where a window stops; slots go downwards in 1 MiB steps while the block stays out of the
    // heap's reach.
    auto provable_window = [&](uint64_t end, Window w) {
        for (int k = 0; k < kProvableSlots; ++k) {
            const uint64_t back = static_cast<uint64_t>(size) + static_cast<uint64_t>(k) * kStep;
            if (back > end) {
                break;
            }
            const uint32_t base = static_cast<uint32_t>(end - back);
            if (base < pi.proof_floor) {
                break;
            }
            add(base, w);
        }
    };
    uint64_t limit_end = 0; // LimitTop: where the heap of a game with this limit can at most end
    if (pi.known) {
        const uint64_t reach = static_cast<uint64_t>(kRegionBase) + AlignUp(in.commit_limit, kStep);
        limit_end = reach < kHeapAreaEnd ? reach : kHeapAreaEnd;
    }
    const bool old_layout = in.region_bytes != 0 && in.region_bytes < kNewConsoleRegion;

    // 1./2. top of the heap area and top of the commit room, while provably out of the heap's reach;
    // an Old 3DS tries the second first
    if (pi.provable) {
        if (old_layout) {
            provable_window(limit_end, Window::LimitTop);
        }
        provable_window(kHeapAreaEnd, Window::HeapAreaTop);
        provable_window(limit_end, Window::LimitTop);
    }
    // 3. reserved range, from just under the stack downwards (never below 0x0E000000)
    uint32_t top = in.free_end != 0 ? (in.free_end < kRegionEnd ? in.free_end : kRegionEnd) : kDefaultTopEnd;
    top &= ~(kPage - 1);
    for (int k = 0; k < kReserveSlots; ++k) {
        const uint64_t back = static_cast<uint64_t>(size) + static_cast<uint64_t>(k) * kStep;
        if (back > top) {
            break;
        }
        const uint32_t base = static_cast<uint32_t>(top - back);
        if (base < kHeapAreaEnd) {
            break;
        }
        add(base, Window::ReserveTop);
    }
    // 4. margin-only, the last resort (when provable these bases are already in the list)
    for (int k = 0; k < kMarginSlots; ++k) {
        const uint64_t back = static_cast<uint64_t>(size) + static_cast<uint64_t>(k) * kStep;
        if (back > kHeapAreaEnd) {
            break;
        }
        add(static_cast<uint32_t>(kHeapAreaEnd - back), Window::HeapAreaMargin);
    }
    if (info) {
        *info = pi;
    }
    return n;
}

} // namespace socplan
} // namespace ctr
} // namespace azl
