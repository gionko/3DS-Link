// The trail of a sleep (azl/sleep_crumbs.hpp), run by Tests/run.sh: what is noted by whoever sees an event (the game's thread
// inside its APT call, the framework's keep-alive thread) comes out as the lines of AzaharLink.sleep.log in order, the wake
// marker is found from the console's clock running ahead of the tick, the "alive" markers follow, the ring survives
// producers that outrun the thread, and two producers and the thread together lose and duplicate nothing.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "azl/sleep_crumbs.hpp"

#pragma GCC diagnostic ignored "-Wunused-function" // (support.hpp has helpers this test does not use)
#include "support.hpp"

using namespace azl;

namespace {

constexpr u64 kClockBase = 3'900'000'000'000ull; // osGetTime() when the tick reads 0 (a date in the 2020s)

u64 Os(u32 tick_ms, u64 slept_ms = 0) {
    return kClockBase + tick_ms + slept_ms;
}

bool Has(const std::string& line, const char* needle) {
    return line.find(needle) != std::string::npos;
}

std::string Joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& l : lines) {
        all += l + "\n";
    }
    return all;
}

} // namespace

static void TestEventsBecomeLines() {
    std::printf("sleep crumbs: the events of the sleep protocol become lines, in order, with the tick and the clock\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines); // the thread's first turn: the reference of the clock
    CHECK(lines.empty());
    CHECK(!crumbs.watching());
    crumbs.Push(kAptNoteNotification, 3, 5000);        // the system asks
    crumbs.Push(kAptNoteReplyQuery, 1, 5002);          // the game accepts
    crumbs.Push(kAptNoteNotification, 5, 5004);        // the sleep begins
    crumbs.Push(kAptNoteReplyComplete, 0, 5030);       // the game acknowledges
    crumbs.Push(SleepCrumbs::kFwEnter, 0, 5031);       // the framework's event
    crumbs.Poll(5100, Os(5100), &lines);
    CHECK_EQ(lines.size(), std::size_t(5));
    if (lines.size() == 5) {
        CHECK(Has(lines[0], "sleep: APT notification 3 (sleep query) | tick 5000 ms | written at console clock "));
        CHECK(Has(lines[1], "sleep: the game answers the sleep query: accept | tick 5002 ms"));
        CHECK(Has(lines[2], "sleep: APT notification 5 (sleep begins) | tick 5004 ms"));
        CHECK(Has(lines[3], "ReplySleepNotificationComplete: this line is written before the call) | tick 5030 ms"));
        CHECK(Has(lines[4], "sleep: framework event SLEEP_ENTER | tick 5031 ms"));
        char wall[24];
        FormatConsoleClock(Os(5100), wall);
        CHECK(Has(lines[0], (std::string("written at console clock ") + wall).c_str()));
    }
    CHECK(crumbs.watching()); // a sleep began: the clock is watched
    // the other answers and notifications
    lines.clear();
    crumbs.Push(kAptNoteReplyQuery, 0, 5200);
    crumbs.Push(kAptNoteReplyQuery, 2, 5201);
    crumbs.Push(kAptNoteNotification, 1, 5202);
    crumbs.Push(kAptNoteNotification, 6, 5203);
    crumbs.Push(kAptNoteNotification, 99, 5204);
    crumbs.Push(SleepCrumbs::kFwExit, 0, 5205);
    crumbs.Poll(5300, Os(5300), &lines);
    const std::string all = Joined(lines);
    CHECK(Has(all, "answers the sleep query: reject"));
    CHECK(Has(all, "answers the sleep query: later"));
    CHECK(Has(all, "APT notification 1 (home button)"));
    CHECK(Has(all, "APT notification 6 (wake-up)"));
    CHECK(Has(all, "APT notification 99 (?)"));
    CHECK(Has(all, "framework event SLEEP_EXIT"));
    // an event of a kind nobody knows is skipped, and nothing is repeated
    lines.clear();
    crumbs.Push(77, 0, 5400);
    crumbs.Poll(5400, Os(5400), &lines);
    CHECK(lines.empty());
}

static void TestWakeMarker() {
    std::printf("sleep crumbs: the wake-up is found from the console's clock running ahead of the tick, then it says it is alive\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(10000, Os(10000), &lines);
    crumbs.Push(kAptNoteNotification, 5, 10010);
    crumbs.Poll(10020, Os(10020), &lines);
    CHECK_EQ(lines.size(), std::size_t(1));
    CHECK(crumbs.watching());
    // the thread's turns while the console is awake: nothing
    for (u32 t = 10270; t < 10400; t += 250) {
        crumbs.Poll(t, Os(t), &lines);
    }
    CHECK_EQ(lines.size(), std::size_t(1));
    // asleep for 3 s: the tick stands still, the clock does not
    lines.clear();
    crumbs.Poll(10450, Os(10450, 3000), &lines);
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "sleep: WOKE UP: the console's clock ran 3000 ms ahead of the tick"));
    }
    lines.clear();
    crumbs.Poll(10700, Os(10700, 3000), &lines);
    CHECK(lines.empty());
    crumbs.Poll(11500, Os(11500, 3000), &lines);       // 1.05 s after the wake
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "sleep: alive 1 s after the wake-up"));
    }
    CHECK(crumbs.watching());
    lines.clear();
    crumbs.Poll(15500, Os(15500, 3000), &lines);       // 5.05 s after
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "sleep: alive 5 s after the wake-up"));
    }
    CHECK(!crumbs.watching());                         // done: the thread waits for an event again
    lines.clear();
    crumbs.Poll(16000, Os(16000, 3000), &lines);
    CHECK(lines.empty());

    // a second sleep is measured from the first wake: the reference moved on
    crumbs.Push(SleepCrumbs::kFwEnter, 0, 20000);
    crumbs.Poll(20010, Os(20010, 3000), &lines);
    CHECK(crumbs.watching());
    lines.clear();
    crumbs.Poll(20100, Os(20100, 3000 + 12000), &lines);
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "WOKE UP: the console's clock ran 12000 ms ahead"));
    }

    // a thread that was starved through the whole sleep (the events are processed after the wake): the jump is found at once
    SleepCrumbs late;
    lines.clear();
    late.Poll(100, Os(100), &lines);
    late.Push(kAptNoteNotification, 5, 200);
    late.Push(kAptNoteReplyComplete, 0, 230);
    late.Poll(300, Os(300, 8000), &lines);
    const std::string all = Joined(lines);
    CHECK(Has(all, "APT notification 5 (sleep begins) | tick 200 ms"));
    CHECK(Has(all, "WOKE UP: the console's clock ran 8000 ms ahead"));
    CHECK(all.find("APT notification 5") < all.find("WOKE UP")); // (in the order they happened)
}

static void TestNoWakeInAMinute() {
    std::printf("sleep crumbs: a sleep notification that is not followed by a sleep is dropped after 60 s awake\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines);
    crumbs.Push(kAptNoteNotification, 5, 1010);
    crumbs.Poll(1020, Os(1020), &lines);
    lines.clear();
    for (u32 t = 1270; t < 61000; t += 250) {
        crumbs.Poll(t, Os(t), &lines);
    }
    CHECK(lines.empty());
    CHECK(crumbs.watching());
    crumbs.Poll(61100, Os(61100), &lines);
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "no wake-up seen in 60 s of running: the sleep did not happen"));
    }
    CHECK(!crumbs.watching());
}

static void TestRingOverflowAndLimit() {
    std::printf("sleep crumbs: producers that outrun the thread lose the oldest events and say so; one launch writes 400 lines at the most\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines);
    for (u32 i = 0; i < 100; ++i) {
        crumbs.Push(kAptNoteNotification, 1, 2000 + i); // 100 events, 32 slots
    }
    crumbs.Poll(3000, Os(3000), &lines);
    CHECK_EQ(lines.size(), std::size_t(1 + SleepCrumbs::kSlots));
    if (!lines.empty()) {
        CHECK(Has(lines[0], "events were lost"));
        CHECK(Has(lines.back(), "tick 2099 ms")); // the newest are the ones kept
        CHECK(Has(lines[1], "tick 2068 ms"));
    }

    // the limit of one launch
    SleepCrumbs many;
    std::vector<std::string> all;
    many.Poll(1000, Os(1000), &all);
    for (u32 round = 0; round < 20; ++round) {
        for (u32 i = 0; i < 30; ++i) {
            many.Push(kAptNoteNotification, 1, 5000 + round * 100 + i);
        }
        many.Poll(5000 + round * 100 + 50, Os(5000 + round * 100 + 50), &all);
    }
    CHECK_EQ(all.size(), std::size_t(SleepCrumbs::kMaxLines));
    CHECK(Has(all.back(), "the limit of lines for one launch is reached"));
    CHECK_EQ(many.lines(), SleepCrumbs::kMaxLines);
}

static void TestTwoProducersAndTheThread() {
    std::printf("sleep crumbs: two producers and the thread together lose and repeat nothing\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    std::atomic<bool> producers_done{false};
    std::atomic<u32> pushed{0}, consumed{0};
    std::thread consumer([&] {
        std::vector<std::string> mine;
        while (!producers_done.load()) {
            crumbs.Poll(1000, Os(1000), &mine);
            consumed = static_cast<u32>(mine.size());
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        crumbs.Poll(1000, Os(1000), &mine);
        lines = std::move(mine);
    });
    // (the producers stay within the ring: a loaded machine that deschedules the thread for a while must not lose events here;
    // the overflow has its own test)
    auto produce = [&](u32 first) {
        for (u32 i = 0; i < 150; ++i) {
            while (pushed.load() - consumed.load() >= 20) {
                std::this_thread::yield();
            }
            ++pushed;
            crumbs.Push(kAptNoteNotification, 1, first + i);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    };
    std::thread a(produce, 10000), b(produce, 20000);
    a.join();
    b.join();
    producers_done = true;
    consumer.join();
    CHECK_EQ(lines.size(), std::size_t(300));
    std::vector<int> seen(300, 0);
    for (const std::string& l : lines) {
        unsigned tick = 0;
        const std::size_t at = l.find("| tick ");
        if (at != std::string::npos && std::sscanf(l.c_str() + at, "| tick %u ms", &tick) == 1) {
            const unsigned slot = tick >= 20000 ? 150 + tick - 20000 : tick - 10000;
            if (slot < 300) {
                ++seen[slot];
            }
        }
    }
    int missing = 0, repeated = 0;
    for (int n : seen) {
        missing += n == 0;
        repeated += n > 1;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(repeated, 0);
}

// What an Old 3DS needs from the trail: the word as it came, the swap with the memory, the APT:ReceiveParameter decision, a failed call.
static void TestOld3dsLines() {
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines);
    lines.clear();
    crumbs.Push(kAptNoteNotification, 0x608, 2000);
    crumbs.Push(kAptNoteNotification, 1, 2001);
    crumbs.Push(kAptNoteNotification, 0x1BC00005, 2002); // a sleep with bits above the signal still arms the watch
    crumbs.Poll(2100, Os(2100), &lines);
    CHECK_EQ(lines.size(), std::size_t(3));
    if (lines.size() == 3) {
        CHECK(Has(lines[0], "sleep: APT notification 8 (power button), reply word 0x00000608 | tick 2000 ms"));
        CHECK(Has(lines[1], "sleep: APT notification 1 (home button) | tick 2001 ms"));
        CHECK(Has(lines[2], "sleep: APT notification 5 (sleep begins), reply word 0x1bc00005 | tick 2002 ms"));
    }
    CHECK(crumbs.watching());
    lines.clear();
    CHECK_EQ(crumbs.pushed(), crumbs.taken());
    crumbs.Push(SleepCrumbs::kFwSwapEnter, 3, 3000);
    crumbs.Push(SleepCrumbs::kMemSystem, SleepCrumbs::PackRegion(40u << 20, 44u << 20), 3000);
    crumbs.Push(SleepCrumbs::kMemApplication, SleepCrumbs::PackRegion(64u << 20, 64u << 20), 3000);
    crumbs.Push(SleepCrumbs::kMemBase, SleepCrumbs::PackRegion(21u << 20, 20u << 20), 3000); // (used over total: never a huge number)
    CHECK(crumbs.pushed() != crumbs.taken());
    crumbs.Push(SleepCrumbs::kFwSwapExit, 3, 4000);
    crumbs.Push(8, 0x070D37C8, 4001); // (8 to 10: who made the call that raised the swap, up to 1.1.2; free since the gate: nothing is written for them)
    crumbs.Push(9, 0x00123450, 4002);
    crumbs.Push(10, 0x070D37C8, 4003);
    crumbs.Poll(4100, Os(4100), &lines);
    CHECK_EQ(crumbs.pushed(), crumbs.taken());
    CHECK_EQ(lines.size(), std::size_t(5));
    if (lines.size() == 5) {
        CHECK(Has(lines[0], "framework event SWAP_ENTER #3:"));
        CHECK(Has(lines[1], "memory region SYSTEM: 40960 of 45056 KB used, 4096 KB free"));
        CHECK(Has(lines[2], "memory region APPLICATION: 65536 of 65536 KB used, 0 KB free"));
        CHECK(Has(lines[3], "memory region BASE: 21504 of 20480 KB used, 0 KB free"));
        CHECK(Has(lines[4], "framework event SWAP_EXIT #3:"));
    }
    lines.clear();
    crumbs.Push(SleepCrumbs::kFwSwapBack, 2017, 4499);
    crumbs.Push(kAptNoteLimit, 0, 4499);
    crumbs.Push(kAptNoteReceiveReply, 0x01010003, 4500);
    crumbs.Push(kAptNoteReceiveReply, kAptReceiveFailed, 4501);
    crumbs.Push(kAptNoteUtility, 0x12340007, 4502);
    crumbs.Push(kAptNoteUtilityResult, 0, 4502);
    crumbs.Poll(4600, Os(4600), &lines);
    CHECK_EQ(lines.size(), std::size_t(5));
    if (lines.size() == 5) {
        CHECK(Has(lines[0], "the svc hook's gate open again in 2017 us"));
        CHECK(Has(lines[1], "reached their limit for this launch (64)"));
        CHECK(Has(lines[2], "APT:ReceiveParameter answered: sender 0x00000101, command 3"));
        CHECK(Has(lines[3], "APT:ReceiveParameter failed"));
        CHECK(Has(lines[4], "APT:AppletUtility id 7 (input 0x00001234) -> 0x00000000"));
    }
    lines.clear();
    crumbs.Push(kAptNoteBadHandle, 0, 5000);
    crumbs.Push(kAptNoteWord, 0x2A, 5000);
    crumbs.Push(kAptNoteWord, 0x004B00C2, 5000);
    crumbs.Poll(5001, Os(5001), &lines);
    CHECK(lines.empty()); // (the line waits for its words, across turns of the thread)
    crumbs.Push(kAptNoteEnd, 0, 5000);
    crumbs.Push(kAptNoteFatal, 0x123, 5100);
    crumbs.Push(kAptNoteWord, 0xD8E007F7, 5100);
    crumbs.Push(kAptNoteEnd, 0, 5100);
    crumbs.Poll(5200, Os(5200), &lines);
    CHECK_EQ(lines.size(), std::size_t(2));
    if (lines.size() == 2) {
        CHECK(Has(lines[0], "a call of the game on handle 0x00000000 answered invalid handle: 0x0000002a 0x004b00c2 | the words:"));
        CHECK(Has(lines[1], "fatal error to err:f (session 0x00000123;"));
        CHECK(Has(lines[1], " 0xd8e007f7 | the words:"));
    }
}

// The close of a game (REGISTRO, phase N): the framework's HOME_ENTER / HOME_EXIT / EXIT of the process and the word after every ReceiveParameter carry
// Luma3DS's plugin word, read as the state (low 16 bits) and the events not handled yet (high 16). The vectors are what Luma3DS writes (rosalina
// plgloader.c): HOME_ENTER 0x00010001 and HOME_EXIT 0x00010002 (the state is written after the reply), EXIT 0x00000003 (EXITING written whole,
// events cleared, BEFORE the plugin is told: an EXIT callback never shows 0x00020001), 0x00000001 after the close command once HOME_EXIT went,
// and the events HOME and EXIT together (0x00030001 / 0x00030002), which Luma3DS handles in no branch: never at a callback.
static void TestCloseOfTheGame() {
    std::printf("sleep crumbs: the close of a game: Luma3DS's plugin word at HOME_ENTER, HOME_EXIT, EXIT and after ReceiveParameter, srv's 0x100\n");
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines);
    crumbs.Push(SleepCrumbs::kFwProcHomeEnter, 0x00010001, 2000);
    crumbs.Push(kAptNoteReceiveReply, 0x0101000C, 2100);   // sender 0x101 (the HOME menu), command 12: Close
    crumbs.Push(kAptNoteReceiveWord, 0x00010002, 2100);    // HOME_EXIT raised by Luma3DS's hook of the call, not handled yet
    crumbs.Push(SleepCrumbs::kFwProcHomeExit, 0x00010002, 2101);
    crumbs.Push(kAptNoteReceiveWord, 0x00000001, 2150);
    crumbs.Push(kAptNoteSrvNotification, 0x100, 2200);
    crumbs.Push(SleepCrumbs::kFwProcExit, 0x00000003, 2300);
    crumbs.Push(kAptNoteReceiveWord, 0x00030001, 2400);    // the events together: seen only after a call
    crumbs.Push(kAptNoteReceiveWord, 0x00030002, 2401);
    crumbs.Push(kAptNoteReceiveWord, 0x00000000, 2402);    // (Azahar: no plugin word)
    crumbs.Push(kAptNoteReceiveWord, 0x00070009, 2403);    // (nothing Luma3DS writes: said as it is)
    crumbs.Push(kAptNoteCloseLimit, 0, 2404);
    crumbs.Poll(2500, Os(2500), &lines);
    CHECK_EQ(lines.size(), std::size_t(12));
    if (lines.size() == 12) {
        CHECK(Has(lines[0], "sleep: framework event HOME_ENTER of the process, Luma3DS's plugin word 0x00010001 (state 1 running, events HOME) | tick 2000 ms"));
        CHECK(Has(lines[1], "APT:ReceiveParameter answered: sender 0x00000101, command 12"));
        CHECK(Has(lines[2], "after APT:ReceiveParameter, Luma3DS's plugin word 0x00010002 (state 2 in the HOME menu, events HOME) | tick 2100 ms"));
        CHECK(Has(lines[3], "framework event HOME_EXIT of the process, Luma3DS's plugin word 0x00010002 (state 2 in the HOME menu, events HOME)"));
        CHECK(Has(lines[4], "after APT:ReceiveParameter, Luma3DS's plugin word 0x00000001 (state 1 running, events none)"));
        CHECK(Has(lines[5], "srv notification 0x00000100 (the system asks the game to terminate) received by the game | tick 2200 ms"));
        CHECK(Has(lines[6], "framework event EXIT of the process, Luma3DS's plugin word 0x00000003 (state 3 exiting, events none) | tick 2300 ms"));
        CHECK(Has(lines[7], "0x00030001 (state 1 running, events HOME and EXIT together)"));
        CHECK(Has(lines[8], "0x00030002 (state 2 in the HOME menu, events HOME and EXIT together)"));
        CHECK(Has(lines[9], "0x00000000 (state 0 none, events none)"));
        CHECK(Has(lines[10], "0x00070009 (state 9 ?, events 0x00000007)"));
        CHECK(Has(lines[11], "the lines after the close command reached their own limit (32)"));
    }
    // the EXIT of the process is not the end of a sleep: kFwExit stays SLEEP_EXIT
    lines.clear();
    crumbs.Push(SleepCrumbs::kFwExit, 0, 2600);
    crumbs.Poll(2700, Os(2700), &lines);
    CHECK_EQ(lines.size(), std::size_t(1));
    if (lines.size() == 1) {
        CHECK(Has(lines[0], "framework event SLEEP_EXIT"));
    }
    // the numbers of the kinds are all different (a line read as another kind would be a wrong verdict)
    const u32 kinds[] = {kAptNoteNotification, kAptNoteReplyQuery, kAptNoteReplyComplete, SleepCrumbs::kFwEnter, SleepCrumbs::kFwExit,
                         SleepCrumbs::kFwSwapEnter, SleepCrumbs::kFwSwapExit, kAptNoteBadHandle, kAptNoteFatal, kAptNoteWord, kAptNoteEnd,
                         SleepCrumbs::kMemSystem, SleepCrumbs::kMemApplication, SleepCrumbs::kMemBase, kAptNoteReceiveReply, kAptNoteUtility,
                         kAptNoteUtilityResult, kAptNoteLimit, SleepCrumbs::kFwSwapBack, SleepCrumbs::kFwProcHomeEnter, SleepCrumbs::kFwProcHomeExit,
                         SleepCrumbs::kFwProcExit, kAptNoteReceiveWord, kAptNoteSrvNotification, kAptNoteCloseLimit,
                         SleepCrumbs::kExitGuardEnter, SleepCrumbs::kExitGuardRosalina, SleepCrumbs::kExitGuardWaited,
                         SleepCrumbs::kExitGuardWordLast, SleepCrumbs::kExitGuardAction, SleepCrumbs::kChatKeyboardOpen,
                         SleepCrumbs::kChatMirrorOn, SleepCrumbs::kChatKeyboardClosed, SleepCrumbs::kChatMirrorBackBegins,
                         SleepCrumbs::kChatMirrorBackDone};
    const std::size_t count = sizeof(kinds) / sizeof(kinds[0]);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            CHECK(kinds[i] != kinds[j]);
        }
    }
    CHECK_EQ(kLumaPluginWordAddress, 0x1FF800F0u);
}

// The chat's keyboard and its input mirror in the trail (2026-11-10): five lines an opening - open, the switch, closed, "about to hold the
// game's threads" (pushed BEFORE the hold) and "the threads let go" (pushed after) - so that a console that stops around the way back of the
// game's HID page leaves the step it had reached. The words are packed in one value each (SleepCrumbs::PackMirror).
static void TestChatMirror() {
    std::printf("sleep crumbs: the chat's keyboard and the input mirror: open, switched, closed, about to hold, let go\n");
    using hidmirror::Outcome;
    SleepCrumbs crumbs;
    std::vector<std::string> lines;
    crumbs.Poll(1000, Os(1000), &lines);
    // one opening as console 63 logged it (RR3D: open 18.727, mirror 18.775 with the threads held 1056 us, closed 31.688), then the way back
    crumbs.Push(SleepCrumbs::kChatKeyboardOpen, 3, 18727);
    crumbs.Push(SleepCrumbs::kChatMirrorOn, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kDone), 22, 1056), 18775);
    crumbs.Push(SleepCrumbs::kChatKeyboardClosed, SleepCrumbs::kChatClosedShown | SleepCrumbs::kChatClosedMirror, 31688);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(SleepCrumbs::kMirrorByPlugin, 24, 0), 32690);
    crumbs.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kDone), 24, 870), 32692);
    crumbs.Poll(32700, Os(32700), &lines);
    CHECK_EQ(lines.size(), std::size_t(5));
    if (lines.size() == 5) {
        CHECK(Has(lines[0], "sleep: chat: keyboard open (#3 of this launch) | tick 18727 ms"));
        CHECK(Has(lines[1], "sleep: chat: input mirror on, the game's HID page is the mirror's (22 threads, held 1056 us) | tick 18775 ms"));
        CHECK(Has(lines[2], "sleep: chat: keyboard closed, the game's HID page goes back once the keys are let go (1 s at most) | tick 31688 ms"));
        // (2026-11-11, the second review: the plugin's thread's line says that the hold may come after a wait - it is pushed BEFORE the
        // time this thread gives the trail to reach the card, and a console that stops in that wait leaves it with nothing after it)
        CHECK(Has(lines[3], "sleep: chat: input mirror: the game's HID page goes back, by the plugin's thread: about to hold the game's threads "
                            "(24 threads; this line is pushed before the hold, and this thread may first wait for it to be on the card: the "
                            "game's threads run in that wait, the hold is not begun) | tick 32690 ms"));
        CHECK(Has(lines[4], "sleep: chat: input mirror: the game's HID page is back, the game's threads let go (done; 24 threads, held 870 us) | "
                            "tick 32692 ms"));
    }
    // the other ways: a keyboard never shown, a close at once, no mirror (an emulator), a switch refused, a way back from a game's thread
    // and from the framework's events, one that fell back, the limits of the packed word
    lines.clear();
    crumbs.Push(SleepCrumbs::kChatKeyboardClosed, 0, 40000);
    crumbs.Push(SleepCrumbs::kChatKeyboardClosed,
                SleepCrumbs::kChatClosedShown | SleepCrumbs::kChatClosedMirror | SleepCrumbs::kChatClosedAtOnce, 40001);
    crumbs.Push(SleepCrumbs::kChatKeyboardClosed, SleepCrumbs::kChatClosedShown, 40002);
    crumbs.Push(SleepCrumbs::kChatMirrorOn, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kNeighbour), 30, 412), 40003);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(SleepCrumbs::kMirrorByNotification, 31, 0), 40004);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(3, 31, 0), 40005);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(5, 0, 0), 40006);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(0, 300, 0), 40007);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(1, 12, 0), 40008);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(42, 12, 0), 40009);
    crumbs.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kFellBack), 31, 70000), 40010);
    crumbs.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kLeftEmpty), 0, 5), 40011);
    crumbs.Poll(40100, Os(40100), &lines);
    CHECK_EQ(lines.size(), std::size_t(12));
    if (lines.size() == 12) {
        CHECK(Has(lines[0], "chat: keyboard closed, never shown, no input mirror to give back | tick 40000 ms"));
        CHECK(Has(lines[1], "chat: keyboard closed, the game's HID page goes back at once | tick 40001 ms"));
        CHECK(Has(lines[2], "chat: keyboard closed, no input mirror to give back | tick 40002 ms"));
        CHECK(Has(lines[3], "chat: no input mirror (a writable shared neighbour; 30 threads, held 412 us)"));
        CHECK(Has(lines[4], "goes back, by a game's thread, at a notification: about to hold the game's threads (31 threads;"));
        // (the threads that push the line right before the hold say nothing of a wait: only the plugin's thread has one)
        for (std::size_t i = 4; i <= 9; ++i) {
            CHECK(Has(lines[i], "; this line is pushed before the hold) | tick "));
            CHECK(!Has(lines[i], "may first wait"));
        }
        CHECK(Has(lines[5], "goes back, by the framework's thread, at HOME_ENTER: about to hold the game's threads (31 threads;"));
        CHECK(Has(lines[6], "goes back, by the framework's thread, at SWAP_ENTER: about to hold the game's threads (threads not counted;"));
        CHECK(Has(lines[7], "goes back, by the framework's thread, at EXIT: about to hold the game's threads (255 or more threads;"));
        CHECK(Has(lines[8], "goes back, by the framework's thread, at SLEEP_ENTER: about to hold"));
        CHECK(Has(lines[9], "goes back, by ?: about to hold"));
        CHECK(Has(lines[10], "chat: input mirror: the way back ended without the page back, the game's threads let go (the real page could not be "
                             "mapped back: the mirror stays; 31 threads, held 65535 us)"));
        CHECK(Has(lines[11], "the way back ended without the page back, the game's threads let go (the real page could not be mapped back: the "
                             "address is EMPTY; threads not counted, held 5 us)"));
    }
    // 2026-11-11: the plugin's thread said "about to hold", waited for the trail without the mirror's lock, and another thread gave the
    // page back meanwhile (or had the lock): its "about to hold" is closed by a line that says it held nothing
    lines.clear();
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(SleepCrumbs::kMirrorByPlugin, 24, 0), 41000);
    crumbs.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(SleepCrumbs::kMirrorByNotification, 24, 0), 41010);
    crumbs.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kDone), 24, 800), 41011);
    crumbs.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(static_cast<u32>(Outcome::kBusy), 24, 0), 41020);
    crumbs.Poll(41100, Os(41100), &lines);
    CHECK_EQ(lines.size(), std::size_t(4));
    if (lines.size() == 4) {
        CHECK(Has(lines[2], "chat: input mirror: the game's HID page is back, the game's threads let go (done; 24 threads, held 800 us)"));
        CHECK(Has(lines[3], "sleep: chat: input mirror: this thread did not hold the game's threads after all ("));
        CHECK(Has(lines[3], ": the lines of that thread say how it went) | tick 41020 ms"));
        CHECK(!Has(lines[3], "let go"));
    }
    // the packed word: 8 bits, 8 bits, 16 bits, each one capped and none spilling into its neighbour
    CHECK_EQ(SleepCrumbs::PackMirror(0, 0, 0), 0u);
    CHECK_EQ(SleepCrumbs::PackMirror(0x1FF, 0, 0), 0xFFu);
    CHECK_EQ(SleepCrumbs::PackMirror(0, 254, 0), 254u << 8);
    CHECK_EQ(SleepCrumbs::PackMirror(0, 1000, 0), 0xFFu << 8);
    CHECK_EQ(SleepCrumbs::PackMirror(0, 0, 65534), 65534u << 16);
    CHECK_EQ(SleepCrumbs::PackMirror(0, 0, 0xFFFFFFFFu), 0xFFFFu << 16);
    CHECK_EQ(SleepCrumbs::PackMirror(100, 64, 1007), 100u | 64u << 8 | 1007u << 16);
    // who gives the page back: the numbers the console's side uses (Sources/ctr/input_mirror.cpp asserts the framework's events)
    CHECK_EQ(SleepCrumbs::kMirrorByPlugin, 99u);
    CHECK_EQ(SleepCrumbs::kMirrorByNotification, 100u);
    // The chat's lines do not arm the watch of a sleep, and they have a limit of lines of their own (2026-11-11: counted against the
    // launch's 400, 80 openings of 5 lines used them all up, and the HOME, the swap or the exit at which the console then stopped left
    // no line). 48 openings of 5 lines, the last line the limit's; then no more of them - and every other line as before.
    CHECK(!crumbs.watching());
    CHECK_EQ(SleepCrumbs::kMaxChatLines, 240u);
    SleepCrumbs many;
    std::vector<std::string> all;
    many.Poll(1000, Os(1000), &all);
    for (u32 opening = 0; opening < 80; ++opening) {
        many.Push(SleepCrumbs::kChatKeyboardOpen, opening + 1, 2000 + opening);
        many.Push(SleepCrumbs::kChatMirrorOn, SleepCrumbs::PackMirror(0, 20, 700), 2000 + opening);
        many.Push(SleepCrumbs::kChatKeyboardClosed, 3, 2000 + opening);
        many.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(99, 20, 0), 2000 + opening);
        many.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(0, 20, 700), 2000 + opening);
        many.Poll(2000 + opening, Os(2000 + opening), &all);
    }
    CHECK_EQ(all.size(), std::size_t(SleepCrumbs::kMaxChatLines));
    CHECK_EQ(many.chat_lines(), SleepCrumbs::kMaxChatLines);
    CHECK_EQ(many.lines(), 0u); // (none of them counted against the launch's lines)
    if (all.size() == SleepCrumbs::kMaxChatLines) {
        CHECK(Has(all[SleepCrumbs::kMaxChatLines - 2], "about to hold the game's threads"));
        CHECK(Has(all[SleepCrumbs::kMaxChatLines - 1], "the limit of lines about the chat's keyboard for one launch is reached"));
        CHECK(!Has(all[SleepCrumbs::kMaxChatLines - 1], "nothing more is written"));
    }
    // the 81st opening leaves nothing; the HOME after it, the swap, the exit guard and the sleep are written like the first of the launch
    all.clear();
    many.Push(SleepCrumbs::kChatKeyboardOpen, 81, 3000);
    many.Push(SleepCrumbs::kChatMirrorBackBegins, SleepCrumbs::PackMirror(3, 20, 0), 3001);
    many.Push(SleepCrumbs::kFwProcHomeEnter, 0x00010001, 3002);
    many.Push(SleepCrumbs::kChatMirrorBackDone, SleepCrumbs::PackMirror(0, 20, 700), 3003);
    many.Push(SleepCrumbs::kFwSwapEnter, 1, 3004);
    many.Push(SleepCrumbs::kExitGuardEnter, 0x00000003, 3005);
    many.Push(SleepCrumbs::kExitGuardAction, 2, 3006);
    many.Push(kAptNoteNotification, 5, 3007);
    many.Poll(3010, Os(3010), &all);
    CHECK_EQ(all.size(), std::size_t(5));
    if (all.size() == 5) {
        CHECK(Has(all[0], "framework event HOME_ENTER of the process"));
        CHECK(Has(all[1], "framework event SWAP_ENTER #1"));
        CHECK(Has(all[2], "exit guard: the game calls svcExitProcess"));
        CHECK(Has(all[3], "exit guard: word set to running"));
        CHECK(Has(all[4], "APT notification 5 (sleep begins)"));
    }
    CHECK_EQ(many.lines(), 5u);
    // the mark of a chat's line does not outlive its event: after one (the last event of a turn), the thread's own lines - events that
    // were lost, the wake-up - are not taken for the chat's and dropped at the chat's limit
    all.clear();
    many.Push(SleepCrumbs::kChatKeyboardOpen, 82, 3100);
    many.Poll(3100, Os(3100), &all);
    CHECK(all.empty());
    for (u32 i = 0; i < 40; ++i) {
        many.Push(SleepCrumbs::kFwSwapExit, i, 3200 + i); // (40 events, 32 slots)
    }
    many.Poll(3300, Os(3300), &all);
    CHECK_EQ(all.size(), std::size_t(1 + SleepCrumbs::kSlots));
    if (!all.empty()) {
        CHECK(Has(all[0], "events were lost"));
    }
    // the other way round: the launch's own limit reached ("nothing more is written") stops the chat's lines too, as it says
    SleepCrumbs full;
    all.clear();
    full.Poll(1000, Os(1000), &all);
    full.Push(SleepCrumbs::kChatKeyboardOpen, 1, 1500);
    full.Poll(1500, Os(1500), &all);
    CHECK_EQ(all.size(), std::size_t(1));
    for (u32 round = 0; round < 20; ++round) {
        for (u32 i = 0; i < 30; ++i) {
            full.Push(SleepCrumbs::kFwSwapExit, i, 5000 + round * 100 + i);
        }
        full.Poll(5000 + round * 100 + 50, Os(5000 + round * 100 + 50), &all);
    }
    CHECK_EQ(all.size(), std::size_t(1 + SleepCrumbs::kMaxLines));
    CHECK(Has(all.back(), "the limit of lines for one launch is reached: nothing more is written"));
    full.Push(SleepCrumbs::kChatKeyboardOpen, 2, 9000);
    full.Poll(9000, Os(9000), &all);
    CHECK_EQ(all.size(), std::size_t(1 + SleepCrumbs::kMaxLines));
    CHECK_EQ(full.chat_lines(), 1u);
}

// The budget of the ReceiveParameter / AppletUtility lines (azl/apt_sleep.hpp, AptNoteBudget): 64, the limit's line once; after a close command 32
// more of their own, the close's own line among them even when it comes after the 64; then the close's limit line once, and nothing.
static void TestNoteBudget() {
    std::printf("sleep crumbs: the lines of ReceiveParameter: 64, then nothing until a close command, then 32 more\n");
    AptNoteBudget budget;
    for (u32 i = 0; i < 64; ++i) {
        CHECK_EQ(budget.Take(false), AptNoteBudget::kNoteWrite);
    }
    CHECK_EQ(budget.Take(false), AptNoteBudget::kNoteLimit); // the 65th: the limit's line instead of it
    for (u32 i = 0; i < 100; ++i) {
        CHECK_EQ(budget.Take(false), 0u); // a game that polls: nothing more
    }
    CHECK_EQ(budget.Take(true), AptNoteBudget::kNoteWrite); // the close (12) is written all the same
    for (u32 i = 1; i < 32; ++i) {
        CHECK_EQ(budget.Take(false), AptNoteBudget::kNoteWrite); // and what follows it
    }
    CHECK_EQ(budget.Take(false), AptNoteBudget::kNoteCloseLimit);
    CHECK_EQ(budget.Take(true), 0u);
    CHECK_EQ(budget.Take(false), 0u);
    // a close that comes as the 65th line: written, with the limit's line before it
    AptNoteBudget early;
    for (u32 i = 0; i < 64; ++i) {
        early.Take(false);
    }
    CHECK_EQ(early.Take(true), AptNoteBudget::kNoteLimit | AptNoteBudget::kNoteWrite);
    // a close within the 64: the normal lines go on, and after them the close's own
    AptNoteBudget within;
    CHECK_EQ(within.Take(true), AptNoteBudget::kNoteWrite);
    for (u32 i = 1; i < 64; ++i) {
        within.Take(false);
    }
    CHECK_EQ(within.Take(false), AptNoteBudget::kNoteLimit | AptNoteBudget::kNoteWrite);
    // the close command: 12 only (the HOME menu's Close). 10 (a library applet exited) and 11 (the return from the HOME menu) close nothing; 3 (a response) neither
    CHECK(AptCommandCloses(12));
    CHECK(!AptCommandCloses(10) && !AptCommandCloses(11));
    CHECK(!AptCommandCloses(3) && !AptCommandCloses(9) && !AptCommandCloses(13) && !AptCommandCloses(0));
    CHECK_EQ(kAptCommandCloseFromHome, 12u);
    // a HOME round trip (11) and a library applet (10) after the 64, then a game that polls, then the real close: the close's line is written
    AptNoteBudget home_trip;
    for (u32 i = 0; i < 64; ++i) {
        home_trip.Take(false);
    }
    CHECK_EQ(home_trip.Take(AptCommandCloses(11)), AptNoteBudget::kNoteLimit);
    CHECK_EQ(home_trip.Take(AptCommandCloses(10)), 0u);
    for (u32 i = 0; i < 64 + 32; ++i) {
        CHECK_EQ(home_trip.Take(AptCommandCloses(7)), 0u); // (no close-limit line: no close came)
    }
    CHECK_EQ(home_trip.Take(AptCommandCloses(12)), AptNoteBudget::kNoteWrite);
}

int main() {
    TestEventsBecomeLines();
    TestWakeMarker();
    TestNoWakeInAMinute();
    TestRingOverflowAndLimit();
    TestTwoProducersAndTheThread();
    TestOld3dsLines();
    TestCloseOfTheGame();
    TestChatMirror();
    TestNoteBudget();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
