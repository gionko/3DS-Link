// The guard of the framework's link to Luma3DS's plugin loader (azl/plg_guard.hpp), run by Tests/run.sh: the decision after each try, the loop with a
// fake plgLdrInit (fails N times then connects; always fails; connects without its arbiter; an emulator without one), the fetch that answers 0 and
// never connects, and the line the plugin writes (REGISTRO, phase N: console 63, SSF4's data abort in PLGLDR__FetchEvent on a NULL event pointer).
#include <cstdio>
#include <cstring>
#include <string>

#include "azl/plg_guard.hpp"

#pragma GCC diagnostic ignored "-Wunused-function" // (support.hpp has helpers this test does not use)
#include "support.hpp"

using namespace azl;

namespace {

constexpr s32 kPortFull = static_cast<s32>(0xD0401834);     // svcConnectToPort: the port's one session is taken
constexpr s32 kNoSessions = static_cast<s32>(0xC8601809);   // the kernel's KSession objects are used up

// A fake plgLdrInit: `fails` tries answer `error`, then the connection goes through, with `arbiter` (0: the IPC 0x90000 did not give one) for
// the first `no_arbiter` sessions and 0x15 after. Counts what the guard asked of it.
struct FakeLoader {
    u32 fails = 0;
    s32 error = kPortFull;
    u32 no_arbiter = 0;
    u32 tries = 0;
    u32 closes = 0;
    u32 pauses = 0;
    u32 fetches = 0;
    s32 event = 0;
    bool open = false; ///< a session is held (plgLdrInit succeeded and no plgLdrExit since)

    plgguard::Try TryOnce() {
        ++tries;
        if (open) {
            // (plgLdrInit with its count above 0 does not connect again: the guard must have closed the session it did not take)
            return {static_cast<s32>(0xDEADBEEF), 0};
        }
        if (tries <= fails) {
            return {error, 0};
        }
        open = true;
        const u32 sessions = tries - fails;
        return {0, sessions <= no_arbiter ? 0u : 0x15u};
    }
    s32 Run(plgguard::State* s) {
        return plgguard::Connect(
            s, [this]() { return TryOnce(); },
            [this]() {
                ++closes;
                open = false;
            },
            [this]() { ++pauses; });
    }
    s32 Fetch() {
        ++fetches;
        return event;
    }
};

std::string Line(const plgguard::State& s) {
    char text[320];
    const int n = plgguard::FormatLine(s, text, sizeof(text));
    return n > 0 ? std::string(text, static_cast<std::size_t>(n)) : std::string();
}

bool Has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

static void TestFirstTry() {
    std::printf("plg guard: the first try goes through with its arbiter: one try, no pause, no line\n");
    FakeLoader fake;
    plgguard::State s;
    CHECK_EQ(fake.Run(&s), 0);
    CHECK(s.connected);
    CHECK_EQ(s.tries, 1u);
    CHECK_EQ(fake.tries, 1u);
    CHECK_EQ(fake.pauses, 0u);
    CHECK_EQ(fake.closes, 0u);
    CHECK_EQ(s.first_error, 0);
    CHECK(!plgguard::WorthALine(s));
    CHECK(Has(Line(s), "connected to Luma3DS's plugin loader at try 1 of 100"));
}

static void TestPortBusyThenFree() {
    std::printf("plg guard: the port busy for 3 tries, then free: try 4, the first error kept, a line\n");
    FakeLoader fake;
    fake.fails = 3;
    plgguard::State s;
    CHECK_EQ(fake.Run(&s), 0);
    CHECK(s.connected);
    CHECK_EQ(s.tries, 4u);
    CHECK_EQ(fake.pauses, 3u);  // (a pause between two tries, none after the last)
    CHECK_EQ(fake.closes, 0u);  // (a failed connection leaves nothing to close: plgLdrInit called plgLdrExit itself)
    CHECK_EQ(s.first_error, kPortFull);
    CHECK(plgguard::WorthALine(s));
    const std::string line = Line(s);
    CHECK(Has(line, "at try 4 of 100"));
    CHECK(Has(line, "first error 0xd0401834"));
    // the first error is the first one, not the last
    FakeLoader mixed;
    mixed.fails = 2;
    mixed.error = kNoSessions;
    plgguard::State m;
    CHECK_EQ(mixed.Run(&m), 0);
    CHECK_EQ(m.first_error, kNoSessions);
}

static void TestNeverConnects() {
    std::printf("plg guard: no connection in the whole budget: 100 tries, 99 pauses of 20 ms (about 2 s), not connected, the error returned\n");
    FakeLoader fake;
    fake.fails = 1000;
    plgguard::State s;
    CHECK_EQ(fake.Run(&s), kPortFull);
    CHECK(!s.connected);
    CHECK_EQ(s.tries, plgguard::kMaxTries);
    CHECK_EQ(fake.tries, 100u);
    CHECK_EQ(fake.pauses, 99u);
    CHECK_EQ(plgguard::kRetryMs * (plgguard::kMaxTries - 1), 1980u);
    CHECK(plgguard::WorthALine(s));
    const std::string line = Line(s);
    CHECK(Has(line, "could NOT connect to Luma3DS's plugin loader in 100 tries"));
    CHECK(Has(line, "the plugin stays off for this launch, restart the game"));
    // ... and the fetch answers 0 ("no event": KeepThreadMain waits again) without ever calling the framework's fetch or a connection
    for (int i = 0; i < 40; ++i) {
        fake.event = 5;
        CHECK_EQ(plgguard::FetchOrNone(s, [&fake]() { return fake.Fetch(); }), 0);
    }
    CHECK_EQ(fake.fetches, 0u);
    CHECK_EQ(fake.tries, 100u); // (no lazy connection: a try while Rosalina waits for our reply would block 10 s and read an old event)
}

static void TestSessionWithoutArbiter() {
    std::printf("plg guard: a session without its arbiter is closed and tried again; one that never gets it is no connection\n");
    FakeLoader fake;
    fake.no_arbiter = 2;
    plgguard::State s;
    CHECK_EQ(fake.Run(&s), 0);
    CHECK(s.connected);
    CHECK_EQ(s.tries, 3u);
    CHECK_EQ(s.no_arbiter, 2u);
    CHECK_EQ(fake.closes, 2u); // (each session without its arbiter went: the next plgLdrInit connects again from a count of 0)
    CHECK(plgguard::WorthALine(s));
    CHECK(Has(Line(s), "2 tries without the arbiter"));
    // on the console, an arbiter that never comes: no connection (every reply of the plugin would leave Luma3DS waiting 10 s)
    FakeLoader never;
    never.no_arbiter = 1000;
    plgguard::State n;
    CHECK_EQ(never.Run(&n), static_cast<s32>(0xD8E007F7));
    CHECK(!n.connected);
    CHECK_EQ(n.no_arbiter, 100u);
    CHECK_EQ(never.closes, 100u); // (the last one too: nothing is left open)
    CHECK(!never.open);
    CHECK_EQ(n.first_error, 0);
    CHECK(Has(Line(n), "could NOT connect"));
    CHECK(Has(Line(n), "100 tries without the arbiter"));
}

static void TestEmulator() {
    std::printf("plg guard: on an emulator a session without an arbiter is taken (Azahar's plg:ldr), and said\n");
    FakeLoader fake;
    fake.no_arbiter = 1000;
    plgguard::State s;
    s.emulator = true;
    CHECK_EQ(fake.Run(&s), 0);
    CHECK(s.connected);
    CHECK_EQ(s.tries, 1u);
    CHECK_EQ(fake.closes, 0u);
    CHECK(!plgguard::WorthALine(s)); // (the emulator's normal case: no synchronous line at every launch; the debug log still says it)
    CHECK(Has(Line(s), "taken on an emulator"));
    // ... and with an arbiter, nothing to say
    FakeLoader good;
    plgguard::State g;
    g.emulator = true;
    CHECK_EQ(good.Run(&g), 0);
    CHECK(!plgguard::WorthALine(g));
}

static void TestFetchConnected() {
    std::printf("plg guard: connected, the fetch is the framework's own\n");
    FakeLoader fake;
    plgguard::State s;
    fake.Run(&s);
    for (s32 event : {0, 1, 3, 4, 5, 6, -1}) {
        fake.event = event;
        CHECK_EQ(plgguard::FetchOrNone(s, [&fake]() { return fake.Fetch(); }), event);
    }
    CHECK_EQ(fake.fetches, 7u);
    CHECK_EQ(fake.tries, 1u);
}

static void TestLineFits() {
    std::printf("plg guard: the line fits the plugin's buffer whatever the numbers, and a buffer too small gives no line\n");
    plgguard::State s;
    s.tries = 4294967295u;
    s.first_error = static_cast<s32>(0xFFFFFFFF);
    s.no_arbiter = 4294967295u;
    s.ms = 4294967295u;
    char text[320 - 26]; // (the plugin's 320 bytes less the clock in front, "[2026-11-08 01:02:03.456] ")
    CHECK(plgguard::FormatLine(s, text, sizeof(text)) > 0);
    s.connected = true;
    CHECK(plgguard::FormatLine(s, text, sizeof(text)) > 0);
    char small[16];
    CHECK_EQ(plgguard::FormatLine(s, small, sizeof(small)), 0);
}

int main() {
    TestFirstTry();
    TestPortBusyThenFree();
    TestNeverConnects();
    TestSessionWithoutArbiter();
    TestEmulator();
    TestFetchConnected();
    TestLineFits();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
