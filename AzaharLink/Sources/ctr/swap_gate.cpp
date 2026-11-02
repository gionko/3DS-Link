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

// A wait with no end: the thread has nothing to do until SWAP_EXIT, and Luma3DS stops it anyway before the block goes. (The slices are for an event that could not be made.)
void ParkAtGate() {
    if (g_gate_open != 0) {
        svcWaitSynchronization(g_gate_open, -1);
    } else {
        svcSleepThread(kSwapGateSliceNs);
    }
}

} // namespace

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
    GateCloseAndDrain(
        g_swap_gate, static_cast<u64>(timeout_ms) * 1000, []() { return platform::NowUs(); }, []() { svcSleepThread(kSwapGateStepNs); });
}

void OpenSwapGate() {
    GateOpen(g_swap_gate);
    if (g_gate_open != 0) {
        svcSignalEvent(g_gate_open);
    }
}

} // namespace ctr
} // namespace azl
