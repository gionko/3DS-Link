// The kernel's books of the game's process (its KResourceLimit), plain so that the host tests run it (Tests/kernel_books_test.cpp): the
// arithmetic of the word the plugin writes in kernel mode (limit[kCommit]; current[kCommit] only in the fallback, Charge), and the steps that
// move current[kCommit] through the kernel's own calls (ReserveThroughKernel, ReleaseThroughKernel and the ones after them: templates over the
// calls; the console's calls are in ctr/kernel_books.cpp).
//
// A private block taken from outside the game's budget (the SYSTEM region, Luma's svcControlMemoryUnsafe) is charged to nobody,
// but when the process ends the kernel adds up its private blocks and gives the sum back to the budget, and a sum above the books
// gives back NOTHING: the next game would find no budget until a reboot. So such a
// block is put on the books when it is taken. current[] is moved through the kernel's own calls first: the kernel changes it under the
// KResourceLimit's own KLightMutex, which only the kernel's own calls take and let go the kernel's way (ctr/kernel_books.cpp says why), and
// the plugin never takes that mutex. current += X is a block of X taken with svcControlMemory (the kernel reserves X) and freed with svc
// 0xA3 (the pages go, the reservation stays); current -= X is a block taken with svc 0xA3 and freed with svcControlMemory (the kernel
// releases X). Both need free pages: svcControlMemory's in the process's own region, svc 0xA3's in the SYSTEM region (then APPLICATION,
// then BASE, a page at a time). When the kernel answers that it has none (kNoPages, from the ALLOC), and only then, Charge and Uncharge
// write current[kCommit] themselves in kernel mode with the interrupts off, the load-add-store 1.0.13-1.1.1 made for every late block
// (ChargeOrWrite, UnchargeOrWrite; never with the kernel's mutex taken by hand): an Old 3DS whose late title holds the whole region,
// Monster Hunter XX, has no page for the reservation, and without it no network at all.
//   * Charge: current += X. For a block taken after the game has sized its heap (the socket block of a late title, ctr/soc.cpp): made
//     BEFORE the block is taken, and taken back (Uncharge: current -= X) when the block is not had. A process that dies in between then
//     keeps X too many on current, never too few. The game has spent its budget: the limit is lifted by X, and by what current already
//     stands above it (ChargeLift), around the reservation only.
//   * Raise: current += X and limit += X. For a block taken BEFORE the game sizes its heap (the gate's page, at start-up): limit - current
//     does not change. Monster Hunter XX does not size its heap from that: it reads APPMEMALLOC (0x1FF80040) and takes the commit off it
//     (analysis/mhxx-crash-2026-11-01/NOTA.md: heap = min(APPMEMALLOC, 80 MiB) - commit - 59 MiB), so every current += X before its first
//     instruction shortens its heap by X, Raise included. The gate's page passes only because the game leaves a margin (about 150 KB left
//     at its start against the 52 KB it still asks for); the 128 KB socket block did not (the crash 1.0.13 fixed), and no Raise of it would.
//   * Settle: limit -= X, once the game has its heap. The books are then what Charge would have left: the end balances by itself.
//     Not on a New 3DS in memtype 6 (SettleNeeded): pm puts the limit back there at the end of every application.
//     Settle is owed once (TakeOwed, shared by all of them): at LateInit; at the EXIT event (an Old 3DS too when the game is closed from
//     the HOME menu: Luma3DS maps the plugin's block back, then sends it); at SWAP_ENTER (an Old 3DS, HOME) once the game's heap is there
//     (TakeOwedOnceHeapSeen), a net for an end with no EXIT event (a process killed, a block that does not come back).
//   * Drift: on an Old 3DS (and a New 3DS in memtype 7), what launches that died between Raise and Settle with none of the three left on
//     the limit (it outlives the process there), taken back with Settle before the next Raise; measured against DriftReference.
// The arithmetic (Lift, Settle, Charge) is a leaf, always inlined: the console runs it in kernel mode (svcCustomBackdoor), where nothing
// else may run. The steps (ReserveThroughKernel and the ones after it) run in user mode and only call the kernel.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace books {

/// KResourceLimit (Luma's kernel.h): KAutoObject (vtable, refcount), s32 limit[10], s32 current[10].
constexpr u32 kLimitsOffset = 0x08;
constexpr u32 kCurrentOffset = 0x30;
constexpr int kSlots = 10;
/// libctru's ResourceLimitType: the commit (memory) is slot 1; the others the console checks the offsets with.
constexpr int kCommit = 1;
constexpr int kThread = 2;
constexpr int kEvent = 3;
constexpr int kMutex = 4;
constexpr int kChecked = 4;
constexpr int kCheckedNames[kChecked] = {kCommit, kThread, kEvent, kMutex};

/// limit += bytes (Raise for good, Charge around its reservation only).
__attribute__((always_inline)) inline void Lift(volatile s32* limits, s32 bytes) {
    limits[kCommit] += bytes;
}

/// limit -= bytes (Settle, and the way back of a Lift).
__attribute__((always_inline)) inline void Settle(volatile s32* limits, s32 bytes) {
    limits[kCommit] -= bytes;
}

/// current += bytes (bytes < 0: -=), the one write of current[] (ChargeOrWrite and UnchargeOrWrite, only when the kernel had no pages):
/// a load, an add and a store, the interrupts off, outside the kernel's mutex. The window it leaves is the other core's Reserve or Release
/// between the load and the store (one of the two lost); 1.0.13-1.1.1 charged the late socket block this way every time (there with the
/// interrupts on, after the block was taken).
__attribute__((always_inline)) inline void Charge(volatile s32* current, s32 bytes) {
    current[kCommit] += bytes;
}

/// The steps that move current[kCommit] with the kernel's own calls. `Kernel` (the console's in ctr/kernel_books.cpp, a made-up kernel in
/// the test) gives:
///   Read(&limit, &current)          limit[kCommit] and current[kCommit] as they are, read in kernel mode (KernelReadBooks)
///   Lift(bytes), Lower(bytes)       limit[kCommit] += / -= bytes, in kernel mode (the one word the plugin writes)
///   AllocCharged(bytes, &address)   svcControlMemory MEMOP_ALLOC_LINEAR, at an address the kernel chooses: the kernel RESERVES `bytes`
///                                   (KProcess::ReserveResource before KProcessPageTable::ControlMemory, under the KResourceLimit's mutex;
///                                   refused above the limit, and released again by the kernel itself when no pages are had)
///   FreeUncharged(address, bytes)   svc 0xA3 MEMOP_FREE: the pages go, the books stay (Luma3DS's ControlMemoryUnsafe calls no Release)
///   AllocUncharged(address, bytes, region)  svc 0xA3 MEMOP_ALLOC in `region`: pages, no Reserve
///   FreeCharged(address, bytes)     svcControlMemory MEMOP_FREE: the pages go and the kernel RELEASES `bytes`, whoever charged them
///   FreeSpan(bytes)                 a free place of `bytes` in the heap area, where svcControlMemory's FREE takes a block (0: none)
///   WriteCurrent(bytes)             current[kCommit] += bytes in kernel mode, the interrupts off (Charge): the fallback, nothing else
/// Results are the kernel's (negative: failed).
constexpr u32 kPageBytes = 0x1000;
/// FreeSpan found no place for the block ReleaseThroughKernel takes and frees (the kernel's own "invalid address" summary).
constexpr s32 kNoFreeSpan = static_cast<s32>(0xE0E01BF5);
/// The kernel's "no physical page left in the region" (RS_OUTOFRESOURCE): svcControlMemory's ALLOC when the reservation was made and the
/// pages were not had (Kernel11 ControlMemory 13882-13884, the reservation released again; the linear ALLOC's own "no contiguous pages",
/// KProcessPageTable::ControlMemory 20644-20649 with MakeResultCode 20737, and the same code when the linear address would not fit,
/// 20654-20662, the pages given back), svc 0xA3's ALLOC with no page in its region: Luma3DS's svc 0xA3 maps with doControlMemory, which is
/// KPageTable::Operate (k11_extension main.c:270-271 takes it from the call inside KProcessHwInfo::MapListOfKBlockInfo, which has the
/// arguments of Kernel11's KPageTable::OperateOnGroup 28062-28085, whose one call is Operate, 8 arguments both), and Operate with pa 0 takes the pages from the heap of
/// the region it is given, APPLICATION, SYSTEM or BASE alike (32006-32025), and answers 0xD86007F3 when KPageHeap::AllocateBackwards has
/// none (32104-32106). A FREE never answers it (it allocates nothing). The one answer that lets Charge and Uncharge write current[]
/// themselves.
constexpr s32 kNoPages = static_cast<s32>(0xD86007F3);
/// The kernel's refusal of the reservation above the limit (Kernel11 ControlMemory 13890, KResourceLimit::Reserve 25211). Charge lifts the
/// limit so that its own reservation is never above it (ChargeLift): this answer then means a game thread took the lifted room first, and it
/// stands (no fallback; ctr/kernel_books.cpp ChargeBooks writes its own line).
constexpr s32 kAboveLimit = static_cast<s32>(0xC860180A);
/// The region svc 0xA3 takes the pages of ReleaseThroughKernel's block from (MEMOP_REGION_SYSTEM, _APP, _BASE on the console). svcControlMemory's
/// FREE gives the pages back by their physical address, whatever the region (KMemoryManager::FreeContiguousLocked, MEMOP_NONE), and the
/// block's state (0xBB05) is the same in all three.
enum class Region : u32 { System, Application, Base };
/// The order ReleaseThroughKernel tries them in, a page at a time (the whole block is tried in the SYSTEM region only): SYSTEM, where the
/// plugin's own blocks are; APPLICATION, where a Charge made through the kernel has just found the pages; BASE.
constexpr int kReleaseRegionCount = 3;
constexpr Region kReleaseRegions[kReleaseRegionCount] = {Region::System, Region::Application, Region::Base};
/// How ChargeOrWrite and UnchargeOrWrite moved current[]: through the kernel's calls (or not at all: their result says), or written in
/// kernel mode because the kernel had no pages.
enum class Way : u32 { Kernel, Written };

/// current += bytes: a block of the process's own (the kernel reserves it), freed with svc 0xA3 (the reservation stays). 0, or the
/// kernel's refusal with the books as they were. When svc 0xA3 cannot free it, svcControlMemory does (the reservation goes with it); if
/// that fails too, the block stays, a private block on the books like any of the game's: the books balance either way.
template <class Kernel>
s32 ReserveThroughKernel(Kernel& kernel, u32 bytes) {
    u32 address = 0;
    const s32 rc = kernel.AllocCharged(bytes, &address);
    if (rc < 0) {
        return rc;
    }
    const s32 freed = kernel.FreeUncharged(address, bytes);
    if (freed < 0) {
        kernel.FreeCharged(address, bytes);
        return freed;
    }
    return 0;
}

/// current -= bytes: a block taken with svc 0xA3 (no Reserve), freed with svcControlMemory (the kernel releases it). Tried whole first in
/// the SYSTEM region, then a page at a time (svc 0xA3 needs the pages, and the SYSTEM region may be short of the whole block right after its
/// own allocation failed; a page is back in its region at once, so one free page is enough for all of them); a region with no page left
/// (kNoPages) passes the pages on to the next of kReleaseRegions. 0, or the kernel's result of the step that failed; `released`: what came
/// off current (only the pieces whose FREE was made).
template <class Kernel>
s32 ReleaseThroughKernel(Kernel& kernel, u32 bytes, u32* released) {
    *released = 0;
    u32 piece = bytes;
    int region = 0;
    while (*released < bytes) {
        if (piece > bytes - *released) {
            piece = bytes - *released;
        }
        const u32 address = kernel.FreeSpan(piece);
        const s32 rc = address == 0 ? kNoFreeSpan : kernel.AllocUncharged(address, piece, kReleaseRegions[region]);
        if (rc < 0) {
            if (piece > kPageBytes) {
                piece = kPageBytes;
                continue;
            }
            if (rc == kNoPages && region + 1 < kReleaseRegionCount) {
                ++region;
                continue;
            }
            return rc;
        }
        const s32 freed = kernel.FreeCharged(address, piece);
        if (freed < 0) {
            kernel.FreeUncharged(address, piece); // (the pages back; nothing came off current)
            return freed;
        }
        *released += piece;
    }
    return 0;
}

/// Raise: limit += bytes, then current += bytes through the kernel (room for its Reserve: the limit was just lifted, and the game has no
/// heap yet). Refused: the limit goes back and the books are as they were. The room the game sees does not change either way.
template <class Kernel>
s32 RaiseThroughKernel(Kernel& kernel, u32 bytes) {
    kernel.Lift(bytes);
    const s32 rc = ReserveThroughKernel(kernel, bytes);
    if (rc != 0) {
        kernel.Lower(bytes);
    }
    return rc;
}

/// What Charge lifts the limit by: `bytes`, and what current already stands above the limit. A game that sizes its heap to the byte on
/// limit - current (libctru's way) with the gate's page raised before it and settled after it ends with current above the limit by that page
/// (TestRaiseThenSettle); a lift of `bytes` alone would leave the reservation above the limit, refused (kAboveLimit) before the kernel ever
/// looks for pages, and the fallback, for want of pages, would never come. Lifted by this, the limit is max(limit, current) + bytes: the
/// reservation passes the limit and meets the pages (an Old 3DS whose game holds the whole region: kNoPages, the fallback). The room a game
/// thread could take in between is `bytes`, as for a game at its limit, and the sum stays in range (current + bytes).
inline u32 ChargeLift(s32 limit, s32 current, u32 bytes) {
    const s64 over = static_cast<s64>(current) - limit;
    return over > 0 ? bytes + static_cast<u32>(over) : bytes;
}

/// Charge: current += bytes for a game that has spent its budget. The books are read first, then the limit is lifted by ChargeLift for the
/// reservation only and comes back down by the same amount whatever it gave. A game thread that takes the lifted room first makes the
/// reservation fail: refused (kAboveLimit), the books as they were (with the game's own block on them, which its end gives back like any
/// other). A game thread that moves current between the read and the lift changes nothing of that: above the limit it can take nothing,
/// and at or under it the lift is `bytes`, as before.
template <class Kernel>
s32 ChargeThroughKernel(Kernel& kernel, u32 bytes) {
    s32 limit = 0;
    s32 current = 0;
    kernel.Read(&limit, &current);
    const u32 lift = ChargeLift(limit, current, bytes);
    kernel.Lift(lift);
    const s32 rc = ReserveThroughKernel(kernel, bytes);
    kernel.Lower(lift);
    return rc;
}

/// Charge of the late socket block (ctr/kernel_books.cpp ChargeBooks): through the kernel first (ChargeThroughKernel, the limit already back
/// down). Only when the kernel had no pages for the reservation (kNoPages: an Old 3DS whose game holds the whole region, Monster Hunter XX),
/// current += bytes written in kernel mode (WriteCurrent, books::Charge), as 1.0.13-1.1.1 did: 0 and `way` Written. Any other answer is the
/// kernel's and stands: made (0), or refused (kAboveLimit: with the lift of ChargeLift only a game thread that took the lifted room first;
/// a FREE that failed), nothing written.
template <class Kernel>
s32 ChargeOrWrite(Kernel& kernel, u32 bytes, Way* way) {
    *way = Way::Kernel;
    const s32 rc = ChargeThroughKernel(kernel, bytes);
    if (rc != kNoPages) {
        return rc;
    }
    kernel.WriteCurrent(static_cast<s32>(bytes));
    *way = Way::Written;
    return 0;
}

/// Uncharge (a Charge taken back when the block was not had, ctr/kernel_books.cpp UnchargeBooks): through the kernel first
/// (ReleaseThroughKernel: SYSTEM whole, then a page in SYSTEM, APPLICATION, BASE). Only when no region had a page (kNoPages), what did not
/// come off is written off current in kernel mode (WriteCurrent, books::Charge with the bytes negative): 0 and `way` Written; `released`
/// stays what the kernel released. Any other answer stands (no place in the heap area, a FREE that failed): what was not released stays.
template <class Kernel>
s32 UnchargeOrWrite(Kernel& kernel, u32 bytes, u32* released, Way* way) {
    *way = Way::Kernel;
    const s32 rc = ReleaseThroughKernel(kernel, bytes, released);
    if (rc != kNoPages) {
        return rc;
    }
    kernel.WriteCurrent(-static_cast<s32>(bytes - *released));
    *way = Way::Written;
    return 0;
}

/// The drift an Old 3DS keeps from launches that raised the limit and died before Settle with no event to settle at (closed from the HOME
/// menu before the game had its heap, a crash or an abort; a game that exits by itself is settled at its EXIT event, one closed from the
/// HOME menu at the SWAP_ENTER before it, TakeOwed). pm sets the APPLICATION category's commit limit at boot, to
/// APPMEMALLOC (Luma3DS sysmodules/pm: reslimit.c fixupReslimitValues), and writes it again only on a New 3DS with app_memtype 6: at the
/// launch of a title in Old 3DS mode (launch.c, setAppMemLimit, with APPMEMALLOC) and at the end of EVERY application (process_monitor.c,
/// resetAppMemLimit, back to the default). On an Old 3DS, and a New 3DS in memtype 7, it never does, and the category's KResourceLimit is
/// one object since boot: such a launch leaves it higher by its page until a reboot, and every other one adds its own. A game that sizes
/// its heap "to the byte" as limit - current (libctru's way; not Monster Hunter XX, which reads APPMEMALLOC) would then ask for room the
/// region does not have (on a New 3DS in memtype 6 the end of the application puts the limit back, and the drift found is 0). Drift() is
/// what to take back before this launch raises it again: limit - reference (DriftReference) when it is a whole number of pages and no more than kMaxDrift; 0 otherwise. A bigger
/// difference is not ours.
constexpr s32 kMaxDrift = 1024 * 1024;
constexpr s32 kDriftUnit = 0x1000;

inline s32 Drift(s32 limit, u32 reference) {
    const s64 diff = static_cast<s64>(limit) - static_cast<s64>(reference);
    if (diff <= 0 || diff > kMaxDrift || diff % kDriftUnit != 0) {
        return 0;
    }
    return static_cast<s32>(diff);
}

/// What the limit was set to at boot, which Drift() measures against. pm sets it to APPMEMALLOC as the kernel wrote it at boot, the size
/// of the APPLICATION region (pm reslimit.c fixupReslimitValues: OS_KernelConfig->memregion_sz[0]). On a New 3DS Rosalina takes the plugin's
/// whole block (2, 5 or 10 MiB) off APPMEMALLOC at the first launch with a plugin, whenever APPMEMALLOC is the region's size, and never puts
/// it back (Luma3DS rosalina plugin/memoryblock.c, MemoryBlock__IsReady, `isN3DS || MODE3`): there APPMEMALLOC is no longer the limit
/// of boot, the region's size is (`region_size`: Luma3DS's svcGetSystemInfo(0x10000, 0x80), the region's size in the kernel; 0 when it did
/// not answer, and APPMEMALLOC then). On an Old 3DS the plugin's block is in the SYSTEM region (the swap strategy, the only one this
/// plugin is loaded with) and APPMEMALLOC is untouched: it stays the reference there.
inline u32 DriftReference(bool is_new, u32 region_size, u32 appmemalloc) {
    return is_new && region_size != 0 ? region_size : appmemalloc;
}

/// The bytes of the gate's page that Settle still owes, taken by ONE of those that may settle (LateInit on the plugin's thread, SWAP_ENTER
/// and the EXIT event on the framework's keep-alive thread): the first gets them, the others 0. An exchange, never a load and a store.
inline u32 TakeOwed(u32* owed) {
    return __atomic_exchange_n(owed, 0u, __ATOMIC_SEQ_CST);
}

/// SWAP_ENTER (an Old 3DS, HOME): a net for an end with no EXIT event (a process killed, a block that does not come back; a game closed
/// from the HOME menu gets its EXIT all the same, once Luma3DS has mapped the plugin's block back), so the Settle still owed is made
/// there too - but only once the game has its heap (`heap_seen`: its region is mapped). Before it, a lower limit would
/// take a page off the room the game sizes its heap with: the bytes stay owed (LateInit, a later SWAP_ENTER, the EXIT, or the next
/// launch's drift).
inline u32 TakeOwedOnceHeapSeen(u32* owed, bool heap_seen) {
    return heap_seen ? TakeOwed(owed) : 0u;
}

/// Whether Settle is owed once the game has its heap. Not on a New 3DS in memtype 6 (OS_KernelConfig->app_memtype, 0x1FF80030): pm puts
/// the limit of the applications back to its default at the end of every application (Luma3DS pm process_monitor.c, resetAppMemLimit), so
/// a limit left raised by the page does not outlive the game, and Raise alone leaves the game the room it would have had without the page
/// for the whole game (a Settle would take a page of it back after LateInit). Everywhere else pm never writes it again: Settle is owed.
inline bool SettleNeeded(bool is_new, u32 app_memtype) {
    return !(is_new && app_memtype == 6);
}

/// The room the game sees: what svcGetResourceLimitLimitValues - CurrentValues tells it.
inline s64 Room(const volatile s32* limits, const volatile s32* current) {
    return static_cast<s64>(limits[kCommit]) - current[kCommit];
}

/// Whether the words read at the object's offsets (in kernel mode) are what the kernel's own calls said: the offsets are right
/// on this kernel. Anything else and nothing is ever written there.
inline bool SameAsTheCalls(const s32 read_limits[kChecked], s32 read_commit, const s64 call_limits[kChecked], s64 call_commit) {
    bool ok = call_commit == read_commit;
    for (int i = 0; i < kChecked; ++i) {
        ok = ok && call_limits[i] == read_limits[i];
    }
    return ok;
}

} // namespace books
} // namespace azl
