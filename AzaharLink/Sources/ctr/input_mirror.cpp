// The input mirror page (ctr/input_mirror.hpp): the console's side of azl/hid_mirror.hpp - the kernel's calls (Luma3DS's svcs included),
// the lock between the threads that switch and give back, and the plugin's thread's work of a frame.
//
// Who does what:
// - the plugin's thread (ChatFrame -> MirrorFrame) makes the switch when the keyboard opens, refreshes the mirror's live parts every frame,
//   waits for the release when the keyboard closes and gives the page back - or, with something still down at the wait's cap, keeps the
//   mirror as the real input without it until it is let go (MaskFrame); it is the only one that allocates, frees and logs;
// - the framework's keep-alive thread (MirrorProcessEvent) gives the page back at HOME_ENTER, SWAP_ENTER, SLEEP_ENTER and EXIT, before the
//   plugin takes its hooks out (Old 3DS: the mirror's page is in the plugin's block, which Luma3DS frees right after the answer). The
//   mirror is an alias of the plugin's memory, and an alias left mapped outlives the game (console 63, 2026-11-06): at the game's last
//   event (EXIT; SWAP_ENTER, on an Old 3DS the net for an end with no EXIT event) it waits up to 1 s for the lock, with the plugin's
//   thread standing aside, and takes the mirror off; nothing is changed without the lock, so a lock not had then leaves the mirror mapped,
//   said in the shutdown's line;
// - a game's thread (MirrorRestoreFromGame, from the hook) gives it back when a notification takes the game away, if nobody holds the lock.
// No new thread, no new hook, no kernel object: a word for the lock (azl/hid_mirror.hpp, TryMutex), a few atomics.
#include "ctr/input_mirror.hpp"

#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <atomic>
#include <cstdlib>
#include <malloc.h>

#include "azl/chat_host.hpp"
#include "azl/hid_mirror.hpp"
#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/sleep_crumbs.hpp"
#include "azl/stats.hpp"
#include "azl/ticks.h"
#include "csvc.h"
#include "ctr/gate_flag.hpp"
#include "ctr/plugin.hpp"
#include "ctr/zombie_probe.hpp"
#include "ctr/swap_gate.hpp"

extern "C" {
// The predicate's state, written by the thread that holds the mirror's lock before each pass of the hold (one pass at a time).
volatile u32 azl_mirror_pred_calls = 0;
volatile u32 azl_mirror_pred_caller_tls = 0;
// Not 0: the hold and the let-go touch no thread. Set (under the lock, then cleared) by a way back made while the framework's pause holds
// the game (its menu over the game, Process::Pause): libctrpf's LockGameThreads is PROCESSOP_SCHEDULE_THREADS too, on the same bit of the
// kernel's thread, so our let-go would wake the game under the menu. Its threads are held already. 0 whenever a game's thread runs.
volatile u32 azl_mirror_pred_off = 0;

// The zombie probe's counters (ctr/zombie_probe.hpp reads them). freeze/unfreeze: how many times the keyboard's hold of the game's threads
// was taken and let go, to catch an imbalance that would leave a thread held (0x20) at the exit. (The cross-core signal is read per thread
// at the events instead - ThreadSchedDump compares each notable thread's core with the caller's - so the frameless predicate is left alone.)
volatile u32 azl_mirror_freeze_count = 0;
volatile u32 azl_mirror_unfreeze_count = 0;

// Runs in the KERNEL, inside PROCESSOP_SCHEDULE_THREADS (Luma3DS's ControlProcess.c: svc mode, the scheduler's lock held), once per thread
// of this process and pass, on the caller's core and in this process's address space (the TLS pages are ours). True: hold this thread.
// Not the caller (it would hold itself: the svc stops the current thread too), not a plugin thread (the mark), nothing without a TLS
// (azl/hid_mirror.hpp, HoldThread, inlined: Tests/hid_mirror_test.cpp runs it inside Luma3DS's loop). No call, no frame to speak of.
__attribute__((noinline, used)) bool azl_mirror_thread_predicate(const u32* kthread) {
    azl_mirror_pred_calls = azl_mirror_pred_calls + 1;
    return azl::hidmirror::HoldThread(kthread, azl_mirror_pred_caller_tls, azl_mirror_pred_off);
}
}

namespace azl {
namespace ctr {

namespace {

using hidmirror::Outcome;

constexpr s64 kSettleNs = 100000;            // the pause after a pass of the hold (the interrupt to the other core)
constexpr u64 kSettleTicks = SYSCLOCK_ARM11 / 10000; // ... the same as ticks, for a game's thread that spins instead
constexpr s32 kThreadCap = 64;               // the most threads the hold is trusted to have seen (Cap())
// The keep-alive thread waits for the lock at most 1 s, looking every 1 ms, with the plugin's thread standing aside (event_waiting): the
// longest work under the lock - the hold of the threads, an unmap and a map - took 620-1007 us in 56 samples on console (logs-friend_a,
// prova-B, 2026-11-06; median 701 us, 55 of them under 1 ms), so a lock not had in 1 s is held by a thread that no longer runs. Luma3DS
// waits 10 s for the answer. The same 1 s at HOME_ENTER and SLEEP_ENTER, where a lock not had only leaves the way back to the plugin's
// thread: accepted because the lock is taken only while the keyboard opens or closes (about 1 ms each), so at almost every HOME or sleep it
// is free at once and otherwise free within about 1 ms; the full 1 s is waited only when its holder no longer runs.
constexpr u64 kEventLockTicks = SYSCLOCK_ARM11;
constexpr s64 kEventLockStepNs = 1000000;
constexpr u64 kPauseLockTicks = SYSCLOCK_ARM11 / 10; // the in-game menu's wait for the lock, at most 100 ms (MirrorBeforePause)
constexpr s64 kPauseLockStepNs = 200000;
constexpr u32 kRetryMs = 500;                // a way back that left the mirror is tried again this often
// The plugin's thread, with the log on, right before it holds the game's threads to give the page back: how long it gives the trail
// (AzaharLink.sleep.log) to put "about to hold" on the card, looking every 2 ms (2026-11-10; GiveTrailTime).
constexpr u64 kTrailWaitTicks = SYSCLOCK_ARM11 / 10;
constexpr s64 kTrailStepNs = 2000000;

// svc 0xA0 (Luma3DS's MapProcessMemoryEx) in its first form, five arguments: r0 the destination process, r4 the size. libctrpf's stub is
// the later six-argument form, which puts a magic number in r0 and the flags in r5 (Includes/csvc.h): a Luma3DS from before that form takes
// the magic number for a handle and fails. Every Luma3DS takes this one (the later ones clear the flags when r0 is not the magic number), and
// the mapping is then shared (0x5806), which is what azl/hid_mirror.hpp checks for.
__attribute__((noinline)) Result MapProcessMemoryEx5(Handle dst, u32 dst_va, Handle src, u32 src_va, u32 size) {
    register u32 r0 asm("r0") = dst;
    register u32 r1 asm("r1") = dst_va;
    register u32 r2 asm("r2") = src;
    register u32 r3 asm("r3") = src_va;
    register u32 r4 asm("r4") = size;
    asm volatile("svc 0xA0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : "r"(r4) : "r12", "memory");
    return static_cast<Result>(r0);
}

// What the threads share (a static: as long as the plugin is).
struct Shared {
    hidmirror::TryMutex lock;
    hidmirror::Switcher sw;             // under the lock
    std::atomic<u32> active{0};         // sw.active(), for a look without the lock
    std::atomic<u32> page{0};           // the mirror's address in the plugin's heap (0: none)
    std::atomic<u32> closing{0};        // HOME, power, sleep, exit, swap: a switch wanted before one of them is not made
    std::atomic<u32> want_restore{0};   // a game's thread found the lock taken: the plugin's thread gives the page back
    std::atomic<u32> dead{0};           // EXIT: never again
    std::atomic<u32> event_waiting{0};  // the keep-alive thread waits for the lock: the plugin's thread does not take it meanwhile
    // The last way back made by another thread, for the plugin's thread's log (the fields under the lock).
    std::atomic<u32> report_seq{0};
    u32 report_where = 0, report_outcome = 0, report_ticks = 0;
};
Shared g_m;

// (or the framework's event number; the plugin's thread logs its own. The trail's lines say who by the same numbers.)
enum Where : u32 { kWherePlugin = SleepCrumbs::kMirrorByPlugin, kWhereNotification = SleepCrumbs::kMirrorByNotification };
static_assert(chat::kEventExit == 0 && chat::kEventSleepEnter == 1 && chat::kEventHomeEnter == 3 && chat::kEventSwapEnter == 5,
              "the trail names the framework's events that give the page back by these numbers (azl/sleep_crumbs.hpp, MirrorWhoText)");
std::atomic<u32> g_last{kMirrorLastNone}; // what became of the mirror at the game's last event (MirrorLastState)

// >>> game-thread code (Tests/policy_check.sh: the way back runs on the game's thread that got a notification: no heap, no line, no sleep)
// The kernel's calls of azl/hid_mirror.hpp's Switcher. `spin`: on a game's thread the pause spins - it never sleeps in the plugin's code.
struct CtrOps {
    u32 mirror = 0;
    bool spin = false;

    bool Query(u32 va, hidmirror::Region* out) {
        MemInfo mi;
        PageInfo pi;
        if (svcQueryMemory(&mi, &pi, va) != 0) {
            return false;
        }
        out->base = mi.base_addr;
        out->size = mi.size;
        out->perm = mi.perm;
        out->state = mi.state;
        return true;
    }
    s32 Freeze(u32* calls) {
        azl_mirror_pred_calls = 0;
        azl_mirror_freeze_count = azl_mirror_freeze_count + 1;
        const Result rc = svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_SCHEDULE_THREADS, 1, reinterpret_cast<u32>(&azl_mirror_thread_predicate));
        *calls = azl_mirror_pred_calls;
        return rc;
    }
    void Unfreeze() {
        azl_mirror_unfreeze_count = azl_mirror_unfreeze_count + 1;
        svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_SCHEDULE_THREADS, 0, reinterpret_cast<u32>(&azl_mirror_thread_predicate));
    }
    void Settle() {
        if (spin) {
            const u64 end = svcGetSystemTick() + kSettleTicks;
            while (svcGetSystemTick() < end) {
            }
        } else {
            svcSleepThread(kSettleNs);
        }
    }
    // The number of the process's threads. NEVER svcGetThreadList: the kernel's GetThreadList takes a reference on the
    // process (IncrementRefcount for CUR_PROCESS_HANDLE, ConvertToAutoObject for a handle) and never gives it back
    // (decompiled kernel 11.14, GetThreadList: "they forget to decref the process!"). Each call left the game one reference
    // to itself, so a game in which the chat's keyboard had switched the mirror was TERMINATED at HOME -> Close but never
    // finalized: all its memory stayed until the console restarted, and the next launch of Monster Hunter XX got heap 0
    // (2026-11-07, REGISTRO phase K). GetProcessInfo type 7 counts the same threads (KProcess::DumpThreadIds with no list)
    // and gives its reference back on every path. (Tests/policy_check.sh forbids svcGetThreadList in the plugin.)
    // (out of line, as before: its frame stays out of Switcher::Hold's 32-byte budget, Tests/stack_usage.sh)
    __attribute__((noinline)) s32 Threads() {
        s64 n = 0;
        if (svcGetProcessInfo(&n, CUR_PROCESS_HANDLE, 7) != 0) {
            return -1;
        }
        return static_cast<s32>(n);
    }
    s32 Cap() const {
        return kThreadCap;
    }
    s32 UnmapReal(u32 va) {
        return svcUnmapMemoryBlock(hidMemHandle, va);
    }
    s32 MapReal(u32 va, u32 perm) {
        return svcMapMemoryBlock(hidMemHandle, va, static_cast<MemPerm>(perm), MEMPERM_DONTCARE);
    }
    s32 MapMirror(u32 va) {
        return MapProcessMemoryEx5(CUR_PROCESS_HANDLE, va, CUR_PROCESS_HANDLE, mirror, hidmirror::kPageBytes);
    }
    s32 UnmapMirror(u32 va) {
        return svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, va, hidmirror::kPageBytes);
    }
};

// The trail's two lines of a way back (AzaharLink.sleep.log, debug=1 only: at debug=0 one load and out), from whichever thread makes it:
// "about to hold the game's threads" BEFORE the hold and "the page is back" AFTER the let-go, each with the number of the game's
// threads (2026-11-10: a console stopped 1.6-4.6 s after the keyboard closed, RR3D on a test build, exactly when the way back was due, and
// neither log could say whether the plugin was inside it; the main log's lines come out after the let-go and go through the writer's
// queue). A few stores and an event signal (Plugin::NoteSleepEvent, what the hook's APT observer calls on a game's thread too): no wait
// here - only the plugin's thread gives the trail time to reach the card (GiveTrailTime).
// `low`: who gives it back (kChatMirrorBackBegins) or the outcome (the other kinds); `ticks`: how long the game was held.
__attribute__((noinline)) void BackCrumb(u32 kind, u32 low, u32 ticks) {
    if (g_debug_level <= 0) {
        return;
    }
    CtrOps ops;
    const s32 threads = ops.Threads();
    Plugin::NoteSleepEvent(kind, SleepCrumbs::PackMirror(low, threads > 0 ? static_cast<u32>(threads) : 0,
                                                         static_cast<u32>(static_cast<u64>(ticks) * 1000 / AZL_TICKS_PER_MS)));
}

// Under the lock: the way back; what another thread did is kept for the plugin's thread's log. `ticks`: how long the game was held.
// (One function, not a clone per caller: Tests/stack_usage.sh holds its frame by name.)
__attribute__((noinline, noclone)) Outcome RestoreLocked(bool spin, bool may_fall_back, u32 where, u32* ticks) {
    CtrOps ops;
    ops.mirror = g_m.page.load(std::memory_order_relaxed);
    ops.spin = spin;
    azl_mirror_pred_caller_tls = reinterpret_cast<u32>(getThreadLocalStorage());
    const u64 start = svcGetSystemTick();
    const Outcome out = g_m.sw.Restore(ops, may_fall_back);
    *ticks = static_cast<u32>(svcGetSystemTick() - start);
    g_m.active.store(g_m.sw.active() ? 1u : 0u, std::memory_order_release);
    if (where != kWherePlugin) {
        g_m.report_where = where;
        g_m.report_outcome = static_cast<u32>(out);
        g_m.report_ticks = *ticks;
        g_m.report_seq.fetch_add(1, std::memory_order_release);
    }
    return out;
}

} // namespace

// A game's thread, in the hook: the closing counted first (a switch the plugin's thread has not made yet will not be made), then the page
// back if it is there and the lock is free; else the flag. The common case - the mirror never used - is one add and one load.
void MirrorRestoreFromGame() {
    g_m.closing.fetch_add(1, std::memory_order_seq_cst);
    if (g_m.page.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    if (!g_m.lock.TryLock()) {
        g_m.want_restore.store(1, std::memory_order_release);
        return;
    }
    if (g_m.sw.active()) {
        u32 ticks = 0;
        BackCrumb(SleepCrumbs::kChatMirrorBackBegins, kWhereNotification, 0); // (pushed, never waited for: this is a game's thread)
        RestoreLocked(true, true, kWhereNotification, &ticks);
        BackCrumb(SleepCrumbs::kChatMirrorBackDone, g_m.report_outcome, ticks); // (the outcome RestoreLocked left, under the lock)
    }
    g_m.lock.Unlock();
}
// <<< game-thread code

namespace {

// The plugin's thread's own.
u8* g_page = nullptr;              // the mirror (memalign'd from the plugin's heap; g_m.page says it to the others)
bool g_want = false;               // the keyboard is open: the switch is wanted
u32 g_want_closing = 0;            // g_m.closing when it was wanted: the one taken before that frame's look (MirrorBeforeLook)
u32 g_look_closing = 0;            // g_m.closing before this frame's look at the chat's signals
bool g_tried = false;              // this opening's switch was tried (it is not tried again until the next opening)
int g_supported = -1;              // -1: not looked at yet
u32 g_report_logged = 0;
u32 g_retry_ms = 0;
bool g_retry = false;
hidmirror::Released g_released;
hidmirror::ReleaseWait g_release_wait;
u32 g_unquiet_seen = 0;            // hidmirror::Unquiet's bits of every look of this wait for the release (the line of a capped wait)
hidmirror::HeldMask g_held;        // what was still down when a wait ended at its cap: masked for the game until let go (MaskFrame)

u8* RealPage() {
    return reinterpret_cast<u8*>(const_cast<u32*>(hidSharedMem));
}

// The way back from a thread that is not the game's (the plugin's, the keep-alive), under the lock. While the framework's pause holds the
// game (its count goes up before its hold and down before its let-go; its menu runs on the plugin's thread, so the keep-alive thread's
// event - the lid closed with the menu open - is the one that meets it), the hold is the framework's: ours neither adds to it nor lets it go.
// (Read once, so the count is right only for a settled pause: the plugin's own menu gives the page back before its Process::Pause and makes
// no switch while it is open, MirrorBeforePause, and the keep-alive never meets its start or its end with the mirror there.)
Outcome RestoreOffGameThread(bool may_fall_back, u32 where, u32* ticks) {
    azl_mirror_pred_off = CTRPluginFramework::Process::IsPaused() ? 1u : 0u;
    const Outcome out = RestoreLocked(false, may_fall_back, where, ticks);
    azl_mirror_pred_off = 0;
    return out;
}

// What a way back at the game's last event left: `ok` when nothing of the mirror stays.
u32 LastState(Outcome out, u32 ok) {
    if (out == Outcome::kDone) {
        return ok;
    }
    return out == Outcome::kLeftEmpty ? static_cast<u32>(kMirrorLastEmpty) : static_cast<u32>(kMirrorLastStays);
}

u32 TicksToUs(u32 ticks) {
    return static_cast<u32>(static_cast<u64>(ticks) * 1000 / AZL_TICKS_PER_MS);
}

const char* WhereText(u32 where) {
    switch (where) {
    case kWhereNotification:
        return "a notification to the game";
    case chat::kEventHomeEnter:
        return "HOME_ENTER";
    case chat::kEventSwapEnter:
        return "SWAP_ENTER";
    case chat::kEventSleepEnter:
        return "SLEEP_ENTER";
    case chat::kEventExit:
        return "EXIT";
    default:
        return "?";
    }
}

// Once: can this system switch at all? An emulator (Azahar) has neither Luma3DS's hold of the threads nor its mapping calls.
bool Supported() {
    if (g_supported < 0) {
        const char* why = nullptr;
        if (CTRPluginFramework::System::IsCitra()) {
            why = "an emulator (Azahar) has none of Luma3DS's calls that hold the threads and map the page";
        } else if (hidSharedMem == nullptr || hidMemHandle == 0) {
            why = "the framework has no HID page of its own";
        } else if (svcConvertVAToPA(const_cast<u32*>(hidSharedMem), false) == 0) {
            why = "svcConvertVAToPA does not answer";
        }
        g_supported = why ? 0 : 1;
        if (why) {
            Log("chat: no input mirror (%s): the game sees the keys while the keyboard is open", why);
        }
    }
    return g_supported == 1;
}

// The plugin's thread, with the log on, between the trail's "about to hold" and the hold: the trail's thread (priority 0x20, one write and
// one flush) gets kTrailWaitTicks at most to put that line on the card, so that a console that stops inside the way back leaves it
// written. The scheme of Plugin::WaitForCloseTrail, with the same end at the gate's close (an Old 3DS's swap: never a wait there), and
// ended at once by anything that wants the page back now: an event of the framework waiting for the lock, a game's thread that found it
// taken, a closing counted since the frame's look. With the log off there is no trail: no wait at all. NEVER under the mirror's lock
// (2026-11-11, RestoreHere): this thread is the lowest of the process and one of these sleeps can last a second under a busy game.
void GiveTrailTime() {
    const u32 mark = Plugin::TrailMark();
    const u64 deadline = svcGetSystemTick() + kTrailWaitTicks;
    while (!Plugin::TrailOnCard(mark) && !GateClosing() && g_m.event_waiting.load(std::memory_order_seq_cst) == 0 &&
           g_m.want_restore.load(std::memory_order_acquire) == 0 && g_m.closing.load(std::memory_order_seq_cst) == g_look_closing &&
           svcGetSystemTick() < deadline) {
        svcSleepThread(kTrailStepNs);
    }
}

// The plugin's thread: the page back now. False when the lock was taken (by a game's thread giving it back itself: the next frame looks).
// `trail_wait`: the way back is the keyboard's own (closed by the user, or no longer shown), not one a HOME, a sleep or the menu asked for:
// with the log on the trail's "about to hold" is given time to reach the card first (GiveTrailTime).
bool RestoreHere(const char* why, bool trail_wait = false) {
    if (g_m.active.load(std::memory_order_acquire) == 0) {
        g_m.want_restore.store(0, std::memory_order_relaxed);
        return true;
    }
    // The trail first (debug only), BEFORE the hold. Not for a way back tried again every kRetryMs (a mirror that stays): the trail has
    // its limit of lines a launch, and the first try said it.
    const bool trail = !g_retry;
    // The keyboard's own way back, with the log on: the line and the time it is given to reach the card come BEFORE the lock, not under
    // it (2026-11-11). Under it, as on 2026-11-10, the lock was held for the whole wait - up to 100 ms of sleeps of 2 ms on the lowest
    // thread of the process, each of which a game that fills the CPU (RR3D's film) stretches to a second - where 1.1.3 held it about 1 ms
    // for the svcs alone; and for that long a game's thread that got HOME, sleep or power found the lock taken and could not give the
    // page back itself before the game acted on the notification (Sources/ctr/hook.cpp), and the keep-alive waited for it at an event.
    // Without the lock they do it themselves, and the wait ends at once (GiveTrailTime looks at the closing count and at the event).
    // What it costs the trail's reader (the second review, the same day): between this "about to hold" and the hold there is that wait,
    // with the game's threads running and nothing of the way back called yet, so this thread's "about to hold" with no line after it is
    // a console that stopped in the wait OR in the hold. The line says so itself (azl/sleep_crumbs.hpp, kMirrorPluginWaitsText).
    bool said = false;
    if (trail && trail_wait && g_debug_level > 0 && g_m.event_waiting.load(std::memory_order_seq_cst) == 0) {
        BackCrumb(SleepCrumbs::kChatMirrorBackBegins, kWherePlugin, 0);
        GiveTrailTime(); // (NOT under the lock)
        said = true;
        if (g_m.active.load(std::memory_order_acquire) == 0) {
            // Another thread gave the page back meanwhile (its own two lines are in the trail): this one held nothing, and says so.
            BackCrumb(SleepCrumbs::kChatMirrorBackDone, static_cast<u32>(Outcome::kBusy), 0);
            g_m.want_restore.store(0, std::memory_order_relaxed);
            return true;
        }
    }
    if (g_m.event_waiting.load(std::memory_order_seq_cst) != 0 || !g_m.lock.TryLock()) {
        if (said) {
            BackCrumb(SleepCrumbs::kChatMirrorBackDone, static_cast<u32>(Outcome::kBusy), 0); // ("about to hold" was said, and it did not)
        }
        return false; // (an event waits for the lock: it gives the page back itself)
    }
    g_m.want_restore.store(0, std::memory_order_relaxed);
    u32 ticks = 0;
    if (trail && !said) {
        BackCrumb(SleepCrumbs::kChatMirrorBackBegins, kWherePlugin, 0); // (under the lock: only for a way back that is made here)
    }
    const Outcome out = RestoreOffGameThread(true, kWherePlugin, &ticks);
    const u32 us = TicksToUs(ticks);
    g_m.lock.Unlock();
    if (trail) {
        BackCrumb(SleepCrumbs::kChatMirrorBackDone, static_cast<u32>(out), ticks); // (AFTER the let-go: the game's threads run again)
    }
    g_retry = g_m.active.load(std::memory_order_acquire) != 0;
    g_retry_ms = static_cast<u32>(platform::NowMs()) + kRetryMs;
    if (out == Outcome::kDone) {
        Log("chat: the game has its input back (%s; its threads held %lu us)", why, static_cast<unsigned long>(us));
    } else {
        Log("chat: the game's input page (%s): %s", why, hidmirror::OutcomeText(out));
    }
    // The zombie probe (debug only): the game's reference count and thread states when the mirror is given back.
    zombie::GameRefcount("mirror give-back");
    zombie::ThreadSchedDump("mirror give-back");
    return true;
}

// What the keep-alive thread or a game's thread did, said by the plugin's thread.
void LogReport() {
    if (g_m.report_seq.load(std::memory_order_acquire) == g_report_logged || g_m.event_waiting.load(std::memory_order_seq_cst) != 0 ||
        !g_m.lock.TryLock()) {
        return;
    }
    const u32 seq = g_m.report_seq.load(std::memory_order_relaxed), where = g_m.report_where, outcome = g_m.report_outcome, ticks = g_m.report_ticks;
    const bool active = g_m.sw.active();
    g_m.lock.Unlock();
    g_report_logged = seq;
    Log("chat: the game's input page given back at %s: %s (its threads held %lu us)%s", WhereText(where),
        hidmirror::OutcomeText(static_cast<Outcome>(outcome)), static_cast<unsigned long>(TicksToUs(ticks)),
        active ? "; the mirror is still there" : "");
    if (active) {
        g_retry = true;
        g_retry_ms = static_cast<u32>(platform::NowMs()) + kRetryMs;
    }
}

// The debug log's line of a wait for the release that ended at its cap and not by the quiet (debug=1 only; the plugin's thread, never a
// hook). 2026-11-10: on console 63 the 50 ms of quiet had never ended a wait, and the two raw words of the pad (0x1C, 0x20), which
// hidmirror::PageQuiet looked at until then, were the suspects with no dump of the page to prove it. They are printed here with the raw
// touch's flag (0xC4) and the conditions that were not met, at the last look and at any look of the wait: a cap with the keys let go
// names the condition to look into, a cap with a key held all along is the cap doing its job.
void LogCappedWait(const u8* real, u32 last) {
    if (g_debug_level <= 0) {
        return;
    }
    const auto bit = [](u32 bits, u32 one, const char* text) { return (bits & one) != 0 ? text : ""; };
    Log("chat: the wait for the release ended at its cap (%lu ms), the page never quiet for %lu ms | not met at the last look:%s%s%s%s%s | at "
        "any look: 0x%lx (1 key, 2 Circle Pad, 4 touch, 8 raw touch) | raw pad 0x%08lx, raw Circle Pad 0x%08lx, raw touch flag 0x%08lx",
        static_cast<unsigned long>(hidmirror::kMaxWaitMs), static_cast<unsigned long>(hidmirror::kQuietMs),
        bit(last, hidmirror::kUnquietKey, " a key held,"), bit(last, hidmirror::kUnquietCircle, " the Circle Pad off its rest,"),
        bit(last, hidmirror::kUnquietTouch, " a finger in a touch entry,"), bit(last, hidmirror::kUnquietRawTouch, " the raw touch's flag,"),
        last == 0 ? " none (quiet, not yet for long enough)" : "", static_cast<unsigned long>(g_unquiet_seen),
        static_cast<unsigned long>(hidmirror::Load32(real, hidmirror::kPadRaw)),
        static_cast<unsigned long>(hidmirror::Load32(real, hidmirror::kPadRawCircle)),
        static_cast<unsigned long>(hidmirror::Load32(real, hidmirror::kTouchRaw + hidmirror::kTouchFlag)));
}

// The debug log's lines of the masks (debug=1 only; the plugin's thread, never a hook): what the game still does not see, said when the
// masks start, each time one falls and when the last one does, with the time since the wait's cap, and how the mirror was kept meanwhile:
// the frames that wrote it and the longest time between two, which is what a key the user pressed or let go with the masks on waited for
// this thread - the lowest of the process - before the game could see it (hidmirror::HeldMask). 2026-11-13.
__attribute__((noinline)) void LogMask(const char* what_happened, u32 now_ms) {
    if (g_debug_level <= 0) {
        return;
    }
    const u32 what = g_held.what();
    Log("chat: %s | masked for the game now: keys 0x%08lx%s%s%s | %lu ms after the wait's cap, the mirror written %lu times, %lu ms at most "
        "between two",
        what_happened, static_cast<unsigned long>(g_held.keys()), (what & hidmirror::kMaskCircle) != 0 ? ", the Circle Pad (centred)" : "",
        (what & hidmirror::kMaskTouch) != 0 ? ", the touch (absent)" : "", g_held.any() ? "" : " - nothing",
        static_cast<unsigned long>(g_held.since(now_ms)), static_cast<unsigned long>(g_held.frames()),
        static_cast<unsigned long>(g_held.longest_gap_ms()));
}

// A frame of the mirror with the masks on (the keyboard closed, its wait for the release ended at the cap with something still down): what
// was let go leaves the mask, the mirror is the real page without the rest, and once nothing is left the page goes back. With no wait for
// the trail before the hold: while this thread sleeps the mirror is not written, and now it carries the game's own input. A way back that
// cannot be made yet (the lock taken, the real page not mapped) leaves the masks on with nothing in them: the mirror stays the real page's
// copy, never the neutral one again, and the way back is tried as for any mirror that stays.
__attribute__((noinline)) void MaskFrame(const u8* real, u32 now_ms) {
    const u32 keys = g_held.keys(), what = g_held.what();
    const bool left = g_held.Frame(g_page, real, now_ms);
    const bool pad_capped = (what & ~g_held.what() & hidmirror::kMaskCircle) != 0 && g_held.circle_capped();
    if (left) {
        if (g_held.keys() != keys || g_held.what() != what) {
            LogMask(pad_capped ? "the Circle Pad is still off its rest 10 s after the cap: its mask is lifted" : "let go since the last look", now_ms);
        }
        return;
    }
    if (keys != 0 || what != 0) {
        LogMask(pad_capped ? "the Circle Pad is still off its rest 10 s after the cap, its mask is lifted: the game's page goes back"
                           : "the last mask fell: the game's page goes back",
                now_ms);
    }
    if ((!g_retry || static_cast<s32>(now_ms - g_retry_ms) >= 0) && RestoreHere(g_retry ? "once more, after the masks" : "the last mask fell") &&
        g_m.active.load(std::memory_order_acquire) == 0) {
        g_held.Clear();
    }
}

// The switch, once per opening of the keyboard.
void Engage() {
    g_tried = true;
    if (!Supported()) {
        return;
    }
    if (!g_page) {
        g_page = static_cast<u8*>(memalign(hidmirror::kPageBytes, hidmirror::kPageBytes));
        if (!g_page) {
            Log("chat: no input mirror (no memory for its page): the game sees the keys while the keyboard is open");
            return;
        }
    }
    const u8* real = RealPage();
    const u32 own = reinterpret_cast<u32>(real);
    const u32 hid_pa = svcConvertVAToPA(real, false);
    CtrOps ops;
    ops.mirror = reinterpret_cast<u32>(g_page);
    const hidmirror::Found f = hidmirror::FindHidVas([&ops](u32 va, hidmirror::Region* r) { return ops.Query(va, r); },
                                                     [](u32 va) { return svcConvertVAToPA(reinterpret_cast<void*>(va), false); }, hid_pa, own);
    // The framework's own mapping must be among the pages of the physical address (it is how the search knows it looks at the right page),
    // and it is never switched: the plugin reads the real input there.
    Outcome out = Outcome::kDone;
    if (!f.own_seen) {
        Log("chat: ASSERTION: the framework's HID page at 0x%08lx (PA 0x%08lx) is not among the shared pages of its own physical address",
            static_cast<unsigned long>(own), static_cast<unsigned long>(hid_pa));
        out = Outcome::kOwnNotFound;
    } else if (f.too_many) {
        out = Outcome::kTooMany;
    } else if (f.n == 0) {
        out = Outcome::kNotFound;
    }
    if (out != Outcome::kDone) {
        Log("chat: no input mirror (%s): the game sees the keys while the keyboard is open", hidmirror::OutcomeText(out));
        return;
    }
    const u32 keys = hidmirror::HeldNow(real);
    hidmirror::PrepareMirror(g_page, real, keys);
    g_released.Start(keys);
    g_released.Next(); // (the prepared page is the first sample)
    g_m.page.store(reinterpret_cast<u32>(g_page), std::memory_order_seq_cst);
    if (g_m.event_waiting.load(std::memory_order_seq_cst) != 0 || !g_m.lock.TryLock()) {
        g_tried = false; // (an event or a game's thread is giving a page back: the next frame)
        return;
    }
    u32 ticks = 0;
    if (g_m.closing.load(std::memory_order_seq_cst) != g_want_closing || g_m.dead.load(std::memory_order_relaxed) != 0) {
        out = Outcome::kCancelled;
    } else {
        azl_mirror_pred_caller_tls = reinterpret_cast<u32>(getThreadLocalStorage());
        const u64 start = svcGetSystemTick();
        out = g_m.sw.Engage(ops, f);
        ticks = static_cast<u32>(svcGetSystemTick() - start);
        g_m.active.store(g_m.sw.active() ? 1u : 0u, std::memory_order_release);
    }
    g_m.lock.Unlock();
    if (out != Outcome::kCancelled) {
        BackCrumb(SleepCrumbs::kChatMirrorOn, static_cast<u32>(out), ticks); // (the trail, debug only: the switch was tried, and how it went)
    }
    if (out == Outcome::kDone) {
        Log("chat: the game's input is the mirror's now (the game's HID page at 0x%08lx%s, the framework's own at 0x%08lx left alone; the game's "
            "threads held %lu us)",
            static_cast<unsigned long>(f.va[0]), f.n > 1 ? " and another" : "", static_cast<unsigned long>(own),
            static_cast<unsigned long>(TicksToUs(ticks)));
        // The blocks around it, for the fold no svc can undo (a writable shared block the game maps beside the mirror while it is there):
        // how often a neighbour is free, and so could take such a block, is what decides whether a free neighbour must refuse the switch.
        hidmirror::Region below, above;
        if (!ops.Query(f.va[0] - 1, &below)) {
            below = hidmirror::Region();
        }
        if (!ops.Query(f.va[0] + hidmirror::kPageBytes, &above)) {
            above = hidmirror::Region();
        }
        Log("chat: the mirror's neighbours: below 0x%08lx+0x%lx state %lu perm %lu, above 0x%08lx+0x%lx state %lu perm %lu",
            static_cast<unsigned long>(below.base), static_cast<unsigned long>(below.size), static_cast<unsigned long>(below.state),
            static_cast<unsigned long>(below.perm), static_cast<unsigned long>(above.base), static_cast<unsigned long>(above.size),
            static_cast<unsigned long>(above.state), static_cast<unsigned long>(above.perm));
        // The zombie probe (debug only): the reference count and thread states while the mirror holds the game's input.
        zombie::GameRefcount("mirror on");
        zombie::ThreadSchedDump("mirror on");
    } else if (out != Outcome::kCancelled) {
        Log("chat: no input mirror (%s): the game sees the keys while the keyboard is open", hidmirror::OutcomeText(out));
    }
    // A notification or an event that came during the switch: the page back at once (the other thread found the lock taken, or found
    // nothing to give back yet).
    if (g_m.active.load(std::memory_order_acquire) != 0 &&
        (out != Outcome::kDone || g_m.closing.load(std::memory_order_seq_cst) != g_want_closing)) {
        g_want = false;
        if (!RestoreHere("closed during the switch")) {
            g_m.want_restore.store(1, std::memory_order_release);
        }
    }
}

} // namespace

void MirrorBeforeLook() {
    g_look_closing = g_m.closing.load(std::memory_order_seq_cst);
}

void MirrorWant() {
    g_want = true;
    g_tried = false;
    g_want_closing = g_look_closing; // (not the count now: one that came after the look would be taken as seen)
    g_release_wait.Cancel(); // (opened again while the page was still waiting for the release: the mirror stays)
    if (g_held.on()) {
        // Opened again with the masks on: they are dropped, and the mirror is the neutral one again from this frame's refresh - the keys
        // the game was seeing held through it "released" in its entries, as at a switch. (No mirror any more - the page went back by
        // another way and this thread has not looked yet: nothing to release, the switch is made anew.)
        LogMask("the keyboard opened again: the masks are dropped, the mirror is the neutral one again", static_cast<u32>(platform::NowMs()));
        if (g_page && g_m.active.load(std::memory_order_acquire) != 0) {
            g_released.Start(hidmirror::HeldNow(g_page));
        }
        g_held.Clear();
    }
}

void MirrorRelease(u32 now_ms) {
    g_want = false;
    if (g_m.active.load(std::memory_order_acquire) != 0) {
        g_release_wait.Start(now_ms);
        g_unquiet_seen = 0;
    }
}

void MirrorRestoreNow(const char* why, bool trail_wait) {
    g_want = false;
    g_release_wait.Cancel();
    if (!RestoreHere(why, trail_wait)) {
        g_m.want_restore.store(1, std::memory_order_release);
    }
}

bool MirrorActive() {
    return g_m.active.load(std::memory_order_acquire) != 0;
}

bool MirrorInputQuiet() {
    // (no mirror, nothing to give back: quiet, and the page is not read - where the switch was never possible it may not be there at all)
    return g_m.active.load(std::memory_order_acquire) == 0 || hidmirror::PageQuiet(RealPage());
}

bool MirrorBeforePause() {
    const u64 deadline = svcGetSystemTick() + kPauseLockTicks;
    while (!RestoreHere("the in-game menu") && svcGetSystemTick() < deadline) {
        svcSleepThread(kPauseLockStepNs);
    }
    return g_m.active.load(std::memory_order_acquire) == 0;
}

void MirrorFrame(u32 now_ms) {
    LogReport();
    if (g_m.want_restore.load(std::memory_order_acquire) != 0 ||
        (g_want && g_m.closing.load(std::memory_order_seq_cst) != g_want_closing)) {
        g_want = false;
        g_release_wait.Cancel();
        RestoreHere("HOME, power, sleep or exit");
    }
    if (g_want && !g_tried && g_m.active.load(std::memory_order_acquire) == 0) {
        Engage();
    }
    if (g_m.active.load(std::memory_order_acquire) != 0) {
        // The live parts every frame (the page is ours to write: only this thread frees it).
        const u8* real = RealPage();
        if (g_held.on()) {
            MaskFrame(real, now_ms); // (the masks on: the real input without what is still down from the close, never the neutral page)
            return;
        }
        hidmirror::RefreshMirror(g_page, real, g_released.Next());
        if (!g_want) {
            if (g_release_wait.waiting()) {
                const u32 unquiet = hidmirror::Unquiet(real); // (0: hidmirror::PageQuiet)
                g_unquiet_seen |= unquiet;
                if (g_release_wait.Update(unquiet == 0, now_ms)) {
                    if (g_release_wait.capped()) {
                        LogCappedWait(real, unquiet);
                    }
                    if (unquiet != 0 && g_held.Start(g_page, real, now_ms)) {
                        // Something is still down at the cap: the page stays the mirror's, from now the real input without it (MaskFrame).
                        LogMask("still down at the wait's cap: the page stays the mirror's, the real input without it, until it is let go", now_ms);
                        return;
                    }
                    RestoreHere("the keyboard closed", true);
                }
            } else if (!g_retry || static_cast<s32>(now_ms - g_retry_ms) >= 0) {
                RestoreHere(g_retry ? "once more" : "the keyboard closed");
            }
        }
    } else if (!g_want && g_page && g_m.event_waiting.load(std::memory_order_seq_cst) == 0 && g_m.lock.TryLock()) {
        // Nothing maps the page any more: back to the heap, under the lock (no other thread is giving it back now).
        if (!g_m.sw.active()) {
            g_m.page.store(0, std::memory_order_seq_cst);
            std::free(g_page);
            g_page = nullptr;
            g_retry = false;
        }
        g_m.lock.Unlock();
        if (g_held.on()) {
            // The page went back by another way with the masks on (HOME, sleep, power, the in-game menu, the exit): they went with it.
            LogMask("the page went back by another way (HOME, sleep, the menu, the exit): the masks went with it", now_ms);
            g_held.Clear();
        }
    }
}

void MirrorProcessEvent(int event) {
    if (!chat::IsClosingEvent(event)) {
        return;
    }
    g_m.closing.fetch_add(1, std::memory_order_seq_cst);
    if (event == chat::kEventExit) {
        g_m.dead.store(1, std::memory_order_relaxed);
    }
    // The game's last event: EXIT, or SWAP_ENTER (an Old 3DS: a game closed from the HOME menu gets its EXIT once Luma3DS has mapped the
    // plugin's block back, plgloader.c; SWAP_ENTER is the net for an end with no EXIT event: a process killed, a block that does not come back).
    const bool last = event == chat::kEventExit || event == chat::kEventSwapEnter;
    if (g_m.page.load(std::memory_order_seq_cst) == 0) {
        if (last) {
            g_last.store(kMirrorLastNone, std::memory_order_release);
        }
        return; // (no mirror, and none will be made: the switch sees the closing under the lock)
    }
    // (the keep-alive thread, 4 KB of stack, Luma3DS waiting for the answer: no line, no format; the plugin's thread does not take the lock
    // while this waits, so a holder that still runs is done in about 1 ms - kEventLockTicks)
    g_m.event_waiting.fetch_add(1, std::memory_order_seq_cst);
    const u64 left = SwapTicksLeft(); // (SWAP_ENTER's one deadline, ctr/swap_gate.hpp; none at EXIT: the full second)
    const u64 deadline = svcGetSystemTick() + (left < kEventLockTicks ? left : kEventLockTicks);
    bool locked = g_m.lock.TryLock();
    while (!locked && svcGetSystemTick() < deadline) {
        svcSleepThread(kEventLockStepNs);
        locked = g_m.lock.TryLock();
    }
    if (!locked && SwapTicksLeft() == 0) { // (no live copy of `left` across the loop: 8 bytes of the keep-alive's frame)
        NoteSwapWaitCut(1); // (the deadline ended the wait for the lock)
    }
    u32 state = kMirrorLastNone;
    if (locked) {
        if (g_m.sw.active()) {
            // At the swap and at the exit the mirror never stays: its page goes with the plugin's block, and an alias left mapped outlives
            // the game (the next start of the game finds the plugin's memory a page short until a reboot).
            u32 ticks = 0;
            BackCrumb(SleepCrumbs::kChatMirrorBackBegins, static_cast<u32>(event), 0); // (the trail, debug only: stores and a signal, no wait)
            state = LastState(RestoreOffGameThread(!last, static_cast<u32>(event), &ticks), kMirrorLastGivenBack);
            BackCrumb(SleepCrumbs::kChatMirrorBackDone, g_m.report_outcome, ticks);
        }
        g_m.lock.Unlock();
    } else {
        // The lock not had in 1 s: its holder no longer runs (a game's thread ended inside its way back, or held by the framework's pause).
        // Nothing is changed without the lock - the holder may be between the unmap and the map of the game's HID page - so the way back is
        // left to the plugin's thread. At HOME and sleep it comes; after the game's last event it does not, and the mirror stays mapped:
        // an alias of the plugin's memory outlives the game (console 63, 2026-11-06), said in the shutdown's line when an EXIT comes (an end
        // with no EXIT event, a process killed: no line).
        g_m.want_restore.store(1, std::memory_order_release);
        state = kMirrorLastNoLock;
    }
    if (last) {
        g_last.store(state, std::memory_order_release);
    }
    g_m.event_waiting.fetch_sub(1, std::memory_order_seq_cst);
}

u32 MirrorLastState() {
    return g_last.load(std::memory_order_acquire);
}

const char* MirrorLastText(u32 state) {
    switch (state) {
    case kMirrorLastNone:
        return "no mirror";
    case kMirrorLastGivenBack:
        return "given back";
    case kMirrorLastStays:
        return "STILL MAPPED (folded with a block of the game, or not unmapped): a page of the plugin's memory outlives the game";
    case kMirrorLastNoLock:
        return "STILL MAPPED, the lock not had in 1 s (nothing touched): a page of the plugin's memory outlives the game";
    case kMirrorLastEmpty:
        return "taken off, the game's address left EMPTY";
    default:
        return "?";
    }
}

} // namespace ctr
} // namespace azl
