// dlp:FKCL through the IPC front-end: the wireless check of the games that play over UDS (azl/dlp_fkcl_hle.hpp).
//
//   * off by default: a game that opens dlp:FKCL gets the REAL service (srvGetServiceHandle is not intercepted);
//   * with SetDlpProbeFake(true): a fake session; Initialize, InitializeWithName and Finalize succeed and carry the
//     reply header of the real service (one normal word, no translate words), however the request was laid out;
//   * every other command of the service is refused with "invalid state" (and logged, the first few), an id the
//     service does not have gets the dispatcher's own answer;
//   * the session closes like any fake one (and nwm::UDS and ndm:u next to it are not disturbed);
//   * game threads calling dlp:FKCL while others use nwm::UDS and ndm:u (lock order);
//   * 1.1.4 (G4): the prepared Download Play child's dlp:SRVR (SetDlpChild): IsChild TRUE, nothing else.
//
//   dlp_fkcl_test              (needs no room server; built by Tests/ipc_test/build.sh)
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_fkcl_hle.hpp"
#include "azl/log.hpp"

#include "hle_harness.hpp"

using namespace azl;

static std::vector<std::string> g_log;
static std::mutex g_log_mutex;
static int LogLines(const std::string& text) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    int n = 0;
    for (const std::string& line : g_log) {
        if (line.find(text) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// The three commands, with the headers the 3DS libraries and the games send.
static std::vector<u32> InitializeWithNameRequest() {
    // [0x00110203]: 8 normal words (shared memory size, scan number, the 24-byte user name, ...) + 3 translate words
    std::vector<u32> v = req::Make(0x11, 8, 3);
    v[1] = 0x30000;
    v[2] = 16;
    return v;
}
static std::vector<u32> InitializeRequest() {
    // [0x00010083]: 2 normal words + 3 translate words
    std::vector<u32> v = req::Make(0x01, 2, 3);
    v[1] = 0x30000;
    return v;
}

static void TestOffByDefault() {
    MockRoom room;
    HleConsole ds(room, 1);
    // The harness' Open() insists on an answer from us. Here the interception must DECLINE: the shim's srv then refuses
    // (on a console the REAL service answers), no fake session is made.
    g_test_hle = ds.hle.get();
    Handle h = 0;
    const Result rc = srvGetServiceHandle(&h, "dlp:FKCL");
    CHECK_EQ(static_cast<u32>(rc), 0xD8E06406u);
    CHECK_EQ(static_cast<u32>(h), 0u);
}

static void TestWirelessCheck() {
    MockRoom room;
    HleConsole ds(room, 1);
    ds.hle->SetDlpProbeFake(true);
    const int opened_before = LogLines("uds: game opened dlp:FKCL");
    const Handle h = ds.Open("dlp:FKCL");
    CHECK(h != 0);
    CHECK_EQ(LogLines("uds: game opened dlp:FKCL") - opened_before, 1);

    Reply r = ds.Call(h, InitializeWithNameRequest());
    CHECK_EQ(r.w[0], 0x00110040u);
    CHECK_EQ(r.w[1], 0u);
    CHECK_EQ(LogLines("dlp: dlp:FKCL is answered here"), 1);

    r = ds.Call(h, req::Make(0x02, 0, 0)); // Finalize [0x00020000]
    CHECK_EQ(r.w[0], 0x00020040u);
    CHECK_EQ(r.w[1], 0u);

    // Initialize twice (plain and with a name) and Finalize again: still successes (it is only a check)
    r = ds.Call(h, InitializeRequest());
    CHECK_EQ(r.w[0], 0x00010040u);
    CHECK_EQ(r.w[1], 0u);
    r = ds.Call(h, InitializeWithNameRequest());
    CHECK_EQ(r.w[1], 0u);
    CHECK_EQ(LogLines("dlp: dlp:FKCL is answered here"), 1); // (said once)
    r = ds.Call(h, req::Make(0x02, 0, 0));
    CHECK_EQ(r.w[1], 0u);
    ds.Close(h);
}

static void TestOtherCommandsAreRefused() {
    MockRoom room;
    HleConsole ds(room, 1);
    ds.hle->SetDlpProbeFake(true);
    const Handle h = ds.Open("dlp:FKCL");
    // StartScan [0x00050180], GetMyStatus [0x000C0000], StartFakeSession [0x000B0100], GetChannels [0x00040000],
    // and the last command, StopSession [0x00100000]: "invalid state", one normal word, the command's own id
    const struct {
        u32 id, normal;
    } cmds[] = {{0x05, 6}, {0x0C, 0}, {0x0B, 4}, {0x04, 0}, {0x10, 0}};
    int n = 0;
    for (const auto& c : cmds) {
        const Reply r = ds.Call(h, req::Make(c.id, c.normal, 0));
        CHECK_EQ(r.w[0], (c.id << 16) | 0x40u);
        CHECK_EQ(r.w[1], 0xE0A0AC01u);
        ++n;
    }
    // an id the service does not have (id 0 and 0x12): the dispatcher's own answer [0x40, 0xD9001830]
    for (u32 id : {0x12u, 0x7Fu}) {
        const Reply r = ds.Call(h, req::Make(id, 0, 0));
        CHECK_EQ(r.w[0], 0x40u);
        CHECK_EQ(r.w[1], 0xD9001830u);
    }
    // the first three refusals are in the log, the rest only counted
    CHECK_EQ(LogLines("dlp: dlp:FKCL command 0x"), 3);
    CHECK(LogLines("beyond the wireless check") >= 1);
    // a successful command still works after the refusals
    const Reply ok = ds.Call(h, InitializeWithNameRequest());
    CHECK_EQ(ok.w[1], 0u);
    ds.Close(h);
    (void)n;
}

static void TestSessionCloseAndNeighbours() {
    MockRoom room;
    HleConsole ds(room, 1);
    ds.hle->SetDlpProbeFake(true);
    const Handle dlp = ds.Open("dlp:FKCL");
    const Handle ndm = ds.Open("ndm:u");
    const Handle uds = ds.Open("nwm::UDS");
    CHECK(dlp != ndm && ndm != uds && dlp != uds);
    // each one answers its own commands
    Reply r = ds.Call(dlp, InitializeWithNameRequest());
    CHECK_EQ(r.w[1], 0u);
    r = ds.Call(ndm, req::Words(0x01, 1, 2)); // ndm EnterExclusiveState-like request: just that it is served by ndm
    CHECK(r.w[0] != 0);
    const int closed_before = LogLines("uds: game closed its fake dlp:FKCL session");
    ds.Close(dlp);
    CHECK_EQ(LogLines("uds: game closed its fake dlp:FKCL session") - closed_before, 1);
    // the two others are still ours
    r = ds.Call(ndm, req::Words(0x01, 1, 2));
    CHECK(r.w[0] != 0);
    ds.Close(ndm);
    ds.Close(uds);
}

static void TestThreads() {
    MockRoom room;
    HleConsole ds(room, 1);
    ds.hle->SetDlpProbeFake(true);
    const Handle dlp = ds.Open("dlp:FKCL");
    const Handle ndm = ds.Open("ndm:u");
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < 300; ++i) {
            const Reply r = ds.Call(dlp, InitializeWithNameRequest());
            if (r.w[1] != 0) {
                ++bad;
            }
            const Reply f = ds.Call(dlp, req::Make(0x02, 0, 0));
            if (f.w[1] != 0) {
                ++bad;
            }
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < 300; ++i) {
            const Handle extra = ds.Open("dlp:FKCL");
            ds.Close(extra);
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < 300; ++i) {
            const Reply r = ds.Call(ndm, req::Words(0x01, 1, 2));
            if (r.w[0] == 0) {
                ++bad;
            }
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    CHECK_EQ(bad.load(), 0);
    ds.Close(dlp);
    ds.Close(ndm);
}

static void TestHleClassAlone() {
    // The class on its own (no front-end): every command id, counts
    DlpFkclHle h;
    u32 cmd[64];
    for (u32 id = 0; id < 0x20; ++id) {
        std::memset(cmd, 0xA5, sizeof(cmd));
        cmd[0] = (id << 16) | 0x40;
        h.Dispatch(cmd);
        const bool ok = id == 0x01 || id == 0x02 || id == 0x11;
        const bool valid = id >= 1 && id <= 0x11;
        CHECK_EQ(cmd[1], ok ? 0u : valid ? 0xE0A0AC01u : 0xD9001830u);
        CHECK_EQ(cmd[0], valid ? ((id << 16) | 0x40u) : 0x40u);
    }
    CHECK_EQ(h.initializes(), 2u);
    CHECK_EQ(h.finalizes(), 1u);
    CHECK_EQ(h.refused(), 0x20u - 3u);
}

// 1.1.4 (G4): the Download Play child 3DS Link prepared (UdsHle::SetDlpChild, Sources/ctr/plugin.cpp contract K8). By default, and with
// SetDlpChild(false) or with a hub, dlp:SRVR is the real service. With it: a fake dlp:SRVR session whose IsChild [0x000E0040, pid] answers
// [0x000E0080, 0, 1] - the one request the child makes of it - and whose every other command is "invalid state" (written, the first few);
// dlp:FKCL stays the real one (the probe is off for this title); the session closes like any fake one beside nwm::UDS and ndm:u.
static void TestPreparedChild() {
    {
        MockRoom room;
        HleConsole ds(room, 1);
        g_test_hle = ds.hle.get();
        Handle h = 0;
        CHECK(!ds.hle->dlp_child());
        CHECK_EQ(static_cast<u32>(srvGetServiceHandle(&h, "dlp:SRVR")), 0xD8E06406u);
        ds.hle->SetDlpChild(false);
        CHECK(!ds.hle->dlp_child());
        CHECK_EQ(static_cast<u32>(srvGetServiceHandle(&h, "dlp:SRVR")), 0xD8E06406u);
        // a hub never says "child": with one, SetDlpChild makes nothing (the pointer is never followed here: no session is opened)
        static int not_a_hub = 0;
        ds.hle->SetDlpHub(reinterpret_cast<DlpHub*>(&not_a_hub));
        ds.hle->SetDlpChild(true);
        CHECK(!ds.hle->dlp_child());
        ds.hle->SetDlpHub(nullptr);
        CHECK_EQ(static_cast<u32>(h), 0u);
    }
    MockRoom room;
    HleConsole ds(room, 1);
    ds.hle->SetDlpChild(true);
    CHECK(ds.hle->dlp_child());
    ds.hle->SetDlpChild(true);  // (once: a second call changes nothing)
    CHECK(ds.hle->dlp_child());
    g_test_hle = ds.hle.get();
    Handle fkcl = 0;
    CHECK_EQ(static_cast<u32>(srvGetServiceHandle(&fkcl, "dlp:FKCL")), 0xD8E06406u);  // (the real dlp:FKCL)
    const int opened_before = LogLines("uds: game opened dlp:SRVR");
    const Handle srvr = ds.Open("dlp:SRVR");
    const Handle ndm = ds.Open("ndm:u");
    const Handle uds = ds.Open("nwm::UDS");
    CHECK(srvr != 0 && srvr != ndm && srvr != uds && ndm != uds);
    CHECK_EQ(LogLines("uds: game opened dlp:SRVR") - opened_before, 1);
    // IsChild, as the child sends it (one normal word: its process id): TRUE
    std::vector<u32> is_child = req::Make(0x0E, 1, 0);
    is_child[1] = 0x2A;
    Reply r = ds.Call(srvr, is_child);
    CHECK_EQ(r.w[0], 0x000E0080u);
    CHECK_EQ(r.w[1], 0u);
    CHECK_EQ(r.w[2], 1u);
    CHECK_EQ(LogLines("dlp: dlp:SRVR IsChild answered TRUE here"), 1);
    r = ds.Call(srvr, is_child);  // (asked again: the same answer, the line said once)
    CHECK_EQ(r.w[2], 1u);
    CHECK_EQ(LogLines("dlp: dlp:SRVR IsChild answered TRUE here"), 1);
    // anything else of the service: "invalid state", the first three written; an id it does not have: the dispatcher's own answer
    const int refused_before = LogLines("dlp: the prepared child called dlp:SRVR command 0x");
    for (u32 id : {0x01u, 0x0Fu, 0x03u, 0x10u}) {
        r = ds.Call(srvr, req::Make(id, 0, 0));
        CHECK_EQ(r.w[0], (id << 16) | 0x40u);
        CHECK_EQ(r.w[1], 0xE0A0AC01u);
    }
    r = ds.Call(srvr, req::Make(0x11, 0, 0));
    CHECK_EQ(r.w[0], 0x40u);
    CHECK_EQ(r.w[1], 0xD9001830u);
    CHECK_EQ(LogLines("dlp: the prepared child called dlp:SRVR command 0x") - refused_before, 3);
    CHECK_EQ(LogLines("dlp: the prepared child called dlp:SRVR command 0x01 (answered invalid state; 1 so far)"), 1);
    r = ds.Call(srvr, is_child);  // (and IsChild is still TRUE after them)
    CHECK_EQ(r.w[2], 1u);
    CHECK_EQ(LogLines("dlp: dlp:SRVR IsChild answered TRUE here"), 1);
    // the neighbours are still the plugin's
    r = ds.Call(ndm, req::Make(0x01, 1, 2));
    CHECK(r.w[0] != 0);
    const int closed_before = LogLines("uds: game closed its fake dlp:SRVR session");
    ds.Close(srvr);
    CHECK_EQ(LogLines("uds: game closed its fake dlp:SRVR session") - closed_before, 1);
    CHECK_EQ(LogLines("its role ends with it"), 0);  // (no hub: the child's session has no role to end)
    r = ds.Call(ndm, req::Make(0x01, 1, 2));
    CHECK(r.w[0] != 0);
    // opened again (a game may): served the same
    const Handle again = ds.Open("dlp:SRVR");
    r = ds.Call(again, is_child);
    CHECK_EQ(r.w[2], 1u);
    ds.Close(again);
    ds.Close(ndm);
    ds.Close(uds);
}

int main() {
    LogOn log_on; // (the tests read the lines of the log)
    SetLogSink([](const char* line) {
        std::lock_guard<std::mutex> l(g_log_mutex);
        g_log.push_back(line);
    });
    Watchdog dog(120);
    dog.Step("class alone");
    TestHleClassAlone();
    dog.Step("off by default");
    TestOffByDefault();
    dog.Step("wireless check");
    TestWirelessCheck();
    dog.Step("other commands");
    TestOtherCommandsAreRefused();
    dog.Step("close and neighbours");
    TestSessionCloseAndNeighbours();
    dog.Step("threads");
    TestThreads();
    dog.Step("prepared child");
    TestPreparedChild();
    dog.Step("");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
