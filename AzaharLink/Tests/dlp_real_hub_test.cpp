// The real Download Play AS THE PLUGIN HAS IT (1.1.3, phase F5 of analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md): a DlpHub per console
// whose lobby runs on the adapter RealDlpNet (azl/dlp_real_net.hpp, the hub's backend: what MakeRealDlpHub of Sources/ctr/plugin.cpp makes for a game
// in RealDlp mode), with a network thread per console (the engine's Tick, then the hub's: what RoomClient's thread does) and the games' IPC words
// served by DlpHub::Serve on game threads. Engines joined by the in-memory room of Tests/mock_room.hpp with the Azahar-peer rule checker on every
// frame; real time. The game is Luigi's Mansion (2018) as NETCODE.md reads its code: InitializeWithName, StartAccepting by hand on channel 6,
// AcceptClient when the host starts, StartDistribution, then SendWirelessRebootPassphrase every 33 ms with a NEW passphrase while the state is 8;
// the searcher InitializeWithName with 2 beacons, StartScan with its child's filter, GetTitleInfoInOrder until it finds the host, StopScan,
// StartFakeSession, GetMyStatus until 9, the passphrase, Finalize.
//
// Scenarios (design 7.3; the engine's and the adapter's own halves are in dlp_real_engine_test, dlp_real_net_test, dlp_real_client_test):
//   R3   3DS - 3DS through the hubs, 1 and 3 clients, run in BOTH modes on the same commands (the dialect's EngineLobbyNet, as 1.1.2 has it, and the
//        real Download Play): the external states the games see - the creator's states, the clients' states, the searchers' GetMyStatus, the
//        lists, the records, the roster at 9, the passphrase, the answers to the second passphrase, the engine given back - are IDENTICAL
//   R8   the game's nwm::UDS starts (DlpHub::OnGameUdsStarting) during a probe, while hosting, with a creator at 2 / 8 / 9 and a searcher at
//        5 / 6 / 9, with the network thread alive (a posted request) and gone (run on the calling thread): the engine free (not initialised, not
//        held, the hub says so), the binds gone, the client's foreign-lobby filter off; no Tick after it takes the engine again; the game's own
//        Initialize then works on it
//   R10  Finalize, StopScan, StopSession, EndAccepting in every state of both roles (with the network thread): the engine given back, and a second
//        Finalize finds nothing to end (success, nothing changes)
//   R11b through the hub: a client that leaves and is back within 3 s is ONE client of the creator (no ghost, no node 0), and a third console
//        that reads the announcement as a spectator is never one of GetConnectingClients (NETCODE point 3); the game then goes to 9
//   R13  the two modes in one room (a console of 1.1.2 and one of 1.1.3 with the same game): neither lists the other, the real searcher never
//        probes the dialect's host, a join by MAC fails
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_hub_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp
//       dlph_wire.o <the core sources of Tests/run.sh>
#pragma GCC diagnostic ignored "-Wunused-function"  // (the rig's helpers this test does not use)

#include <algorithm>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>

#include "dlp_net_rig.hpp"

#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

// ---------------------------------------------------------------------------------------------------------------------
// Checks that threads may use.

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

static constexpr DlpKind kFk = DlpKind::Fkcl;
static constexpr DlpKind kSv = DlpKind::Srvr;
static constexpr u64 kLuigiTid = 0x00040000001D1A00ULL;
static constexpr u64 kLuigiFilter = 0x00040001001D1A00ULL;  // StartScan's filter: the child of the game (log of prova 3)
static constexpr u32 kLuigiLow = 0x001D1A00;
static const u8 kOther9[9] = {'f', 'f', 'e', 'e', 'd', 'd', 'c', 'c', 0};

static u32 Plain(u32 id) {
    return (id << 16) | 0x40;
}
static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------------------------------------------------
// The memory of the games (their buffers), the console with its hub, the network thread.

class GMem {
public:
    u32 Alloc(u32 size) {
        std::lock_guard<std::mutex> lk(m_);
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
        std::lock_guard<std::mutex> lk(m_);
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.data.size()) {
                return r.data.data() + (addr - r.addr);
            }
        }
        return nullptr;
    }
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        return static_cast<GMem*>(ctx)->Find(addr, size);
    }

private:
    struct Region {
        u32 addr = 0;
        std::vector<u8> data;
    };
    std::mutex m_;
    std::deque<Region> regions_;  // (a deque: a pointer into one region stays valid while others are added)
    u32 next_ = 0x08000000;
};

static LobbyConfig Cfg(u8 last, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = kLuigiTid;
    c.friend_seed = 0xF000u + last;
    Put(c.name, name);
    return c;
}

/// One console as the plugin makes it for Luigi's Mansion: the engine on the room, the adapter of the real Download Play (real) or none (the
/// dialect of 1.1.2: the hub's own EngineLobbyNet), and the hub.
struct RNode {
    RNode(MockRoom& room, u8 last, const char* name, bool real)
        : con(room, last), real_net(real ? new RealDlpNet(con.engine.get()) : nullptr),
          hub(con.engine.get(), &con.fake_kernel, Cfg(last, name), mem.Resolver(), real_net.get()) {
        if (real_net) {
            real_net->SetIdentity(Me(0xF000u + last, name));  // (MakeRealDlpHub's: the lobby's own identity)
        }
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    Console con;
    GMem mem;
    std::unique_ptr<RealDlpNet> real_net;
    DlpHub hub;
};

/// The engine is the game's again: not initialised, not held (the hub and the adapter agree), no probe, the filter off, no bind of the adapter.
static bool EngFree(RNode& n, const char* where) {
    bool ok = !n.e().initialized() && !n.hub.EngineHeld() && !n.e().client_foreign_filter_enforced();
    if (n.real_net) {
        ok = ok && !n.real_net->Holds() && !n.real_net->Probing();
    }
    for (u32 id : {RealDlpNet::kHostBind, RealDlpNet::kProbeBind, RealDlpNet::kSessionBind}) {
        UdsEngine::UnbindInfo info;
        info.channel = 0;
        n.e().Unbind(id, &info);
        ok = ok && info.channel == 0;
    }
    if (!ok) {
        std::printf("   engine not free %s: initialised %d, held %d, filter %d, adapter holds %d, probing %d\n", where, n.e().initialized(),
                    n.hub.EngineHeld(), n.e().client_foreign_filter_enforced(), n.real_net ? n.real_net->Holds() : 0,
                    n.real_net ? n.real_net->Probing() : 0);
    }
    return ok;
}

/// The network thread of a console: the engine's Tick, then the hub's (RoomClient's tick callback in plugin.cpp).
class NetThread {
public:
    explicit NetThread(RNode& n) : node_(n), thread_([this] { Run(); }) {}
    ~NetThread() {
        Stop();
    }
    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
        }
    }
    u64 rounds() const {
        return rounds_;
    }

private:
    void Run() {
        while (!stop_) {
            node_.e().Tick(platform::NowMs());
            node_.hub.Tick();
            ++rounds_;
            Sleep(3);
        }
    }
    RNode& node_;
    std::atomic<bool> stop_{false};
    std::atomic<u64> rounds_{0};
    std::thread thread_;
};

struct Room {
    Room(std::initializer_list<std::pair<const char*, bool>> nodes) {
        g_rules.Reset();
        u8 last = 1;
        for (const auto& nm : nodes) {
            n.push_back(std::make_unique<RNode>(room, last++, nm.first, nm.second));
        }
        for (auto& x : n) {
            nets.push_back(std::make_unique<NetThread>(*x));
        }
        for (auto& t : nets) {  // (the plugin's network thread has ticked before a game can ask)
            WaitFor(2000, [&] { return t->rounds() >= 2; });
        }
    }
    ~Room() {
        nets.clear();   // (no Tick once the consoles begin to go)
        CHECK_RULES();  // no frame of the room broke a rule of Azahar's (Tests/azahar_rules.hpp)
    }
    MockRoom room;
    std::vector<std::unique_ptr<RNode>> n;
    std::vector<std::unique_ptr<NetThread>> nets;
};

// ---------------------------------------------------------------------------------------------------------------------
// A game thread: the commands as the games send them (Tests/dlp_hub_test.cpp's, with Luigi's ids) through DlpHub::Serve.

struct Rep {
    u32 w[16] = {};
    DlpOutcome out = DlpOutcome::Done;
    u32 hdr() const {
        return w[0];
    }
    u32 res() const {
        return w[1];
    }
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
static std::string NameAt(const u8* p) {
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
static std::string Hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", v);
    return b;
}

class Game {
public:
    explicit Game(RNode& n) : n_(n) {
        title_ = n.mem.Alloc(kTitleInfoSize);
        server_ = n.mem.Alloc(kServerInfoSize);
        ids_ = n.mem.Alloc(16);
        cli_ = n.mem.Alloc(16);
        sb_title_[0] = (kTitleInfoSize << 14) | 2;
        sb_title_[1] = title_;
        sb_server_[0] = (kServerInfoSize << 14) | 2;
        sb_server_[1] = server_;
        sb_ids_[0] = (16u << 14) | 2;
        sb_ids_[1] = ids_;
    }
    Rep Call(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf = nullptr) {
        u32 buf[64 + 8];
        for (u32 i = 0; i < 64; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < 8; ++i) {
            buf[64 + i] = 0xFEEDFACE;
        }
        buf[0] = header;
        u32 i = 1;
        for (u32 a : args) {
            buf[i++] = a;
        }
        Rep r;
        r.out = n_.hub.Serve(kind, buf, sbuf);
        std::memcpy(r.w, buf, sizeof(r.w));
        for (u32 k = 0; k < 8; ++k) {
            TCHECK(buf[64 + k] == 0xFEEDFACE);
        }
        return r;
    }
    bool Pending(Rep* r) {
        u32 buf[64];
        for (u32 i = 0; i < 64; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        const bool done = n_.hub.PollPending(buf);
        if (done) {
            std::memcpy(r->w, buf, sizeof(r->w));
        }
        return done;
    }
    /// StartFakeSession and the wait of the HLE (PollPending every 10 ms). The reply's result, or 0xFFFFFFFF if it never came.
    u32 Join(const Mac& mac, u32 max_ms = 15000) {
        Rep r = Call(kFk, 0x000B0100, {MacW1(mac), MacW2(mac), kLuigiLow, 0x00040001});
        if (r.out != DlpOutcome::JoinPending) {
            return r.res();
        }
        const u64 t0 = platform::NowMs();
        while (platform::NowMs() - t0 < max_ms) {
            if (Pending(&r)) {
                return r.res();
            }
            Sleep(10);
        }
        return 0xFFFFFFFFu;
    }
    // dlp:FKCL
    Rep FkInitName(const char* name, u32 beacons = 2) {
        u32 n[5];
        NameWords(name, n);
        return Call(kFk, 0x00110203, {0x30000, beacons, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    Rep FkFinalize() { return Call(kFk, 0x00020000, {}); }
    Rep FkChannels() { return Call(kFk, 0x00040000, {}); }
    Rep FkScan() { return Call(kFk, 0x00050180, {0x0421, static_cast<u32>(kLuigiFilter), static_cast<u32>(kLuigiFilter >> 32), 0, 0, 0}); }
    Rep FkStopScan() { return Call(kFk, 0x00060000, {}); }
    Rep FkServerInfo(const Mac& m) { return Call(kFk, 0x00070080, {MacW1(m), MacW2(m)}, sb_server_); }
    Rep FkInOrder(u32 restart) { return Call(kFk, 0x00090040, {restart}, sb_title_); }
    Rep FkStatus() { return Call(kFk, 0x000C0000, {}); }
    Rep FkNodes(u32 n) { return Call(kFk, 0x000D0040, {n}, sb_ids_); }
    Rep FkNodeInfo(u32 node) { return Call(kFk, 0x000E0040, {node}); }
    Rep FkPass() { return Call(kFk, 0x000F0000, {}); }
    Rep FkStopSession() { return Call(kFk, 0x00100000, {}); }
    // dlp:SRVR
    Rep SvInitName(u32 max_clients, const char* name) {
        u32 n[5];
        NameWords(name, n);
        return Call(kSv, 0x000F0303, {0x85000, max_clients, 0xE, 0, 0x40000, 2, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x7777, 0x8888});
    }
    Rep SvFinalize() { return Call(kSv, 0x00020000, {}); }
    Rep SvState() { return Call(kSv, 0x00030000, {}); }
    Rep SvAccepting(u32 manual, u32 channel) { return Call(kSv, 0x00050080, {manual, channel}); }
    Rep SvEnd() { return Call(kSv, 0x00060000, {}); }
    Rep SvDistribute() { return Call(kSv, 0x00070000, {}); }
    Rep SvPass(const u8* nine) {
        u32 w[3];
        PassWords(nine, w);
        return Call(kSv, 0x000800C0, {w[0], w[1], w[2]});
    }
    Rep SvAccept(u32 node) { return Call(kSv, 0x00090040, {node}); }
    Rep SvClients(u32 n, u32 bytes) { return Call(kSv, 0x000B0042, {n, (bytes << 4) | 0xC, cli_}); }
    Rep SvClientInfo(u32 node) { return Call(kSv, 0x000C0040, {node}); }
    Rep SvClientState(u32 node) { return Call(kSv, 0x000D0040, {node}); }
    // what the game reads
    const u8* TitleRec() { return n_.mem.Find(title_, kTitleInfoSize); }
    const u8* ServerRec() { return n_.mem.Find(server_, kServerInfoSize); }
    u16 NodeId(u32 i) { return LoadLE16(n_.mem.Find(ids_, 8) + 2 * i); }
    u16 ClientId(u32 i) { return LoadLE16(n_.mem.Find(cli_, 8) + 2 * i); }
    u32 StatusState() { return FkStatus().w[2] >> 24; }
    u32 ServerState() { return SvState().w[2]; }
    u32 ClientsTotal() {
        const Rep r = SvClients(4, 8);
        return r.res() == 0 ? r.w[2] : 0xFFFFFFFFu;
    }
    /// GetTitleInfoInOrder(1) until the host is listed (true) or `ms` ran out.
    bool Find(const Mac& host, u32 ms) {
        return WaitFor(ms, [&] {
            const Rep a = FkInOrder(1);
            return a.res() == 0 && std::memcmp(TitleRec() + kTitleInfoMac, host.data(), 6) == 0;
        });
    }
    /// The searcher up to the join: Initialize with a name, GetChannels, StartScan, the host in the list, StopScan, StartFakeSession.
    u32 ScanAndJoin(const char* name, const Mac& host, u32 find_ms = 8000) {
        TCHECK_EQ(FkInitName(name).res(), 0u);
        TCHECK_EQ(FkChannels().res(), 0u);
        TCHECK_EQ(FkScan().res(), 0u);
        const bool found = Find(host, find_ms);
        TCHECK(found);
        TCHECK_EQ(FkStopScan().res(), 0u);
        return found ? Join(host) : 0xFFFFFFFFu;
    }
    /// The creator up to accepting: InitializeWithName, StartAccepting by hand on channel 6 (Luigi's).
    void Open(u32 max_clients, const char* name) {
        TCHECK_EQ(SvInitName(max_clients, name).res(), 0u);
        TCHECK_EQ(SvAccepting(1, 6).res(), 0u);
    }

private:
    RNode& n_;
    u32 title_ = 0, server_ = 0, ids_ = 0, cli_ = 0;
    u32 sb_title_[2] = {}, sb_server_[2] = {}, sb_ids_[2] = {};
};

/// A passphrase of the game's kind: 8 hex digits and a 0, a new one each call (Luigi calls BeginGame every 33 ms with a new one).
static void NewPass(u32 k, u8 out[9]) {
    static const char* hex = "0123456789abcdef";
    for (u32 i = 0; i < 8; ++i) {
        out[i] = static_cast<u8>(hex[(k * 7 + i * 5 + 3) & 15]);
    }
    out[8] = 0;
}

// =====================================================================================================================
// R3: the same game, the same commands, in both modes: what the games see must not differ.

struct Trace {
    std::mutex m;
    std::vector<std::string> lines;
    void Add(const std::string& who, const std::string& what) {
        std::lock_guard<std::mutex> lk(m);
        lines.push_back(who + ": " + what);
    }
    std::vector<std::string> Sorted() {
        std::lock_guard<std::mutex> lk(m);
        std::vector<std::string> v = lines;
        std::stable_sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return a.substr(0, a.find(':')) < b.substr(0, b.find(':')); });
        return v;
    }
};

static const char* kNames[4] = {"Alice", "Bob", "Carol", "Dave"};

static void Creator(RNode& n, u32 clients, Trace& t, std::atomic<u32>& in_session) {
    Game g(n);
    const std::string me = "creator";
    Rep r = g.SvInitName(clients, kNames[0]);
    t.Add(me, "init " + Hex(r.hdr()) + " " + Hex(r.res()) + " state " + std::to_string(g.ServerState()));
    r = g.SvAccepting(1, 6);
    t.Add(me, "accepting " + Hex(r.res()) + " state " + std::to_string(g.ServerState()) + " held " + std::to_string(n.hub.EngineHeld()));
    const bool all = WaitFor(15000, [&] { return g.ClientsTotal() == clients; });
    TCHECK(all);
    t.Add(me, "clients " + std::to_string(g.ClientsTotal()));
    std::vector<std::string> infos;  // (who got which node is the order of the joins, a race in both modes: the records are compared as a set)
    for (u32 node = 2; node < 2 + clients; ++node) {
        r = g.SvClientState(node);
        t.Add(me, "client " + std::to_string(node) + " state " + Hex(r.w[2]) + " before accept");
        r = g.SvClientInfo(node);
        const u8* rec = reinterpret_cast<const u8*>(&r.w[2]);
        infos.push_back("client info " + Hex(r.res()) + " seed " + Hex(LoadLE32(rec)) + " " + NameAt(rec + 0x0C) + " node " +
                        std::to_string(LoadLE32(rec + 0x24) == node ? 1 : 0));
        r = g.SvAccept(node);
        t.Add(me, "accept " + std::to_string(node) + " " + Hex(r.res()));
        r = g.SvClientState(node);
        t.Add(me, "client " + std::to_string(node) + " state " + Hex(r.w[2]) + " accepted");
    }
    std::sort(infos.begin(), infos.end());
    for (const std::string& i : infos) {
        t.Add(me, i);
    }
    TCHECK(WaitFor(15000, [&] { return in_session.load() == clients; }));
    r = g.SvDistribute();
    t.Add(me, "distribute " + Hex(r.res()));
    TCHECK(WaitFor(15000, [&] { return g.ServerState() == 8; }));
    t.Add(me, "state 8");
    for (u32 node = 2; node < 2 + clients; ++node) {
        t.Add(me, "client " + std::to_string(node) + " state " + Hex(g.SvClientState(node).w[2]) + " at 8");
    }
    Sleep(300);  // (the searchers sit in 6 meanwhile: the state is seen in both modes)
    const u64 passed = platform::NowMs();
    r = g.SvPass(kNine);
    t.Add(me, "first passphrase " + Hex(r.res()));
    r = g.SvPass(kOther9);
    t.Add(me, "second passphrase " + Hex(r.res()));
    for (u32 k = 0; g.ServerState() == 8 && k < 200; ++k) {  // (the game's storm: a new passphrase every 33 ms while the state is 8)
        u8 p[9];
        NewPass(k, p);
        g.SvPass(p);
        Sleep(33);
    }
    TCHECK(WaitFor(15000, [&] { return g.ServerState() == 9; }));
    const u32 to9 = static_cast<u32>(platform::NowMs() - passed);
    std::printf("   the creator at 9 %u ms after its first passphrase\n", static_cast<unsigned>(to9));
    TCHECK(to9 <= DlpLobby::kGoGraceMs + 500);  // (the lobby's own bound, both modes)
    t.Add(me, "state 9, clients " + std::to_string(g.ClientsTotal()) + ", held " + std::to_string(n.hub.EngineHeld()));
    r = g.SvFinalize();
    t.Add(me, "finalize " + Hex(r.res()) + " state " + std::to_string(g.ServerState()));
    TCHECK(EngFree(n, "after the creator's Finalize"));
}

static void Searcher(RNode& n, RNode& host, u32 index, Trace& t, std::atomic<u32>& in_session) {
    Game g(n);
    const std::string me = std::string("searcher ") + kNames[index];
    Rep r = g.FkInitName(kNames[index]);
    t.Add(me, "init " + Hex(r.res()) + " status " + std::to_string(g.StatusState()));
    r = g.FkChannels();
    t.Add(me, "channels " + Hex(r.res()) + " " + Hex(r.w[2]));
    r = g.FkScan();
    t.Add(me, "scan " + Hex(r.res()) + " status " + std::to_string(g.StatusState()));
    const bool found = g.Find(host.mac(), 10000);
    TCHECK(found);
    t.Add(me, std::string("found ") + (found ? "yes" : "no") + " unique id " + Hex(LoadLE32(g.TitleRec() + kTitleInfoUniqueId)) + " variation " +
                  Hex(LoadLE32(g.TitleRec() + kTitleInfoVariation)));
    r = g.FkServerInfo(host.mac());
    const u8* si = g.ServerRec();
    t.Add(me, "server " + Hex(r.res()) + " max " + std::to_string(si[kServerInfoMaxClients]) + " host " + NameAt(si + kServerInfoHostName));
    r = g.FkStopScan();
    t.Add(me, "stop scan " + Hex(r.res()) + " status " + std::to_string(g.StatusState()));
    const u32 joined = g.Join(host.mac());
    TCHECK_EQ(joined, 0u);
    t.Add(me, "join " + Hex(joined) + " status " + std::to_string(g.StatusState()));
    in_session.fetch_add(1);
    std::vector<u32> seen;
    const bool nine = WaitFor(20000, [&] {
        const u32 s = g.StatusState();
        if (seen.empty() || seen.back() != s) {
            seen.push_back(s);
        }
        return s == 9;
    });
    TCHECK(nine);
    std::string states;
    for (u32 s : seen) {
        states += (states.empty() ? "" : " ") + std::to_string(s);
    }
    t.Add(me, "statuses " + states);
    r = g.FkPass();
    t.Add(me, "passphrase " + Hex(r.res()) + " " + std::string(reinterpret_cast<const char*>(&r.w[2]), 8));
    r = g.FkNodes(4);
    std::string roster = std::to_string(r.w[2]) + ":";
    for (u32 i = 0; i < r.w[2] && i < 4; ++i) {
        roster += " " + std::to_string(g.NodeId(i));
    }
    t.Add(me, "roster at 9 " + roster);
    r = g.FkNodeInfo(1);
    t.Add(me, "host record " + Hex(r.res()) + " " + NameAt(reinterpret_cast<const u8*>(&r.w[2]) + 0x0C));
    r = g.FkFinalize();
    t.Add(me, "finalize " + Hex(r.res()));
    TCHECK(EngFree(n, "after the searcher's Finalize"));
}

static std::vector<std::string> RunR3(bool real, u32 clients) {
    Room room({{kNames[0], real}});
    for (u32 i = 1; i <= clients; ++i) {
        room.n.push_back(std::make_unique<RNode>(room.room, static_cast<u8>(i + 1), kNames[i], real));
        room.nets.push_back(std::make_unique<NetThread>(*room.n.back()));
        WaitFor(2000, [&] { return room.nets.back()->rounds() >= 2; });
    }
    Trace t;
    std::atomic<u32> in_session{0};
    std::vector<std::thread> threads;
    const u64 t0 = platform::NowMs();
    threads.emplace_back([&] { Creator(*room.n[0], clients, t, in_session); });
    for (u32 i = 1; i <= clients; ++i) {
        threads.emplace_back([&, i] { Searcher(*room.n[i], *room.n[0], i, t, in_session); });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::printf("   %s, %u client%s: %u ms\n", real ? "real Download Play" : "dialect (1.1.2)", clients, clients == 1 ? "" : "s", static_cast<unsigned>(platform::NowMs() - t0));
    u32 missed = 0;
    for (auto& n : room.n) {
        missed += n->hub.missed_ticks();  // (rounds of the network thread that found a game thread in the hub's mutex: design 7.4 point 8, R12 of 8.2)
        TCHECK_EQ(n->hub.cancelled(), 0u);
        TCHECK_EQ(n->hub.failed_fast(), 0u);
        TCHECK_EQ(n->hub.ran_here(), 0u);
        if (real) {
            TCHECK_EQ(n->real_net->counters().content, 0u);
        }
    }
    if (real) {
        // the real Download Play was on the wire: the host announced and dialogued, each searcher probed and answered
        const RealDlpNet::Counters& h = room.n[0]->real_net->counters();
        TCHECK(h.announce >= 5);
        TCHECK(h.auth >= clients);
        TCHECK(h.dist >= clients);
        TCHECK(h.final >= clients);
        for (u32 i = 1; i <= clients; ++i) {
            const RealDlpNet::Counters& c = room.n[i]->real_net->counters();
            TCHECK(c.verified >= 1);
            // A2, A4, A6 at least; the A9 only if a notice came before the game finalised (Luigi finalises 1 ms after reading the passphrase,
            // and the host's first notice is 100 ms after the final: then its lobby ends on the client's leaving instead, as in the dialect)
            TCHECK(c.answers >= 3);
            TCHECK_EQ(c.host_bad, 0u);
            std::printf("   searcher %u: probes %u, verified %u, answers %u; host: auth %u, dist %u, finish %u, final %u, notice %u, ignored %u\n", i,
                        c.probes, c.verified, c.answers, h.auth, h.dist, h.finish, h.final, h.notice, h.ignored);
        }
        TCHECK_EQ(h.bad, 0u);
    }
    std::printf("   rounds of the network threads that skipped the hub (a game thread held its mutex): %u\n", static_cast<unsigned>(missed));
    return t.Sorted();
}

static void R3() {
    for (u32 clients : {1u, 3u}) {
        std::printf("R3: 3DS - 3DS through the hubs, %u client%s, both modes: the games see the same\n", clients, clients == 1 ? "" : "s");
        const std::vector<std::string> lobby = RunR3(false, clients);
        const std::vector<std::string> real = RunR3(true, clients);
        TCHECK_EQ(lobby.size(), real.size());
        for (std::size_t i = 0; i < lobby.size() && i < real.size(); ++i) {
            if (lobby[i] != real[i]) {
                std::printf("   differ: dialect '%s' / real '%s'\n", lobby[i].c_str(), real[i].c_str());
            }
            TCHECK(lobby[i] == real[i]);
        }
        // and what the games must see, written out (the same in both: the comparison above)
        auto has = [&](const std::string& line) { return std::find(real.begin(), real.end(), line) != real.end(); };
        TCHECK(has("searcher Bob: statuses 5 6 9"));
        TCHECK(has("searcher Bob: passphrase 00000000 1a2b3c4d"));
        TCHECK(has("searcher Bob: found yes unique id 00001d1a variation 00000000"));
        TCHECK(has("creator: second passphrase e0a0ac01"));
        TCHECK(has("creator: client 2 state 07010002 at 8"));
        TCHECK(has("creator: finalize 00000000 state 0"));
        TCHECK(has(std::string("searcher Bob: roster at 9 ") + (clients == 1 ? "2: 1 2" : "4: 1 2 3 4")));
        if (g_tfails.load() != 0) {
            for (const std::string& l : real) {
                std::printf("   real: %s\n", l.c_str());
            }
        }
    }
}

// =====================================================================================================================
// R8: the game's nwm::UDS starts in every state, with the network thread alive and gone.

/// The network thread of console `i` is gone: no Tick for longer than DlpHub::kAliveMs.
static void KillNet(Room& r, std::size_t i) {
    r.nets[i]->Stop();
    Sleep(DlpHub::kAliveMs + 150);
}

/// OnGameUdsStarting on console `i`, then 300 ms of rounds of its network thread (if alive): the engine stays free, then the game's own
/// Initialize works on it and the hub stays out of its way.
static void GiveBack(Room& r, std::size_t i, bool alive, const char* what) {
    RNode& n = *r.n[i];
    const u32 here0 = n.hub.ran_here();
    const bool held = n.hub.EngineHeld();
    n.hub.OnGameUdsStarting();
    // (gone: run on this thread when the lobby held anything; a lobby that holds nothing - a creator at 9 - returns at once, either way)
    TCHECK(n.hub.ran_here() - here0 <= (alive ? 0u : 1u));
    if (!alive && held) {
        TCHECK_EQ(n.hub.ran_here() - here0, 1u);
    }
    TCHECK(EngFree(n, what));
    if (alive) {
        Sleep(300);
        TCHECK(EngFree(n, what));  // (no Tick took the engine back: no probe, no session)
    }
    NodeInfo info;
    std::memset(&info, 0, sizeof(info));
    u32 event = 0;
    TCHECK_EQ(n.e().Initialize(info, 0x3000, 0x400, &event), 0u);  // the game's own Initialize
    if (alive) {
        Sleep(100);
    }
    TCHECK(n.e().initialized());
    TCHECK(!n.hub.EngineHeld());
    n.e().Shutdown();
}

static void R8() {
    for (const bool alive : {true, false}) {
        const char* how = alive ? "network thread alive" : "network thread gone";
        std::printf("R8: OnGameUdsStarting in every state (%s)\n", how);
        {  // a searcher in the middle of a probe (its engine held as a spectator)
            Room r({{"Alice", true}, {"Bob", true}});
            Game ga(*r.n[0]), gb(*r.n[1]);
            ga.Open(1, "Alice");
            TCHECK_EQ(gb.FkInitName("Bob").res(), 0u);
            TCHECK_EQ(gb.FkScan().res(), 0u);
            const bool probing = WaitFor(5000, [&] { return r.n[1]->real_net->Probing() && r.n[1]->hub.EngineHeld(); });
            TCHECK(probing);
            if (!alive) {
                KillNet(r, 1);  // (the probe stays where it was: nobody pumps it)
                TCHECK(r.n[1]->real_net->Probing());
            }
            GiveBack(r, 1, alive, "during a probe");
            TCHECK_EQ(gb.StatusState(), alive ? 1u : 1u);
        }
        {  // a creator at 2 with a client in, and the searcher at 5
            Room r({{"Alice", true}, {"Bob", true}});
            Game ga(*r.n[0]), gb(*r.n[1]);
            ga.Open(1, "Alice");
            TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
            TCHECK_EQ(gb.StatusState(), 5u);
            TCHECK_EQ(ga.ServerState(), 2u);
            if (!alive) {
                KillNet(r, 0);
                KillNet(r, 1);
            }
            GiveBack(r, 0, alive, "creator at 2");
            GiveBack(r, 1, alive, "searcher at 5");
        }
        {  // a creator at 8 and the searcher at 6
            Room r({{"Alice", true}, {"Bob", true}});
            Game ga(*r.n[0]), gb(*r.n[1]);
            ga.Open(1, "Alice");
            TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
            TCHECK_EQ(ga.SvAccept(2).res(), 0u);
            TCHECK_EQ(ga.SvDistribute().res(), 0u);
            TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 8; }));
            TCHECK(WaitFor(5000, [&] { return gb.StatusState() == 6; }));
            if (!alive) {
                KillNet(r, 0);
                KillNet(r, 1);
            }
            GiveBack(r, 0, alive, "creator at 8");
            GiveBack(r, 1, alive, "searcher at 6");
        }
        {  // a creator at 9 and the searcher at 9 (the searcher keeps its roster: the engine is held until the game finalises or starts UDS)
            Room r({{"Alice", true}, {"Bob", true}});
            Game ga(*r.n[0]), gb(*r.n[1]);
            ga.Open(1, "Alice");
            TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
            TCHECK_EQ(ga.SvAccept(2).res(), 0u);
            TCHECK_EQ(ga.SvDistribute().res(), 0u);
            TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 8; }));
            TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
            TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 9 && gb.StatusState() == 9; }));
            TCHECK(r.n[1]->hub.EngineHeld());
            if (!alive) {
                KillNet(r, 0);
                KillNet(r, 1);
            }
            GiveBack(r, 0, alive, "creator at 9");
            GiveBack(r, 1, alive, "searcher at 9");
            TCHECK_EQ(gb.StatusState(), 9u);  // (9 stays: the passphrase and the roster are still readable)
            const Rep p = gb.FkPass();
            TCHECK_EQ(p.res(), 0u);
            TCHECK(std::memcmp(&p.w[2], kNine, 9) == 0);
        }
    }
}

// =====================================================================================================================
// R10: the ends of every role in every state.

static void R10() {
    std::printf("R10: Finalize, StopScan, StopSession, EndAccepting in every state\n");
    for (int end = 0; end < 3; ++end) {  // the searcher: StopScan / Finalize while scanning (a probe may be under way)
        Room r({{"Alice", true}, {"Bob", true}});
        Game ga(*r.n[0]), gb(*r.n[1]);
        ga.Open(1, "Alice");
        TCHECK_EQ(gb.FkInitName("Bob").res(), 0u);
        TCHECK_EQ(gb.FkScan().res(), 0u);
        if (end == 0) {
            Sleep(30);  // (a probe has just begun)
        } else {
            TCHECK(gb.Find(r.n[0]->mac(), 8000));
        }
        const Rep x = end == 2 ? gb.FkFinalize() : gb.FkStopScan();
        TCHECK_EQ(x.res(), 0u);
        if (end != 2) {
            TCHECK_EQ(gb.FkFinalize().res(), 0u);
        }
        TCHECK(EngFree(*r.n[1], "searcher ended while scanning"));
        TCHECK_EQ(gb.FkFinalize().res(), 0u);  // (a second one: nothing to end, success)
        TCHECK(EngFree(*r.n[1], "a second Finalize"));
        TCHECK_EQ(ga.SvFinalize().res(), 0u);
        TCHECK(EngFree(*r.n[0], "creator ended at 2 with nobody in"));
    }
    for (int state : {5, 6, 9}) {
        for (int how = 0; how < 2; ++how) {  // StopSession then Finalize, or Finalize alone
            Room r({{"Alice", true}, {"Bob", true}});
            Game ga(*r.n[0]), gb(*r.n[1]);
            ga.Open(1, "Alice");
            TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
            if (state >= 6) {
                TCHECK_EQ(ga.SvAccept(2).res(), 0u);
                TCHECK_EQ(ga.SvDistribute().res(), 0u);
                TCHECK(WaitFor(5000, [&] { return gb.StatusState() == 6; }));
            }
            if (state == 9) {
                TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 8; }));
                TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
                TCHECK(WaitFor(5000, [&] { return gb.StatusState() == 9; }));
            }
            if (how == 0) {
                TCHECK_EQ(gb.FkStopSession().res(), 0u);
            }
            TCHECK_EQ(gb.FkFinalize().res(), 0u);
            TCHECK(EngFree(*r.n[1], "searcher ended in a session"));
            TCHECK_EQ(gb.FkFinalize().res(), 0u);
            TCHECK(EngFree(*r.n[1], "a second Finalize"));
            if (state < 9) {
                // the creator sees the client go (the engine's Deauthentication of the searcher's Release)
                TCHECK(WaitFor(3000, [&] { return ga.ClientsTotal() == 0; }));
            }
            if (state != 9 && how == 1) {
                TCHECK_EQ(ga.SvEnd().res(), 0u);  // EndAccepting first: the network stays, closed to newcomers
            }
            TCHECK_EQ(ga.SvFinalize().res(), 0u);
            TCHECK(EngFree(*r.n[0], "creator ended"));
            TCHECK_EQ(ga.SvFinalize().res(), 0u);
            TCHECK(EngFree(*r.n[0], "a second Finalize of the creator"));
        }
    }
}

// =====================================================================================================================
// R11b through the hub: a quick rejoin is one client; a spectator is never one.

/// Carol scans for `ms`: true if her probe was a spectator of Alice's network at some point; `*steady` = Alice's GetConnectingClients said `want`
/// at every read meanwhile (NETCODE point 3: a spectator is never one of them; Luigi's host leaves its wait when the number changes).
static bool Spectate(Room& r, Game& ga, Game& gc, u32 want, u32 ms, bool* steady) {
    TCHECK_EQ(gc.FkInitName("Carol").res(), 0u);
    TCHECK_EQ(gc.FkScan().res(), 0u);
    bool spectated = false;
    *steady = true;
    const u64 end = platform::NowMs() + ms;
    while (platform::NowMs() < end) {
        spectated = spectated || r.n[0]->e().SpectatorCount() > 0;
        *steady = *steady && ga.ClientsTotal() == want;
        Sleep(2);
    }
    TCHECK(gc.Find(r.n[0]->mac(), 10));  // (she read the announcement: the host is in her list)
    TCHECK_EQ(gc.FkStopScan().res(), 0u);
    TCHECK_EQ(gc.FkFinalize().res(), 0u);
    TCHECK(EngFree(*r.n[2], "the spectator's console after its scan"));
    // after her Deauthentication the count is still the same
    for (int k = 0; k < 50; ++k) {
        *steady = *steady && ga.ClientsTotal() == want;
        Sleep(4);
    }
    return spectated;
}

static void R11b() {
    std::printf("R11b: a client back within 3 s is one client; a third console reading the announcement is never one\n");
    Room r({{"Alice", true}, {"Bob", true}, {"Carol", true}});
    Game ga(*r.n[0]), gb(*r.n[1]), gc(*r.n[2]);
    ga.Open(2, "Alice");  // (two clients: the network is not full with Bob in, so Carol can still read the announcement)
    // Carol reads the announcement while nobody is in: 0 clients all the while, and after she left
    bool steady = false;
    TCHECK(Spectate(r, ga, gc, 0, 1500, &steady));
    TCHECK(steady);
    TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
    TCHECK(WaitFor(3000, [&] { return ga.ClientsTotal() == 1; }));
    // Bob leaves and is back at once (the game scans again, finds the host again, joins again)
    const u64 left = platform::NowMs();
    TCHECK_EQ(gb.FkStopSession().res(), 0u);
    TCHECK_EQ(gb.FkFinalize().res(), 0u);
    TCHECK_EQ(gb.ScanAndJoin("Bob", r.n[0]->mac()), 0u);
    const u32 back = static_cast<u32>(platform::NowMs() - left);
    std::printf("   back in %u ms\n", static_cast<unsigned>(back));
    TCHECK(back < 3000);  // (inside the engine's kLateDeauthMs: the window of the ghost)
    // the creator has exactly one client, a real node (never 0), whose record is Bob's
    bool one = true;
    const u64 end = platform::NowMs() + 800;
    while (platform::NowMs() < end) {
        one = one && ga.ClientsTotal() <= 1;
        Sleep(5);
    }
    TCHECK(one);
    TCHECK_EQ(ga.ClientsTotal(), 1u);
    const u16 node = ga.ClientId(0);
    TCHECK(node >= 2 && node <= 4);
    const Rep info = ga.SvClientInfo(node);
    TCHECK_EQ(info.res(), 0u);
    TCHECK(NameAt(reinterpret_cast<const u8*>(&info.w[2]) + 0x0C) == "Bob");
    // Carol reads the announcement again with Bob in: 1 client all the while, and after she left
    TCHECK(Spectate(r, ga, gc, 1, 1500, &steady));
    TCHECK(steady);
    // and the game goes on to 9 with Bob alone
    TCHECK_EQ(ga.SvAccept(node).res(), 0u);
    TCHECK_EQ(ga.SvDistribute().res(), 0u);
    TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 8; }));
    TCHECK_EQ(ga.SvPass(kNine).res(), 0u);
    TCHECK(WaitFor(5000, [&] { return ga.ServerState() == 9 && gb.StatusState() == 9; }));
    const Rep nodes = gb.FkNodes(4);
    TCHECK_EQ(nodes.w[2], 2u);
    TCHECK_EQ(gb.NodeId(0), 1u);
    TCHECK_EQ(gb.NodeId(1), node);
    TCHECK_EQ(gb.FkFinalize().res(), 0u);
    TCHECK_EQ(ga.SvFinalize().res(), 0u);
    for (auto& n : r.n) {
        TCHECK(EngFree(*n, "after R11b"));
    }
}

// =====================================================================================================================
// R13: a console of 1.1.2 (the dialect) and one of 1.1.3 (the real Download Play) with the same game in one room.

static void R13() {
    for (const bool host_real : {false, true}) {
        std::printf("R13: a %s host and a %s searcher: invisible to each other\n", host_real ? "real Download Play" : "dialect (1.1.2)",
                    host_real ? "dialect (1.1.2)" : "real Download Play");
        Room r({{"Alice", host_real}, {"Bob", !host_real}});
        Game ga(*r.n[0]), gb(*r.n[1]);
        ga.Open(1, "Alice");
        TCHECK_EQ(gb.FkInitName("Bob").res(), 0u);
        TCHECK_EQ(gb.FkScan().res(), 0u);
        TCHECK(!gb.Find(r.n[0]->mac(), 2500));
        const Rep none = gb.FkInOrder(1);
        TCHECK_EQ(none.res(), R_NO_DATA);
        if (!host_real) {
            TCHECK_EQ(r.n[1]->real_net->counters().probes, 0u);  // (the dialect's beacon is not filed: never probed)
        } else {
            TCHECK_EQ(r.n[0]->real_net->counters().announce, 0u);  // (nobody connected as a spectator)
        }
        TCHECK_EQ(gb.FkStopScan().res(), 0u);
        const u32 joined = gb.Join(r.n[0]->mac(), 6000);
        TCHECK(joined != 0u);
        TCHECK_EQ(ga.ClientsTotal(), 0u);
        TCHECK_EQ(gb.FkFinalize().res(), 0u);
        TCHECK_EQ(ga.SvFinalize().res(), 0u);
        TCHECK(EngFree(*r.n[0], "R13 host"));
        TCHECK(EngFree(*r.n[1], "R13 searcher"));
    }
}

int main(int argc, char** argv) {
    struct Scenario {
        const char* name;
        void (*fn)();
    };
    static const Scenario all[] = {{"R3", R3}, {"R8", R8}, {"R10", R10}, {"R11b", R11b}, {"R13", R13}};
    for (const Scenario& s : all) {
        bool run = argc < 2;
        for (int i = 1; i < argc; ++i) {
            run = run || std::strcmp(argv[i], s.name) == 0;
        }
        if (run) {
            s.fn();
        }
    }
    std::printf("\n%d checks, %d failures (and %d / %d of the rig)\n", g_tchecks.load(), g_tfails.load(), g_checks, g_failures);
    return g_tfails.load() == 0 && g_failures == 0 ? 0 : 1;
}
