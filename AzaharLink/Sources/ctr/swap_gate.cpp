#include "ctr/swap_gate.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include "azl/platform.hpp"
#include "azl/swap_gate.hpp"

namespace azl {
namespace ctr {

namespace {

constexpr s64 kSwapGateStepNs = 1000000LL;  // how often the close looks at who is inside
constexpr s64 kSwapGateSliceNs = 2000000LL; // a thread's stop with no event

Handle g_gate_open = 0; // sticky, signalled while the gate is open

constexpr u64 kTicksPerMs = SYSCLOCK_ARM11 / 1000;
u64 g_swap_start_tick = 0;    // where the deadline counts from (Luma's event, or the callback): the keep-alive thread only
u64 g_swap_deadline_tick = 0; // 0: no SWAP_ENTER running
bool g_swap_raised = false;   // Rosalina waits 15 s (set once, at EarlyInit)
SwapDeadlineStats g_swap_deadline_stats;

// A wait with no end: the thread has nothing to do until SWAP_EXIT, and Luma3DS stops it anyway before the block goes. (The slices are for an event that could not be made.)
void ParkAtGate() {
    if (g_gate_open != 0) {
        svcWaitSynchronization(g_gate_open, -1);
    } else {
        svcSleepThread(kSwapGateSliceNs);
    }
}

} // namespace

void SetSwapDeadlineRaised(bool raised) {
    g_swap_raised = raised;
}

void BeginSwapDeadline(u64 anchor_tick) {
    const u64 now = svcGetSystemTick();
    const u64 with_anchor = static_cast<u64>(g_swap_raised ? kSwapDeadlineRaisedMs : kSwapDeadlineMs) * kTicksPerMs;
    const u64 without = static_cast<u64>(g_swap_raised ? kSwapDeadlineRaisedNoAnchorMs : kSwapDeadlineNoAnchorMs) * kTicksPerMs;
    const u64 age = anchor_tick != 0 && anchor_tick <= now ? now - anchor_tick : ~0ull;
    g_swap_deadline_stats = SwapDeadlineStats{};
    if (age < static_cast<u64>(kSwapAnchorMaxAgeMs) * kTicksPerMs) {
        g_swap_start_tick = anchor_tick;
        g_swap_deadline_tick = anchor_tick + with_anchor;
        g_swap_deadline_stats.anchored = 1;
    } else if (age < with_anchor && anchor_tick + with_anchor <= now + without) {
        // (an older anchor, still inside its deadline: Luma3DS's event came no earlier than it and no later than now, so the earlier one)
        g_swap_start_tick = anchor_tick;
        g_swap_deadline_tick = anchor_tick + with_anchor;
        g_swap_deadline_stats.anchored = 2;
    } else {
        g_swap_start_tick = now;
        g_swap_deadline_tick = now + without;
    }
    g_swap_deadline_stats.deadline_ms = static_cast<u32>((g_swap_deadline_tick - g_swap_start_tick) / kTicksPerMs);
    g_swap_deadline_stats.anchor_age_ms = age == ~0ull ? 0xFFFFFFFFu : age / kTicksPerMs > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(age / kTicksPerMs);
}

void EndSwapDeadline() {
    g_swap_deadline_tick = 0;
}

u64 SwapTicksLeft() {
    if (g_swap_deadline_tick == 0) {
        return ~0ull;
    }
    const u64 now = svcGetSystemTick();
    return now < g_swap_deadline_tick ? g_swap_deadline_tick - now : 0;
}

u32 SwapClampMs(u32 ms) {
    const u64 left_ms = SwapTicksLeft() / kTicksPerMs;
    return left_ms < ms ? static_cast<u32>(left_ms) : ms;
}

const SwapDeadlineStats& LastSwapDeadline() {
    return g_swap_deadline_stats;
}

void NoteSwapWaitCut(u32 bit) {
    g_swap_deadline_stats.cut |= bit;
}

void NoteSwapAnswered() {
    const u64 took = (svcGetSystemTick() - g_swap_start_tick) / kTicksPerMs;
    g_swap_deadline_stats.answered_ms = took > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(took);
}

void InitSwapGate() {
    if (CTRPluginFramework::System::IsNew3DS()) {
        return;
    }
    if (R_SUCCEEDED(svcCreateEvent(&g_gate_open, RESET_STICKY))) {
        svcSignalEvent(g_gate_open);
    } else {
        g_gate_open = 0;
    }
    g_swap_gate.park = ParkAtGate;
    g_swap_gate.on = 1;
}

void CloseSwapGate(u32 timeout_ms) {
    if (g_gate_open != 0) {
        svcClearEvent(g_gate_open); // (before the gate closes: a thread stopped by it must find the event clear)
    }
    const u32 budget_ms = SwapClampMs(timeout_ms); // (the one deadline of SWAP_ENTER: SwapClampMs)
    const u32 left = GateCloseAndDrain(
        g_swap_gate, static_cast<u64>(budget_ms) * 1000, []() { return platform::NowUs(); }, []() { svcSleepThread(kSwapGateStepNs); });
    if (budget_ms < timeout_ms && left != 0) {
        NoteSwapWaitCut(8); // (the deadline ended it with a thread still inside, not just a shorter budget)
    }
}

void OpenSwapGate() {
    GateOpen(g_swap_gate);
    if (g_gate_open != 0) {
        svcSignalEvent(g_gate_open);
    }
}

} // namespace ctr
} // namespace azl
