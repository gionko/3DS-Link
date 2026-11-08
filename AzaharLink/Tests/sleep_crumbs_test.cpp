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
                         SleepCrumbs::kExitGuardWordLast, SleepCrumbs::kExitGuardAction};
    const std::size_t count = sizeof(kinds) / sizeof(kinds[0]);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            CHECK(kinds[i] != kinds[j]);
        }
    }
    CHECK_EQ(kLumaPluginWordAddress, 0x1FF800F0u);
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
    TestNoteBudget();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
