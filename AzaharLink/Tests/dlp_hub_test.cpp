// The hub of the fake Download Play lobby (azl/dlp_hub.hpp, phase 3: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE3.md) with REAL threads
// and REAL engines: the lobby, the service and the adapter behind one DlpHub per console; each console has a "network thread" (a thread
// that loops engine.Tick(now); hub.Tick(); sleep) and the test's "game threads" call hub.Serve(kind, cmd, sbuf) with the real IPC command
// words of Triple Deluxe (the builders are those of Tests/dlp_ipc_test.cpp), over the in-memory room of Tests/mock_room.hpp and its Azahar-peer
// rule checker. Real time throughout: the lobby's times (the join gives up at 3000 ms, a request is cancelled at 2000 ms) are the real ones.
//
// SCENARIOS (name = the argument that runs only that one; no argument runs them all, then the thread-discipline summary H2)
//   H1  the whole Triple Deluxe sequence by IPC words only, between two consoles on two game threads (the creator: SRVR Initialize, GetDupNoticeNeed,
//       StartAccepting, GetServerState, GetConnectingClients, GetClientInfo, GetClientState, AcceptClient, StartDistribution, SendPassphrase,
//       state 9, Finalize; the searcher: FKCL InitializeWithName, GetChannels, StartScan, GetTitleInfoInOrder, GetServerInfo, StopScan,
//       StartFakeSession + PollPending, GetMyStatus, GetConnectingNodes, GetNodeInfo, GetPassphrase, Finalize), then the game's own UDS on
//       the same two engines with the game's ids (data both ways; nothing of the lobby is left in the engine: names, seeds, nodes, channel, ids)
//   H2  THREAD DISCIPLINE. The hub builds its adapter inside, so the spy is the linker's: every UdsEngine member the adapter calls is wrapped
//       (-Wl,--wrap, the list below) and records the kind of thread that made the call (a game thread, the network thread, any other). H2a is the
//       table of DlpHub::IsInline against the table written out by hand; H2b runs every inline command, in every state of both roles, over a
//       LobbyNet spy (a decorator of EngineLobbyNet): nothing of LobbyNet but the clock; H2 (last) asserts that ALL the scenarios together made
//       ZERO engine calls from a game thread (the run-here paths of H4 are marked "exempt" and counted apart) and a large number from the
//       network thread (so the spy is alive: every wrapper must have fired).
//   H3  a request nobody serves is cancelled after about kQueuedMs and never runs; H3b: a second requester behind the cancelled one is refused at once
//   H4  no network thread (never ticked / dead / Stop()): posted commands fail at once, inline ones answer, OnGameUdsStarting / OnSessionClosed run
//       on the calling thread and free the engine; Stop() with a request in the queue answers it
//   H5  three game threads at once on one hub (inline reads and posted commands mixed, thousands of calls), a watchdog, counts that add up
//   H6  OnSessionClosed without Finalize; OnGameUdsStarting while a scan, a join, a session at 5/6/9 and a creator at 1/2/6/8/9 are in progress
//   H7  EngineHeld() over the life of a lobby; and already true when StartAccepting returns (a slow pump after the answer)
//   H8  a join nobody answers times out at kJoinMs through PollPending; a creator that is full and one that is closed
//   H9  a slow network thread (40 ms a round) and one stalled for 700 ms mid-sequence: the sequence completes, nothing is lost or doubled
//   (H10 is in Tests/dlp_adv_lobby_test.cpp: the lobby's external-poll mode.)
//   X1..X12 (below the H scenarios): what the mutation round of phase 3 found missing (the alive window, a long-running command, Stop(), the events, deferred closes)
//   F1a..F1d the commands that succeed in every state (FKCL Finalize / StopScan / StopSession, SRVR Finalize / EndAccepting) when the request cannot be served
//            (network thread silent / alive but stalled / never ticked / stopped), F2 the Stop()/Post race, F3 OnGameUdsStarting with a request in flight,
//            F4 a Tick longer than kAliveMs, F5 SetPlayerName (the descriptions are above F1a). `F1` runs F1a..F1d.
//
// BUILD (inside the devkitpro/devkitarm container, from AzaharLink/; CORE and the ENet objects as in Tests/run.sh). The --wrap options are the
// mangled names of the UdsEngine members the adapter calls (nm Sources/azl/dlp_lobby_net.o | grep UdsEngine): without them the thread spy sees
// nothing and H2 fails on "every wrapper fired".
//   WRAP="-Wl,--wrap=_ZN3azl9UdsEngine18InitializeForLobbyEmPKt -Wl,--wrap=_ZN3azl9UdsEngine17BeginHostingLobbyEjhhhPKhj
//         -Wl,--wrap=_ZN3azl9UdsEngine17ConnectBeginLobbyERKSt5arrayIhLm6EE -Wl,--wrap=_ZNK3azl9UdsEngine11ConnectPollEv
//         -Wl,--wrap=_ZNK3azl9UdsEngine12HeardLobbyAtEjjPNS0_10HeardLobbyE -Wl,--wrap=_ZN3azl9UdsEngine12PrepareLobbyEv
//         -Wl,--wrap=_ZNK3azl9UdsEngine12NodeSnapshotEPtS1_ -Wl,--wrap=_ZN3azl9UdsEngine18GetNodeInformationEtPNS_8NodeInfoE
//         -Wl,--wrap=_ZN3azl9UdsEngine15PopLobbyMessageEPhS1_jPj -Wl,--wrap=_ZN3azl9UdsEngine6SendToEthjhPKhm
//         -Wl,--wrap=_ZN3azl9UdsEngine11EjectClientEt -Wl,--wrap=_ZN3azl9UdsEngine8ShutdownEv
//         -Wl,--wrap=_ZN3azl9UdsEngine18SetApplicationDataEPKhj -Wl,--wrap=_ZN3azl9UdsEngine22UpdateNetworkAttributeEth
//         -Wl,--wrap=_ZNK3azl9UdsEngine11initializedEv"
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_hub_test.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_hub.cpp <CORE> <enet objects> $WRAP -o /tmp/dlp_hub_test
//   ThreadSanitizer (a separate binary, no ASan: the two cannot be combined; same source, same --wrap options):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=thread
//       (the same files) $WRAP -o /tmp/dlp_hub_test_tsan
//       (the CORE sources are compiled with -fsanitize=thread too, so that the engine is checked with the hub; the ENet C objects may stay as
//        built for ASan: they hold no state the test shares. Last run: 0 reports, 0 failures, three times.)
//   Run: /tmp/dlp_hub_test [H1 H3 F1 ...] (F4b, opt-in, only by name). About 65 s (ASan/UBSan, ThreadSanitizer alike); a failure prints "FAIL file:line: expression", a data race "WARNING: ThreadSanitizer", and the exit code is 1.
#pragma GCC diagnostic ignored "-Wunused-function"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <functional>
#include <initializer_list>

#include "azl/dlp_hub.hpp"
#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

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

// Results and replies written out (not the constants of the code under test).
static constexpr u32 R_WIRELESS_OFF = 0xC940AC04;  // what a request that cannot be served says
static constexpr u32 R_BAD_HEADER = 0xD9001830;
static constexpr u32 R_NOT_FOUND = 0xC880AC05;
static constexpr DlpKind kFk = DlpKind::Fkcl;
static constexpr DlpKind kSv = DlpKind::Srvr;
static constexpr u64 kFilterTd = 0x000400010010C000ull;  // Triple Deluxe's scan filter (child index 0)
static const u8 kOther9[9] = {'f', 'f', 'e', 'e', 'd', 'd', 'c', 'c', 0};

// The headers of the replies, by command id (SPEC-DLP.md sez. 2 and 3; Tests/dlp_ipc_test.cpp). Index = id.
static const u32 kFkReplyHeader[0x12] = {0,          0x00010040, 0x00020040, 0x000301C0, 0x00040080, 0x00050040,
                                         0x00060040, 0x00070042, 0x00080042, 0x00090042, 0x000A0040, 0x000B0040,
                                         0x000C0180, 0x000D0082, 0x000E02C0, 0x000F0100, 0x00100040, 0x00110040};
static const u32 kSvReplyHeader[0x11] = {0,          0x00010040, 0x00020040, 0x00030080, 0x00040200, 0x00050040,
                                         0x00060040, 0x00070040, 0x00080040, 0x00090040, 0x000A0040, 0x000B0082,
                                         0x000C02C0, 0x000D0180, 0x000E0080, 0x000F0040, 0x00100080};
static u32 Plain(u32 id) {
    return (id << 16) | 0x40;
}

// ---------------------------------------------------------------------------------------------------------------------
// The thread spy: the UdsEngine members the adapter calls, wrapped by the linker. A thread says what it is with these.

enum Fn : int {
    kFnInitForLobby, kFnBeginHostingLobby, kFnConnectBeginLobby, kFnConnectPoll, kFnHeardLobbyAt, kFnPrepareLobby, kFnNodeSnapshot,
    kFnGetNodeInformation, kFnPopLobbyMessage, kFnSendTo, kFnEjectClient, kFnShutdown, kFnSetApplicationData, kFnUpdateNetworkAttribute,
    kFnInitialized, kFnCount
};
static const char* const kFnNames[kFnCount] = {"InitializeForLobby", "BeginHostingLobby", "ConnectBeginLobby", "ConnectPoll", "HeardLobbyAt",
                                               "PrepareLobby", "NodeSnapshot", "GetNodeInformation", "PopLobbyMessage", "SendTo",
                                               "EjectClient", "Shutdown", "SetApplicationData", "UpdateNetworkAttribute", "initialized"};

static thread_local bool t_game = false;  // a game thread (the test's main thread and its game threads)
static thread_local bool t_net = false;   // a network thread
static thread_local int t_direct = 0;     // the test uses the engine itself, as the game's own nwm::UDS would
static thread_local int t_exempt = 0;     // a call that is allowed to run on a game thread (the hub's run-here paths when no network thread lives)
// (X9) a game thread whose buffer resolver waits for g_map_hold to clear: it sits inside a table read, holding the hub's dlp_mutex_
static thread_local bool t_map_gate = false;
static std::atomic<bool> g_map_hold{false};
static std::atomic<bool> g_map_entered{false};
// (F1 stalled) the same idea per console: a game thread whose t_gate_mem is a console's memory parks in that memory's resolver until its gate_hold clears
class GMem;
static thread_local GMem* t_gate_mem = nullptr;

struct SpyBook {
    std::atomic<u32> game[kFnCount];    // a game thread, not exempt: must stay zero
    std::atomic<u32> exempt[kFnCount];  // a game thread inside an Exempt scope
    std::atomic<u32> net[kFnCount];
    std::atomic<u32> other[kFnCount];
    std::atomic<u32> direct[kFnCount];  // the test's own use as the game
};
static SpyBook g_book;
static constexpr int kMaxEngines = 16;
static std::atomic<const void*> g_eng_ptr[kMaxEngines];
static std::atomic<u32> g_eng_count[kMaxEngines][kFnCount];  // per engine, the adapter's calls (any thread but a Direct scope)
static std::atomic<const void*> g_slow_engine{nullptr};      // the engine whose NodeSnapshot, on a network thread, sleeps g_slow_ms
static std::atomic<u32> g_slow_ms{0};
// (X scenarios) the engine whose BeginHostingLobby, on a network thread, sleeps g_exec_slow_ms: a posted command that is RUNNING for longer than kQueuedMs
static std::atomic<const void*> g_exec_slow_engine{nullptr};
static std::atomic<u32> g_exec_slow_ms{0};
// (X scenarios) the kernel the hub sees: no event at all (CreateEvent answers 0), or events that are not one-shot (a wait does not consume the signal)
static std::atomic<bool> g_no_event{false};
static std::atomic<bool> g_sticky_events{false};
static std::atomic<bool> g_relaxed_events{false};  // (X8) events built on relaxed atomics only: a signal and a wait order NOTHING

struct Direct {  // the test is the game here
    Direct() { ++t_direct; }
    ~Direct() { --t_direct; }
};
struct Exempt {
    Exempt() { ++t_exempt; }
    ~Exempt() { --t_exempt; }
};
struct AsNet {  // the calling thread plays the network thread for a while (H2b)
    bool g, n;
    AsNet() : g(t_game), n(t_net) { t_game = false; t_net = true; }
    ~AsNet() { t_game = g; t_net = n; }
};

static void RegisterEngine(const void* e) {
    for (int i = 0; i < kMaxEngines; ++i) {
        const void* expected = nullptr;
        if (g_eng_ptr[i].compare_exchange_strong(expected, e)) {
            for (int f = 0; f < kFnCount; ++f) {
                g_eng_count[i][f].store(0);
            }
            return;
        }
    }
    std::fprintf(stderr, "too many engines registered\n");
    std::abort();
}
static void UnregisterEngine(const void* e) {
    for (int i = 0; i < kMaxEngines; ++i) {
        const void* expected = e;
        g_eng_ptr[i].compare_exchange_strong(expected, nullptr);
    }
}
static u32 EngineCount(const void* e, int fn) {
    for (int i = 0; i < kMaxEngines; ++i) {
        if (g_eng_ptr[i].load() == e) {
            return g_eng_count[i][fn].load();
        }
    }
    return 0;
}
static void Note(const void* self, int fn) {
    if (t_direct != 0) {
        g_book.direct[fn].fetch_add(1);
        return;
    }
    if (t_net) {
        g_book.net[fn].fetch_add(1);
    } else if (t_game) {
        if (t_exempt != 0) {
            g_book.exempt[fn].fetch_add(1);
        } else {
            g_book.game[fn].fetch_add(1);
        }
    } else {
        g_book.other[fn].fetch_add(1);
    }
    for (int i = 0; i < kMaxEngines; ++i) {
        if (g_eng_ptr[i].load() == self) {
            g_eng_count[i][fn].fetch_add(1);
            break;
        }
    }
}

extern "C" {
u32 __real__ZN3azl9UdsEngine18InitializeForLobbyEmPKt(UdsEngine*, u64, const u16*);
u32 __wrap__ZN3azl9UdsEngine18InitializeForLobbyEmPKt(UdsEngine* self, u64 seed, const u16* name) {
    Note(self, kFnInitForLobby);
    return __real__ZN3azl9UdsEngine18InitializeForLobbyEmPKt(self, seed, name);
}
u32 __real__ZN3azl9UdsEngine17BeginHostingLobbyEjhhhPKhj(UdsEngine*, u32, u8, u8, u8, const u8*, u32);
u32 __wrap__ZN3azl9UdsEngine17BeginHostingLobbyEjhhhPKhj(UdsEngine* self, u32 wlan, u8 id8, u8 channel, u8 max_nodes, const u8* app, u32 size) {
    Note(self, kFnBeginHostingLobby);
    if (t_net && self == g_exec_slow_engine.load() && g_exec_slow_ms.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_exec_slow_ms.load()));  // (a posted command that RUNS for a long time)
    }
    return __real__ZN3azl9UdsEngine17BeginHostingLobbyEjhhhPKhj(self, wlan, id8, channel, max_nodes, app, size);
}
u32 __real__ZN3azl9UdsEngine17ConnectBeginLobbyERKSt5arrayIhLm6EE(UdsEngine*, const Mac*);
u32 __wrap__ZN3azl9UdsEngine17ConnectBeginLobbyERKSt5arrayIhLm6EE(UdsEngine* self, const Mac* host) {
    Note(self, kFnConnectBeginLobby);
    return __real__ZN3azl9UdsEngine17ConnectBeginLobbyERKSt5arrayIhLm6EE(self, host);
}
int __real__ZNK3azl9UdsEngine11ConnectPollEv(const UdsEngine*);
int __wrap__ZNK3azl9UdsEngine11ConnectPollEv(const UdsEngine* self) {
    Note(self, kFnConnectPoll);
    if (t_net && self == g_slow_engine.load() && g_slow_ms.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_slow_ms.load()));  // (idem: the pump after the answer of StartFakeSession)
    }
    return __real__ZNK3azl9UdsEngine11ConnectPollEv(self);
}
bool __real__ZNK3azl9UdsEngine12HeardLobbyAtEjjPNS0_10HeardLobbyE(const UdsEngine*, u32, u32, UdsEngine::HeardLobby*);
bool __wrap__ZNK3azl9UdsEngine12HeardLobbyAtEjjPNS0_10HeardLobbyE(const UdsEngine* self, u32 index, u32 age, UdsEngine::HeardLobby* out) {
    Note(self, kFnHeardLobbyAt);
    return __real__ZNK3azl9UdsEngine12HeardLobbyAtEjjPNS0_10HeardLobbyE(self, index, age, out);
}
bool __real__ZN3azl9UdsEngine12PrepareLobbyEv(UdsEngine*);
bool __wrap__ZN3azl9UdsEngine12PrepareLobbyEv(UdsEngine* self) {
    Note(self, kFnPrepareLobby);
    return __real__ZN3azl9UdsEngine12PrepareLobbyEv(self);
}
bool __real__ZNK3azl9UdsEngine12NodeSnapshotEPtS1_(const UdsEngine*, u16*, u16*);
bool __wrap__ZNK3azl9UdsEngine12NodeSnapshotEPtS1_(const UdsEngine* self, u16* node, u16* mask) {
    Note(self, kFnNodeSnapshot);
    if (t_net && self == g_slow_engine.load() && g_slow_ms.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_slow_ms.load()));  // (a slow pump of the network thread, after it answered)
    }
    return __real__ZNK3azl9UdsEngine12NodeSnapshotEPtS1_(self, node, mask);
}
u32 __real__ZN3azl9UdsEngine18GetNodeInformationEtPNS_8NodeInfoE(UdsEngine*, u16, NodeInfo*);
u32 __wrap__ZN3azl9UdsEngine18GetNodeInformationEtPNS_8NodeInfoE(UdsEngine* self, u16 node, NodeInfo* out) {
    Note(self, kFnGetNodeInformation);
    return __real__ZN3azl9UdsEngine18GetNodeInformationEtPNS_8NodeInfoE(self, node, out);
}
bool __real__ZN3azl9UdsEngine15PopLobbyMessageEPhS1_jPj(UdsEngine*, u8*, u8*, u32, u32*);
bool __wrap__ZN3azl9UdsEngine15PopLobbyMessageEPhS1_jPj(UdsEngine* self, u8* from, u8* data, u32 capacity, u32* size) {
    Note(self, kFnPopLobbyMessage);
    return __real__ZN3azl9UdsEngine15PopLobbyMessageEPhS1_jPj(self, from, data, capacity, size);
}
u32 __real__ZN3azl9UdsEngine6SendToEthjhPKhm(UdsEngine*, u16, u8, u32, u8, const u8*, std::size_t);
u32 __wrap__ZN3azl9UdsEngine6SendToEthjhPKhm(UdsEngine* self, u16 dest, u8 channel, u32 size, u8 flags, const u8* data, std::size_t avail) {
    Note(self, kFnSendTo);
    return __real__ZN3azl9UdsEngine6SendToEthjhPKhm(self, dest, channel, size, flags, data, avail);
}
u32 __real__ZN3azl9UdsEngine11EjectClientEt(UdsEngine*, u16);
u32 __wrap__ZN3azl9UdsEngine11EjectClientEt(UdsEngine* self, u16 node) {
    Note(self, kFnEjectClient);
    return __real__ZN3azl9UdsEngine11EjectClientEt(self, node);
}
u32 __real__ZN3azl9UdsEngine8ShutdownEv(UdsEngine*);
u32 __wrap__ZN3azl9UdsEngine8ShutdownEv(UdsEngine* self) {
    Note(self, kFnShutdown);
    return __real__ZN3azl9UdsEngine8ShutdownEv(self);
}
u32 __real__ZN3azl9UdsEngine18SetApplicationDataEPKhj(UdsEngine*, const u8*, u32);
u32 __wrap__ZN3azl9UdsEngine18SetApplicationDataEPKhj(UdsEngine* self, const u8* data, u32 size) {
    Note(self, kFnSetApplicationData);
    return __real__ZN3azl9UdsEngine18SetApplicationDataEPKhj(self, data, size);
}
u32 __real__ZN3azl9UdsEngine22UpdateNetworkAttributeEth(UdsEngine*, u16, u8);
u32 __wrap__ZN3azl9UdsEngine22UpdateNetworkAttributeEth(UdsEngine* self, u16 mask, u8 flag) {
    Note(self, kFnUpdateNetworkAttribute);
    return __real__ZN3azl9UdsEngine22UpdateNetworkAttributeEth(self, mask, flag);
}
bool __real__ZNK3azl9UdsEngine11initializedEv(const UdsEngine*);
bool __wrap__ZNK3azl9UdsEngine11initializedEv(const UdsEngine* self) {
    Note(self, kFnInitialized);
    return __real__ZNK3azl9UdsEngine11initializedEv(self);
}
}  // extern "C"

static u32 BookSum(std::atomic<u32> (&a)[kFnCount]) {
    u32 n = 0;
    for (int f = 0; f < kFnCount; ++f) {
        n += a[f].load();
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// Memory of the game (regions at fake 32-bit addresses; read-only after the set-up, so threads may share the resolver).

class GMem {
public:
    u32 Alloc(u32 size) {
        Region r;
        r.addr = next_;
        r.data.assign(size == 0 ? 1 : size, 0xCC);
        next_ += (size + 0x40 + 15) & ~15u;
        regions_.push_back(std::move(r));
        return regions_.back().addr;
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = &Thunk;
        return r;
    }
    u8* Find(u32 addr, u32 size = 1) {
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.data.size()) {
                return r.data.data() + (addr - r.addr);
            }
        }
        return nullptr;
    }
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        if (t_map_gate) {
            g_map_entered.store(true);
            while (g_map_hold.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        GMem* self = static_cast<GMem*>(ctx);
        if (t_gate_mem == self) {
            self->gate_entered.store(true);
            while (self->gate_hold.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return self->Find(addr, size);
    }
    std::atomic<bool> gate_hold{false};     // (F1 stalled) see t_gate_mem
    std::atomic<bool> gate_entered{false};

private:
    struct Region {
        u32 addr = 0;
        std::vector<u8> data;
    };
    std::vector<Region> regions_;
    u32 next_ = 0x08000000;
};

// ---------------------------------------------------------------------------------------------------------------------
// One console: the engine on the room, the memory of its games, the hub (the lobby, the service and the adapter) and its counters.

static LobbyConfig MakeCfg(u8 last, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = 0x000400000010C000ull;
    c.friend_seed = 0xF000u + last;
    Put(c.name, name);
    return c;
}

/// The kernel the hub sees: the rig's, and a count of the waits that ended by their slice and not by a signal.
class HubKernel final : public KernelOps {
public:
    explicit HubKernel(KernelOps* inner) : inner_(inner) {}
    u32 CreateEvent() override {
        if (g_no_event.load()) {
            return 0;  // (X6: the kernel has no event for the hub)
        }
        if (g_sticky_events.load()) {
            sticky_ = true;  // (X7: an event that a wait does not consume; only ClearEvent resets it)
            return 77;
        }
        if (g_relaxed_events.load()) {
            relaxed_ = true;  // (X8: one-shot, relaxed atomics only: no signal/wait pair synchronises anything)
            return 88;
        }
        return inner_->CreateEvent();
    }
    u32 DuplicateForGame(u32 h) override { return inner_->DuplicateForGame(h); }
    void SignalEvent(u32 h) override {
        signals_.fetch_add(1);
        if (relaxed_) {
            flag_.store(true, std::memory_order_relaxed);
            return;
        }
        if (sticky_) {
            std::lock_guard<std::mutex> lk(m_);
            set_ = true;
            cv_.notify_all();
            return;
        }
        inner_->SignalEvent(h);
    }
    void ClearEvent(u32 h) override {
        {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lk(hook_m_);
                hook.swap(on_clear_);
            }
            if (hook) {
                hook();  // (F2: Post clears the event right before it stores Queued - after both of its checks of the network thread)
            }
        }
        if (relaxed_) {
            flag_.store(false, std::memory_order_relaxed);
            return;
        }
        if (sticky_) {
            std::lock_guard<std::mutex> lk(m_);
            set_ = false;
            return;
        }
        inner_->ClearEvent(h);
    }
    bool WaitEvent(u32 h, s64 timeout_ns) override {
        waits_.fetch_add(1);
        bool ok;
        if (relaxed_) {
            const u64 end = platform::NowMs() + static_cast<u64>(timeout_ns / 1000000);
            ok = false;
            while (!(ok = flag_.exchange(false, std::memory_order_relaxed)) && platform::NowMs() < end) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } else if (sticky_) {
            std::unique_lock<std::mutex> lk(m_);
            ok = cv_.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [&] { return set_; });
        } else {
            ok = inner_->WaitEvent(h, timeout_ns);
        }
        if (!ok) {
            timeouts_.fetch_add(1);
        }
        return ok;
    }
    void CloseEvent(u32 h) override {
        if (!sticky_ && !relaxed_) {
            inner_->CloseEvent(h);
        }
    }
    void SleepNs(s64 ns) override { inner_->SleepNs(ns); }
    u32 waits() const { return waits_.load(); }
    u32 timeouts() const { return timeouts_.load(); }
    u32 signals() const { return signals_.load(); }
    /// Runs `f` once, inside the next ClearEvent (a requester's Post, between its checks and its store of Queued). Empty clears it.
    void SetOnClear(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(hook_m_);
        on_clear_ = std::move(f);
    }

private:
    KernelOps* inner_;
    std::mutex hook_m_;
    std::function<void()> on_clear_;
    std::atomic<u32> waits_{0}, timeouts_{0}, signals_{0};
    bool sticky_ = false;
    bool relaxed_ = false;
    std::atomic<bool> flag_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool set_ = false;
};

struct Node {
    Node(MockRoom& room, u8 last, const char* name)
        : con(room, last), kernel(&con.fake_kernel), hub(con.engine.get(), &kernel, MakeCfg(last, name), mem.Resolver()) {
        RegisterEngine(con.engine.get());
    }
    ~Node() { UnregisterEngine(con.engine.get()); }
    UdsEngine& e() { return *con.engine; }
    const Mac& mac() const { return con.mac; }
    u32 Spy(int fn) const { return EngineCount(con.engine.get(), fn); }

    Console con;
    HubKernel kernel;
    GMem mem;
    DlpHub hub;
    std::atomic<u32> n_inline{0};  // the calls the games made, by the hub's own classification
    std::atomic<u32> n_posted{0};
};

/// The network thread of a console: what RoomClient's thread does every round (the engine's Tick, then the hub's).
class NetThread {
public:
    explicit NetThread(Node& n, u32 sleep_ms = 3) : node_(n), sleep_ms_(sleep_ms), thread_([this] { Run(); }) {}
    ~NetThread() { Stop(); }
    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
        }
    }
    void Pause(bool on) { paused_ = on; }
    void Stall(u32 ms) { stall_ms_ = ms; }  // the next round sleeps this long instead
    void SetSleep(u32 ms) { sleep_ms_ = ms; }
    u64 rounds() const { return rounds_; }

private:
    void Run() {
        t_net = true;
        t_game = false;
        while (!stop_) {
            if (!paused_) {
                node_.e().Tick(platform::NowMs());
                node_.hub.Tick();
                ++rounds_;
            }
            const u32 st = stall_ms_.exchange(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(st != 0 ? st : sleep_ms_.load()));
        }
    }
    Node& node_;
    std::atomic<u32> sleep_ms_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<u32> stall_ms_{0};
    std::atomic<u64> rounds_{0};
    std::thread thread_;
};

struct Fix {
    explicit Fix(std::initializer_list<const char*> names, u32 sleep_ms = 3, bool start_net = true) : sleep_ms_(sleep_ms) {
        g_rules.Reset();
        u8 last = 1;
        for (const char* nm : names) {
            nodes.push_back(std::make_unique<Node>(room, last++, nm));
        }
        if (start_net) {
            StartNet();
        }
    }
    void StartNet() {
        for (auto& n : nodes) {
            nets.push_back(std::make_unique<NetThread>(*n, sleep_ms_));
        }
        for (auto& t : nets) {  // (the plugin's network thread has ticked before a game can ask: the hub treats a thread it never saw as no thread)
            WaitFor(2000, [&] { return t->rounds() >= 2; });
        }
    }
    Node& n(std::size_t i) { return *nodes[i]; }
    NetThread& net(std::size_t i) { return *nets[i]; }

    MockRoom room;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<NetThread>> nets;  // (destroyed first: no tick after the nodes begin to go)
    u32 sleep_ms_;
};

static bool EngFree(Node& n) {  // the engine is the game's again: not initialised, and the hub says so
    Direct d;
    return !n.e().initialized() && !n.hub.EngineHeld();
}

// ---------------------------------------------------------------------------------------------------------------------
// The game thread: the commands as the games send them (Tests/dlp_ipc_test.cpp) through DlpHub::Serve.

static constexpr u32 kCmdWords = 64;
static constexpr u32 kGuardWords = 8;
static constexpr u32 kGuard = 0xFEEDFACE;

struct Rep {
    u32 w[16] = {};
    DlpOutcome out = DlpOutcome::Done;
    u32 hdr() const { return w[0]; }
    u32 res() const { return w[1]; }
};
struct JoinResult {
    bool done = false;
    bool pending = false;
    Rep rep;
    u32 ms = 0;
};

static u32 MacW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 MacW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}
static void NameWords(const char* name, u32 w[5]) {
    u16 u[kNameUnits];
    Put(u, name);
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
    for (u32 i = 0; i < kNameUnits; ++i) {
        const u16 u = LoadLE16(p + 2 * i);
        if (u == 0) {
            break;
        }
        s += static_cast<char>(u);
    }
    return s;
}
/// The 10 words of a DLPNodeInfo in a reply, from word `at`.
static void CheckNodeRecord(const Rep& r, u32 at, u64 seed, const char* name, u32 node) {
    const u8* rec = reinterpret_cast<const u8*>(&r.w[at]);
    TCHECK_EQ(LoadLE64(rec), seed);
    TCHECK(NameAt(rec + 0x0C) == name);
    TCHECK_EQ(LoadLE32(rec + 0x24), node);
}

using ServeFn = std::function<DlpOutcome(DlpKind, u32*, const u32*)>;
using PendFn = std::function<bool(u32*)>;

class Game {
public:
    Game(GMem& mem, ServeFn serve, PendFn pend, std::atomic<u32>* n_inline, std::atomic<u32>* n_posted)
        : mem_(mem), serve_(std::move(serve)), pend_(std::move(pend)), n_inline_(n_inline), n_posted_(n_posted) {
        title_ = mem.Alloc(kTitleInfoSize);
        server_ = mem.Alloc(kServerInfoSize);
        ids_ = mem.Alloc(16);
        cli_ = mem.Alloc(16);
        sb_title_[0] = (kTitleInfoSize << 14) | 2;
        sb_title_[1] = title_;
        sb_server_[0] = (kServerInfoSize << 14) | 2;
        sb_server_[1] = server_;
        sb_ids_[0] = (16u << 14) | 2;
        sb_ids_[1] = ids_;
    }
    explicit Game(Node& n)
        : Game(n.mem, [&n](DlpKind k, u32* c, const u32* s) { return n.hub.Serve(k, c, s); }, [&n](u32* c) { return n.hub.PollPending(c); },
               &n.n_inline, &n.n_posted) {}

    /// One request through the hub, on the calling thread. The words after the arguments are garbage and the 8 after the 64 are guards: a reply
    /// that wrote beyond what its header declares (or a network thread that scribbled on the buffer) shows.
    Rep Call(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf = nullptr) {
        u32 buf[kCmdWords + kGuardWords];
        u32 before[kCmdWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            buf[kCmdWords + i] = kGuard;
        }
        buf[0] = header;
        u32 i = 1;
        for (u32 a : args) {
            buf[i++] = a;
        }
        std::memcpy(before, buf, sizeof(before));
        const u32 id = header >> 16;
        if (n_inline_ != nullptr) {
            (DlpHub::IsInline(kind, id) ? *n_inline_ : *n_posted_).fetch_add(1);
        }
        Rep r;
        r.out = serve_(kind, buf, sbuf);
        std::memcpy(r.w, buf, sizeof(r.w));
        if (r.out == DlpOutcome::Done) {
            u32 extent = 1 + ((buf[0] >> 6) & 0x3F) + (buf[0] & 0x3F);
            extent = std::min(extent, kCmdWords);
            for (u32 k = extent; k < kCmdWords; ++k) {
                TCHECK(buf[k] == before[k]);
            }
        } else {
            for (u32 k = 0; k < kCmdWords; ++k) {  // a join in progress touches nothing
                TCHECK(buf[k] == before[k]);
            }
        }
        for (u32 k = 0; k < kGuardWords; ++k) {
            TCHECK(buf[kCmdWords + k] == kGuard);
        }
        return r;
    }
    bool Pending(Rep* r) {
        u32 buf[kCmdWords + kGuardWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            buf[kCmdWords + i] = kGuard;
        }
        const bool done = pend_(buf);
        if (done) {
            std::memcpy(r->w, buf, sizeof(r->w));
            for (u32 k = 2; k < kCmdWords; ++k) {
                TCHECK(buf[k] == 0xC0DE0000u + k);
            }
        }
        for (u32 k = 0; k < kGuardWords; ++k) {
            TCHECK(buf[kCmdWords + k] == kGuard);
        }
        return done;
    }
    /// StartFakeSession and the wait of the HLE: PollPending until done (10 ms apart), at most `max_ms`.
    JoinResult FkJoinWait(const Mac& mac, u32 max_ms = 15000) {
        JoinResult j;
        const u64 t0 = platform::NowMs();
        j.rep = FkJoin(mac);
        if (j.rep.out != DlpOutcome::JoinPending) {
            j.done = true;
            j.ms = static_cast<u32>(platform::NowMs() - t0);
            return j;
        }
        j.pending = true;
        while (platform::NowMs() - t0 < max_ms) {
            if (Pending(&j.rep)) {
                j.done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        j.ms = static_cast<u32>(platform::NowMs() - t0);
        return j;
    }

    // ---- dlp:FKCL (request headers of SPEC-DLP.md sez. 2)
    Rep FkInit(u32 beacons = 16) { return Call(kFk, 0x00010083, {0x30000, beacons, 0x04000000, 0x1111, 0x2222}); }
    Rep FkInitName(const char* name, u32 beacons = 16) {
        u32 n[5];
        NameWords(name, n);
        return Call(kFk, 0x00110203, {0x30000, beacons, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    Rep FkFinalize() { return Call(kFk, 0x00020000, {}); }
    Rep FkEventDesc() { return Call(kFk, 0x00030000, {}); }
    Rep FkChannels() { return Call(kFk, 0x00040000, {}); }
    Rep FkScan(u64 filter = kFilterTd) {
        return Call(kFk, 0x00050180, {0x0421, static_cast<u32>(filter), static_cast<u32>(filter >> 32), 0, 0, 0});
    }
    Rep FkStopScan() { return Call(kFk, 0x00060000, {}); }
    Rep FkServerInfo(const Mac& m) { return Call(kFk, 0x00070080, {MacW1(m), MacW2(m)}, sb_server_); }
    Rep FkTitleInfo(const Mac& m) { return Call(kFk, 0x00080100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}, sb_title_); }
    Rep FkInOrder(u32 restart) { return Call(kFk, 0x00090040, {restart}, sb_title_); }
    Rep FkDelete(const Mac& m) { return Call(kFk, 0x000A0080, {MacW1(m), MacW2(m)}); }
    Rep FkJoin(const Mac& m) { return Call(kFk, 0x000B0100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}); }
    Rep FkStatus() { return Call(kFk, 0x000C0000, {}); }
    Rep FkNodes(u32 n) { return Call(kFk, 0x000D0040, {n}, sb_ids_); }
    Rep FkNodeInfo(u32 node) { return Call(kFk, 0x000E0040, {node}); }
    Rep FkPass() { return Call(kFk, 0x000F0000, {}); }
    Rep FkStopSession() { return Call(kFk, 0x00100000, {}); }

    // ---- dlp:SRVR (SPEC-DLP.md sez. 3)
    Rep SvInit(u32 max_clients, u32 child_index = 0) {
        return Call(kSv, 0x00010183, {0x85000, max_clients, 0xE, child_index, 0x40000, 2, 0x04000000, 0x5555, 0x6666});
    }
    Rep SvInitName(u32 max_clients, const char* name) {
        u32 n[5];
        NameWords(name, n);
        return Call(kSv, 0x000F0303, {0x85000, max_clients, 0xE, 0, 0x40000, 2, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x7777, 0x8888});
    }
    Rep SvFinalize() { return Call(kSv, 0x00020000, {}); }
    Rep SvState() { return Call(kSv, 0x00030000, {}); }
    Rep SvEventDesc() { return Call(kSv, 0x00040000, {}); }
    Rep SvAccepting(u32 manual, u32 channel) { return Call(kSv, 0x00050080, {manual, channel}); }
    Rep SvEnd() { return Call(kSv, 0x00060000, {}); }
    Rep SvDistribute() { return Call(kSv, 0x00070000, {}); }
    Rep SvPass(const u8* nine) {
        u32 w[3];
        PassWords(nine, w);
        return Call(kSv, 0x000800C0, {w[0], w[1], w[2]});
    }
    Rep SvAccept(u32 node) { return Call(kSv, 0x00090040, {node}); }
    Rep SvDisconnect(u32 node) { return Call(kSv, 0x000A0040, {node}); }
    Rep SvClients(u32 n, u32 bytes) { return Call(kSv, 0x000B0042, {n, (bytes << 4) | 0xC, cli_}); }
    Rep SvClientInfo(u32 node) { return Call(kSv, 0x000C0040, {node}); }
    Rep SvClientState(u32 node) { return Call(kSv, 0x000D0040, {node}); }
    Rep SvIsChild() { return Call(kSv, 0x000E0040, {0xE}); }
    Rep SvDup() { return Call(kSv, 0x00100000, {}); }

    // ---- what the game reads in its buffers
    const u8* TitleRec() { return mem_.Find(title_, kTitleInfoSize); }
    const u8* ServerRec() { return mem_.Find(server_, kServerInfoSize); }
    u16 NodeId(u32 i) { return LoadLE16(mem_.Find(ids_, 8) + 2 * i); }
    u16 ClientId(u32 i) { return LoadLE16(mem_.Find(cli_, 8) + 2 * i); }
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
    GMem& mem_;
    ServeFn serve_;
    PendFn pend_;
    std::atomic<u32>* n_inline_;
    std::atomic<u32>* n_posted_;
    u32 title_ = 0, server_ = 0, ids_ = 0, cli_ = 0;
    u32 sb_title_[2] = {}, sb_server_[2] = {}, sb_ids_[2] = {};
};

// ---------------------------------------------------------------------------------------------------------------------
// A watchdog: a scenario that does not end (a deadlock, a wake-up that never comes) kills the process with a message.

class Watchdog {
public:
    Watchdog() : thread_([this] { Run(); }) {}
    ~Watchdog() {
        stop_ = true;
        thread_.join();
    }
    void Arm(u32 seconds, const char* what) {
        what_.store(what);
        deadline_.store(platform::NowMs() + static_cast<u64>(seconds) * 1000);
    }
    void Disarm() { deadline_.store(0); }

private:
    void Run() {
        while (!stop_) {
            const u64 d = deadline_.load();
            if (d != 0 && platform::NowMs() > d) {
                std::fprintf(stderr, "WATCHDOG: %s did not finish in time (a deadlock or a wake-up that never came)\n", what_.load());
                std::fflush(stderr);
                std::_Exit(3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::atomic<u64> deadline_{0};
    std::atomic<const char*> what_{""};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
static Watchdog* g_watchdog = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Small drivers shared by the scenarios.

static u32 Ms(u64 since) {
    return static_cast<u32>(platform::NowMs() - since);
}

/// The creator as Triple Deluxe opens a lobby (SRVR Initialize, GetDupNoticeNeed, StartAccepting(0, channel)).
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
        Rep a = g.FkInOrder(1);
        if (a.res() != 0) {
            return false;
        }
        const u8* t = g.TitleRec();
        return std::memcmp(t + kTitleInfoMac, host.data(), 6) == 0;
    });
}
/// StopScan, then StartFakeSession and the wait.
static JoinResult JoinGroup(Game& g, const Mac& host, u32 max_ms = 15000) {
    const Rep r = g.FkStopScan();
    TCHECK_EQ(r.res(), 0u);
    return g.FkJoinWait(host, max_ms);
}

// =====================================================================================================================
// H1: the whole of Triple Deluxe's lobby by IPC words, then the game's own UDS on the same engines.

struct TdShared {
    std::atomic<bool> searcher_in_session{false};
    std::function<void()> before_join;        // run on the searcher's game thread just before StartFakeSession
    std::function<void()> before_distribute;  // ... and on the creator's before StartDistribution
    std::atomic<u32> join_ms{0};
    std::atomic<bool> creator_done{false}, searcher_done{false};
    std::atomic<u32> pending_polls{0};
};

static void CreatorTd(Game& g, Node& n, TdShared& s) {
    t_game = true;
    Rep r = g.SvInit(3);
    TCHECK_EQ(r.hdr(), 0x00010040u);
    TCHECK_EQ(r.res(), 0u);
    r = g.SvDup();  // GetDupNoticeNeed: [0x00100080, 0, 0]
    TCHECK_EQ(r.hdr(), 0x00100080u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0u);
    TCHECK_EQ(g.ServerState(), 1u);
    TCHECK(!n.hub.EngineHeld());
    r = g.SvAccepting(0, 6);
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(n.hub.EngineHeld());  // (already true when the answer is: the hub refreshes its hints before it publishes the answer)
    TCHECK_EQ(g.ServerState(), 2u);

    bool seen = WaitFor(10000, [&] { return g.ClientsTotal() == 1; });
    TCHECK(seen);
    r = g.SvClients(4, 8);
    TCHECK_EQ(r.hdr(), 0x000B0082u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 1u);
    TCHECK_EQ(r.w[3], (8u << 4) | 0xC);  // the descriptor and the address of the game's array, echoed
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
    r = g.SvAccept(2);  // (TD never calls it: the clients are accepted by themselves)
    TCHECK_EQ(r.hdr(), Plain(9));
    TCHECK_EQ(r.res(), R_NO_DATA);

    // the Start button, once the searcher has read its roster
    TCHECK(WaitFor(15000, [&] { return s.searcher_in_session.load(); }));
    if (s.before_distribute) {
        s.before_distribute();
    }
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
    r = g.SvPass(kOther9);
    TCHECK_EQ(r.hdr(), 0x00080040u);
    TCHECK_EQ(r.res(), R_INVALID_STATE);
    TCHECK(WaitFor(15000, [&] { return g.ServerState() == 9; }));
    TCHECK(!n.hub.EngineHeld());  // (the network was closed before the game saw 9)
    TCHECK_EQ(g.ClientsTotal(), 0u);
    r = g.SvFinalize();
    TCHECK_EQ(r.hdr(), 0x00020040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(g.ServerState(), 0u);
    TCHECK(!n.hub.EngineHeld());
    s.creator_done = true;
}

static void SearcherTd(Game& g, Node& n, Node& host, TdShared& s) {
    t_game = true;
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
    TCHECK(!n.hub.EngineHeld());  // looking needs no engine
    // GetTitleInfoInOrder(1) then (0) until kNoData, as the game does
    bool got = WaitFor(10000, [&] {
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
    // the record of the group
    r = g.FkServerInfo(host.mac());
    TCHECK_EQ(r.hdr(), 0x00070042u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], StaticBufferDesc(kServerInfoSize, 0));
    const u8* si = g.ServerRec();
    TCHECK(std::memcmp(si + kServerInfoMac, host.mac().data(), 6) == 0);
    TCHECK_EQ(si[kServerInfoMaxClients], 4u);
    TCHECK_EQ(si[kServerInfoJoined], 1u);  // (the host counted)
    TCHECK(NameAt(si + kServerInfoHostName) == "Alice");
    const u64 stamp = LoadLE64(si + kServerInfoTick);  // the tick the entry was last heard at: within the 5 s the game keeps entries for
    const u64 now_ticks = platform::NowTicks();
    TCHECK(stamp <= now_ticks + 10 * AZL_TICKS_PER_MS);
    TCHECK(now_ticks - stamp < 5000ull * AZL_TICKS_PER_MS);
    r = g.FkTitleInfo(host.mac());  // (not called by TD, answered all the same)
    TCHECK_EQ(r.res(), 0u);
    r = g.FkStopScan();
    TCHECK_EQ(r.hdr(), 0x00060040u);
    TCHECK_EQ(r.res(), 0u);
    if (s.before_join) {
        s.before_join();
    }
    const JoinResult j = g.FkJoinWait(host.mac());
    s.join_ms = j.ms;
    TCHECK(j.pending);
    TCHECK(j.done);
    TCHECK_EQ(j.rep.hdr(), 0x000B0040u);
    TCHECK_EQ(j.rep.res(), 0u);
    TCHECK(n.hub.EngineHeld());
    r = g.FkStatus();  // state 5, connected, node 2
    TCHECK_EQ(r.hdr(), 0x000C0180u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(r.w[2], 0x05010002u);
    TCHECK(WaitFor(8000, [&] { return g.NodesTotal() == 2; }));
    r = g.FkNodes(4);
    TCHECK_EQ(r.hdr(), 0x000D0082u);
    TCHECK_EQ(r.w[2], 2u);
    TCHECK_EQ(r.w[3], StaticBufferDesc(4, 0));
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
    TCHECK(n.hub.EngineHeld());  // a searcher at 9 keeps the engine (its roster) until the game finalises
    r = g.FkFinalize();
    TCHECK_EQ(r.hdr(), 0x00020040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK(!n.hub.EngineHeld());
    s.searcher_done = true;
}

/// The two game threads of the sequence, joined. `a` is the creator, `b` the searcher.
static void RunTd(Fix& f, TdShared& s) {
    Game ga(f.n(0)), gb(f.n(1));
    std::thread tc([&] { CreatorTd(ga, f.n(0), s); });
    std::thread ts([&] { SearcherTd(gb, f.n(1), f.n(0), s); });
    tc.join();
    ts.join();
    TCHECK(s.creator_done.load());
    TCHECK(s.searcher_done.load());
}

/// What the engines saw of the lobby, from the spy: nothing lost, nothing doubled.
static void CheckTdEngineCalls(Node& a, Node& b) {
    TCHECK_EQ(a.Spy(kFnInitForLobby), 1u);
    TCHECK_EQ(a.Spy(kFnBeginHostingLobby), 1u);
    TCHECK_EQ(a.Spy(kFnConnectBeginLobby), 0u);
    TCHECK_EQ(a.Spy(kFnShutdown), 1u);  // (state 9 closed the network; Finalize found nothing to close)
    TCHECK(a.Spy(kFnSendTo) >= 2u);     // START and GO
    TCHECK(a.Spy(kFnSetApplicationData) >= 1u);
    TCHECK(a.Spy(kFnUpdateNetworkAttribute) >= 1u);
    TCHECK_EQ(b.Spy(kFnInitForLobby), 1u);
    TCHECK_EQ(b.Spy(kFnConnectBeginLobby), 1u);
    TCHECK_EQ(b.Spy(kFnBeginHostingLobby), 0u);
    TCHECK_EQ(b.Spy(kFnShutdown), 1u);
    TCHECK(b.Spy(kFnPrepareLobby) >= 1u);
    TCHECK(b.Spy(kFnHeardLobbyAt) >= 1u);
    TCHECK(b.Spy(kFnPopLobbyMessage) >= 1u);
    TCHECK(b.Spy(kFnGetNodeInformation) >= 2u);
    TCHECK(b.Spy(kFnSendTo) >= 2u);  // START_ACK and GO_ACK
}

static void CheckHubBooks(Node& n, u32 expect_ran_here = 0) {
    TCHECK_EQ(n.hub.served_inline(), n.n_inline.load());
    TCHECK_EQ(n.hub.served_posted(), n.n_posted.load());
    TCHECK_EQ(n.hub.cancelled(), 0u);
    TCHECK_EQ(n.hub.failed_fast(), 0u);
    TCHECK_EQ(n.hub.ran_here(), expect_ran_here);
}

static void H1() {
    std::printf("H1: the whole Triple Deluxe sequence by IPC words, then the game's own UDS\n");
    Fix f({"Alice", "Bob"}, 3, false);
    Node& a = f.n(0);
    Node& b = f.n(1);
    SentFrames sent_a(a.con);
    f.StartNet();

    TdShared s;
    RunTd(f, s);
    CheckTdEngineCalls(a, b);
    CheckHubBooks(a);
    CheckHubBooks(b);
    TCHECK(EngFree(a));
    TCHECK(EngFree(b));

    // What A put on the air during the lobby: beacons of the Download Play's ids and channel, none of the game's
    constexpr std::size_t kWlanAt = offsetof(NetworkInfo, wlan_comm_id_be) - offsetof(NetworkInfo, oui_value);
    constexpr std::size_t kId8At = offsetof(NetworkInfo, id) - offsetof(NetworkInfo, oui_value);
    u32 lobby_beacons = 0;
    for (const WifiPacket& p : sent_a.All()) {
        if (p.type == WifiPacket::Type::Beacon) {
            const BeaconTags tags = FindBeaconTags(p.data);
            TCHECK(tags.network != nullptr && tags.network_size > kId8At);
            if (tags.network != nullptr && tags.network_size > kId8At) {
                TCHECK_EQ(LoadBE32(tags.network + kWlanAt), UdsEngine::kLobbyWlanCommId);
                TCHECK_EQ(tags.network[kId8At], UdsEngine::kLobbyId8);
                TCHECK_EQ(p.channel, 6u);
                ++lobby_beacons;
            }
        }
    }
    std::printf("  H1: %u beacons of the lobby\n", lobby_beacons);
    TCHECK(lobby_beacons >= 1u);
    sent_a.Clear();

    // The game's nwm::UDS now, on the same engines, with ids, names, seeds and channel of its own - as the plugin lets it: the hub first.
    const u32 posted_a = a.hub.served_posted(), posted_b = b.hub.served_posted();
    a.hub.OnGameUdsStarting();  // (TD finalised both roles: nothing is held, nothing is asked of the network thread)
    b.hub.OnGameUdsStarting();
    TCHECK_EQ(a.hub.served_posted(), posted_a);
    TCHECK_EQ(b.hub.served_posted(), posted_b);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    {
        Direct d;
        u32 ev = 0;
        TCHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        TCHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        NetworkInfo ni = MakeNetworkInfo(4, 4);
        ni.channel = 1;
        TCHECK_EQ(a.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
        u8 ch = 0;
        TCHECK_EQ(a.e().GetChannel(&ch), 0u);
        TCHECK_EQ(ch, 1u);  // (not the lobby's 6)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // (a host has beaconed for a while when a game scans for it)
        std::vector<u8> out(0x4000);
        ScanParams sp;
        sp.wlan_comm_id = 0x0010CAFE;
        sp.id8 = 1;
        sp.channel_mask = 0x0001;
        sp.dwell_ms = 250;
        TCHECK_EQ(b.e().RecvBeaconBroadcastData(out.data(), static_cast<u32>(out.size()), sp), 0u);
        TCHECK_EQ(LoadLE32(out.data() + 8), 1u);  // the game's network only
        u8 info[0x3C] = {};
        std::memcpy(info, a.mac().data(), 6);
        info[6] = 1;
        TCHECK_EQ(b.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
        NodeInfo got;
        TCHECK_EQ(b.e().GetNodeInformation(1, &got), 0u);
        TCHECK_EQ(got.friend_code_seed, 0xA1A1u);  // not the lobby's identity (Alice, 0xF001)
        TCHECK(Str(got.username) == "GameHost");
        TCHECK_EQ(b.e().GetNodeInformation(2, &got), 0u);
        TCHECK_EQ(got.friend_code_seed, 0xB2B2u);
        TCHECK(Str(got.username) == "GameGuest");
        TCHECK_EQ(a.e().GetNodeInformation(2, &got), 0u);
        TCHECK(Str(got.username) == "GameGuest");
        TCHECK_EQ(a.e().GetNodeInformation(1, &got), 0u);
        TCHECK_EQ(got.friend_code_seed, 0xA1A1u);
        // data both ways on the game's own channel, and no lobby message ever shows in a bind
        u32 event = 0;
        TCHECK_EQ(a.e().Bind(1, 0x2000, 3, 0xFFFF, &event), 0u);
        TCHECK_EQ(b.e().Bind(1, 0x2000, 3, 0xFFFF, &event), 0u);
        const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
        const u8 world[6] = {'w', 'o', 'r', 'l', 'd', '!'};
        TCHECK_EQ(b.e().SendTo(1, 3, 5, 0, hello, 5), 0u);
        TCHECK_EQ(a.e().SendTo(2, 3, 6, 0, world, 6), 0u);
        f.room.WaitIdle();
        std::vector<u8> pulled;
        u32 size = 0;
        u16 src = 0;
        TCHECK_EQ(PullInto(a.e(), 1, &pulled, &size, &src), 0u);
        TCHECK_EQ(size, 5u);
        TCHECK_EQ(src, 2u);
        TCHECK(std::memcmp(pulled.data(), hello, 5) == 0);
        TCHECK_EQ(PullInto(b.e(), 1, &pulled, &size, &src), 0u);
        TCHECK_EQ(size, 6u);
        TCHECK_EQ(src, 1u);
        TCHECK(std::memcmp(pulled.data(), world, 6) == 0);
        TCHECK_EQ(PullInto(a.e(), 1, &pulled, &size, &src), 0u);
        TCHECK_EQ(size, 0u);  // nothing more (no lobby message, no echo)
        u8 from = 0, mdata[16];
        u32 msize = 0;
        TCHECK(!a.e().PopLobbyMessage(&from, mdata, 16, &msize));  // the ring is off: nothing is kept for a game
        TCHECK(!b.e().PopLobbyMessage(&from, mdata, 16, &msize));
    }
    // and on the air the creator beaconed the game's network and only that
    u32 game_beacons = 0;
    for (const WifiPacket& p : sent_a.All()) {
        if (p.type == WifiPacket::Type::Beacon) {
            const BeaconTags tags = FindBeaconTags(p.data);
            if (tags.network != nullptr && tags.network_size > kId8At) {
                TCHECK_EQ(LoadBE32(tags.network + kWlanAt), 0x0010CAFEu);
                TCHECK_EQ(tags.network[kId8At], 1u);
                TCHECK_EQ(p.channel, 1u);
                const u8* app = nullptr;
                u32 app_size = 0;
                TCHECK(GetBeaconApplicationData(tags, &app, &app_size));
                TCHECK_EQ(app_size, 4u);  // the game's four bytes, not the lobby's announcement
                TCHECK(app_size == 4 && app[0] == 0xA0 && app[3] == 0xA3);
                ++game_beacons;
            }
        }
    }
    TCHECK(game_beacons >= 2u);
    CHECK_RULES();
}

// =====================================================================================================================
// H2: the thread discipline. H2a the table; H2b every inline command over a LobbyNet spy; H2 (last) the totals of the linker's spy.

static void H2a() {
    std::printf("H2a: the table of DlpHub::IsInline\n");
    // FKCL: 3 GetEventDesc, 4 GetChannels, 7 GetServerInfo, 8 GetTitleInfo, 9 GetTitleInfoInOrder, 0xA DeleteScanInfo, 0xC GetMyStatus,
    // 0xD GetConnectingNodes, 0xE GetNodeInfo, 0xF GetPassphrase. SRVR: 3 GetServerState, 4 GetEventDescription, 9 AcceptClient,
    // 0xB GetConnectingClients, 0xD GetClientState, 0xE IsChild, 0x10 GetDupNoticeNeed. Every id the service does not have (0 and above
    // the last) is "inline" too: its answer is the service's own, no network.
    static const bool fk[0x20] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    static const bool sv[0x20] = {1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    for (u32 id = 0; id < 0x20; ++id) {
        TCHECK_EQ(DlpHub::IsInline(kFk, id), fk[id]);
        TCHECK_EQ(DlpHub::IsInline(kSv, id), sv[id]);
    }
    TCHECK(DlpHub::IsInline(kFk, 0xFFFF));
    TCHECK(DlpHub::IsInline(kSv, 0x7FFF));
    TCHECK(DlpHub::IsInline(kFk, 0xFFFFFFFFu >> 16));
}

/// A LobbyNet that counts and forwards to the real adapter: the clock apart, every method a game thread must never reach.
class SpyNet final : public LobbyNet {
public:
    explicit SpyNet(UdsEngine* e) : inner_(e) {}
    u32 NonClock() const {
        u32 n = 0;
        for (int i = 0; i < 15; ++i) {
            n += calls_[i].load();
        }
        return n;
    }
    u32 Clock() const { return clock_.load(); }
    u64 NowMs() override { clock_.fetch_add(1); return inner_.NowMs(); }
    u64 NowTicks() override { clock_.fetch_add(1); return inner_.NowTicks(); }
    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override { calls_[0].fetch_add(1); return inner_.HostOpen(a, channel, me); }
    void HostUpdate(const HostAnnounce& a) override { calls_[1].fetch_add(1); inner_.HostUpdate(a); }
    void HostClose() override { calls_[2].fetch_add(1); inner_.HostClose(); }
    void Kick(u8 node) override { calls_[3].fetch_add(1); inner_.Kick(node); }
    bool ScanOpen() override { calls_[4].fetch_add(1); return inner_.ScanOpen(); }
    void ScanClose() override { calls_[5].fetch_add(1); inner_.ScanClose(); }
    bool ScanRead(u32 index, HeardGroup* out) override { calls_[6].fetch_add(1); return inner_.ScanRead(index, out); }
    bool ConnectBegin(const Mac& host, const Identity& me) override { calls_[7].fetch_add(1); return inner_.ConnectBegin(host, me); }
    int ConnectPoll() override { calls_[8].fetch_add(1); return inner_.ConnectPoll(); }
    void Disconnect() override { calls_[9].fetch_add(1); inner_.Disconnect(); }
    u8 MyNode() override { calls_[10].fetch_add(1); return inner_.MyNode(); }
    u32 Members(u8 (&nodes)[kMaxPlayers]) override { calls_[11].fetch_add(1); return inner_.Members(nodes); }
    bool ReadNode(u8 node, NodeEntry* out) override { calls_[12].fetch_add(1); return inner_.ReadNode(node, out); }
    bool Send(u8 to_node, const u8* data, u32 size) override { calls_[13].fetch_add(1); return inner_.Send(to_node, data, size); }
    bool Receive(u8* from, u8* data, u32 capacity, u32* size) override { calls_[14].fetch_add(1); return inner_.Receive(from, data, capacity, size); }
    bool engine_held() const { return inner_.engine_held(); }

private:
    EngineLobbyNet inner_;
    std::atomic<u32> calls_[16] = {};
    std::atomic<u32> clock_{0};
};

/// A console that drives the lobby and the service itself, in the hub's external-poll mode: the test plays the network thread when it says so.
struct SpyPlayer {
    SpyPlayer(MockRoom& room, u8 last, const char* name) : con(room, last), net(con.engine.get()), lobby(&net, MakeCfg(last, name)) {
        svc.SetLobby(&lobby);
        lobby.SetExternalPoll(true);
        game.reset(new Game(
            mem, [this](DlpKind k, u32* c, const u32* s) { return svc.Dispatch(k, c, s, mem.Resolver()); }, [this](u32* c) { return svc.PollPending(c); },
            nullptr, nullptr));
        RegisterEngine(con.engine.get());
    }
    ~SpyPlayer() { UnregisterEngine(con.engine.get()); }
    /// What the network thread does in a round: the pump, with the flag up.
    void Pump() {
        AsNet an;
        lobby.SetOnNet(true);
        lobby.Pump();
        lobby.SetOnNet(false);
    }
    /// A command that is the network thread's (not inline), run as it.
    template <typename F>
    Rep OnNet(F f) {
        AsNet an;
        lobby.SetOnNet(true);
        Rep r = f(*game);
        lobby.SetOnNet(false);
        return r;
    }
    Console con;
    SpyNet net;
    DlpLobby lobby;
    DlpService svc;
    GMem mem;
    std::unique_ptr<Game> game;
};

/// Every inline command of both services, with the arguments of a game, on a game thread: nothing of LobbyNet but the clock (the spy), and
/// nothing of the engine (the linker's spy, which counts this thread as a game thread).
static void InlineMatrix(SpyPlayer& p, const Mac& peer, const char* where) {
    Game& g = *p.game;
    const u32 before = p.net.NonClock();
    const Mac zero = {};
    const u8 pass_probe[1] = {0};
    (void)pass_probe;
    g.FkEventDesc();
    g.FkChannels();
    g.FkServerInfo(peer);
    g.FkTitleInfo(peer);
    g.FkInOrder(1);
    g.FkInOrder(0);
    g.FkDelete(zero);
    g.FkStatus();
    g.FkNodes(4);
    g.FkNodeInfo(1);
    g.FkNodeInfo(2);
    g.FkPass();
    g.SvState();
    g.SvEventDesc();
    g.SvAccept(2);
    g.SvAccept(9);
    g.SvClients(4, 8);
    g.SvClientState(2);
    g.SvClientState(1);
    g.SvIsChild();
    g.SvDup();
    for (u32 id : {0u, 0x12u, 0x13u, 0x7FFFu, 0xFFFFu}) {  // ids the service does not have
        const Rep r = g.Call(kFk, id << 16 | 0x40, {1, 2});
        TCHECK_EQ(r.hdr(), 0x00000040u);
        TCHECK_EQ(r.res(), R_BAD_HEADER);
    }
    for (u32 id : {0u, 0x11u, 0x12u, 0x7FFFu, 0xFFFFu}) {
        const Rep r = g.Call(kSv, id << 16 | 0x40, {1, 2});
        TCHECK_EQ(r.hdr(), 0x00000040u);
        TCHECK_EQ(r.res(), R_BAD_HEADER);
    }
    if (p.net.NonClock() != before) {
        std::fprintf(stderr, "  (H2b: %s: the inline commands reached LobbyNet)\n", where);
    }
    TCHECK_EQ(p.net.NonClock(), before);
}

static void H2b() {
    std::printf("H2b: every inline command, in every state of both roles, calls nothing of LobbyNet but the clock\n");
    g_rules.Reset();
    MockRoom room;
    SpyPlayer a(room, 1, "Alice"), b(room, 2, "Bob");
    Ticker ticker(std::vector<UdsEngine*>{a.con.engine.get(), b.con.engine.get()});
    auto pump_both = [&](u32 ms) {
        const u64 end = platform::NowMs() + ms;
        while (platform::NowMs() < end) {
            a.Pump();
            b.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    };
    auto until = [&](u32 ms, const std::function<bool()>& done) {
        const u64 end = platform::NowMs() + ms;
        for (;;) {
            a.Pump();
            b.Pump();
            if (done()) {
                return true;
            }
            if (platform::NowMs() >= end) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    };
    const Mac& ma = a.con.mac;
    const Mac& mb = b.con.mac;
    Game& ga = *a.game;
    Game& gb = *b.game;

    InlineMatrix(a, mb, "both off");
    InlineMatrix(b, ma, "both off");
    // the creator: Initialize (1), StartAccepting (2)
    TCHECK_EQ(a.OnNet([](Game& g) { return g.SvInit(3); }).res(), 0u);
    InlineMatrix(a, mb, "creator 1");
    TCHECK_EQ(a.OnNet([](Game& g) { return g.SvAccepting(0, 6); }).res(), 0u);
    InlineMatrix(a, mb, "creator 2, no client");
    // the searcher: Initialize (1), scanning with the group in its table (2)
    TCHECK_EQ(b.OnNet([](Game& g) { return g.FkInitName("Bob", 4); }).res(), 0u);
    InlineMatrix(b, ma, "searcher 1");
    TCHECK_EQ(b.OnNet([](Game& g) { return g.FkScan(); }).res(), 0u);
    TCHECK(until(6000, [&] { return gb.FkTitleInfo(ma).res() == 0; }));
    InlineMatrix(b, ma, "searcher 2 with a group");
    // the join: state 3 while it lasts, then 5
    TCHECK_EQ(b.OnNet([](Game& g) { return g.FkStopScan(); }).res(), 0u);
    Rep jr = b.OnNet([&](Game& g) { return g.FkJoin(ma); });
    TCHECK(jr.out == DlpOutcome::JoinPending);
    InlineMatrix(b, ma, "searcher 3 (joining)");
    Rep done;
    TCHECK(until(6000, [&] { return gb.Pending(&done); }));
    TCHECK_EQ(done.res(), 0u);
    TCHECK(until(4000, [&] { return ga.ClientsTotal() == 1; }));
    InlineMatrix(a, mb, "creator 2 with a client");
    InlineMatrix(b, ma, "searcher 5");
    // the distribution: the creator at 6 (the searcher not pumped yet, so it has not acknowledged), then both
    TCHECK_EQ(a.OnNet([](Game& g) { return g.SvDistribute(); }).res(), 0u);
    InlineMatrix(a, mb, "creator 6");
    TCHECK(until(6000, [&] { return ga.ServerState() == 8 && gb.StatusState() == 6; }));
    InlineMatrix(a, mb, "creator 8");
    InlineMatrix(b, ma, "searcher 6");
    TCHECK_EQ(a.OnNet([](Game& g) { return g.SvPass(kNine); }).res(), 0u);
    TCHECK(until(8000, [&] { return ga.ServerState() == 9 && gb.StatusState() == 9; }));
    InlineMatrix(a, mb, "creator 9");
    InlineMatrix(b, ma, "searcher 9");
    TCHECK_EQ(b.OnNet([](Game& g) { return g.FkFinalize(); }).res(), 0u);
    TCHECK_EQ(a.OnNet([](Game& g) { return g.SvFinalize(); }).res(), 0u);
    InlineMatrix(a, mb, "creator 0");
    InlineMatrix(b, ma, "searcher 1 again");
    pump_both(50);
    TCHECK(!a.net.engine_held() && !b.net.engine_held());
    // and the same commands that are NOT inline do reach it when the lobby is pumped as the network thread (the spy is alive)
    TCHECK(a.net.NonClock() > 0u);
    TCHECK(b.net.NonClock() > 0u);
    TCHECK(a.net.Clock() > 0u);  // (the reads of the tables look at the clock: that is the only thing they may ask)
    CHECK_RULES();
}

static bool g_ran_all = false;  // main(): every scenario ran (so every wrapper must have fired)

static void H2() {
    std::printf("H2: the linker's spy, over every scenario that ran\n");
    u32 game_total = 0, net_total = 0, exempt_total = 0;
    for (int f = 0; f < kFnCount; ++f) {
        const u32 g = g_book.game[f].load(), n = g_book.net[f].load(), e = g_book.exempt[f].load(), o = g_book.other[f].load(), d = g_book.direct[f].load();
        std::printf("  %-22s game %u  exempt %u  net %u  other %u  (test as the game: %u)\n", kFnNames[f], g, e, n, o, d);
        game_total += g;
        net_total += n;
        exempt_total += e;
        TCHECK_EQ(g, 0u);  // NO engine call from a game thread
    }
    std::printf("  game-thread calls %u, network-thread calls %u, exempt (run-here paths) %u\n", game_total, net_total, exempt_total);
    TCHECK_EQ(game_total, 0u);
    TCHECK(net_total > (g_ran_all ? 500u : 3u));  // the spy is alive (a run of one small scenario, F4b for one, makes only a few calls)
    // every wrapper fired on the network thread (the scenarios cover them all when they all run)
    if (g_ran_all) {
        for (int f = 0; f < kFnCount; ++f) {
            if (g_book.net[f].load() == 0) {
                std::fprintf(stderr, "  the wrapper of %s never fired on a network thread\n", kFnNames[f]);
            }
            TCHECK(g_book.net[f].load() > 0u);
        }
        TCHECK(exempt_total > 0u);  // the run-here paths of H4 did run on a game thread (and were allowed to)
    }
}


// =====================================================================================================================
// H3: a request nobody serves is cancelled after kQueuedMs and never runs.

static void H3() {
    std::printf("H3: a request that is not served in time is cancelled and never runs\n");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    Rep r = g.SvInit(3);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(g.ServerState(), 1u);
    // the hub has seen the network thread tick (it is "alive"), then the thread stops
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.net(0).Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 t0 = platform::NowMs();
    r = g.SvAccepting(0, 6);
    const u32 took = Ms(t0);
    TCHECK_EQ(r.hdr(), Plain(5));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    TCHECK(took >= DlpHub::kQueuedMs - 100);
    TCHECK(took < DlpHub::kQueuedMs + 900);
    TCHECK_EQ(a.hub.cancelled(), 1u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.served_posted(), 1u);  // (only the Initialize)
    // the network thread comes back: the cancelled request does NOT run
    f.net(0).Pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TCHECK_EQ(g.ServerState(), 1u);
    TCHECK(EngFree(a));
    TCHECK_EQ(a.hub.served_posted(), 1u);
    TCHECK_EQ(a.Spy(kFnBeginHostingLobby), 0u);
    TCHECK_EQ(a.Spy(kFnInitForLobby), 0u);
    // and the next one is served as ever (the slot is free again)
    r = g.SvAccepting(0, 6);
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(g.ServerState(), 2u);
    TCHECK(a.hub.EngineHeld());
    TCHECK_EQ(a.hub.served_posted(), 2u);
    TCHECK_EQ(a.hub.cancelled(), 1u);
}

/// Two game threads ask while the network thread is silent: the first is cancelled at kQueuedMs; the second, which waited for the slot, must not wait
/// another kQueuedMs for a thread that has been silent for two seconds - it is refused at once.
static void H3b() {
    std::printf("H3b: a second requester behind a cancelled one is refused at once\n");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g1(a), g2(a);
    TCHECK_EQ(g1.SvInit(3).res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.net(0).Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 t0 = platform::NowMs();
    Rep r1, r2;
    u32 took1 = 0, took2 = 0;
    std::thread t1([&] {
        t_game = true;
        r1 = g1.SvAccepting(0, 6);
        took1 = Ms(t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread t2([&] {
        t_game = true;
        r2 = g2.FkInit();
        took2 = Ms(t0);
    });
    t1.join();
    t2.join();
    TCHECK_EQ(r1.res(), R_WIRELESS_OFF);
    TCHECK_EQ(r2.res(), R_WIRELESS_OFF);
    TCHECK_EQ(r2.hdr(), Plain(1));
    TCHECK(took1 >= DlpHub::kQueuedMs - 100);
    TCHECK(took1 < DlpHub::kQueuedMs + 900);
    TCHECK(took2 >= DlpHub::kQueuedMs - 100);  // (it had to wait for the slot)
    TCHECK(took2 < DlpHub::kQueuedMs + 900);   // (not 2 x kQueuedMs)
    TCHECK_EQ(a.hub.cancelled() + a.hub.failed_fast(), 2u);
    f.net(0).Pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    TCHECK_EQ(g1.ServerState(), 1u);
    TCHECK_EQ(g2.StatusState(), 0u);  // neither ran
    TCHECK(EngFree(a));
}

// =====================================================================================================================
// H4: no network thread.

static void H4() {
    std::printf("H4: no network thread: posted commands fail at once, inline ones answer, the run-here paths free the engine\n");
    {
        // (a) a hub nobody ever ticked
        Fix f({"Alice"}, 3, false);
        Node& a = f.n(0);
        Game g(a);
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
        r = g.FkJoin(a.mac());  // (Done at once, not JoinPending: there is nothing to wait for)
        TCHECK(r.out == DlpOutcome::Done);
        TCHECK_EQ(r.hdr(), Plain(0xB));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK(Ms(t0) < 500);
        TCHECK_EQ(a.hub.failed_fast(), 6u);
        TCHECK_EQ(a.hub.cancelled(), 0u);
        TCHECK_EQ(a.hub.served_posted(), 0u);
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
        r = g.SvDup();
        TCHECK_EQ(r.res(), 0u);
        r = g.SvIsChild();
        TCHECK_EQ(r.res(), 0u);
        r = g.Call(kFk, 0x00120000, {});
        TCHECK_EQ(r.hdr(), 0x00000040u);
        TCHECK_EQ(r.res(), R_BAD_HEADER);
        r = g.SvClients(4, 8);
        TCHECK_EQ(r.res(), R_INVALID_STATE);  // (the creator is off)
        TCHECK_EQ(a.hub.served_inline(), 7u);
        // nothing is held: the game's Initialize needs nothing of the hub
        a.hub.OnGameUdsStarting();
        TCHECK_EQ(a.hub.ran_here(), 0u);
        TCHECK_EQ(a.hub.failed_fast(), 6u);
        // the end of a session runs here (the lobby is idle: the role's Finalize has nothing to close)
        a.hub.OnSessionClosed(kSv);
        TCHECK_EQ(a.hub.ran_here(), 0u);
        TCHECK_EQ(a.hub.deferred_closes(), 1u);
        a.hub.OnSessionClosed(kFk);
        TCHECK_EQ(a.hub.ran_here(), 0u);
        TCHECK_EQ(a.hub.deferred_closes(), 2u);
        TCHECK_EQ(a.hub.failed_fast(), 8u);
        TCHECK(EngFree(a));
        TCHECK_EQ(a.hub.served_posted(), 0u);
    }
    {
        // (b) the network thread dies (RoomClient left) while the lobby holds the engine, and kAliveMs go by
        Fix f({"Alice", "Bob"});
        Node& a = f.n(0);
        Node& b = f.n(1);
        Game ga(a), gb(b);
        OpenCreator(ga);
        TCHECK(FindGroup(gb, a.mac()));
        const JoinResult j = JoinGroup(gb, a.mac());
        TCHECK(j.done && j.rep.res() == 0);
        TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
        TCHECK(a.hub.EngineHeld());
        f.net(0).Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
        u64 t0 = platform::NowMs();
        Rep r = ga.SvDistribute();  // (not SvEnd: EndAccepting succeeds in every state, F1 - this one is refused)
        TCHECK(Ms(t0) < 300);
        TCHECK_EQ(r.hdr(), Plain(7));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK_EQ(a.hub.failed_fast(), 1u);
        TCHECK_EQ(ga.ServerState(), 2u);  // (inline answers: the lobby is as it was)
        TCHECK(a.hub.EngineHeld());
        // the game goes on to its nwm::UDS: the engine must still come back, on this thread
        const u32 shut0 = g_book.exempt[kFnShutdown].load();
        t0 = platform::NowMs();
        {
            Exempt ex;
            a.hub.OnGameUdsStarting();
        }
        TCHECK(Ms(t0) < 300);
        TCHECK_EQ(a.hub.ran_here(), 1u);
        TCHECK_EQ(a.hub.failed_fast(), 2u);
        TCHECK(g_book.exempt[kFnShutdown].load() > shut0);  // (the Shutdown ran on the game's thread: nothing else could)
        TCHECK(EngFree(a));
        TCHECK_EQ(ga.ServerState(), 1u);
        TCHECK_EQ(ga.ClientsTotal(), 0u);
        TCHECK(WaitFor(4000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));  // (its Deauthentications reached B)
        {
            Direct d;
            u32 ev = 0;
            TCHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
            TCHECK_EQ(a.e().Shutdown(), 0u);
        }
        // the creator's game closes its session without Finalize and the network thread is still gone: the role's Finalize runs here too
        {
            Exempt ex;
            a.hub.OnSessionClosed(kSv);
        }
        TCHECK_EQ(a.hub.ran_here(), 1u);  // (the end of a session is deferred now: nothing of it runs on this thread)
        TCHECK_EQ(a.hub.deferred_closes(), 1u);
        TCHECK(EngFree(a));
        // B is alive: its own hub serves as ever
        TCHECK_EQ(gb.FkStopSession().res(), 0u);
        TCHECK_EQ(b.hub.failed_fast(), 0u);
    }
    {
        // (c) Stop() with a request in the queue
        Fix f({"Alice"});
        Node& a = f.n(0);
        Game g(a);
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        f.net(0).Pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Rep r;
        u64 t_done = 0;
        const u64 t0 = platform::NowMs();
        std::thread th([&] {
            t_game = true;
            r = g.SvAccepting(0, 6);
            t_done = platform::NowMs();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const u64 t_stop = platform::NowMs();
        a.hub.Stop();
        th.join();
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK(t_done >= t_stop);
        TCHECK(t_done - t_stop < 300);  // answered by Stop(), not after kQueuedMs
        TCHECK(t_done - t0 < 1500);
        TCHECK_EQ(a.hub.cancelled(), 0u);
        TCHECK_EQ(a.hub.failed_fast(), 0u);
        f.net(0).Pause(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        TCHECK_EQ(g.ServerState(), 1u);  // it never ran, not even later
        TCHECK(EngFree(a));
        TCHECK_EQ(a.Spy(kFnBeginHostingLobby), 0u);
        // later ones fail at once, and Stop() twice is harmless
        r = g.SvAccepting(0, 6);
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK_EQ(a.hub.failed_fast(), 1u);
        a.hub.Stop();
        r = g.SvDistribute();  // (not SvEnd: EndAccepting succeeds in every state, F1)
        TCHECK_EQ(r.hdr(), Plain(7));
        TCHECK_EQ(r.res(), R_WIRELESS_OFF);
        TCHECK_EQ(a.hub.failed_fast(), 2u);
    }
    {
        // (d) a join in progress when the network thread dies: nothing decides its end (PollPending stays "not yet": the HLE's own ceiling is the way
        // out), and the end of the session, run here, cancels it and frees the engine
        Fix f({"Alice", "Bob"});
        Node& a = f.n(0);
        Node& b = f.n(1);
        Game ga(a), gb(b);
        OpenCreator(ga);
        TCHECK(FindGroup(gb, a.mac()));
        TCHECK_EQ(gb.FkStopScan().res(), 0u);
        TCHECK_EQ(ga.SvEnd().res(), 0u);  // (the door shut after the list froze: nobody answers the join)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const Rep jr = gb.FkJoin(a.mac());
        TCHECK(jr.out == DlpOutcome::JoinPending);
        f.net(1).Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
        Rep r;
        TCHECK(!gb.Pending(&r));  // (still joining: only the pump decides the end of a join)
        TCHECK(!gb.Pending(&r));
        TCHECK(b.hub.EngineHeld());
        {
            Exempt ex;
            b.hub.OnSessionClosed(kFk);
        }
        // the end of the session is deferred (nothing of the lobby's network side runs on the closing thread): the join is still pending and the
        // lobby still holds the engine until a network thread ticks again
        TCHECK_EQ(b.hub.ran_here(), 0u);
        TCHECK_EQ(b.hub.deferred_closes(), 1u);
        TCHECK(!gb.Pending(&r));
        TCHECK(b.hub.EngineHeld());
        {
            AsNet an;  // (a network thread comes back)
            b.hub.Tick();
        }
        TCHECK(gb.Pending(&r));
        TCHECK_EQ(r.hdr(), 0x000B0040u);
        TCHECK_EQ(r.res(), R_INVALID_STATE);
        TCHECK(EngFree(b));
        TCHECK_EQ(gb.StatusState(), 1u);
    }
}

// =====================================================================================================================
// H5: several game threads at once on one hub.

static bool WellFormed(DlpKind kind, u32 id, const Rep& r) {
    const u32 full = kind == kFk ? kFkReplyHeader[id] : kSvReplyHeader[id];
    switch (r.res()) {
    case 0:
        return r.hdr() == full;
    case R_INVALID_STATE:
    case R_BUSY:
    case R_NOT_FOUND:
        return r.hdr() == Plain(id);
    case R_NO_DATA:
        return r.hdr() == Plain(id) || r.hdr() == full;  // (GetClientInfo of the host: kNoData with the full header)
    default:
        return false;  // (R_WIRELESS_OFF too: the network thread lives, nothing may be refused)
    }
}

static void H5() {
    std::printf("H5: three game threads at once on one hub\n");
    g_watchdog->Arm(110, "H5");
    Fix f({"Alice"}, 1);
    Node& a = f.n(0);
    Game g1(a), g2(a), g3(a);
    std::atomic<bool> stop{false};
    std::atomic<u32> bad{0}, slow{0}, max_lat{0}, posted{0}, inline_calls{0};
    std::atomic<u32> slow_ms[16] = {};
    const Mac zero = {};
    auto timed = [&](bool is_posted, DlpKind kind, u32 id, const std::function<Rep()>& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        const Rep r = fn();
        const u32 ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (!WellFormed(kind, id, r)) {
            if (bad.fetch_add(1) < 8) {
                std::fprintf(stderr, "  H5: bad reply to %s 0x%X: %08X %08X\n", kind == kFk ? "FKCL" : "SRVR", id, r.hdr(), r.res());
            }
        }
        if (is_posted) {
            posted.fetch_add(1);
            if (ms >= 95) {
                const u32 k = slow.fetch_add(1);
                if (k < 16) {
                    slow_ms[k].store(ms);
                }
            }
            u32 prev = max_lat.load();
            while (ms > prev && !max_lat.compare_exchange_weak(prev, ms)) {
            }
        } else {
            inline_calls.fetch_add(1);
        }
    };
    // thread 1: reads of tables only (the game's per-frame polls)
    std::thread t1([&] {
        t_game = true;
        while (!stop) {
            timed(false, kSv, 3, [&] { return g1.SvState(); });
            timed(false, kSv, 0xB, [&] { return g1.SvClients(4, 8); });
            timed(false, kSv, 0xD, [&] { return g1.SvClientState(2); });
            timed(false, kFk, 0xC, [&] { return g1.FkStatus(); });
            timed(false, kFk, 9, [&] { return g1.FkInOrder(1); });
            timed(false, kFk, 0xD, [&] { return g1.FkNodes(4); });
            timed(false, kFk, 4, [&] { return g1.FkChannels(); });
            timed(false, kSv, 0xE, [&] { return g1.SvIsChild(); });
        }
    });
    // thread 2: the creator's whole life, over and over (posted commands, one lobby open and closed again and again)
    std::thread t2([&] {
        t_game = true;
        for (int i = 0; i < 160; ++i) {
            timed(true, kSv, 1, [&] { return g2.SvInit(3); });
            timed(true, kSv, 5, [&] { return g2.SvAccepting(0, 6); });
            timed(true, kSv, 6, [&] { return g2.SvEnd(); });
            timed(true, kSv, 5, [&] { return g2.SvAccepting(0, 6); });
            timed(true, kSv, 0xC, [&] { return g2.SvClientInfo(2); });
            timed(true, kSv, 0xA, [&] { return g2.SvDisconnect(2); });
            timed(false, kSv, 9, [&] { return g2.SvAccept(2); });
            timed(true, kSv, 7, [&] { return g2.SvDistribute(); });
            timed(true, kSv, 8, [&] { return g2.SvPass(kNine); });
            timed(true, kSv, 2, [&] { return g2.SvFinalize(); });
        }
    });
    // thread 3: the searcher's side, posted and inline mixed
    std::thread t3([&] {
        t_game = true;
        for (int i = 0; i < 160; ++i) {
            timed(true, kFk, 0x11, [&] { return g3.FkInitName("Bob", 4); });
            timed(true, kFk, 5, [&] { return g3.FkScan(); });
            timed(false, kFk, 9, [&] { return g3.FkInOrder(1); });
            timed(false, kFk, 8, [&] { return g3.FkTitleInfo(zero); });
            timed(true, kFk, 6, [&] { return g3.FkStopScan(); });
            timed(false, kFk, 0xC, [&] { return g3.FkStatus(); });
            timed(true, kFk, 0xB, [&] { return g3.FkJoin(zero); });
            timed(false, kFk, 0xE, [&] { return g3.FkNodeInfo(1); });
            timed(true, kSv, 0xC, [&] { return g3.SvClientInfo(3); });
            timed(true, kFk, 2, [&] { return g3.FkFinalize(); });
        }
    });
    // thread 4: the plugin's own events - the game's UDS arriving and sessions closing without Finalize - in the middle of that traffic
    std::atomic<u32> session_calls{0}, uds_calls{0};
    std::thread t4([&] {
        t_game = true;
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            a.hub.OnGameUdsStarting();  // (posted only when the lobby holds something: counted apart)
            uds_calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            a.n_posted.fetch_add(1);  // (the end of a session is always posted while the network thread lives)
            a.hub.OnSessionClosed(i % 2 == 0 ? kSv : kFk);
            session_calls.fetch_add(1);
        }
    });
    t2.join();
    t3.join();
    t4.join();
    stop = true;
    t1.join();
    g_watchdog->Disarm();
    std::printf("  H5: %u posted calls, %u inline calls, slowest posted %u ms, %u at 95 ms or more, %u missed ticks; the hub waited %u times, %u by the slice, %u signals\n",
                posted.load(), inline_calls.load(), max_lat.load(), slow.load(), a.hub.missed_ticks(), a.kernel.waits(), a.kernel.timeouts(),
                a.kernel.signals());
    for (u32 k = 0; k < slow.load() && k < 16; ++k) {
        std::printf("  H5: a posted call took %u ms\n", slow_ms[k].load());  // (waiting behind the other requester for post_mutex_ - see the line above)
    }
    TCHECK_EQ(bad.load(), 0u);
    TCHECK(posted.load() >= 2000u);  // (160 cycles of 9 + 6 posted commands)
    TCHECK(inline_calls.load() >= 5000u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK(a.hub.served_posted() >= a.n_posted.load());                  // (the session ends are in n_posted; OnGameUdsStarting posts only when something is held)
    TCHECK(a.hub.served_posted() <= a.n_posted.load() + uds_calls.load());
    TCHECK_EQ(a.hub.served_inline(), a.n_inline.load());
    // no lost wake-up: one signal per request the network thread served, never a wait that ended by its slice (100 ms); the slow calls above are
    // calls that waited behind another game thread for the slot (the slot is one: a plain mutex hands it over unfairly), not for the network thread
    TCHECK_EQ(a.kernel.signals(), a.hub.served_posted());
    TCHECK(a.kernel.timeouts() * 100 <= a.hub.served_posted());  // (zero in every run made; the allowance is for a machine that stops a thread for 100 ms)
    TCHECK(slow.load() * 50 <= posted.load());
    // everything given back
    Game g(a);
    g.SvFinalize();
    g.FkFinalize();
    TCHECK(EngFree(a));
    TCHECK_EQ(g.ServerState(), 0u);
}

// =====================================================================================================================
// H6: the ends of sessions and the game's UDS arriving in every state.

/// After the hub gave the engine back: the game's Initialize finds a clean service, with the game's own name and seed.
static void CheckGameUds(Node& n) {
    TCHECK(EngFree(n));
    Direct d;
    u32 ev = 0;
    TCHECK_EQ(n.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    TCHECK_EQ(n.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    NodeInfo got;
    TCHECK_EQ(n.e().GetNodeInformation(1, &got), 0u);
    TCHECK(Str(got.username) == "GameHost");
    TCHECK_EQ(got.friend_code_seed, 0xA1A1u);
    u8 from = 0, data[16];
    u32 size = 0;
    TCHECK(!n.e().PopLobbyMessage(&from, data, 16, &size));
    TCHECK_EQ(n.e().Shutdown(), 0u);
}

/// A creator A with the client B in, state `state` (1, 2, 6, 8, 9), then OnGameUdsStarting on A.
static void H6Creator(int state) {
    std::printf("  H6: the game's UDS starts with the creator at %d\n", state);
    Fix f({"Alice", "Bob"});
    Node& a = f.n(0);
    Node& b = f.n(1);
    Game ga(a), gb(b);
    if (state == 1) {
        OpenCreator(ga);
        TCHECK_EQ(ga.SvEnd().res(), 0u);  // EndAccepting: back to 1 with the network still open
        TCHECK(a.hub.EngineHeld());
    } else {
        OpenCreator(ga);
        TCHECK(FindGroup(gb, a.mac()));
        const JoinResult j = JoinGroup(gb, a.mac());
        TCHECK(j.done && j.rep.res() == 0);
        TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
        if (state == 6) {
            f.net(1).Pause(true);  // (B does not pump: no START_ACK, the creator stays at 6)
        }
        if (state >= 6) {
            TCHECK_EQ(ga.SvDistribute().res(), 0u);
        }
        if (state >= 8) {
            TCHECK(WaitFor(6000, [&] { return ga.ServerState() == 8; }));
        }
        if (state == 9) {
            TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
            TCHECK(WaitFor(8000, [&] { return ga.ServerState() == 9; }));
        }
    }
    TCHECK_EQ(ga.ServerState(), static_cast<u32>(state));
    const u32 posted = a.hub.served_posted();
    a.hub.OnGameUdsStarting();
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    if (state == 9) {
        TCHECK_EQ(a.hub.served_posted(), posted);  // (the network is closed already: nothing is held, nothing is asked)
        TCHECK_EQ(ga.ServerState(), 9u);
    } else {
        TCHECK_EQ(a.hub.served_posted(), posted + 1);
        TCHECK_EQ(ga.ServerState(), 1u);  // back to Initialized, the table dropped
        TCHECK_EQ(ga.ClientsTotal(), 0u);
    }
    CheckGameUds(a);
    if (state == 2) {
        // B finds its host gone: the Deauthentications of the closing, then the host-gone time of the lobby
        TCHECK(WaitFor(8000, [&] { return gb.StatusState() == 0x80; }));
        TCHECK(EngFree(b));
        TCHECK_EQ(gb.FkStopSession().res(), 0u);
        TCHECK_EQ(gb.StatusState(), 1u);
    }
}

/// A searcher B at `state` (2 scanning, 3 joining, 5, 6, 9), then OnGameUdsStarting on B.
static void H6Searcher(int state) {
    std::printf("  H6: the game's UDS starts with the searcher at %d\n", state);
    Fix f({"Alice", "Bob"});
    Node& a = f.n(0);
    Node& b = f.n(1);
    Game ga(a), gb(b);
    OpenCreator(ga);
    TCHECK(FindGroup(gb, a.mac()));
    if (state == 3) {
        // the creator closes its door after the searcher froze its list: the join is never answered
        TCHECK_EQ(gb.FkStopScan().res(), 0u);
        TCHECK_EQ(ga.SvEnd().res(), 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const Rep r = gb.FkJoin(a.mac());
        TCHECK(r.out == DlpOutcome::JoinPending);
        TCHECK_EQ(gb.StatusState(), 1u);  // (a join shows as 1)
        TCHECK(b.hub.EngineHeld());
    } else if (state >= 5) {
        const JoinResult j = JoinGroup(gb, a.mac());
        TCHECK(j.done && j.rep.res() == 0);
        TCHECK_EQ(gb.StatusState(), 5u);
        if (state >= 6) {
            TCHECK_EQ(ga.SvDistribute().res(), 0u);
            TCHECK(WaitFor(6000, [&] { return gb.StatusState() >= 6; }));
            TCHECK(WaitFor(6000, [&] { return ga.ServerState() == 8; }));
        }
        if (state == 9) {
            TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
            TCHECK(WaitFor(8000, [&] { return gb.StatusState() == 9; }));
        }
    }
    TCHECK_EQ(gb.StatusState(), static_cast<u32>(state == 3 ? 1 : state));
    const u32 posted = b.hub.served_posted();
    b.hub.OnGameUdsStarting();
    TCHECK_EQ(b.hub.ran_here(), 0u);
    TCHECK_EQ(b.hub.failed_fast(), 0u);
    TCHECK_EQ(b.hub.served_posted(), posted + 1);
    TCHECK(EngFree(b));
    GroupView unused;
    (void)unused;
    switch (state) {
    case 2:  // the table is frozen, the group in it
        TCHECK_EQ(gb.StatusState(), 1u);
        TCHECK_EQ(gb.FkTitleInfo(a.mac()).res(), 0u);
        break;
    case 3: {  // the join is cancelled; the frozen table stays (the same MAC can be tried again)
        TCHECK_EQ(gb.StatusState(), 1u);
        Rep r;
        TCHECK(gb.Pending(&r));
        TCHECK_EQ(r.hdr(), 0x000B0040u);
        TCHECK_EQ(r.res(), R_INVALID_STATE);
        TCHECK_EQ(gb.FkTitleInfo(a.mac()).res(), 0u);
        break;
    }
    case 5:
    case 6:  // back to 1, everything dropped
        TCHECK_EQ(gb.StatusState(), 1u);
        TCHECK_EQ(gb.NodesTotal(), 0u);
        TCHECK_EQ(gb.FkTitleInfo(a.mac()).res(), R_NO_DATA);
        break;
    case 9: {  // stays 9: the passphrase and the roster remain readable, the engine is free
        TCHECK_EQ(gb.StatusState(), 9u);
        const Rep r = gb.FkPass();
        TCHECK(std::memcmp(&r.w[2], kNine, 9) == 0);
        TCHECK_EQ(gb.NodesTotal(), 2u);
        break;
    }
    }
    CheckGameUds(b);
}

static void H6Closed() {
    std::printf("  H6: OnSessionClosed without Finalize, then a second session\n");
    Fix f({"Alice", "Bob"});
    Node& a = f.n(0);
    Node& b = f.n(1);
    Game ga(a), gb(b);
    // (a) the creator at 2 with a client closes the session without Finalize
    OpenCreator(ga);
    TCHECK(FindGroup(gb, a.mac()));
    JoinResult j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
    const u32 posted = a.hub.served_posted();
    a.hub.OnSessionClosed(kSv);
    TCHECK_EQ(a.hub.served_posted(), posted + 1);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK(EngFree(a));
    TCHECK_EQ(ga.ServerState(), 0u);  // finalised
    TCHECK(WaitFor(8000, [&] { return gb.StatusState() == 0x80; }));  // B: the host is gone
    TCHECK(EngFree(b));
    TCHECK_EQ(gb.FkStopSession().res(), 0u);
    TCHECK_EQ(gb.StatusState(), 1u);
    // (b) the second session works: a new lobby (opened with InitializeWithName, SRVR 0x0F: the name is the player's), found, joined, the roster of two
    TCHECK_EQ(ga.SvInitName(3, "Zed").hdr(), Plain(0xF));
    TCHECK_EQ(ga.SvDup().res(), 0u);
    TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
    TCHECK(FindGroup(gb, a.mac()));
    j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK_EQ(gb.StatusState(), 5u);
    TCHECK(WaitFor(5000, [&] { return gb.NodesTotal() == 2; }));
    {
        const Rep r = gb.FkNodeInfo(1);
        TCHECK_EQ(r.res(), 0u);
        CheckNodeRecord(r, 2, 0xF001, "Zed", 1);  // (the name the creator's game gave, in the engine's node and in the group's record)
        TCHECK_EQ(gb.FkServerInfo(a.mac()).res(), 0u);
        TCHECK(NameAt(gb.ServerRec() + kServerInfoHostName) == "Zed");
    }
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
    // (b2) the creator's game sends the client away (DisconnectClient, a posted command): the Kick reaches the engine, on the network thread
    TCHECK_EQ(ga.SvDisconnect(2).res(), 0u);
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 0; }));
    TCHECK(a.Spy(kFnEjectClient) >= 1u);
    // (c) the searcher's game closes its session without Finalize: B leaves (it was sent away already), A sees it
    b.hub.OnSessionClosed(kFk);
    TCHECK(EngFree(b));
    TCHECK_EQ(gb.StatusState(), 1u);
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 0; }));
    // (d) and B can join once more (after the creator's pump has seen the seat empty: a node that comes back before that is the one it kicked, and is kicked again)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    TCHECK_EQ(gb.FkInit(4).hdr(), Plain(1));  // (the plain Initialize, FKCL 0x01, as Triple Deluxe sends it)
    TCHECK(FindGroup(gb, a.mac()));
    j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK_EQ(gb.StatusState(), 5u);
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
    TCHECK_EQ(a.hub.cancelled() + b.hub.cancelled() + a.hub.failed_fast() + b.hub.failed_fast(), 0u);
}

static void H6() {
    std::printf("H6: OnSessionClosed without Finalize; OnGameUdsStarting in every state\n");
    g_watchdog->Arm(220, "H6");
    for (int st : {2, 3, 5, 6, 9}) {
        H6Searcher(st);
    }
    for (int st : {1, 2, 6, 8, 9}) {
        H6Creator(st);
    }
    H6Closed();
    g_watchdog->Disarm();
}

// =====================================================================================================================
// H7: EngineHeld().

/// The creator at 8 and the searcher at 6 -> the passphrase -> both at 9 (A has let go of the engine, B holds it).
static void Drive9(Game& ga, Game& gb) {
    TCHECK_EQ(ga.SvDistribute().res(), 0u);
    TCHECK(WaitFor(6000, [&] { return ga.ServerState() == 8; }));
    TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
    TCHECK(WaitFor(8000, [&] { return ga.ServerState() == 9 && gb.StatusState() == 9; }));
}

static void H7() {
    std::printf("H7: EngineHeld() over the life of a lobby\n");
    {
        Fix f({"Alice", "Bob"});
        Node& a = f.n(0);
        Node& b = f.n(1);
        Game ga(a), gb(b);
        TCHECK(!a.hub.EngineHeld());
        TCHECK_EQ(ga.SvInit(3).res(), 0u);
        TCHECK(!a.hub.EngineHeld());
        TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
        TCHECK(a.hub.EngineHeld());
        TCHECK_EQ(ga.SvEnd().res(), 0u);
        TCHECK(a.hub.EngineHeld());  // (the door is shut, the network is open)
        TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
        TCHECK(a.hub.EngineHeld());
        TCHECK_EQ(ga.SvFinalize().res(), 0u);
        TCHECK(!a.hub.EngineHeld());
        TCHECK(EngFree(a));
        OpenCreator(ga);
        TCHECK(a.hub.EngineHeld());
        a.hub.OnGameUdsStarting();
        TCHECK(!a.hub.EngineHeld());
        TCHECK(EngFree(a));
        TCHECK_EQ(ga.ServerState(), 1u);
        // the game has the engine first: the lobby is refused (kBusy) and the game's session is untouched
        {
            Direct d;
            u32 ev = 0;
            TCHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        }
        Rep r = ga.SvAccepting(0, 6);
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_BUSY);
        TCHECK(!a.hub.EngineHeld());
        {
            Direct d;
            TCHECK(a.e().initialized());
            TCHECK_EQ(a.e().Shutdown(), 0u);
        }
        // the searcher: scanning holds nothing; a join holds the engine from the moment it begins
        OpenCreator(ga);
        TCHECK(a.hub.EngineHeld());
        TCHECK(!b.hub.EngineHeld());
        TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);
        TCHECK(!b.hub.EngineHeld());
        TCHECK_EQ(gb.FkScan().res(), 0u);
        TCHECK(!b.hub.EngineHeld());
        TCHECK(WaitFor(8000, [&] { return gb.FkInOrder(1).res() == 0; }));
        TCHECK_EQ(gb.FkStopScan().res(), 0u);
        TCHECK(!b.hub.EngineHeld());
        const Rep j0 = gb.FkJoin(a.mac());
        TCHECK(j0.out == DlpOutcome::JoinPending);
        TCHECK(b.hub.EngineHeld());
        Rep done;
        TCHECK(WaitFor(8000, [&] { return gb.Pending(&done); }));
        TCHECK_EQ(done.res(), 0u);
        TCHECK(b.hub.EngineHeld());
        TCHECK_EQ(gb.FkStopSession().res(), 0u);
        TCHECK(!b.hub.EngineHeld());
        TCHECK(EngFree(b));
        TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 0; }));
        // again, and the game's UDS takes it away
        TCHECK(FindGroup(gb, a.mac()));
        JoinResult j = JoinGroup(gb, a.mac());
        TCHECK(j.done && j.rep.res() == 0);
        TCHECK(b.hub.EngineHeld());
        b.hub.OnGameUdsStarting();
        TCHECK(!b.hub.EngineHeld());
        TCHECK(EngFree(b));
        TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 0; }));
        // to state 9: the creator has let go, the searcher holds the engine until its game finalises
        TCHECK(FindGroup(gb, a.mac()));
        j = JoinGroup(gb, a.mac());
        TCHECK(j.done && j.rep.res() == 0);
        TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
        Drive9(ga, gb);
        TCHECK(!a.hub.EngineHeld());
        TCHECK(b.hub.EngineHeld());
        TCHECK_EQ(gb.FkFinalize().res(), 0u);
        TCHECK(!b.hub.EngineHeld());
        TCHECK(EngFree(b));
        // the game has the engine first (searcher): the scan is refused
        {
            Direct d;
            u32 ev = 0;
            TCHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        }
        TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);
        r = gb.FkScan();
        TCHECK_EQ(r.hdr(), Plain(5));
        TCHECK_EQ(r.res(), R_BUSY);
        TCHECK(!b.hub.EngineHeld());
    }
    {
        // The hub answers a posted command before its pump has run. EngineHeld() must be true when StartAccepting / StartFakeSession return
        // (the game's UDS Initialize may come right behind them): the network thread's pump after the answer is made slow on purpose.
        Fix f({"Alice", "Bob"});
        Node& a = f.n(0);
        Node& b = f.n(1);
        Game ga(a), gb(b);
        g_slow_engine.store(a.con.engine.get());
        g_slow_ms.store(60);
        TCHECK_EQ(ga.SvInit(3).res(), 0u);
        const Rep r = ga.SvAccepting(0, 6);
        const bool held = a.hub.EngineHeld();  // (read at once, before the slow pump ends)
        g_slow_ms.store(0);
        g_slow_engine.store(nullptr);
        TCHECK_EQ(r.res(), 0u);
        TCHECK(held);
        TCHECK(FindGroup(gb, a.mac()));
        TCHECK_EQ(gb.FkStopScan().res(), 0u);
        g_slow_engine.store(b.con.engine.get());
        g_slow_ms.store(60);
        const Rep j = gb.FkJoin(a.mac());
        const bool held_b = b.hub.EngineHeld();
        g_slow_ms.store(0);
        g_slow_engine.store(nullptr);
        TCHECK(j.out == DlpOutcome::JoinPending);
        TCHECK(held_b);
        // and the game's UDS right behind: the engine is free when OnGameUdsStarting returns, whatever the pump was doing
        b.hub.OnGameUdsStarting();
        TCHECK(EngFree(b));
    }
}

// =====================================================================================================================
// H8: the join that times out; a creator that is full, and one that is closed.

static void H8() {
    std::printf("H8: a join nobody answers times out through PollPending\n");
    g_watchdog->Arm(110, "H8");
    Fix f({"Alice", "Bob", "Cleo"});
    Node& a = f.n(0);
    Node& b = f.n(1);
    Node& c = f.n(2);
    Game ga(a), gb(b), gc(c);
    // (1) a creator with room for ONE client, which has it
    OpenCreator(ga, 1, 0);
    TCHECK(FindGroup(gc, a.mac()));
    TCHECK_EQ(gc.FkStopScan().res(), 0u);  // the list is frozen with the group in it: the stale list of a game that is slow to choose
    TCHECK(FindGroup(gb, a.mac()));
    JoinResult j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
    g_rules.Reset();  // (C sends an EAPoL-Start to a host that is full - what a stale list does - and the checker, written for Azahar hosts, counts it)
    j = gc.FkJoinWait(a.mac(), 9000);
    TCHECK(j.pending);
    TCHECK(j.done);
    TCHECK_EQ(j.rep.hdr(), 0x000B0040u);
    TCHECK_EQ(j.rep.res(), R_TIMEOUT);  // the lobby's own time-out
    TCHECK(j.ms >= 2900);               // (kJoinMs: 3000)
    TCHECK(j.ms < 4600);
    TCHECK_EQ(gc.StatusState(), 1u);
    TCHECK(EngFree(c));
    TCHECK_EQ(gc.FkTitleInfo(a.mac()).res(), 0u);  // the list is as it was: the same MAC can be tried again
    TCHECK_EQ(a.hub.cancelled() + c.hub.cancelled() + c.hub.failed_fast(), 0u);
    // the seat is free again: B leaves, the same stale entry now gets in
    TCHECK_EQ(gb.FkStopSession().res(), 0u);
    TCHECK(WaitFor(5000, [&] { return ga.ClientsTotal() == 0; }));
    j = gc.FkJoinWait(a.mac(), 9000);
    TCHECK(j.done);
    TCHECK_EQ(j.rep.res(), 0u);
    TCHECK_EQ(gc.StatusState(), 5u);
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 1; }));
    // (2) a creator that stopped taking players: its group is no longer offered, and a stale entry gets no answer
    TCHECK(FindGroup(gb, a.mac()));
    TCHECK_EQ(gb.FkStopScan().res(), 0u);
    TCHECK_EQ(ga.SvEnd().res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    j = gb.FkJoinWait(a.mac(), 9000);
    TCHECK(j.pending && j.done);
    TCHECK_EQ(j.rep.res(), R_TIMEOUT);
    TCHECK(j.ms >= 2900);
    TCHECK(j.ms < 4600);
    TCHECK(EngFree(b));
    // (3) a join cancelled by the end of the session: the wait ends at once with the answer of an empty join
    TCHECK_EQ(gb.FkTitleInfo(a.mac()).res(), 0u);  // (the table kept the group from the failed join)
    const Rep pend = gb.FkJoin(a.mac());
    TCHECK(pend.out == DlpOutcome::JoinPending);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const u64 t0 = platform::NowMs();
    b.hub.OnSessionClosed(kFk);
    Rep r;
    TCHECK(gb.Pending(&r));
    TCHECK(Ms(t0) < 800);
    TCHECK_EQ(r.hdr(), 0x000B0040u);
    TCHECK_EQ(r.res(), R_INVALID_STATE);
    TCHECK(EngFree(b));
    TCHECK_EQ(gb.StatusState(), 1u);
    // (4) a new search does not list the closed lobby
    TCHECK_EQ(gb.FkScan().res(), 0u);
    TCHECK(!WaitFor(1500, [&] {
        const Rep q = gb.FkInOrder(1);
        return q.res() == 0 && std::memcmp(gb.TitleRec() + kTitleInfoMac, a.mac().data(), 6) == 0;
    }));
    g_watchdog->Disarm();
}

// =====================================================================================================================
// H9: a slow network thread, and one stalled mid-sequence.

static void H9() {
    std::printf("H9: a slow network thread (40 ms a round) and one stalled for 700 ms mid-sequence\n");
    g_watchdog->Arm(200, "H9");
    {
        Fix f({"Alice", "Bob"}, 40);
        TdShared s;
        const u64 t0 = platform::NowMs();
        RunTd(f, s);
        std::printf("  slow rounds: the whole sequence took %u ms\n", Ms(t0));
        CheckTdEngineCalls(f.n(0), f.n(1));
        CheckHubBooks(f.n(0));
        CheckHubBooks(f.n(1));
        TCHECK(EngFree(f.n(0)));
        TCHECK(EngFree(f.n(1)));
        CHECK_RULES();
    }
    {
        Fix f({"Alice", "Bob"});
        TdShared s;
        s.before_join = [&] { f.net(1).Stall(700); };
        s.before_distribute = [&] { f.net(0).Stall(700); };
        const u64 t0 = platform::NowMs();
        RunTd(f, s);
        std::printf("  two stalls of 700 ms: the whole sequence took %u ms (the join %u ms)\n", Ms(t0), s.join_ms.load());
        TCHECK(s.join_ms.load() >= 600u);  // (the join waited for the stalled thread)
        CheckTdEngineCalls(f.n(0), f.n(1));
        CheckHubBooks(f.n(0));
        CheckHubBooks(f.n(1));
        TCHECK(EngFree(f.n(0)));
        TCHECK(EngFree(f.n(1)));
        CHECK_RULES();
    }
    g_watchdog->Disarm();
}

// =====================================================================================================================

// =====================================================================================================================
// X scenarios: what the mutation round of phase 3 found missing in H1..H9 (analysis/dlp-lobby-fake-2026-10-28, label "mutants").
//   X1  the network thread's "alive" window with LITERAL times: a 750 ms hiccup is waited for (the game asks 450 ms into it), a silence of 1.3 s fails at once
//   X2  a posted command that RUNS for longer than kQueuedMs (2.5 s) is not cancelled: the requester waits for the answer that is coming
//   X3  Stop() wakes a waiting requester at once (not at the end of its 100 ms slice); an answer given by Stop is not counted as served
//   X4  every posted request is answered by its signal: no wait ends by its slice, one signal per request
//   X5  a close left for the next Tick (no network thread) ends the role ONCE; a role begun afterwards is not ended by the old close
//   X6  a kernel that has no event for the hub (CreateEvent answers 0): posted commands fail at once, inline ones answer
//   X7  an event that a wait does not consume: the hub resets it before it queues (no spinning on a stale signal)
//   X8  the slot's protocol on a kernel whose events synchronise nothing (relaxed atomics): the ThreadSanitizer build judges the memory orders
//   X9  a game thread inside a table read holds dlp_mutex_: the network thread's Ticks miss the lock and never wait for it; a close left for a later Tick runs
//       BEFORE the request of the new session that is queued behind it
//   X10 a close left for the next Tick ends the role of ITS OWN service only (searcher's close leaves the creator up, creator's leaves the searcher)
//   X11 the run-here path (no network thread) takes dlp_mutex_: inline reads on another game thread race with nothing (ThreadSanitizer)
//   X12 a requester that arrives behind a waiting one, the network thread silent over 1 s, is refused at once
// (H4 of the repo's test is also updated here: with no network thread the end of a session is DEFERRED to the next Tick, not run on the calling thread.)

static void X1() {
    std::printf("X1: the network thread's 'alive' window: a hiccup shorter than 1 s is waited for, a silence over 1 s fails at once\n");
    g_watchdog->Arm(60, "X1");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // (1) a hiccup of 750 ms; the game asks 450 ms into it
    f.net(0).Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    Rep r;
    u32 took = 0;
    u64 t0 = platform::NowMs();
    std::thread th([&] {
        t_game = true;
        r = g.SvAccepting(0, 6);
        took = Ms(t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    f.net(0).Pause(false);
    th.join();
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);  // (served once the thread came back: not "wireless off")
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK(took >= 250u && took < 1500u);
    TCHECK_EQ(g.ServerState(), 2u);
    // (2) a silence of 1.3 s (the literal: kAliveMs is 1000): the next posted command is refused at once
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.net(0).Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    t0 = platform::NowMs();
    r = g.SvDistribute();  // (not SvEnd: EndAccepting succeeds in every state, F1)
    took = Ms(t0);
    TCHECK_EQ(r.hdr(), Plain(7));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    TCHECK(took < 300u);
    TCHECK_EQ(a.hub.failed_fast(), 1u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    f.net(0).Pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // (3) and it is served again as soon as the thread ticks
    TCHECK_EQ(g.SvEnd().res(), 0u);
    g_watchdog->Disarm();
}

static void X2() {
    std::printf("X2: a posted command that runs for longer than kQueuedMs is waited for, not cancelled\n");
    g_watchdog->Arm(60, "X2");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    g_exec_slow_engine.store(&a.e());
    g_exec_slow_ms.store(2500);
    const u64 t0 = platform::NowMs();
    const Rep r = g.SvAccepting(0, 6);
    const u32 took = Ms(t0);
    g_exec_slow_ms.store(0);
    g_exec_slow_engine.store(nullptr);
    TCHECK(took >= 2400u);
    TCHECK_EQ(r.hdr(), 0x00050040u);
    TCHECK_EQ(r.res(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.served_posted(), 2u);
    TCHECK_EQ(g.ServerState(), 2u);
    TCHECK(a.hub.EngineHeld());
    // the slot is free and consistent afterwards (the network thread's stamp is that of the START of the long Tick: let it tick once more)
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    TCHECK_EQ(g.SvEnd().res(), 0u);
    TCHECK_EQ(a.hub.served_posted(), 3u);
    g_watchdog->Disarm();
}

static void X3() {
    std::printf("X3: Stop() wakes a waiting requester at once; an answer given by Stop is not a served request\n");
    g_watchdog->Arm(60, "X3");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.net(0).Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u32 posted0 = a.hub.served_posted();
    const u32 w0 = a.kernel.waits();
    Rep r;
    std::atomic<u64> t_done{0};
    std::thread th([&] {
        t_game = true;
        r = g.SvAccepting(0, 6);
        t_done.store(platform::NowMs());
    });
    // the requester's first wait, then the start of its second slice (100 ms later); Stop() 5 ms into that slice: a missing wake-up shows as ~95 ms
    TCHECK(WaitFor(2000, [&] { return a.kernel.waits() > w0; }));
    const u32 w1 = a.kernel.waits();
    const u64 give_up = platform::NowMs() + 1000;
    while (a.kernel.waits() == w1 && platform::NowMs() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TCHECK(a.kernel.waits() > w1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const u64 t_stop = platform::NowMs();
    a.hub.Stop();
    th.join();
    TCHECK(t_done.load() >= t_stop);
    TCHECK(t_done.load() - t_stop < 60u);
    TCHECK_EQ(r.hdr(), Plain(5));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    TCHECK_EQ(a.hub.served_posted(), posted0);  // (it never ran: not counted)
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    f.net(0).Pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TCHECK_EQ(g.ServerState(), 1u);  // (and it never runs later)
    g_watchdog->Disarm();
}

static void X4() {
    std::printf("X4: every posted request is answered by its signal: no wait ends by its slice, one signal per request\n");
    g_watchdog->Arm(60, "X4");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.FkInit().res(), 0u);
    for (int i = 0; i < 12; ++i) {
        TCHECK_EQ(g.FkScan().res(), 0u);
        TCHECK_EQ(g.FkStopScan().res(), 0u);
    }
    TCHECK_EQ(a.hub.served_posted(), 25u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.kernel.timeouts(), 0u);
    TCHECK_EQ(a.kernel.signals(), 25u);
    TCHECK(a.kernel.waits() >= 25u && a.kernel.waits() < 25u + 25u);
    g_watchdog->Disarm();
}

static void X5() {
    std::printf("X5: a close left for the next Tick ends the role once; a role begun afterwards is not ended by the old close\n");
    g_watchdog->Arm(60, "X5");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a);
    // the network thread (re)started: the hub treats a thread it has not seen tick as no thread
    auto restart_net = [&] {
        f.nets.push_back(std::make_unique<NetThread>(a, 3));
        NetThread& nt = *f.nets.back();
        TCHECK(WaitFor(2000, [&] { return nt.rounds() >= 2; }));
    };
    // (1) the searcher's role: a scan in progress, the game closes its session, the network thread is gone
    TCHECK_EQ(g.FkInitName("Bob", 4).res(), 0u);
    TCHECK_EQ(g.FkScan().res(), 0u);
    const u32 st_scan = g.StatusState();
    TCHECK(st_scan != 0u);
    f.net(0).Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
    a.hub.OnSessionClosed(kFk);  // (no network thread: left for the next Tick)
    TCHECK_EQ(a.hub.deferred_closes(), 1u);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK_EQ(g.StatusState(), st_scan);  // (nothing of it ran on this thread)
    restart_net();  // (the first Tick ends the role)
    TCHECK(WaitFor(2000, [&] { return g.StatusState() != st_scan; }));
    // a NEW client role: the old close must not touch it, however many rounds go by
    TCHECK_EQ(g.FkInitName("Bob", 4).res(), 0u);
    TCHECK_EQ(g.FkScan().res(), 0u);
    TCHECK_EQ(g.StatusState(), st_scan);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    TCHECK_EQ(g.StatusState(), st_scan);  // (still scanning)
    TCHECK_EQ(a.hub.deferred_closes(), 1u);
    TCHECK_EQ(g.FkFinalize().res(), 0u);
    TCHECK(EngFree(a));
    // (2) the creator's role: the engine is held, the game closes its session, the network thread is gone
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
    TCHECK(a.hub.EngineHeld());
    f.nets.back()->Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
    a.hub.OnSessionClosed(kSv);
    TCHECK_EQ(a.hub.deferred_closes(), 2u);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK(a.hub.EngineHeld());
    TCHECK_EQ(g.ServerState(), 2u);
    restart_net();
    TCHECK(WaitFor(2000, [&] { return !a.hub.EngineHeld(); }));
    TCHECK(EngFree(a));
    TCHECK_EQ(g.ServerState(), 0u);  // (the creator's service is over: Finalize by the close)
    // a NEW creator role is not ended by the old close
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    TCHECK(a.hub.EngineHeld());
    TCHECK_EQ(g.ServerState(), 2u);
    TCHECK_EQ(a.hub.deferred_closes(), 2u);
    g_watchdog->Disarm();
}

static void X6() {
    std::printf("X6: no event for the hub: posted commands fail at once (even with a live network thread), inline ones answer\n");
    g_watchdog->Arm(60, "X6");
    g_no_event.store(true);
    Fix f({"Alice"});
    g_no_event.store(false);
    Node& a = f.n(0);
    Game g(a);
    const u64 t0 = platform::NowMs();
    Rep r = g.SvInit(3);
    TCHECK_EQ(r.hdr(), Plain(1));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    r = g.FkScan();
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
    TCHECK(Ms(t0) < 300u);
    TCHECK_EQ(a.hub.failed_fast(), 2u);
    TCHECK_EQ(a.hub.served_posted(), 0u);
    TCHECK_EQ(a.kernel.waits(), 0u);
    TCHECK_EQ(a.Spy(kFnInitForLobby), 0u);  // (the command never ran)
    TCHECK_EQ(g.ServerState(), 0u);
    TCHECK_EQ(g.SvDup().res(), 0u);
    TCHECK(EngFree(a));
    g_watchdog->Disarm();
}

static void X7() {
    std::printf("X7: an event that a wait does not consume: the hub resets it before it queues (no spinning on a stale signal)\n");
    g_watchdog->Arm(60, "X7");
    g_sticky_events.store(true);
    Fix f({"Alice"});
    g_sticky_events.store(false);
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.FkInit().res(), 0u);
    for (int i = 0; i < 6; ++i) {
        TCHECK_EQ(g.FkScan().res(), 0u);
        TCHECK_EQ(g.FkStopScan().res(), 0u);
    }
    TCHECK_EQ(a.hub.served_posted(), 13u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK(a.kernel.waits() < 13u * 4u);  // (a stale signal makes every wait return at once: thousands of waits)
    g_watchdog->Disarm();
}

static void X8() {
    std::printf("X8: the slot's protocol stands on its own memory order (a kernel whose events synchronise nothing; the ThreadSanitizer build is the judge)\n");
    g_watchdog->Arm(60, "X8");
    g_relaxed_events.store(true);
    Fix f({"Alice"});
    g_relaxed_events.store(false);
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.FkInit().res(), 0u);
    for (int i = 0; i < 25; ++i) {
        TCHECK_EQ(g.FkScan().res(), 0u);
        TCHECK_EQ(g.FkStopScan().res(), 0u);
    }
    TCHECK_EQ(a.hub.served_posted(), 51u);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    g_watchdog->Disarm();
}

static void X9() {
    std::printf("X9: the network thread never waits for dlp_mutex_ (a game thread inside a table read); a close left for a later Tick runs BEFORE the request that follows it\n");
    g_watchdog->Arm(90, "X9");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a), gh(a), gp(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
    TCHECK(a.hub.EngineHeld());
    f.net(0).Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
    a.hub.OnSessionClosed(kSv);  // (no network thread: left for a Tick)
    TCHECK_EQ(a.hub.deferred_closes(), 1u);
    TCHECK(a.hub.EngineHeld());
    // a game thread is inside a table read: it holds dlp_mutex_ and its buffer's resolver waits
    g_map_entered.store(false);
    g_map_hold.store(true);
    Rep rh;
    std::thread th([&] {
        t_game = true;
        t_map_gate = true;
        rh = gh.SvClients(4, 8);
    });
    TCHECK(WaitFor(3000, [&] { return g_map_entered.load(); }));
    const u32 missed0 = a.hub.missed_ticks();
    // the network thread comes back: its Ticks stamp (the hub is "alive" again) but cannot take the lock - and must not wait for it
    f.nets.push_back(std::make_unique<NetThread>(a, 3));
    NetThread& nt = *f.nets.back();
    TCHECK(WaitFor(2000, [&] { return nt.rounds() >= 5; }));
    TCHECK(a.hub.missed_ticks() > missed0);
    TCHECK(a.hub.EngineHeld());  // (the deferred close has not run: it needs the lock)
    // the game opens a NEW creator session meanwhile: the request is queued behind the pending close
    Rep rp;
    std::thread tp([&] {
        t_game = true;
        rp = gp.SvInit(3);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    g_map_hold.store(false);
    th.join();
    tp.join();
    TCHECK_EQ(rp.hdr(), 0x00010040u);
    TCHECK_EQ(rp.res(), 0u);  // (the old role was ended FIRST: the new Initialize found a service that was over)
    TCHECK_EQ(g.ServerState(), 1u);
    TCHECK(EngFree(a));
    TCHECK_EQ(a.hub.deferred_closes(), 1u);
    g_watchdog->Disarm();
}

static void X10() {
    std::printf("X10: a close left for the next Tick ends the ROLE OF ITS OWN SERVICE only\n");
    g_watchdog->Arm(90, "X10");
    // (a) the searcher's session closes, the creator's role goes on
    {
        Fix f({"Alice"});
        Node& a = f.n(0);
        Game g(a);
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
        TCHECK_EQ(g.FkInitName("Bob", 4).res(), 0u);
        TCHECK(a.hub.EngineHeld());
        f.net(0).Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
        a.hub.OnSessionClosed(kFk);
        TCHECK_EQ(a.hub.deferred_closes(), 1u);
        f.nets.push_back(std::make_unique<NetThread>(a, 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // (many rounds: the Tick that ends the searcher service has run; its Finalize leaves state 1, as Azahar)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TCHECK(a.hub.EngineHeld());  // (the creator's lobby is still up)
        TCHECK_EQ(g.ServerState(), 2u);
    }
    // (b) the creator's session closes, the searcher's service goes on
    {
        Fix f({"Alice"});
        Node& a = f.n(0);
        Game g(a);
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        TCHECK_EQ(g.FkInitName("Bob", 4).res(), 0u);
        TCHECK_EQ(g.FkScan().res(), 0u);
        const u32 fk_state = g.StatusState();
        TCHECK(fk_state != 0u);
        f.net(0).Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
        a.hub.OnSessionClosed(kSv);
        TCHECK_EQ(a.hub.deferred_closes(), 1u);
        f.nets.push_back(std::make_unique<NetThread>(a, 3));
        TCHECK(WaitFor(2000, [&] { return g.ServerState() == 0u; }));  // (the creator's service is over)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TCHECK_EQ(g.StatusState(), fk_state);  // (the searcher is still scanning)
    }
    g_watchdog->Disarm();
}

static void X11() {
    std::printf("X11: the run-here path (no network thread) takes dlp_mutex_: it does not race with inline reads on other game threads\n");
    g_watchdog->Arm(60, "X11");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g(a), gr(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    TCHECK_EQ(g.SvAccepting(0, 6).res(), 0u);
    TCHECK(a.hub.EngineHeld());
    f.net(0).Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 150));
    // a second game thread reads the creator's tables (inline) in a loop while this one runs OnGameUdsStarting on the calling thread
    std::atomic<bool> stop{false};
    std::atomic<u32> reads{0};
    std::thread th([&] {
        t_game = true;
        while (!stop.load()) {
            gr.ServerState();
            gr.ClientsTotal();
            reads.fetch_add(1);
        }
    });
    TCHECK(WaitFor(3000, [&] { return reads.load() >= 200; }));
    {
        Exempt ex;
        a.hub.OnGameUdsStarting();
    }
    const u32 r1 = reads.load();
    TCHECK(WaitFor(3000, [&] { return reads.load() >= r1 + 200; }));
    stop.store(true);
    th.join();
    TCHECK_EQ(a.hub.ran_here(), 1u);
    TCHECK(EngFree(a));
    TCHECK_EQ(g.ServerState(), 1u);
    g_watchdog->Disarm();
}

static void X12() {
    std::printf("X12: a requester that arrives while another waits, with the network thread silent for over a second, is refused at once\n");
    g_watchdog->Arm(60, "X12");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game g1(a), g2(a);
    TCHECK_EQ(g1.SvInit(3).res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.net(0).Pause(true);
    const u64 t0 = platform::NowMs();
    Rep r1, r2;
    u32 took1 = 0, took2 = 0;
    std::thread th1([&] {  // (posted while the thread is still "alive": it waits for its deadline, kQueuedMs)
        t_game = true;
        r1 = g1.SvAccepting(0, 6);
        took1 = Ms(t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));  // (the literal: kAliveMs is 1000)
    const u64 t1 = platform::NowMs();
    std::thread th2([&] {
        t_game = true;
        r2 = g2.FkInit();
        took2 = Ms(t1);
    });
    th2.join();
    th1.join();
    TCHECK_EQ(r2.hdr(), Plain(1));
    TCHECK_EQ(r2.res(), R_WIRELESS_OFF);
    TCHECK(took2 < 300u);  // (refused at once, not when the first one gives up at 2 s)
    TCHECK_EQ(r1.res(), R_WIRELESS_OFF);
    TCHECK(took1 >= DlpHub::kQueuedMs - 100 && took1 < DlpHub::kQueuedMs + 900);
    TCHECK_EQ(a.hub.failed_fast(), 1u);
    TCHECK_EQ(a.hub.cancelled(), 1u);
    f.net(0).Pause(false);
    g_watchdog->Disarm();
}

// =====================================================================================================================
// F scenarios: the regressions of the fixes made after the review of phase 3 (the third round of the hub test).
//   F1a..F1d  the commands that succeed in every state - FKCL Finalize 0x02 / StopScan 0x06 / StopSession 0x10, SRVR Finalize 0x02 / EndAccepting 0x06 -
//             when the request cannot be served: a) the network thread silent for longer than kAliveMs after it had ticked, b) alive but stalled so
//             that the request is cancelled at kQueuedMs (a game thread parked in a table read holds dlp_mutex_: every Tick stamps and misses the lock),
//             c) never ticked, d) after Stop(). They answer [(id << 16) | 0x40, 0] (Triple Deluxe and Planet Robobot treat a negative Finalize as FATAL);
//             every other command answers [(id << 16) | 0x40, ResultWirelessOff]; the roles that Finalize and StopSession end are ended by the next Tick
//             (deferred_closes() counts them); the ones whose role is not ended (StopScan, EndAccepting) change nothing; and a new session works
//             (not after Stop(), which is final).
//   F2        Stop() lands between Post's second check of the network thread and its store of Queued (Stop() runs inside the kernel's ClearEvent): the
//             request fails at once, counted by failed_fast() and not by cancelled()
//   F3        OnGameUdsStarting while a posted StartAccepting is in flight (the network thread is inside the engine for 600 ms): it waits behind the request
//             and gives the engine back, and the game's own Initialize that follows finds a clean engine
//   F4        a Tick that lasts longer than kAliveMs does not make the next requester find the network thread "dead" (the stamp at the END of the Tick)
//   F5        SetPlayerName: the name of a console whose Initialize sends none, in the creator's list, in the roster and in the GetClientInfo / GetNodeInfo
//             records; InitializeWithName still overrides it

static void ExpectOk(const Rep& r, u32 id) {  // [(id << 16) | 0x40, 0]
    TCHECK(r.out == DlpOutcome::Done);
    TCHECK_EQ(r.hdr(), Plain(id));
    TCHECK_EQ(r.res(), 0u);
}
static void ExpectOff(const Rep& r, u32 id) {  // [(id << 16) | 0x40, ResultWirelessOff]
    TCHECK(r.out == DlpOutcome::Done);
    TCHECK_EQ(r.hdr(), Plain(id));
    TCHECK_EQ(r.res(), R_WIRELESS_OFF);
}

enum class Mute { Silent, Stalled, NeverTicked, Stopped };

/// A game thread parked inside a table read of a console (an inline command whose buffer resolver waits): it holds the hub's dlp_mutex_, so every
/// Tick of the network thread stamps its liveness, misses the lock and serves NOTHING - a network thread that is alive and stalled.
class Gate {
public:
    explicit Gate(Node& n) : n_(n), g_(n) {
        n.mem.gate_entered.store(false);
        n.mem.gate_hold.store(true);
        th_ = std::thread([this] {
            t_game = true;
            t_gate_mem = &n_.mem;
            g_.FkInOrder(1);  // (GetTitleInfoInOrder maps its buffer first, with dlp_mutex_ held: the resolver parks here)
        });
        TCHECK(WaitFor(3000, [&] { return n_.mem.gate_entered.load(); }));
    }
    ~Gate() { Release(); }
    void Release() {
        if (th_.joinable()) {
            n_.mem.gate_hold.store(false);
            th_.join();
        }
    }

private:
    Node& n_;
    Game g_;
    std::thread th_;
};

/// The searcher's side of F1 on console `b`: `scanning` = its client role is up and scanning (state 2), else the role is off (state 0). Every
/// command goes to the hub while nothing can serve it.
static void F1Searcher(Node& b, Game& gb, Mute mode, bool scanning) {
    const bool slow = mode == Mute::Stalled;  // (each request waits for kQueuedMs: fewer of them)
    const bool reads = !slow;                 // (a table read would wait for the gate thread)
    const u32 posted0 = b.hub.served_posted(), ff0 = b.hub.failed_fast(), cc0 = b.hub.cancelled(), dc0 = b.hub.deferred_closes();
    u32 attempts = 0;
    auto call = [&](u32 id, bool ok, const std::function<Rep()>& fn) {
        const u64 t0 = platform::NowMs();
        const Rep r = fn();
        const u32 took = Ms(t0);
        ++attempts;
        if (ok) {
            ExpectOk(r, id);
        } else {
            ExpectOff(r, id);
        }
        if (slow) {
            TCHECK(took >= DlpHub::kQueuedMs - 100);
            TCHECK(took < DlpHub::kQueuedMs + 900);
        } else {
            TCHECK(took < 400);
        }
        if (reads) {
            TCHECK_EQ(gb.StatusState(), scanning ? 2u : 0u);  // (nothing of it ran: a scan is still a scan, StopScan or not)
        }
    };
    call(0x06, true, [&] { return gb.FkStopScan(); });  // StopScan: success, and the scan goes on
    call(0x05, false, [&] { return gb.FkScan(); });
    call(0x0C, false, [&] { return gb.SvClientInfo(2); });  // (SRVR GetClientInfo, asked of the hub)
    if (!slow) {
        call(0x0B, false, [&] { return gb.FkJoin(b.mac()); });  // (Done at once, not JoinPending)
        call(0x11, false, [&] { return gb.FkInitName("Bob", 4); });
    }
    call(0x10, true, [&] { return gb.FkStopSession(); });  // ends the role: the next Tick does it
    call(0x02, true, [&] { return gb.FkFinalize(); });
    TCHECK_EQ(b.hub.served_posted(), posted0);  // (nothing was served)
    TCHECK_EQ(b.hub.deferred_closes(), dc0 + 2);
    TCHECK_EQ(b.hub.ran_here(), 0u);
    TCHECK_EQ(b.hub.cancelled(), cc0 + (slow ? attempts : 0u));
    TCHECK_EQ(b.hub.failed_fast(), ff0 + (slow ? 0u : attempts));
}

/// The creator's side of F1 on console `a`: `open` = the lobby is accepting (state 2, the engine held), else the role is off (state 0).
static void F1Creator(Node& a, Game& ga, Mute mode, bool open) {
    const bool slow = mode == Mute::Stalled;
    const bool reads = !slow;
    const u32 posted0 = a.hub.served_posted(), ff0 = a.hub.failed_fast(), cc0 = a.hub.cancelled(), dc0 = a.hub.deferred_closes();
    u32 attempts = 0;
    auto call = [&](u32 id, bool ok, const std::function<Rep()>& fn) {
        const u64 t0 = platform::NowMs();
        const Rep r = fn();
        const u32 took = Ms(t0);
        ++attempts;
        if (ok) {
            ExpectOk(r, id);
        } else {
            ExpectOff(r, id);
        }
        if (slow) {
            TCHECK(took >= DlpHub::kQueuedMs - 100);
            TCHECK(took < DlpHub::kQueuedMs + 900);
        } else {
            TCHECK(took < 400);
        }
        TCHECK_EQ(a.hub.EngineHeld(), open);  // (lock-free: allowed while the gate thread holds the lock)
        if (reads) {
            TCHECK_EQ(ga.ServerState(), open ? 2u : 0u);
        }
    };
    call(0x06, true, [&] { return ga.SvEnd(); });  // EndAccepting: success, and the door stays as it was
    call(0x05, false, [&] { return ga.SvAccepting(0, 6); });
    call(0x0C, false, [&] { return ga.SvClientInfo(2); });
    if (!slow) {
        call(0x07, false, [&] { return ga.SvDistribute(); });
        call(0x01, false, [&] { return ga.SvInit(3); });
        call(0x08, false, [&] { return ga.SvPass(kNine); });
    }
    call(0x02, true, [&] { return ga.SvFinalize(); });  // ends the role: the next Tick does it
    TCHECK_EQ(a.hub.served_posted(), posted0);
    TCHECK_EQ(a.hub.deferred_closes(), dc0 + 1);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK_EQ(a.hub.cancelled(), cc0 + (slow ? attempts : 0u));
    TCHECK_EQ(a.hub.failed_fast(), ff0 + (slow ? 0u : attempts));
}

static void F1(Mute mode) {
    const bool never = mode == Mute::NeverTicked;
    Fix f({"Alice", "Bob"}, 3, !never);  // 0: the creator, 1: the searcher
    Node& a = f.n(0);
    Node& b = f.n(1);
    Game ga(a), gb(b);
    if (!never) {
        TCHECK_EQ(ga.SvInit(3).res(), 0u);
        TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
        TCHECK_EQ(gb.FkInitName("Bob", 4).res(), 0u);
        TCHECK_EQ(gb.FkScan().res(), 0u);
        TCHECK_EQ(ga.ServerState(), 2u);
        TCHECK_EQ(gb.StatusState(), 2u);
        TCHECK(a.hub.EngineHeld());
        TCHECK(!b.hub.EngineHeld());
    }
    const u32 posted_a = a.hub.served_posted(), posted_b = b.hub.served_posted();
    // 1. nothing can serve
    std::unique_ptr<Gate> gate_a, gate_b;
    switch (mode) {
    case Mute::Silent: {
        f.net(0).Pause(true);
        f.net(1).Pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // (a round in flight ends)
        const u64 ra = f.net(0).rounds(), rb = f.net(1).rounds();
        std::this_thread::sleep_for(std::chrono::milliseconds(DlpHub::kAliveMs + 250));
        TCHECK_EQ(f.net(0).rounds(), ra);  // (silent for longer than kAliveMs after it had ticked)
        TCHECK_EQ(f.net(1).rounds(), rb);
        break;
    }
    case Mute::Stalled: {
        const u32 ma = a.hub.missed_ticks(), mb = b.hub.missed_ticks();
        const u64 ra = f.net(0).rounds();
        gate_a.reset(new Gate(a));
        gate_b.reset(new Gate(b));
        TCHECK(WaitFor(2000, [&] { return a.hub.missed_ticks() > ma + 3 && b.hub.missed_ticks() > mb + 3; }));  // (alive: it ticks, and misses the lock)
        TCHECK(f.net(0).rounds() > ra);
        break;
    }
    case Mute::NeverTicked:
        TCHECK(f.nets.empty());  // (no network thread was ever started: the hub has never been ticked)
        break;
    case Mute::Stopped:
        f.net(0).Stop();
        f.net(1).Stop();
        a.hub.Stop();
        b.hub.Stop();
        break;
    }
    // 2. both roles, at the same time
    std::thread ts([&] {
        t_game = true;
        F1Searcher(b, gb, mode, !never);
    });
    std::thread tc([&] {
        t_game = true;
        F1Creator(a, ga, mode, !never);
    });
    ts.join();
    tc.join();
    // 3. the network thread comes back (or a Tick is called)
    switch (mode) {
    case Mute::Silent:
        f.net(0).Pause(false);
        f.net(1).Pause(false);
        break;
    case Mute::Stalled:
        gate_a->Release();
        gate_b->Release();
        break;
    case Mute::NeverTicked:
        f.StartNet();
        break;
    case Mute::Stopped: {
        AsNet an;  // (the test plays the network thread for one round each)
        a.hub.Tick();
        b.hub.Tick();
        break;
    }
    }
    // the roles that Finalize and StopSession end ARE ended: the creator is over (0), the searcher is back to idle (1; 0 where it never was up)
    TCHECK(WaitFor(4000, [&] { return gb.StatusState() == (never ? 0u : 1u) && ga.ServerState() == 0 && !a.hub.EngineHeld(); }));
    TCHECK(EngFree(a));
    TCHECK(EngFree(b));
    TCHECK_EQ(a.hub.deferred_closes(), 1u);
    TCHECK_EQ(b.hub.deferred_closes(), 2u);
    TCHECK_EQ(a.hub.served_posted(), posted_a);  // (ended by the Tick, not served as requests)
    TCHECK_EQ(b.hub.served_posted(), posted_b);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK_EQ(b.hub.ran_here(), 0u);
    if (mode != Mute::Stopped) {
        // 4. a new session works, and the old closes do not touch it (they were consumed by the Tick that ended the roles)
        ExpectOk(ga.SvInit(3), 1);
        ExpectOk(ga.SvAccepting(0, 6), 5);
        ExpectOk(gb.FkInitName("Bob", 4), 0x11);
        ExpectOk(gb.FkScan(), 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        TCHECK_EQ(ga.ServerState(), 2u);
        TCHECK_EQ(gb.StatusState(), 2u);
        TCHECK(a.hub.EngineHeld());
        ExpectOk(gb.FkStopScan(), 6);
        ExpectOk(gb.FkFinalize(), 2);
        ExpectOk(ga.SvFinalize(), 2);
        TCHECK_EQ(a.hub.served_posted(), posted_a + 3);
        TCHECK_EQ(b.hub.served_posted(), posted_b + 4);
        TCHECK_EQ(a.hub.deferred_closes(), 1u);
        TCHECK_EQ(b.hub.deferred_closes(), 2u);
        TCHECK(EngFree(a));
        TCHECK(EngFree(b));
    } else {
        // Stop() is final: nothing is served again, every posted command still answers at once
        const u32 ffa = a.hub.failed_fast(), ffb = b.hub.failed_fast();
        ExpectOff(ga.SvInit(3), 1);
        ExpectOff(gb.FkInitName("Bob", 4), 0x11);
        ExpectOk(ga.SvFinalize(), 2);
        TCHECK_EQ(a.hub.failed_fast(), ffa + 2);
        TCHECK_EQ(b.hub.failed_fast(), ffb + 1);
        TCHECK_EQ(a.hub.deferred_closes(), 2u);
    }
}

static void F1a() {
    std::printf("F1a: the network thread silent for over kAliveMs after it had ticked: Finalize / StopScan / StopSession / EndAccepting succeed, the rest is refused\n");
    F1(Mute::Silent);
}
static void F1b() {
    std::printf("F1b: a network thread that is alive but stalled (a request is cancelled at kQueuedMs): the same replies\n");
    F1(Mute::Stalled);
}
static void F1c() {
    std::printf("F1c: a hub that was never ticked: the same replies\n");
    F1(Mute::NeverTicked);
}
static void F1d() {
    std::printf("F1d: after Stop(): the same replies, the roles ended by a later Tick\n");
    F1(Mute::Stopped);
}

static void F2() {
    std::printf("F2: Stop() lands between Post's checks and its store of Queued: the request fails at once (failed_fast, not cancelled)\n");
    for (int variant = 0; variant < 2; ++variant) {  // 0: StartAccepting (refused), 1: Finalize (succeeds, the role is closed by a later Tick)
        Fix f({"Alice"});
        Node& a = f.n(0);
        Game g(a);
        TCHECK_EQ(g.SvInit(3).res(), 0u);
        NetThread* net = &f.net(0);
        DlpHub* hub = &a.hub;
        a.kernel.SetOnClear([hub, net] {
            hub->Stop();       // Plugin::Shutdown: the hub first ...
            net->Pause(true);  // ... then the network thread
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        });
        const u32 posted0 = a.hub.served_posted();
        const u64 t0 = platform::NowMs();
        const Rep r = variant == 0 ? g.SvAccepting(0, 6) : g.SvFinalize();
        const u32 took = Ms(t0);
        a.kernel.SetOnClear(nullptr);
        if (variant == 0) {
            ExpectOff(r, 5);
        } else {
            ExpectOk(r, 2);
        }
        std::printf("  F2 variant %d: the command came back after %u ms (failed_fast %u, cancelled %u)\n", variant, took, a.hub.failed_fast(), a.hub.cancelled());
        TCHECK(took < 500);  // (without the second look at stopped_ it waits for kQueuedMs: 2000)
        TCHECK_EQ(a.hub.failed_fast(), 1u);
        TCHECK_EQ(a.hub.cancelled(), 0u);
        TCHECK_EQ(a.hub.served_posted(), posted0);
        TCHECK_EQ(a.hub.deferred_closes(), variant == 0 ? 0u : 1u);
        net->Pause(false);
        if (variant == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            TCHECK_EQ(g.ServerState(), 1u);  // (it never ran, not even later)
            TCHECK_EQ(a.Spy(kFnBeginHostingLobby), 0u);
            TCHECK(EngFree(a));
        } else {
            TCHECK(WaitFor(2000, [&] { return g.ServerState() == 0u; }));  // (the close was remembered: the next Tick ends the role)
            TCHECK(EngFree(a));
        }
    }
}

static void F3() {
    std::printf("F3: OnGameUdsStarting while a posted StartAccepting is in flight waits behind it and gives the engine back\n");
    Fix f({"Alice"});
    Node& a = f.n(0);
    Game ga(a);
    TCHECK_EQ(ga.SvInit(3).res(), 0u);
    const u32 posted0 = a.hub.served_posted();
    // nothing held and nothing in flight: it returns at once, posts nothing, and leaves the slot free (the request below needs it)
    const u64 tq = platform::NowMs();
    a.hub.OnGameUdsStarting();
    TCHECK(Ms(tq) < 100u);
    TCHECK_EQ(a.hub.served_posted(), posted0);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    g_exec_slow_engine.store(&a.e());
    g_exec_slow_ms.store(600);
    Rep rs;
    std::thread ts([&] {
        t_game = true;
        rs = ga.SvAccepting(0, 6);
    });
    // the network thread is inside the engine's BeginHostingLobby (the lobby's InitializeForLobby has run): the request is RUNNING, and the hub's
    // hints (EngineHeld, "active") are refreshed only after it ran, so the lobby "holds nothing" yet
    TCHECK(WaitFor(3000, [&] { return a.Spy(kFnBeginHostingLobby) >= 1; }));
    TCHECK(!a.hub.EngineHeld());
    const u64 t0 = platform::NowMs();
    a.hub.OnGameUdsStarting();  // the first thing the HLE does at the game's nwm::UDS Initialize
    const u32 waited = Ms(t0);
    TCHECK(waited >= 300u);  // (it waited behind the request that holds the slot: the early return the fix replaced came back at once)
    TCHECK(waited < 3000u);
    TCHECK(!a.hub.EngineHeld());
    TCHECK(EngFree(a));  // (the engine is the game's again, whatever the lobby had begun)
    ts.join();
    g_exec_slow_ms.store(0);
    g_exec_slow_engine.store(nullptr);
    std::printf("  F3: OnGameUdsStarting came back after %u ms\n", waited);
    TCHECK_EQ(rs.hdr(), Plain(5));
    TCHECK_EQ(rs.res(), 0u);                        // (the request ran to its end and was answered)
    TCHECK_EQ(a.hub.served_posted(), posted0 + 2);  // (StartAccepting, and the give-back)
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.ran_here(), 0u);
    TCHECK_EQ(ga.ServerState(), 1u);  // (back to Initialized: the lobby let go)
    CheckGameUds(a);                  // the game's own Initialize: its node, name and seed; BeginHostingNetwork works; no lobby message
}

static void F4() {
    std::printf("F4: a Tick longer than kAliveMs does not make the next requester find the network thread dead\n");
    Fix f({"Alice"}, 3);
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    // a slow network loop (500 ms between two rounds): between the END of the long Tick and the START of the next round there is time for a requester
    f.net(0).SetSleep(500);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_exec_slow_engine.store(&a.e());
    g_exec_slow_ms.store(DlpHub::kAliveMs + 500);  // StartAccepting stalls the engine for 1.5 s INSIDE Execute: that Tick is longer than kAliveMs
    const u64 t0 = platform::NowMs();
    const Rep r = g.SvAccepting(0, 6);
    const u32 took = Ms(t0);
    g_exec_slow_ms.store(0);
    g_exec_slow_engine.store(nullptr);
    ExpectOk(r, 5);
    TCHECK(took >= DlpHub::kAliveMs + 400);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    // the next requester arrives right after (the pump behind the answer is a few microseconds): the stamp of the START of the long Tick is older than
    // kAliveMs; the one taken at its END is what keeps the thread alive for it
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const u64 t1 = platform::NowMs();
    const Rep r2 = g.SvClientInfo(3);  // (nobody there: served, kNoData - not "wireless off")
    const u32 took2 = Ms(t1);
    std::printf("  F4: the long Tick took %u ms; the next request, 60 ms after the answer, came back after %u ms with 0x%08X\n", took, took2, r2.res());
    TCHECK_EQ(r2.hdr(), Plain(0xC));
    TCHECK_EQ(r2.res(), R_NO_DATA);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
    TCHECK_EQ(a.hub.cancelled(), 0u);
    TCHECK_EQ(a.hub.served_posted(), 3u);
    TCHECK(took2 >= 100u);  // (served by the next round of the slow loop: it was queued, not refused)
    TCHECK(took2 < 1500u);
    f.net(0).SetSleep(3);
}

/// F4b (opt-in, runs only by name - `dlp_hub_test F4b` - until DlpHub::Tick takes its liveness stamp BEFORE it publishes the answer): the window F4 leaves.
/// F4 asks the next requester 60 ms after the answer; this one asks 10 ms after it, while the pump behind the answer is still running (it takes 120 ms:
/// the engine's NodeSnapshot sleeps on the network thread). The stamp at the END of the Tick is not there yet, the one at its START is older than kAliveMs,
/// so the request is refused at once with ResultWirelessOff. The smallest fix: Stamp() right before `state_.store(kDone, ...)` in DlpHub::Tick (the
/// requester that is released by the answer then finds the thread as alive as the Tick that answered it).
static void F4b() {
    std::printf("F4b: a Tick longer than kAliveMs, a slow pump behind its answer, the next requester 10 ms after the answer\n");
    Fix f({"Alice"}, 3);
    Node& a = f.n(0);
    Game g(a);
    TCHECK_EQ(g.SvInit(3).res(), 0u);
    g_exec_slow_engine.store(&a.e());
    g_exec_slow_ms.store(DlpHub::kAliveMs + 500);
    g_slow_engine.store(&a.e());
    g_slow_ms.store(120);
    const Rep r = g.SvAccepting(0, 6);
    g_exec_slow_ms.store(0);
    g_exec_slow_engine.store(nullptr);
    ExpectOk(r, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const u64 t1 = platform::NowMs();
    const Rep r2 = g.SvClientInfo(3);
    const u32 took2 = Ms(t1);
    g_slow_ms.store(0);
    g_slow_engine.store(nullptr);
    std::printf("  F4b: the next request, 10 ms after the answer, came back after %u ms with 0x%08X (failed_fast %u)\n", took2, r2.res(), a.hub.failed_fast());
    TCHECK_EQ(r2.hdr(), Plain(0xC));
    TCHECK_EQ(r2.res(), R_NO_DATA);
    TCHECK_EQ(a.hub.failed_fast(), 0u);
}

static void F5() {
    std::printf("F5: SetPlayerName is the name of a console whose Initialize sends none; InitializeWithName overrides it\n");
    Fix f({"Alice", "Bob"});
    Node& a = f.n(0);
    Node& b = f.n(1);
    Game ga(a), gb(b);
    u16 zoe[kNameUnits], yan[kNameUnits];
    Put(zoe, "Zoe");
    Put(yan, "Yan");
    a.hub.SetPlayerName(zoe);  // (the plugin: the console's profile name; the configuration said "Alice" / "Bob")
    b.hub.SetPlayerName(yan);
    // the creator: SRVR Initialize, with no name (what Triple Deluxe and Planet Robobot send)
    TCHECK_EQ(ga.SvInit(3).res(), 0u);
    TCHECK_EQ(ga.SvDup().res(), 0u);
    TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
    // the searcher: FKCL Initialize (0x01), no name either; it finds the group
    TCHECK_EQ(gb.FkInit(4).res(), 0u);
    TCHECK_EQ(gb.FkChannels().res(), 0u);
    TCHECK_EQ(gb.FkScan().res(), 0u);
    TCHECK(WaitFor(8000, [&] {
        const Rep q = gb.FkInOrder(1);
        return q.res() == 0 && std::memcmp(gb.TitleRec() + kTitleInfoMac, a.mac().data(), 6) == 0;
    }));
    Rep r = gb.FkServerInfo(a.mac());  // the creator's entry in the searcher's list
    TCHECK_EQ(r.res(), 0u);
    TCHECK(NameAt(gb.ServerRec() + kServerInfoHostName) == "Zoe");
    JoinResult j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK(WaitFor(5000, [&] { return gb.NodesTotal() == 2 && ga.ClientsTotal() == 1; }));
    r = gb.FkNodeInfo(1);  // the roster, as the searcher reads it
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF001, "Zoe", 1);
    r = gb.FkNodeInfo(2);
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF002, "Yan", 2);
    r = ga.SvClientInfo(2);  // ... and as the creator reads it
    TCHECK_EQ(r.res(), 0u);
    CheckNodeRecord(r, 2, 0xF002, "Yan", 2);
    r = ga.SvClientInfo(1);  // the host's own record (kNoData, filled)
    TCHECK_EQ(r.res(), R_NO_DATA);
    CheckNodeRecord(r, 2, 0xF001, "Zoe", 1);
    // the names given by the games' own InitializeWithName override the default
    TCHECK_EQ(gb.FkFinalize().res(), 0u);
    TCHECK(WaitFor(4000, [&] { return ga.ClientsTotal() == 0u; }));
    TCHECK_EQ(ga.SvFinalize().res(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // (the creator's pump sees the seat empty before the next join)
    TCHECK_EQ(ga.SvInitName(3, "Zed").hdr(), Plain(0xF));
    TCHECK_EQ(ga.SvDup().res(), 0u);
    TCHECK_EQ(ga.SvAccepting(0, 6).res(), 0u);
    TCHECK_EQ(gb.FkInitName("Cy", 4).hdr(), Plain(0x11));
    TCHECK_EQ(gb.FkChannels().res(), 0u);
    TCHECK_EQ(gb.FkScan().res(), 0u);
    TCHECK(WaitFor(8000, [&] {
        const Rep q = gb.FkInOrder(1);
        return q.res() == 0 && std::memcmp(gb.TitleRec() + kTitleInfoMac, a.mac().data(), 6) == 0;
    }));
    TCHECK_EQ(gb.FkServerInfo(a.mac()).res(), 0u);
    TCHECK(NameAt(gb.ServerRec() + kServerInfoHostName) == "Zed");
    j = JoinGroup(gb, a.mac());
    TCHECK(j.done && j.rep.res() == 0);
    TCHECK(WaitFor(5000, [&] { return gb.NodesTotal() == 2 && ga.ClientsTotal() == 1; }));
    r = gb.FkNodeInfo(1);
    CheckNodeRecord(r, 2, 0xF001, "Zed", 1);
    r = gb.FkNodeInfo(2);
    CheckNodeRecord(r, 2, 0xF002, "Cy", 2);
    r = ga.SvClientInfo(2);
    CheckNodeRecord(r, 2, 0xF002, "Cy", 2);
    TCHECK_EQ(a.hub.cancelled() + b.hub.cancelled() + a.hub.failed_fast() + b.hub.failed_fast(), 0u);
}

// =====================================================================================================================

int main(int argc, char** argv) {
    t_game = true;  // the main thread is a game thread (the test calls the hub as the game does)
    Watchdog watchdog;
    g_watchdog = &watchdog;
    struct Scenario {
        const char* name;
        void (*fn)();
        u32 limit_s;
        bool by_name = false;  // runs only when named on the command line (F4b: see its comment)
    };
    static const Scenario all[] = {{"H1", H1, 120},  {"H2a", H2a, 10}, {"H2b", H2b, 120}, {"H3", H3, 40}, {"H3b", H3b, 40},
                                   {"H4", H4, 120},  {"H5", H5, 150},  {"H6", H6, 260},   {"H7", H7, 120}, {"H8", H8, 120},
                                   {"H9", H9, 220}, {"X1", X1, 80}, {"X2", X2, 80}, {"X3", X3, 80}, {"X4", X4, 80},
                                   {"X5", X5, 80}, {"X6", X6, 80}, {"X7", X7, 80}, {"X8", X8, 80}, {"X9", X9, 100},
                                   {"X10", X10, 100}, {"X11", X11, 80}, {"X12", X12, 80}, {"F1a", F1a, 60}, {"F1b", F1b, 90},
                                   {"F1c", F1c, 60},  {"F1d", F1d, 60}, {"F2", F2, 60},   {"F3", F3, 60},   {"F4", F4, 60},
                                   {"F5", F5, 90},    {"F4b", F4b, 60}};
    std::printf("dlp_hub_test: the hub of the fake Download Play lobby, real threads, real engines\n");
    const u64 t_start = platform::NowMs();
    int ran = 0;
    for (const Scenario& s : all) {
        bool wanted = argc <= 1 && !s.by_name;
        for (int i = 1; i < argc; ++i) {
            wanted |= std::strcmp(argv[i], s.name) == 0 || (std::strcmp(argv[i], "H2") == 0 && s.name[1] == '2') ||
                      (std::strcmp(argv[i], "F1") == 0 && std::strncmp(s.name, "F1", 2) == 0);
        }
        if (!wanted) {
            continue;
        }
        const u64 t0 = platform::NowMs();
        const int fails0 = g_tfails.load() + g_failures;
        watchdog.Arm(s.limit_s, s.name);
        s.fn();
        watchdog.Disarm();
        ++ran;
        std::printf("  %s: %u ms, %d new failures\n", s.name, Ms(t0), g_tfails.load() + g_failures - fails0);
        std::fflush(stdout);
    }
    g_ran_all = argc <= 1;
    H2();
    std::printf("%d scenarios, %u ms, %d checks, %d failures\n", ran, Ms(t_start), g_checks + g_tchecks.load(), g_failures + g_tfails.load());
    return g_failures + g_tfails.load() == 0 ? 0 : 1;
}
