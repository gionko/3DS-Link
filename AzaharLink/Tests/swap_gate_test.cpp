// Tests for the gate of the plugin's threads around the swap of its memory on an Old 3DS (Includes/azl/swap_gate.hpp).
//   swap_gate_test
// What the console side adds (ctr/swap_gate.cpp: the event, the real clock; the callers) is held by Tests/policy_check.sh. Luma3DS and the kernel themselves are not here: only an Old 3DS shows them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "azl/swap_gate.hpp"

namespace azl {
SwapGate g_swap_gate;
}

using namespace azl;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

namespace {

void NoPark() {}

struct FakeClock {
    u64 now = 1000;
    u32 steps = 0;
    void (*on_step)(u32 step) = nullptr;
};
FakeClock g_clock;
auto kNow = []() { return g_clock.now; };
auto kStep = []() {
    g_clock.now += 1000;
    ++g_clock.steps;
    if (g_clock.on_step) {
        g_clock.on_step(g_clock.steps);
    }
};

SwapGate MakeOn() {
    SwapGate g;
    g.on = 1;
    g.park = NoPark;
    return g;
}

// A New 3DS: the gate stays off, nothing is counted, nothing ever stops and nothing is waited for.
void TestOff() {
    SwapGate g;
    g.park = nullptr; // (a stop would crash)
    GateEnter(g, kGateNet);
    CHECK(g.inside == 0);
    GateLeave(g, kGateNet);
    CHECK(g.inside == 0);
    g_clock = FakeClock{};
    g.inside = kGateNet; // (even with somebody counted: nothing to wait for)
    CHECK(GateCloseAndDrain(g, 1000000, kNow, kStep) == 0);
    CHECK(g.closed == 0);
    CHECK(g_clock.steps == 0);
    CHECK(g.stats.closes == 0);
    GateOpen(g);
    CHECK(g.stats.opens == 0);
    g.inside = 0;
    GateEnter(g, kGateNet); // (still open: closing did nothing)
    CHECK(g.inside == 0);
}

void TestCounting() {
    SwapGate g = MakeOn();
    GateEnter(g, kGateNet);
    CHECK(g.inside == kGateNet);
    GateEnter(g, kGateWriter);
    CHECK(g.inside == (kGateNet | kGateWriter));
    GateLeave(g, kGateNet);
    CHECK(g.inside == kGateWriter);
    GateLeave(g, kGateWriter);
    CHECK(g.inside == 0);
    CHECK(g.stops == 0);
}

void TestClosedNotCounted() {
    SwapGate g = MakeOn();
    g_clock = FakeClock{};
    CHECK(GateCloseAndDrain(g, 1000, kNow, kStep) == 0);
    CHECK(!GateTryEnter(g, kGateNet));
    CHECK(g.inside == 0); // (a thread stopped in front of the gate must not be waited for)
    GateOpen(g);
    CHECK(GateTryEnter(g, kGateNet));
    CHECK(g.inside == kGateNet);
}

SwapGate* g_step_gate = nullptr;

void TestDrainWaitsForWhoIsInside() {
    SwapGate g = MakeOn();
    GateEnter(g, kGateNet);
    GateEnter(g, kGateWriter);
    g_step_gate = &g;
    g_clock = FakeClock{};
    g_clock.on_step = [](u32 step) {
        if (step == 3) {
            GateLeave(*g_step_gate, kGateNet);
        }
        if (step == 5) {
            GateLeave(*g_step_gate, kGateWriter);
        }
    };
    CHECK(GateCloseAndDrain(g, 1000000, kNow, kStep) == 0);
    CHECK(g.closed == 1);
    CHECK(g_clock.steps == 5); // (not one step fewer, and none more once they have left)
    CHECK(g.stats.inside_start == (kGateNet | kGateWriter));
    CHECK(g.stats.inside_left == 0);
    CHECK(g.stats.waited_us == 5000);
    CHECK(g.stats.closes == 1);
}

void TestDrainGivesUp() {
    SwapGate g = MakeOn();
    GateEnter(g, kGateNet); // (a getaddrinfo with no DNS: it does not leave)
    g_clock = FakeClock{};
    CHECK(GateCloseAndDrain(g, 3000, kNow, kStep) == kGateNet);
    CHECK(g_clock.steps == 3);
    CHECK(g.closed == 1); // (the gate stays closed: the others must still stop)
    CHECK(g.stats.inside_left == kGateNet);
    CHECK(g.stats.waited_us == 3000);
    CHECK(!GateTryEnter(g, kGateWriter));
    GateOpen(g);
    CHECK(g.closed == 0);
    CHECK(g.stats.opens == 1);
    GateOpen(g); // (the shutdown after a SWAP_EXIT: no second count)
    CHECK(g.stats.opens == 1);
}

void TestDrainNobodyInside() {
    SwapGate g = MakeOn();
    g_clock = FakeClock{};
    CHECK(GateCloseAndDrain(g, 1000000, kNow, kStep) == 0);
    CHECK(g_clock.steps == 0);
    CHECK(g.stats.waited_us == 0);
}

void TestWaitedSaturates() {
    SwapGate g = MakeOn();
    GateEnter(g, kGateNet);
    g_clock = FakeClock{};
    auto big = []() { g_clock.now += 0x200000000ull; };
    CHECK(GateCloseAndDrain(g, 0x100000000ull, kNow, big) == kGateNet);
    CHECK(g.stats.waited_us == 0xFFFFFFFFu);
}

// A stop parks, and comes back to try again.
SwapGate* g_park_gate = nullptr;
int g_parks = 0;
void ParkAndOpen() {
    if (++g_parks == 2) {
        GateOpen(*g_park_gate);
    }
}

void TestParkLoop() {
    SwapGate g = MakeOn();
    g.park = ParkAndOpen;
    g_park_gate = &g;
    g_parks = 0;
    g_clock = FakeClock{};
    GateCloseAndDrain(g, 1000, kNow, kStep);
    GateEnter(g, kGateWriter);
    CHECK(g_parks == 2);
    CHECK(g.stops == 2);
    CHECK(g.inside == kGateWriter);
}

void TestScope() {
    g_swap_gate = MakeOn();
    {
        const SwapGateScope scope(kGateTrail);
        CHECK(g_swap_gate.inside == kGateTrail);
    }
    CHECK(g_swap_gate.inside == 0);
    g_swap_gate = SwapGate{};
}

void TestWeightsApart() {
    CHECK((kGateNet & kGateWriter) == 0);
    CHECK((kGateNet & kGateTrail) == 0);
    CHECK((kGateWriter & kGateTrail) == 0);
    // (each kind counts up to 255 threads before it reaches the next: one thread each today)
    CHECK(kGateWriter / kGateNet >= 16);
    CHECK(kGateTrail / kGateWriter >= 16);
}

// The promise itself, with real threads: from the moment the close returns with nobody left until the gate opens, no thread is in a call. The call is a flag
// with a pause in it, and a thread that is in one when the "block" is gone is the crash of the report.
std::atomic<bool> g_block_gone{false};
std::atomic<u32> g_in_call{0};
std::atomic<u32> g_crashes{0};
std::atomic<u64> g_calls{0};
std::atomic<bool> g_open_flag{true};

void ParkThread() {
    while (!g_open_flag.load()) {
        std::this_thread::yield();
    }
}

void Worker(SwapGate* g, u32 who, std::atomic<bool>* stop) {
    while (!stop->load(std::memory_order_relaxed)) {
        GateEnter(*g, who);
        g_in_call.fetch_add(1);
        if (g_block_gone.load()) {
            g_crashes.fetch_add(1);
        }
        std::this_thread::yield();
        if (g_block_gone.load()) {
            g_crashes.fetch_add(1);
        }
        g_in_call.fetch_sub(1);
        GateLeave(*g, who);
        g_calls.fetch_add(1, std::memory_order_relaxed);
    }
}

void TestRealThreads() {
    SwapGate g;
    g.on = 1;
    g.park = ParkThread;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.emplace_back(Worker, &g, kGateNet, &stop);
    threads.emplace_back(Worker, &g, kGateWriter, &stop);
    threads.emplace_back(Worker, &g, kGateTrail, &stop);
    auto now = []() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    auto step = []() { std::this_thread::yield(); };
    u32 left_total = 0;
    for (int round = 0; round < 2000; ++round) {
        const u64 before = g_calls.load();
        while (g_calls.load() == before) { // (they run between swaps: the threads come back after each one)
            std::this_thread::yield();
        }
        g_open_flag.store(false);
        const u32 left = GateCloseAndDrain(g, 5000000, now, step);
        left_total += left;
        CHECK(g_in_call.load() == 0);
        g_block_gone.store(true);
        std::this_thread::yield();
        CHECK(g_in_call.load() == 0);
        g_block_gone.store(false);
        GateOpen(g);
        g_open_flag.store(true);
    }
    stop.store(true);
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(left_total == 0);
    CHECK(g_crashes.load() == 0);
    CHECK(g.inside == 0);
    CHECK(g.stats.closes == 2000);
    CHECK(g.stats.opens == 2000);
    std::printf("real threads: %llu calls, %u stops\n", static_cast<unsigned long long>(g_calls.load()), g.stops);
}

} // namespace

int main() {
    TestOff();
    TestCounting();
    TestClosedNotCounted();
    TestDrainWaitsForWhoIsInside();
    TestDrainGivesUp();
    TestDrainNobodyInside();
    TestWaitedSaturates();
    TestParkLoop();
    TestScope();
    TestWeightsApart();
    TestRealThreads();
    std::printf("swap_gate_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
