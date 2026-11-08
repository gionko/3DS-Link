// Redirects the game's svcSendSyncRequest / svcCloseHandle calls to our code, wherever and however
// the game's compiler placed them (shared stub or inlined at hundreds of call sites): answers
// IPC to "nwm::UDS" / "ndm:u" with UdsHle and lets everything else reach the real kernel.
#pragma once

#include <3ds.h>

#include "azl/apt_sleep.hpp"
#include "azl/hook_swap.hpp"
#include "ctr/gate_flag.hpp"
#include "ctr/uds_hle.hpp"

/// The plugin's side of the gate (Sources/ctr/gate.s), on the game's thread that made the call. pre returns r0 = the handle for the svc
/// (or the answer) and r1 = what the gate is to do: 0 answered, 1 the svc, 2 and up the svc and then post (which reads the value back in
/// ctx[3]). post returns the result. ctx: four words on the game's stack, values only. The gate's header holds their addresses
/// (InstallSvcHooks); GateClosing() (ctr/gate_flag.hpp) is what the waits on a game thread look at.
extern "C" u64 azl_pre32(Handle handle, u32 stub_ret, u32* ctx);
extern "C" Result azl_post32(Result result, u32 stub_ret, u32* ctx);
extern "C" u64 azl_pre23(Handle handle, u32 stub_ret, u32* ctx);
extern "C" Result azl_post23(Result result, u32 stub_ret, u32* ctx);

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
    bool gate_ready = false;   ///< the gate's page is there (ctr/gate_page.hpp) and the sites point at its stubs
    const char* no_gate = nullptr; ///< ... and when it is not, why (the hooks stay out for this launch: the notice on the screen says it)
};

/// A game whose GSP wrapper has its `svc 0x32` behind a stub (LEGO Batman 2) makes CTRPluginFramework abort at start unless the `bl` to the stub is
/// written as the `svc` itself before the framework searches (azl/gsp_wrapper.hpp). Reads the game's .text and writes one word: called from Plugin::EarlyInit
/// (PatchProcess) whether the plugin is on or off, BEFORE InstallSvcHooks. Returns how many wrappers it rewrote.
u32 RewriteStubbedGspWrappers();

/// Scans the game's .text, takes the gate's page (ctr/gate_page.hpp), writes the gate and one stub per site there, and then turns every ARM
/// `svc 0x32` / `svc 0x23` into ONE word, `b stub` (azl/gate_plan.hpp, WriteGate). Nothing of the game's text is written again after this.
/// Must run before the game executes (PatchProcess).
HookStats InstallSvcHooks(UdsHle* hle, bool reserve_exit_tramp);

/// LateInit, the plugin's main thread: the books of the gate's page back in line once the game has its heap (ctr/gate_page.hpp).
void SettleGatePage();

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

/// Called (with the log on) on the game's thread when its APT:ReceiveParameter brought the close command (12), after the lines of that call: the
/// thread may give the trail a moment to put them on the card before the game can be killed (Plugin::WaitForCloseTrail: bounded, ended by the
/// gate's close, never on an Old 3DS, whose HOME goes through the swap of the block and its own budget).
using TrailWaiter = void (*)();
void SetTrailWaiter(TrailWaiter waiter);

/// Luma3DS's plugin word (azl/apt_sleep.hpp, kLumaPluginWordAddress). Luma writes it through the uncached alias of its physical page
/// (PA | 0x80000000, mapped by Luma's kernel for every process); the configuration page itself is a CACHED mapping, whose copy read within
/// about 1 ms of a change is the old value (REGISTRO, phase Q: the probes of phase O read that copy). g_luma_word_ptr is the alias once
/// SetupLumaWordAlias found it (EarlyInit, a New 3DS with Luma3DS), else the configuration page.
extern const volatile u32* g_luma_word_ptr;
inline u32 LumaPluginWord() {
    return *g_luma_word_ptr;
}
/// The cached copy, for the lines that compare the two.
inline u32 LumaPluginWordCached() {
    return *reinterpret_cast<const volatile u32*>(kLumaPluginWordAddress);
}
/// EarlyInit: g_luma_word_ptr to the uncached alias (svcControlProcess PROCESSOP_GET_PA_FROM_VA, Luma3DS's own svc). False (and the cached
/// page kept) on an emulator or when the svc does not answer an address the alias covers.
bool SetupLumaWordAlias();
/// The alias, writable (the exit guard only): nullptr when SetupLumaWordAlias did not set it.
volatile u32* LumaWordAliasForWrite();

/// The exit guard (azl/exit_guard.hpp, Sources/ctr/exit_tramp.s): every ARM `svc 0x03` word of the game's text turned into a branch to the
/// trampoline copied into the tail of the gate's page. A New 3DS with the gate only; before the game runs (EarlyInit, after InstallSvcHooks).
struct ExitGuardStats {
    u32 sites = 0;        ///< ARM svc 0x03 words found in the game's text
    u32 hooked = 0;       ///< ... turned into the branch
    u32 thumb = 0;        ///< Thumb `svc 3` halfwords (counted only: not hooked)
    u32 tramp = 0;        ///< where the trampoline's copy is (0: none)
    const char* why_not = nullptr;
};
ExitGuardStats InstallExitGuard(void (*guard)(), u32 stack_top);

/// The swap of the plugin's block on an Old 3DS (HOME, power, library applets): it only flips the gate's flag (azl/hook_swap.hpp, GateClose and
/// GateOpen). CloseGate runs at SWAP_ENTER, on the framework's keep-alive thread, before it answers Luma3DS, and outside every lock pre/post may
/// take: flag = 0, then a wait (at most `timeout_ms`) for the game's threads counted in the gate to leave the plugin's code; `wake(arg)` is called
/// once, with the flag down and a thread still counted, for a waiter that sleeps in slices (the Download Play hub's queue). Returns how many were left (0: the
/// block may go). OpenGate runs once the block is mapped again (OnSwapBack, after the framework's reply; SWAP_EXIT again: idempotent). Without a
/// gate (no page) both do nothing.
u32 CloseGate(u32 timeout_ms, void (*wake)(void*), void* arg);
/// After CloseGate and the plugin's threads: waits, reading the count only, for game threads still counted in the hook, up to what is left of
/// SWAP_ENTER's one deadline (ctr/swap_gate.hpp). Returns how many are left.
u32 WaitGateDrained();
/// The tick of the game's last APT ReceiveParameter(application) call, the moment Luma3DS raises its HOME/swap event (0: none yet).
u64 LastAptReceiveTick();
void OpenGate();

/// The gate's table of the plugin's fake sessions (Sources/ctr/gate.s, azl/gate_plan.hpp GateHeader::fake; risk O-A of analysis/piano-2026-11-08):
/// with the flag 0 (an Old 3DS's swap) the gate answers a svc 0x32 on one of them itself, `result` in cmdbuf[1], and marks a svc 0x23 on one in
/// fake_closed. Called under UdsHle's lock, the table's only writer. 1: in the table; 0: no gate; -1: the table is full (the session's calls
/// with the flag 0 then reach the kernel, as before).
int GateFakeAdd(Handle handle, u32 result);
void GateFakeRemove(Handle handle);
/// Plugin::Tick: for a fake session the game closed with the flag 0, what azl_pre23 would have done (UdsHle and the followed sessions forget it),
/// once the kernel says the handle is gone; a handle still alive is looked at again for a while, then let be.
void SettleGateFakeClosed();

/// What the gate saw (for the log of the swap): its last close and open, the posts skipped, and the table of the followed sessions.
struct GateStats {
    HookGateStats gate;
    u32 last_site_address = 0; ///< the call site of gate.last_site (0: not one of the stubs)
    u32 traced_stale = 0;      ///< followed sessions closed out of the hook's sight, found by their handle's index (azl/traced_sessions.hpp)
    u32 traced_evicted = 0;    ///< APT sessions that gave their slot to a newer one (the table was full)
    u32 traced_dropped = 0;    ///< sessions that could not be followed (the table was full)
    u32 fake_in_table = 0;     ///< fake sessions in the gate's table now
    u32 fake_hits = 0;         ///< svc 0x32 on a fake session answered by the gate with the flag 0 (all swaps)
    u32 fake_settled = 0;      ///< fake sessions closed with the flag 0 and forgotten once back
    u32 fake_full = 0;         ///< fake sessions that found the gate's table full
};
GateStats GetGateStats();

/// Logs the sites whose first word is no longer ours (somebody else patched them after us). Call it
/// once the framework has finished its own initialisation (LateInit).
void ReportForeignPatches();

} // namespace ctr
} // namespace azl
