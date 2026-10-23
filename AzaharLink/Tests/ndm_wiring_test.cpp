// ndm:u through the IPC front-end (F19). test_main covers NdmHle itself (every command, every result, the
// sessions); this test covers what UdsHle adds around it, with requests built the way a game builds them:
//
//   * one NdmHle session per fake ndm:u handle, released when the handle is closed;
//   * one process: every request is the game's own, whatever the PID word holds (the hook serves the request
//     before the kernel can fill that word in; libctru leaves it as the buffer held it, the SDK writes 0);
//   * the 7th handle (the real service has 6 sessions) still gets a fake session, never the real ndm:u, and
//     shares the newest one until the last handle on it is closed;
//   * the debug capture shows the words the header declares (no stale reply words) and the session numbers;
//     requests with a header the firmware refuses are served, counted and logged (the first few);
//   * game threads calling ndm:u while others open and close sessions and call nwm::UDS (lock order).
//
//   ndm_wiring_test            (needs no room server; built by Tests/ipc_test/build.sh)
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/log.hpp"

#include "hle_harness.hpp"

using namespace azl;

static std::vector<std::string> g_log;
static std::mutex g_log_mutex;
static int LogLines(const std::string& prefix) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    int n = 0;
    for (const std::string& line : g_log) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}
static bool LogHas(const std::string& text) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    for (const std::string& line : g_log) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

constexpr u32 kResAlreadyInState = 0xC8A06811;
constexpr u32 kResNotLocked = 0xC8A0680B;

// GetDaemonDisableCount(daemon) -> {this session's count, the global count}
struct Counts {
    u32 session;
    u32 global;
};
static Counts DaemonCount(HleConsole& ds, Handle h, u32 daemon) {
    const Reply r = ds.Call(h, req::Words(0x0E, 1, daemon));
    CHECK_EQ(r.w[0], 0x000E00C0u);
    CHECK_EQ(r.result(), 0u);
    return {r.w[2], r.w[3]};
}
static u32 ExclusiveState(HleConsole& ds, Handle h) {
    const Reply r = ds.Call(h, req::Make(0x03, 0, 0));
    CHECK_EQ(r.w[0], 0x00030080u);
    CHECK_EQ(r.result(), 0u);
    return r.w[2];
}

static void TestSessionPerHandle() {
    std::printf("ndm:u: one session per fake handle, released when the handle is closed\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle a = ds.Open("ndm:u");
    const Handle b = ds.Open("ndm:u");
    CHECK(a != 0 && b != 0 && a != b);
    // both start with Boss and Nim suspended once, by the default mask 0x09
    Counts c = DaemonCount(ds, b, 1);
    CHECK_EQ(c.session, 0u);
    CHECK_EQ(c.global, 1u);
    CHECK_EQ(DaemonCount(ds, b, 0).global, 0u);
    // A suspends Cec: B sees the global count and none of its own
    CHECK_EQ(ds.Call(a, req::Words(0x06, 1, 1)).result(), 0u);
    c = DaemonCount(ds, b, 0);
    CHECK_EQ(c.session, 0u);
    CHECK_EQ(c.global, 1u);
    c = DaemonCount(ds, a, 0);
    CHECK_EQ(c.session, 1u);
    CHECK_EQ(c.global, 1u);
    // B cannot resume what A suspended (the counts are per session)
    CHECK_EQ(ds.Call(b, req::Words(0x07, 1, 1)).result(), 0xC8A0680Du);
    // A also changes the scan interval and owns the override of the defaults
    CHECK_EQ(ds.Call(a, req::Words(0x10, 1, 99)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::Words(0x14, 1, 0x0F)).result(), 0u);
    CHECK_EQ(ds.Call(b, req::Words(0x14, 1, 0x01)).result(), 0xC8A06809u);
    CHECK_EQ(ds.Call(b, req::Make(0x16, 0, 0)).w[2], 0x0Fu);
    CHECK_EQ(ds.Call(b, req::Make(0x11, 0, 0)).w[2], 99u);
    // the game closes A (or dies): everything it held is released
    ds.Close(a);
    CHECK_EQ(DaemonCount(ds, b, 0).global, 0u);
    CHECK_EQ(ds.Call(b, req::Make(0x16, 0, 0)).w[2], 0x09u);
    CHECK_EQ(ds.Call(b, req::Make(0x11, 0, 0)).w[2], 30u);
    CHECK_EQ(DaemonCount(ds, b, 1).global, 1u);
    ds.Close(b);
}

static void TestOneProcess() {
    std::printf("ndm:u: every request is the game's own, whatever its PID word holds\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle a = ds.Open("ndm:u");
    const Handle b = ds.Open("ndm:u");
    // libctru writes the descriptor and leaves the PID word as the buffer held it (here 0xA5A5A5A5)
    CHECK_EQ(ds.Call(a, req::NdmEnter(2, false)).result(), 0u);
    CHECK_EQ(ExclusiveState(ds, b), 2u);
    CHECK_EQ(ds.Call(b, req::NdmEnter(2, false)).result(), kResAlreadyInState); // the same process owns it
    CHECK_EQ(ds.Call(b, req::NdmLeave(false)).result(), 0u);                    // so the other session may leave
    CHECK_EQ(ExclusiveState(ds, a), 0u);
    // the SDK's way (the word is 0) and libctru's in one sequence
    CHECK_EQ(ds.Call(a, req::NdmEnter(2, true)).result(), 0u);
    CHECK_EQ(ds.Call(b, req::NdmLeave(false)).result(), 0u);
    CHECK_EQ(ds.Call(b, req::NdmLeave(true)).result(), 0u); // leaving when nobody owns the state succeeds
    // the lock is recursive for the one process, across its sessions
    CHECK_EQ(ds.Call(a, req::NdmLock(0x04, false)).result(), 0u);
    CHECK_EQ(ds.Call(b, req::NdmLock(0x04, true)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::NdmLock(0x05, false)).result(), 0u);
    CHECK_EQ(ds.Call(b, req::NdmLock(0x05, true)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::NdmLock(0x05, false)).result(), kResNotLocked);
    ds.Close(a);
    ds.Close(b);
}

static void TestCloseLeavesTheState() {
    std::printf("ndm:u: a session that closes while it holds the exclusive state leaves it\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle a = ds.Open("ndm:u");
    const Handle b = ds.Open("ndm:u");
    CHECK_EQ(ds.Call(a, req::Words(0x14, 1, 0x0F)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::Words(0x06, 1, 6)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::NdmEnter(2)).result(), 0u);
    CHECK_EQ(ds.Call(a, req::NdmLock(0x04)).result(), 0u);
    CHECK_EQ(ExclusiveState(ds, b), 2u);
    CHECK_EQ(DaemonCount(ds, b, 1).global, 2u); // this session and the exclusive state
    ds.Close(a);
    CHECK_EQ(ExclusiveState(ds, b), 0u);
    CHECK_EQ(ds.Call(b, req::Make(0x16, 0, 0)).w[2], 0x09u); // the override is gone, Boss and Nim are back to default
    CHECK_EQ(DaemonCount(ds, b, 1).global, 1u);
    CHECK_EQ(DaemonCount(ds, b, 2).global, 1u);
    CHECK_EQ(DaemonCount(ds, b, 0).global, 0u);
    CHECK_EQ(ds.Call(b, req::NdmLock(0x04)).result(), 0u); // the lock went with it
    ds.Close(b);
}

static void TestAllSessionsInUse() {
    std::printf("ndm:u: the 7th handle shares a fake session, never the real service\n");
    MockRoom room;
    HleConsole ds(room, 3);
    DebugLines debug;
    const int logged_before = LogLines("uds: game opened ndm:u -> fake session 0x");
    const int opened_before = debug.Count("uds game opened ndm:u");
    Handle h[8];
    for (int i = 0; i < 8; ++i) {
        h[i] = ds.Open("ndm:u");
        CHECK(h[i] != 0);
        for (int j = 0; j < i; ++j) {
            CHECK(h[i] != h[j]);
        }
    }
    CHECK_EQ(debug.Count("uds game opened ndm:u") - opened_before, 8);
    CHECK_EQ(LogLines("uds: game opened ndm:u -> fake session 0x") - logged_before, 8);
    CHECK(LogHas("(ndm session 5)"));
    CHECK(LogHas("(all 6 ndm sessions are in use: shares session 5)"));
    CHECK(debug.Last("uds game opened ndm:u").find("shares session 5") != std::string::npos);
    // the 7th and 8th handle are served (HleConsole::Call fails the test if the request went to the kernel)
    CHECK_EQ(ds.Call(h[5], req::Words(0x06, 1, 4)).result(), 0u); // Nim
    Counts c = DaemonCount(ds, h[6], 2);
    CHECK_EQ(c.session, 1u); // it is session 5's own count
    CHECK_EQ(c.global, 2u);
    CHECK_EQ(DaemonCount(ds, h[7], 2).session, 1u);
    CHECK_EQ(DaemonCount(ds, h[0], 2).session, 0u);
    // the session goes with its LAST handle
    ds.Close(h[6]);
    CHECK_EQ(DaemonCount(ds, h[5], 2).session, 1u);
    ds.Close(h[5]);
    c = DaemonCount(ds, h[7], 2);
    CHECK_EQ(c.session, 1u);
    CHECK_EQ(c.global, 2u);
    ds.Close(h[7]);
    CHECK_EQ(DaemonCount(ds, h[0], 2).global, 1u); // released
    // the slot is free again: a new handle gets a session of its own
    const Handle again = ds.Open("ndm:u");
    CHECK_EQ(DaemonCount(ds, again, 2).session, 0u);
    CHECK_EQ(ds.Call(again, req::Words(0x06, 1, 4)).result(), 0u);
    CHECK_EQ(DaemonCount(ds, again, 2).session, 1u);
    CHECK_EQ(DaemonCount(ds, h[0], 2).session, 0u);
    ds.Close(again);
    for (int i = 0; i < 5; ++i) {
        ds.Close(h[i]);
    }
}

// The lines the front-end writes when a game opens and closes its fake sessions are built without printf now (they run
// on the game's thread: see azl/line.hpp); the text is the 0.1.15 text, character for character.
/// `line` is exactly  <prefix> <one or more lowercase hex digits> <suffix>.
static bool HexLine(const std::string& line, const std::string& prefix, const std::string& suffix) {
    if (line.size() <= prefix.size() + suffix.size() || line.compare(0, prefix.size(), prefix) != 0 ||
        line.compare(line.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    for (std::size_t i = prefix.size(); i < line.size() - suffix.size(); ++i) {
        const char c = line[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/// The newest line of the log that starts with `starts_with`, or "".
static std::string LastLog(const std::string& starts_with) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it) {
        if (it->compare(0, starts_with.size(), starts_with) == 0) {
            return *it;
        }
    }
    return "";
}

static void TestSessionLineText() {
    std::printf("fake sessions: the open / close lines read exactly as they did\n");
    MockRoom room;
    HleConsole ds(room, 3);
    DebugLines debug;
    Handle h[8];
    for (int i = 0; i < 8; ++i) {
        h[i] = ds.Open("ndm:u");
        if (i == 0) {
            CHECK(HexLine(LastLog("uds: game opened ndm:u"), "uds: game opened ndm:u -> fake session 0x", " (ndm session 0)"));
            CHECK(HexLine(debug.Last("uds game opened ndm:u"), "uds game opened ndm:u -> fake session 0x", " (ndm session 0)"));
        }
    }
    const std::string shared = " (all 6 ndm sessions are in use: shares session 5)";
    CHECK(HexLine(LastLog("uds: game opened ndm:u"), "uds: game opened ndm:u -> fake session 0x", shared));
    CHECK(HexLine(debug.Last("uds game opened ndm:u"), "uds game opened ndm:u -> fake session 0x", shared));
    ds.Close(h[3]);
    CHECK(HexLine(LastLog("uds: game closed its fake ndm:u"), "uds: game closed its fake ndm:u session 0x", " (ndm session 3)"));
    CHECK(HexLine(debug.Last("uds game closed its fake ndm:u"), "uds game closed its fake ndm:u session 0x", " (ndm session 3)"));
    const Handle uds = ds.Open("nwm::UDS");
    CHECK(HexLine(LastLog("uds: game opened nwm::UDS"), "uds: game opened nwm::UDS -> fake session 0x", ""));
    CHECK(HexLine(debug.Last("uds game opened nwm::UDS"), "uds game opened nwm::UDS -> fake session 0x", ""));
    ds.Close(uds);
    CHECK(HexLine(LastLog("uds: game closed its fake nwm::UDS"), "uds: game closed its fake nwm::UDS session 0x", ""));
    CHECK(HexLine(debug.Last("uds game closed its fake nwm::UDS"), "uds game closed its fake nwm::UDS session 0x", ""));
    for (int i = 0; i < 8; ++i) {
        if (i != 3) {
            ds.Close(h[i]);
        }
    }
}

static void TestAnomaliesAndDebugLines() {
    std::printf("ndm:u: headers the firmware refuses, and the lines of the capture\n");
    MockRoom room;
    HleConsole ds(room, 3);
    DebugLines debug;
    const int requests_before = LogLines("ndm: request 0x");
    const Handle a = ds.Open("ndm:u");
    CHECK(debug.Last("uds game opened ndm:u").find("(ndm session 0)") != std::string::npos);

    // a reply with more words than the next one must not leave its tail in the next line (the old bug)
    CHECK_EQ(ds.Call(a, req::Words(0x0E, 1, 1)).result(), 0u); // 3 reply words
    CHECK_EQ(ds.Call(a, req::NdmLeave()).result(), 0u);        // 1 reply word
    const auto leave = DebugLines::WordCounts(debug.Last("ndm cmd 0x02"));
    CHECK_EQ(leave.first, 2);
    CHECK_EQ(leave.second, 1);
    const auto count = DebugLines::WordCounts(debug.Last("ndm cmd 0x0e"));
    CHECK_EQ(count.first, 1);
    CHECK_EQ(count.second, 3);
    CHECK_EQ(ds.hle->ndm_header_anomalies(), 0u);
    CHECK_EQ(LogLines("ndm: request 0x") - requests_before, 0);

    // an Enter whose header declares no translate words: served as usual, counted, logged with its header
    std::vector<u32> bad = req::Make(0x01, 1, 0);
    bad[1] = 2;
    CHECK_EQ(ds.Call(a, bad).result(), 0u);
    CHECK_EQ(ExclusiveState(ds, a), 2u);
    CHECK_EQ(ds.hle->ndm_header_anomalies(), 1u);
    CHECK_EQ(LogLines("ndm: request 0x00010040 "), 1); // (no other test sends one)
    CHECK_EQ(ds.Call(a, req::NdmLeave()).result(), 0u);
    // the first 8 are in the log, the rest only in the count
    for (int i = 0; i < 12; ++i) {
        std::vector<u32> leave_bad = req::Make(0x02, 1, 2);
        leave_bad[2] = IPC_Desc_CurProcessId();
        CHECK_EQ(ds.Call(a, leave_bad).result(), 0u);
    }
    CHECK_EQ(ds.hle->ndm_header_anomalies(), 13u);
    CHECK_EQ(LogLines("ndm: request 0x") - requests_before, 8);
    // commands the firmware does not validate are never an anomaly
    CHECK_EQ(ds.Call(a, req::Words(0x06, 1, 0x1234)).result(), 0xD8E06BEDu);
    CHECK_EQ(ds.Call(a, req::Make(0x19, 0, 0)).result(), 0u); // an id the 02.46.01 ndm does not know: success
    CHECK_EQ(ds.hle->ndm_header_anomalies(), 13u);
    ds.Close(a);
}

// Game threads calling ndm:u while others open and close sessions and call nwm::UDS: no deadlock, and
// everything that was released ends where it started (lock order: UdsHle::mutex_ is never held into the engine).
static void TestThreads() {
    std::printf("ndm:u: threads on several sessions, sessions opened and closed, nwm::UDS in between\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle uds = ds.Open("nwm::UDS");
    CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0xABCD, "Race", 0), 0x14000, 0x400)).result(), 0u);
    std::atomic<bool> stop{false};
    std::atomic<int> bad_results{0};
    std::atomic<unsigned long> calls{0};
    auto worker = [&](int n) {
        const Handle h = ds.Open("ndm:u");
        while (!stop.load()) {
            const u32 mask = 1u << (n & 3);
            // a result outside {0, the firmware's codes} would mean a corrupted state
            const u32 r1 = ds.Call(h, req::Words(0x06, 1, mask)).result();
            const u32 r2 = ds.Call(h, req::Words(0x07, 1, mask)).result();
            const u32 r3 = ds.Call(h, req::NdmEnter(1, n & 1)).result();
            const u32 r4 = ds.Call(h, req::NdmLeave(n & 1)).result();
            if (r1 != 0 || r2 != 0 || (r3 != 0 && r3 != kResAlreadyInState) || (r4 != 0 && r4 != 0xC8A06810)) {
                bad_results.fetch_add(1);
            }
            calls.fetch_add(4);
        }
        ds.Close(h);
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back(worker, i);
    }
    std::thread churn([&] { // opens and closes sessions, 6 at a time and more
        while (!stop.load()) {
            std::vector<Handle> hs;
            for (int i = 0; i < 4; ++i) {
                hs.push_back(ds.Open("ndm:u"));
            }
            for (const Handle h : hs) {
                ds.Call(h, req::Words(0x08, 1, 0));
            }
            for (const Handle h : hs) {
                ds.Close(h);
            }
        }
    });
    std::thread udsy([&] {
        while (!stop.load()) {
            ds.Call(uds, req::GetConnectionStatus());
            ds.Call(uds, req::Disconnect());
            calls.fetch_add(2);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop = true;
    for (std::thread& t : workers) {
        t.join();
    }
    churn.join();
    udsy.join();
    CHECK(calls.load() > 1000);
    CHECK_EQ(bad_results.load(), 0);
    // every session is closed: what is left is the default state
    const Handle probe = ds.Open("ndm:u");
    CHECK_EQ(ExclusiveState(ds, probe), 0u);
    CHECK_EQ(ds.Call(probe, req::Make(0x16, 0, 0)).w[2], 0x09u);
    CHECK_EQ(DaemonCount(ds, probe, 0).global, 0u);
    CHECK_EQ(DaemonCount(ds, probe, 1).global, 1u);
    CHECK_EQ(DaemonCount(ds, probe, 2).global, 1u);
    CHECK_EQ(DaemonCount(ds, probe, 3).global, 0u);
    CHECK_EQ(ds.Call(probe, req::Make(0x0F, 0, 0)).w[3], 0u); // the scheduler
    ds.Close(probe);
    ds.Close(uds);
}

int main() {
    LogOn log_on; // (the tests read the lines of the log)
    SetLogSink([](const char* line) {
        std::lock_guard<std::mutex> l(g_log_mutex);
        g_log.push_back(line);
    });
    Watchdog dog(120);
    dog.Step("sessions");
    TestSessionPerHandle();
    dog.Step("one process");
    TestOneProcess();
    dog.Step("close");
    TestCloseLeavesTheState();
    dog.Step("all sessions in use");
    TestAllSessionsInUse();
    dog.Step("session line text");
    TestSessionLineText();
    dog.Step("anomalies and debug lines");
    TestAnomaliesAndDebugLines();
    dog.Step("threads");
    TestThreads();
    dog.Step("");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
