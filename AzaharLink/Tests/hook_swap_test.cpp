// Tests for the plugin's side of the persistent gate of the svc hook (Includes/azl/hook_swap.hpp: GateClose, GateOpen, GateClosing).
//   hook_swap_test
// The order of the closer's accesses, the wait, real threads that go through a model of Sources/ctr/gate.s while a closer takes the
// "block" away (built again with ThreadSanitizer), and the permanent test that no word of the game's text changes after the installation
// (WriteGate of azl/gate_plan.hpp) whatever the swaps do. The control of that test - the same scenario through the two-word sites of
// 1.1.2, whose writes the checker found on every swap - ran in step 2 of analysis/fix-gancio-swap-old-2026-11-05/PIANO-LAVORO.md and
// left with that code. What the console side adds (ctr/hook.cpp: CloseGate, OpenGate, InstallSvcHooks with WriteGate) is held by
// Tests/chain_sim, Tests/hook_sim/run_sim.py and Tests/policy_check.sh; the gate's assembly by Tests/hook_sim/gate_sim.py.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "azl/hook_swap.hpp"

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

constexpr u32 kBxLrWord = 0xE12FFF1E; // bx lr

// ---- The persistent gate ----

/// The closer's accesses, recorded in order (the console's are GateAtomics).
std::vector<std::string> g_gate_trace;
struct RecordingOps {
    static void StoreFlag(volatile u32* w, u32 v) {
        g_gate_trace.push_back("flag=" + std::to_string(v));
        *w = v;
    }
    static u32 LoadCount(const volatile u32* w) {
        g_gate_trace.push_back("count");
        return *w;
    }
    static u32 LoadWord(const volatile u32* w) { return *w; }
    static void Barrier() { g_gate_trace.push_back("barrier"); }
};

u64 g_fake_us = 0;
GateHeader* g_drained = nullptr; // the step lets one thread out of the count each time
u32 g_steps = 0;
auto kFakeNow = []() { return g_fake_us; };
auto kFakeStep = []() {
    g_fake_us += 100;
    ++g_steps;
    if (g_drained && g_drained->in_flight > 0) {
        g_drained->in_flight = g_drained->in_flight - 1;
    }
};

// Dekker: the flag goes to 0, then the barrier, then the count is read - in that order, and the first look at the count only after both.
void TestGateCloseOrder() {
    GateHeader g{};
    g.flag = 1;
    g_gate_trace.clear();
    HookGateStats st;
    CHECK((GateClose<RecordingOps>(g, 1000, kFakeNow, kFakeStep, &st)) == 0);
    CHECK(g.flag == 0);
    CHECK(g_gate_trace.size() == 3);
    CHECK(g_gate_trace.size() >= 3 && g_gate_trace[0] == "flag=0" && g_gate_trace[1] == "barrier" && g_gate_trace[2] == "count");
    // the open writes the flag and nothing else
    g_gate_trace.clear();
    GateOpen<RecordingOps>(g, &st);
    CHECK(g_gate_trace.size() == 1 && g_gate_trace[0] == "flag=1");
    CHECK(g.flag == 1 && st.opens == 1);
}

void TestGateCloseWaits() {
    GateHeader g{};
    g.flag = 1;
    g.in_flight = 3;
    g.last_site = 0x01E7E3A8;
    g.last_handle = 0x00C40001;
    g.post_skipped = 5;
    g_drained = &g;
    g_steps = 0;
    g_fake_us = 1000;
    HookGateStats st;
    CHECK(GateClose(g, 1500000, kFakeNow, kFakeStep, &st) == 0); // the three leave, one a step
    CHECK(g_steps == 3);
    CHECK(st.closes == 1 && st.in_flight_start == 3 && st.in_flight_left == 0 && st.waited_us == 300);
    CHECK(st.last_site == 0x01E7E3A8 && st.last_handle == 0x00C40001 && st.post_skipped == 5);
    CHECK(GateClosing(g));
    // one that never leaves: the close gives up after the timeout, and says who it was waiting for
    g_drained = nullptr;
    g.in_flight = 1;
    g_steps = 0;
    CHECK(GateClose(g, 1500000, kFakeNow, kFakeStep, &st) == 1);
    CHECK(st.closes == 2 && st.in_flight_start == 1 && st.in_flight_left == 1 && st.waited_us >= 1500000 && st.waited_us < 1500200);
    CHECK(g_steps == 15000);
    CHECK(g.flag == 0); // (closed either way)
    // nobody inside: no step at all
    g.in_flight = 0;
    g_steps = 0;
    CHECK(GateClose(g, 1500000, kFakeNow, kFakeStep, nullptr) == 0);
    CHECK(g_steps == 0);
}

void TestGateOpenClosing() {
    GateHeader g{};
    HookGateStats st;
    CHECK(GateClosing(g)); // (flag 0 until the installation sets it)
    GateOpen(g, &st);
    CHECK(!GateClosing(g) && g.flag == 1 && st.opens == 1);
    GateOpen(g, &st); // idempotent: SWAP_EXIT after OnSwapBack
    CHECK(g.flag == 1 && st.opens == 1);
    g_fake_us = 0;
    GateClose(g, 0, kFakeNow, kFakeStep, &st);
    CHECK(GateClosing(g));
    GateOpen(g, &st);
    CHECK(!GateClosing(g) && st.opens == 2);
}

/// The game's side, as Sources/ctr/gate.s does it: count in, barrier, read the flag; barrier, count out.
bool ModelCountIn(GateHeader& g) {
    __atomic_fetch_add(&g.in_flight, 1u, __ATOMIC_SEQ_CST);
    GateAtomics::Barrier();
    return __atomic_load_n(&g.flag, __ATOMIC_SEQ_CST) != 0;
}
void ModelCountOut(GateHeader& g) {
    GateAtomics::Barrier();
    __atomic_fetch_sub(&g.in_flight, 1u, __ATOMIC_SEQ_CST);
}

/// The plugin's block: pre and post may only run while it is mapped, and only while the thread is counted.
struct FakeBlock {
    std::atomic<bool> mapped{true};
    std::atomic<u32> touched_unmapped{0};
    std::atomic<u32> touched_uncounted{0};
    std::atomic<u32> pres{0};
    std::atomic<u32> posts{0};
    void Touch(GateHeader& g, std::atomic<u32>& counter) {
        if (!mapped.load(std::memory_order_seq_cst)) {
            ++touched_unmapped;
        }
        if (__atomic_load_n(&g.in_flight, __ATOMIC_SEQ_CST) == 0) {
            ++touched_uncounted;
        }
        ++counter;
    }
};

/// One call through the gate: pre (if the plugin is there), the "svc", post (if asked and the plugin is there, again).
/// Returns whether pre ran.
bool ModelCall(GateHeader& g, FakeBlock& b, u32 action, u32 site, bool sleep_in_svc) {
    __atomic_store_n(&g.last_site, site, __ATOMIC_RELAXED);
    bool pre = false;
    if (ModelCountIn(g)) {
        b.Touch(g, b.pres);
        pre = true;
    } else {
        action = 1; // the plugin is away: the svc, no post
    }
    ModelCountOut(g);
    if (action == 0) {
        return pre;
    }
    if (sleep_in_svc) {
        std::this_thread::yield(); // (asleep in the gate's svc: uncounted, the swap may come and go)
    }
    if (action >= 2) {
        if (ModelCountIn(g)) {
            b.Touch(g, b.posts);
        } else {
            __atomic_fetch_add(&g.post_skipped, 1u, __ATOMIC_SEQ_CST);
        }
        ModelCountOut(g);
    }
    return pre;
}

// N game threads go through the gate for ever; a closer closes it, takes the block away, puts it back and opens it, again and again.
// Not one touch of the block while it is away, not one touch while not counted, every close drains, and the count ends at 0.
void TestGateThreads() {
    alignas(8) static GateHeader g{};
    g.flag = 1;
    FakeBlock block;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    std::atomic<u32> calls{0};
    for (u32 t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            u32 i = 0;
            while (!stop.load()) {
                ModelCall(g, block, (t + i) % 3, 0x01E7E000 + 12 * t, (i & 7) == 0);
                ++i;
                ++calls;
            }
        });
    }
    HookGateStats st;
    u32 undrained = 0;
    const auto now = []() {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const auto step = []() { std::this_thread::yield(); };
    for (int round = 0; round < 1500; ++round) {
        if (GateClose(g, 2000000, now, step, &st) != 0) {
            ++undrained;
        }
        block.mapped.store(false, std::memory_order_seq_cst); // the swap: the block is gone
        for (int k = 0; k < 20; ++k) {
            std::this_thread::yield();
        }
        block.mapped.store(true, std::memory_order_seq_cst); // ... and back (OnSwapBack)
        GateOpen(g, &st);
        for (int k = 0; k < 20; ++k) {
            std::this_thread::yield();
        }
    }
    stop.store(true);
    for (std::thread& th : threads) {
        th.join();
    }
    CHECK(undrained == 0);
    CHECK(block.touched_unmapped.load() == 0);
    CHECK(block.touched_uncounted.load() == 0);
    CHECK(g.in_flight == 0);
    CHECK(st.closes == 1500 && st.opens == 1500);
    CHECK(block.pres.load() > 0 && block.posts.load() > 0);
    std::printf("  gate threads: %u calls, %u pre, %u post, %u posts skipped over 1500 swaps\n", calls.load(), block.pres.load(),
                block.posts.load(), static_cast<u32>(g.post_skipped));
}

// ---- No word of the game's text changes after the installation ----

constexpr u32 kTextBase = 0x00100000;
constexpr u32 kGatePage = 0x01E7E000;

/// The game's text and the gate's page; every store to the text after `armed` is counted.
struct TextAndPage {
    std::map<u32, u32> text;
    alignas(8) u32 page[2 * kGatePageBytes / 4] = {};
    u32 page_bytes = 2 * kGatePageBytes;
    bool armed = false;
    u32 text_stores_after = 0;
    u32 Load(u32 a) { return text.count(a) ? text[a] : 0; }
    void StorePage(u32 a, u32 w) { page[(a - kGatePage) / 4] = w; }
    bool PatchText(u32 a, u32 w) {
        if (armed) {
            ++text_stores_after;
        }
        text[a] = w;
        return true;
    }
    void Sync() {}
    GateHeader& Header() { return *reinterpret_cast<GateHeader*>(page); }
};

/// The sites of the scenario: shared `svc; bx lr` stubs (LEGO Batman 2, Monster Hunter XX, the Kirbys) and sites of their own, both svc.
std::vector<GateSite> ScenarioSites(TextAndPage& m) {
    std::vector<GateSite> sites;
    for (u32 i = 0; i < 12; ++i) {
        const u32 a = kTextBase + 0x1000 + i * 0x100;
        const u32 svc = i % 4 == 3 ? kGateSvc23Word : kGateSvc32Word;
        m.text[a - 4] = 0xE1A00000 + i;
        m.text[a] = svc;
        m.text[a + 4] = i % 2 ? kBxLrWord : 0xE2100102 + i; // bx lr, or the game's own next instruction
        sites.push_back({a, svc});
    }
    return sites;
}

// The gate: 100 cycles of HOME (close, the block away, a thread asleep in the svc whose post comes while the plugin is away, open),
// the power menu with no swap (close, open), and calls with every decision (answered, svc, svc + post) in between. The text is the
// same word for word as right after WriteGate; the page's code and stubs too; only the header's data moved.
void TestNoTextWordChangesWithTheGate() {
    static TextAndPage m;
    std::vector<GateSite> sites = ScenarioSites(m);
    static u32 blob[kGateBytes / 4];
    for (u32 i = 0; i < kGateBytes / 4; ++i) {
        blob[i] = 0xE1A00000 | i; // (stand-in code: what matters is that it never changes)
    }
    const GateBlob gb{blob, kGateBytes, kGateCode, kGateEntry23};
    const GatePointers ptrs{0x07001000, 0x07001100, 0x07001200, 0x07001300};
    const GateWriteResult r = WriteGate(m, kGatePage, m.page_bytes, gb, ptrs, sites.data(), static_cast<u32>(sites.size()));
    CHECK(r.ok && r.hooked == 12);
    const std::map<u32, u32> text_after_install = m.text;
    const std::vector<u32> code_after_install(m.page + kGateCode / 4, m.page + m.page_bytes / 4);
    m.armed = true;
    GateHeader& g = m.Header();
    CHECK(g.flag == 1);
    FakeBlock block;
    HookGateStats st;
    u32 homes = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (u32 action = 0; action < 3; ++action) {
            ModelCall(g, block, action, StubsBase(kGatePage) + 12 * (cycle % 12) + 8, false);
        }
        g_fake_us = 0;
        CHECK(GateClose(g, 1500000, kFakeNow, kFakeStep, &st) == 0);
        if (cycle % 4 == 3) {
            GateOpen(g, &st); // the power menu, cancelled: no swap
            continue;
        }
        ++homes;
        block.mapped = false;
        // the thread that raised the swap: asleep in the gate's svc, it wakes while the plugin is away; its post is skipped
        if (ModelCountIn(g)) {
            block.Touch(g, block.posts);
        } else {
            __atomic_fetch_add(&g.post_skipped, 1u, __ATOMIC_SEQ_CST);
        }
        ModelCountOut(g);
        ModelCall(g, block, 2, StubsBase(kGatePage) + 8, false); // a call during the HOME menu: straight to the kernel
        block.mapped = true;
        GateOpen(g, &st);
        CHECK(m.text == text_after_install);
    }
    CHECK(m.text_stores_after == 0);
    CHECK(m.text == text_after_install);
    CHECK(std::vector<u32>(m.page + kGateCode / 4, m.page + m.page_bytes / 4) == code_after_install);
    CHECK(block.touched_unmapped.load() == 0 && block.touched_uncounted.load() == 0);
    CHECK(g.flag == 1 && g.in_flight == 0);
    CHECK(g.post_skipped == homes); // (the sleeper's; a call made during the HOME menu asks for no post: pre never ran)
    CHECK(g.pre32 == ptrs.pre32 && g.post23 == ptrs.post23);
}

} // namespace

int main() {
    TestGateCloseOrder();
    TestGateCloseWaits();
    TestGateOpenClosing();
    TestGateThreads();
    TestNoTextWordChangesWithTheGate();
    std::printf("hook_swap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
