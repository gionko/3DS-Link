// The gate's page of the svc hook (Includes/ctr/gate_page.hpp).
//
// Where: azl/gate_plan.hpp's ChooseAddress, over svcQueryMemory: the highest free place that ends at or below 0x01E80000 (CTRPF's and
// Luma's page, 0x2000 bytes, is never touched) and from which every call site is within the 32 MB of a `b`.
//
// How (azl/gate_plan.hpp, ChooseGateWay, the function the host tests run):
//   * Azahar (System::IsCitra()), and a New 3DS with a Luma3DS older than 13.0: an alias of memory of the plugin's, with svc 0xA0
//     (MapProcessMemoryEx) as CTRPF maps its own page. Not the way of choice on a console: the alias (state 0x5806, not PRIVATE) is outside
//     the kernel's books, and nothing in the plugin unmaps it: it OUTLIVES the game. Its pages stay taken in the APPLICATION region until a
//     reboot, and the next start fails to map the plugin's memory ("Cannot map plugin memory", 0xD86007F3) or an application that asks for
//     all its books give, to the byte, stops at its start (console 63, New 2DS XL, Luma3DS 13.4.0, 2026-11-06; 3DS Link, New 3DS XL, 1.1.2:
//     libctru's linear heap, svcBreak at its start). Only where nothing else can be had (Azahar; a New 3DS below 13.0, decided so).
//   * an Old or New 3DS from Luma3DS 13.0: Luma3DS's svc 0xA3 (svcControlMemoryUnsafe), a block of its own in the SYSTEM region, RWX, which the swap
//     does not take away. It is charged to nobody, but it is a private block all the same: the books of the process are raised by the page at
//     once (the limit, then current through the kernel's own reservation: the room the game sees while it sizes its heap does not change; a
//     Raise that the kernel refuses frees the page again) and the limit comes back down once the game has its heap
//     (SettleGateBooks; not on a New 3DS in memtype 6, where pm puts the limit back at the end of every application). See
//     ctr/kernel_books.cpp for why a block outside the books would cost the next game its budget. Where pm does not put the limit back (an
//     Old 3DS, a New 3DS in memtype 7) a game that ends before SettleGateBooks is settled at its EXIT event (SettleGateBooksAtExit: on an Old
//     3DS a game closed from the HOME menu gets it too, Luma3DS maps the plugin's block back first). SWAP_ENTER settles it as well once the
//     game has its heap (SettleGateBooksAtSwap): a net for an end with no EXIT event (a process killed, a block that does not come back).
//     One that ends with none of them (before it had its heap, a crash) leaves it raised until a reboot: the next launch takes it back
//     first (SettleDrift). The kernel frees the block with the process, whatever way the game ends: nothing to take out at the exit.
//   * an Old or New 3DS where the above fails: NO page and no hooks for this launch (one line in the log, the notice on the screen). Never
//     the alias there: on a New 3DS it outlives the game (above: every launch after it fails until a reboot, where a launch without hooks
//     costs that launch alone); on an Old 3DS it is memory of the plugin's block, which the swap takes away under a thread asleep in the
//     gate: the very defect the gate is for.
//   * an Old 3DS with an older Luma3DS: no page and no hooks either. Never the BASE region, never the two-word hook.
#include "ctr/gate_page.hpp"

#include <3ds.h>
#include <malloc.h>

#include <CTRPluginFramework.hpp>

#include "azl/books_math.hpp"
#include "azl/gate_plan.hpp"
#include "azl/log.hpp"
#include "csvc.h"
#include "ctr/kernel_books.hpp"

namespace azl {
namespace ctr {

namespace {

bool QuerySpan(u32 address, GateSpan* out) {
    MemInfo info{};
    PageInfo page{};
    if (R_FAILED(svcQueryMemory(&info, &page, address))) {
        return false;
    }
    out->base = info.base_addr;
    out->size = info.size;
    out->state = info.state;
    return true;
}

/// Whether [address, address + bytes) is mapped as one block that starts at `address`. `perm` and `state`: what the kernel says of it
/// (written in the log, never a reason to refuse: Luma3DS makes the whole process RWX in the MMU while the kernel's logical permissions
/// may still say RW, see InstallSvcHooks).
bool PageMapped(u32 address, u32 bytes, u32* perm, u32* state) {
    MemInfo info{};
    PageInfo page{};
    if (R_FAILED(svcQueryMemory(&info, &page, address))) {
        return false;
    }
    *perm = info.perm;
    *state = info.state;
    return info.state != MEMSTATE_FREE && info.base_addr == address && static_cast<u64>(info.base_addr) + info.size >= static_cast<u64>(address) + bytes;
}

// svc 0xA0 (MapProcessMemoryEx) in its first form, five arguments, which every Luma3DS takes (libctrpf's svcMapProcessMemoryEx is the later
// form, with a magic number in r0: a Luma3DS from before it takes that for a handle; Sources/ctr/input_mirror.cpp, the same call).
__attribute__((noinline)) Result MapProcessMemoryEx5(Handle dst, u32 dst_va, Handle src, u32 src_va, u32 size) {
    register u32 r0 asm("r0") = dst;
    register u32 r1 asm("r1") = dst_va;
    register u32 r2 asm("r2") = src;
    register u32 r3 asm("r3") = src_va;
    register u32 r4 asm("r4") = size;
    asm volatile("svc 0xA0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : "r"(r4) : "r12", "memory");
    return static_cast<Result>(r0);
}

GatePage Alias(u32 address, u32 bytes, const char* reason, const char** why) {
    GatePage page;
    void* source = memalign(kGatePageBytes, bytes); // (for the whole launch: the page is the gate's until the process ends)
    if (source == nullptr) {
        *why = "no memory for the gate's page in the plugin";
        Log("gate: no hooks: no %lu bytes of the plugin's memory for the alias at 0x%08lx", static_cast<unsigned long>(bytes),
            static_cast<unsigned long>(address));
        return page;
    }
    const Result rc = MapProcessMemoryEx5(CUR_PROCESS_HANDLE, address, CUR_PROCESS_HANDLE, reinterpret_cast<u32>(source), bytes);
    u32 perm = 0;
    u32 state = 0;
    if (R_FAILED(rc) || !PageMapped(address, bytes, &perm, &state)) {
        const u32 source_address = reinterpret_cast<u32>(source);
        if (R_SUCCEEDED(rc)) {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, address, bytes); // (mapped, but not as asked: not ours to keep)
        }
        free(source);
        *why = "the alias of the gate's page failed";
        Log("gate: no hooks: the alias of 0x%08lx at 0x%08lx failed (0x%08lx, state 0x%lx)", static_cast<unsigned long>(source_address),
            static_cast<unsigned long>(address), static_cast<unsigned long>(rc), static_cast<unsigned long>(state));
        return page;
    }
    page.address = address;
    page.bytes = bytes;
    page.kind = GatePageKind::Alias;
    Log("gate: page at 0x%08lx, %lu page(s), alias of the plugin's memory at 0x%08lx (%s; perm 0x%lx, state 0x%lx)",
        static_cast<unsigned long>(address), static_cast<unsigned long>(bytes / kGatePageBytes),
        static_cast<unsigned long>(reinterpret_cast<u32>(source)), reason, static_cast<unsigned long>(perm), static_cast<unsigned long>(state));
    return page;
}

// The bytes of the gate's page that Settle still owes (set once, in TakeGatePage, before any other thread of the plugin or of the game):
// LateInit (SettleGateBooks), SWAP_ENTER (SettleGateBooksAtSwap) or the EXIT event (SettleGateBooksAtExit), whichever comes first, takes
// them (books::TakeOwed); never two of them.
u32 g_settle_owed = 0;
// What the keep-alive thread did with the books at an event, for a line later (it writes none): (event << 28) | (OwedSettle << 24) | KB,
// the last event that had something to say; 0 nothing. LogGateBooksAtEvents takes it.
constexpr u32 kSettleAtExit = 1;
constexpr u32 kSettleAtSwap = 2;
u32 g_event_settle = 0;

void NoteEventSettle(u32 event, OwedSettle result, u32 bytes) {
    if (result != OwedSettle::Nothing) {
        __atomic_store_n(&g_event_settle, (event << 28) | (static_cast<u32>(result) << 24) | ((bytes / 1024) & 0xFFFFFFu), __ATOMIC_SEQ_CST);
    }
}

// Every console on the SYSTEM way that owes Settle (an Old 3DS, a New 3DS in memtype 7): the drift that launches which died between
// RaiseBooks and their Settle with no EXIT event (azl/books_math.hpp, Drift: pm writes the limit there only at boot, and the category's
// object outlives every game) is taken back before this launch raises it. Measured against the limit of boot (books::DriftReference): on a
// New 3DS the region's size, which Luma3DS gives, since Rosalina has taken the plugin's block off APPMEMALLOC; on an Old 3DS APPMEMALLOC.
// Not on a New 3DS in memtype 6 (books::SettleNeeded): pm puts the limit back at the end of every application there, and writes it at the
// launch of a title in Old 3DS mode (64/80/96 MB, nothing to do with a drift). A limit that is not the reference plus a few pages is not
// ours: it is left alone, in the log.
void SettleDrift(bool is_new) {
    s32 limit = 0;
    s32 commit = 0;
    if (!ReadBooks(&limit, &commit)) {
        return;
    }
    const u32 appmemalloc = static_cast<u32>(osGetMemRegionSize(MEMREGION_APPLICATION)); // (APPMEMALLOC, 0x1FF80040)
    s64 region = 0;
    if (R_FAILED(svcGetSystemInfo(&region, 0x10000, 0x80)) || region <= 0 || region >= 0x40000000) {
        region = 0; // (no answer from Luma3DS: APPMEMALLOC)
    }
    const u32 reference = books::DriftReference(is_new, static_cast<u32>(region), appmemalloc);
    const s32 drift = books::Drift(limit, reference);
    if (drift != 0) {
        const bool settled = SettleBooks(static_cast<u32>(drift));
        Log("gate: books drift %ld KB %s (limit %ld KB, reference %lu KB, APPMEMALLOC %lu KB): launches that ended before their books were settled",
            static_cast<long>(drift / 1024), settled ? "settled" : "NOT settled", static_cast<long>(limit / 1024),
            static_cast<unsigned long>(reference / 1024), static_cast<unsigned long>(appmemalloc / 1024));
    } else if (static_cast<s64>(limit) != static_cast<s64>(reference)) {
        Log("gate: books: limit %ld KB, reference %lu KB (APPMEMALLOC %lu KB): not a drift of the gate's page, left as it is",
            static_cast<long>(limit / 1024), static_cast<unsigned long>(reference / 1024), static_cast<unsigned long>(appmemalloc / 1024));
    }
}

} // namespace

GatePage TakeGatePage(u32 min_site, u32 max_site, u32 bytes, const char** why) {
    *why = nullptr;
    GatePage page;
    GatePlaceInfo place;
    const u32 address = ChooseAddress(QuerySpan, min_site, max_site, bytes, &place);
    if (address == 0) {
        *why = GatePlaceName(place.result);
        Log("gate: no hooks: %s (%lu bytes for the sites 0x%08lx..0x%08lx, %lu queries)", GatePlaceName(place.result),
            static_cast<unsigned long>(bytes), static_cast<unsigned long>(min_site), static_cast<unsigned long>(max_site),
            static_cast<unsigned long>(place.queries));
        return page;
    }
    // (Luma3DS's version: in the line of the page, and the test of svc 0xA3 on an Old 3DS; 0 without Luma3DS)
    s64 luma = 0;
    if (R_FAILED(svcGetSystemInfo(&luma, 0x10000, 0))) {
        luma = 0;
    }
    const u32 luma_version = static_cast<u32>(luma);
    const char* reason = nullptr;
    const bool is_new = CTRPluginFramework::System::IsNew3DS();
    const GateWay way = ChooseGateWay(CTRPluginFramework::System::IsCitra(), is_new, luma_version, &reason);
    if (way == GateWay::Alias) {
        return Alias(address, bytes, reason, why);
    }
    if (way == GateWay::None) {
        *why = reason;
        Log("gate: no hooks: %s (Luma3DS %lu.%lu.%lu)", reason, static_cast<unsigned long>(luma_version >> 24),
            static_cast<unsigned long>((luma_version >> 16) & 0xFF), static_cast<unsigned long>((luma_version >> 8) & 0xFF));
        return page;
    }
    // A console with a Luma3DS that has svc 0xA3: the SYSTEM page, on the kernel's books; where it fails, nothing (Old and New alike).
    const char* fail = nullptr;
    Result rc = 0;
    if (!FindBooks()) {
        fail = "the kernel's books of the process were not found";
    } else {
        const bool settle_needed = books::SettleNeeded(is_new, OS_KernelConfig->app_memtype);
        if (settle_needed) {
            SettleDrift(is_new); // (before this launch raises the books: what an earlier one left there)
        }
        ReadBooks(&page.limit_before, &page.commit_before);
        u32 out = 0;
        rc = svcControlMemoryUnsafe(&out, address, bytes, static_cast<MemOp>(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                                    static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE));
        u32 perm = 0;
        u32 state = 0;
        if (R_FAILED(rc)) {
            fail = "svc 0xA3 refused the page";
        } else if (out != address || !PageMapped(address, bytes, &perm, &state)) {
            u32 ignored = 0;
            svcControlMemoryUnsafe(&ignored, out, bytes, MEMOP_FREE, static_cast<MemPerm>(0));
            fail = "the page is not where it was asked";
        } else if (!RaiseBooks(bytes, &rc)) {
            // (the kernel refused the reservation, rc its result: a page left off the books would cost the next game its budget at this one's
            //  end. It goes again, with the svc that took it, which leaves the books alone; then no hooks, as for any failure here)
            u32 ignored = 0;
            const Result freed = svcControlMemoryUnsafe(&ignored, address, bytes, MEMOP_FREE, static_cast<MemPerm>(0));
            if (R_FAILED(freed)) {
                rc = freed;
                fail = "the kernel's books could not be raised, and the page could not be freed";
            } else {
                fail = "the kernel's books could not be raised (the kernel refused the reservation)";
            }
        } else {
            // On the books (RaiseBooks, just above: no thread of the game runs yet). The room the game sees does not change.
            page.books_raised = true;
            __atomic_store_n(&g_settle_owed, settle_needed ? bytes : 0u, __ATOMIC_SEQ_CST);
            page.address = address;
            page.bytes = bytes;
            page.kind = GatePageKind::System;
            Log("gate: page at 0x%08lx, %lu page(s), SYSTEM (commit %ld KB, limit %ld KB; perm 0x%lx, state 0x%lx; Luma3DS %lu.%lu.%lu)",
                static_cast<unsigned long>(address), static_cast<unsigned long>(bytes / kGatePageBytes),
                static_cast<long>(page.commit_before / 1024), static_cast<long>(page.limit_before / 1024), static_cast<unsigned long>(perm),
                static_cast<unsigned long>(state), static_cast<unsigned long>(luma_version >> 24),
                static_cast<unsigned long>((luma_version >> 16) & 0xFF), static_cast<unsigned long>((luma_version >> 8) & 0xFF));
            return page;
        }
    }
    // No SYSTEM page, and no RaiseBooks (the one branch that made it returns; a page whose Raise failed is freed again, and one that could not
    // be freed either stays off the books, said by `fail`). An Old or a New 3DS alike: no alias (on a New 3DS nothing unmaps it and it
    // outlives the game, the next starts fail until a reboot; on an Old 3DS the swap takes the plugin's block away), no other region, no
    // other hook. This launch plays without hooks.
    *why = fail;
    Log("gate: no hooks: SYSTEM page failed: %s (0x%08lx, %lu bytes at 0x%08lx): no other page is taken (never the alias)", fail,
        static_cast<unsigned long>(rc), static_cast<unsigned long>(bytes), static_cast<unsigned long>(address));
    return GatePage{};
}

void SettleGateBooks(const GatePage& page) {
    if (page.kind != GatePageKind::System || !page.books_raised) {
        return;
    }
    if (!books::SettleNeeded(CTRPluginFramework::System::IsNew3DS(), OS_KernelConfig->app_memtype)) {
        // (a New 3DS in memtype 6: pm puts the limit of the applications back to its default at the end of every application, whatever
        //  the books say then. Raise alone leaves the game, for the whole game, the room it would have had without the page, and the end
        //  balances by itself: current holds the page, which KProcess::Finalize gives back with the private blocks.)
        Log("gate: books left raised by the page: pm resets the limit at the end (New 3DS, memtype 6)");
        return;
    }
    if (!WaitForGameHeap()) {
        // (the game sizes its heap later, or elsewhere: taking the bytes back now could leave it short. The limit stays raised by the page
        //  until an event settles it (SettleGateBooksAtSwap once the heap is there, SettleGateBooksAtExit); a game that ends with neither
        //  leaves it raised, and on an Old 3DS, or a New 3DS in memtype 7, nothing puts the limit of the applications back until a reboot:
        //  the next launch takes that drift back, before it raises the books again: SettleDrift in TakeGatePage.)
        Log("gate: books not settled: no heap of the game seen");
        return;
    }
    const u32 owed = books::TakeOwed(&g_settle_owed);
    if (owed == 0) {
        Log("gate: books settled at an event already (SWAP_ENTER or the EXIT)");
        return;
    }
    s32 limit = 0;
    s32 commit = 0;
    ReadBooks(&limit, &commit);
    const bool settled = SettleBooks(owed);
    s32 limit_after = limit;
    s32 commit_after = commit;
    ReadBooks(&limit_after, &commit_after);
    Log("gate: books %s: commit %ld->%ld KB, limit %ld->%ld KB (%lu KB of the gate's page)", settled ? "settled" : "NOT settled",
        static_cast<long>(page.commit_before / 1024), static_cast<long>(commit_after / 1024), static_cast<long>(limit / 1024),
        static_cast<long>(limit_after / 1024), static_cast<unsigned long>(owed / 1024));
}

// The framework's keep-alive thread, at the EXIT event (Luma3DS: the game is in svcExitProcess, its thread waits in the kernel until
// Rosalina has the framework's answer; the process is alive): the Settle the game did not live to see, once. limits[] alone, which no game
// thread writes and KResourceLimit::Reserve only reads; KResourceLimit::Release (at KProcess::Finalize) looks at current alone, which still
// holds the page: the end gives back the game's private blocks and the page, and the limit is the one of boot. Whatever the heap: nothing
// sizes one any more. Never waits for the lock of the books (TrySettleOwedBooks: busy, the bytes stay owed for the next launch's drift).
// No line here.
void SettleGateBooksAtExit() {
    u32 bytes = 0;
    const OwedSettle result = TrySettleOwedBooks(&g_settle_owed, true, &bytes);
    NoteEventSettle(kSettleAtExit, result, bytes);
}

// The framework's keep-alive thread, at SWAP_ENTER (an Old 3DS, HOME; Luma3DS waits for the framework's answer, then swaps the plugin's
// block out). A game closed from the HOME menu gets its EXIT event all the same (Luma3DS maps the plugin's block back first: k11_extension
// SendSyncRequest.c, plgloader.c), and SettleGateBooksAtExit makes the Settle there; this one is a net for an end with no EXIT event (a
// process killed, a block that does not come back). The Settle still owed is made here, once, as at the EXIT - but only once the game has
// its heap (GameHeapSeen, no wait): a game that comes back from the HOME menu then plays with the limit LateInit would have left it; before
// its heap a lower limit would take a page off the room it sizes it with, and the bytes stay owed (LateInit, a later SWAP_ENTER, the EXIT,
// or the next launch's drift). One load at every HOME once the Settle is made; one svcQueryMemory and one svc of the books otherwise. No
// line here.
void SettleGateBooksAtSwap() {
    if (__atomic_load_n(&g_settle_owed, __ATOMIC_SEQ_CST) == 0) {
        return;
    }
    u32 bytes = 0;
    const OwedSettle result = TrySettleOwedBooks(&g_settle_owed, GameHeapSeen(), &bytes);
    NoteEventSettle(kSettleAtSwap, result, bytes);
}

void LogGateBooksAtEvents() {
    const u32 v = __atomic_exchange_n(&g_event_settle, 0u, __ATOMIC_SEQ_CST);
    if (v == 0) {
        return;
    }
    const char* const event = (v >> 28) == kSettleAtExit ? "the EXIT event" : "SWAP_ENTER (HOME)";
    const OwedSettle result = static_cast<OwedSettle>((v >> 24) & 0xF);
    if (result == OwedSettle::Settled) {
        Log("gate: books settled at %s (%lu KB of the gate's page that LateInit had not settled)", event, static_cast<unsigned long>(v & 0xFFFFFFu));
    } else {
        Log("gate: books NOT settled at %s: %s (left owed: LateInit, a later event, or the next launch's drift)", event,
            result == OwedSettle::Busy ? "the lock of the books was held by another thread" : "no heap of the game yet");
    }
}

} // namespace ctr
} // namespace azl
