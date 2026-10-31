// The fake Download Play lobby through the IPC front-end of the plugin (DESIGN-FASE3.md sez. 4 and 5.2): UdsHle (ctr/uds_hle.cpp) with a
// DlpHub (azl/dlp_hub.hpp) and the real UdsEngine behind it, TWO consoles in one process joined by the in-memory room of Tests/mock_room.hpp
// (so no room server is needed), driven the way the games drive them: raw IPC command words through HandleSendSyncRequest (the hook of
// svcSendSyncRequest), sessions opened through the intercepted srv:GetServiceHandle and closed through the hook of svcCloseHandle
// (NoteHandleClosed), on the fake kernel of Tests/ipc_test/shim_svc.cpp. Each console has a "network thread" that does what the plugin's
// RoomClient thread does every round (engine.Tick(now); hub.Tick(); sleep 3 ms). Real time throughout (the 8 s cap of the join's wait, the
// 2 s after which a request nobody serves is cancelled, the 1 s after which a network thread counts as gone).
//
// Everything is written out: the headers, the results and the states below are the numbers of SPEC-DLP.md and of the firmware (the
// constants of the code under test are not used to check it).
//
//   L1  GetServiceHandle. No hub and no probe: dlp:FKCL and dlp:SRVR are the REAL services (the hook declines). Probe only (Kirby Battle
//       Royale): dlp:FKCL is fake and answers as dlp_fkcl_test expects, dlp:SRVR is real, and a probe session closed after Initialize does
//       not shut the game's nwm::UDS down. With a hub: both are fake sessions (the probe, if also on, is not used).
//   L3  The gate. While the lobby holds the engine and the game has not initialised: every nwm::UDS command but 0x01 / 0x1B answers
//       kWrongState, Shutdown answers success and does NOT end the lobby, an id the service does not have gets the dispatcher's own
//       answer; then the game's Initialize (0x1B on the creator, 0x01 on a joined client) gives the engine back FIRST and the commands work.
//       And a loop that looks for the stale windows (the gate right after StartAccepting, the first command right after Initialize).
//   L2  Triple Deluxe by raw words between two consoles (creator SRVR, searcher FKCL) to state 9 on both, with the join's wait inside the
//       front-end; two game-side pollers of nwm::UDS at the same time (the gate under real concurrency); then the game's own nwm::UDS on the
//       same engines (a clean engine: the game's names, seeds and channel; data both ways), with the searcher still at 9 (holding the engine)
//       in one run and finalised in the other; a dlp session closed after the game's UDS is up does not touch it.
//   L4  Closing a dlp session handle: without Finalize it finalises the role (hub.OnSessionClosed) and frees the engine; a session that never
//       initialised does nothing; the closed value is forgotten; a handle found dead is ended too; nwm::UDS still shuts the engine down.
//   L5  No network thread: a posted command answers [(id << 16) | 0x40, 0xC940AC04] at once (but for the ones that succeed in every state,
//       L13), the inline ones answer, the ends of a session are left to the next Tick (deferred_closes) and only the give-back before the
//       game's Initialize runs on the calling thread; a request nobody serves is cancelled after 2 s; Stop(); a network thread that dies
//       while the lobby holds the engine.
//   L9  Four game threads on one console (inline reads, posted commands, nwm::UDS, sessions opened and closed): no deadlock, books that add up.
//   L6  A game that is not in the table (no hub): the dlp services are not faked and the nwm::UDS replies are word for word the replies of a
//       console with an idle hub; the idle hub is never called.
//   L8  Statistics: a hub-served dlp command is neither counted in g_stats.ipc_* nor throttled (a thousand kNoData in a row); the probe's and
//       nwm::UDS commands are accounted and throttled as always.
//   L7  The 8 s cap of the join's wait (last: it takes 8 s): a network thread that never ends the join -> 0xC880AC05 after about 8 s, the
//       client role finalised.
//   L10 / L11  Names that only begin like the two services are not taken; a session that only called Finalize ends nothing; the game's
//       nwm::UDS Initialize while the searcher only scans gives the scan back too.
//   L12 A nwm::UDS session that called Initialize (and Shutdown) closes while the lobby holds the engine - closed through the hook, and found
//       dead on its next call: the engine, the lobby's network and its accepting state stay (the close must not Shutdown the engine under the
//       hub); the control: the same close with no lobby holding the engine still shuts the service down.
//   L13 The commands that succeed in every state (FKCL Finalize / StopScan / StopSession, SRVR Finalize / EndAccepting: Triple Deluxe and Planet
//       Robobot treat a failing Finalize as fatal) when no network thread serves - never ticked, silent for more than a second, after
//       hub.Stop(), not taken in 2 s - come back [(id << 16) | 0x40, 0] with NO "wireless off" trouble line, while a command outside the set
//       (StartAccepting, StartScan) comes back 0xC940AC04 WITH the line; the roles they end are ended by the next Tick (the engine is free, the
//       nwm::UDS gate is off). Runs in a fresh process (this binary again, "--l13"): the trouble lines are the first eight events of a process and
//       one in 64, so there every event has its line and a missing one is the code's.
//   L14 The table of the first-call lines of the log (NoteCommand): a game WITHOUT a hub remembers 48 distinct (command, result) pairs of
//       nwm::UDS / ndm:u / the probe's dlp:FKCL and is silent about the 49th and later (as before the hub); the hub's two services have 32 of
//       their own; neither table starves the other (both orders). The table is the process's: each order runs in a fresh process (this binary
//       again, "--l14 <order>"), so the numbers are exact.
//   L15 The trace of the hub's dlp commands (the "dlpc: " lines, debug only; what the console test of phase 4 is read from), in six fresh
//       processes ("--l15 off|td|flood|alt|refused|probe"; the slots of the trace are the process's; exit 77 = a log line was given up for the
//       lock, or a timed part was too slow for its 2 s window: the run is void and is made again): off = the whole Triple Deluxe sequence at
//       debug level 0 writes not one line and the trace remembers nothing; td = the sequence at level 1, every line against the journal of
//       the games' calls (words of the request taken before the reply, result, reply words, duration of the join, order, the counts of the
//       left-out calls add up, the states 1-2-6-8-9-0 and 5-6-9, the ids from 0x12 on are silent) and the first-command line of each service
//       (once, with the thread and its priority); flood = 5000 polls with an unchanged reply are one line, each change is a line at its call
//       with the right count, a return to a reply the window holds is counted (the intended limit), the same reply is written again after
//       2 s and not before; alt = a command that alternates between replies (a group and "no data"; four groups and "no data" in turn)
//       writes one line per distinct reply, a new reply is a line at once, the window ends after 2 s and the next call says how many it left
//       out, a seventh distinct reply in a window is written every time, replies that differ in the result alone are lines; refused = a
//       command nobody served (0xC940AC04) and an unserved Finalize (0) are lines with their results; probe = no line for the probe of
//       Battle Royale, a game without a hub, nwm::UDS or ndm:u.
//
//   dlp_lobby_hle_test         (needs no room server; built by Tests/ipc_test/build.sh)
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "azl/dlp_hub.hpp"
#include "azl/log.hpp"

#include "../hle_harness.hpp"

using namespace azl;

extern std::atomic<unsigned long long> g_test_sleep_calls; // shim_svc.cpp: every svcSleepThread, and their sum
extern std::atomic<unsigned long long> g_test_sleep_ns;
extern std::atomic<bool> g_test_real_sleep;

// ---------------------------------------------------------------------------------------------------------------------
// Checks that threads may use (support.hpp's counters are plain ints).

static std::atomic<int> g_tchecks{0};
static std::atomic<int> g_tfails{0};
#define TCHECK(cond)                                                                                  \
    do {                                                                                              \
        g_tchecks.fetch_add(1);                                                                       \
        if (!(cond)) {                                                                                \
            g_tfails.fetch_add(1);                                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                                             \
    } while (0)
#define TCHECK_EQ(a, b)                                                                               \
    do {                                                                                              \
        g_tchecks.fetch_add(1);                                                                       \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                            \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                            \
        if (va_ != vb_) {                                                                             \
            g_tfails.fetch_add(1);                                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                             \
    } while (0)

// Results, headers and states, written out.
static constexpr u32 R_WIRELESS_OFF = 0xC940AC04;       // what a request that cannot be served says (the DLP module's "wireless off")
static constexpr u32 R_INVALID_STATE = 0xE0A0AC01;
static constexpr u32 R_NO_DATA = 0xC880AFEF;
static constexpr u32 R_NOT_FOUND = 0xC880AC05;          // "server not found"
static constexpr u32 R_BAD_HEADER = 0xD9001830;         // an id dlp:FKCL / dlp:SRVR does not have
static constexpr u32 R_WRONG_STATE = 0xC8A113EA;        // nwm::UDS before Initialize
static constexpr u32 R_UNKNOWN_UDS_COMMAND = 0xD900182F;
static constexpr u32 R_SRV_REFUSED = 0xD8E06406;        // what the shim's srv: says when the hook declines (on a console the REAL service answers)
static constexpr u64 kFilterTd = 0x000400010010C000ull; // Triple Deluxe's scan filter (child index 0)
static const u8 kNine[9] = {'1', 'a', '2', 'b', '3', 'c', '4', 'd', 0};

static u32 Plain(u32 id) {
    return (id << 16) | 0x40;
}

template <class Pred>
static bool WaitFor(u32 ms, Pred pred) {
    const u64 t0 = platform::NowMs();
    do {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (platform::NowMs() - t0 < ms);
    return pred();
}

static u32 Ms(u64 since) {
    return static_cast<u32>(platform::NowMs() - since);
}

// ---------------------------------------------------------------------------------------------------------------------
// The log, as the plugin writes it.

static std::vector<std::string> g_log;
static std::mutex g_log_mutex;
static size_t LogSize() {
    std::lock_guard<std::mutex> l(g_log_mutex);
    return g_log.size();
}
/// The lines from index `from` on that contain `text`.
static int LogCount(const std::string& text, size_t from = 0) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    int n = 0;
    for (size_t i = from; i < g_log.size(); ++i) {
        if (g_log[i].find(text) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// The trouble lines of the hub's side ("answered "wireless off"": a command the network side did not run) are the first 8 and then one in 64
// of ALL such events of the process: the test counts the events it makes and checks the lines at the end.
static std::atomic<unsigned> g_trouble_events{0};
static void Trouble(unsigned n) {
    g_trouble_events.fetch_add(n);
}
static int ExpectedTroubleLines(unsigned events) {
    int n = 0;
    for (unsigned e = 1; e <= events; ++e) {
        if (e <= 8 || e % 64 == 0) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// A console of the plugin: the engine on the mock room + UdsHle (HleConsole) + a hub (or not) + the network thread (or not).

/// What RoomClient's thread does every round: the engine's Tick, then the hub's.
class NetThread {
public:
    NetThread(UdsEngine* engine, DlpHub* hub) : engine_(engine), hub_(hub), thread_([this] { Run(); }) {}
    ~NetThread() {
        Stop();
    }
    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
        }
    }
    void Pause(bool on) {
        paused_ = on;
    }
    u64 rounds() const {
        return rounds_;
    }

private:
    void Run() {
        while (!stop_) {
            if (!paused_) {
                engine_->Tick(platform::NowMs());
                if (hub_ != nullptr) {
                    hub_->Tick();
                }
                ++rounds_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    UdsEngine* engine_;
    DlpHub* hub_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<u64> rounds_{0};
    std::thread thread_;
};

/// The kernel the hub sees: the console's, and a pause after every SignalEvent. The hub signals its event to wake the game thread that
/// waits for its answer; the pause keeps the network thread where it is - after the answer, before the rest of its round - long enough for
/// the game thread to run its next command in that window (the hints the gate reads must be fresh by then: DlpHub::Tick refreshes them
/// BEFORE it publishes the answer).
class SlowSignalKernel final : public KernelOps {
public:
    explicit SlowSignalKernel(KernelOps* inner) : inner_(inner) {}
    u32 CreateEvent() override { return inner_->CreateEvent(); }
    u32 DuplicateForGame(u32 h) override { return inner_->DuplicateForGame(h); }
    void SignalEvent(u32 h) override {
        inner_->SignalEvent(h);
        const u32 ms = delay_ms.load();
        if (ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    void ClearEvent(u32 h) override { inner_->ClearEvent(h); }
    bool WaitEvent(u32 h, s64 ns) override { return inner_->WaitEvent(h, ns); }
    void CloseEvent(u32 h) override { inner_->CloseEvent(h); }
    void SleepNs(s64 ns) override { inner_->SleepNs(ns); }
    std::atomic<u32> delay_ms{0};

private:
    KernelOps* inner_;
};

/// The IpcResolver of the test: an address the game put in a request is the pointer itself (32-bit build); null and the first page are refused.
static u8* MapAddress(void*, u32 address, u32) {
    return address >= 0x1000 ? reinterpret_cast<u8*>(address) : nullptr;
}

static LobbyConfig MakeCfg(u8 last, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = 0x000400000010C000ull;
    c.friend_seed = 0xF000u + last;
    for (u32 i = 0; i < kNameUnits && name[i]; ++i) {
        c.name[i] = static_cast<u16>(static_cast<u8>(name[i]));
    }
    return c;
}

struct Lc {
    Lc(MockRoom& room, u8 last, const char* name, bool hub_on, bool net_on, bool probe = false) : ds(room, last), hub_kernel(&ds.kernel) {
        ds.hle->SetDlpProbeFake(probe);
        if (hub_on) {
            IpcResolver resolver;
            resolver.ctx = nullptr;
            resolver.map = &MapAddress;
            hub.reset(new DlpHub(ds.console.engine.get(), &hub_kernel, MakeCfg(last, name), resolver));
            ds.hle->SetDlpHub(hub.get());
        }
        if (net_on) {
            StartNet();
        }
    }
    void StartNet() {
        net.reset(new NetThread(ds.console.engine.get(), hub.get()));
        WaitFor(2000, [&] { return net->rounds() >= 2; });  // (the plugin's thread has ticked before a game can ask)
    }
    void StopNet() {
        net.reset();
    }
    UdsEngine& e() {
        return *ds.console.engine;
    }
    const Mac& mac() const {
        return ds.console.mac;
    }
    /// srv:GetServiceHandle through the hook: the fake session, or 0 when the hook declines (`*rc` is then what the shim's srv: said).
    Handle Open(const char* name, Result* rc = nullptr) {
        g_test_hle = ds.hle.get();
        Handle h = 0;
        const Result r = srvGetServiceHandle(&h, name);
        if (rc != nullptr) {
            *rc = r;
        }
        return R_SUCCEEDED(r) ? h : 0;
    }
    /// The hook of svcCloseHandle, then the kernel.
    void Close(Handle h) {
        ds.hle->NoteHandleClosed(h);
        svcCloseHandle(h);
    }

    HleConsole ds;
    SlowSignalKernel hub_kernel;  // (what the hub's kernel calls go through: the console's kernel)
    std::unique_ptr<DlpHub> hub;
    std::unique_ptr<NetThread> net;  // (declared last: destroyed first)
};

// ---------------------------------------------------------------------------------------------------------------------
// One request through the front-end, on the calling thread. The words after the request are stale (0xA5A5A5A5, like a real TLS); `strict`
// asks that the reply wrote nothing beyond what its own header declares.

struct Rep {
    u32 w[64];
    bool handled = false;
    u32 hdr() const {
        return w[0];
    }
    u32 res() const {
        return w[1];
    }
};

static Rep Do(Lc& c, Handle h, const std::vector<u32>& request, void* out = nullptr, u32 out_size = 0, bool strict = false,
              u32 stale = 0xA5A5A5A5) {
    u32* cmd = getThreadCommandBuffer();
    for (u32 i = 0; i < 64; ++i) {
        cmd[i] = stale;
    }
    std::memcpy(cmd, request.data(), request.size() * sizeof(u32));
    u32 before[64];
    std::memcpy(before, cmd, sizeof(before));
    u32* sb = getThreadStaticBuffers();
    sb[0] = IPC_Desc_StaticBuffer(out_size, 0);
    sb[1] = req::Ptr(out);
    Result rc = -1;
    Rep r;
    r.handled = c.ds.hle->HandleSendSyncRequest(h, &rc);
    std::memcpy(r.w, cmd, sizeof(r.w));
    if (r.handled) {
        TCHECK_EQ(static_cast<u32>(rc), 0u);
        if (strict) {
            const u32 extent = std::min<u32>(1 + ((r.w[0] >> 6) & 0x3F) + (r.w[0] & 0x3F), 64);
            for (u32 k = extent; k < 64; ++k) {
                TCHECK(cmd[k] == before[k]);
            }
        }
    }
    return r;
}

static std::vector<u32> Words(u32 header, std::initializer_list<u32> args) {
    std::vector<u32> v;
    v.push_back(header);
    v.insert(v.end(), args.begin(), args.end());
    return v;
}

static u32 MacW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 MacW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}
static void NameWords(const char* name, u32 w[5]) {
    u16 u[10] = {};
    for (int i = 0; i < 10 && name[i]; ++i) {
        u[i] = static_cast<u16>(static_cast<u8>(name[i]));
    }
    for (int i = 0; i < 5; ++i) {
        w[i] = static_cast<u32>(u[2 * i]) | (static_cast<u32>(u[2 * i + 1]) << 16);
    }
}
static void PassWords(const u8* b, u32 w[3]) {
    w[0] = static_cast<u32>(b[0]) | (static_cast<u32>(b[1]) << 8) | (static_cast<u32>(b[2]) << 16) | (static_cast<u32>(b[3]) << 24);
    w[1] = static_cast<u32>(b[4]) | (static_cast<u32>(b[5]) << 8) | (static_cast<u32>(b[6]) << 16) | (static_cast<u32>(b[7]) << 24);
    w[2] = static_cast<u32>(b[8]);
}
static std::string NameAt(const u8* p) {  // 10 UTF-16 units, little endian
    std::string s;
    for (u32 i = 0; i < 10; ++i) {
        const u16 u = LoadLE16(p + 2 * i);
        if (u == 0) {
            break;
        }
        s += static_cast<char>(u);
    }
    return s;
}
static std::string NameOf(const u16* n) {
    std::string s;
    for (u32 i = 0; i < 10 && n[i]; ++i) {
        s += static_cast<char>(n[i]);
    }
    return s;
}
/// The 10 words of a DLPNodeInfo in a reply, from word `at`.
static void CheckNodeRecord(const Rep& r, u32 at, u64 seed, const char* name, u32 node) {
    u8 rec[40];
    std::memcpy(rec, &r.w[at], sizeof(rec));
    TCHECK_EQ(LoadLE64(rec), seed);
    TCHECK(NameAt(rec + 0x0C) == name);
    TCHECK_EQ(LoadLE32(rec + 0x24), node);
}

// ---------------------------------------------------------------------------------------------------------------------
// The game's side of the Download Play: the commands as Triple Deluxe sends them (SPEC-DLP.md sez. 2 and 3), by session handle.

/// What the game side saw of one command of a Game that keeps a journal (L15 compares the journal with the "dlpc: " lines of the log): the
/// request as the game sent it (six words after the header, 0xA5A5A5A5 where the header declares a word the game did not set, 0 beyond what
/// the header declares), the reply as the game got it (the result and the words after it that the reply header declares, five at the most)
/// and the clock before and after the call.
struct JCall {
    bool srvr = false;
    u32 id = 0;
    u32 req[6] = {};
    u32 result = 0;
    u32 rep[5] = {};
    u32 rep_n = 0;
    u32 t0 = 0, t1 = 0;
};

/// The number of words after the header that an IPC header declares: the normal words (bits 6-11) and the translate words (bits 0-5).
static u32 DeclaredWordsOf(u32 header) {
    return std::min<u32>(((header >> 6) & 0x3F) + (header & 0x3F), 63);
}

/// The same reply for the trace's purposes: the result and the words of the reply the line shows.
static bool SameReply(const JCall& a, const JCall& b) {
    if (a.result != b.result || a.rep_n != b.rep_n) {
        return false;
    }
    for (u32 i = 0; i < a.rep_n; ++i) {
        if (a.rep[i] != b.rep[i]) {
            return false;
        }
    }
    return true;
}

class Game {
public:
    Game(Lc& c, bool open_fk, bool open_sv) : c_(c), title_(kTitleInfoSize, 0xCC), server_(kServerInfoSize, 0xCC), ids_(16, 0xCC), cli_(16, 0xCC) {
        if (open_fk) {
            fk_ = c.Open("dlp:FKCL");
            TCHECK(fk_ != 0);
        }
        if (open_sv) {
            sv_ = c.Open("dlp:SRVR");
            TCHECK(sv_ != 0);
        }
    }
    Handle fk() const {
        return fk_;
    }
    Handle sv() const {
        return sv_;
    }

    Rep Fk(u32 header, std::initializer_list<u32> args, std::vector<u8>* out = nullptr) {
        const std::vector<u32> request = Words(header, args);
        const u32 t0 = static_cast<u32>(platform::NowMs());
        const Rep r = Do(c_, fk_, request, out ? out->data() : nullptr, out ? static_cast<u32>(out->size()) : 0, true);
        Note(false, request, r, t0);
        return r;
    }
    Rep Sv(u32 header, std::initializer_list<u32> args) {
        const std::vector<u32> request = Words(header, args);
        const u32 t0 = static_cast<u32>(platform::NowMs());
        const Rep r = Do(c_, sv_, request, nullptr, 0, true);
        Note(true, request, r, t0);
        return r;
    }
    /// When set, every command of Fk() / Sv() is written down here (one thread per Game: the vector is not locked).
    std::vector<JCall>* journal = nullptr;

    // ---- dlp:FKCL
    Rep FkInit(u32 beacons = 16) { return Fk(0x00010083, {0x30000, beacons, 0x04000000, 0x1111, 0x2222}); }
    Rep FkInitName(const char* name, u32 beacons = 16) {
        u32 n[5];
        NameWords(name, n);
        return Fk(0x00110203, {0x30000, beacons, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    Rep FkFinalize() { return Fk(0x00020000, {}); }
    Rep FkChannels() { return Fk(0x00040000, {}); }
    Rep FkScan(u64 filter = kFilterTd) {
        return Fk(0x00050180, {0x0421, static_cast<u32>(filter), static_cast<u32>(filter >> 32), 0, 0, 0});
    }
    Rep FkStopScan() { return Fk(0x00060000, {}); }
    Rep FkServerInfo(const Mac& m) { return Fk(0x00070080, {MacW1(m), MacW2(m)}, &server_); }
    Rep FkTitleInfo(const Mac& m) { return Fk(0x00080100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}, &title_); }
    Rep FkInOrder(u32 restart) { return Fk(0x00090040, {restart}, &title_); }
    /// GetTitleInfoInOrder into a buffer of the caller's: the reply echoes the address of the game's buffer, so a call into another buffer is
    /// another reply (L15's alternation part: Triple Deluxe reads each group into a slot of its own).
    Rep FkInOrderInto(u32 restart, u8* buf) {
        const std::vector<u32> request = Words(0x00090040, {restart});
        const u32 t0 = static_cast<u32>(platform::NowMs());
        const Rep r = Do(c_, fk_, request, buf, kTitleInfoSize, true);
        Note(false, request, r, t0);
        return r;
    }
    /// StartFakeSession: the front-end waits for the join (the hub's PollPending loop); the reply is the join's outcome.
    Rep FkJoin(const Mac& m) { return Fk(0x000B0100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}); }
    Rep FkStatus() { return Fk(0x000C0000, {}); }
    Rep FkNodes(u32 n) { return Fk(0x000D0040, {n}, &ids_); }
    Rep FkNodeInfo(u32 node) { return Fk(0x000E0040, {node}); }
    Rep FkPass() { return Fk(0x000F0000, {}); }
    Rep FkStopSession() { return Fk(0x00100000, {}); }

    // ---- dlp:SRVR
    Rep SvInit(u32 max_clients, u32 child_index = 0) {
        return Sv(0x00010183, {0x85000, max_clients, 0xE, child_index, 0x40000, 2, 0x04000000, 0x5555, 0x6666});
    }
    Rep SvInitName(u32 max_clients, const char* name) {
        u32 n[5];
        NameWords(name, n);
        return Sv(0x000F0303, {0x85000, max_clients, 0xE, 0, 0x40000, 2, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x7777, 0x8888});
    }
    Rep SvFinalize() { return Sv(0x00020000, {}); }
    Rep SvState() { return Sv(0x00030000, {}); }
    Rep SvAccepting(u32 manual, u32 channel) { return Sv(0x00050080, {manual, channel}); }
    Rep SvEnd() { return Sv(0x00060000, {}); }
    Rep SvDistribute() { return Sv(0x00070000, {}); }
    Rep SvPass(const u8* nine) {
        u32 w[3];
        PassWords(nine, w);
        return Sv(0x000800C0, {w[0], w[1], w[2]});
    }
    Rep SvAccept(u32 node) { return Sv(0x00090040, {node}); }
    Rep SvDisconnect(u32 node) { return Sv(0x000A0040, {node}); }
    Rep SvClients(u32 n, u32 bytes) { return Sv(0x000B0042, {n, (bytes << 4) | 0xC, req::Ptr(cli_.data())}); }
    Rep SvClientInfo(u32 node) { return Sv(0x000C0040, {node}); }
    Rep SvClientState(u32 node) { return Sv(0x000D0040, {node}); }
    Rep SvIsChild() { return Sv(0x000E0040, {0xE}); }
    Rep SvDup() { return Sv(0x00100000, {}); }

    // ---- what the game reads in its buffers
    u32 CliPtr() { return req::Ptr(cli_.data()); }
    const u8* TitleRec() { return title_.data(); }
    const u8* ServerRec() { return server_.data(); }
    u16 NodeId(u32 i) { return LoadLE16(ids_.data() + 2 * i); }
    u16 ClientId(u32 i) { return LoadLE16(cli_.data() + 2 * i); }
    u32 StatusState() { return FkStatus().w[2] >> 24; }
    u32 ServerState() { return SvState().w[2]; }
    u32 ClientsTotal() {
        const Rep r = SvClients(4, 8);
        return r.res() == 0 ? r.w[2] : 0xFFFFFFFFu;
    }
    u32 NodesTotal() {
        const Rep r = FkNodes(4);
        return r.res() == 0 ? r.w[2] : 0xFFFFFFFFu;
    }

private:
    void Note(bool srvr, const std::vector<u32>& request, const Rep& r, u32 t0) {
        if (journal == nullptr) {
            return;
        }
        JCall c;
        c.srvr = srvr;
        c.id = request[0] >> 16;
        const u32 declared = DeclaredWordsOf(request[0]);
        for (u32 i = 0; i < 6; ++i) {
            c.req[i] = i >= declared ? 0u : (1 + i < request.size() ? request[1 + i] : 0xA5A5A5A5u);
        }
        c.result = r.res();
        const u32 words = std::min<u32>(DeclaredWordsOf(r.hdr()), 6);
        c.rep_n = words > 1 ? words - 1 : 0;
        for (u32 i = 0; i < c.rep_n; ++i) {
            c.rep[i] = r.w[2 + i];
        }
        c.t0 = t0;
        c.t1 = static_cast<u32>(platform::NowMs());
        journal->push_back(c);
    }

    Lc& c_;
    Handle fk_ = 0;
    Handle sv_ = 0;
    std::vector<u8> title_, server_, ids_, cli_;
};

/// The creator as Triple Deluxe opens a lobby (Initialize, GetDupNoticeNeed, StartAccepting(0, channel)).
static void OpenCreator(Game& g, u32 max_clients = 3, u32 channel = 6) {
    Rep r = g.SvInit(max_clients);
    TCHECK_EQ(r.hdr(), 0x00010040u);
    TCHECK_EQ(r.res(), 0u);
    r = g.SvDup();
    TCHECK_EQ(r.res(), 0u);
    r = g.SvAccepting(0, channel);
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
}
/// The searcher up to the group in its list: Initialize, GetChannels, StartScan, then GetTitleInfoInOrder until the group of `host` shows.
static bool FindGroup(Game& g, const Mac& host, u32 ms = 8000) {
    Rep r = g.FkInitName("Bob", 4);
    TCHECK_EQ(r.res(), 0u);
    r = g.FkChannels();
    TCHECK_EQ(r.res(), 0u);
    r = g.FkScan();
    TCHECK_EQ(r.res(), 0u);
    return WaitFor(ms, [&] {
        const Rep a = g.FkInOrder(1);
        if (a.res() != 0) {
            return false;
        }
        return std::memcmp(g.TitleRec() + kTitleInfoMac, host.data(), 6) == 0;
    });
}

// The game's nwm::UDS commands.
static Rep UdsInit(Lc& c, Handle uds, u64 seed, const char* name, bool deprecated = false) {
    const NodeInfo node = MakeNode(seed, name, 0);
    if (!deprecated) {
        return Do(c, uds, req::Initialize(node, kTestSharedMemory, kTestVersion));
    }
    std::vector<u32> v = req::Make(0x01, 11, 2);  // the deprecated Initialize: [1] the block size, [2..11] the node info
    v[1] = kTestSharedMemory;
    node.Store(reinterpret_cast<u8*>(&v[2]));
    v[12] = IPC_Desc_SharedHandles(1);
    v[13] = 0x00130003;
    return Do(c, uds, v);
}
static Rep UdsStatus(Lc& c, Handle uds) {
    return Do(c, uds, req::GetConnectionStatus());
}
static NodeInfo UdsNode(Lc& c, Handle uds, u16 node, u32* result) {
    const Rep r = Do(c, uds, req::GetNodeInformation(node));
    *result = r.res();
    NodeInfo info;
    u8 raw[40];
    std::memcpy(raw, &r.w[2], sizeof(raw));
    info.Load(raw);
    return info;
}

// ---------------------------------------------------------------------------------------------------------------------
// A game thread that polls nwm::UDS (GetConnectionStatus) all the time, as the games' event threads do. While the lobby holds the engine
// the answer is kWrongState, while it does not it is a success; a poll across a change (the flag differs before and after) is not judged.

class UdsPoller {
public:
    UdsPoller(Lc& c, Handle uds) : c_(c), uds_(uds), thread_([this] { Run(); }) {}
    ~UdsPoller() {
        Stop();
    }
    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
        }
    }
    u32 polls() const { return polls_; }
    u32 gated() const { return gated_; }   // polls judged "held" that got kWrongState
    u32 passed() const { return passed_; }  // polls judged "free" that got a success
    u32 bad() const { return bad_; }

private:
    void Run() {
        while (!stop_) {
            const bool before = c_.hub->EngineHeld();
            const Rep r = UdsStatus(c_, uds_);
            const bool after = c_.hub->EngineHeld();
            ++polls_;
            if (before && after) {
                if (r.res() == R_WRONG_STATE) {
                    ++gated_;
                } else {
                    ++bad_;
                }
            } else if (!before && !after) {
                if (r.res() == 0) {
                    ++passed_;
                } else {
                    ++bad_;
                }
            }
            // (about 300 polls a second: well under the 50 failing replies in 100 ms after which the front-end throttles a thread)
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    Lc& c_;
    Handle uds_;
    std::atomic<bool> stop_{false};
    std::atomic<u32> polls_{0}, gated_{0}, passed_{0}, bad_{0};
    std::thread thread_;
};

// ---------------------------------------------------------------------------------------------------------------------
// L1: GetServiceHandle.

static std::vector<u32> InitNameRequest() {  // [0x00110203]: 8 normal words + 3 translate words
    std::vector<u32> v = req::Make(0x11, 8, 3);
    v[1] = 0x30000;
    v[2] = 16;
    return v;
}
static std::vector<u32> InitRequest() {  // [0x00010083]
    std::vector<u32> v = req::Make(0x01, 2, 3);
    v[1] = 0x30000;
    return v;
}

static void L1() {
    std::printf("L1: GetServiceHandle: the real services without a hub, the probe's FKCL only, both fake with a hub\n");
    // (a) no hub, no probe: both dlp services are the real ones; nwm::UDS and ndm:u are still fake
    {
        g_rules.Reset();
        MockRoom room;
        Lc c(room, 1, "Alice", false, false);
        const size_t mark = LogSize();
        Result rc = 0;
        TCHECK_EQ(c.Open("dlp:FKCL", &rc), 0u);
        TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
        TCHECK_EQ(c.Open("dlp:SRVR", &rc), 0u);
        TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
        TCHECK_EQ(c.Open("fs:USER", &rc), 0u);
        TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
        const Handle uds = c.Open("nwm::UDS", &rc);
        TCHECK(uds != 0);
        TCHECK_EQ(static_cast<u32>(rc), 0u);
        const Handle ndm = c.Open("ndm:u", &rc);
        TCHECK(ndm != 0);
        TCHECK(uds != ndm);
        TCHECK_EQ(LogCount("uds: game opened dlp:", mark), 0);
        TCHECK_EQ(LogCount("uds: game opened nwm::UDS", mark), 1);
        c.Close(uds);
        c.Close(ndm);
    }
    // (b) the probe only (Kirby Battle Royale): dlp:FKCL fake and answered as dlp_fkcl_test expects, dlp:SRVR real
    {
        g_rules.Reset();
        MockRoom room;
        Lc c(room, 1, "Alice", false, false, true);
        DebugLines debug;  // the debug capture: the probe's commands write no line of it (as before the hub), nwm::UDS ones do
        const size_t mark = LogSize();
        Result rc = 0;
        const Handle uds = c.Open("nwm::UDS");
        TCHECK_EQ(Do(c, uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)).res(), 0u);
        TCHECK_EQ(debug.Count("uds cmd 0x1b"), 1);
        TCHECK(c.e().initialized());
        TCHECK_EQ(c.Open("dlp:SRVR", &rc), 0u);
        TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
        TCHECK_EQ(LogCount("uds: game opened dlp:SRVR", mark), 0);
        const Handle fk = c.Open("dlp:FKCL", &rc);
        TCHECK(fk != 0);
        TCHECK_EQ(static_cast<u32>(rc), 0u);
        TCHECK_EQ(LogCount("uds: game opened dlp:FKCL", mark), 1);
        Rep r = Do(c, fk, InitNameRequest());
        TCHECK_EQ(r.hdr(), 0x00110040u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(LogCount("dlp: dlp:FKCL is answered here", mark), 1);
        r = Do(c, fk, req::Make(0x02, 0, 0));  // Finalize
        TCHECK_EQ(r.hdr(), 0x00020040u);
        TCHECK_EQ(r.res(), 0u);
        r = Do(c, fk, InitRequest());
        TCHECK_EQ(r.hdr(), 0x00010040u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(LogCount("dlp: dlp:FKCL is answered here", mark), 1);  // (said once)
        // every other command of the service is "invalid state"; an id the service does not have is the dispatcher's own answer
        const struct {
            u32 id, normal;
        } cmds[] = {{0x05, 6}, {0x0C, 0}, {0x0B, 4}, {0x04, 0}, {0x10, 0}};
        for (const auto& cmd : cmds) {
            r = Do(c, fk, req::Make(cmd.id, cmd.normal, 0));
            TCHECK_EQ(r.hdr(), (cmd.id << 16) | 0x40u);
            TCHECK_EQ(r.res(), R_INVALID_STATE);
        }
        for (u32 id : {0x12u, 0x7Fu}) {
            r = Do(c, fk, req::Make(id, 0, 0));
            TCHECK_EQ(r.hdr(), 0x40u);
            TCHECK_EQ(r.res(), R_BAD_HEADER);
        }
        TCHECK_EQ(LogCount("dlp: dlp:FKCL command 0x", mark), 3);  // (the first three refusals are in the log, the rest only counted)
        TCHECK(LogCount("beyond the wireless check", mark) >= 1);
        TCHECK_EQ(Do(c, fk, InitNameRequest()).res(), 0u);  // a successful command still works after the refusals
        TCHECK_EQ(debug.Count("ndm cmd 0x"), 0);
        TCHECK_EQ(debug.Count("uds cmd 0x"), 1);  // (the game's Initialize only)
        // The probe's session closed after Initialize ends nothing: the game's own UDS stays up (there is no hub to end a role in)
        c.Close(fk);
        TCHECK(c.e().initialized());
        TCHECK_EQ(Do(c, uds, req::GetConnectionStatus()).res(), 0u);
        TCHECK_EQ(LogCount("dlp: that dlp:FKCL session had called Initialize", mark), 0);
        TCHECK_EQ(LogCount("the service shuts down with it", mark), 0);
        // a probe session opened and closed without a call, next to the others
        const Handle fk2 = c.Open("dlp:FKCL");
        c.Close(fk2);
        TCHECK(c.e().initialized());
        TCHECK_EQ(Do(c, uds, req::Shutdown()).res(), 0u);
        c.Close(uds);
    }
    // (c) a hub (the probe also on, in the second console): BOTH are fake sessions; the hub serves them
    for (int variant = 0; variant < 2; ++variant) {
        g_rules.Reset();
        MockRoom room;
        Lc c(room, 1, "Alice", true, true, variant == 1);
        std::unique_ptr<DebugLines> debug;  // the debug capture, to see what it does NOT write
        if (variant == 0) {
            debug.reset(new DebugLines);
        }
        const size_t mark = LogSize();
        Result rc = 0;
        const Handle fk = c.Open("dlp:FKCL", &rc);
        TCHECK(fk != 0);
        TCHECK_EQ(static_cast<u32>(rc), 0u);
        const Handle sv = c.Open("dlp:SRVR", &rc);
        TCHECK(sv != 0);
        TCHECK_EQ(static_cast<u32>(rc), 0u);
        TCHECK(fk != sv);
        TCHECK_EQ(LogCount("uds: game opened dlp:FKCL -> fake session", mark), 1);
        TCHECK_EQ(LogCount("uds: game opened dlp:SRVR -> fake session", mark), 1);
        // The first command of each service (the first of the process: said once). The probe would say "invalid state" to GetChannels and
        // is not used, the hub answers: [0x00040080, 0, 0x0421]; IsChild: [0x000E0080, 0, 0].
        Rep r = Do(c, fk, req::Make(0x04, 0, 0), nullptr, 0, true);
        TCHECK_EQ(r.hdr(), 0x00040080u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(r.w[2], 0x0421u);
        r = Do(c, sv, req::Make(0x0E, 1, 0), nullptr, 0, true);
        TCHECK_EQ(r.hdr(), 0x000E0080u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(r.w[2], 0u);
        TCHECK_EQ(LogCount("dlp: dlp:FKCL is answered here", mark), 0);  // (not the probe's line)
        if (variant == 0) {
            // (the line names the thread that made the call - decimal - and its priority - hex; the fake kernel of shim_svc.cpp answers
            //  0x1234 = 4660 and 0x30: the numbers are written out here)
            TCHECK_EQ(LogCount("dlp: the first command of dlp:FKCL is 0x04: the Download Play lobby is carried by the room (thread 4660, priority 0x30)"), 1);
            TCHECK_EQ(LogCount("dlp: the first command of dlp:SRVR is 0x0e: the Download Play lobby is carried by the room (thread 4660, priority 0x30)"), 1);
            TCHECK_EQ(LogCount("dlp:FKCL: command 0x04 -> 0x00000000 (first call)"), 1);
            TCHECK_EQ(LogCount("dlp:SRVR: command 0x0e -> 0x00000000 (first call)"), 1);
        }
        // Initialize on both (posted commands: the network thread runs them): the same id and result on the two services keep
        // their own first-call lines (a per-service salt)
        r = Do(c, fk, Words(0x00010083, {0x30000, 4, 0x04000000, 0x1111, 0x2222}), nullptr, 0, true);
        TCHECK_EQ(r.hdr(), 0x00010040u);
        TCHECK_EQ(r.res(), 0u);
        r = Do(c, sv, Words(0x00010183, {0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x5555, 0x6666}), nullptr, 0, true);
        TCHECK_EQ(r.hdr(), 0x00010040u);
        TCHECK_EQ(r.res(), 0u);
        if (variant == 0) {
            TCHECK_EQ(LogCount("dlp:FKCL: command 0x01 -> 0x00000000 (first call)"), 1);
            TCHECK_EQ(LogCount("dlp:SRVR: command 0x01 -> 0x00000000 (first call)"), 1);
        }
        TCHECK_EQ(c.hub->served_inline(), 2u);
        TCHECK_EQ(c.hub->served_posted(), 2u);
        TCHECK_EQ(c.hub->failed_fast() + c.hub->cancelled(), 0u);
        TCHECK_EQ(Do(c, fk, req::Make(0x02, 0, 0), nullptr, 0, true).res(), 0u);
        TCHECK_EQ(Do(c, sv, req::Make(0x02, 0, 0), nullptr, 0, true).res(), 0u);
        if (debug) {
            // The capture writes nothing for the commands of the two dlp services (the hub's are in the main log), and is alive for nwm::UDS
            TCHECK_EQ(debug->Count("uds cmd 0x"), 0);
            TCHECK_EQ(debug->Count("ndm cmd 0x"), 0);
            const Handle uds = c.Open("nwm::UDS");
            TCHECK_EQ(UdsStatus(c, uds).res(), 0u);
            TCHECK_EQ(debug->Count("uds cmd 0x0b"), 1);
            TCHECK_EQ(debug->Count("ndm cmd 0x"), 0);
            c.Close(uds);
        }
        c.Close(fk);
        c.Close(sv);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// L3: the gate, and the game's Initialize giving the engine back first.

static void L3() {
    std::printf("L3: while the lobby holds the engine: kWrongState, Shutdown that ends nothing, Initialize that gives it back first\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    Game ga(a, false, true), gb(b, true, false);
    const Handle au = a.Open("nwm::UDS"), bu = b.Open("nwm::UDS");
    TCHECK(au != 0 && bu != 0);

    // Before the lobby holds anything the game's session is the engine's: not initialised, but a success for the status
    Rep r = UdsStatus(a, au);
    TCHECK_EQ(r.res(), 0u);
    OpenCreator(ga);
    TCHECK(a.hub->EngineHeld());
    TCHECK(a.e().initialized());
    const size_t mark = LogSize();

    // The first three gated commands of the process are in the log, the rest not: Shutdown first (the line says nothing was shut down)
    r = Do(a, au, req::Shutdown());
    TCHECK_EQ(r.hdr(), 0x00030040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(a.e().initialized());  // (answered, NOT run)
    TCHECK(a.hub->EngineHeld());
    TCHECK_EQ(ga.ServerState(), 2u);  // the lobby is still accepting
    r = UdsStatus(a, au);
    TCHECK_EQ(r.hdr(), 0x000B0040u);
    TCHECK_EQ(r.res(), R_WRONG_STATE);
    r = Do(a, au, req::Make(0x02, 0, 0));  // the unnamed "stop the service": gated too (it is a command of the service)
    TCHECK_EQ(r.hdr(), 0x00020040u);
    TCHECK_EQ(r.res(), R_WRONG_STATE);
    // every other command but 0x01 / 0x1B: the same answer, whatever the request (nothing of it is read)
    int gated = 0;
    for (u32 id = 0x04; id <= 0x23; ++id) {
        if (id == 0x1B) {
            continue;
        }
        r = Do(a, au, req::Make(id, 0, 0));
        TCHECK_EQ(r.hdr(), Plain(id));
        TCHECK_EQ(r.res(), R_WRONG_STATE);
        ++gated;
    }
    TCHECK_EQ(gated, 0x23 - 0x04);  // 0x04..0x23 without 0x1B
    // an id the service does not have is the dispatcher's own answer in any state
    for (u32 id : {0u, 0x24u, 0x25u, 0xFFFFu}) {
        r = Do(a, au, std::vector<u32>{id << 16});
        TCHECK_EQ(r.hdr(), 0x40u);
        TCHECK_EQ(r.res(), R_UNKNOWN_UDS_COMMAND);
    }
    TCHECK_EQ(LogCount("while the Download Play lobby holds the network", mark), 3);
    TCHECK_EQ(LogCount("Shutdown answered, nothing shut down", mark), 1);
    TCHECK_EQ(LogCount("answered \"wrong state\"", mark), 2);
    // none of it touched the lobby or the engine; the dlp services still answer
    TCHECK(a.e().initialized());
    TCHECK(a.hub->EngineHeld());
    TCHECK_EQ(ga.ServerState(), 2u);

    // A searcher joins: the same gate on its console (its lobby holds ITS engine), the client in session
    TCHECK(FindGroup(gb, a.mac()));
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    const Rep j = gb.FkJoin(a.mac());
    TCHECK_EQ(j.hdr(), 0x000B0040u);
    TCHECK_EQ(j.res(), 0u);
    TCHECK(b.hub->EngineHeld());
    TCHECK_EQ(gb.StatusState(), 5u);
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 1; }));
    r = UdsStatus(b, bu);
    TCHECK_EQ(r.res(), R_WRONG_STATE);
    r = Do(b, bu, req::Shutdown());  // answered, and the client stays in its session
    TCHECK_EQ(r.res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TCHECK_EQ(gb.StatusState(), 5u);
    TCHECK(b.e().initialized());
    TCHECK_EQ(ga.ClientsTotal(), 1u);
    TCHECK_EQ(ga.ServerState(), 2u);

    // The game of A starts its own network: Initialize (0x1B). The lobby gives the engine back FIRST (posted to the network thread), whole.
    const u32 posted_a = a.hub->served_posted();
    const size_t mark_init = LogSize();
    r = UdsInit(a, au, 0xA1A1, "GameHost");
    TCHECK_EQ(r.hdr(), 0x001B0042u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], IPC_Desc_SharedHandles(1));
    TCHECK(r.w[3] != 0);
    TCHECK_EQ(a.hub->served_posted(), posted_a + 1);  // (OnGameUdsStarting was posted)
    TCHECK(!a.hub->EngineHeld());
    TCHECK_EQ(LogCount("dlp: nwm::UDS Initialize (cmd 0x1b) while the Download Play lobby held the network: given back first", mark_init), 1);
    TCHECK(a.e().initialized());  // (the game's Initialize, not the lobby's: its identity follows)
    TCHECK_EQ(ga.ServerState(), 1u);  // the lobby is back at "initialised", its table dropped
    TCHECK_EQ(ga.ClientsTotal(), 0u);
    r = UdsStatus(a, au);  // the very next command is not gated
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 3u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    ni.channel = 1;
    TCHECK_EQ(Do(a, au, req::BeginHosting(&ni)).res(), 0u);
    r = Do(a, au, req::Make(0x1A, 0, 0));  // GetChannel: the game's channel, not the lobby's 6
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 1u);
    u32 res = 0;
    const NodeInfo own = UdsNode(a, au, 1, &res);
    TCHECK_EQ(res, 0u);
    TCHECK_EQ(own.friend_code_seed, 0xA1A1u);  // not the lobby's identity (0xF001, "Alice")
    TCHECK(NameOf(own.username) == "GameHost");
    r = UdsStatus(a, au);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 6u);  // ConnectedAsHost
    // and Shutdown is the game's own now: it reaches the engine
    TCHECK_EQ(Do(a, au, req::Shutdown()).res(), 0u);
    TCHECK(!a.e().initialized());

    // B's game starts with the deprecated Initialize (0x01), the client in session (5): gate, then OnGameUdsStarting, then the game's engine
    const size_t mark_b = LogSize();
    r = UdsStatus(b, bu);
    TCHECK_EQ(r.res(), R_WRONG_STATE);
    r = UdsInit(b, bu, 0xB2B2, "GameGuest", true);
    TCHECK_EQ(r.hdr(), 0x00010042u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(r.w[3] != 0);
    TCHECK(!b.hub->EngineHeld());
    TCHECK_EQ(LogCount("dlp: nwm::UDS Initialize (cmd 0x01) while the Download Play lobby held the network: given back first", mark_b), 1);
    TCHECK_EQ(gb.StatusState(), 1u);  // the client is back at "initialised": the roster and the session are gone
    TCHECK_EQ(gb.NodesTotal(), 0u);
    r = UdsStatus(b, bu);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 3u);
    TCHECK_EQ(Do(b, bu, req::Shutdown()).res(), 0u);
    TCHECK(!b.e().initialized());

    // The stale windows. (1) right after StartAccepting returned the gate is up (the hub refreshes its hints BEFORE it publishes the answer);
    // (2) right after the game's Initialize returned the gate is down (the same order, the other way). Rounds on one console, with the
    // network thread held 15 ms after each answer it publishes (SlowSignalKernel): the game thread's next command runs in that window.
    Lc c(room, 3, "Carol", true, true);
    Game gc(c, false, true);
    const Handle cu = c.Open("nwm::UDS");
    u32 stale_up = 0, stale_down = 0;
    c.hub_kernel.delay_ms = 15;
    for (int i = 0; i < 12; ++i) {
        TCHECK_EQ(gc.SvInit(3).res(), 0u);
        TCHECK_EQ(gc.SvAccepting(0, 6).res(), 0u);
        if (UdsStatus(c, cu).res() != R_WRONG_STATE) {
            ++stale_up;
        }
        TCHECK_EQ(UdsInit(c, cu, 0xC3C3, "GameCarol").res(), 0u);
        if (UdsStatus(c, cu).res() != 0) {
            ++stale_down;
        }
        TCHECK_EQ(Do(c, cu, req::Shutdown()).res(), 0u);
        TCHECK(!c.e().initialized());
        TCHECK(!c.hub->EngineHeld());
        TCHECK_EQ(gc.SvFinalize().res(), 0u);
    }
    c.hub_kernel.delay_ms = 0;
    TCHECK_EQ(stale_up, 0u);
    TCHECK_EQ(stale_down, 0u);
    CHECK_RULES();
    c.Close(cu);
    c.Close(gc.sv());
    a.Close(au);
    a.Close(ga.sv());
    b.Close(bu);
    b.Close(gb.fk());
}

// ---------------------------------------------------------------------------------------------------------------------
// L2: Triple Deluxe by raw words, then the game's own nwm::UDS on the same engines.

struct TdShared {
    std::atomic<bool> searcher_in_session{false};
    std::atomic<bool> creator_done{false}, searcher_done{false};
    std::atomic<u32> join_ms{0};
    bool searcher_finalizes = true;
};

static void CreatorTd(Game& g, Lc& n, TdShared& s) {
    Rep r = g.SvInit(3);
    TCHECK_EQ(r.hdr(), 0x00010040u);
    TCHECK_EQ(r.res(), 0u);
    r = g.SvDup();  // GetDupNoticeNeed: [0x00100080, 0, 0]
    TCHECK_EQ(r.hdr(), 0x00100080u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0u);
    TCHECK_EQ(g.ServerState(), 1u);
    TCHECK(!n.hub->EngineHeld());
    r = g.SvAccepting(0, 6);
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(n.hub->EngineHeld());  // (already true when the answer is)
    TCHECK_EQ(g.ServerState(), 2u);

    TCHECK(WaitFor(10000, [&] { return g.ClientsTotal() == 1; }));
    r = g.SvClients(4, 8);
    TCHECK_EQ(r.hdr(), 0x000B0082u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 1u);
    TCHECK_EQ(r.w[3], (8u << 4) | 0xC);  // the descriptor and the address of the game's array, echoed
    TCHECK_EQ(r.w[4], g.CliPtr());
    TCHECK_EQ(g.ClientId(0), 2u);
    r = g.SvClientState(2);  // accepted: external 5, joined 1, node 2
    TCHECK_EQ(r.hdr(), 0x000D0180u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0x05010002u);
    r = g.SvClientInfo(2);  // a posted command: ReadNode on the network thread
    TCHECK_EQ(r.hdr(), 0x000C02C0u);
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF002, "Bob", 2);
    r = g.SvClientInfo(1);  // the host's own record: kNoData with the full header (the convention of Azahar)
    TCHECK_EQ(r.hdr(), 0x000C02C0u);
    TCHECK_EQ(r.res(), R_NO_DATA);
    CheckNodeRecord(r, 2, 0xF001, "Alice", 1);
    r = g.SvClientInfo(3);  // nobody there
    TCHECK_EQ(r.hdr(), Plain(0xC));
    TCHECK_EQ(r.res(), R_NO_DATA);

    // the Start button, once the searcher has read its roster
    TCHECK(WaitFor(15000, [&] { return s.searcher_in_session.load(); }));
    r = g.SvDistribute();
    TCHECK_EQ(r.hdr(), 0x00070040u);
    TCHECK_EQ(r.res(), 0u);
    const u32 after = g.ServerState();
    TCHECK(after == 6 || after == 8);
    TCHECK(WaitFor(15000, [&] { return g.ServerState() == 8; }));
    r = g.SvClientState(2);  // START_ACK received: external 7
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0x07010002u);
    // the game generates the passphrase and sends it every frame while the state is 8: the first call wins
    r = g.SvPass(kNine);
    TCHECK_EQ(r.hdr(), 0x00080040u);
    TCHECK_EQ(r.res(), 0u);
    r = g.SvPass(reinterpret_cast<const u8*>("ffeeddcc"));
    TCHECK_EQ(r.hdr(), 0x00080040u);
    TCHECK_EQ(r.res(), R_INVALID_STATE);
    TCHECK(WaitFor(15000, [&] { return g.ServerState() == 9; }));
    TCHECK(!n.hub->EngineHeld());  // (the network was closed before the game saw 9)
    TCHECK_EQ(g.ClientsTotal(), 0u);
    r = g.SvFinalize();
    TCHECK_EQ(r.hdr(), 0x00020040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(g.ServerState(), 0u);
    TCHECK(!n.hub->EngineHeld());
    s.creator_done = true;
}

static void SearcherTd(Game& g, Lc& n, Lc& host, TdShared& s) {
    Rep r = g.FkInitName("Bob", 4);
    TCHECK_EQ(r.hdr(), 0x00110040u);
    TCHECK_EQ(r.res(), 0u);
    r = g.FkChannels();
    TCHECK_EQ(r.hdr(), 0x00040080u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0x0421u);
    r = g.FkScan();
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(!n.hub->EngineHeld());  // looking needs no engine
    // GetTitleInfoInOrder(1) then (0) until kNoData, as the game does
    const bool got = WaitFor(10000, [&] {
        const Rep a = g.FkInOrder(1);
        if (a.res() != 0) {
            return false;
        }
        return std::memcmp(g.TitleRec() + kTitleInfoMac, host.mac().data(), 6) == 0;
    });
    TCHECK(got);
    TCHECK_EQ(LoadLE32(g.TitleRec() + kTitleInfoUniqueId), 0x10C0u);
    TCHECK_EQ(LoadLE32(g.TitleRec() + kTitleInfoVariation), 0u);
    r = g.FkInOrder(0);
    TCHECK_EQ(r.hdr(), Plain(9));
    TCHECK_EQ(r.res(), R_NO_DATA);
    r = g.FkServerInfo(host.mac());
    TCHECK_EQ(r.hdr(), 0x00070042u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], (0x298u << 14) | 2);
    const u8* si = g.ServerRec();
    TCHECK(std::memcmp(si + kServerInfoMac, host.mac().data(), 6) == 0);
    TCHECK_EQ(si[kServerInfoMaxClients], 4u);
    TCHECK_EQ(si[kServerInfoJoined], 1u);  // (the host counted)
    TCHECK(NameAt(si + kServerInfoHostName) == "Alice");
    r = g.FkTitleInfo(host.mac());  // (not called by TD, answered all the same)
    TCHECK_EQ(r.res(), 0u);
    r = g.FkStopScan();
    TCHECK_EQ(r.hdr(), 0x00060040u);
    TCHECK_EQ(r.res(), 0u);

    // StartFakeSession: the front-end holds the call until the join ends (the hub's PollPending loop, 10 ms slices, no lock)
    const unsigned long long sleeps0 = g_test_sleep_calls.load(), sleep_ns0 = g_test_sleep_ns.load();
    const u64 t0 = platform::NowMs();
    const Rep j = g.FkJoin(host.mac());
    s.join_ms = Ms(t0);
    TCHECK_EQ(j.hdr(), 0x000B0040u);
    TCHECK_EQ(j.res(), 0u);
    const unsigned long long sleeps = g_test_sleep_calls.load() - sleeps0;
    TCHECK(sleeps >= 1);                                                 // (the join cannot end before the first look)
    TCHECK_EQ(g_test_sleep_ns.load() - sleep_ns0, sleeps * 10000000ull);  // ... and every wait of it was a 10 ms slice
    TCHECK(n.hub->EngineHeld());
    r = g.FkStatus();  // state 5, connected, node 2
    TCHECK_EQ(r.hdr(), 0x000C0180u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0x05010002u);
    TCHECK(WaitFor(8000, [&] { return g.NodesTotal() == 2; }));
    r = g.FkNodes(4);
    TCHECK_EQ(r.hdr(), 0x000D0082u);
    TCHECK_EQ(r.w[2], 2u);
    TCHECK_EQ(r.w[3], (4u << 14) | 2);
    TCHECK_EQ(g.NodeId(0), 1u);
    TCHECK_EQ(g.NodeId(1), 2u);
    r = g.FkNodeInfo(1);
    TCHECK_EQ(r.hdr(), 0x000E02C0u);
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF001, "Alice", 1);
    r = g.FkNodeInfo(2);
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF002, "Bob", 2);
    r = g.FkNodeInfo(3);
    TCHECK_EQ(r.hdr(), Plain(0xE));
    TCHECK_EQ(r.res(), R_NO_DATA);
    s.searcher_in_session = true;
    TCHECK(WaitFor(20000, [&] { return g.StatusState() == 9; }));  // GetMyStatus every frame until 9
    r = g.FkPass();
    TCHECK_EQ(r.hdr(), 0x000F0100u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(std::memcmp(&r.w[2], kNine, 9) == 0);
    TCHECK(n.hub->EngineHeld());  // a searcher at 9 keeps the engine (its roster) until the game finalises
    if (s.searcher_finalizes) {
        r = g.FkFinalize();
        TCHECK_EQ(r.hdr(), 0x00020040u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK(!n.hub->EngineHeld());
    }
    s.searcher_done = true;
}

/// The game's own network on the same engines, after the lobby: `host` hosts it, `guest` joins it, data both ways. The lobby is still
/// holding the guest's engine (a searcher at 9 that has not finalised) when `guest_held`.
static void GameUds(MockRoom& room, Lc& host, Handle hu, Lc& guest, Handle gu, bool guest_held, Game& gg) {
    // the guest's game starts: a searcher at 9 holds the engine until now (gate), the Initialize gives it back first
    if (guest_held) {
        TCHECK(guest.hub->EngineHeld());
        TCHECK_EQ(UdsStatus(guest, gu).res(), R_WRONG_STATE);
        TCHECK_EQ(Do(guest, gu, req::Shutdown()).res(), 0u);  // answered, nothing shut down: the roster and the passphrase stay
        TCHECK(guest.e().initialized());
        TCHECK_EQ(gg.StatusState(), 9u);
        TCHECK(guest.hub->EngineHeld());
    } else {
        TCHECK(!guest.hub->EngineHeld());
    }
    const u32 posted = guest.hub->served_posted();
    const size_t mark = LogSize();
    TCHECK_EQ(UdsInit(guest, gu, 0xB2B2, "GameGuest").res(), 0u);
    TCHECK_EQ(guest.hub->served_posted(), posted + (guest_held ? 1u : 0u));  // (nothing held: nothing is asked of the network thread)
    TCHECK_EQ(LogCount("while the Download Play lobby held the network: given back first", mark), guest_held ? 1 : 0);
    TCHECK(!guest.hub->EngineHeld());
    if (guest_held) {
        TCHECK_EQ(gg.StatusState(), 9u);  // a searcher at 9 stays at 9: the passphrase and the roster remain readable, the engine is free
        const Rep pass = gg.FkPass();
        TCHECK(std::memcmp(&pass.w[2], kNine, 9) == 0);
        TCHECK_EQ(gg.NodesTotal(), 2u);
    }
    // the host's game
    TCHECK_EQ(UdsInit(host, hu, 0xA1A1, "GameHost").res(), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    ni.channel = 1;
    TCHECK_EQ(Do(host, hu, req::BeginHosting(&ni)).res(), 0u);
    Rep r = Do(host, hu, req::Make(0x1A, 0, 0));
    TCHECK_EQ(r.w[2], 1u);  // (the game's channel, not the lobby's 6)
    u32 res = 0;
    NodeInfo own = UdsNode(host, hu, 1, &res);
    TCHECK_EQ(res, 0u);
    TCHECK_EQ(own.friend_code_seed, 0xA1A1u);
    TCHECK(NameOf(own.username) == "GameHost");
    TCHECK_EQ(Do(host, hu, req::Bind(1, 0x2E30, 3, 0xFFFF)).res(), 0u);

    // the guest scans for the game's network (only the game's: the lobby is gone) and joins it
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // (a host has beaconed for a while when a game scans for it)
    std::vector<u8> scan(0x4000);
    const u32 cancel = guest.ds.kernel.CreateEvent();
    r = Do(guest, gu, req::Scan(scan.data(), 0x4000, 0x0010CAFE, 1, cancel));
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(LoadLE32(scan.data() + 8), 1u);  // one network: the game's
    NetworkInfo net;
    TCHECK(FirstScannedNetwork(scan, &net));
    TCHECK_EQ(Do(guest, gu, req::Bind(1, 0x2E30, 3, 0xFFFF)).res(), 0u);
    r = Do(guest, gu, req::Connect(1, &net));  // blocks until the host admitted us
    TCHECK_EQ(r.res(), 0u);
    room.WaitIdle();
    r = UdsStatus(guest, gu);
    TCHECK_EQ(r.w[2], 9u);  // ConnectedAsClient
    NodeInfo seen = UdsNode(guest, gu, 1, &res);
    TCHECK_EQ(res, 0u);
    TCHECK_EQ(seen.friend_code_seed, 0xA1A1u);  // the host's identity as the game gave it, not the lobby's (0xF001, "Alice")
    TCHECK(NameOf(seen.username) == "GameHost");
    seen = UdsNode(guest, gu, 2, &res);
    TCHECK_EQ(res, 0u);
    TCHECK_EQ(seen.friend_code_seed, 0xB2B2u);  // ... and its own
    TCHECK(NameOf(seen.username) == "GameGuest");

    // data both ways on the game's channel; the lobby's channel never shows
    const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
    const u8 world[6] = {'w', 'o', 'r', 'l', 'd', '!'};
    std::vector<u8> out(0x5C8);
    TCHECK_EQ(Do(guest, gu, req::SendTo(1, 3, 0, hello, 5)).res(), 0u);
    TCHECK_EQ(Do(host, hu, req::SendTo(2, 3, 0, world, 6)).res(), 0u);
    room.WaitIdle();
    r = Do(host, hu, req::Pull(1), out.data(), static_cast<u32>(out.size()));
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 5u);
    TCHECK_EQ(r.w[3], 2u);
    TCHECK(std::memcmp(out.data(), hello, 5) == 0);
    r = Do(guest, gu, req::Pull(1), out.data(), static_cast<u32>(out.size()));
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 6u);
    TCHECK_EQ(r.w[3], 1u);
    TCHECK(std::memcmp(out.data(), world, 6) == 0);
    r = Do(host, hu, req::Pull(1), out.data(), static_cast<u32>(out.size()));
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0u);  // nothing more (no lobby message, no echo)

    // The guest's dlp:FKCL session closes (the game ends Download Play without Finalize): its lobby role is over already, and the game's
    // network is NOT touched.
    if (gg.fk() != 0) {
        guest.Close(gg.fk());
    }
    TCHECK(guest.e().initialized());
    TCHECK_EQ(UdsStatus(guest, gu).w[2], 9u);
    TCHECK_EQ(Do(guest, gu, req::SendTo(1, 3, 0, hello, 5)).res(), 0u);
    room.WaitIdle();
    r = Do(host, hu, req::Pull(1), out.data(), static_cast<u32>(out.size()));
    TCHECK_EQ(r.w[2], 5u);

    TCHECK_EQ(Do(guest, gu, req::Shutdown()).res(), 0u);
    TCHECK_EQ(Do(host, hu, req::Shutdown()).res(), 0u);
    TCHECK(!guest.e().initialized());
    TCHECK(!host.e().initialized());
}

static void L2One(bool searcher_finalizes) {
    std::printf("L2: Triple Deluxe by raw words, then the game's UDS (the searcher %s)\n",
                searcher_finalizes ? "finalises like the game" : "still holds the engine at 9");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    Game ga(a, false, true), gb(b, true, false);
    const Handle au = a.Open("nwm::UDS"), bu = b.Open("nwm::UDS");
    TCHECK(au != 0 && bu != 0);
    TdShared s;
    s.searcher_finalizes = searcher_finalizes;
    const size_t mark = LogSize();
    const unsigned long long sleeps0 = g_test_sleep_calls.load();
    {
        UdsPoller pa(a, au), pb(b, bu);  // the games' event threads, polling nwm::UDS while the lobby runs
        std::thread tc([&] { CreatorTd(ga, a, s); });
        std::thread ts([&] { SearcherTd(gb, b, a, s); });
        tc.join();
        ts.join();
        pa.Stop();
        pb.Stop();
        TCHECK(s.creator_done.load());
        TCHECK(s.searcher_done.load());
        TCHECK_EQ(pa.bad(), 0u);
        TCHECK_EQ(pb.bad(), 0u);
        TCHECK(pa.polls() > 30);
        TCHECK(pb.polls() > 30);
        TCHECK(pa.gated() > 0);  // the creator holds its engine from StartAccepting to 9
        TCHECK(pb.gated() > 0);  // the searcher from the join on
        TCHECK(pa.passed() > 0);
        TCHECK(pb.passed() > 0);
        std::printf("  L2: join %u ms; polls %u/%u (gated %u/%u, free %u/%u)\n", s.join_ms.load(), pa.polls(), pb.polls(), pa.gated(), pb.gated(),
                    pa.passed(), pb.passed());
    }
    TCHECK(g_test_sleep_calls.load() > sleeps0);
    // The books of the hub: nothing refused, nothing cancelled, nothing run on a game thread
    TCHECK_EQ(a.hub->cancelled() + a.hub->failed_fast() + a.hub->ran_here() + a.hub->deferred_closes(), 0u);
    TCHECK_EQ(b.hub->cancelled() + b.hub->failed_fast() + b.hub->ran_here() + b.hub->deferred_closes(), 0u);
    TCHECK(a.hub->served_posted() >= 4u);
    TCHECK(b.hub->served_posted() >= 4u);
    TCHECK(!a.hub->EngineHeld());
    TCHECK_EQ(b.hub->EngineHeld(), !searcher_finalizes);
    TCHECK_EQ(LogCount("dlp: the first command of", mark), 0);  // (said once in the life of the process: by L1)

    // The creator's game closes its dlp:SRVR session after Finalize: the role is over, the close finds nothing to end and shuts nothing down
    const size_t mark_close = LogSize();
    a.Close(ga.sv());
    TCHECK_EQ(LogCount("dlp: that dlp:SRVR session had called Initialize: its role ends with it", mark_close), 1);
    TCHECK(!a.hub->EngineHeld());

    if (searcher_finalizes) {
        gb.FkStopSession();  // (nothing: the session is already at 1)
        b.Close(gb.fk());    // the searcher's game closes its session too (before the UDS, here)
        Game none(b, false, false);
        GameUds(room, a, au, b, bu, false, none);
    } else {
        GameUds(room, a, au, b, bu, true, gb);  // (closes the searcher's FKCL session in the middle of the game's network)
    }
    TCHECK_EQ(a.hub->cancelled() + a.hub->failed_fast() + a.hub->ran_here() + a.hub->deferred_closes(), 0u);
    TCHECK_EQ(b.hub->cancelled() + b.hub->failed_fast() + b.hub->ran_here() + b.hub->deferred_closes(), 0u);
    CHECK_RULES();
    a.Close(au);
    b.Close(bu);
}

static void L2() {
    L2One(false);
    L2One(true);
}

// ---------------------------------------------------------------------------------------------------------------------
// L4: closing a dlp session handle.

static void L4() {
    std::printf("L4: closing a dlp session handle: the role ends, the engine is free, the value is forgotten\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);

    // (a) the creator and a searcher; the searcher's game closes its session without Finalize
    Game ga(a, false, true), gb(b, true, false);
    OpenCreator(ga);
    TCHECK(FindGroup(gb, a.mac()));
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    TCHECK_EQ(gb.FkJoin(a.mac()).res(), 0u);
    TCHECK_EQ(gb.StatusState(), 5u);
    TCHECK(b.hub->EngineHeld());
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 1; }));
    {
        const size_t mark = LogSize();
        const u32 posted = b.hub->served_posted();
        const Handle fk = gb.fk();
        b.Close(fk);
        TCHECK(!b.hub->EngineHeld());
        TCHECK(!b.e().initialized());
        TCHECK_EQ(b.hub->served_posted(), posted + 1);  // (OnSessionClosed was posted to the network thread)
        TCHECK_EQ(b.hub->ran_here(), 0u);
        TCHECK_EQ(LogCount("uds: game closed its fake dlp:FKCL session", mark), 1);
        TCHECK_EQ(LogCount("dlp: that dlp:FKCL session had called Initialize: its role ends with it", mark), 1);
        TCHECK_EQ(LogCount("uds: that session had called Initialize: the service shuts down with it", mark), 0);
        const Rep r = Do(b, fk, Words(0x000C0000, {}));  // the closed value is forgotten: the hook declines
        TCHECK(!r.handled);
        Game gb2(b, true, false);  // a fresh session sees the role finalised: state 1 ("initialised") and no node
        TCHECK_EQ(gb2.StatusState(), 1u);
        TCHECK_EQ(gb2.NodesTotal(), 0u);
        b.Close(gb2.fk());  // (never initialised: nothing)
    }
    TCHECK(WaitFor(8000, [&] { return ga.ClientsTotal() == 0; }));  // A sees B go (the Deauthentications of the engine's Shutdown)

    // (b) a session that never initialised does nothing
    {
        const u32 posted = a.hub->served_posted(), here = a.hub->ran_here(), fast = a.hub->failed_fast();
        const size_t mark = LogSize();
        Game extra(a, true, true);
        TCHECK_EQ(extra.ServerState(), 2u);  // an inline read through another session: the creator is as it was
        TCHECK_EQ(extra.FkChannels().res(), 0u);
        a.Close(extra.sv());
        a.Close(extra.fk());
        TCHECK_EQ(a.hub->served_posted(), posted);
        TCHECK_EQ(a.hub->ran_here(), here);
        TCHECK_EQ(a.hub->failed_fast(), fast);
        TCHECK_EQ(ga.ServerState(), 2u);
        TCHECK(a.hub->EngineHeld());
        TCHECK(a.e().initialized());
        TCHECK_EQ(LogCount("its role ends with it", mark), 0);
        TCHECK_EQ(LogCount("uds: game closed its fake dlp:", mark), 2);
    }

    // (c) the creator's game closes dlp:SRVR without Finalize
    {
        const size_t mark = LogSize();
        const u32 posted = a.hub->served_posted();
        const Handle sv = ga.sv();
        a.Close(sv);
        TCHECK(!a.hub->EngineHeld());
        TCHECK(!a.e().initialized());
        TCHECK_EQ(a.hub->served_posted(), posted + 1);
        TCHECK_EQ(LogCount("uds: game closed its fake dlp:SRVR session", mark), 1);
        TCHECK_EQ(LogCount("dlp: that dlp:SRVR session had called Initialize: its role ends with it", mark), 1);
        TCHECK(!Do(a, sv, Words(0x00030000, {})).handled);
        Game ga2(a, false, true);
        TCHECK_EQ(ga2.ServerState(), 0u);  // finalised
        a.Close(ga2.sv());
    }

    // (d) a handle found dead: the game closed it through a path the hook does not see; the next call on the value ends the role
    {
        Game g(a, false, true);
        OpenCreator(g);
        TCHECK(a.hub->EngineHeld());
        const Handle sv = g.sv();
        svcCloseHandle(sv);  // (no NoteHandleClosed)
        const Rep r = Do(a, sv, Words(0x00030000, {}));
        TCHECK(!r.handled);  // the hook declines: the value now names something else
        TCHECK(!a.hub->EngineHeld());
        TCHECK(!a.e().initialized());
        Game g2(a, false, true);
        TCHECK_EQ(g2.ServerState(), 0u);
        a.Close(g2.sv());
    }

    // (e) nwm::UDS: a session that called Initialize shuts the service down when it closes (the hub is not asked); one that did not
    // (the game opens nwm::UDS twice) does not
    {
        const u32 posted = a.hub->served_posted(), here = a.hub->ran_here();
        const size_t mark = LogSize();
        const Handle u1 = a.Open("nwm::UDS"), u2 = a.Open("nwm::UDS");
        TCHECK_EQ(UdsInit(a, u1, 0xA1A1, "GameHost").res(), 0u);
        TCHECK(a.e().initialized());
        a.Close(u2);
        TCHECK(a.e().initialized());
        TCHECK_EQ(LogCount("uds: that session had called Initialize", mark), 0);
        a.Close(u1);
        TCHECK(!a.e().initialized());
        TCHECK_EQ(LogCount("uds: that session had called Initialize: the service shuts down with it", mark), 1);
        TCHECK_EQ(a.hub->served_posted(), posted);
        TCHECK_EQ(a.hub->ran_here(), here);
    }

    // (f) the other two Initialize commands count as well: dlp:SRVR InitializeWithName (0x0F) and dlp:FKCL Initialize (0x01)
    {
        const size_t mark = LogSize();
        Game g(a, true, true);
        TCHECK_EQ(g.SvInitName(3, "Zed").res(), 0u);
        TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
        TCHECK(a.hub->EngineHeld());
        a.Close(g.sv());
        TCHECK(!a.hub->EngineHeld());
        TCHECK(!a.e().initialized());
        TCHECK_EQ(LogCount("dlp: that dlp:SRVR session had called Initialize: its role ends with it", mark), 1);
        Game s(a, false, true);
        TCHECK_EQ(s.ServerState(), 0u);
        a.Close(s.sv());
        TCHECK_EQ(g.FkInit(4).res(), 0u);
        TCHECK_EQ(g.FkScan().res(), 0u);
        a.Close(g.fk());
        TCHECK_EQ(LogCount("dlp: that dlp:FKCL session had called Initialize: its role ends with it", mark), 1);
        Game f(a, true, false);
        TCHECK_EQ(f.StatusState(), 1u);  // finalised: "initialised", not "scanning" (2)
        a.Close(f.fk());
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// L5: no network thread.

static void L5() {
    std::printf("L5: no network thread: posted commands fail at once, inline ones answer, the ends of a session run on the caller\n");
    g_rules.Reset();
    unsigned troubles = g_trouble_events.load();
    {
        // (a) a hub nobody ever ticked
        MockRoom room;
        Lc a(room, 1, "Alice", true, false);
        Game g(a, true, true);
        const Handle au = a.Open("nwm::UDS");
        const size_t mark = LogSize();
        const u64 t0 = platform::NowMs();
        Rep r = g.SvInit(3);
        TCHECK_EQ(r.hdr(), Plain(1));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        r = g.SvAccepting(0, 6);
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        r = g.FkInit();
        TCHECK_EQ(r.hdr(), Plain(1));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        r = g.FkInitName("Bob", 4);
        TCHECK_EQ(r.hdr(), Plain(0x11));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        r = g.FkScan();
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        r = g.FkJoin(a.mac());  // (answered at once: there is no join to wait for 8 s)
        TCHECK_EQ(r.hdr(), Plain(0xB));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK(Ms(t0) < 500);
        Trouble(6);
        TCHECK_EQ(a.hub->failed_fast(), 6u);
        TCHECK_EQ(a.hub->cancelled(), 0u);
        TCHECK_EQ(a.hub->served_posted(), 0u);
        TCHECK_EQ(LogCount("answered \"wireless off\": the network side did not run it", mark), 6);
        TCHECK_EQ(LogCount("(so far 6 refused at once because no network thread was running, 0 not taken in 2 s)", mark), 1);
        // the inline ones answer, from the lobby as it is (off)
        r = g.SvState();
        TCHECK_EQ(r.hdr(), 0x00030080u);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(r.w[2], 0u);
        r = g.FkStatus();
        TCHECK_EQ(r.hdr(), 0x000C0180u);
        TCHECK_EQ(r.w[2], 0u);
        r = g.FkChannels();
        TCHECK_EQ(r.w[2], 0x0421u);
        TCHECK_EQ(g.SvDup().res(), 0u);
        TCHECK_EQ(g.SvIsChild().res(), 0u);
        r = g.Fk(0x00120000, {});
        TCHECK_EQ(r.hdr(), 0x40u);
        TCHECK_EQ(r.res(), R_BAD_HEADER);
        r = g.SvClients(4, 8);
        TCHECK_EQ(r.res(), R_INVALID_STATE);  // (the creator is off)
        TCHECK_EQ(a.hub->served_inline(), 7u);
        // the game's Initialize needs nothing of a hub that holds nothing: no request, no wait, nothing "run here"
        r = UdsInit(a, au, 0xA1A1, "GameHost");
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(a.hub->ran_here(), 0u);
        TCHECK_EQ(a.hub->failed_fast(), 6u);
        TCHECK_EQ(Do(a, au, req::Shutdown()).res(), 0u);
        // The end of a session whose Initialize was refused (it counts): no network thread takes the request, and nothing of the network
        // side runs on this thread - the close is remembered for the next Tick (deferred_closes), nothing is run here
        a.Close(g.sv());
        TCHECK_EQ(a.hub->ran_here(), 0u);
        TCHECK_EQ(a.hub->deferred_closes(), 1u);
        TCHECK_EQ(a.hub->failed_fast(), 7u);
        a.Close(g.fk());
        TCHECK_EQ(a.hub->ran_here(), 0u);
        TCHECK_EQ(a.hub->deferred_closes(), 2u);
        TCHECK_EQ(a.hub->failed_fast(), 8u);
        TCHECK(!a.hub->EngineHeld());
        TCHECK_EQ(a.hub->served_posted(), 0u);
        a.Close(au);

        // (b) a hundred more, to the line that is said one in 64
        Game g2(a, false, true);
        for (int i = 0; i < 140; ++i) {
            r = g2.SvAccepting(0, 6);
            TCHECK_EQ(r.hdr(), Plain(5));
            TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        }
        Trouble(140);
        TCHECK_EQ(a.hub->failed_fast(), 8u + 140u);
        TCHECK_EQ(LogCount("answered \"wireless off\": the network side did not run it", mark), ExpectedTroubleLines(g_trouble_events.load()) - ExpectedTroubleLines(troubles));

        // (c) the network thread comes up late: the hub serves from its first Tick on
        a.StartNet();
        TCHECK_EQ(g2.SvInit(3).res(), 0u);
        TCHECK_EQ(g2.ServerState(), 1u);
        // (d) Stop(): posted commands fail at once, inline ones answer
        a.hub->Stop();
        const u64 t1 = platform::NowMs();
        r = g2.SvAccepting(0, 6);
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        Trouble(1);
        TCHECK(Ms(t1) < 300);
        TCHECK_EQ(g2.ServerState(), 1u);  // (it never ran)
        a.hub->Stop();                    // twice is harmless
        // EndAccepting is one of the commands that succeed in every state (Triple Deluxe and Planet Robobot treat a failing Finalize as fatal, so
        // the hub answers the whole family with success when nobody serves it - L13): after Stop() it answers success and does not run.
        r = g2.SvEnd();
        TCHECK_EQ(r.hdr(), Plain(6));
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(g2.SvState().w[2], 1u);
        a.Close(g2.sv());
    }
    {
        // (e) a request nobody serves in 2 s is cancelled: the network thread is alive (it ticked a moment ago) but stalled
        MockRoom room;
        Lc a(room, 1, "Alice", true, true);
        Game g(a, false, true);
        a.net->Pause(true);
        const u64 t0 = platform::NowMs();
        const Rep r = g.SvInit(3);
        const u32 waited = Ms(t0);
        TCHECK_EQ(r.hdr(), Plain(1));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        Trouble(1);
        TCHECK(waited >= 1900 && waited < 3500);
        TCHECK_EQ(a.hub->cancelled(), 1u);
        TCHECK_EQ(a.hub->failed_fast(), 0u);
        a.net->Pause(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        TCHECK_EQ(g.ServerState(), 0u);  // it never ran, not even later
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        TCHECK_EQ(g.ServerState(), 1u);
        a.Close(g.sv());
    }
    {
        // (f) the network thread dies while the lobby holds the engine, and the game goes on to its nwm::UDS: the engine still comes back
        MockRoom room;
        Lc a(room, 1, "Alice", true, true);
        Game g(a, false, true);
        const Handle au = a.Open("nwm::UDS");
        OpenCreator(g);
        TCHECK(a.hub->EngineHeld());
        a.StopNet();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
        TCHECK_EQ(UdsStatus(a, au).res(), R_WRONG_STATE);  // (still held: the flag is the lobby's, no thread needed)
        const u64 t0 = platform::NowMs();
        const Rep r = UdsInit(a, au, 0xA1A1, "GameHost");
        TCHECK(Ms(t0) < 300);
        TCHECK_EQ(r.res(), 0u);
        TCHECK_EQ(a.hub->ran_here(), 1u);   // (the lobby's end, on the game's thread: nothing else could)
        TCHECK_EQ(a.hub->failed_fast(), 1u);
        TCHECK(!a.hub->EngineHeld());
        TCHECK_EQ(g.ServerState(), 1u);
        TCHECK_EQ(UdsStatus(a, au).res(), 0u);
        u32 res = 0;
        NetworkInfo ni = MakeNetworkInfo(4, 4);
        TCHECK_EQ(Do(a, au, req::BeginHosting(&ni)).res(), 0u);
        const NodeInfo own = UdsNode(a, au, 1, &res);
        TCHECK_EQ(own.friend_code_seed, 0xA1A1u);
        TCHECK(NameOf(own.username) == "GameHost");
        // a posted command still fails at once (StartAccepting: EndAccepting would be answered with success, L13)
        TCHECK_EQ(g.SvAccepting(0, 6).res(), R_WIRELESS_OFF);
        Trouble(1);
        TCHECK_EQ(Do(a, au, req::Shutdown()).res(), 0u);
        // The creator's session closes with no network thread to take the request: the end of the role is NOT run on this thread (only the
        // give-back above is), it is remembered, and the next Tick of a network thread - the room comes back - ends it
        a.Close(g.sv());
        TCHECK_EQ(a.hub->ran_here(), 1u);
        TCHECK_EQ(a.hub->deferred_closes(), 1u);
        TCHECK_EQ(a.hub->failed_fast(), 3u);  // (refused at once: the give-back, the StartAccepting above, this close)
        Game s(a, false, true);  // (a session of its own reads the lobby: the closed one is forgotten)
        TCHECK_EQ(s.ServerState(), 1u);  // still "initialised": nobody has ended the role yet
        a.StartNet();
        TCHECK(WaitFor(3000, [&] { return s.ServerState() == 0; }));  // finalised by that Tick
        TCHECK_EQ(a.hub->deferred_closes(), 1u);
        a.Close(s.sv());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// L9: four game threads on one console.

static void L9() {
    std::printf("L9: four game threads on one console: inline reads, posted commands, nwm::UDS, sessions opened and closed\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Game g1(a, true, true), g2(a, true, false);
    const Handle au = a.Open("nwm::UDS");
    TCHECK_EQ(g2.FkInitName("Bob", 4).res(), 0u);  // a posted command, before the threads
    g_test_hle = a.ds.hle.get();
    std::atomic<int> bad{0};
    std::atomic<u32> inline_calls{0}, posted_calls{0};
    Watchdog dog(60);
    dog.Step("L9 threads");
    std::vector<std::thread> threads;
    threads.emplace_back([&] {  // inline reads of both services
        Game g(a, true, true);
        for (int i = 0; i < 400; ++i) {
            const Rep s = g.SvState();
            const Rep c = g.FkChannels();
            inline_calls += 2;
            if (s.hdr() != 0x00030080u || s.res() != 0 || s.w[2] != 0 || c.hdr() != 0x00040080u || c.w[2] != 0x0421u) {
                ++bad;
            }
        }
        a.Close(g.fk());
        a.Close(g.sv());
    });
    threads.emplace_back([&] {  // posted commands: scan on, scan off
        for (int i = 0; i < 100; ++i) {
            const Rep s = g2.FkScan();
            const Rep t = g2.FkStopScan();
            posted_calls += 2;
            if (s.res() != 0 || t.res() != 0) {
                ++bad;
            }
        }
    });
    threads.emplace_back([&] {  // nwm::UDS: a status poll, as the game's event thread does
        for (int i = 0; i < 400; ++i) {
            const Rep r = UdsStatus(a, au);
            if (r.res() != 0) {
                ++bad;
            }
        }
    });
    threads.emplace_back([&] {  // sessions opened and closed (no Initialize: nothing for the hub to end)
        for (int i = 0; i < 200; ++i) {
            Handle h = 0;
            const Result rc = srvGetServiceHandle(&h, i % 2 ? "dlp:FKCL" : "dlp:SRVR");
            if (R_FAILED(rc) || h == 0) {
                ++bad;
                continue;
            }
            a.ds.hle->NoteHandleClosed(h);
            svcCloseHandle(h);
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    dog.Step("");
    TCHECK_EQ(bad.load(), 0);
    TCHECK_EQ(a.hub->served_inline(), inline_calls.load());
    TCHECK_EQ(a.hub->served_posted(), 1u + posted_calls.load());
    TCHECK_EQ(a.hub->cancelled() + a.hub->failed_fast() + a.hub->ran_here() + a.hub->deferred_closes(), 0u);
    a.Close(g1.fk());
    a.Close(g1.sv());
    a.Close(g2.fk());
    a.Close(au);
}

// ---------------------------------------------------------------------------------------------------------------------
// L6: a game that is not in the table.

static void L6() {
    std::printf("L6: a game without a hub: the dlp services are the real ones, the nwm::UDS replies are those of an idle hub, word for word\n");
    g_rules.Reset();
    MockRoom room0, room1;
    Lc plain(room0, 1, "Alice", false, false);
    Lc hubbed(room1, 1, "Alice", true, false);  // (an idle hub needs no network thread: it is never asked anything)
    TCHECK(plain.hub == nullptr);
    Result rc = 0;
    TCHECK_EQ(plain.Open("dlp:FKCL", &rc), 0u);
    TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
    TCHECK_EQ(plain.Open("dlp:SRVR", &rc), 0u);
    TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);

    const size_t mark = LogSize();
    const Handle pu = plain.Open("nwm::UDS"), hu = hubbed.Open("nwm::UDS");
    const Handle pn = plain.Open("ndm:u"), hn = hubbed.Open("ndm:u");
    TCHECK(pu != 0 && hu != 0 && pn != 0 && hn != 0);

    // Every id of the service (and the ids around it), with an empty body, before Initialize, after it, hosting, and after Shutdown, on both consoles.
    // The words of the replies must be the same, but for the handles the kernel makes (word 3 of 0x01, 0x1B, 0x12). Not run: the old ConnectToNetwork
    // (0x09, it waits for a host that is not there).
    struct Step {
        const char* what;
        std::vector<u32> request;
    };
    std::vector<Step> script;
    auto all_ids = [&](const char* phase) {
        for (u32 id = 0; id <= 0x26; ++id) {
            if (id == 0x09 || id == 0x01 || id == 0x1B || id == 0x03 || id == 0x1D || id == 0x04) {
                continue;
            }
            script.push_back({phase, req::Make(id, 0, 0)});
        }
        script.push_back({phase, std::vector<u32>{0xFFFFu << 16}});
    };
    all_ids("before Initialize");
    script.push_back({"Initialize", req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)});
    all_ids("initialised");
    NetworkInfo own = MakeNetworkInfo(4, 6);
    own.channel = 6;
    script.push_back({"BeginHosting", req::BeginHosting(&own)});
    all_ids("hosting");
    script.push_back({"Bind", req::Bind(1, 0x2E30, 0xF3, 0xFFFF)});
    script.push_back({"Pull", req::Pull(1)});
    script.push_back({"GetNodeInformation", req::GetNodeInformation(1)});
    script.push_back({"Destroy", req::Destroy()});
    script.push_back({"Shutdown", req::Shutdown()});
    all_ids("after Shutdown");
    int differ = 0;
    size_t i = 0;
    for (const Step& step : script) {
        const u32 id = step.request[0] >> 16;
        // (the words after the request are zero here: an id with an empty body reads null pointers, never the stale pattern)
        const Rep r0 = Do(plain, pu, step.request, nullptr, 0, false, 0);
        const Rep r1 = Do(hubbed, hu, step.request, nullptr, 0, false, 0);
        TCHECK(r0.handled && r1.handled);
        u32 n0 = std::min<u32>(1 + ((r0.w[0] >> 6) & 0x3F) + (r0.w[0] & 0x3F), 64);
        u32 n1 = std::min<u32>(1 + ((r1.w[0] >> 6) & 0x3F) + (r1.w[0] & 0x3F), 64);
        bool same = n0 == n1;
        for (u32 k = 0; same && k < n0; ++k) {
            const bool handle_word = k == 3 && (id == 0x01 || id == 0x1B || id == 0x12);
            if (!handle_word && r0.w[k] != r1.w[k]) {
                same = false;
            }
        }
        if (!same) {
            ++differ;
            std::fprintf(stderr, "  DIFFER at step %zu (%s, id 0x%02x): 0x%08x 0x%08x vs 0x%08x 0x%08x\n", i, step.what, id, r0.w[0], r0.w[1], r1.w[0], r1.w[1]);
        }
        ++i;
    }
    TCHECK_EQ(differ, 0);
    std::printf("  L6: %zu commands, %d differ\n", script.size(), differ);
    TCHECK_EQ(plain.ds.hle->requests(), hubbed.ds.hle->requests());
    // ndm:u: one command each
    const Rep n0 = Do(plain, pn, req::NdmEnter(1));
    const Rep n1 = Do(hubbed, hn, req::NdmEnter(1));
    TCHECK_EQ(n0.w[0], n1.w[0]);
    TCHECK_EQ(n0.w[1], n1.w[1]);
    // the idle hub was never called, and nothing was logged about it
    TCHECK_EQ(hubbed.hub->served_inline() + hubbed.hub->served_posted() + hubbed.hub->ran_here() + hubbed.hub->failed_fast() + hubbed.hub->cancelled() + hubbed.hub->deferred_closes(), 0u);
    TCHECK(!hubbed.hub->EngineHeld());
    TCHECK_EQ(LogCount("dlp:", mark), 0);
    TCHECK_EQ(LogCount("while the Download Play lobby holds the network", mark), 0);
    plain.Close(pu);
    hubbed.Close(hu);
    plain.Close(pn);
    hubbed.Close(hn);
}

// ---------------------------------------------------------------------------------------------------------------------
// L8: statistics.

// L10 (mutation round of phase 3): names that only BEGIN like the fake services are the real services' business (the interception compares all 8
// characters of the name), and a dlp session that only called Finalize (never Initialize) ends no role when it is closed.
static void L10() {
    std::printf("L10: names that only begin like dlp:FKCL / dlp:SRVR are not taken; a session that only called Finalize ends nothing\n");
    g_rules.Reset();
    MockRoom room;
    Lc c(room, 1, "Alice", true, true);
    const size_t mark = LogSize();
    const char* const names[] = {"dlp:SRVx", "dlp:FKCx", "dlp:SRV", "dlp:FKC", "dlp:CLNT", "xlp:SRVR", "dlp;FKCL", "DLP:SRVR", "dlp:SRV\x01"};
    for (const char* n : names) {
        Result rc = 0;
        TCHECK_EQ(c.Open(n, &rc), 0u);
        TCHECK_EQ(static_cast<u32>(rc), R_SRV_REFUSED);
    }
    TCHECK_EQ(LogCount("uds: game opened dlp:", mark), 0);
    const Handle fk = c.Open("dlp:FKCL");
    const Handle sv = c.Open("dlp:SRVR");
    TCHECK(fk != 0 && sv != 0 && fk != sv);
    TCHECK_EQ(LogCount("uds: game opened dlp:FKCL", mark), 1);
    TCHECK_EQ(LogCount("uds: game opened dlp:SRVR", mark), 1);
    // Finalize without Initialize, then the close
    (void)Do(c, fk, req::Make(0x02, 0, 0));
    (void)Do(c, sv, req::Make(0x02, 0, 0));
    const u32 posted = c.hub->served_posted();
    const size_t mark2 = LogSize();
    c.Close(fk);
    c.Close(sv);
    TCHECK_EQ(LogCount("its role ends with it", mark2), 0);
    TCHECK_EQ(c.hub->served_posted(), posted);  // (no close was posted)
    TCHECK_EQ(c.hub->deferred_closes(), 0u);
    TCHECK_EQ(c.hub->failed_fast() + c.hub->cancelled(), 0u);
}

// L11 (mutation round of phase 3): the game starts nwm::UDS while the searcher only SCANS - the lobby is active but holds no engine (EngineHeld() is
// false). The front-end must still ask the hub to give back (a scan is not the engine's, but it is the lobby's): the scan ends, the status is "idle".
static void L11() {
    std::printf("L11: the game's nwm::UDS Initialize while the searcher only scans: the scan is given back too (EngineHeld() was false)\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Game g(a, true, false);
    TCHECK_EQ(g.FkInitName("Bob", 4).res(), 0u);
    TCHECK_EQ(g.FkScan().res(), 0u);
    TCHECK(!a.hub->EngineHeld());
    const u32 scanning = g.StatusState();
    TCHECK(scanning != 0u && scanning != 1u);  // (2: scanning)
    const size_t mark = LogSize();
    const Handle uds = a.Open("nwm::UDS");
    TCHECK(uds != 0);
    TCHECK_EQ(Do(a, uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)).res(), 0u);
    TCHECK(a.e().initialized());                  // (the game's own network)
    TCHECK_EQ(g.StatusState(), 1u);               // (the scan is over: the searcher is idle)
    TCHECK_EQ(LogCount("while the Download Play lobby held the network: given back first", mark), 0);  // (it held none)
    TCHECK_EQ(UdsStatus(a, uds).res(), 0u);
    a.Close(uds);
    a.Close(g.fk());
}

static void L8() {
    std::printf("L8: a hub-served dlp command is not accounted or throttled; the probe's and nwm::UDS commands are, as always\n");
    g_rules.Reset();
    MockRoom room0, room1;
    Lc h(room0, 1, "Alice", true, true);                   // a game with a hub
    Lc p(room1, 2, "Bob", false, false, true);             // Battle Royale: the probe
    Game gh(h, true, true);
    const Handle hu = h.Open("nwm::UDS"), pu = p.Open("nwm::UDS");
    const Handle pfk = p.Open("dlp:FKCL");
    TCHECK(pfk != 0);
    TCHECK_EQ(Do(p, pfk, InitNameRequest()).res(), 0u);
    TCHECK_EQ(gh.FkInitName("Bob", 4).res(), 0u);
    g_test_real_sleep = false;  // (the throttle's pause is counted by the shim: 0.5 ms a call is not slept here)

    // (1) a thousand kNoData in a row from the hub (GetTitleInfoInOrder with no group in the list) and sixty posted commands
    unsigned req0 = g_stats.ipc_requests;
    unsigned long long sum0 = g_stats.ipc_sum_ticks;
    unsigned thr0 = g_stats.hle_throttled;
    unsigned long long sleeps0 = g_test_sleep_calls.load();
    u32 requests0 = h.ds.hle->requests();
    int no_data = 0;
    for (int i = 0; i < 1000; ++i) {
        const Rep r = gh.FkInOrder(1);
        if (r.hdr() == Plain(9) && r.res() == R_NO_DATA) {
            ++no_data;
        }
    }
    TCHECK_EQ(no_data, 1000);
    for (int i = 0; i < 30; ++i) {
        TCHECK_EQ(gh.FkScan().res(), 0u);
        TCHECK_EQ(gh.FkStopScan().res(), 0u);
    }
    TCHECK_EQ(g_stats.ipc_requests, req0);
    TCHECK_EQ(g_stats.ipc_sum_ticks, sum0);
    TCHECK_EQ(g_stats.hle_throttled, thr0);
    TCHECK_EQ(g_test_sleep_calls.load(), sleeps0);  // (no pause at all: not the throttle's, not a join's)
    TCHECK_EQ(h.ds.hle->requests(), requests0);
    TCHECK_EQ(h.hub->served_inline(), 1000u);  // (the books of the hub: the thousand reads, and the 1 + 60 posted commands)
    TCHECK_EQ(h.hub->served_posted(), 61u);

    // (2) the probe's commands are accounted, and a thousand failing ones in a row are throttled after the free ones
    req0 = g_stats.ipc_requests;
    sum0 = g_stats.ipc_sum_ticks;
    thr0 = g_stats.hle_throttled;
    sleeps0 = g_test_sleep_calls.load();
    int refused = 0;
    for (int i = 0; i < 1000; ++i) {
        const Rep r = Do(p, pfk, req::Make(0x05, 6, 0));  // StartScan: "invalid state" for the probe
        if (r.res() == R_INVALID_STATE) {
            ++refused;
        }
    }
    TCHECK_EQ(refused, 1000);
    TCHECK_EQ(g_stats.ipc_requests - req0, 1000u);
    TCHECK(g_stats.ipc_sum_ticks > sum0);
    TCHECK(g_stats.hle_throttled - thr0 >= 900u);  // 50 free calls in a window of 100 ms, then every one waits
    TCHECK(g_test_sleep_calls.load() - sleeps0 >= 900u);

    // (3) nwm::UDS commands of the game with a hub: counted (10 successes), and failing ones throttled as ever (a pull of an unbound id)
    req0 = g_stats.ipc_requests;
    thr0 = g_stats.hle_throttled;
    requests0 = h.ds.hle->requests();
    for (int i = 0; i < 10; ++i) {
        TCHECK_EQ(UdsStatus(h, hu).res(), 0u);
    }
    TCHECK_EQ(g_stats.ipc_requests - req0, 10u);
    TCHECK_EQ(h.ds.hle->requests() - requests0, 10u);
    TCHECK_EQ(g_stats.hle_throttled, thr0);  // (successes: nothing to throttle)
    TCHECK_EQ(UdsInit(h, hu, 0xA1A1, "GameHost").res(), 0u);
    req0 = g_stats.ipc_requests;
    thr0 = g_stats.hle_throttled;
    std::vector<u8> out(0x5C8);
    int failing = 0;
    for (int i = 0; i < 300; ++i) {
        const Rep r = Do(h, hu, req::Pull(7), out.data(), static_cast<u32>(out.size()));
        if (r.res() != 0) {
            ++failing;
        }
    }
    TCHECK_EQ(failing, 300);
    TCHECK_EQ(g_stats.ipc_requests - req0, 300u);
    TCHECK(g_stats.hle_throttled - thr0 >= 200u);

    // (4) and the hub's commands are still not counted after all that
    req0 = g_stats.ipc_requests;
    thr0 = g_stats.hle_throttled;
    for (int i = 0; i < 100; ++i) {
        gh.FkInOrder(1);
        gh.SvState();
    }
    TCHECK_EQ(g_stats.ipc_requests, req0);
    TCHECK_EQ(g_stats.hle_throttled, thr0);
    g_test_real_sleep = true;
    TCHECK_EQ(Do(h, hu, req::Shutdown()).res(), 0u);
    h.Close(hu);
    p.Close(pu);
    p.Close(pfk);
    h.Close(gh.fk());
    h.Close(gh.sv());
}

// ---------------------------------------------------------------------------------------------------------------------
// L7: the 8 s cap of the join's wait.

static void L7() {
    std::printf("L7: a join the network thread never ends: server not found after 8 s, the client role finalised\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    Game ga(a, false, true), gb(b, true, false);
    OpenCreator(ga);
    TCHECK(FindGroup(gb, a.mac()));
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    TCHECK_EQ(ga.SvEnd().res(), 0u);  // the door shuts after the searcher froze its list: nobody answers the join
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const size_t mark = LogSize();
    const unsigned long long sleeps0 = g_test_sleep_calls.load(), sleep_ns0 = g_test_sleep_ns.load();
    Rep r;
    u64 t0 = 0, t1 = 0;
    std::thread th([&] {
        t0 = platform::NowMs();
        r = gb.FkJoin(a.mac());  // the request runs on the network thread; the join begins; then the wait is the front-end's
        t1 = platform::NowMs();
    });
    TCHECK(WaitFor(3000, [&] { return b.hub->EngineHeld(); }));  // the join began (it took the engine)
    b.StopNet();                                                  // ... and the network thread is gone: nobody ends it (the lobby would, in 3 s)
    th.join();
    const u32 elapsed = static_cast<u32>(t1 - t0);
    std::printf("  L7: the join was answered after %u ms\n", elapsed);
    TCHECK_EQ(r.hdr(), 0x000B0040u);
    TCHECK_EQ(r.res(), R_NOT_FOUND);
    TCHECK(elapsed >= 7900 && elapsed < 9500);
    const unsigned long long sleeps = g_test_sleep_calls.load() - sleeps0;
    TCHECK(sleeps >= 400);
    TCHECK_EQ(g_test_sleep_ns.load() - sleep_ns0, sleeps * 10000000ull);
    TCHECK_EQ(LogCount("dlp: dlp:FKCL StartFakeSession: the join did not end in 8 s (the network thread is not running it): answered \"server not found\"", mark), 1);
    // The cap asked the hub to end the client role: no network thread took the request, and nothing of the network side runs on the game's
    // thread, so the end is left for the next Tick of a network thread (the engine stays held, the join pending, until then)
    TCHECK_EQ(b.hub->ran_here(), 0u);
    TCHECK_EQ(b.hub->deferred_closes(), 1u);
    TCHECK_EQ(b.hub->failed_fast(), 1u);
    TCHECK(b.hub->EngineHeld());
    // That the reply is "server not found" and not "wireless off": the hub's trouble line (a command the network side did not run) is for a
    // command that failed, not for the end of a wait that was answered
    TCHECK_EQ(LogCount("answered \"wireless off\"", mark), 0);
    // The network thread comes back (the room is up again): its first Tick ends the role - the engine is free, the join gone, the session
    // back at "initialised"
    b.StartNet();
    TCHECK(WaitFor(3000, [&] { return !b.hub->EngineHeld(); }));
    TCHECK(!b.e().initialized());
    TCHECK_EQ(gb.StatusState(), 1u);
    TCHECK_EQ(b.hub->ran_here(), 0u);
    // It was the role's Finalize (the deferred close) that ended it, not the lobby's own timeout of the join (long past by now, it would
    // end the join all the same): the join the game was waiting for is forgotten, so a look at it finds none (kInvalidState), and not
    // the timeout's outcome (0xC920AFFE) waiting to be collected
    u32 words[64] = {};
    TCHECK(b.hub->PollPending(words));
    TCHECK_EQ(words[0], 0x000B0040u);
    TCHECK_EQ(words[1], R_INVALID_STATE);
    a.Close(ga.sv());
    b.Close(gb.fk());
}

// ---------------------------------------------------------------------------------------------------------------------
// L12: an initialised nwm::UDS session is closed while the lobby holds the engine.

/// `held`: the game's session called Initialize and then Shutdown (it keeps the handle), and the lobby took the engine afterwards (a creator that
/// accepts). `!held` is the control: the session owns the engine (Initialize, no Shutdown) and no lobby holds anything. `dead_path`: the handle is
/// closed behind the hook's back (svcCloseHandle without NoteHandleClosed) and found dead on the next call through the value (svcClearEvent fails).
static void L12One(bool dead_path, bool held) {
    std::printf("L12: an initialised nwm::UDS session %s (%s)\n", dead_path ? "found dead" : "closed through the hook",
                held ? "the lobby holds the engine" : "control: no lobby, the engine is the game's");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    Game ga(a, false, true), gb(b, true, false);
    const Handle s1 = a.Open("nwm::UDS");
    TCHECK(s1 != 0);
    TCHECK_EQ(UdsInit(a, s1, 0xA1A1, "GameHost").res(), 0u);  // (marks the session: it called Initialize)
    TCHECK(a.e().initialized());
    if (held) {
        TCHECK_EQ(Do(a, s1, req::Shutdown()).res(), 0u);  // the game ends its network and keeps the session handle ...
        TCHECK(!a.e().initialized());
        OpenCreator(ga);  // ... and the lobby takes the engine
        TCHECK(a.hub->EngineHeld());
        TCHECK(a.e().initialized());
        TCHECK_EQ(ga.ServerState(), 2u);
    } else {
        TCHECK(!a.hub->EngineHeld());
    }
    const u32 posted = a.hub->served_posted();
    const size_t mark = LogSize();
    if (dead_path) {
        svcCloseHandle(s1);  // a close the hook did not see ...
        const Rep r = Do(a, s1, req::GetConnectionStatus());  // ... found on the next call through the value
        TCHECK(!r.handled);                                   // (the hook declines: the value names something else now)
    } else {
        a.Close(s1);
    }

    if (!held) {
        // The control: nothing holds the engine, so the close shuts the service down, as the real nwm does when a game quits without Shutdown.
        TCHECK(!a.e().initialized());
        TCHECK(!a.hub->EngineHeld());
        if (!dead_path) {
            TCHECK_EQ(LogCount("uds: that session had called Initialize: the service shuts down with it", mark), 1);
        }
        TCHECK_EQ(a.hub->served_posted(), posted);  // (the hub was not asked for anything)
        TCHECK_EQ(a.hub->ran_here() + a.hub->deferred_closes() + a.hub->failed_fast() + a.hub->cancelled(), 0u);
        a.Close(ga.sv());  // (never initialised: nothing)
        return;
    }

    // The lobby's network is not the closed session's: the engine is still initialised, still held, the creator still accepting, and
    // nothing was asked of the hub (no request, nothing run or deferred).
    TCHECK(a.e().initialized());
    TCHECK(a.hub->EngineHeld());
    TCHECK_EQ(ga.ServerState(), 2u);
    TCHECK_EQ(a.hub->served_posted(), posted);
    TCHECK_EQ(a.hub->ran_here() + a.hub->deferred_closes() + a.hub->failed_fast() + a.hub->cancelled(), 0u);
    const Handle s2 = a.Open("nwm::UDS");  // a session of the game that comes next: the gate still stands
    TCHECK(s2 != 0);
    TCHECK_EQ(UdsStatus(a, s2).res(), R_WRONG_STATE);
    // ... and the lobby still works end to end: a searcher finds the group and joins it.
    TCHECK(FindGroup(gb, a.mac(), 5000));
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    TCHECK_EQ(gb.FkJoin(a.mac()).res(), 0u);
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 1; }));
    TCHECK_EQ(ga.ServerState(), 2u);
    TCHECK(a.hub->EngineHeld());
    TCHECK(a.e().initialized());
    TCHECK_EQ(ga.SvFinalize().res(), 0u);
    TCHECK(!a.hub->EngineHeld());
    TCHECK_EQ(gb.FkFinalize().res(), 0u);
    a.Close(s2);
    a.Close(ga.sv());
    b.Close(gb.fk());
}

static void L12() {
    L12One(false, true);
    L12One(true, true);
    L12One(false, false);
    L12One(true, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// L13: the commands that succeed in every state, with nobody to serve them.

/// Makes the NEXT "wireless off" trouble event of the process one that gets a line (the first 8 and one in 64 do: NoteDlpTrouble), by making the
/// events in between - SRVR StartAccepting, which nobody serves while the network side is gone. After this a trouble event of the code under
/// test shows as a line, and no line means it made none, whatever came before in this long run.
static void AlignTrouble(Game& g) {
    for (;;) {
        const unsigned next = g_trouble_events.load() + 1;
        if (next <= 8 || next % 64 == 0) {
            return;
        }
        TCHECK_EQ(g.SvAccepting(0, 6).res(), R_WIRELESS_OFF);
        Trouble(1);
    }
}

static int TroubleLines(size_t from) {
    return LogCount("answered \"wireless off\": the network side did not run it", from);
}

/// One command of the family that succeeds in every state, through the front-end, while no network thread serves: the reply is
/// [(id << 16) | 0x40, 0], the hub counts a request refused at once, runs nothing, and the log has no "wireless off" line for it. A command that
/// ends a role (`ends_role`) is remembered for the next Tick (deferred_closes).
static void Always(Lc& c, Game& g, const char* what, const std::function<Rep()>& call, u32 id, bool ends_role) {
    AlignTrouble(g);
    const int fails0 = g_tfails.load();
    const size_t mark = LogSize();
    const u32 fast = c.hub->failed_fast(), cancelled = c.hub->cancelled(), deferred = c.hub->deferred_closes();
    const u32 posted = c.hub->served_posted(), here = c.hub->ran_here();
    const Rep r = call();
    TCHECK_EQ(r.hdr(), Plain(id));
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(TroubleLines(mark), 0);
    TCHECK_EQ(c.hub->failed_fast(), fast + 1);  // (nobody was asked: refused at once)
    TCHECK_EQ(c.hub->cancelled(), cancelled);
    TCHECK_EQ(c.hub->deferred_closes(), deferred + (ends_role ? 1u : 0u));
    TCHECK_EQ(c.hub->served_posted(), posted);
    TCHECK_EQ(c.hub->ran_here(), here);
    if (g_tfails.load() != fails0) {
        std::fprintf(stderr, "  ^ in: %s\n", what);
    }
}

/// A command outside the family, while no network thread serves: the module's "wireless off" and the trouble line that says so.
static void Refused(Lc& c, Game& g, const char* what, const std::function<Rep()>& call, bool srvr, u32 id) {
    AlignTrouble(g);
    const int fails0 = g_tfails.load();
    const size_t mark = LogSize();
    const u32 fast = c.hub->failed_fast(), deferred = c.hub->deferred_closes();
    const Rep r = call();
    Trouble(1);
    TCHECK_EQ(r.hdr(), Plain(id));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    char text[96];
    std::snprintf(text, sizeof(text), "dlp: %s command 0x%02x answered \"wireless off\"", srvr ? "dlp:SRVR" : "dlp:FKCL", id);
    TCHECK_EQ(LogCount(text, mark), 1);
    TCHECK_EQ(TroubleLines(mark), 1);
    TCHECK_EQ(c.hub->failed_fast(), fast + 1);
    TCHECK_EQ(c.hub->deferred_closes(), deferred);  // (it ends no role)
    if (g_tfails.load() != fails0) {
        std::fprintf(stderr, "  ^ in: %s\n", what);
    }
}

/// (a) A hub nobody ever ticked: the family succeeds, StartAccepting / StartScan do not, and a network thread that comes up later finds a hub that serves.
static void L13Never() {
    std::printf("L13: a hub nobody ever ticked: the family succeeds without a trouble line, the rest says \"wireless off\" with one\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, false);
    Game g(a, true, true);
    const Handle au = a.Open("nwm::UDS");
    Always(a, g, "never ticked: FKCL StopScan", [&] { return g.FkStopScan(); }, 0x06, false);
    Always(a, g, "never ticked: FKCL StopSession", [&] { return g.FkStopSession(); }, 0x10, true);
    Always(a, g, "never ticked: FKCL Finalize", [&] { return g.FkFinalize(); }, 0x02, true);
    Always(a, g, "never ticked: SRVR EndAccepting", [&] { return g.SvEnd(); }, 0x06, false);
    Always(a, g, "never ticked: SRVR Finalize", [&] { return g.SvFinalize(); }, 0x02, true);
    Refused(a, g, "never ticked: SRVR StartAccepting", [&] { return g.SvAccepting(0, 6); }, true, 0x05);
    Refused(a, g, "never ticked: FKCL StartScan", [&] { return g.FkScan(); }, false, 0x05);
    TCHECK_EQ(a.hub->deferred_closes(), 3u);  // FKCL StopSession, FKCL Finalize, SRVR Finalize (StopScan and EndAccepting end nothing)
    TCHECK_EQ(a.hub->ran_here(), 0u);
    TCHECK_EQ(a.hub->served_posted(), 0u);
    TCHECK(!a.hub->EngineHeld());
    a.StartNet();  // the first Tick takes the deferred ends (nothing is initialised here: they end nothing) and the hub serves from then on
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.ServerState(), 1u);
    TCHECK_EQ(UdsStatus(a, au).res(), 0u);
    TCHECK_EQ(a.hub->deferred_closes(), 3u);
    a.Close(g.fk());
    a.Close(g.sv());
    a.Close(au);
}

/// (b) the network threads silent for more than a second (`stop_hub` false: nobody ticks, the room link is lost) and (c) after hub.Stop() (the
/// plugin is ending; here the threads still tick). A creator holds the engine and a searcher scans; the family succeeds without running, the roles
/// it ends are ended by the next Tick - the engine is free, the lobby is back at "off", the nwm::UDS gate is off.
static void L13Roles(bool stop_hub) {
    std::printf("L13: %s: the family succeeds, the roles it ends are ended by the next Tick\n",
                stop_hub ? "after hub.Stop()" : "network threads silent for more than a second");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    Game ga(a, true, true), gb(b, true, true);
    const Handle au = a.Open("nwm::UDS"), bu = b.Open("nwm::UDS");
    OpenCreator(ga);                               // A: the creator, accepting, holds the engine
    TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);  // B: a searcher, scanning (needs no engine)
    TCHECK_EQ(gb.FkScan().res(), 0u);
    TCHECK(a.hub->EngineHeld());
    TCHECK(!b.hub->EngineHeld());
    TCHECK_EQ(ga.ServerState(), 2u);
    TCHECK_EQ(gb.StatusState(), 2u);
    if (stop_hub) {
        a.hub->Stop();
        b.hub->Stop();
    } else {
        a.StopNet();
        b.StopNet();
        std::this_thread::sleep_for(std::chrono::milliseconds(1150));  // (past the second after which the hub counts the network thread as gone)
    }
    // The commands that end nothing come first: they must not run (the state stays), and nothing may have been ended by a Tick before them.
    Always(a, ga, "SRVR EndAccepting", [&] { return ga.SvEnd(); }, 0x06, false);
    TCHECK_EQ(ga.ServerState(), 2u);  // (it did not run: the door is still open)
    TCHECK(a.hub->EngineHeld());
    TCHECK(a.e().initialized());
    TCHECK_EQ(UdsStatus(a, au).res(), R_WRONG_STATE);  // (the gate stands: the lobby's network is still the lobby's)
    Refused(a, ga, "SRVR StartAccepting", [&] { return ga.SvAccepting(0, 6); }, true, 0x05);
    Always(b, gb, "FKCL StopScan", [&] { return gb.FkStopScan(); }, 0x06, false);
    TCHECK_EQ(gb.StatusState(), 2u);  // (it did not run: still scanning)
    Refused(b, gb, "FKCL StartScan", [&] { return gb.FkScan(); }, false, 0x05);
    // The ones that end a role: the role is left for the next Tick of a network thread
    Always(b, gb, "FKCL StopSession", [&] { return gb.FkStopSession(); }, 0x10, true);
    Always(b, gb, "FKCL Finalize", [&] { return gb.FkFinalize(); }, 0x02, true);
    Always(a, ga, "SRVR Finalize", [&] { return ga.SvFinalize(); }, 0x02, true);
    TCHECK_EQ(a.hub->deferred_closes(), 1u);
    TCHECK_EQ(b.hub->deferred_closes(), 2u);
    TCHECK_EQ(a.hub->ran_here() + b.hub->ran_here(), 0u);
    if (!stop_hub) {
        // (nothing ticked in between: nothing has been ended yet)
        TCHECK_EQ(ga.ServerState(), 2u);
        TCHECK_EQ(gb.StatusState(), 2u);
        TCHECK(a.hub->EngineHeld());
        TCHECK_EQ(UdsStatus(a, au).res(), R_WRONG_STATE);
        a.StartNet();  // the network threads return
        b.StartNet();
    }
    // A Tick ends them: the engine is free, the creator is off, the searcher is back at "initialised", the gate is off.
    TCHECK(WaitFor(3000, [&] { return !a.hub->EngineHeld(); }));
    TCHECK(WaitFor(3000, [&] { return ga.ServerState() == 0; }));
    TCHECK(WaitFor(3000, [&] { return gb.StatusState() == 1; }));
    TCHECK(!a.e().initialized());
    TCHECK(!b.e().initialized());
    TCHECK_EQ(UdsStatus(a, au).res(), 0u);
    TCHECK_EQ(UdsStatus(b, bu).res(), 0u);
    TCHECK_EQ(a.hub->deferred_closes(), 1u);
    TCHECK_EQ(b.hub->deferred_closes(), 2u);
    TCHECK_EQ(a.hub->ran_here() + b.hub->ran_here(), 0u);
    a.Close(ga.fk());
    a.Close(ga.sv());
    a.Close(au);
    b.Close(gb.fk());
    b.Close(gb.sv());
    b.Close(bu);
}

/// (d) a request the network thread does not take in 2 s (alive, but stalled) is cancelled: the family answers success then too (the hub's
/// `cancelled`, not `failed_fast`), no trouble line, and the role it ends is ended when the thread comes back.
static void L13Stalled() {
    std::printf("L13: a Finalize the network thread does not take in 2 s: success, cancelled, no trouble line\n");
    g_rules.Reset();
    MockRoom room;
    Lc a(room, 1, "Alice", true, false);
    Game g(a, false, true);
    AlignTrouble(g);  // (while nobody ticks: these are refused at once)
    a.StartNet();
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.ServerState(), 1u);
    const size_t mark = LogSize();
    const u32 fast = a.hub->failed_fast();
    a.net->Pause(true);
    const u64 t0 = platform::NowMs();
    const Rep r = g.SvFinalize();
    const u32 waited = Ms(t0);
    TCHECK_EQ(r.hdr(), Plain(2));
    TCHECK_EQ(r.res(), 0u);
    TCHECK(waited >= 1900 && waited < 3500);
    TCHECK_EQ(a.hub->cancelled(), 1u);
    TCHECK_EQ(a.hub->failed_fast(), fast);
    TCHECK_EQ(TroubleLines(mark), 0);
    TCHECK_EQ(a.hub->deferred_closes(), 1u);
    TCHECK_EQ(g.ServerState(), 1u);  // (it never ran)
    a.net->Pause(false);
    TCHECK(WaitFor(3000, [&] { return g.ServerState() == 0; }));  // the next Tick ends the role
    a.Close(g.sv());
}

/// This binary again, in a fresh process, with the given arguments: true if it passed. (What the scenarios that count on the process' own statics -
/// the first eight "wireless off" trouble lines of NoteDlpTrouble, the table of NoteCommand - need: nothing of the other scenarios in them.)
static bool RunChild(const char* flag, const char* arg) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        return false;
    }
    exe[n] = 0;
    const std::string cmd = std::string("\"") + exe + "\" " + flag + (arg != nullptr ? std::string(" ") + arg : std::string());
    std::fflush(stdout);
    std::fflush(stderr);
    return std::system(cmd.c_str()) == 0;
}

/// The whole of L13 in a FRESH process: the trouble lines are the first eight events of a process and then one in 64, so here every event of the
/// scenarios (six) has its line - a missing line is the code's, not the count's - and AlignTrouble finds nothing to align.
static int L13Child() {
    g_rules.Reset();
    L13Never();
    L13Roles(false);
    L13Roles(true);
    L13Stalled();
    TCHECK_EQ(g_trouble_events.load(), 6u);  // (StartAccepting and StartScan refused in each of the first three scenarios, none in the last)
    TCHECK_EQ(TroubleLines(0), 6);
    std::printf("%d checks, %d failures\n", g_checks + g_tchecks.load(), g_failures + g_tfails.load());
    return (g_failures + g_tfails.load()) ? 1 : 0;
}

static void L13() {
    TCHECK(RunChild("--l13", nullptr));
}

// ---------------------------------------------------------------------------------------------------------------------
// L14: the table of the first-call lines (NoteCommand) in the log.

/// Does the log hold the line NoteCommand writes when it first sees this (command, result) of `service` ("uds", "ndm", "dlp" for the probe, "dlp:FKCL",
/// "dlp:SRVR")? A pair that is not remembered is not written, so a pair driven once has exactly one line or none.
static bool HasPairLine(const char* service, u32 id, u32 result) {
    char text[80];
    std::snprintf(text, sizeof(text), "%s: command 0x%02x -> 0x%08x", service, id, result);
    return LogCount(text) > 0;
}

/// 60 distinct (command, result) pairs through a game WITHOUT a hub - nwm::UDS (30 unknown ids), the probe's dlp:FKCL (10) and ndm:u (20) - in
/// this order. `room_left` of them fit in the table: the first ones have a line, the rest none. Returns how many have.
static int PlainBatch(Lc& g, Handle uds, Handle probe, Handle ndm, int room_left) {
    int logged = 0, i = 0;
    auto one = [&](const char* service, Handle h, u32 id, u32 result) {
        const Rep r = Do(g, h, std::vector<u32>{id << 16});
        TCHECK_EQ(r.res(), result);
        const bool line = HasPairLine(service, id, result);
        if (line != (i < room_left)) {
            std::fprintf(stderr, "  plain pair %d (%s 0x%02x): %s\n", i, service, id, line ? "has a line, should be silent" : "is silent, should have a line");
        }
        TCHECK_EQ(line ? 1 : 0, i < room_left ? 1 : 0);
        logged += line ? 1 : 0;
        ++i;
    };
    for (u32 id = 0x24; id < 0x24 + 30; ++id) {
        one("uds", uds, id, R_UNKNOWN_UDS_COMMAND);
    }
    for (u32 id = 0x12; id < 0x12 + 10; ++id) {
        one("dlp", probe, id, R_BAD_HEADER);
    }
    for (u32 id = 0x30; id < 0x30 + 20; ++id) {
        one("ndm", ndm, id, 0);
    }
    return logged;
}

/// 40 distinct pairs through a game with a hub: dlp:FKCL (20 ids the service does not have) then dlp:SRVR (20). `room_left` of them fit.
static int HubBatch(Game& g, int room_left) {
    int logged = 0, i = 0;
    auto one = [&](bool srvr, u32 id) {
        const Rep r = srvr ? g.Sv(id << 16, {}) : g.Fk(id << 16, {});
        TCHECK_EQ(r.res(), R_BAD_HEADER);
        const bool line = HasPairLine(srvr ? "dlp:SRVR" : "dlp:FKCL", id, R_BAD_HEADER);
        if (line != (i < room_left)) {
            std::fprintf(stderr, "  hub pair %d (%s 0x%02x): %s\n", i, srvr ? "dlp:SRVR" : "dlp:FKCL", id, line ? "has a line, should be silent" : "is silent, should have a line");
        }
        TCHECK_EQ(line ? 1 : 0, i < room_left ? 1 : 0);
        logged += line ? 1 : 0;
        ++i;
    };
    for (u32 id = 0x12; id < 0x12 + 20; ++id) {
        one(false, id);
    }
    for (u32 id = 0x11; id < 0x11 + 20; ++id) {
        one(true, id);
    }
    return logged;
}

/// The whole of L14 in a FRESH process (the table is the process's, filled by every scenario before): the plain batch and the hub batch in the
/// order given. 48 of the 60 plain pairs and 32 of the 40 hub pairs have a line, whichever table was filled first.
static int L14Child(const char* order) {
    const bool plain_first = std::strcmp(order, "plain-first") == 0;
    std::printf("L14 (%s): 60 pairs through a game without a hub, 40 through a game with one, in a fresh process\n", order);
    g_rules.Reset();
    MockRoom room0, room1;
    Lc plain(room0, 1, "Alice", false, false, true);  // no hub; the probe on (the dlp:FKCL of Battle Royale)
    Lc hubbed(room1, 2, "Bob", true, false);           // a hub (its dlp services answer the unknown ids inline: no network thread needed)
    TCHECK(plain.hub == nullptr);
    const Handle uds = plain.Open("nwm::UDS"), ndm = plain.Open("ndm:u"), probe = plain.Open("dlp:FKCL");
    TCHECK(uds != 0 && ndm != 0 && probe != 0);
    Game g(hubbed, true, true);
    int plain_logged = 0, hub_logged = 0;
    if (plain_first) {
        plain_logged = PlainBatch(plain, uds, probe, ndm, 48);
        hub_logged = HubBatch(g, 32);
    } else {
        hub_logged = HubBatch(g, 32);
        plain_logged = PlainBatch(plain, uds, probe, ndm, 48);
    }
    TCHECK_EQ(plain_logged, 48);  // exactly what the table held before the hub; the 49th pair on is silent
    TCHECK_EQ(hub_logged, 32);    // the hub's two services have 32 of their own
    std::printf("  L14 (%s): %d of 60 plain pairs and %d of 40 hub pairs have a line\n", order, plain_logged, hub_logged);
    plain.Close(uds);
    plain.Close(ndm);
    plain.Close(probe);
    hubbed.Close(g.fk());
    hubbed.Close(g.sv());
    std::printf("%d checks, %d failures\n", g_checks + g_tchecks.load(), g_failures + g_tfails.load());
    return (g_failures + g_tfails.load()) ? 1 : 0;
}

static void L14() {
    std::printf("L14: the table of first-call lines of the log: 48 pairs for a game without a hub, 32 of their own for the hub's services\n");
    TCHECK(RunChild("--l14", "plain-first"));
    TCHECK(RunChild("--l14", "hub-first"));
}

// ---------------------------------------------------------------------------------------------------------------------
// L15: the trace of the hub's dlp commands (the "dlpc: " lines of the debug log: what the console test will have to be read from).
//
// The spec of the trace, written out here and not read from the code:
//   * debug level 0: no line, and nothing remembered for the next time the level is 1;
//   * debug level 1: one line per command of dlp:FKCL / dlp:SRVR, in the order the game made them,
//         dlpc: <dlp:FKCL|dlp:SRVR> 0x<id> req <6 words> -> 0x<result> rep[ <words>] (<n> ms[, <m> more like it since the last line])
//     the six words are the first six words after the request header (0 beyond what the header declares), taken BEFORE the reply replaces
//     them; the rep words are the ones after the result that the reply header declares (five at the most); n is how long the call took;
//   * each command (of each service) has a WINDOW of 2 s that begins at its first line and remembers the replies (result and shown words)
//     written in it, six at the most. A call whose reply is one of them writes no line and is counted: the next line of the command says
//     m = the calls left out since the last line. A reply that is NEW in the window - a state that changed (the result included), a group
//     that appeared, the other one of a group / "no data" pair - is a line at the very call that made it. The first call after the window is
//     over is a line whatever its reply, and begins the next window. A seventh distinct reply in one window is written at every call (it is
//     not remembered: loud, never silent). THE INTENDED LIMIT: a return to a reply the window already holds is counted, not written (a
//     status that goes 5 -> 6 -> 9 -> 5 within 2 s is three lines, the last 5 is in the count of the next line): the log has to hold
//     Triple Deluxe's polls of GetTitleInfoInOrder(1) then (0) every frame, which alternate between a few replies;
//   * ids from 0x12 on make no line (the services have none), the ids below that do, the unknown ones too;
//   * only for the commands the hub serves: not for the probe of Battle Royale, not for a game without a hub, not for nwm::UDS / ndm:u;
//   * the first command of each dlp service, once in the life of the process, is a line of its own BEFORE the command's trace line:
//         dlp: the first command of <service> is 0x<id>: the Download Play lobby is carried by the room (thread <n>, priority 0x<p>)
//     n (decimal) and p (hex) are the thread that makes the call and its priority (the hub's threads wait for each other's locks and a
//     LightLock has no priority inheritance: the console test reads from this line where the game's dlp threads stand against the network
//     thread's 0x2F). The fake kernel of the 32-bit tests answers thread 0x1234 = 4660 at priority 0x30.
// L15 runs in six fresh processes ("--l15 off|td|flood|alt|refused|probe"): the slots of the trace are the process's, a line the log's lock
// gave up (a thread preempted inside the sink) voids a run, and so does a timed part that overran its window (a machine too busy for 2 s
// to hold the calls it makes); a void run ends with 77 and is made again.
//
// The books are kept by CheckTraceAgainstJournal against the journal of the calls: the model of the window below is the one written
// here, followed call by call with the clocks of the journal (a call is certainly inside the window, certainly outside it, or too close
// to the 2 s to say - then either is accepted).

struct TraceLine {
    std::string text;
    bool srvr = false;
    u32 id = 0;
    u32 req[6] = {};
    u32 result = 0;
    std::vector<u32> rep;
    u32 ms = 0;
    bool has_more = false;
    u32 more = 0;
};

static std::vector<std::string> SplitSpaces(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = s.find(' ', i);
        if (j == std::string::npos) {
            out.push_back(s.substr(i));
            break;
        }
        out.push_back(s.substr(i, j - i));  // (an empty token is a double space, which the format has none of: it fails the parse below)
        i = j + 1;
    }
    return out;
}
static bool IsLowerHex(const std::string& s, size_t digits) {
    if (s.size() != digits) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}
static bool IsDecimal(const std::string& s) {
    if (s.empty() || s.size() > 10) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}
static u32 HexOf(const std::string& s) {
    return static_cast<u32>(std::strtoul(s.c_str(), nullptr, 16));
}
static u32 DecOf(const std::string& s) {
    return static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
}

/// One "dlpc: " line, strictly: false if it is not exactly the format above.
static bool ParseTrace(const std::string& text, TraceLine* t) {
    t->text = text;
    const std::vector<std::string> w = SplitSpaces(text);
    const size_t n = w.size();
    if (n < 15 || w[0] != "dlpc:") {
        return false;
    }
    if (w[1] == "dlp:FKCL") {
        t->srvr = false;
    } else if (w[1] == "dlp:SRVR") {
        t->srvr = true;
    } else {
        return false;
    }
    if (w[2].size() != 4 || w[2].compare(0, 2, "0x") != 0 || !IsLowerHex(w[2].substr(2), 2)) {
        return false;
    }
    t->id = HexOf(w[2].substr(2));
    if (w[3] != "req") {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (!IsLowerHex(w[4 + i], 8)) {
            return false;
        }
        t->req[i] = HexOf(w[4 + i]);
    }
    if (w[10] != "->" || w[11].size() != 10 || w[11].compare(0, 2, "0x") != 0 || !IsLowerHex(w[11].substr(2), 8) || w[12] != "rep") {
        return false;
    }
    t->result = HexOf(w[11].substr(2));
    size_t k = 13;
    t->rep.clear();
    while (k < n && IsLowerHex(w[k], 8)) {
        t->rep.push_back(HexOf(w[k]));
        ++k;
    }
    if (t->rep.size() > 5 || k + 2 > n || w[k].size() < 2 || w[k][0] != '(' || !IsDecimal(w[k].substr(1))) {
        return false;
    }
    t->ms = DecOf(w[k].substr(1));
    if (w[k + 1] == "ms)" && k + 2 == n) {
        t->has_more = false;
        t->more = 0;
        return true;
    }
    static const char* const kTail[] = {"more", "like", "it", "since", "the", "last", "line)"};
    if (w[k + 1] != "ms," || n != k + 10 || !IsDecimal(w[k + 2])) {
        return false;
    }
    for (size_t i = 0; i < 7; ++i) {
        if (w[k + 3 + i] != kTail[i]) {
            return false;
        }
    }
    t->has_more = true;
    t->more = DecOf(w[k + 2]);
    return true;
}

/// Every "dlpc: " line of the log from index `from` on, parsed; a line that does not parse fails (and is shown), and so does a log line that
/// has "dlpc:" anywhere but at its start.
static std::vector<TraceLine> ReadTrace(size_t from) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> l(g_log_mutex);
        for (size_t i = from; i < g_log.size(); ++i) {
            if (g_log[i].compare(0, 6, "dlpc: ") == 0) {
                lines.push_back(g_log[i]);
            }
        }
    }
    TCHECK_EQ(LogCount("dlpc:", from), static_cast<int>(lines.size()));
    std::vector<TraceLine> out;
    for (const std::string& s : lines) {
        TraceLine t;
        const bool ok = ParseTrace(s, &t);
        if (!ok) {
            std::fprintf(stderr, "  not a trace line: %s\n", s.c_str());
        }
        TCHECK(ok);
        if (ok) {
            out.push_back(t);
        }
    }
    return out;
}

/// The index in the log (from `from` on) of the first line that contains `text`, -1 if there is none.
static long LogIndexOf(const std::string& text, size_t from = 0) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    for (size_t i = from; i < g_log.size(); ++i) {
        if (g_log[i].find(text) != std::string::npos) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

static std::vector<TraceLine> OfService(const std::vector<TraceLine>& all, bool srvr) {
    std::vector<TraceLine> out;
    for (const TraceLine& t : all) {
        if (t.srvr == srvr) {
            out.push_back(t);
        }
    }
    return out;
}
static std::vector<TraceLine> OfId(const std::vector<TraceLine>& all, u32 id) {
    std::vector<TraceLine> out;
    for (const TraceLine& t : all) {
        if (t.id == id) {
            out.push_back(t);
        }
    }
    return out;
}
static std::vector<u32> FirstLineOrder(const std::vector<TraceLine>& lines) {
    std::vector<u32> out;
    for (const TraceLine& t : lines) {
        if (std::find(out.begin(), out.end(), t.id) == out.end()) {
            out.push_back(t.id);
        }
    }
    return out;
}
static std::vector<u32> FirstCallOrder(const std::vector<JCall>& journal, bool srvr) {
    std::vector<u32> out;
    for (const JCall& c : journal) {
        if (c.srvr == srvr && std::find(out.begin(), out.end(), c.id) == out.end()) {
            out.push_back(c.id);
        }
    }
    return out;
}
static bool IsOrder(const std::vector<u32>& v, std::initializer_list<u32> expect) {
    return std::vector<u32>(expect) == v;
}

static s32 MsDiff(u32 a, u32 b) {  // a - b, wrap-safe
    return static_cast<s32>(a - b);
}

/// The "dlpc: " lines of ONE service (in the order of the log) against the journal of the game that made the calls (a fresh process: the first
/// call of each command is its first line). Each line is the call its counts point at: the call index of a line is the previous line's plus
/// one plus the "more like it" count. Checked: the words, the result and the duration of every line; then the model of the window, call by
/// call, with the clocks of the journal:
///   * the first call of a command is a line, and begins the window; the window remembers the replies written in it (six at the most);
///   * a call that is not a line has a reply the window holds, and comes less than 2 s after the call that began the window - a reply
///     that is new, or a call after the window, is never left out;
///   * a call that is a line is either a reply the window does not hold (new, or a seventh: not remembered), or comes 2 s after the call
///     that began the window; after the window the line begins the next one, with only its own reply in it;
///   * a clock within a few ms of the 2 s cannot say which (the call's own clock is somewhere between the journal's two stamps): either is
///     accepted there, and the window is then the union of the two possible ones;
///   * the lines follow the order of the calls; no line for an id the services do not have, and a line for every id that was called.
/// The count of a line is what the journal makes of it (the call it points at must carry the line's words); a test that knows the number of
/// calls it made says it itself (TraceCallsAdd).
static constexpr s32 kWindowMs = 2000;
static constexpr s32 kClockSlackMs = 3;  // (the clock is in whole ms, and read once inside a call whose stamps are read outside it)
static constexpr size_t kTraceSigsHeld = 6;  // replies a command remembers per window (written out: the constant of the code is not used to check it)

static void CheckTraceAgainstJournal(const char* what, bool srvr, const std::vector<JCall>& journal, const std::vector<TraceLine>& lines) {
    const int fails0 = g_tfails.load();
    std::map<u32, std::vector<size_t>> calls;  // id -> indices into the journal
    for (size_t i = 0; i < journal.size(); ++i) {
        if (journal[i].srvr == srvr) {
            calls[journal[i].id].push_back(i);
        }
    }
    std::map<u32, std::vector<size_t>> mine;  // id -> indices into `lines`
    for (size_t i = 0; i < lines.size(); ++i) {
        mine[lines[i].id].push_back(i);
    }
    for (const auto& kv : mine) {
        TCHECK(calls.count(kv.first) != 0);  // (a line for a command that nobody made)
    }
    std::vector<size_t> at(lines.size(), static_cast<size_t>(-1));  // line -> index of its call in the journal
    for (const auto& kv : calls) {
        const u32 id = kv.first;
        const std::vector<size_t>& cl = kv.second;
        const auto found = mine.find(id);
        const std::vector<size_t> ll = found == mine.end() ? std::vector<size_t>() : found->second;
        if (id >= 0x12) {
            TCHECK(ll.empty());
            continue;
        }
        TCHECK(!ll.empty());  // (the first call of a command is a line)
        if (ll.empty()) {
            continue;
        }
        // Which calls are lines: the counts say.
        std::vector<bool> written(cl.size(), false);
        size_t pos = 0;
        bool ok = true;
        for (size_t j = 0; j < ll.size() && ok; ++j) {
            const TraceLine& L = lines[ll[j]];
            const u32 more = L.has_more ? L.more : 0;
            const size_t prev = pos;
            pos = j == 0 ? more : prev + 1 + more;
            if (j == 0) {
                TCHECK_EQ(more, 0u);
            }
            if (pos >= cl.size()) {
                std::fprintf(stderr, "  %s: id 0x%02x: line %zu counts %u calls left out, but only %zu calls were made: %s\n", what, id, j, more,
                             cl.size(), L.text.c_str());
                TCHECK(false);
                ok = false;
                break;
            }
            written[pos] = true;
            const JCall& c = journal[cl[pos]];
            at[ll[j]] = cl[pos];
            for (u32 i = 0; i < 6; ++i) {
                TCHECK_EQ(L.req[i], c.req[i]);
            }
            TCHECK_EQ(L.result, c.result);
            TCHECK_EQ(L.rep.size(), c.rep_n);
            for (size_t i = 0; i < L.rep.size() && i < c.rep_n; ++i) {
                TCHECK_EQ(L.rep[i], c.rep[i]);
            }
            TCHECK(L.ms <= (c.t1 - c.t0) + 2);  // (the line's own clock is inside the game's)
        }
        if (!ok) {
            continue;
        }
        // The window, call by call.
        std::vector<size_t> held;  // the calls (indices into cl) whose replies the window holds
        size_t opener = 0;         // the call that began the window
        size_t opener_late = 0;    // when unsure: the call that began it if the last new reply did
        bool unsure = false;       // the window may or may not have been reset at the last line (the union is held)
        for (size_t q = 0; q < cl.size(); ++q) {
            const JCall& c = journal[cl[q]];
            if (q == 0) {
                TCHECK(written[0]);
                held.assign(1, 0);
                opener = 0;
                opener_late = 0;
                continue;
            }
            const JCall& o = journal[cl[opener]];
            const JCall& o_late = journal[cl[unsure ? opener_late : opener]];
            const bool alive = MsDiff(c.t1, o.t0) + kClockSlackMs < kWindowMs;            // certainly inside the window
            const bool expired = MsDiff(c.t0, o_late.t1) - kClockSlackMs >= kWindowMs;    // certainly outside it
            bool in_set = false;
            for (size_t s : held) {
                in_set = in_set || SameReply(journal[cl[s]], c);
            }
            if (written[q]) {
                if (in_set && !unsure) {
                    TCHECK(!alive);  // a reply the window holds is written only once the window is over
                    if (alive) {
                        std::fprintf(stderr, "  %s: id 0x%02x: call %zu repeats a reply of its window and was written %d ms into it\n", what, id, q,
                                     MsDiff(c.t1, o.t0));
                    }
                }
                if (in_set || expired) {
                    held.assign(1, q);  // the next window, with only this reply in it
                    opener = q;
                    opener_late = q;
                    unsure = false;
                } else {
                    // a new reply: remembered while there is room (a seventh is written every time and not held); at the edge of the 2 s it
                    // may have begun a window too, so the old replies stay in the union
                    if (held.size() < kTraceSigsHeld) {
                        held.push_back(q);
                    }
                    if (!alive) {
                        unsure = true;
                        opener_late = q;
                    }
                }
            } else {
                TCHECK(in_set);    // a reply that is new is never left out
                TCHECK(!expired);  // ... nor a call after the window
                if (!in_set || expired) {
                    std::fprintf(stderr, "  %s: id 0x%02x: call %zu was left out (%s, %d ms into the window)\n", what, id, q,
                                 in_set ? "its reply is in the window" : "its reply is NEW", MsDiff(c.t1, o.t0));
                }
            }
        }
    }
    bool any = false;
    size_t before = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (at[i] == static_cast<size_t>(-1)) {
            continue;
        }
        if (any) {
            TCHECK(at[i] > before);  // (the lines follow the calls)
        }
        before = at[i];
        any = true;
    }
    if (g_tfails.load() != fails0) {
        std::fprintf(stderr, "  ^ in: %s (%zu calls, %zu lines)\n", what, journal.size(), lines.size());
    }
}

/// For a part that ends a command's calls with a line: the lines and the calls they left out add up to every call made (the books of the
/// count itself, which the journal cannot give: the calls left out look like the one that was written).
static void TraceCallsAdd(const char* what, bool srvr, u32 id, const std::vector<JCall>& journal, const std::vector<TraceLine>& lines) {
    size_t calls = 0;
    for (const JCall& c : journal) {
        calls += (c.srvr == srvr && c.id == id) ? 1 : 0;
    }
    size_t counted = 0;
    for (const TraceLine& t : lines) {
        if (t.srvr == srvr && t.id == id) {
            counted += 1 + (t.has_more ? t.more : 0);
        }
    }
    if (counted != calls) {
        std::fprintf(stderr, "  %s: id 0x%02x: the lines and their counts add up to %zu calls, the game made %zu\n", what, id, counted, calls);
    }
    TCHECK_EQ(counted, calls);
}

/// The Triple Deluxe sequence between two consoles (the creator's game on A with dlp:SRVR, the searcher's on B with dlp:FKCL), the threads of
/// CreatorTd / SearcherTd (L2), each Game with a journal.
struct TdPair {
    MockRoom room;
    Lc a, b;
    std::vector<JCall> creator, searcher;
    Game ga, gb;
    TdShared s;
    TdPair() : a(room, 1, "Alice", true, true), b(room, 2, "Bob", true, true), ga(a, false, true), gb(b, true, false) {
        ga.journal = &creator;
        gb.journal = &searcher;
    }
    void Run() {
        std::thread tc([&] { CreatorTd(ga, a, s); });
        std::thread ts([&] { SearcherTd(gb, b, a, s); });
        tc.join();
        ts.join();
        TCHECK(s.creator_done.load());
        TCHECK(s.searcher_done.load());
    }
};

// What the sequence calls, in the order of the first call of each command (CreatorTd / SearcherTd above, the ids of SPEC-DLP.md).
#define TD_CREATOR_ORDER 0x01, 0x10, 0x03, 0x05, 0x0B, 0x0D, 0x0C, 0x07, 0x08, 0x02
#define TD_SEARCHER_ORDER 0x11, 0x04, 0x05, 0x09, 0x07, 0x08, 0x06, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x02

/// Set by a part that counts on the calls it makes fitting in the 2 s window of the trace and found that they did not (a machine too busy).
static bool g_l15_slow = false;

/// The end of a child of L15: the checks, and 77 (void, to be made again) when a line was given up for the log's lock or a timed part overran.
static int L15End(const char* part) {
    const u32 drops = LogTakeBusyDrops();
    const int fails = g_failures + g_tfails.load();
    std::printf("  L15 (%s): %d checks, %d failures%s%s\n", part, g_checks + g_tchecks.load(), fails,
                drops ? " (a log line was given up for the lock)" : "", g_l15_slow ? " (a timed part overran its window: the machine is too busy)" : "");
    if (std::getenv("L15_DUMP") != nullptr) {  // (to read the lines: L15_DUMP=1 dlp_lobby_hle_test --l15 td)
        std::lock_guard<std::mutex> l(g_log_mutex);
        for (const std::string& s : g_log) {
            if (s.compare(0, 6, "dlpc: ") == 0) {
                std::printf("%s\n", s.c_str());
            }
        }
    }
    if (fails != 0) {
        return (drops != 0 || g_l15_slow) ? 77 : 1;
    }
    return 0;
}

/// (a) debug level 0: the whole sequence writes not one line, and the trace remembers nothing: the first call at level 1 is a line.
static int L15Off() {
    std::printf("L15 (off): the whole Triple Deluxe sequence at debug level 0: no \"dlpc:\" line, nothing remembered\n");
    g_debug_level = 0;
    g_rules.Reset();
    LogTakeBusyDrops();
    TdPair td;
    td.Run();
    TCHECK(IsOrder(FirstCallOrder(td.creator, true), {TD_CREATOR_ORDER}));  // (it was the real sequence)
    TCHECK(IsOrder(FirstCallOrder(td.searcher, false), {TD_SEARCHER_ORDER}));
    TCHECK(td.creator.size() > 20 && td.searcher.size() > 20);
    TCHECK_EQ(LogCount("dlpc:"), 0);
    TCHECK_EQ(LogSize(), 0u);  // (debug 0 is no log at all)
    // The same reply twice at level 0, then at level 1: the trace did not run, so it is the first of its command and a line
    td.ga.SvState();
    td.ga.SvState();
    td.gb.FkStatus();
    td.gb.FkStatus();
    TCHECK_EQ(LogCount("dlpc:"), 0);
    g_debug_level = 1;
    const size_t mark = LogSize();
    const Rep rs = td.ga.SvState();
    const Rep rf = td.gb.FkStatus();
    const std::vector<TraceLine> first = ReadTrace(mark);
    TCHECK_EQ(first.size(), 2u);
    if (first.size() == 2) {
        TCHECK(first[0].srvr && first[0].id == 0x03 && first[0].result == rs.res() && !first[0].has_more);
        TCHECK(!first[1].srvr && first[1].id == 0x0C && first[1].result == rf.res() && !first[1].has_more);
    }
    const size_t again = LogSize();  // (control: the same replies again, at level 1, within 2 s: left out)
    td.ga.SvState();
    td.gb.FkStatus();
    TCHECK_EQ(LogCount("dlpc:", again), 0);
    td.b.Close(td.gb.fk());
    td.a.Close(td.ga.sv());
    CHECK_RULES();
    return L15End("off");
}

/// (b) debug level 1: the whole sequence between two consoles, line by line against what the games did.
static int L15Td() {
    std::printf("L15 (td): the whole Triple Deluxe sequence at debug level 1: the lines, their order, words, results, durations and counts\n");
    g_rules.Reset();
    LogTakeBusyDrops();
    TdPair td;
    const size_t mark = LogSize();
    td.Run();
    const std::vector<TraceLine> all = ReadTrace(mark);
    const std::vector<TraceLine> sv = OfService(all, true), fk = OfService(all, false);
    TCHECK_EQ(sv.size() + fk.size(), all.size());
    TCHECK(IsOrder(FirstCallOrder(td.creator, true), {TD_CREATOR_ORDER}));
    TCHECK(IsOrder(FirstCallOrder(td.searcher, false), {TD_SEARCHER_ORDER}));
    TCHECK(IsOrder(FirstLineOrder(sv), {TD_CREATOR_ORDER}));  // the commands come in the order of the calls
    TCHECK(IsOrder(FirstLineOrder(fk), {TD_SEARCHER_ORDER}));
    CheckTraceAgainstJournal("creator (dlp:SRVR)", true, td.creator, sv);
    CheckTraceAgainstJournal("searcher (dlp:FKCL)", false, td.searcher, fk);
    // The trace line of a command is written BEFORE the first-call line of its (command, result) pair: NoteCommand must stay the last call of
    // RunDlp (a tail call; written after it, the frame of RunDlp stays under NoteCommand's Log(): 452 bytes at debug 1 - Tests/chain_sim)
    const long t_sv = LogIndexOf("dlpc: dlp:SRVR 0x01 ", mark), p_sv = LogIndexOf("dlp:SRVR: command 0x01 -> 0x00000000 (first call)", mark);
    const long t_fk = LogIndexOf("dlpc: dlp:FKCL 0x11 ", mark), p_fk = LogIndexOf("dlp:FKCL: command 0x11 -> 0x00000000 (first call)", mark);
    TCHECK(t_sv >= 0 && p_sv >= 0 && t_sv < p_sv);
    TCHECK(t_fk >= 0 && p_fk >= 0 && t_fk < p_fk);
    // The first command of each service: ONE line each in the life of the process (this one: the first commands are SRVR Initialize and FKCL
    // InitializeWithName), before the trace line of that command, with the thread that made the call (4660 = 0x1234 of the fake kernel, in
    // decimal) and its priority (0x30, in hex) - and not a second line when the service is called again, on the same console or on the other
    const long f_sv = LogIndexOf("dlp: the first command of dlp:SRVR is 0x01: the Download Play lobby is carried by the room (thread 4660, priority 0x30)", mark);
    const long f_fk = LogIndexOf("dlp: the first command of dlp:FKCL is 0x11: the Download Play lobby is carried by the room (thread 4660, priority 0x30)", mark);
    TCHECK(f_sv >= 0 && f_sv < t_sv);
    TCHECK(f_fk >= 0 && f_fk < t_fk);
    TCHECK_EQ(LogCount("dlp: the first command of dlp:SRVR is ", mark), 1);
    TCHECK_EQ(LogCount("dlp: the first command of dlp:FKCL is ", mark), 1);
    TCHECK_EQ(LogCount("dlp: the first command of ", mark), 2);
    std::printf("  L15 (td): %zu + %zu calls, %zu + %zu lines, join %u ms\n", td.creator.size(), td.searcher.size(), sv.size(), fk.size(),
                td.s.join_ms.load());

    // The creator
    const std::vector<TraceLine> init = OfId(sv, 0x01);
    TCHECK_EQ(init.size(), 1u);
    if (!init.empty()) {  // Initialize: the first six of its nine words
        const u32 want[6] = {0x85000, 3, 0xE, 0, 0x40000, 2};
        for (int i = 0; i < 6; ++i) {
            TCHECK_EQ(init[0].req[i], want[i]);
        }
        TCHECK_EQ(init[0].result, 0u);
        TCHECK(init[0].rep.empty());
    }
    const std::vector<TraceLine> acc = OfId(sv, 0x05);
    TCHECK_EQ(acc.size(), 1u);
    if (!acc.empty()) {  // StartAccepting(manual 0, channel 6): the two words, then zeros (not the 0xA5A5A5A5 behind them), no reply words
        const u32 want[6] = {0, 6, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            TCHECK_EQ(acc[0].req[i], want[i]);
        }
        TCHECK_EQ(acc[0].result, 0u);
        TCHECK(acc[0].rep.empty());
    }
    std::vector<u32> states;  // GetServerState: the reply word, one per change
    for (const TraceLine& t : OfId(sv, 0x03)) {
        TCHECK_EQ(t.result, 0u);
        TCHECK_EQ(t.rep.size(), 1u);
        if (t.rep.size() == 1 && (states.empty() || states.back() != t.rep[0])) {
            states.push_back(t.rep[0]);
        }
    }
    // 1 (initialised), 2 (accepting), [6 (distributing: gone in one poll of the network thread)], 8 (the passphrase), 9, and 0 after Finalize
    TCHECK(states == std::vector<u32>({1, 2, 8, 9, 0}) || states == std::vector<u32>({1, 2, 6, 8, 9, 0}));
    const std::vector<TraceLine> pass = OfId(sv, 0x08);  // SendPassphrase: the first call wins, the second is refused: two lines, two replies
    TCHECK_EQ(pass.size(), 2u);
    if (pass.size() == 2) {
        u32 w[3];
        PassWords(kNine, w);
        TCHECK_EQ(pass[0].req[0], w[0]);
        TCHECK_EQ(pass[0].req[1], w[1]);
        TCHECK_EQ(pass[0].result, 0u);
        TCHECK_EQ(pass[1].req[0], static_cast<u32>('f') | (static_cast<u32>('f') << 8) | (static_cast<u32>('e') << 16) | (static_cast<u32>('e') << 24));
        TCHECK_EQ(pass[1].result, R_INVALID_STATE);
    }
    const std::vector<TraceLine> fin = OfId(sv, 0x02);
    TCHECK_EQ(fin.size(), 1u);
    if (!fin.empty()) {
        TCHECK_EQ(fin[0].result, 0u);
        TCHECK(fin[0].rep.empty());
    }

    // The searcher
    const std::vector<TraceLine> iname = OfId(fk, 0x11);
    TCHECK_EQ(iname.size(), 1u);
    if (!iname.empty()) {  // InitializeWithName("Bob", 4 beacons): 0x30000, 4, then the first four words of the name
        u32 n[5];
        NameWords("Bob", n);
        const u32 want[6] = {0x30000, 4, n[0], n[1], n[2], n[3]};
        for (int i = 0; i < 6; ++i) {
            TCHECK_EQ(iname[0].req[i], want[i]);
        }
    }
    const std::vector<TraceLine> scan = OfId(fk, 0x05);
    TCHECK_EQ(scan.size(), 1u);
    if (!scan.empty()) {
        const u32 want[6] = {0x0421, 0x0010C000, 0x00040001, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            TCHECK_EQ(scan[0].req[i], want[i]);
        }
    }
    const std::vector<TraceLine> info = OfId(fk, 0x07);  // GetServerInfo(host MAC): the two MAC words; the reply: the buffer descriptor and its address
    TCHECK_EQ(info.size(), 1u);
    if (!info.empty()) {
        TCHECK_EQ(info[0].req[0], MacW1(td.a.mac()));
        TCHECK_EQ(info[0].req[1], MacW2(td.a.mac()));
        TCHECK_EQ(info[0].req[2], 0u);
        TCHECK_EQ(info[0].rep.size(), 2u);
        if (info[0].rep.size() == 2) {
            TCHECK_EQ(info[0].rep[0], (0x298u << 14) | 2);
        }
    }
    const std::vector<TraceLine> join = OfId(fk, 0x0B);  // StartFakeSession(host MAC, title): the words BEFORE the reply replaced them
    TCHECK_EQ(join.size(), 1u);
    if (!join.empty()) {
        TCHECK_EQ(join[0].req[0], MacW1(td.a.mac()));
        TCHECK_EQ(join[0].req[1], MacW2(td.a.mac()));
        TCHECK_EQ(join[0].req[2], 0x0010C000u);
        TCHECK_EQ(join[0].req[3], 0x00040001u);
        TCHECK_EQ(join[0].result, 0u);
        TCHECK(join[0].rep.empty());
        // ... and the duration of the join: the front-end held the call for it, in 10 ms slices
        TCHECK(join[0].ms >= 10);
        TCHECK(join[0].ms <= td.s.join_ms.load());
        TCHECK(join[0].ms + 25 >= td.s.join_ms.load());
    }
    std::vector<u32> status;  // GetMyStatus: the state (top byte of the first reply word), one per change
    for (const TraceLine& t : OfId(fk, 0x0C)) {
        TCHECK_EQ(t.result, 0u);
        TCHECK_EQ(t.rep.size(), 5u);
        const u32 state = t.rep.empty() ? 0xFF : t.rep[0] >> 24;
        if (status.empty() || status.back() != state) {
            status.push_back(state);
        }
    }
    // 5 (joined) at the first look, 9 (complete) at the end, 6 in between when a poll saw it; never backwards
    TCHECK(status.size() >= 2 && status.size() <= 3);
    if (status.size() >= 2) {
        TCHECK_EQ(status.front(), 5u);
        TCHECK_EQ(status.back(), 9u);
        TCHECK(std::is_sorted(status.begin(), status.end()));
        if (status.size() == 3) {
            TCHECK_EQ(status[1], 6u);
        }
    }
    const std::vector<TraceLine> gpass = OfId(fk, 0x0F);  // GetPassphrase: the nine characters in the reply words
    TCHECK(!gpass.empty());
    if (!gpass.empty()) {
        u32 w[3];
        PassWords(kNine, w);
        TCHECK_EQ(gpass[0].rep.size(), 3u);
        if (gpass[0].rep.size() == 3) {
            TCHECK_EQ(gpass[0].rep[0], w[0]);
            TCHECK_EQ(gpass[0].rep[1], w[1]);
        }
    }

    // Ids the services do not have: below 0x12 a line (the unknown ones too), from 0x12 on none. Their replies are the dispatcher's own.
    const size_t m2 = LogSize();
    const size_t j2s = td.searcher.size(), j2c = td.creator.size();
    TCHECK_EQ(td.gb.Fk(0x00000000, {}).res(), R_BAD_HEADER);   // FKCL 0x00
    TCHECK_EQ(td.gb.Fk(0x00120000, {}).res(), R_BAD_HEADER);   // FKCL 0x12
    TCHECK_EQ(td.gb.Fk(0x7F000000, {}).res(), R_BAD_HEADER);   // FKCL 0x7F00
    TCHECK_EQ(td.ga.Sv(0x00110000, {}).res(), R_BAD_HEADER);   // SRVR 0x11 (the service has up to 0x10)
    TCHECK_EQ(td.ga.Sv(0x00120000, {}).res(), R_BAD_HEADER);   // SRVR 0x12
    TCHECK_EQ(td.ga.Sv(0xFFFF0000, {}).res(), R_BAD_HEADER);   // SRVR 0xFFFF
    const std::vector<TraceLine> extra = ReadTrace(m2);
    TCHECK_EQ(extra.size(), 2u);  // FKCL 0x00 and SRVR 0x11
    if (extra.size() == 2) {
        TCHECK(!extra[0].srvr && extra[0].id == 0x00 && extra[0].result == R_BAD_HEADER && extra[0].rep.empty());
        TCHECK(extra[1].srvr && extra[1].id == 0x11 && extra[1].result == R_BAD_HEADER && extra[1].rep.empty());
    }
    TCHECK_EQ(td.searcher.size(), j2s + 3);
    TCHECK_EQ(td.creator.size(), j2c + 3);
    TCHECK_EQ(LogCount("dlp: the first command of ", mark), 2);  // (still the two: the unknown ids of both services made no first-command line)
    // ... and the books still add up with them (the unknown ones are in the journal too)
    if (extra.size() == 2) {
        std::vector<TraceLine> fk2 = fk, sv2 = sv;
        fk2.push_back(extra[0]);
        sv2.push_back(extra[1]);
        CheckTraceAgainstJournal("creator, with the ids the service does not have", true, td.creator, sv2);
        CheckTraceAgainstJournal("searcher, with the ids the service does not have", false, td.searcher, fk2);
    }
    td.b.Close(td.gb.fk());
    td.a.Close(td.ga.sv());
    CHECK_RULES();
    return L15End("td");
}

static u32 g_slowest_burst = 0;  // (the slowest Burst of this process, in ms: printed by L15 (flood))

/// n calls back to back on this thread; true if every reply was the first reply (the header, the result and the words after it).
static bool Burst(u32 n, const std::function<Rep()>& call, u32* took_ms) {
    const u64 t0 = platform::NowMs();
    const Rep first = call();
    bool same = true;
    for (u32 i = 1; i < n; ++i) {
        const Rep r = call();
        for (u32 k = 0; k < 8; ++k) {
            same = same && r.w[k] == first.w[k];
        }
    }
    *took_ms = Ms(t0);
    g_slowest_burst = std::max(g_slowest_burst, *took_ms);
    return same;
}

/// A part that counts on its calls fitting in the 2 s window of the trace: true if `since` is further back than `limit_ms` (the part is then
/// void on a failure, see L15End).
static bool Overran(u64 since, u32 limit_ms) {
    if (Ms(since) > limit_ms) {
        g_l15_slow = true;
        return true;
    }
    return false;
}

/// (c) the polls: 5000 calls with an unchanged reply are one line, a change is a line at once with the right count, a return to a reply the
/// window already holds is counted and not written (the intended limit of the set), and the same reply is written again 2 s after the window
/// began - not before.
static int L15Flood() {
    std::printf("L15 (flood): 5000 polls of GetMyStatus / GetServerState / GetTitleInfoInOrder with an unchanged reply, then changes, then a 2.6 s pace\n");
    g_rules.Reset();
    LogTakeBusyDrops();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    std::vector<JCall> ja, jb;
    Game ga(a, false, true), gb(b, true, false);
    ga.journal = &ja;
    gb.journal = &jb;
    u32 took = 0;
    size_t m = 0;
    std::vector<TraceLine> w;

    // FKCL GetMyStatus (0x0C), the searcher: 1 (initialised) x 5000, 2 (scanning) x 1000, 1 again (3 calls)
    TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);
    m = LogSize();
    const u64 win_status = platform::NowMs();  // (the window of the command begins at its first line, which is its first call)
    TCHECK(Burst(5000, [&] { return gb.FkStatus(); }, &took));
    w = ReadTrace(m);
    TCHECK(took < 1000);  // (under a second)
    TCHECK_EQ(w.size(), 1u);  // the first call, and none of the other 4999
    if (w.size() == 1) {
        TCHECK(!w[0].srvr && w[0].id == 0x0C && w[0].result == 0 && w[0].rep.size() == 5 && (w[0].rep[0] >> 24) == 1);
        TCHECK(!w[0].has_more);
    }
    TCHECK_EQ(gb.FkScan().res(), 0u);
    m = LogSize();
    TCHECK(Burst(1000, [&] { return gb.FkStatus(); }, &took));
    w = ReadTrace(m);
    TCHECK_EQ(w.size(), 1u);  // the change 1 -> 2 is a line at its call, and says how many calls of state 1 it left out: 4999
    if (w.size() == 1) {
        TCHECK(w[0].id == 0x0C && w[0].rep.size() == 5 && (w[0].rep[0] >> 24) == 2);
        TCHECK(w[0].has_more);
        TCHECK_EQ(w[0].more, 4999u);
    }
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    m = LogSize();
    TCHECK(Burst(3, [&] { return gb.FkStatus(); }, &took));
    w = ReadTrace(m);
    // Back to state 1: the window holds that reply (its line is less than 2 s old), so the three calls are COUNTED, not written. This is the
    // intended limit of the set: a status that goes 1 -> 2 -> 1 inside the window makes two lines, and the return is in the count (999 + 3)
    // of the next line. After the window the same call is a line again (the books below take either, by the clock).
    if (!Overran(win_status, 1700)) {
        TCHECK_EQ(w.size(), 0u);
    }

    // dlp:SRVR GetServerState (0x03), the creator: 1 x 5000, 2 x 1000 (StartAccepting), then paced
    TCHECK_EQ(ga.SvInit(3).res(), 0u);
    m = LogSize();
    const u64 win_state = platform::NowMs();
    TCHECK(Burst(5000, [&] { return ga.SvState(); }, &took));
    w = ReadTrace(m);
    TCHECK(took < 1000);
    TCHECK_EQ(w.size(), 1u);
    if (w.size() == 1) {
        TCHECK(w[0].srvr && w[0].id == 0x03 && w[0].rep.size() == 1 && w[0].rep[0] == 1 && !w[0].has_more);
    }
    TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
    m = LogSize();
    TCHECK(Burst(1000, [&] { return ga.SvState(); }, &took));
    w = ReadTrace(m);
    TCHECK_EQ(w.size(), 1u);
    if (w.size() == 1) {
        TCHECK(w[0].id == 0x03 && w[0].rep.size() == 1 && w[0].rep[0] == 2);
        TCHECK(w[0].has_more);
        TCHECK_EQ(w[0].more, 4999u);
    }
    // The same reply 2 s after the window began is written again, with the calls left out in between; and not before. The window began at
    // the first call above (state 1; the line of state 2 is in the same window), so the pace runs until 2.6 s after it: ONE line, 2 s in.
    Overran(win_state, 1700);
    m = LogSize();
    u32 paced = 0;
    while (Ms(win_state) < 2600) {
        ga.SvState();
        ++paced;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    w = ReadTrace(m);
    TCHECK(paced > 300);
    TCHECK_EQ(w.size(), 1u);
    if (w.size() == 1) {
        TCHECK(w[0].id == 0x03 && w[0].rep.size() == 1 && w[0].rep[0] == 2);
        TCHECK(w[0].has_more);
        TCHECK(w[0].more >= 999 && w[0].more <= 998 + paced);  // (the 999 left out of the burst, and the paced calls before the 2 s; the books say which)
    }

    // FKCL GetTitleInfoInOrder (0x09), the searcher scanning the creator's group: the group x 5000, then kNoData x 5000. The polls that wait for
    // the group are in the command's window, and a reply it holds would be counted: the window is let run out first (2.1 s of silence), so that
    // the group is the first call of a new one - a line - and "no data", which that window does not hold, a line at its first call.
    TCHECK_EQ(gb.FkScan().res(), 0u);
    TCHECK(WaitFor(8000, [&] {
        const Rep r = gb.FkInOrder(1);
        return r.res() == 0 && std::memcmp(gb.TitleRec() + kTitleInfoMac, a.mac().data(), 6) == 0;
    }));
    TCHECK_EQ(gb.FkInOrder(0).res(), R_NO_DATA);  // (the end of the list)
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    m = LogSize();
    TCHECK(Burst(5000, [&] { return gb.FkInOrder(1); }, &took));
    w = ReadTrace(m);
    TCHECK(took < 1000);
    TCHECK_EQ(w.size(), 1u);  // (the polls before the silence left some out: the line may say so)
    if (w.size() == 1) {
        TCHECK(w[0].id == 0x09 && w[0].result == 0);
    }
    m = LogSize();
    bool ended = true;
    {
        const Rep one = gb.FkInOrder(0);
        ended = one.res() == R_NO_DATA;
    }
    TCHECK(ended);
    TCHECK(Burst(4999, [&] { return gb.FkInOrder(0); }, &took));
    w = ReadTrace(m);
    TCHECK(took < 1000);
    TCHECK_EQ(w.size(), 1u);  // the change group -> kNoData at its first call: 4999 calls of the group left out
    if (w.size() == 1) {
        TCHECK(w[0].id == 0x09 && w[0].result == R_NO_DATA && w[0].has_more);
        TCHECK_EQ(w[0].more, 4999u);
    }

    // The books of both services, call by call
    const std::vector<TraceLine> all = ReadTrace(0);
    CheckTraceAgainstJournal("flood: creator", true, ja, OfService(all, true));
    CheckTraceAgainstJournal("flood: searcher", false, jb, OfService(all, false));
    std::printf("  L15 (flood): the slowest burst of calls (up to 5000, back to back) took %u ms\n", g_slowest_burst);
    b.Close(gb.fk());
    a.Close(ga.sv());
    CHECK_RULES();
    return L15End("flood");
}

/// The "dlpc: " lines of one command of one service written since log index `mark`, parsed.
static std::vector<TraceLine> TraceOf(size_t mark, bool srvr, u32 id) {
    return OfId(OfService(ReadTrace(mark), srvr), id);
}

/// (f) replies that ALTERNATE, which is how the games poll (Triple Deluxe asks GetTitleInfoInOrder(1) then (0) every frame until "no data", so
/// one command alternates between a group and "no data", or between several groups and "no data"; the first version of the trace remembered
/// ONE reply per command and wrote a line at every call - about 120 a second on the searcher, in a log whose queue holds 64 KB). The
/// replies are real: the searcher on console B reads the creator's group (A) into buffers of its own - the reply of GetTitleInfoInOrder echoes
/// the address of the game's buffer, so a call into another buffer is another reply, and the buffers are 64 KB apart: whatever the 16-bit fold
/// of the plugin is made of, two of them cannot collide. Everything is on the one test thread, and every call is in one journal.
///   (a) 4000 calls alternating "group" / "no data": two lines; then 200 rounds of four groups (four buffers) and "no data" in turn: three
///       more lines, the counts of the calls left out are exact (3999 before the first of them, 0 after)
///   (b) GetMyStatus 1 -> 2 -> 1 -> 2 (the lobby cannot go 5 -> 6 -> 9 -> 5 in one window; that sequence, forward, is L15 (td)): the new
///       state is a line at its call, the return to a state the window holds is COUNTED - the intended limit of the set
///   (c) after the 2 s of the window the next call is written again, with the number of calls it left out
///   (d) six replies are remembered per window; a seventh is written at every call and is never silent; the six stay silent
///   (e) the first command of each service: one line each, with the thread and its priority
///   (g) replies that differ in the RESULT alone (the same header, no reply word: a posted command that nobody served, then served) are lines
/// The windows: the command's window begins at its first line. The part waits 2.1 s after the searcher has found the group so that the
/// polls that waited for it are out of the window of GetTitleInfoInOrder, and it keeps (a) and (d) inside 1.7 s of their first call.
static int L15Alt() {
    std::printf("L15 (alt): replies that alternate: a group and \"no data\", four groups and \"no data\", a return to a state, a seventh reply\n");
    g_rules.Reset();
    LogTakeBusyDrops();
    MockRoom room;
    Lc a(room, 1, "Alice", true, true);
    Lc b(room, 2, "Bob", true, true);
    std::vector<JCall> j;
    Game ga(a, false, true), gb(b, true, false);
    ga.journal = &j;
    gb.journal = &j;
    OpenCreator(ga);  // dlp:SRVR Initialize, GetDupNoticeNeed, StartAccepting: the lobby beacons

    // (b) the searcher's status: 1 (initialised) x3, 2 (scanning) x3, 1 again x3, 2 again x3
    TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);
    const size_t m_status = LogSize();
    const u64 t_status = platform::NowMs();
    u32 wrong = 0;
    for (int k = 0; k < 3; ++k) {
        wrong += gb.StatusState() != 1 ? 1 : 0;
    }
    TCHECK_EQ(gb.FkChannels().res(), 0u);
    TCHECK_EQ(gb.FkScan().res(), 0u);
    for (int k = 0; k < 3; ++k) {
        wrong += gb.StatusState() != 2 ? 1 : 0;
    }
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    for (int k = 0; k < 3; ++k) {
        wrong += gb.StatusState() != 1 ? 1 : 0;
    }
    TCHECK_EQ(gb.FkScan().res(), 0u);
    for (int k = 0; k < 3; ++k) {
        wrong += gb.StatusState() != 2 ? 1 : 0;
    }
    TCHECK_EQ(wrong, 0u);
    const bool status_in_window = !Overran(t_status, 1700);
    const std::vector<TraceLine> st = TraceOf(m_status, false, 0x0C);
    if (status_in_window) {
        TCHECK_EQ(st.size(), 2u);  // state 1 at the first call, state 2 at its first call; the returns are in the count
        if (st.size() == 2) {
            TCHECK(st[0].rep.size() == 5 && (st[0].rep[0] >> 24) == 1 && !st[0].has_more);
            TCHECK(st[1].rep.size() == 5 && (st[1].rep[0] >> 24) == 2 && st[1].has_more);
            TCHECK_EQ(st[1].more, 2u);  // the two calls of state 1 that the new state left out
        }
    }

    // (g) a posted command (SRVR DistributeTitle: the Start button) on a console whose network thread is not running: refused at once
    // ("wireless off"); the network thread then starts, and the same command, the same request, no reply word, is served - and the answer is
    // another result (the console never opened a lobby: "invalid state"). Console C, a third player.
    {
        Lc c(room, 3, "Carol", true, false);
        Game gc(c, false, true);
        gc.journal = &j;
        const size_t m3 = LogSize();
        TCHECK_EQ(gc.SvDistribute().res(), R_WIRELESS_OFF);
        TCHECK_EQ(gc.SvDistribute().res(), R_WIRELESS_OFF);  // (the same reply: counted)
        c.StartNet();
        const Rep served = gc.SvDistribute();
        TCHECK(served.res() != R_WIRELESS_OFF);
        const std::vector<TraceLine> acc = TraceOf(m3, true, 0x07);
        TCHECK_EQ(acc.size(), 2u);  // a result that differs is a new reply, whatever the words after it
        if (acc.size() == 2) {
            TCHECK(acc[0].result == R_WIRELESS_OFF && !acc[0].has_more);
            TCHECK(acc[1].result == served.res() && acc[1].has_more);
            TCHECK_EQ(acc[1].more, 1u);
        }
        TraceCallsAdd("alt: SRVR DistributeTitle", true, 0x07, j, ReadTrace(0));
        c.Close(gc.sv());
    }

    // the searcher finds the group, then 2.1 s of silence: the polls (and the status block above) are out of every window
    TCHECK(WaitFor(8000, [&] {
        const Rep r = gb.FkInOrder(1);
        return r.res() == 0 && std::memcmp(gb.TitleRec() + kTitleInfoMac, a.mac().data(), 6) == 0;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    // the buffers of the game: nine of 64 KB, from an address that is a multiple of 64 KB
    std::vector<u8> arena(10 * 0x10000);
    u8* const base = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(arena.data()) + 0xFFFF) & ~static_cast<uintptr_t>(0xFFFF));
    auto slot = [&](u32 k) { return base + k * 0x10000; };
    u32 bad = 0;
    auto group = [&](u32 k) { bad += gb.FkInOrderInto(1, slot(k)).res() != 0 ? 1 : 0; };
    auto nodata = [&] { bad += gb.FkInOrderInto(0, slot(0)).res() != R_NO_DATA ? 1 : 0; };

    // (a) the window of GetTitleInfoInOrder begins at the next call
    const size_t m1 = LogSize();
    const u64 t_w1 = platform::NowMs();
    for (u32 i = 0; i < 2000; ++i) {  // 4000 calls: a group, "no data", a group, "no data" ...
        group(0);
        nodata();
    }
    const bool a_in_window = !Overran(t_w1, 1700);
    const std::vector<TraceLine> alt2 = TraceOf(m1, false, 0x09);
    if (a_in_window) {
        TCHECK_EQ(alt2.size(), 2u);  // at most (distinct replies + 1) lines: here exactly the distinct replies
        TCHECK(alt2.size() <= 2 + 1);
        if (alt2.size() == 2) {
            TCHECK(alt2[0].result == 0 && alt2[0].rep.size() == 2);
            TCHECK(alt2[1].result == R_NO_DATA && alt2[1].rep.empty() && !alt2[1].has_more);
        }
    }
    const size_t m2 = LogSize();
    for (u32 round = 0; round < 200; ++round) {  // 1000 calls: four groups (four buffers) and "no data", in turn
        for (u32 k = 0; k < 4; ++k) {
            group(k);
        }
        nodata();
    }
    const std::vector<TraceLine> four = TraceOf(m2, false, 0x09);
    if (a_in_window && !Overran(t_w1, 1700)) {
        TCHECK_EQ(four.size(), 3u);  // groups 1, 2, 3 are new; group 0 and "no data" are in the window
        if (four.size() == 3) {
            TCHECK(four[0].has_more);
            TCHECK_EQ(four[0].more, 3999u);  // 3998 of the 4000 above (all but the two lines) and the first group 0 of the first round
            TCHECK(!four[1].has_more && !four[2].has_more);
        }
        TCHECK(alt2.size() + four.size() <= 5 + 1);  // five distinct replies in the window, one line each (+ 1 at the most)
    }

    // (d) the sixth reply of the window is the last one the command remembers; the seventh is written at every call
    const size_t m4 = LogSize();
    group(4);                  // the sixth distinct reply: a line, and remembered
    group(5);                  // the seventh: a line
    group(5);                  // ... and again, and again: it is not remembered (a log must never lose a reply it has no room for)
    group(5);
    group(0);                  // the six that are remembered are counted
    group(4);
    group(1);
    group(6);                  // an eighth: a line, with the three the remembered ones left out
    const std::vector<TraceLine> seven = TraceOf(m4, false, 0x09);
    const bool d_in_window = !Overran(t_w1, 1700);
    if (d_in_window && a_in_window) {
        TCHECK_EQ(seven.size(), 5u);  // group 4, group 5 three times, group 6
        if (seven.size() == 5) {
            TCHECK(seven[0].has_more);
            TCHECK_EQ(seven[0].more, 996u);  // the calls of the rounds above after the line of the third group (1000 - 4)
            for (int k = 1; k < 4; ++k) {
                TCHECK(!seven[k].has_more);
            }
            TCHECK(seven[4].has_more);
            TCHECK_EQ(seven[4].more, 3u);
            TCHECK(seven[1].rep == seven[2].rep && seven[2].rep == seven[3].rep && seven[1].rep != seven[0].rep && seven[1].rep != seven[4].rep);
        }
    }
    group(0);                  // seven calls with replies the window holds: counted
    group(1);
    group(2);
    group(3);
    group(4);
    nodata();
    group(0);

    // (c) 2.1 s after the first call of the window, the next call is written again - whatever its reply - and says how many it left out
    while (Ms(t_w1) < 2100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const size_t m5 = LogSize();
    group(0);                  // in the window's set, but the window is over: a line, with the seven calls
    const std::vector<TraceLine> again = TraceOf(m5, false, 0x09);
    TCHECK_EQ(again.size(), 1u);
    if (again.size() == 1 && d_in_window && a_in_window) {
        TCHECK(again[0].has_more);
        TCHECK_EQ(again[0].more, 7u);
    }
    nodata();                  // the new window holds only the group: "no data" is news again - a line, with nothing left out since the last
    const std::vector<TraceLine> news = TraceOf(m5, false, 0x09);
    TCHECK_EQ(news.size(), 2u);
    if (news.size() == 2) {
        TCHECK(news[1].result == R_NO_DATA && !news[1].has_more);
    }
    // ... and the status: the window of GetMyStatus is over long ago too; state 2 is written again with the 8 calls left out since its line
    const size_t m6 = LogSize();
    TCHECK_EQ(gb.StatusState(), 2u);
    const std::vector<TraceLine> status_again = TraceOf(m6, false, 0x0C);
    TCHECK_EQ(status_again.size(), 1u);
    if (status_again.size() == 1 && status_in_window) {
        TCHECK(status_again[0].has_more);
        TCHECK_EQ(status_again[0].more, 8u);
    }
    TCHECK_EQ(bad, 0u);        // (every group was a group and every end of the list the end of the list)

    // (e) the first command of each service: one line each in the life of the process, whoever calls it again
    TCHECK_EQ(LogCount("dlp: the first command of dlp:SRVR is 0x01: the Download Play lobby is carried by the room (thread 4660, priority 0x30)"), 1);
    TCHECK_EQ(LogCount("dlp: the first command of dlp:FKCL is 0x11: the Download Play lobby is carried by the room (thread 4660, priority 0x30)"), 1);
    TCHECK_EQ(LogCount("dlp: the first command of "), 2);

    // The books: every call is a line or a call a window holds, call by call, and the lines and their counts add up to the calls made (the
    // last call of both commands is a line)
    const std::vector<TraceLine> all = ReadTrace(0);
    CheckTraceAgainstJournal("alt: dlp:SRVR", true, j, OfService(all, true));
    CheckTraceAgainstJournal("alt: dlp:FKCL", false, j, OfService(all, false));
    TraceCallsAdd("alt: GetTitleInfoInOrder", false, 0x09, j, all);
    TraceCallsAdd("alt: GetMyStatus", false, 0x0C, j, all);
    std::printf("  L15 (alt): %zu calls, %zu lines\n", j.size(), all.size());
    b.Close(gb.fk());
    a.Close(ga.sv());
    CHECK_RULES();
    return L15End("alt");
}

/// (d) a command nobody served: the refusal and the unserved Finalize are lines with their results.
static int L15Refused() {
    std::printf("L15 (refused): commands nobody served (no network thread, or one that does not take them): the results are in the lines\n");
    g_rules.Reset();
    LogTakeBusyDrops();
    {
        MockRoom room;
        Lc a(room, 1, "Alice", true, false);  // nobody ticks the hub: a posted command is refused at once
        std::vector<JCall> j;
        Game g(a, true, true);
        g.journal = &j;
        const size_t m = LogSize();
        TCHECK_EQ(g.SvAccepting(0, 6).res(), R_WIRELESS_OFF);
        TCHECK_EQ(g.FkScan().res(), R_WIRELESS_OFF);
        TCHECK_EQ(g.FkFinalize().res(), 0u);  // (the commands that succeed in every state: L13)
        TCHECK_EQ(g.SvFinalize().res(), 0u);
        TCHECK_EQ(g.SvEnd().res(), 0u);
        TCHECK_EQ(g.FkStopScan().res(), 0u);
        TCHECK_EQ(g.FkStopSession().res(), 0u);
        const std::vector<TraceLine> all = ReadTrace(m);
        TCHECK_EQ(all.size(), 7u);
        CheckTraceAgainstJournal("refused: dlp:SRVR", true, j, OfService(all, true));
        CheckTraceAgainstJournal("refused: dlp:FKCL", false, j, OfService(all, false));
        if (all.size() == 7) {
            // in the order of the calls
            const struct {
                bool srvr;
                u32 id, result;
            } want[7] = {{true, 0x05, R_WIRELESS_OFF}, {false, 0x05, R_WIRELESS_OFF}, {false, 0x02, 0}, {true, 0x02, 0},
                         {true, 0x06, 0},             {false, 0x06, 0},             {false, 0x10, 0}};
            for (int i = 0; i < 7; ++i) {
                TCHECK(all[i].srvr == want[i].srvr && all[i].id == want[i].id && all[i].result == want[i].result);
                TCHECK(all[i].rep.empty());
                TCHECK(all[i].ms < 100);
            }
            const u32 acc[6] = {0, 6, 0, 0, 0, 0};  // StartAccepting(0, 6) and StartScan(0x0421, the filter) as the game sent them
            const u32 scan[6] = {0x0421, 0x0010C000, 0x00040001, 0, 0, 0};
            for (int i = 0; i < 6; ++i) {
                TCHECK_EQ(all[0].req[i], acc[i]);
                TCHECK_EQ(all[1].req[i], scan[i]);
            }
        }
        a.Close(g.fk());
        a.Close(g.sv());
    }
    {
        // A network thread that is alive but does not take the request in 2 s: the Finalize that was not served says success, after the wait
        MockRoom room;
        Lc a(room, 1, "Alice", true, false);
        std::vector<JCall> j;
        Game g(a, false, true);
        g.journal = &j;
        a.StartNet();
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        const size_t m = LogSize();
        a.net->Pause(true);
        // (dlp:SRVR Finalize had its line in the first part, which this process wrote less than a second ago: the 2 s window of that command
        //  must be over by the time this one answers, and it is - the call itself waits 2 s for the network thread; the pause below is the
        //  margin, so that the line is the window's end and not a race with it)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        TCHECK_EQ(g.SvFinalize().res(), 0u);
        const std::vector<TraceLine> all = ReadTrace(m);
        TCHECK_EQ(all.size(), 1u);
        if (all.size() == 1) {
            TCHECK(all[0].srvr && all[0].id == 0x02 && all[0].result == 0 && all[0].rep.empty());
            TCHECK(all[0].ms >= 1900 && all[0].ms < 3500);  // (the 2 s it was waited for)
        }
        a.net->Pause(false);
        a.Close(g.sv());
    }
    return L15End("refused");
}

/// (e) not for the probe of Battle Royale, not for a game without a hub, not for nwm::UDS / ndm:u of a game with one.
static int L15Probe() {
    std::printf("L15 (probe): no \"dlpc:\" line for the probe, for a game without a hub, for nwm::UDS and ndm:u\n");
    g_rules.Reset();
    LogTakeBusyDrops();
    const size_t m0 = LogSize();
    {  // Battle Royale: the probe only
        MockRoom room;
        Lc c(room, 1, "Alice", false, false, true);
        const Handle uds = c.Open("nwm::UDS"), ndm = c.Open("ndm:u"), fk = c.Open("dlp:FKCL");
        Result rc = 0;
        TCHECK(uds != 0 && ndm != 0 && fk != 0);
        TCHECK_EQ(c.Open("dlp:SRVR", &rc), 0u);
        TCHECK_EQ(Do(c, fk, InitNameRequest()).res(), 0u);
        TCHECK_EQ(Do(c, fk, req::Make(0x02, 0, 0)).res(), 0u);
        TCHECK_EQ(Do(c, fk, InitRequest()).res(), 0u);
        u32 refused = 0;
        for (u32 id = 0; id < 0x20; ++id) {  // every id of the service and some beyond: the probe answers them all, and traces none
            const Rep r = Do(c, fk, req::Make(id, 6, 0));
            refused += r.res() != 0 ? 1 : 0;
        }
        TCHECK(refused > 10);  // (the probe really answered: "invalid state" to most of them)
        TCHECK_EQ(Do(c, uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)).res(), 0u);
        TCHECK_EQ(UdsStatus(c, uds).res(), 0u);
        TCHECK_EQ(Do(c, ndm, std::vector<u32>{0x30u << 16}).res(), 0u);
        c.Close(fk);
        TCHECK_EQ(Do(c, uds, req::Shutdown()).res(), 0u);
        c.Close(uds);
        c.Close(ndm);
    }
    TCHECK_EQ(LogCount("dlpc:", m0), 0);
    {  // a game that is not in the table: neither the probe nor a hub
        MockRoom room;
        Lc c(room, 1, "Alice", false, false);
        Result rc = 0;
        const Handle uds = c.Open("nwm::UDS"), ndm = c.Open("ndm:u");
        TCHECK_EQ(c.Open("dlp:FKCL", &rc), 0u);
        TCHECK_EQ(c.Open("dlp:SRVR", &rc), 0u);
        TCHECK_EQ(Do(c, uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)).res(), 0u);
        TCHECK_EQ(UdsStatus(c, uds).res(), 0u);
        TCHECK_EQ(Do(c, uds, std::vector<u32>{0x24u << 16}).res(), R_UNKNOWN_UDS_COMMAND);
        TCHECK_EQ(Do(c, ndm, std::vector<u32>{0x30u << 16}).res(), 0u);
        TCHECK_EQ(Do(c, uds, req::Shutdown()).res(), 0u);
        c.Close(uds);
        c.Close(ndm);
    }
    TCHECK_EQ(LogCount("dlpc:", m0), 0);
    {  // a game with a hub: its nwm::UDS and ndm:u commands make none; its dlp commands do (the control)
        MockRoom room;
        Lc c(room, 1, "Alice", true, true);
        const Handle uds = c.Open("nwm::UDS"), ndm = c.Open("ndm:u");
        TCHECK(uds != 0 && ndm != 0);
        TCHECK_EQ(Do(c, uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), kTestSharedMemory, kTestVersion)).res(), 0u);
        TCHECK_EQ(UdsStatus(c, uds).res(), 0u);
        TCHECK_EQ(UdsStatus(c, uds).res(), 0u);
        TCHECK_EQ(Do(c, uds, std::vector<u32>{0x24u << 16}).res(), R_UNKNOWN_UDS_COMMAND);
        TCHECK_EQ(Do(c, ndm, std::vector<u32>{0x30u << 16}).res(), 0u);
        TCHECK_EQ(Do(c, uds, req::Shutdown()).res(), 0u);
        TCHECK_EQ(LogCount("dlpc:", m0), 0);
        Game g(c, true, true);
        const size_t m = LogSize();
        TCHECK_EQ(g.FkChannels().res(), 0u);
        TCHECK_EQ(g.FkChannels().res(), 0u);  // (the same reply again: left out)
        const std::vector<TraceLine> lines = ReadTrace(m);
        TCHECK_EQ(lines.size(), 1u);
        if (lines.size() == 1) {
            TCHECK(!lines[0].srvr && lines[0].id == 0x04 && lines[0].result == 0 && lines[0].rep.size() == 1 && lines[0].rep[0] == 0x0421);
        }
        c.Close(g.fk());
        c.Close(g.sv());
        c.Close(uds);
        c.Close(ndm);
    }
    return L15End("probe");
}

/// This binary again with the given arguments: its exit code (-1 if it did not run or did not exit).
static int RunChildCode(const char* flag, const char* arg) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        return -1;
    }
    exe[n] = 0;
    const std::string cmd = std::string("\"") + exe + "\" " + flag + " " + arg;
    std::fflush(stdout);
    std::fflush(stderr);
    const int status = std::system(cmd.c_str());
    return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void L15() {
    std::printf("L15: the trace of the hub's dlp commands (\"dlpc:\" lines): off at debug 0, the whole sequence, the polls, replies that alternate, the refusals, not for the probe\n");
    for (const char* part : {"off", "td", "flood", "alt", "refused", "probe"}) {
        int code = 77;
        for (int attempt = 0; attempt < 3 && code == 77; ++attempt) {  // (77: a log line was given up for the lock; the run is void)
            code = RunChildCode("--l15", part);
        }
        if (code != 0) {
            std::fprintf(stderr, "  L15 (%s) failed (exit %d)\n", part, code);
        }
        TCHECK_EQ(code, 0);
    }
}

static int L15Child(const char* part) {
    if (std::strcmp(part, "off") == 0) {
        return L15Off();
    }
    if (std::strcmp(part, "td") == 0) {
        return L15Td();
    }
    if (std::strcmp(part, "flood") == 0) {
        return L15Flood();
    }
    if (std::strcmp(part, "alt") == 0) {
        return L15Alt();
    }
    if (std::strcmp(part, "refused") == 0) {
        return L15Refused();
    }
    if (std::strcmp(part, "probe") == 0) {
        return L15Probe();
    }
    std::fprintf(stderr, "unknown L15 part: %s\n", part);
    return 2;
}

// =====================================================================================================================

int main(int argc, char** argv) {
    LogOn log_on;  // (the tests read the lines of the log; also the level the plugin's first-call lines need)
    SetLogSink([](const char* line) {
        std::lock_guard<std::mutex> l(g_log_mutex);
        g_log.push_back(line);
    });
    if (argc == 3 && std::strcmp(argv[1], "--l14") == 0) {  // (L14 in a fresh process, see RunChild)
        Watchdog child_dog(60);
        child_dog.Step("L14 child");
        return L14Child(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--l15") == 0) {  // (one part of L15 in a fresh process, see L15)
        Watchdog child_dog(90);
        child_dog.Step("L15 child");
        return L15Child(argv[2]);
    }
    if (argc == 2 && std::strcmp(argv[1], "--l13") == 0) {  // (L13 in a fresh process)
        Watchdog child_dog(120);
        child_dog.Step("L13 child");
        return L13Child();
    }
    Watchdog dog(180);
    dog.Step("L1");
    L1();  // (first: the "first command" lines are said once in the life of the process)
    dog.Step("L3");
    L3();
    dog.Step("L2");
    L2();
    dog.Step("L4");
    L4();
    dog.Step("L7");
    L7();  // (before L5: the hub's trouble lines are the first 8 events and then one in 64 - this must not be one of the events)
    dog.Step("L5");
    L5();
    dog.Step("L9");
    L9();
    dog.Step("L6");
    L6();
    dog.Step("L8");
    L8();
    dog.Step("L10");
    L10();
    dog.Step("L11");
    L11();
    dog.Step("L12");
    L12();
    dog.Step("L13");  // (a fresh process: the trouble lines of this one are counted from its own events, and L5 expects the first eight)
    L13();
    dog.Step("L14");
    L14();
    dog.Step("L15");  // (six fresh processes: the slots of the trace are the process's)
    L15();
    dog.Step("");
    // The trouble lines of the whole run: the first 8 events and one in 64, and only the events the test made
    const int lines = LogCount("answered \"wireless off\": the network side did not run it");
    TCHECK_EQ(lines, ExpectedTroubleLines(g_trouble_events.load()));
    std::printf("\n%d checks, %d failures\n", g_checks + g_tchecks.load(), g_failures + g_tfails.load());
    return (g_failures + g_tfails.load()) ? 1 : 0;
}
