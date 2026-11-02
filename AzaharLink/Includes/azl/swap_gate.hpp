// The gate of the plugin's own threads around the swap of the plugin's memory (Old 3DS): no service call of theirs may be under way when the block goes
// (see ctr/swap_gate.cpp for the console side, Tests/swap_gate_test.cpp for the tests).
//
// On an Old 3DS Luma3DS stops the plugin's threads and unmaps the plugin's block when the game goes to the HOME menu. Stopping a thread does not take back the service
// call it is waiting for: the service answers later, and the kernel, on the service's own thread, writes the answer into the client's memory. With that memory
// gone the kernel stops the console (the report: "kernel panic", current process `socket`, the answer of poll() into its malloc'd buffer, in the room's network
// thread, which sits in poll() 91% of the time). A request the service has not picked up yet falls in the same
// way on the way in. So at SWAP_ENTER the gate closes: who is inside a call is waited for, who comes later stops in front of the next one, with no call under way, until SWAP_EXIT.
//
// `inside` is counted before `closed` is read and `closed` is set before `inside` is read: either the closer sees the thread, or the thread sees the gate closed.
// The count is weighed by the kind of thread (kGateNet...), so that the log can say who was left. A thread never enters twice: it would wait for itself.
// `on` stays 0 on a New 3DS: there is no swap there, so the gate is one test of a flag.
#pragma once

#include "azl/common.hpp"

namespace azl {

constexpr u32 kGateNet = 1u << 0;     ///< the room's network thread, around each socket call
constexpr u32 kGateWriter = 1u << 8;  ///< the log's writer, around a pass of the SD card
constexpr u32 kGateTrail = 1u << 16;  ///< the thread of the sleep's trail, around its lines

struct SwapGateStats {
    u32 closes = 0;       ///< SWAP_ENTER handled
    u32 inside_start = 0; ///< `inside` when the last close began (weighed)
    u32 inside_left = 0;  ///< ... and when it gave up waiting (0: all had left)
    u32 waited_us = 0;    ///< how long the last close waited
    u32 opens = 0;        ///< SWAP_EXIT handled
};

struct SwapGate {
    volatile u32 on = 0;
    volatile u32 closed = 0;
    volatile u32 inside = 0;
    volatile u32 stops = 0; ///< how many times a thread stopped in front of it
    void (*park)() = nullptr; ///< waits until the gate may have opened
    SwapGateStats stats;
};

/// False when the gate is closed: the thread is not counted then.
inline bool GateTryEnter(SwapGate& g, u32 who) {
    __atomic_fetch_add(&g.inside, who, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g.closed, __ATOMIC_SEQ_CST) == 0) {
        return true;
    }
    __atomic_fetch_sub(&g.inside, who, __ATOMIC_SEQ_CST);
    return false;
}

inline void GateEnter(SwapGate& g, u32 who) {
    if (g.on == 0) {
        return;
    }
    while (!GateTryEnter(g, who)) {
        __atomic_fetch_add(&g.stops, 1u, __ATOMIC_RELAXED);
        g.park();
    }
}

inline void GateLeave(SwapGate& g, u32 who) {
    if (g.on != 0) {
        __atomic_fetch_sub(&g.inside, who, __ATOMIC_SEQ_CST);
    }
}

/// SWAP_ENTER: closes the gate and waits for who is inside, at most `timeout_us`. Returns who is left (weighed). The gate stays closed either way: who comes later must stop.
template <class Now, class Step>
inline u32 GateCloseAndDrain(SwapGate& g, u64 timeout_us, Now now_us, Step step) {
    if (g.on == 0) {
        return 0;
    }
    __atomic_store_n(&g.closed, 1u, __ATOMIC_SEQ_CST);
    const u64 start = now_us();
    u32 left = __atomic_load_n(&g.inside, __ATOMIC_SEQ_CST);
    g.stats.inside_start = left;
    while (left != 0 && now_us() - start < timeout_us) {
        step();
        left = __atomic_load_n(&g.inside, __ATOMIC_SEQ_CST);
    }
    const u64 waited = now_us() - start;
    g.stats.inside_left = left;
    g.stats.waited_us = waited > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(waited);
    ++g.stats.closes;
    return left;
}

/// SWAP_EXIT, or the plugin going away. The caller then wakes who is parked.
inline void GateOpen(SwapGate& g) {
    if (g.on == 0) {
        return;
    }
    if (__atomic_exchange_n(&g.closed, 0u, __ATOMIC_SEQ_CST) != 0) {
        ++g.stats.opens;
    }
}

/// The one gate of the plugin (defined with the other C bridges, azl/log.cpp).
extern SwapGate g_swap_gate;

/// Around a pass of a thread that makes many calls.
class SwapGateScope {
public:
    explicit SwapGateScope(u32 who) : who_(who) { GateEnter(g_swap_gate, who_); }
    ~SwapGateScope() { GateLeave(g_swap_gate, who_); }
    SwapGateScope(const SwapGateScope&) = delete;
    SwapGateScope& operator=(const SwapGateScope&) = delete;

private:
    u32 who_;
};

} // namespace azl
