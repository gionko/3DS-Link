// The exit guard (New 3DS; REGISTRO-MODIFICHE-ERRORI.md, phase Q, 2026-11-08): what to do with Luma3DS's plugin word just before the game's
// svcExitProcess, so that Rosalina gets its EXIT and frees the plugin's block (Sources/ctr/exit_tramp.s says why). Pure: the host tests run the
// same Guard() against a model of Luma3DS (Tests/exit_guard_test.cpp).
//
// Luma3DS (aef3130; the consoles run 13.4.0): the word's low 16 bits are the state, written whole by Rosalina only (PLG__SetConfigMemoryStatus,
// plgloader.c:515); the high bits are events, OR-ed in by the kernel (PLG_SignalEvent, globals.c:149). ExitProcessHook raises EXIT only from
// state 1 (ExitProcess.c:10); HandleKernelEvent takes it only when the event bits are exactly EXIT (plgloader.c:569). While Rosalina answers an
// event it waits for the plugin's reply (PLG__WaitForReply: plgReply = PLG_WAIT, up to waitForReplyTimeout, 10 s on a New 3DS), and ends that
// with a whole-word store of the state, which wipes an EXIT raised in the meantime. So, at the exit:
//   * state 3 (EXITING), or an EXIT bit without a HOME bit (an exit already raised, e.g. from a site we did not hook): never touched;
//   * while Rosalina's reply word is PLG_WAIT or an event bit is up (Rosalina has an event to answer or is answering it): wait, in steps, up to
//     kWaitBudgetMs (the system gives a closing game about 3 s);
//   * the budget over with Rosalina still waiting for the plugin's reply to HOME_ENTER / HOME_EXIT (the framework stuck: Smash's log stopped
//     1.5 s after HOME): reply on the framework's behalf (PLGLDR__Reply; never for ABOUT_TO_SWAP / ABOUT_TO_EXIT), and wait up to
//     kAfterReplyMs more for Rosalina's store;
//   * the trail (what was seen, what is done) goes to the card on the plugin's breadcrumb thread, waited for kTrailWaitMs at most: no file is
//     opened on the exiting thread, and nothing slow sits between the last look at the word and the svc;
//   * then one last look: state 1 with no event = nothing to do; state 0 (NONE: Rosalina lost the game), 2 (INHOME with no HOME_EXIT) or 1/2
//     with a stale event bit = write 1 (RUNNING), so that the kernel raises EXIT on it at once; any other value = never written over.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace exitguard {

constexpr u32 kStatusMask = 0xFFFF;
constexpr u32 kStatusNone = 0;
constexpr u32 kStatusRunning = 1;
constexpr u32 kStatusInHome = 2;
constexpr u32 kStatusExiting = 3;
constexpr u32 kEventHome = 0x10000;
constexpr u32 kEventExit = 0x20000;
constexpr s32 kReplyWait = -1;   ///< PLG_WAIT (Luma3DS plgldr.h)
constexpr s32 kPlgHomeEnter = 5; ///< PLG_HOME_ENTER: the events the guard may answer for the framework
constexpr s32 kPlgHomeExit = 6;

constexpr u32 kWaitBudgetMs = 1000;
constexpr u32 kAfterReplyMs = 300;
constexpr u32 kTrailWaitMs = 300;
constexpr u32 kWaitStepMs = 5;

enum class Action : u8 { Leave, Wait, WriteRunning };

/// One look. `word`: Luma's plugin word through its uncached alias; `reply`: Rosalina's reply word (the header's plgldrReply); `waited_ms`:
/// how long the guard has waited; past `budget_ms` it no longer waits.
inline Action Decide(u32 word, s32 reply, u32 waited_ms, u32 budget_ms = kWaitBudgetMs) {
    const u32 status = word & kStatusMask;
    const u32 events = word & ~kStatusMask;
    if (status == kStatusExiting || ((events & kEventExit) != 0 && (events & kEventHome) == 0)) {
        return Action::Leave; // (Rosalina has the exit, or the kernel has raised it already)
    }
    if ((reply == kReplyWait || events != 0) && waited_ms < budget_ms) {
        return Action::Wait;
    }
    if (status > kStatusExiting) {
        return Action::Leave; // (not a state Luma3DS writes: never written over)
    }
    if (status == kStatusRunning && events == 0) {
        return Action::Leave;
    }
    return Action::WriteRunning;
}

inline const char* ActionName(Action a) {
    return a == Action::Leave ? "nothing to do" : a == Action::Wait ? "wait" : "word set to running";
}

/// What the guard saw and did (the trail's lines; the host test).
struct Seen {
    u32 word_first = 0;
    s32 reply_first = 0;
    s32 event_first = 0;
    u32 waited_ms = 0;
    bool replied_for_plugin = false;
    u32 word_last = 0;
    Action action = Action::Leave;
};

/// The guard (the plugin's Plugin::RunExitGuard, and the host test against its model of Luma3DS). The callbacks: `word()` / `reply()` /
/// `event()` read; `sleep_step()` sleeps kWaitStepMs; `reply_for_plugin(event)` is PLGLDR__Reply; `trail(seen)` hands what was seen to the
/// breadcrumb thread and waits for it (kTrailWaitMs at most); `write_running()` stores kStatusRunning.
template <class Word, class Reply, class Event, class Sleep, class ReplyFor, class Trail, class WriteRunning>
inline Seen Guard(Word word, Reply reply, Event event, Sleep sleep_step, ReplyFor reply_for_plugin, Trail trail, WriteRunning write_running) {
    Seen s;
    s.word_first = word();
    s.reply_first = reply();
    s.event_first = event();
    u32 waited = 0;
    while (Decide(word(), reply(), waited) == Action::Wait) {
        sleep_step();
        waited += kWaitStepMs;
    }
    const s32 ev = event();
    if (reply() == kReplyWait && (ev == kPlgHomeEnter || ev == kPlgHomeExit)) {
        reply_for_plugin(ev);
        s.replied_for_plugin = true;
        for (u32 after = 0; after < kAfterReplyMs; after += kWaitStepMs) {
            const u32 w = word();
            if (reply() != kReplyWait && (w & ~kStatusMask) == 0) {
                break; // (Rosalina has stored its state)
            }
            sleep_step();
            waited += kWaitStepMs;
        }
    }
    s.waited_ms = waited;
    s.word_last = word();
    s.action = Decide(s.word_last, reply(), kWaitBudgetMs, kWaitBudgetMs);
    trail(s);
    // The last look, right before the svc (the trail may have taken a while): the same rule, no more waiting.
    const u32 w = word();
    if (Decide(w, reply(), kWaitBudgetMs, kWaitBudgetMs) == Action::WriteRunning) {
        write_running();
    }
    return s;
}

} // namespace exitguard
} // namespace azl
