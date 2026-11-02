// The console side of the gate of the plugin's threads around the swap of its memory on an Old 3DS (azl/swap_gate.hpp).
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace ctr {

/// Before the plugin's threads start. On a New 3DS it does nothing and the gate stays off for good: Luma3DS swaps on an Old 3DS only.
void InitSwapGate();
/// SWAP_ENTER, on the framework's keep-alive thread, with the game's threads out of the hook already: they may wait for what a thread stopped here holds.
/// Waits at most `timeout_ms`; the log says who was left.
void CloseSwapGate(u32 timeout_ms);
/// SWAP_EXIT, and the plugin's shutdown (a thread stopped at the gate could never be joined).
void OpenSwapGate();

} // namespace ctr
} // namespace azl
