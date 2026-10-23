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

int main() {
    TestEventsBecomeLines();
    TestWakeMarker();
    TestNoWakeInAMinute();
    TestRingOverflowAndLimit();
    TestTwoProducersAndTheThread();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
