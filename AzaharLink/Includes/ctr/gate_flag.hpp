// The flag of the svc hook's gate, as the plugin's code on a game thread reads it (Sources/ctr/gate.s, azl/gate_plan.hpp: GateHeader::flag).
// 0 = the swap of the plugin's block (Old 3DS: HOME, power, library applets) is closing the gate and waits for every game thread counted in
// it to leave the plugin's code: a wait of the plugin's on a game thread (in pre/post, in the HLE they reach) must end at once then, with the
// answer it would give at its own limit.
// Sources/ctr/hook.cpp owns the word (until the gate's page exists, a word of the plugin's own that stays 1); the host builds of the HLE
// define it in their shim (Tests/ipc_test/shim_svc.cpp).
#pragma once

#include <3ds.h>

namespace azl {
namespace ctr {

extern volatile u32* g_gate_flag;

inline bool GateClosing() {
    return __atomic_load_n(g_gate_flag, __ATOMIC_ACQUIRE) == 0;
}

} // namespace ctr
} // namespace azl
