// Redirects the game's svcSendSyncRequest / svcCloseHandle calls to our code, wherever and however
// the game's compiler placed them (shared stub or inlined at hundreds of call sites): answers
// IPC to "nwm::UDS" / "ndm:u" with UdsHle and lets everything else reach the real kernel.
#pragma once

#include <3ds.h>

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
    bool trampolines_ready = false;
};

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

/// Logs the sites whose first word is no longer ours (somebody else patched them after us). Call it
/// once the framework has finished its own initialisation (LateInit).
void ReportForeignPatches();

} // namespace ctr
} // namespace azl
