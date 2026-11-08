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

/// The ONE deadline of an Old 3DS's SWAP_ENTER (2026-11-08, risk O-B of analysis/piano-2026-11-08): Luma3DS waits for the framework's answer
/// from ITS event, the end of the game's APT ReceiveParameter (k11 SendSyncRequest.c), and the framework only fetches the event up to 250 ms
/// later and does its own work after our callback. So every wait of SWAP_ENTER (the chat's mirror, the trail, the hooks' drain, the plugin's
/// threads, a second drain) takes the smaller of its own budget and what is left of one deadline: `anchor_tick` (the tick stored in the hook's
/// APT pre, 0 when none) + kSwapDeadlineMs when it is fresh (younger than kSwapAnchorMaxAgeMs); an older one that is still inside the
/// deadline gives the earlier of (anchor + kSwapDeadlineMs) and (callback + kSwapDeadlineNoAnchorMs) (the event can be no earlier than the
/// anchor, nor later than the callback); none, or a stale one: the callback + kSwapDeadlineNoAnchorMs. With Rosalina's wait raised to 15 s
/// (Plugin::RaiseRosalinaReplyWait, SetSwapDeadlineRaised) the two are kSwapDeadlineRaisedMs and kSwapDeadlineRaisedNoAnchorMs: the
/// waits keep their own budgets (8.64 s in all) and the deadline only cuts what Luma3DS would not wait for.
constexpr u32 kSwapDeadlineMs = 8000;
constexpr u32 kSwapDeadlineNoAnchorMs = 7500;
constexpr u32 kSwapDeadlineRaisedMs = 12500;
constexpr u32 kSwapDeadlineRaisedNoAnchorMs = 12000;
constexpr u32 kSwapAnchorMaxAgeMs = 2000;
void SetSwapDeadlineRaised(bool raised);
void BeginSwapDeadline(u64 anchor_tick);
void EndSwapDeadline();
/// Ticks left of the deadline: ~0 when no deadline runs (the waits keep their own budgets), 0 once it passed.
u64 SwapTicksLeft();
/// `ms`, or less if the deadline comes first.
u32 SwapClampMs(u32 ms);
/// What the last SWAP_ENTER did with its deadline, for the writer's line: ms from Luma's event (or the callback) to the end of our part,
/// the anchor (0 none or stale, 1 fresh, 2 older: the earlier deadline), the deadline in ms, and a bit per wait the deadline ended with
/// something still left (1 mirror, 2 trail, 4 hooks, 8 plugin's threads, 16 second drain).
struct SwapDeadlineStats {
    u32 answered_ms = 0;
    u32 anchored = 0;
    u32 deadline_ms = 0;
    u32 anchor_age_ms = 0;
    u32 cut = 0;
};
const SwapDeadlineStats& LastSwapDeadline();
void NoteSwapWaitCut(u32 bit);
void NoteSwapAnswered();

} // namespace ctr
} // namespace azl
