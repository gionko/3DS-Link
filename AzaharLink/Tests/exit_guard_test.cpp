// The exit guard (azl/exit_guard.hpp; REGISTRO-MODIFICHE-ERRORI.md, phase Q), run by Tests/run.sh: the plugin's own exitguard::Guard against a
// model of Luma3DS (aef3130) that transcribes what decides whether Rosalina frees the plugin's block, with time:
//   * k11 ExitProcess.c:7-11, globals.c:144-174 - svcExitProcess raises EXIT (the bit OR-ed in, one notification 0x1001 queued) only from
//     state 1 (the state masked: 0x00010001 counts as 1);
//   * plgloader.c:513-516 - Rosalina writes the state as a WHOLE word (the event bits go with it);
//   * plgloader.c:557-642 - HandleKernelEvent, one notification at a time: state 3 = nothing; events == EXIT (equality, not a mask) = the EXIT
//     branch; events == HOME = the HOME branch, which picks HOME_ENTER or HOME_EXIT by ctx->pluginIsHome (not by the state), tells the plugin
//     (plgEvent = 5 / 6), waits for its reply (plgReply = PLG_WAIT until the plugin answers; after the timeout it STAYS PLG_WAIT), then stores
//     INHOME / RUNNING whole; anything else = nothing;
//   * a notification can be queued while Rosalina is busy (sm notifications.c: queued, not merged).
// Each case: the outcome without the guard and with it, written before (what the case must give). Time is the model's, in ms.
#include <cstdio>

#include "azl/exit_guard.hpp"

#pragma GCC diagnostic ignored "-Wunused-function" // (support.hpp has helpers this test does not use)
#include "support.hpp"

using namespace azl;
using namespace azl::exitguard;

namespace {

constexpr u32 kNever = 0xFFFFFFFFu;
constexpr u32 kTimeoutMs = 10000;

struct Luma {
    u32 word = kStatusRunning;
    s32 reply = 0;
    s32 event = 0;
    bool plugin_is_home = false;
    u32 now = 0;
    u32 queued = 0;            // notifications not handled yet
    u32 handle_at = 0;         // Rosalina takes the next queued notification at this time (when idle)
    bool busy = false;         // in a HOME branch, waiting for the plugin's reply
    u32 busy_end = 0;          // the reply comes (or the timeout) at this time
    bool reply_comes = true;   // the plugin replies at busy_end; else the timeout: the reply word stays PLG_WAIT
    u32 plugin_reply_ms = 20;  // how long the plugin takes for a HOME event it gets while the model runs
    bool exit_branch = false;
    bool plugin_stuck = false; // the framework never answers (the Smash log stopped)

    void FinishHome(bool replied) {
        busy = false;
        if (replied) {
            reply = 0;
        }
        word = plugin_is_home ? kStatusRunning : kStatusInHome; // (HOME_EXIT stores RUNNING, HOME_ENTER INHOME: plgloader.c:626/636)
        plugin_is_home = !plugin_is_home;
    }
    void Handle() {
        --queued;
        const u32 status = word & kStatusMask;
        const u32 events = word & ~kStatusMask;
        if (status == kStatusExiting) {
            return;
        }
        if (events == kEventExit) {
            word = kStatusExiting;
            exit_branch = true; // (ABOUT_TO_EXIT, the reply, then WaitForProcessTerminated frees the block)
        } else if (events == kEventHome) {
            busy = true;
            event = plugin_is_home ? kPlgHomeExit : kPlgHomeEnter;
            reply = kReplyWait;
            reply_comes = !plugin_stuck;
            busy_end = now + (plugin_stuck ? kTimeoutMs : plugin_reply_ms);
        }
    }
    void Advance(u32 ms) {
        const u32 end = now + ms;
        while (true) {
            if (busy && busy_end <= end) {
                now = busy_end;
                FinishHome(reply_comes);
                continue;
            }
            if (!busy && queued > 0 && handle_at <= end) {
                now = handle_at > now ? handle_at : now;
                Handle();
                continue;
            }
            break;
        }
        now = end;
    }
    void ExitProcess() {
        if ((word & kStatusMask) == kStatusRunning) {
            word |= kEventExit;
            ++queued;
            if (handle_at < now) {
                handle_at = now;
            }
        }
    }
    void ReplyForPlugin() {
        if (busy) {
            FinishHome(true);
        } else {
            reply = 0; // (a reply nobody waits for: the word is written, the arbiter signalled for nobody)
        }
    }
};

struct Outcome {
    bool exit_branch;
    Seen seen;
};

Outcome Play(Luma l, bool guard) {
    Outcome o{false, Seen{}};
    if (guard) {
        o.seen = Guard([&l]() { return l.word; }, [&l]() { return l.reply; }, [&l]() { return l.event; }, [&l]() { l.Advance(kWaitStepMs); },
                       [&l](s32) { l.ReplyForPlugin(); }, [&l](const Seen&) { l.Advance(kTrailWaitMs / 3); },
                       [&l]() { l.word = kStatusRunning; });
    }
    l.ExitProcess();
    l.Advance(30000); // (Rosalina goes on: every wait and notification)
    o.exit_branch = l.exit_branch;
    return o;
}

void Expect(const char* name, const Luma& l, bool without, bool with, Action action) {
    const Outcome no = Play(l, false);
    const Outcome yes = Play(l, true);
    std::printf("  %-58s without %-3s with %-3s (%s, waited %u ms%s)\n", name, no.exit_branch ? "EXIT" : "-", yes.exit_branch ? "EXIT" : "-",
                ActionName(yes.seen.action), static_cast<unsigned>(yes.seen.waited_ms), yes.seen.replied_for_plugin ? ", replied" : "");
    CHECK_EQ(no.exit_branch, without);
    CHECK_EQ(yes.exit_branch, with);
    CHECK(yes.seen.action == action);
}

void TestCases() {
    std::printf("the model of Luma3DS: the outcome without the guard (the consoles) and with it\n");
    {
        Luma l;
        Expect("normal: RUNNING, nothing pending", l, true, true, Action::Leave);
    }
    {
        Luma l;
        l.word = kStatusInHome;
        l.plugin_is_home = true;
        Expect("A: INHOME, Rosalina idle, no HOME_EXIT raised", l, false, true, Action::WriteRunning);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventHome; // (its notification already taken: the bit is stale)
        Expect("B: a stale HOME bit on RUNNING", l, false, true, Action::WriteRunning);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventHome;
        l.queued = 1;
        l.handle_at = 50; // (Rosalina takes it in 50 ms, the plugin answers HOME_ENTER in 20)
        Expect("B': HOME raised and queued at the exit", l, false, true, Action::WriteRunning);
    }
    {
        Luma l;
        l.word = kStatusNone;
        Expect("SSF4: NONE for the whole session", l, false, true, Action::WriteRunning);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventHome;
        l.busy = true;
        l.event = kPlgHomeEnter;
        l.reply = kReplyWait;
        l.busy_end = 300;
        Expect("S3: the exit while the plugin answers HOME_ENTER (300 ms)", l, false, true, Action::WriteRunning);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventHome;
        l.busy = true;
        l.event = kPlgHomeEnter;
        l.reply = kReplyWait;
        l.busy_end = kTimeoutMs;
        l.reply_comes = false;
        const Outcome yes = Play(l, true);
        Expect("D: the framework never answers HOME_ENTER (Smash's log)", l, false, true, Action::WriteRunning);
        CHECK(yes.seen.replied_for_plugin);
        CHECK(yes.seen.waited_ms >= kWaitBudgetMs && yes.seen.waited_ms <= kWaitBudgetMs + kAfterReplyMs);
    }
    {
        Luma l;
        l.word = kStatusInHome | kEventHome;
        l.plugin_is_home = true;
        l.busy = true;
        l.event = kPlgHomeExit;
        l.reply = kReplyWait;
        l.busy_end = kTimeoutMs;
        l.reply_comes = false;
        Expect("D': the framework never answers HOME_EXIT", l, false, true, Action::Leave);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventHome;
        l.busy = true;
        l.event = 3; // (ABOUT_TO_SWAP: never answered by the guard; an Old 3DS case, here only to hold the rule)
        l.reply = kReplyWait;
        l.busy_end = kTimeoutMs;
        l.reply_comes = false;
        const Outcome yes = Play(l, true);
        CHECK(!yes.seen.replied_for_plugin);
        CHECK(!yes.exit_branch); // (Rosalina's store at 10 s wipes the EXIT: written so that nobody believes it is covered)
    }
    {
        Luma l;
        l.reply = kReplyWait; // (left by an old timeout; Rosalina idle)
        l.event = kPlgHomeEnter;
        Expect("a PLG_WAIT left by an old timeout, RUNNING", l, true, true, Action::Leave);
    }
    {
        Luma l;
        l.word = kStatusRunning | kEventExit; // (an exit raised from a site we did not hook, not handled yet)
        l.queued = 1;
        l.handle_at = 100;
        Expect("an EXIT already raised and queued", l, true, true, Action::Leave);
    }
    {
        Luma l;
        l.word = kStatusExiting;
        const Outcome yes = Play(l, true);
        CHECK(yes.seen.action == Action::Leave && yes.seen.waited_ms == 0);
    }
    {
        Luma l;
        l.word = 0x00000007;
        const Outcome yes = Play(l, true);
        CHECK(yes.seen.action == Action::Leave);
    }
}

void TestDecideEdges() {
    std::printf("decide: the edges the cases do not reach\n");
    CHECK(Decide(0x00020001, 0, 0) == Action::Leave);              // EXIT raised, no HOME: never touched
    CHECK(Decide(0x00030001, 0, kWaitBudgetMs) == Action::WriteRunning); // HOME and EXIT both: Rosalina takes neither, the guard cleans it
    CHECK(Decide(0x00010001, 0, 0) == Action::Wait);               // an event bit: wait (it may be handled)
    CHECK(Decide(0x00000002, kReplyWait, kWaitBudgetMs) == Action::WriteRunning);
    CHECK(Decide(0x00020007, 0, 0) == Action::Leave);
}

} // namespace

int main() {
    TestCases();
    TestDecideEdges();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
