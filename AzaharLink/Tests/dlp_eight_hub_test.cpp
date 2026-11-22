// dlp_eight_hub_test (2026-11-13): the lobby of EIGHT players as a console runs it - one DlpHub per console (the lobby, the service and the
// adapter behind it, built with the configuration the plugin gives Mario Kart 7), a network thread per console that ticks the engine and the hub,
// REAL engines joined by the in-memory room - driven only with the words of dlp:FKCL / dlp:SRVR that Mario Kart 7 sent on a console
// (analysis/prova-1.1.4-2026-11-13/c63-mk7: 0x0F req 00089000 00000007 ..., 0x0B req 0000000f 000001ec ..., 0x11 req 0000f000 00000004 ...).
// Tests/dlp_eight_test.cpp holds the same lobby on the in-memory air (with the IPC words) and on the real engine (without the hub and without the
// words); this file is the whole chain at once, with the game's threads of eight consoles asking at the same instant:
//   E1  seven searchers enter in the same instant; the ids the creator's game reads in its vector of 15 (seven, 2..8) and the eight each client
//       reads; the ninth and the tenth are never let in; the beacon says 8 of 8
//   E2  two leave while the ninth and the tenth enter; the two that left cannot come back (full)
//   E3  START with one client silent (thrown out at the 1500 ms of the grace) and one whose answers reach the creator only after the grace:
//       who is out, what the others read, GO with the five that are left
//   E4  the creator's game ends the lobby with seven clients in it (and once more in the middle of START): the seven read 0x80; one of them
//       opens a lobby of its own and the others enter it
//   E5  a whole session to state 9, then every game opens nwm::UDS (the hub gives the engine back first) and the same eight consoles are the
//       eight nodes of the game's own network: every node hears every other, one leaves
// What it does not hold: the lobby's frames arrive at once here (the in-memory room has no latency), and when a client leaves the GAME's network
// the other clients' node lists are only printed (the engine's host tells the other clients of a lobby, not of a game's network).
// Real time, about 25 s.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_hub.hpp"
#include "azl/uds_frames.hpp"
#include "mock_room.hpp"

using namespace azl;

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

namespace {

constexpr u32 kBase = 0x08000000;       // the game's memory
constexpr u32 kTitleAt = kBase;         // DLPTitleInfo (0x13A8)
constexpr u32 kServerAt = kBase + 0x1400;  // DLPServerInfo (0x298)
constexpr u32 kIdsAt = kBase + 0x1800;  // the vectors of ids
constexpr u64 kMk7 = 0x0004000000030700ull;
constexpr u32 kMk7Low = 0x00030700;
static const DlpKind kFk = DlpKind::Fkcl;
static const DlpKind kSv = DlpKind::Srvr;
static const u8 kPass[9] = {'0', 'f', '1', 'e', '2', 'd', '3', 'c', 0};

template <typename F>
static bool Wait(u32 ms, F f) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        if (f()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static LobbyConfig Cfg(u8 last, const char* name, u8 max_players) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = kMk7;
    c.friend_seed = 0xF000u + last;
    for (u32 i = 0; i < kNameUnits && name[i] != 0; ++i) {
        c.name[i] = static_cast<u16>(static_cast<u8>(name[i]));
    }
    c.max_players = max_players;
    return c;
}

/// One console: the engine on the room, the hub, the game's memory, the network thread (the engine's Tick, then the hub's, every 3 ms).
struct Con8 {
    Con8(MockRoom& room, u8 last, const char* nm, u8 max_players = 8)
        : name(nm), con(room, last), mem(0x2000, 0xCC), hub(con.engine.get(), &con.fake_kernel, Cfg(last, nm, max_players), Resolver()) {
        thread = std::thread([this] {
            while (!stop.load()) {
                if (!paused.load()) {
                    con.engine->Tick(platform::NowMs());
                    hub.Tick();
                    ++rounds;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });
        Wait(2000, [this] { return rounds.load() >= 2; });
    }
    ~Con8() {
        stop = true;
        thread.join();
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = [](void* ctx, u32 address, u32 size) -> u8* {
            Con8* c = static_cast<Con8*>(ctx);
            if (address < kBase || address - kBase > c->mem.size() || size > c->mem.size() - (address - kBase)) {
                return nullptr;
            }
            return c->mem.data() + (address - kBase);
        };
        return r;
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }

    // ---- the game's thread: one command through the hub (a join in progress is waited for as the HLE does, 10 ms apart)
    u32 w[64];
    u32 Ask(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf = nullptr) {
        for (u32& x : w) {
            x = 0xC0DE0000u;
        }
        w[0] = header;
        u32 i = 1;
        for (const u32 a : args) {
            w[i++] = a;
        }
        if (hub.Serve(kind, w, sbuf) == DlpOutcome::JoinPending) {
            const u64 end = platform::NowMs() + 8000;
            while (!hub.PollPending(w)) {
                if (platform::NowMs() >= end) {
                    return 0xDEADDEAD;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return w[1];
    }
    void NameWords(u32 out[5]) const {
        u16 n[10] = {};
        for (u32 i = 0; i < 10 && name[i] != 0; ++i) {
            n[i] = static_cast<u16>(static_cast<u8>(name[i]));
        }
        for (int i = 0; i < 5; ++i) {
            out[i] = static_cast<u32>(n[2 * i]) | (static_cast<u32>(n[2 * i + 1]) << 16);
        }
    }
    // dlp:SRVR, Mario Kart 7's words
    u32 SvInit(u32 clients = 7) {
        u32 n[5];
        NameWords(n);
        return Ask(kSv, 0x000F0303, {0x00089000, clients, 0x4A, 0, 0x00040000, 2, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x7777, 0x8888});
    }
    u32 SvAccepting(u32 manual = 0, u32 channel = 11) {
        return Ask(kSv, 0x00050080, {manual, channel});
    }
    u32 SvState() {
        return Ask(kSv, 0x00030000, {}) != 0 ? 0xEE : w[2];
    }
    /// GetConnectingClients as the game asks it (15 elements, descriptor 0x1EC): "total:id,id,..." - the ids are read out of the game's vector,
    /// and what the reply did not declare must still be the 0xCC it was.
    std::string SvClients() {
        std::memset(mem.data() + (kIdsAt - kBase), 0xCC, 64);
        if (Ask(kSv, 0x000B0042, {0x0F, 0x1EC, kIdsAt}) != 0) {
            return "error";
        }
        return Ids(w[2], 15);
    }
    u32 SvClientState(u32 node) {
        return Ask(kSv, 0x000D0040, {node}) != 0 ? w[1] : w[2];
    }
    /// GetClientInfo: the name of the record, or "" when the command fails.
    std::string SvClientName(u32 node) {
        if (Ask(kSv, 0x000C0040, {node}) != 0) {
            return "";
        }
        const u8* rec = reinterpret_cast<const u8*>(&w[2]);
        std::string s;
        for (u32 i = 0; i < kNameUnits && LoadLE16(rec + 0x0C + 2 * i) != 0; ++i) {
            s += static_cast<char>(LoadLE16(rec + 0x0C + 2 * i));
        }
        return LoadLE32(rec + 0x24) == node ? s : "wrong-node";
    }
    u32 SvStart() {
        return Ask(kSv, 0x00070000, {});
    }
    u32 SvPass() {
        u32 p[3] = {0, 0, 0};
        std::memcpy(p, kPass, 9);
        return Ask(kSv, 0x000800C0, {p[0], p[1], p[2]});
    }
    u32 SvFinalize() {
        return Ask(kSv, 0x00020000, {});
    }
    // dlp:FKCL, Mario Kart 7's words
    u32 FkInit() {
        u32 n[5];
        NameWords(n);
        return Ask(kFk, 0x00110203, {0x0000F000, 4, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    u32 FkScan() {
        return Ask(kFk, 0x00050180, {0x00000421, kMk7Low, 0x00040001, 0, 0, 0});
    }
    u32 FkStopScan() {
        return Ask(kFk, 0x00060000, {});
    }
    /// GetServerInfo of `host`: max << 8 | joined, or 0xFFFF when the lobby is not in the list.
    u32 FkServer(const Mac& host) {
        const u32 sb[2] = {(0x298u << 14) | 2, kServerAt};
        const u32 w1 = static_cast<u32>(host[0]) | (static_cast<u32>(host[1]) << 8) | (static_cast<u32>(host[2]) << 16) | (static_cast<u32>(host[3]) << 24);
        const u32 w2 = static_cast<u32>(host[4]) | (static_cast<u32>(host[5]) << 8);
        if (Ask(kFk, 0x00070080, {w1, w2}, sb) != 0) {
            return 0xFFFF;
        }
        const u8* rec = mem.data() + (kServerAt - kBase);
        return (static_cast<u32>(rec[8]) << 8) | rec[9];
    }
    u32 FkJoin(const Mac& host) {
        const u32 w1 = static_cast<u32>(host[0]) | (static_cast<u32>(host[1]) << 8) | (static_cast<u32>(host[2]) << 16) | (static_cast<u32>(host[3]) << 24);
        const u32 w2 = static_cast<u32>(host[4]) | (static_cast<u32>(host[5]) << 8);
        return Ask(kFk, 0x000B0100, {w1, w2, kMk7Low, 0x00040001});
    }
    u32 FkStatus() {
        return Ask(kFk, 0x000C0000, {}) != 0 ? 0xEEEEEEEE : w[2];
    }
    u32 FkState() {
        return FkStatus() >> 24;
    }
    u32 FkNode() {
        return FkStatus() & 0xFFFF;
    }
    std::string FkNodes() {
        std::memset(mem.data() + (kIdsAt - kBase), 0xCC, 64);
        const u32 sb[2] = {(30u << 14) | 2, kIdsAt};
        if (Ask(kFk, 0x000D0040, {15}, sb) != 0) {
            return "error";
        }
        return Ids(w[2], 15);
    }
    std::string FkPass() {
        Ask(kFk, 0x000F0000, {});
        return std::string(reinterpret_cast<const char*>(w + 2), 8);
    }
    u32 FkStop() {
        return Ask(kFk, 0x00100000, {});
    }
    u32 FkFinalize() {
        return Ask(kFk, 0x00020000, {});
    }
    std::string Ids(u32 total, u32 elements) {
        const u8* p = mem.data() + (kIdsAt - kBase);
        std::string s = std::to_string(total) + ":";
        u32 shown = 0;
        for (u32 i = 0; i < 32; ++i) {
            const u32 id = LoadLE16(p + 2 * i);
            if (id == 0xCCCC) {
                break;
            }
            s += (shown++ ? "," : "") + std::to_string(id);
        }
        if (shown > (total < elements ? total : elements)) {
            s += " OVERRUN";
        }
        return s;
    }
    /// The searcher's whole way in, as the game does it: InitializeWithName, StartScan, wait until the lobby of `host` is listed with a free place
    /// (`need_room`), StopScan, StartFakeSession. The result of the join, or 0xFFFF0001 when the lobby was never listed that way in `list_ms`.
    u32 Enter(const Mac& host, bool need_room = true, u32 list_ms = 5000) {
        FkInit();
        if (FkScan() != 0) {
            return 0xFFFF0002;
        }
        const bool listed = Wait(list_ms, [&] {
            const u32 s = FkServer(host);
            return s != 0xFFFF && (!need_room || (s & 0xFF) < (s >> 8));
        });
        FkStopScan();
        if (!listed) {
            return 0xFFFF0001;
        }
        return FkJoin(host);
    }

    const char* name;
    Console con;
    std::vector<u8> mem;
    DlpHub hub;
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<u64> rounds{0};
    std::thread thread;
};

struct World {
    MockRoom room;
    std::vector<std::unique_ptr<Con8>> c;
    explicit World(int n, u8 max_players = 8) {
        static const char* const kNames[12] = {"Host", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "Ninth", "Tenth", "Late", "Last"};
        g_rules.Reset();
        for (int i = 0; i < n; ++i) {
            c.emplace_back(new Con8(room, static_cast<u8>(i + 1), kNames[i], max_players));
        }
    }
    Con8& h() {
        return *c[0];
    }
    /// Runs f(i) for every i of `who` on a thread of its own, all released in the same instant; returns when all are done.
    void Together(const std::vector<int>& who, const std::function<void(int)>& f) {
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        for (const int i : who) {
            ts.emplace_back([&, i] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                f(i);
            });
        }
        go = true;
        for (std::thread& t : ts) {
            t.join();
        }
    }
    /// The creator opens for seven, consoles 1..7 enter in the same instant. False = they are not all in.
    bool SevenEnter() {
        CHECK_EQ(h().SvInit(7), 0u);
        CHECK_EQ(h().SvAccepting(0, 11), 0u);
        CHECK_EQ(h().SvState(), 2u);
        u32 res[8] = {};
        Together({1, 2, 3, 4, 5, 6, 7}, [&](int i) { res[i] = c[i]->Enter(h().mac()); });
        bool in = true;
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(res[i], 0u);
            in = in && res[i] == 0;
        }
        if (!in) {
            return false;
        }
        const bool settled = Wait(4000, [&] {
            if (h().SvClients() != "7:2,3,4,5,6,7,8") {
                return false;
            }
            for (int i = 1; i <= 7; ++i) {
                if (c[i]->FkNodes() != "8:1,2,3,4,5,6,7,8") {
                    return false;
                }
            }
            return true;
        });
        CHECK(settled);
        return settled;
    }
};

/// The last beacon a console sent: max_nodes << 8 | total_nodes of its NetworkInfo (0 = none yet).
struct BeaconWatch {
    std::atomic<u32> last{0};
    std::atomic<u32> count{0};
    void Attach(Con8& c) {
        c.con.sniff = [this](const WifiPacket& p) {
            if (p.type != WifiPacket::Type::Beacon) {
                return;
            }
            const BeaconTags tags = FindBeaconTags(p.data);
            const std::size_t total_at = offsetof(NetworkInfo, total_nodes) - offsetof(NetworkInfo, oui_value);
            const std::size_t max_at = offsetof(NetworkInfo, max_nodes) - offsetof(NetworkInfo, oui_value);
            if (tags.network != nullptr && tags.network_size > max_at) {
                last = (static_cast<u32>(tags.network[max_at]) << 8) | tags.network[total_at];
                ++count;
            }
        };
    }
};

// ---------------------------------------------------------------------------------------------------------------------

static void E1_SevenTogether_E2_Churn() {
    std::printf("E1: seven enter in the same instant through the hubs; the ids the games read; the ninth and the tenth; the beacon\n");
    World w(10);
    BeaconWatch beacon;
    beacon.Attach(w.h());
    if (!w.SevenEnter()) {
        return;
    }
    Con8& h = w.h();
    // the network the room carries is of eight nodes and the beacon says so (max 8, 8 in)
    CHECK(Wait(2000, [&] { return beacon.last.load() == 0x0808; }));
    u16 node = 0, mask = 0;
    CHECK(h.e().NodeSnapshot(&node, &mask));
    CHECK_EQ(node, 1u);
    CHECK_EQ(mask, 0x00FFu);
    // every client has a node of its own, 2..8; the creator's game reads each one's name and state by that id
    u32 seen = 0;
    for (int i = 1; i <= 7; ++i) {
        const u32 n = w.c[i]->FkNode();
        CHECK(n >= 2 && n <= 8);
        seen |= 1u << n;
        CHECK_EQ(w.c[i]->FkState(), 5u);
        CHECK(h.SvClientName(n) == w.c[i]->name);
        CHECK_EQ(h.SvClientState(n), (5u << 24) | (1u << 16) | n);
    }
    CHECK_EQ(seen, 0x1FCu);
    CHECK(h.SvClientName(9) == "");
    CHECK(h.SvClientName(0) == "");
    CHECK(h.SvClientName(16) == "");
    // the ninth and the tenth: they list the lobby full (8 of 8) and a join asked all the same is never let in
    u32 res9 = 0, res10 = 0;
    w.Together({8, 9}, [&](int i) { (i == 8 ? res9 : res10) = w.c[i]->Enter(h.mac(), false); });
    CHECK(res9 != 0 && res9 != 0xDEADDEAD && res9 != 0xFFFF0001);
    CHECK(res10 != 0 && res10 != 0xDEADDEAD && res10 != 0xFFFF0001);
    CHECK_EQ(w.c[8]->FkState(), 1u);
    CHECK_EQ(w.c[9]->FkState(), 1u);
    CHECK_EQ(h.SvClients(), std::string("7:2,3,4,5,6,7,8"));
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(w.c[i]->FkNodes(), std::string("8:1,2,3,4,5,6,7,8"));
        CHECK_EQ(w.c[i]->FkState(), 5u);
    }
    std::printf("E2: two leave while the ninth and the tenth enter; the two that left find the lobby full\n");
    u32 r[10] = {};
    w.Together({2, 4, 8, 9}, [&](int i) {
        if (i == 2 || i == 4) {
            r[i] = w.c[i]->FkStop();
        } else {
            r[i] = w.c[i]->Enter(h.mac(), true, 6000);
        }
    });
    CHECK_EQ(r[2], 0u);
    CHECK_EQ(r[4], 0u);
    CHECK_EQ(r[8], 0u);
    CHECK_EQ(r[9], 0u);
    const std::vector<int> in = {1, 3, 5, 6, 7, 8, 9};
    CHECK(Wait(4000, [&] {
        if (h.SvClients() != "7:2,3,4,5,6,7,8") {
            return false;
        }
        for (const int i : in) {
            if (w.c[i]->FkNodes() != "8:1,2,3,4,5,6,7,8" || w.c[i]->FkState() != 5) {
                return false;
            }
        }
        return true;
    }));
    seen = 0;
    for (const int i : in) {
        const u32 n = w.c[i]->FkNode();
        seen |= 1u << n;
        CHECK(h.SvClientName(n) == w.c[i]->name);  // the place that was left now has the name of who took it
    }
    CHECK_EQ(seen, 0x1FCu);
    CHECK_EQ(w.c[2]->FkState(), 1u);
    CHECK_EQ(w.c[4]->FkState(), 1u);
    // the two that left come back at once: full, never let in, and nothing of the session moves
    w.Together({2, 4}, [&](int i) { r[i] = w.c[i]->Enter(h.mac(), false); });
    CHECK(r[2] != 0 && r[2] != 0xDEADDEAD);
    CHECK(r[4] != 0 && r[4] != 0xDEADDEAD);
    CHECK_EQ(h.SvClients(), std::string("7:2,3,4,5,6,7,8"));
    CHECK(Wait(1000, [&] { return beacon.last.load() == 0x0808; }));
    // the ninth leaves: one of the two comes back, takes the node that was left, and the creator's game reads its name there
    const u32 freed = w.c[8]->FkNode();
    CHECK_EQ(w.c[8]->FkStop(), 0u);
    CHECK_EQ(w.c[2]->Enter(h.mac(), true, 6000), 0u);
    CHECK_EQ(w.c[2]->FkNode(), freed);
    CHECK(Wait(4000, [&] { return h.SvClients() == "7:2,3,4,5,6,7,8" && h.SvClientName(freed) == w.c[2]->name; }));
    CHECK(Wait(4000, [&] { return w.c[2]->FkNodes() == "8:1,2,3,4,5,6,7,8" && w.c[1]->FkNodes() == "8:1,2,3,4,5,6,7,8"; }));
    CHECK_RULES();
}

static void E3_StartLosesTwo() {
    std::printf("E3: START to seven, one silent and one whose answers come after the grace; GO with the five that are left\n");
    World w(8);
    if (!w.SevenEnter()) {
        return;
    }
    Con8& h = w.h();
    Con8& mute = *w.c[3];
    Con8& late = *w.c[6];
    const u32 mute_node = mute.FkNode(), late_node = late.FkNode();
    // the silent one: its network thread stops. The late one: what it sends on the lobby's channel is held back on its way to the creator
    mute.paused = true;
    std::mutex held_m;
    std::vector<WifiPacket> held;
    std::atomic<bool> holding{true};
    const Mac late_mac = late.mac();
    h.con.drop = [&](const WifiPacket& p) {
        SecureDataHeader sd;
        EtherType type;
        if (!holding.load() || p.type != WifiPacket::Type::Data || !(p.transmitter_address == late_mac) || !GetFrameEtherType(p.data, &type) ||
            type != EtherType::SecureData || !ParseSecureDataHeader(p.data, &sd) || sd.data_channel != UdsEngine::kLobbyChannel) {
            return false;
        }
        std::lock_guard<std::mutex> l(held_m);
        held.push_back(p);
        return true;
    };
    const u64 t0 = platform::NowMs();
    CHECK_EQ(h.SvStart(), 0u);
    CHECK_EQ(h.SvState(), 6u);
    CHECK(Wait(600, [&] { return h.SvClientState(mute_node) >> 24 == 6 && h.SvClientState(late_node) >> 24 == 6; }));  // "starting": no answer yet
    for (int i = 1; i <= 7; ++i) {
        if (i != 3 && i != 6) {
            CHECK(Wait(1000, [&] { return h.SvClientState(w.c[i]->FkNode()) >> 24 == 7; }));  // the five that answered
        }
    }
    CHECK(Wait(3000, [&] { return h.SvState() == 8; }));
    const u64 took = platform::NowMs() - t0;
    std::printf("  state 8 came %llu ms after StartDistribution (the grace is 1500)\n", static_cast<unsigned long long>(took));
    CHECK(took >= 1450 && took <= 2600);  // (not before the grace: the two are waited for; not long after it)
    // who is out: the two, and only the two; the creator's game reads five ids
    std::string five = "5:";
    std::string six = "6:1";
    for (u32 n = 2; n <= 8; ++n) {
        if (n != mute_node && n != late_node) {
            five += (five.size() > 2 ? "," : "") + std::to_string(n);
            six += "," + std::to_string(n);
        }
    }
    CHECK(Wait(1000, [&] { return h.SvClients() == five; }));
    CHECK_EQ(h.SvClientState(mute_node), 0xC880AFEFu);
    CHECK_EQ(h.SvClientState(late_node), 0xC880AFEFu);
    // the others know: each of the five reads six nodes, and is still "started"
    for (int i = 1; i <= 7; ++i) {
        if (i != 3 && i != 6) {
            CHECK(Wait(2000, [&] { return w.c[i]->FkNodes() == six; }));
            CHECK_EQ(w.c[i]->FkState(), 6u);
        }
    }
    // the late answers arrive now, after the grace: the creator has thrown that node out, nothing comes back
    holding = false;
    std::vector<WifiPacket> late_frames;
    {
        std::lock_guard<std::mutex> l(held_m);
        late_frames = held;
    }
    std::printf("  %zu answers of the late client were held back; delivered now\n", late_frames.size());
    CHECK(!late_frames.empty());
    for (const WifiPacket& p : late_frames) {
        h.e().OnWifiPacket(p);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(h.SvState(), 8u);
    CHECK_EQ(h.SvClients(), five);
    // GO: the five at 9 with the passphrase and the roster of six; the creator at 9
    CHECK_EQ(h.SvPass(), 0u);
    CHECK(Wait(4000, [&] {
        for (int i = 1; i <= 7; ++i) {
            if (i != 3 && i != 6 && w.c[i]->FkState() != 9) {
                return false;
            }
        }
        return h.SvState() == 9;
    }));
    for (int i = 1; i <= 7; ++i) {
        if (i != 3 && i != 6) {
            CHECK(w.c[i]->FkPass() == std::string(reinterpret_cast<const char*>(kPass), 8));
            CHECK_EQ(w.c[i]->FkNodes(), six);
        }
    }
    // the two that were thrown out: the silent one wakes, both read "the host is gone" (0x80), never 9, and they have no passphrase
    mute.paused = false;
    CHECK(Wait(5000, [&] { return mute.FkState() == 0x80 && late.FkState() == 0x80; }));
    CHECK(mute.FkPass() == std::string(8, '\0'));
    CHECK(late.FkPass() == std::string(8, '\0'));
    h.con.drop = nullptr;
    CHECK_RULES();
}

static void E4_HostGoes() {
    std::printf("E4: the creator ends the lobby with seven clients in it, and in the middle of START; one of the seven opens its own\n");
    {
        World w(8);
        if (!w.SevenEnter()) {
            return;
        }
        Con8& h = w.h();
        CHECK_EQ(h.SvFinalize(), 0u);
        CHECK_EQ(h.SvState(), 0u);
        CHECK(Wait(5000, [&] {
            for (int i = 1; i <= 7; ++i) {
                if (w.c[i]->FkState() != 0x80) {
                    return false;
                }
            }
            return true;
        }));
        CHECK(!h.e().initialized());
        // the games close their sessions; console 1 opens a lobby of its own, the old creator and the other six enter it
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(w.c[i]->FkFinalize(), 0u);
            CHECK_EQ(w.c[i]->FkState(), 1u);
        }
        Con8& h2 = *w.c[1];
        CHECK_EQ(h2.SvInit(7), 0u);
        CHECK_EQ(h2.SvAccepting(0, 6), 0u);
        u32 res[8] = {};
        w.Together({0, 2, 3, 4, 5, 6, 7}, [&](int i) { res[i] = w.c[i]->Enter(h2.mac()); });
        for (const int i : {0, 2, 3, 4, 5, 6, 7}) {
            CHECK_EQ(res[i], 0u);
        }
        CHECK(Wait(4000, [&] {
            if (h2.SvClients() != "7:2,3,4,5,6,7,8") {
                return false;
            }
            for (const int i : {0, 2, 3, 4, 5, 6, 7}) {
                if (w.c[i]->FkNodes() != "8:1,2,3,4,5,6,7,8") {
                    return false;
                }
            }
            return true;
        }));
        CHECK_RULES();
    }
    {
        World w(8);
        if (!w.SevenEnter()) {
            return;
        }
        Con8& h = w.h();
        CHECK_EQ(h.SvStart(), 0u);
        CHECK_EQ(h.SvFinalize(), 0u);  // (the game gives up while the clients are answering START)
        CHECK(Wait(5000, [&] {
            for (int i = 1; i <= 7; ++i) {
                if (w.c[i]->FkState() != 0x80) {
                    return false;
                }
            }
            return true;
        }));
        for (int i = 1; i <= 7; ++i) {
            CHECK(w.c[i]->FkPass() == std::string(8, '\0'));
        }
        CHECK_RULES();
    }
}

static void E5_ThenTheGamesNetwork() {
    std::printf("E5: a session of eight to state 9, then nwm::UDS: the hub gives the engine back and the eight are the game's eight nodes\n");
    World w(8);
    if (!w.SevenEnter()) {
        return;
    }
    Con8& h = w.h();
    CHECK_EQ(h.SvStart(), 0u);
    CHECK(Wait(3000, [&] { return h.SvState() == 8; }));
    CHECK_EQ(h.SvClients(), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(h.SvPass(), 0u);
    CHECK(Wait(4000, [&] {
        for (int i = 1; i <= 7; ++i) {
            if (w.c[i]->FkState() != 9) {
                return false;
            }
        }
        return h.SvState() == 9;
    }));
    for (int i = 1; i <= 7; ++i) {
        CHECK(w.c[i]->FkPass() == std::string(reinterpret_cast<const char*>(kPass), 8));
        CHECK_EQ(w.c[i]->FkNodes(), std::string("8:1,2,3,4,5,6,7,8"));  // the roster of the lobby stays readable in 9
    }
    // the games open nwm::UDS: the hub first (UdsHle::Dispatch does it), then Initialize finds the engine free on every console
    u32 init[8] = {};
    w.Together({0, 1, 2, 3, 4, 5, 6, 7}, [&](int i) {
        w.c[i]->hub.OnGameUdsStarting();
        u32 ev = 0;
        init[i] = w.c[i]->e().Initialize(MakeNode(0xA000u + static_cast<u32>(i), w.c[i]->name, 0), kTestSharedMemory, kTestVersion, &ev);
    });
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(init[i], 0u);
        CHECK(!w.c[i]->hub.EngineHeld());
    }
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(w.c[i]->FkState(), 9u);  // (9 stays 9 for the game: the passphrase is still readable)
    }
    NetworkInfo ni = MakeNetworkInfo(8, 4);
    ni.channel = 1;
    CHECK_EQ(h.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    u32 conn[8] = {};
    w.Together({1, 2, 3, 4, 5, 6, 7}, [&](int i) {
        u8 info[0x3C] = {};
        std::memcpy(info, h.mac().data(), 6);
        info[6] = 1;
        conn[i] = w.c[i]->e().ConnectToNetwork(info, sizeof(info), 1);
    });
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(conn[i], 0u);
    }
    // eight nodes on every console
    CHECK(Wait(4000, [&] {
        for (int i = 0; i < 8; ++i) {
            u16 node = 0, mask = 0;
            if (!w.c[i]->e().NodeSnapshot(&node, &mask) || mask != 0x00FF) {
                return false;
            }
        }
        return true;
    }));
    u32 nodes_seen = 0;
    u16 node_of[8] = {};
    for (int i = 0; i < 8; ++i) {
        u16 mask = 0;
        CHECK(w.c[i]->e().NodeSnapshot(&node_of[i], &mask));
        CHECK_EQ(mask, 0x00FFu);
        nodes_seen |= 1u << node_of[i];
        const ConnectionStatus cs = w.c[i]->e().GetConnectionStatus();
        CHECK_EQ(cs.max_nodes, 8u);
        CHECK_EQ(cs.total_nodes, 8u);
        // every console knows the eight by the name the GAME gave (not the lobby's identity)
        for (u16 n = 1; n <= 8; ++n) {
            NodeInfo got;
            CHECK_EQ(w.c[i]->e().GetNodeInformation(n, &got), 0u);
            CHECK_EQ(got.friend_code_seed & 0xFFF8u, 0xA000u);
        }
    }
    CHECK_EQ(nodes_seen, 0x1FEu);
    // every node broadcasts once: every other node pulls seven packets, one from each of the others
    for (int i = 0; i < 8; ++i) {
        u32 event = 0;
        CHECK_EQ(w.c[i]->e().Bind(1, 0x4000, 3, 0xFFFF, &event), 0u);
    }
    for (int i = 0; i < 8; ++i) {
        const u8 body[4] = {'m', 'k', '7', static_cast<u8>('0' + node_of[i])};
        CHECK_EQ(w.c[i]->e().SendTo(0xFFFF, 3, 4, 0, body, 4), 0u);
    }
    w.room.WaitIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int i = 0; i < 8; ++i) {
        u32 from = 0;
        for (int k = 0; k < 16; ++k) {
            std::vector<u8> pulled;
            u32 size = 0;
            u16 src = 0;
            if (PullInto(w.c[i]->e(), 1, &pulled, &size, &src) != 0 || size == 0) {
                break;
            }
            CHECK_EQ(size, 4u);
            CHECK_EQ(pulled[3], static_cast<u8>('0' + src));
            CHECK((from & (1u << src)) == 0);  // no duplicate
            from |= 1u << src;
        }
        CHECK_EQ(from, 0x1FEu & ~(1u << node_of[i]));
    }
    // no lobby message is left for anybody, and the hubs hold nothing
    for (int i = 0; i < 8; ++i) {
        u8 from = 0, data[16];
        u32 size = 0;
        CHECK(!w.c[i]->e().PopLobbyMessage(&from, data, 16, &size));
    }
    // one client leaves the game's network: what the host and the six others read afterwards (printed; the host's view is held)
    const u16 gone = node_of[4];
    CHECK_EQ(w.c[4]->e().DisconnectNetwork(), 0u);
    const u16 want = static_cast<u16>(0x00FF & ~(1u << (gone - 1)));
    CHECK(Wait(3000, [&] {
        u16 node = 0, mask = 0;
        return h.e().NodeSnapshot(&node, &mask) && mask == want;
    }));
    const bool others = Wait(3000, [&] {
        for (int i = 1; i <= 7; ++i) {
            u16 node = 0, mask = 0;
            if (i != 4 && (!w.c[i]->e().NodeSnapshot(&node, &mask) || mask != want)) {
                return false;
            }
        }
        return true;
    });
    std::printf("  a client left the game's network of eight: the host's node mask follows; the other six clients' masks %s\n",
                others ? "follow too" : "do NOT follow within 3 s");
    for (int i = 0; i < 8; ++i) {
        w.c[i]->e().Shutdown();
    }
    CHECK_RULES();
}

}  // namespace

int main() {
    std::printf("dlp_eight_hub_test: the lobby of eight through the hubs of eight consoles, with Mario Kart 7's words\n");
    E1_SevenTogether_E2_Churn();
    E3_StartLosesTwo();
    E4_HostGoes();
    E5_ThenTheGamesNetwork();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
