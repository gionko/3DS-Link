// The svc hook around the swap of the plugin's memory (Old 3DS): which call sites go back to the game's own code, when, and in which order
// (see ctr/hook.cpp for the console side, Tests/hook_swap_test.cpp for the tests).
//
// On an Old 3DS, Luma3DS swaps the plugin's block (the .3gx with the trampolines of the hook) out to a file and frees it when the game goes to the HOME menu,
// and brings it back when the game returns. The game's threads are never stopped by that. Two kinds of thread can be inside the block when it goes:
//   * THE thread that causes the swap: the kernel raises the event at the end of the game's own APT:ReceiveParameter (k11_extension/source/svc/SendSyncRequest.c),
//     on that thread, and puts it to sleep inside the kernel until the swap is over. If that svc was made by our code (the hook), the thread wakes up in
//     unmapped memory. This one is handed back: the call goes to its original call site, which gets the game's `svc` and the next instruction back, and the game
//     runs its own svc from its own code (HandBackSite).
//   * any other thread that happens to be in the hook (a call that blocks in a real service, a lock of the Download Play hub): the hooks are taken out of the
//     game when the framework tells the plugin that the swap is about to happen (SuspendSites), the threads already inside are waited for (ctr/hook.cpp counts them),
//     and the hooks are put back when the block is back (ResumeSites).
//   * THE thread that brings the block back: the same kernel code, at the end of the game's own APT:ReceiveParameter while the game is in the HOME menu, with the hooks out. It wakes up at
//     the word after its svc, and the framework, woken at the same moment, puts the hooks back: with the hook in, that word is the trampoline's address, run as an instruction in place of the game's
//     next one. So the sites where a ReceiveParameter was seen stay the game's when the hooks go back, and take the hook later (ResumeSites, RearmSites). A site that is only `svc; bx lr` is every call
//     of the game (some games have one such site and no other): it cannot wait out of the hook, a fake session would go to the kernel; there the risk stays.
// (The plugin's own threads have their own gate: azl/swap_gate.hpp.)
// Everything here is plain code over an injected memory (`Mem`), so that the host tests can run it against an array.
#pragma once

#include "azl/common.hpp"
#include "azl/gsp_wrapper.hpp"
#include "azl/svc_patch.hpp"

namespace azl {

/// One redirected call site of the game: the two words that were there and the trampoline its patch jumps to.
struct HookSite {
    u32 address;
    u32 original[2]; ///< `svc` and the instruction after it
    u32 tramp;       ///< where `ldr pc, [pc, #-4]` at `address` goes (the second word of the patch)
};

constexpr u32 kHooksActive = 0;
constexpr u32 kHooksSuspended = 1; ///< from SWAP_ENTER to SWAP_EXIT: the game runs its own svc everywhere
constexpr u32 kNoSite = 0xFFFFFFFFu;

/// The sites the game's own svc was given back for (a ReceiveParameter that may raise the swap). They go back into the hook when the swap came (ResumeSites),
/// or `kRearmDelayMs` after the last one when it did not.
constexpr u32 kMaxHandedBack = 8;
constexpr u32 kRearmDelayMs = 2000;
struct HandBackLedger {
    volatile u32 slot[kMaxHandedBack] = {}; ///< site index + 1, 0 = free (a slot is claimed with a compare-and-swap: two game threads can come at once)
    volatile u32 last_ms = 0;
    volatile u32 receive[kMaxHandedBack] = {}; ///< the sites of APT:ReceiveParameter (site index + 1, 0 = free), for the whole launch: see ResumeSites
};

struct SwapStats {
    u32 suspends = 0;       ///< SWAP_ENTER handled
    u32 restored = 0;       ///< sites given back to the game by the last suspend
    u32 not_ours = 0;       ///< sites the last suspend found neither ours nor original (somebody else patched them): left alone
    u32 in_flight = 0;      ///< threads inside the hook when the last suspend began
    u32 in_flight_left = 0; ///< ... and when it gave up waiting (0: all had left)
    u32 waited_us = 0;      ///< how long the last suspend waited for them
    u32 resumes = 0;        ///< SWAP_EXIT handled
    u32 hooked_again = 0;   ///< sites put back by the last resume
    u32 handbacks = 0;      ///< calls handed back to their call site (all swaps)
    u32 handback_refused = 0; ///< hand-backs that were asked for and could not be done
    u32 rearmed = 0;        ///< handed-back sites that went back into the hook with no swap
    u32 asked = 0;          ///< APT:ReceiveParameter(application) calls the entry asked about (on every console: this one is how the log tells the New 3DS from the Old)
    u32 no_home = 0;        ///< ... of which, on an Old 3DS, with no HOME or power button seen in the last seconds: left with the hook
    u32 kept_out = 0;       ///< sites of APT:ReceiveParameter the last resume left to the game for a while
};

template <class Mem>
inline bool SiteIsOurs(Mem& m, const HookSite& s) {
    return m.Load(s.address) == kSiteJumpWord && m.Load(s.address + 4) == s.tramp;
}

template <class Mem>
inline bool SiteIsOriginal(Mem& m, const HookSite& s) {
    return m.Load(s.address) == s.original[0] && m.Load(s.address + 4) == s.original[1];
}

/// The site whose trampoline was entered: `tramp_return` is the address the trampoline's `bl` returns to (the trampoline + 8).
inline u32 FindSiteIndex(const HookSite* sites, u32 count, u32 tramp_return) {
    for (u32 i = 0; i < count; ++i) {
        if (sites[i].tramp != 0 && sites[i].tramp + 8 == tramp_return) {
            return i;
        }
    }
    return kNoSite;
}

inline bool LedgerHas(const HandBackLedger& ledger, u32 index) {
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        if (__atomic_load_n(&ledger.slot[i], __ATOMIC_ACQUIRE) == index + 1) {
            return true;
        }
    }
    return false;
}

inline bool LedgerAny(const HandBackLedger& ledger) {
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        if (__atomic_load_n(&ledger.slot[i], __ATOMIC_ACQUIRE) != 0) {
            return true;
        }
    }
    return false;
}

/// False when the ledger is full.
inline bool LedgerAdd(HandBackLedger& ledger, u32 index) {
    if (LedgerHas(ledger, index)) {
        return true;
    }
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        u32 expected = 0;
        if (__atomic_compare_exchange_n(&ledger.slot[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

inline void LedgerClear(HandBackLedger& ledger) {
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        __atomic_store_n(&ledger.slot[i], 0, __ATOMIC_RELEASE);
    }
}

/// Remembers where the game makes its APT:ReceiveParameter: the svc of the thread that brings the block back is there (see the top of this file). Runs on the GAME's thread, like HandBackSite.
inline void NoteReceiveSite(const HookSite* sites, u32 count, HandBackLedger& ledger, u32 tramp_return) {
    const u32 index = FindSiteIndex(sites, count, tramp_return);
    if (index == kNoSite || sites[index].original[1] == kBxLrWord) {
        return;
    }
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        const u32 entry = __atomic_load_n(&ledger.receive[i], __ATOMIC_ACQUIRE);
        if (entry == index + 1) {
            return;
        }
        u32 expected = 0;
        if (entry == 0 && __atomic_compare_exchange_n(&ledger.receive[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
        if (expected == index + 1) {
            return;
        }
    }
}

inline bool IsReceiveSite(const HandBackLedger& ledger, u32 index) {
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        if (__atomic_load_n(&ledger.receive[i], __ATOMIC_ACQUIRE) == index + 1) {
            return true;
        }
    }
    return false;
}

/// The game's thread that is about to make the APT:ReceiveParameter that may raise the swap goes back to its call site with the game's own `svc` and next
/// instruction in it. Returns the address of the call site, or 0 when the call stays with the hook (hooks out already, the trampoline is not one of ours, a site
/// somebody else changed, no room in the ledger).
/// Runs on the GAME's thread: no lock, no wait, no call but the memory's. The site's two words are written in the order that leaves
/// (svc, literal) for a thread that passes in between: the svc is the game's own, the literal runs as a harmless conditional instruction at worst.
template <class Mem>
inline u32 HandBackSite(Mem& m, HookSite* sites, u32 count, HandBackLedger& ledger, const volatile u32* state, u32 tramp_return,
                        u32 now_ms) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != kHooksActive) {
        return 0;
    }
    const u32 index = FindSiteIndex(sites, count, tramp_return);
    if (index == kNoSite) {
        return 0;
    }
    const HookSite& site = sites[index];
    const bool ours = SiteIsOurs(m, site);
    if (!ours && !SiteIsOriginal(m, site)) { // (a second thread through the same trampoline finds the site already given back: it goes there too)
        return 0;
    }
    if (!LedgerAdd(ledger, index)) { // (in the ledger BEFORE the site changes: a re-hook can never miss it)
        return 0;
    }
    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);
    if (ours) {
        m.StoreOriginal(site.address, site.original);
        m.Sync();
    }
    return site.address;
}

/// SWAP_ENTER: every site that is ours goes back to the game. Returns how many. (The caller waits for the threads that are inside the hook afterwards.)
template <class Mem>
inline u32 SuspendSites(Mem& m, HookSite* sites, u32 count, HandBackLedger& ledger, volatile u32* state, SwapStats* stats) {
    __atomic_store_n(state, kHooksSuspended, __ATOMIC_RELEASE);
    u32 restored = 0;
    u32 not_ours = 0;
    for (u32 i = 0; i < count; ++i) {
        if (SiteIsOurs(m, sites[i])) {
            m.StoreOriginal(sites[i].address, sites[i].original);
            ++restored;
        } else if (!SiteIsOriginal(m, sites[i])) {
            ++not_ours;
        }
    }
    m.Sync();
    LedgerClear(ledger);
    if (stats) {
        ++stats->suspends;
        stats->restored = restored;
        stats->not_ours = not_ours;
    }
    return restored;
}

/// SWAP_EXIT: the sites that hold the game's own words go back into the hook, and the hook is active again. Returns how many. The sites of APT:ReceiveParameter wait: the thread that
/// brought the block back may be about to wake up at one of them. They go into the ledger, and RearmSites hooks them `kRearmDelayMs` from now.
template <class Mem>
inline u32 ResumeSites(Mem& m, HookSite* sites, u32 count, HandBackLedger& ledger, volatile u32* state, u32 now_ms, SwapStats* stats) {
    LedgerClear(ledger);
    u32 again = 0;
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
        if (SiteIsOriginal(m, sites[i])) {
            if (IsReceiveSite(ledger, i) && LedgerAdd(ledger, i)) {
                ++kept;
                continue;
            }
            const u32 words[2] = {kSiteJumpWord, sites[i].tramp};
            m.StoreHook(sites[i].address, words);
            ++again;
        }
    }
    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);
    m.Sync();
    if (stats) {
        ++stats->resumes;
        stats->hooked_again = again;
        stats->kept_out = kept;
    }
    __atomic_store_n(state, kHooksActive, __ATOMIC_RELEASE); // (last: a hand-back before this would find sites half done)
    return again;
}

/// A hand-back that was not followed by a swap: after `kRearmDelayMs` the handed-back sites go back into the hook. Called with the patch lock
/// held, from a plugin thread (not the game's). Returns how many.
template <class Mem>
inline u32 RearmSites(Mem& m, HookSite* sites, u32 count, HandBackLedger& ledger, const volatile u32* state, u32 now_ms,
                      SwapStats* stats) {
    if (!LedgerAny(ledger) || __atomic_load_n(state, __ATOMIC_ACQUIRE) != kHooksActive) {
        return 0;
    }
    if (static_cast<u32>(now_ms - __atomic_load_n(&ledger.last_ms, __ATOMIC_ACQUIRE)) < kRearmDelayMs) {
        return 0;
    }
    u32 again = 0;
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        const u32 entry = __atomic_load_n(&ledger.slot[i], __ATOMIC_ACQUIRE);
        if (entry != 0 && entry - 1 < count && SiteIsOriginal(m, sites[entry - 1])) {
            const u32 words[2] = {kSiteJumpWord, sites[entry - 1].tramp};
            m.StoreHook(sites[entry - 1].address, words);
            ++again;
        }
    }
    m.Sync();
    LedgerClear(ledger);
    if (stats) {
        stats->rearmed += again;
    }
    return again;
}

} // namespace azl
