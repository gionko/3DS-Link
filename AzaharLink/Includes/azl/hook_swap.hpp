// The swap of the plugin's block on an Old 3DS, the plugin's side of the persistent gate of the svc hook (Sources/ctr/gate.s is the game's
// side, azl/gate_plan.hpp where it goes; ctr/hook.cpp calls these, Tests/hook_swap_test.cpp tests them).
//
// On an Old 3DS, Luma3DS swaps the plugin's block (0x07000000 and the plugin's heap) out to a file and frees it when the game goes to the
// HOME menu, the power menu or a library applet, and brings it back afterwards; the game's threads are never stopped for it. Every hooked
// call site of the game holds ONE word, `b stub_i`, written once before the game runs; the stubs and the gate are in a page of the game's
// process the swap does not take away. So no word of code changes, on any swap: a game thread asleep in a `svc` (the gate makes every real
// svc of the game itself) wakes up on the very instructions it went to sleep on. The swap only flips the gate's flag: GateClose at
// SWAP_ENTER (it waits for the game threads counted in the plugin's code), GateOpen once the block is mapped again.
// (The plugin's own threads have their own gate: azl/swap_gate.hpp.)
// Everything here is plain code over the gate's header, so that the host tests run it with real threads.
#pragma once

#include "azl/common.hpp"
#include "azl/gate_plan.hpp"

namespace azl {

// ---- The persistent gate, the plugin's side (Sources/ctr/gate.s is the game's side) ----
//
// A game thread in the gate counts itself in `in_flight` (ldrex/strex), makes a barrier, and only then reads `flag`; it calls into the
// plugin's block only if it read 1, and its count comes down (after another barrier) only once it has left the plugin's code. The
// closer writes flag = 0, makes a barrier, and only then reads `in_flight` (Dekker): either it sees the thread's count, or the
// thread sees flag = 0 and does not touch the plugin. So once GateClose has read in_flight == 0, no game thread is or will be in the
// plugin's code until GateOpen: the block may go. Neither writes anything but the flag: no word of code changes, on any swap.

struct HookGateStats {
    u32 closes = 0;          ///< GateClose calls (SWAP_ENTER)
    u32 opens = 0;           ///< GateOpen calls that opened a closed gate
    u32 in_flight_start = 0; ///< threads in the gate's count when the last close began
    u32 in_flight_left = 0;  ///< ... and when it gave up waiting (0: all had left)
    u32 waited_us = 0;       ///< how long the last close waited
    u32 last_site = 0;       ///< stub+8 of the last thread in, when the last close ended (for the line of a close that times out)
    u32 last_handle = 0;     ///< ... and its handle
    u32 post_skipped = 0;    ///< posts the gate did not call because the plugin was away (all swaps)
};

/// The loads, stores and barrier of the protocol. These are the console's (and the threads test's): the test of the order records
/// them instead. The barrier is a DMB on the console; the sequentially consistent accesses alone would make one on each side, the
/// explicit one says where Dekker needs it (ThreadSanitizer does not take a bare fence: there the accesses carry it).
struct GateAtomics {
    static void StoreFlag(volatile u32* w, u32 v) { __atomic_store_n(w, v, __ATOMIC_SEQ_CST); }
    static u32 LoadCount(const volatile u32* w) { return __atomic_load_n(w, __ATOMIC_SEQ_CST); }
    static u32 LoadWord(const volatile u32* w) { return __atomic_load_n(w, __ATOMIC_ACQUIRE); }
    static void Barrier() {
#if !defined(__SANITIZE_THREAD__)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    }
};

/// SWAP_ENTER (and the power menu): closes the gate and waits for the game threads counted in it, at most `timeout_us`
/// (`step` between two looks: a short sleep on the console). Returns how many are left (0: the plugin's block may go). The gate
/// stays closed either way: a thread that comes later goes straight to the kernel. Never under a lock that pre/post may take.
template <class Ops = GateAtomics, class Now, class Step>
inline u32 GateClose(GateHeader& g, u64 timeout_us, Now now_us, Step step, HookGateStats* stats) {
    Ops::StoreFlag(&g.flag, 0u);
    Ops::Barrier(); // (Dekker: the flag is out before the count is read)
    const u64 start = now_us();
    u32 left = Ops::LoadCount(&g.in_flight);
    const u32 at_start = left;
    while (left != 0 && now_us() - start < timeout_us) {
        step();
        left = Ops::LoadCount(&g.in_flight);
    }
    if (stats) {
        const u64 waited = now_us() - start;
        ++stats->closes;
        stats->in_flight_start = at_start;
        stats->in_flight_left = left;
        stats->waited_us = waited > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(waited);
        stats->last_site = Ops::LoadWord(&g.last_site);
        stats->last_handle = Ops::LoadWord(&g.last_handle);
        stats->post_skipped = Ops::LoadWord(&g.post_skipped);
    }
    return left;
}

/// The plugin's block is back (and mapped): the gate calls pre/post again. Any time after the block is mapped; idempotent.
template <class Ops = GateAtomics>
inline void GateOpen(GateHeader& g, HookGateStats* stats) {
    const bool was_closed = Ops::LoadWord(&g.flag) == 0;
    Ops::StoreFlag(&g.flag, 1u);
    if (stats && was_closed) {
        ++stats->opens;
    }
}

/// True while the gate is closed or closing: a wait of the plugin's code on a game thread (in pre/post) must end now, the closer
/// is waiting for it.
inline bool GateClosing(const GateHeader& g) {
    return __atomic_load_n(&g.flag, __ATOMIC_ACQUIRE) == 0;
}

} // namespace azl
