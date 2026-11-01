// Redirects the game's svcSendSyncRequest / svcCloseHandle calls to our code, wherever and however
// the game's compiler placed them (shared stub or inlined at hundreds of call sites): answers
// IPC to "nwm::UDS" / "ndm:u" with UdsHle and lets everything else reach the real kernel.
#pragma once

#include <3ds.h>

#include "azl/hook_swap.hpp"
#include "ctr/uds_hle.hpp"

namespace azl {
namespace ctr {

struct HookStats {
    u32 text_start = 0;
    u32 text_size = 0;
    u32 send_sync_sites = 0;   ///< `svc 0x32` call sites redirected
    u32 close_sites = 0;       ///< `svc 0x23` call sites redirected
    u32 skipped_sites = 0;     ///< sites that could not be hooked safely (see the log)
    u32 framework_sites = 0;   ///< svc 0x32 sites left alone: CTRPluginFramework hooks them itself
    u32 thumb_candidates = 0;  ///< Thumb `svc 0x32` encodings seen (not hooked)
    u32 gsp_stub_wrappers = 0; ///< GSP wrappers whose `bl` to the svc stub was rewritten as the svc itself (see azl/gsp_wrapper.hpp)
    bool trampolines_ready = false;
};

/// A game whose GSP wrapper has its `svc 0x32` behind a stub (LEGO Batman 2) makes CTRPluginFramework abort at start unless the `bl` to the stub is
/// written as the `svc` itself before the framework searches (azl/gsp_wrapper.hpp). Reads the game's .text and writes one word: called from Plugin::EarlyInit
/// (PatchProcess) whether the plugin is on or off, BEFORE InstallSvcHooks. Returns how many wrappers it rewrote.
u32 RewriteStubbedGspWrappers();

/// Scans the game's .text and redirects every ARM `svc 0x32` / `svc 0x23`.
/// Must run before the game executes (PatchProcess).
HookStats InstallSvcHooks(UdsHle* hle);

/// Told when the game's APT session gets the system's "the console goes to sleep now" (`enter`) and "it woke up"
/// notifications. Called on the game's own thread, right after the call returned: it must be short and must never
/// wait on a kernel object or issue an IPC call (the game's command buffer is in use); engine entry points are fine.
using SleepObserver = void (*)(bool enter);
void SetSleepObserver(SleepObserver observer);

/// Told, with the log on, of the whole sleep protocol of the game's APT session: every notification (kind
/// kAptNoteNotification, the value its number), the game's answer to the sleep query (kAptNoteReplyQuery) and its
/// acknowledgement of a sleep (kAptNoteReplyComplete, told before the call). Same rules as the sleep observer, and
/// shorter still: it may only store a few words and signal an event (a thread of its own writes the trail down).
using AptObserver = void (*)(u32 kind, u32 value);
void SetAptObserver(AptObserver observer);

/// Told (with the log on) when the game's APT session gets the HOME button notification (ids 1 and 2). Same rules as
/// the sleep observer, and shorter still: it may only signal an event (the writer thread does the SD card work).
using HomeObserver = void (*)();
void SetHomeObserver(HomeObserver observer);

/// The swap of the plugin's block on an Old 3DS (azl/hook_swap.hpp, svc_entry.s). SuspendSvcHooks runs when the framework says the swap is about to
/// happen (SWAP_ENTER, on its keep-alive thread, before it answers Luma3DS): every call site goes back to the game's own words, and the threads that are inside
/// the hook are waited for (at most `timeout_ms`; the report says how many were left). ResumeSvcHooks runs at SWAP_EXIT. RearmHandedBackSites runs on a plugin
/// thread (the frame callback): a call that was handed back to its call site and no swap followed goes back into the hook after a while.
void SuspendSvcHooks(u32 timeout_ms);
void ResumeSvcHooks();
void RearmHandedBackSites();
u32 HooksInFlight();
SwapStats GetSwapStats();

/// Logs the sites whose first word is no longer ours (somebody else patched them after us). Call it
/// once the framework has finished its own initialisation (LateInit).
void ReportForeignPatches();

} // namespace ctr
} // namespace azl
