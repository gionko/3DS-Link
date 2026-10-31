// Tests of the logic of the fake Download Play lobby: the codec of the virtual network, the two state machines (the
// searcher = the games' dlp:FKCL, the creator = dlp:SRVR), the ceiling of 4 players, times, loss / duplication / delay /
// reordering of the four messages, the event "the game is starting nwm::UDS", the ownership of the network, the sizes, the
// determinism, and complete rounds in the order Battle Royale and Triple Deluxe call the commands.
//
// Over the in-memory air of Tests/dlp_fakenet.hpp (which behaves like the engine, not like the design). The expected
// values below are written by hand from DESIGN-FASE1.md and SPEC-DLP.md: bytes, times in ms, result codes and state words
// are literals, not the constants of the code under test.
//
//   dlp_lobby_test      (needs nothing; built by Tests/run.sh)
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_lobby_test.cpp -o /tmp/dlp_lobby_test
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_lobby.hpp"
#include "dlp_fakenet.hpp"

using namespace azl;

// A report of the undefined-behaviour sanitizer stops the test (the default only prints it): an out-of-range index in the
// tables is a failure, not a line nobody reads. (Ignored when the test is built without -fsanitize=undefined.)
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(cond)) {                                                                               \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                           \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                           \
        if (va_ != vb_) {                                                                            \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b,   \
                        va_, vb_);                                                                   \
        }                                                                                            \
    } while (0)
#define CHECK_STR(a, b)                                                                              \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const std::string sa_ = (a);                                                                 \
        const std::string sb_ = (b);                                                                 \
        if (sa_ != sb_) {                                                                            \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s == %s ('%s' vs '%s')\n", __FILE__, __LINE__, #a, #b,       \
                        sa_.c_str(), sb_.c_str());                                                   \
        }                                                                                            \
    } while (0)

// Results and states, written out (not the constants of the header).
static constexpr u32 R_INVALID_STATE = 0xE0A0AC01;
static constexpr u32 R_BUSY = 0xE0A0AC03;
static constexpr u32 R_NO_DATA = 0xC880AFEF;
static constexpr u32 R_NOT_FOUND = 0xC880AC05;
static constexpr u32 R_TIMEOUT = 0xC920AFFE;
static constexpr u32 R_OUT_OF_RANGE = 0xD8E0AFFD;

// Message types on the wire (written out).
static constexpr int T_START = 1;
static constexpr int T_START_ACK = 2;
static constexpr int T_GO = 3;
static constexpr int T_GO_ACK = 4;

// ---------------------------------------------------------------------------------------------------------------------
// Consoles and the rig.

static LobbyConfig MakeCfg(u32 tid_low, u64 seed, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = tid_low;  // (the high word is not used: only the low 32 bits are)
    c.friend_seed = seed;
    FakeAir::Name(c.name, name);
    return c;
}

static const char* const kNames[8] = {"Host", "Cli1", "Cli2", "Cli3", "Cli4", "Cli5", "Cli6", "Cli7"};

struct Con {
    FakeLobbyNet net;
    DlpLobby lobby;
    Con(FakeAir& air, int i, u32 tid_low) : net(air, i), lobby(&net, MakeCfg(tid_low, 0x1000u + static_cast<u32>(i), kNames[i])) {}
};

struct Rig {
    FakeAir air;
    std::vector<std::unique_ptr<Con>> c;
    explicit Rig(int n = 4, u64 start = 100000, u32 tid_low = 0x0010C000) : air(start) {
        for (int i = 0; i < n; ++i) {
            c.push_back(std::make_unique<Con>(air, i, tid_low));
        }
    }
    Con& operator[](int i) { return *c[static_cast<size_t>(i)]; }
    /// Advances the clock in steps and pumps every console after each (what the plugin's network thread does).
    void Run(u32 ms, u32 step = 20) {
        for (u32 t = 0; t < ms; t += step) {
            air.Advance(step);
            for (auto& x : c) {
                x->lobby.Pump();
            }
        }
    }
    template <typename F>
    bool RunUntil(F cond, u32 max_ms, u32 step = 20) {
        for (u32 t = 0; t <= max_ms; t += step) {
            if (cond()) {
                return true;
            }
            air.Advance(step);
            for (auto& x : c) {
                x->lobby.Pump();
            }
        }
        return cond();
    }
    /// One forced poll of every console without moving the clock.
    void PollAll() {
        for (auto& x : c) {
            x->lobby.Poll(true);
        }
    }
};

static Mac MacOf(Con& c) {
    return c.net.mac();
}
/// What the IPC front-end does before every command: a Pump.
static u32 CSt(Con& c) {
    c.lobby.Pump();
    return c.lobby.ClientStatusWord() >> 24;
}
static u32 SSt(Con& c) {
    c.lobby.Pump();
    return c.lobby.ServerState();
}
static u32 NClients(Con& c) {
    c.lobby.Pump();
    u16 ids[4] = {};
    u32 total = 99;
    const u32 r = c.lobby.ServerConnectingClients(ids, 4, &total);
    return r == 0 ? total : 0xFFFFFFFFu;
}
static u32 NNodes(Con& c) {
    c.lobby.Pump();
    u16 ids[4] = {};
    u32 total = 99;
    c.lobby.ClientConnectingNodes(ids, 4, &total);
    return total;
}
static bool Sees(Con& c, const Mac& m) {
    c.lobby.Pump();
    GroupView v;
    return c.lobby.ClientServerInfo(m, &v) == 0;
}
static std::string NameOf(const u16* n) {
    std::string s;
    for (int i = 0; i < 10 && n[i] != 0; ++i) {
        s += static_cast<char>(n[i]);
    }
    return s;
}
static std::string PassOf(Con& c) {
    u8 p[9];
    c.lobby.ClientPassphrase(p);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x%02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                  p[7], p[8]);
    return buf;
}
static const u8 kPass1[9] = {'a', 'b', 'c', 'd', '1', '2', '3', '4', 0};
static const u8 kPass2[9] = {'z', 'y', 'x', 'w', '9', '8', '7', '6', 0};
static const char* const kPass1Hex = "616263643132333400";
static const char* const kPass2Hex = "7a7978773938373600";

static HostAnnounce Ann(u32 tid_low, const char* name, u8 max_nodes = 4, u8 accepting = 1) {
    HostAnnounce a;
    std::memset(&a, 0, sizeof(a));
    a.child_tid_low = tid_low;
    FakeAir::Name(a.name, name);
    a.max_nodes = max_nodes;
    a.accepting = accepting;
    return a;
}
static Mac AirMac(int i) {
    return Mac{0x00, 0x1F, 0x32, 0xA0, 0xB0, static_cast<u8>(i)};
}

static void OpenHost(Con& h, u32 max_clients = 3, bool manual = false, u32 child = 0) {
    CHECK_EQ(h.lobby.ServerInitialize(max_clients, child, nullptr), 0);
    CHECK_EQ(h.lobby.ServerStartAccepting(manual, 6), 0);
}

/// The seeker `ci` finds the host `hi` and joins (state 5). Returns false on any failure.
static bool Enter(Rig& r, int ci, int hi, u64 filter = 0) {
    Con& c = r[ci];
    Con& h = r[hi];
    c.lobby.ClientInitialize(nullptr, 16);
    if (c.lobby.ClientStartScan(filter) != 0) {
        return false;
    }
    if (!r.RunUntil([&] { return Sees(c, MacOf(h)); }, 3000)) {
        return false;
    }
    c.lobby.ClientStopScan();
    u32 res = 77;
    if (c.lobby.ClientJoinBegin(MacOf(h), &res) != JoinStep::Pending) {
        return false;
    }
    bool done = false;
    if (!r.RunUntil(
            [&] {
                if (!done) {
                    done = c.lobby.ClientJoinPoll(&res);
                }
                return done;
            },
            5000)) {
        return false;
    }
    r.PollAll();  // (the host tracks the new client at once; the game's own commands pump first)
    return res == 0 && CSt(c) == 5;
}

/// Host `hi` starts the distribution and sends the passphrase; the clients answer. Runs until the host is in 9.
static bool Finish(Rig& r, int hi, const u8* pass = kPass1) {
    Con& h = r[hi];
    if (h.lobby.ServerStartDistribution() != 0) {
        return false;
    }
    if (!r.RunUntil([&] { return SSt(h) == 8; }, 4000)) {
        return false;
    }
    if (h.lobby.ServerSendPassphrase(pass) != 0) {
        return false;
    }
    return r.RunUntil([&] { return SSt(h) == 9; }, 4000);
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. Codec, helpers, sizes.

static void TestCodec() {
    std::printf("codec\n");
    HostAnnounce a = Ann(0x0010C002, "Ann", 4, 1);
    u8 out[kAnnounceSize];
    std::memset(out, 0xEE, sizeof(out));
    EncodeAnnounce(a, out);
    const u8 want[29] = {'D', 'L', 1, 1, 0x02, 0xC0, 0x10, 0x00, 4, 'A', 0, 'n', 0, 'n', 0};  // then 14 zero bytes
    CHECK(std::memcmp(out, want, 29) == 0);
    a.accepting = 0;
    a.max_nodes = 2;
    a.child_tid_low = 0x00183610;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[4], 0x10);
    CHECK_EQ(out[5], 0x36);
    CHECK_EQ(out[6], 0x18);
    CHECK_EQ(out[8], 2);
    // round trip
    HostAnnounce b = Ann(0x0010C002, "Zoe", 3, 1);
    EncodeAnnounce(b, out);
    HostAnnounce r;
    std::memset(&r, 0x77, sizeof(r));
    CHECK(DecodeAnnounce(out, 29, &r));
    CHECK_EQ(r.child_tid_low, 0x0010C002);
    CHECK_EQ(r.max_nodes, 3);
    CHECK_EQ(r.accepting, 1);
    CHECK_STR(NameOf(r.name), "Zoe");
    // every size from 0 to 28 on a buffer of exactly that size (the heap of the test, for ASan)
    for (u32 n = 0; n < 29; ++n) {
        std::unique_ptr<u8[]> p(new u8[n == 0 ? 1 : n]);
        std::memcpy(p.get(), out, n);
        HostAnnounce untouched;
        std::memset(&untouched, 0x5A, sizeof(untouched));
        CHECK(!DecodeAnnounce(p.get(), n, &untouched));
        CHECK_EQ(reinterpret_cast<u8*>(&untouched)[0], 0x5A);  // *out untouched
        CHECK_EQ(untouched.max_nodes, 0x5A);
    }
    {
        std::unique_ptr<u8[]> p(new u8[29]);
        std::memcpy(p.get(), out, 29);
        CHECK(DecodeAnnounce(p.get(), 29, &r));
        std::unique_ptr<u8[]> q(new u8[40]);  // longer, version 1: accepted
        std::memcpy(q.get(), out, 29);
        std::memset(q.get() + 29, 0xFF, 11);
        CHECK(DecodeAnnounce(q.get(), 40, &r));
    }
    CHECK(!DecodeAnnounce(nullptr, 29, &r));
    // magic and version
    u8 bad[29];
    for (int k = 0; k < 4; ++k) {
        std::memcpy(bad, out, 29);
        if (k == 0) bad[0] = 'X';
        if (k == 1) bad[1] = 'M';
        if (k == 2) bad[2] = 0;
        if (k == 3) bad[2] = 2;
        CHECK(!DecodeAnnounce(bad, 29, &r));
    }
    // max_nodes 0, 1, 5, 17 refused; 2, 3, 4 accepted
    const int mn[7] = {0, 1, 2, 3, 4, 5, 17};
    for (int k = 0; k < 7; ++k) {
        std::memcpy(bad, out, 29);
        bad[8] = static_cast<u8>(mn[k]);
        CHECK_EQ(DecodeAnnounce(bad, 29, &r), (mn[k] >= 2 && mn[k] <= 4) ? 1 : 0);
    }
    // flags: only bit 0 is "accepting"
    std::memcpy(bad, out, 29);
    bad[3] = 0x02;
    CHECK(DecodeAnnounce(bad, 29, &r));
    CHECK_EQ(r.accepting, 0);
    bad[3] = 0x03;
    CHECK(DecodeAnnounce(bad, 29, &r));
    CHECK_EQ(r.accepting, 1);

    // messages, byte by byte
    u8 m[16];
    CHECK_EQ(EncodeMessage(MsgType::Start, nullptr, 0, m), 2);
    CHECK(m[0] == 1 && m[1] == 0);
    CHECK_EQ(EncodeMessage(MsgType::StartAck, nullptr, 0, m), 2);
    CHECK(m[0] == 2 && m[1] == 0);
    CHECK_EQ(EncodeMessage(MsgType::GoAck, nullptr, 0, m), 2);
    CHECK(m[0] == 4 && m[1] == 0);
    const u8 body[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::memset(m, 0xEE, sizeof(m));
    CHECK_EQ(EncodeMessage(MsgType::Go, body, 9, m), 11);
    const u8 go[11] = {3, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(std::memcmp(m, go, 11) == 0);
    CHECK_EQ(m[11], 0xEE);  // nothing written beyond
    // refused encodings write nothing
    std::memset(m, 0xEE, sizeof(m));
    CHECK_EQ(EncodeMessage(MsgType::Go, body, 8, m), 0);
    CHECK_EQ(EncodeMessage(MsgType::Go, body, 10, m), 0);
    CHECK_EQ(EncodeMessage(MsgType::Go, nullptr, 9, m), 0);
    CHECK_EQ(EncodeMessage(MsgType::Start, body, 1, m), 0);
    CHECK_EQ(EncodeMessage(static_cast<MsgType>(0), nullptr, 0, m), 0);
    CHECK_EQ(EncodeMessage(static_cast<MsgType>(5), nullptr, 0, m), 0);
    CHECK_EQ(m[0], 0xEE);
    // decode, exact-size heap buffers
    MsgType t = MsgType::Start;
    const u8* bp = nullptr;
    {
        std::unique_ptr<u8[]> p(new u8[11]);
        std::memcpy(p.get(), go, 11);
        CHECK(DecodeMessage(p.get(), 11, &t, &bp));
        CHECK(t == MsgType::Go);
        CHECK(bp == p.get() + 2);
        CHECK_EQ(bp[8], 9);
        CHECK(!DecodeMessage(p.get(), 10, &t, &bp));  // truncated body
        CHECK(!DecodeMessage(p.get(), 12, &t, &bp));  // size beyond 2 + length (reads nothing of byte 12)
        CHECK(!DecodeMessage(p.get(), 1, &t, &bp));
        CHECK(!DecodeMessage(p.get(), 0, &t, &bp));
    }
    {
        std::unique_ptr<u8[]> p(new u8[2]);
        p[0] = 1;
        p[1] = 0;
        CHECK(DecodeMessage(p.get(), 2, &t, &bp));
        CHECK(t == MsgType::Start);
        p[1] = 1;  // length 1 for a type that has none
        CHECK(!DecodeMessage(p.get(), 2, &t, &bp));
        p[0] = 0;
        p[1] = 0;
        CHECK(!DecodeMessage(p.get(), 2, &t, &bp));
        p[0] = 5;
        CHECK(!DecodeMessage(p.get(), 2, &t, &bp));
        p[0] = 3;
        p[1] = 9;  // a GO with its body missing
        CHECK(!DecodeMessage(p.get(), 2, &t, &bp));
    }
    CHECK(!DecodeMessage(nullptr, 2, &t, &bp));
    // the helpers of the IPC words, with the literal values of the games
    CHECK_EQ(Header(0x0B, 2, 2), 0x000B0082u);
    CHECK_EQ(Header(0x01, 1, 0), 0x00010040u);
    CHECK_EQ(StaticBufferDesc(0x298, 0), 0x00A60002u);
    CHECK_EQ(StaticBufferDesc(0x13A8, 0), 0x04EA0002u);
    CHECK_EQ(MappedWriteDesc(8), 0x8Cu);
    CHECK_EQ(MappedWriteDesc(0), 0xCu);
    // layout constants, by hand
    CHECK_EQ(kTitleInfoSize, 5032);
    CHECK_EQ(kServerInfoSize, 664);
    CHECK_EQ(kDlpNodeInfoSize, 40);
    CHECK_EQ(kServerInfoHostName, 0x1C);
    CHECK_EQ(kServerInfoTick, 0x290);
    CHECK_EQ(kNodeInfoName, 0x0C);
    CHECK_EQ(kNodeInfoNodeId, 0x24);
    CHECK_EQ(kEventDescSize, 0x18);
}

static void TestSizes() {
    std::printf("sizes\n");
    std::printf("  sizeof(DlpLobby)=%zu  Group=%zu  Track=%zu  Tables=%zu  NodeEntry=%zu  Identity=%zu  HostAnnounce=%zu  "
                "HeardGroup=%zu  GroupView=%zu  pointer=%zu\n",
                sizeof(DlpLobby), sizeof(Group), sizeof(DlpLobby::Track), sizeof(DlpLobby::Tables), sizeof(NodeEntry),
                sizeof(Identity), sizeof(HostAnnounce), sizeof(HeardGroup), sizeof(GroupView), sizeof(void*));
    CHECK(sizeof(DlpLobby) <= 320);
    CHECK_EQ(sizeof(Group), 40);
    CHECK_EQ(sizeof(DlpLobby::Track), 8);
    CHECK_EQ(sizeof(NodeEntry), 32);
    CHECK_EQ(sizeof(DlpLobby::Tables), 168);
    CHECK(sizeof(HeardGroup) <= 48);
    CHECK_EQ(kMaxPlayers, 4);
    CHECK_EQ(kMaxClients, 3);
    CHECK_EQ(kMaxGroups, 4);
    DlpLobby::Tables tabs;
    CHECK_EQ(sizeof(tabs.scan.g) / sizeof(tabs.scan.g[0]), kMaxGroups);
    CHECK_EQ(sizeof(tabs.session.roster) / sizeof(tabs.session.roster[0]), kMaxPlayers);
    CHECK_EQ(sizeof(tabs.host.t) / sizeof(tabs.host.t[0]), kMaxClients);

    // max_clients 0..15 and max_beacons 0..16, seen from outside: the announcement and the table
    for (u32 req = 0; req <= 15; ++req) {
        Rig r(1);
        CHECK_EQ(r[0].lobby.ServerInitialize(req, 0, nullptr), 0);
        CHECK_EQ(r[0].lobby.ServerStartAccepting(false, 6), 0);
        const u32 eff = req < 1 ? 1 : (req > 3 ? 3 : req);  // by hand: min(max(req, 1), 3)
        CHECK_EQ(r[0].net.LastAnnounce().max_nodes, eff + 1);
        CHECK(r[0].net.LastAnnounce().max_nodes >= 2 && r[0].net.LastAnnounce().max_nodes <= 4);
    }
    for (u32 beacons = 0; beacons <= 16; ++beacons) {
        Rig r(1);
        for (int i = 0; i < 6; ++i) {
            r.air.AddAir(AirMac(i), Ann(0x0010C000, "H", 4, 1), 1);
        }
        r[0].lobby.ClientInitialize(nullptr, beacons);
        CHECK_EQ(r[0].lobby.ClientStartScan(0), 0);
        r.Run(100);
        u32 n = 0;
        GroupView v;
        bool first = true;
        while (r[0].lobby.ClientTitleInfoInOrder(first, &v) == 0) {
            first = false;
            ++n;
        }
        const u32 want = beacons < 1 ? 1 : (beacons > 4 ? 4 : beacons);
        CHECK_EQ(n, want);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. The searcher's table: scan, slots, cursor, expiry, filter.

static u32 CountEntries(Con& c) {
    u32 n = 0;
    GroupView v;
    bool first = true;
    while (c.lobby.ClientTitleInfoInOrder(first, &v) == 0) {
        first = false;
        ++n;
    }
    return n;
}

static void TestScanTable() {
    std::printf("scan table\n");
    {  // one lobby heard: the fields of the entry, the view
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "Hosty", 4, 1), 1, 2);
        Con& s = r[0];
        CHECK_EQ(s.lobby.ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        r.Run(40);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group->host == AirMac(1));
        CHECK_STR(NameOf(v.group->name), "Hosty");
        CHECK_EQ(v.group->max_nodes, 4);
        CHECK_EQ(v.group->total_nodes, 1);
        CHECK_EQ(v.group->signal, 2);
        CHECK_EQ(v.group->child_tid_low, 0x0010C000);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);  // restart
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(s.lobby.ClientTitleInfo(AirMac(1), &v), 0);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(9), &v), R_NO_DATA);
        // signal above 3 is clamped to 3
        r.air.AirGroup(0).signal = 200;
        r.Run(40);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(v.group->signal, 3);
    }
    {  // the game filter: same unique id, other games ignored, the low byte of the filter is the child index (ignored)
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C002, "Same", 4, 1), 1);   // TD, child index 2
        r.air.AddAir(AirMac(2), Ann(0x001C2100, "Other", 4, 1), 1);  // Battle Royale
        r.air.AddAir(AirMac(3), Ann(0x0010C100, "Near", 4, 1), 1);   // unique id 0x10C1: another game
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        r.Run(40);
        CHECK_EQ(CountEntries(s), 1);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(2), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(3), &v), R_NO_DATA);
        // a filter with another low byte finds it; one with another unique id does not
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0x000400010010C005ull), 0);
        r.Run(40);
        CHECK_EQ(CountEntries(s), 1);
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0x000400010010C100ull), 0);
        r.Run(40);
        CHECK_EQ(CountEntries(s), 0);
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0x000400010010C000ull), 0);
        r.Run(40);
        CHECK_EQ(CountEntries(s), 1);
        // the high word of the filter is part of it
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0x000400020010C000ull), 0);
        r.Run(40);
        CHECK_EQ(CountEntries(s), 0);
    }
    {  // invalid announcements are ignored: max_nodes 1 and 5, total_nodes 0 and above max_nodes
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 1, 1), 1);
        r.air.AddAir(AirMac(2), Ann(0x0010C000, "B", 5, 1), 1);
        r.air.AddAir(AirMac(3), Ann(0x0010C000, "C", 4, 1), 0);
        r.air.AddAir(AirMac(4), Ann(0x0010C000, "D", 3, 1), 4);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        CHECK_EQ(CountEntries(s), 0);
        r.air.RemoveAir(0);
        r.air.RemoveAir(1);
        r.air.AddAir(AirMac(5), Ann(0x0010C000, "E", 17, 1), 2);  // (a room never holds more than 4 lobbies: 4 reads suffice)
        r.air.AddAir(AirMac(6), Ann(0x0010C000, "F", 2, 1), 2);   // valid at the border: max 2, total 2
        r.Run(60);
        CHECK_EQ(CountEntries(s), 1);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(6), &v), 0);
    }
    {  // six lobbies, four places: the first four, stable; a place that frees goes to the next
        Rig r(1);
        for (int i = 0; i < 6; ++i) {
            r.air.AddAir(AirMac(i), Ann(0x0010C000, kNames[i], 4, 1), 1);
        }
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(100);
        GroupView v;
        CHECK_EQ(CountEntries(s), 4);
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(s.lobby.ClientServerInfo(AirMac(i), &v), 0);
        }
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(4), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(5), &v), R_NO_DATA);
        r.Run(5000);  // stable for as long as it lasts
        CHECK_EQ(CountEntries(s), 4);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(4), &v), R_NO_DATA);
        // lobby 0 goes away: after 1500 ms its place is free and the 5th lobby (now at index 3 of the air) takes it, slot 0
        r.air.RemoveAir(0);
        r.Run(1600);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(0), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(4), &v), 0);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group->host == AirMac(4));  // slot 0
    }
    {  // the same MAC twice is ONE entry, the last reading wins
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "First", 4, 1), 1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "Second", 3, 1), 2);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(CountEntries(s), 1);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_STR(NameOf(v.group->name), "Second");
        CHECK_EQ(v.group->max_nodes, 3);
        CHECK_EQ(v.group->total_nodes, 2);
    }
    {  // accepting = 0: absent (and removed if it was there); back when accepting = 1 returns
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "Hosty", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        r.air.AirGroup(0).announce.accepting = 0;
        r.Run(40);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), R_NO_DATA);
        CHECK_EQ(CountEntries(s), 0);
        r.air.AirGroup(0).announce.accepting = 1;
        r.Run(40);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        // a lobby that was never accepting never gets in
        r.air.AddAir(AirMac(2), Ann(0x0010C000, "Closed", 4, 0), 1);
        r.Run(60);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(2), &v), R_NO_DATA);
    }
    {  // DeleteScanInfo: removes the entry only; a live lobby comes back at the next round
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "Hosty", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        CHECK_EQ(s.lobby.ClientDeleteScanInfo(AirMac(1)), 0);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientDeleteScanInfo(AirMac(1)), R_NO_DATA);
        r.Run(40);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);  // alive: it comes back
        CHECK_EQ(s.lobby.ClientDeleteScanInfo(AirMac(77)), R_NO_DATA);
    }
}

static void TestCursorAndExpiry() {
    std::printf("cursor, expiry, frozen table\n");
    {  // the cursor: table [A, B, C]; after A, A expires: the next call gives B then C, without repeating or skipping
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        r.air.AddAir(AirMac(2), Ann(0x0010C000, "B", 4, 1), 1);
        r.air.AddAir(AirMac(3), Ann(0x0010C000, "C", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group->host == AirMac(1));
        r.air.RemoveAir(0);  // A goes away
        r.Run(1600);         // ... and expires; B and C stay alive
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group->host == AirMac(2));
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group->host == AirMac(3));
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);  // the next round: B, C
        CHECK(v.group->host == AirMac(2));
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group->host == AirMac(3));
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);
    }
    {  // DeleteScanInfo in the middle of a round leaves the rest alone
        Rig r(1);
        for (int i = 1; i <= 3; ++i) {
            r.air.AddAir(AirMac(i), Ann(0x0010C000, kNames[i], 4, 1), 1);
        }
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group->host == AirMac(1));
        CHECK_EQ(s.lobby.ClientDeleteScanInfo(AirMac(2)), 0);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group->host == AirMac(3));
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);
    }
    {  // expiry at 1499 / 1500 ms, in state 2 only
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.air.Advance(10);
        s.lobby.Poll(true);  // heard at T
        r.air.RemoveAir(0);
        GroupView v;
        r.air.Advance(1499);
        s.lobby.Poll(true);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        r.air.Advance(1);
        s.lobby.Poll(true);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), R_NO_DATA);
    }
    {  // StopScan freezes the table: nothing expires for 60 s, nothing is added; Finalize / Initialize / StartScan clear it
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        CHECK_EQ(s.lobby.ClientStopScan(), 0);
        CHECK_EQ(s.net.ScanCloseCalls(), 1);
        CHECK_EQ(s.lobby.client_state(), 1);
        r.air.RemoveAir(0);
        r.air.AddAir(AirMac(2), Ann(0x0010C000, "B", 4, 1), 1);
        r.Run(60000, 100);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);       // still there
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(2), &v), R_NO_DATA);  // not added
        CHECK_EQ(CountEntries(s), 1);
        CHECK_EQ(s.lobby.ClientStopScan(), 0);  // again: nothing changes
        CHECK_EQ(s.net.ScanCloseCalls(), 1);
        // JoinBegin on the frozen entry reaches ConnectBegin (the lobby is not there: it times out at 3000 ms)
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK_EQ(s.net.ConnectBeginCalls(), 1);
        r.air.Advance(2999);
        s.lobby.Poll(true);
        CHECK(s.lobby.join_active());
        r.air.Advance(1);
        CHECK(s.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_TIMEOUT);
        // the table is intact after the error: the same MAC can be tried again without StartScan
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK_EQ(s.net.ConnectBeginCalls(), 2);
        // StartScan clears the old table
        s.lobby.ClientStopSession();
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        CHECK_EQ(CountEntries(s), 0);
    }
    {  // GetTitleInfoInOrder / ServerInfo out of the table states
        Rig r(1);
        Con& s = r[0];
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), R_NO_DATA);  // state 0
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientDeleteScanInfo(AirMac(1)), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientStartScan(0), R_INVALID_STATE);
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        CHECK_EQ(s.lobby.ClientStartScan(0), R_INVALID_STATE);  // twice
        CHECK_EQ(s.net.ScanOpenCalls(), 1);
    }
}

static void TestViewTick() {
    std::printf("view tick\n");
    // 0x290: the tick the entry was last heard at = NowTicks() - age * 268112 (ticks per ms), by hand
    for (int variant = 0; variant < 3; ++variant) {
        const u64 start = variant == 2 ? 0x1'0000'4000ull : 9000;  // 2: more than 2^32 ms
        Rig r(1, start);
        r.air.SetTickOffset(variant >= 1 ? (1ull << 41) : 0);
        const u64 off = variant >= 1 ? (1ull << 41) : 0;
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        s.lobby.Poll(true);  // heard at `start`
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(v.tick, start * 268112ull + off);  // age 0
        r.air.Advance(1000);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(v.tick, (start + 1000) * 268112ull + off - 1000ull * 268112ull);
        r.air.Advance(400);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        const u64 now_ticks = (start + 1400) * 268112ull + off;
        CHECK(now_ticks - v.tick >= 1400ull * 268112ull && now_ticks - v.tick <= 1401ull * 268112ull);
        // frozen: heard 3.9 s ago and 5.1 s ago: the tick follows the age (the game drops what is older than 5000 ms)
        s.lobby.ClientStopScan();
        r.air.Advance(2500);  // 3.9 s
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        u64 age_ticks = ((start + 3900) * 268112ull + off) - v.tick;
        CHECK_EQ(age_ticks, 3900ull * 268112ull);
        r.air.Advance(1200);  // 5.1 s
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        age_ticks = ((start + 5100) * 268112ull + off) - v.tick;
        CHECK_EQ(age_ticks, 5100ull * 268112ull);
        CHECK(age_ticks * 3.7296 / 1e6 > 5000.0);  // the game would drop it (ticks x 3.7296 ns against 5000 ms)
    }
    {  // heard before the 32-bit clock wraps, read after it: the age is still right
        Rig r(1, 0xFFFFFF00ull);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        s.lobby.Poll(true);
        r.air.Advance(0x200);  // across the wrap
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(r.air.ticks() - v.tick, 0x200ull * 268112ull);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. The join.

/// A seeker on console 1 with the entry of a silent lobby (an announcement nobody runs) in the table, scanning stopped.
static void SilentJoinSetup(Rig& r) {
    r.air.AddAir(AirMac(1), Ann(0x0010C000, "Ghost", 4, 1), 1);
    Con& s = r[1];
    s.lobby.ClientInitialize(nullptr, 16);
    s.lobby.ClientStartScan(0);
    r.Run(60);
    s.lobby.ClientStopScan();
}

static void TestJoin() {
    std::printf("join\n");
    {  // ConnectBegin false: kNotFound at once, state 1, nothing to undo
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        s.net.FailConnectBegin(true);
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Failed);
        CHECK_EQ(res, R_NOT_FOUND);
        CHECK_EQ(s.lobby.client_state(), 1);
        CHECK_EQ(s.net.DisconnectCalls(), 0);
        CHECK(!s.lobby.join_active());
        CHECK(s.lobby.ClientJoinPoll(&res));  // no join, no outcome
        CHECK_EQ(res, R_INVALID_STATE);
    }
    {  // ConnectPoll -1 at the first round: kNotFound well before 3000 ms; Disconnect once; state 1
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        s.net.ConnectResult(-1);
        u32 res = 0;
        const u64 t0 = r.air.now();
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK_EQ(s.lobby.ClientStatusWord() >> 24, 1);  // Joining shows as 1
        CHECK_EQ(s.lobby.client_state(), 3);
        const int rx_before = s.net.ReceiveCalls();
        r.air.Advance(20);
        bool done = s.lobby.ClientJoinPoll(&res);
        CHECK(done);
        CHECK_EQ(res, R_NOT_FOUND);
        CHECK(r.air.now() - t0 < 3000);
        CHECK_EQ(s.lobby.client_state(), 1);
        CHECK_EQ(s.net.DisconnectCalls(), 1);
        // the network is released in that same poll: nothing is received from it afterwards (and nothing is counted as a bad message)
        CHECK_EQ(s.net.ReceiveCalls() - rx_before, 0);
        CHECK_EQ(s.lobby.bad_messages(), 0);
        CHECK(s.lobby.ClientJoinPoll(&res));  // the outcome is handed over once
        CHECK_EQ(res, R_INVALID_STATE);
    }
    {  // a full host and a closed host: the engine stays silent, the outcome is kTimeout at exactly 3000 ms
        for (int variant = 0; variant < 2; ++variant) {
            Rig r(3);
            OpenHost(r[0], 1);  // max 1 client
            Con& a = r[1];
            Con& b = r[2];
            CHECK(Enter(r, 1, 0));  // the one place is taken
            b.lobby.ClientInitialize(nullptr, 16);
            b.lobby.ClientStartScan(0);
            if (variant == 0) {
                r.Run(60);
                CHECK(Sees(b, MacOf(r[0])));  // full but accepting = 1: still announced
            } else {
                // the closed host: the entry was heard while open, then the host stops accepting
                r[0].lobby.ServerEndAccepting();
                a.lobby.ClientStopSession();
                r.Run(60);
            }
            if (variant == 1) {
                // b missed the open phase: put the entry in its table from a synthetic copy of the announcement
                r.air.AddAir(MacOf(r[0]), Ann(0x0010C000, "Host", 2, 1), 1);
                r.Run(60);
                CHECK(Sees(b, MacOf(r[0])));
            }
            b.lobby.ClientStopScan();
            u32 res = 0;
            CHECK(b.lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
            r.air.Advance(2999);
            CHECK(!b.lobby.ClientJoinPoll(&res));
            CHECK(b.lobby.join_active());
            CHECK_EQ(b.net.DisconnectCalls(), 0);
            r.air.Advance(1);
            CHECK(b.lobby.ClientJoinPoll(&res));
            CHECK_EQ(res, R_TIMEOUT);
            CHECK_EQ(b.lobby.client_state(), 1);
            CHECK_EQ(b.net.DisconnectCalls(), 1);
            CHECK_EQ(b.lobby.ClientStatusWord() >> 24, 1);
        }
    }
    {  // a host that answers late: 2999 ms still joins, 3001 ms is too late; at exactly 3000 ms the engine's answer wins over the deadline
        for (int variant = 0; variant < 3; ++variant) {
            Rig r(2);
            OpenHost(r[0], 3);
            Con& s = r[1];
            s.lobby.ClientInitialize(nullptr, 16);
            s.lobby.ClientStartScan(0);
            r.Run(60);
            s.lobby.ClientStopScan();
            s.net.ConnectDelay(variant == 0 ? 2990 : variant == 1 ? 3001 : 3000);
            u32 res = 0;
            CHECK(s.lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
            r.air.Advance(variant == 2 ? 3000 : 2995);
            const bool done = s.lobby.ClientJoinPoll(&res);
            if (variant != 1) {
                CHECK(done);
                CHECK_EQ(res, 0);
                CHECK_EQ(s.lobby.client_state(), 5);
            } else {
                CHECK(!done);
                r.air.Advance(5);  // 3000
                CHECK(s.lobby.ClientJoinPoll(&res));
                CHECK_EQ(res, R_TIMEOUT);
                r.air.Advance(10);  // the engine would have admitted us at 3001: too late, we left
                r.Run(100);
                CHECK_EQ(r.air.Present(0), 1);
            }
        }
    }
    {  // a ConnectPoll that takes 2900 ms: the end is within 3000 ms + one round
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        s.net.ConnectPollCost(2900);
        u32 res = 0;
        const u64 t0 = r.air.now();
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        bool done = false;
        for (int i = 0; i < 20 && !done; ++i) {
            r.air.Advance(20);
            done = s.lobby.ClientJoinPoll(&res);
        }
        CHECK(done);
        CHECK_EQ(res, R_TIMEOUT);
        CHECK(r.air.now() - t0 <= 3000 + 2900 + 40);
        CHECK_EQ(s.net.DisconnectCalls(), 1);
    }
    {  // a join nobody asks about times out by itself, with ONE Disconnect; a new JoinBegin then works
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        r.Run(10000, 50);
        CHECK_EQ(s.lobby.client_state(), 1);
        CHECK_EQ(s.net.DisconnectCalls(), 1);
        CHECK(!s.lobby.join_active());
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);  // the old outcome is dropped
        CHECK(!s.lobby.ClientJoinPoll(&res));
    }
    {  // cancelled in the middle: StopSession, Finalize, Initialize, OnGameUdsStarting
        for (int how = 0; how < 4; ++how) {
            Rig r(2);
            SilentJoinSetup(r);
            Con& s = r[1];
            u32 res = 0;
            CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
            r.Run(500);
            CHECK(s.lobby.join_active());
            if (how == 0) CHECK_EQ(s.lobby.ClientStopSession(), 0);
            if (how == 1) CHECK_EQ(s.lobby.ClientFinalize(), 0);
            if (how == 2) CHECK_EQ(s.lobby.ClientInitialize(nullptr, 16), 0);
            if (how == 3) CHECK(s.lobby.OnGameUdsStarting());
            CHECK_EQ(s.lobby.client_state(), 1);
            CHECK(!s.lobby.join_active());
            CHECK_EQ(s.net.DisconnectCalls(), 1);
            CHECK(s.lobby.ClientJoinPoll(&res));
            CHECK_EQ(res, R_INVALID_STATE);
            r.Run(4000);
            CHECK_EQ(s.net.DisconnectCalls(), 1);  // and no second one from a timeout
            CHECK_EQ(s.lobby.client_state(), 1);
        }
    }
    {  // a second JoinBegin during one in progress fails and leaves the first alone
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Failed);
        CHECK_EQ(res, R_INVALID_STATE);
        CHECK(s.lobby.join_active());
        CHECK_EQ(s.net.ConnectBeginCalls(), 1);
        CHECK_EQ(s.lobby.ClientStartScan(0), R_INVALID_STATE);  // also not while joining
    }
    {  // JoinBegin: unknown MAC -> kNoData (before anything else on the network); state 0 -> kInvalidState
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(9), &res) == JoinStep::Failed);
        CHECK_EQ(res, R_NO_DATA);
        CHECK_EQ(s.net.ConnectBeginCalls(), 0);
        Con& z = r[0];  // never initialised
        CHECK(z.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Failed);
        CHECK_EQ(res, R_INVALID_STATE);
    }
    {  // the child title the command carries is not even a parameter; the MAC is all that counts (Azahar's behaviour)
        Rig r(2);
        OpenHost(r[0], 3, false, 2);  // child index 2
        CHECK(Enter(r, 1, 0, 0x000400010010C000ull));  // the game scans with child index 0 and still gets in
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. The session of the searcher.

static void TestSession() {
    std::printf("searcher session\n");
    {  // the whole round with 3 clients: values by hand
        Rig r(4);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        // joined: 5 at once; connected = 1, node 2 (the lowest free)
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x05010002u);
        CHECK(Enter(r, 2, 0));
        CHECK_EQ(r[2].lobby.ClientStatusWord(), 0x05010003u);
        CHECK(Enter(r, 3, 0));
        CHECK_EQ(r[3].lobby.ClientStatusWord(), 0x05010004u);
        r.Run(400);
        // the host sees 3 clients, ascending, host excluded
        u16 ids[4] = {};
        u32 total = 0;
        CHECK_EQ(r[0].lobby.ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 3);
        CHECK_EQ(ids[0], 2);
        CHECK_EQ(ids[1], 3);
        CHECK_EQ(ids[2], 4);
        NodeEntry e;
        CHECK_EQ(r[0].lobby.ServerClientInfo(2, &e), 0);
        CHECK_EQ(e.node, 2);
        CHECK_EQ(e.seed, 0x1001);
        CHECK_STR(NameOf(e.name), "Cli1");
        CHECK_EQ(r[0].lobby.ServerClientInfo(4, &e), 0);
        CHECK_STR(NameOf(e.name), "Cli3");
        CHECK_EQ(e.seed, 0x1003);
        // the host as a client: kNoData with the host's identity in `out`
        CHECK_EQ(r[0].lobby.ServerClientInfo(1, &e), R_NO_DATA);
        CHECK_EQ(e.node, 1);
        CHECK_EQ(e.seed, 0x1000);
        CHECK_STR(NameOf(e.name), "Host");
        CHECK_EQ(r[0].lobby.ServerClientInfo(7, &e), R_NO_DATA);
        CHECK_EQ(e.node, 0);
        // a seeker sees all 4 players in its roster, host first
        u32 st = 0;
        CHECK_EQ(r[1].lobby.ClientConnectingNodes(ids, 4, &st), 0);
        CHECK_EQ(st, 4);
        CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 4);
        CHECK_EQ(r[1].lobby.ClientNodeInfo(1, &e), 0);
        CHECK_STR(NameOf(e.name), "Host");
        CHECK_EQ(e.seed, 0x1000);
        CHECK_EQ(r[1].lobby.ClientNodeInfo(3, &e), 0);
        CHECK_STR(NameOf(e.name), "Cli2");
        CHECK_EQ(r[1].lobby.ClientNodeInfo(0x0102, &e), R_NO_DATA);  // not node 2
        CHECK_EQ(r[1].lobby.ClientNodeInfo(0x0004, &e), 0);
        CHECK_EQ(r[1].lobby.ClientNodeInfo(0, &e), R_NO_DATA);
        CHECK_EQ(r[1].lobby.ClientNodeInfo(5, &e), R_NO_DATA);
        // capacity smaller than the total: writes capacity ids, reports the total
        u16 two[4] = {0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA};
        CHECK_EQ(r[1].lobby.ClientConnectingNodes(two, 2, &st), 0);
        CHECK_EQ(st, 4);
        CHECK(two[0] == 1 && two[1] == 2 && two[2] == 0xAAAA);
        // the host's entry in the session
        GroupView v;
        CHECK_EQ(r[1].lobby.ClientServerInfo(MacOf(r[0]), &v), 0);
        CHECK_STR(NameOf(v.group->name), "Host");
        CHECK_EQ(v.group->max_nodes, 4);
        CHECK_EQ(v.group->total_nodes, 4);
        // the distribution
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        CHECK_EQ(r[0].lobby.ServerState(), 6);
        r.Run(100);
        CHECK_EQ(CSt(r[1]), 6);
        CHECK_EQ(CSt(r[3]), 6);
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x06010002u);
        CHECK_EQ(SSt(r[0]), 8);
        u32 w = 0;
        CHECK_EQ(r[0].lobby.ServerClientState(3, &w), 0);
        CHECK_EQ(w, 0x07010003u);  // START_ACK received
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        r.Run(100);
        CHECK_EQ(SSt(r[0]), 9);
        CHECK_EQ(r[0].net.HostCloseCalls(), 1);
        CHECK_EQ(NClients(r[0]), 0);
        for (int i = 1; i <= 3; ++i) {
            CHECK_EQ(r[i].lobby.ClientStatusWord(), 0x09010000u + static_cast<u32>(i + 1));
            CHECK_STR(PassOf(r[i]), kPass1Hex);
        }
        CHECK_EQ(r.air.Present(0), 0);
        // the bytes that travelled, by hand: type, length of the body, body
        CHECK_STR(r[0].net.LastMsg(T_START), std::string("\x01\x00", 2));
        CHECK_STR(r[1].net.LastMsg(T_START_ACK), std::string("\x02\x00", 2));
        CHECK_STR(r[0].net.LastMsg(T_GO), std::string("\x03\x09" "abcd1234\0", 11));
        CHECK_STR(r[1].net.LastMsg(T_GO_ACK), std::string("\x04\x00", 2));
    }
    {  // START duplicated 10 times: ten START_ACK, one change of state
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Dup(T_START, 9);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(60);
        CHECK_EQ(CSt(r[1]), 6);
        CHECK_EQ(r[1].net.Sent(T_START_ACK, 1), 10);
        CHECK_EQ(r[1].lobby.bad_messages(), 0);
    }
    {  // START from a node other than 1 is ignored; GO twice keeps the FIRST passphrase and answers both
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        const u8 start[2] = {1, 0};
        r.air.Inject(1, 3, start, 2);  // "from node 3": a peer, not the host
        r.Run(40);
        CHECK_EQ(CSt(r[1]), 5);
        CHECK_EQ(r[1].lobby.bad_messages(), 1);
        CHECK_EQ(r[1].net.SentTotal(T_START_ACK), 0);
        // GO without START is valid (the START may have been lost): 5 -> 9 straight, GO_ACK sent
        u8 go[11] = {3, 9, 'a', 'b', 'c', 'd', '1', '2', '3', '4', 0};
        r.air.Inject(1, 1, go, 11);
        r.Run(40);
        CHECK_EQ(CSt(r[1]), 9);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_EQ(r[1].net.Sent(T_GO_ACK, 1), 1);
        // a second GO with other bytes: the passphrase stays, the answer is repeated
        u8 go2[11] = {3, 9, 'z', 'y', 'x', 'w', '9', '8', '7', '6', 0};
        r.air.Inject(1, 1, go2, 11);
        r.Run(40);
        CHECK_EQ(CSt(r[1]), 9);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_EQ(r[1].net.Sent(T_GO_ACK, 1), 2);
        // a late START in 9 is ignored apart from the ack
        r.air.Inject(1, 1, start, 2);
        r.Run(40);
        CHECK_EQ(CSt(r[1]), 9);
        CHECK_EQ(r[1].net.Sent(T_START_ACK, 1), 1);
        CHECK_EQ(r[1].lobby.bad_messages(), 1);
        // the other client has no passphrase yet
        CHECK_STR(PassOf(r[2]), "000000000000000000");
    }
    {  // 9 is final: the host closes right after the GO_ACKs; 60 s later still 9 with the same passphrase and roster
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        r.Run(300);
        CHECK(Finish(r, 0));
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x09010002u);
        const u32 nodes_before = NNodes(r[1]);
        CHECK_EQ(nodes_before, 3);
        r.Run(60000, 100);
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x09010002u);
        CHECK_EQ(r[2].lobby.ClientStatusWord(), 0x09010003u);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_EQ(NNodes(r[1]), 3);
        NodeEntry e;
        CHECK_EQ(r[1].lobby.ClientNodeInfo(3, &e), 0);
        CHECK_STR(NameOf(e.name), "Cli2");
        // a kick of the host cannot make it worse either (not that the engine would deliver one)
        CHECK_EQ(r[1].lobby.client_state(), 9);
    }
    {  // the host vanishes in 5 and in 6: 0x80 at exactly kHostGoneMs (1999 no, 2000 yes); never in 9
        for (int where = 0; where < 2; ++where) {
            Rig r(2);
            OpenHost(r[0], 3);
            CHECK(Enter(r, 1, 0));
            if (where == 1) {
                r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);  // host stays in 6
                CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
                r.Run(60);
                CHECK_EQ(CSt(r[1]), 6);
            }
            Con& c = r[1];
            r.air.Flap(0, 1, 100000);  // node 1 is gone from Members, the client is still in the network
            c.lobby.Poll(true);       // it notices
            CHECK(c.lobby.ClientStatusWord() >> 24 == (where == 0 ? 5u : 6u));
            r.air.Advance(1999);
            c.lobby.Poll(true);
            CHECK(c.lobby.ClientStatusWord() >> 24 == (where == 0 ? 5u : 6u));
            CHECK_EQ(c.net.DisconnectCalls(), 0);
            r.air.Advance(1);
            c.lobby.Poll(true);
            CHECK_EQ(c.lobby.ClientStatusWord(), 0x80000000u);  // lost: connected 0, node 0
            CHECK_EQ(c.net.DisconnectCalls(), 1);
            CHECK_EQ(c.lobby.client_state(), 0x80);
            // 0x80 stays until StopSession / Finalize / Initialize
            r.air.Advance(60000);
            c.lobby.Poll(true);
            CHECK_EQ(c.lobby.ClientStatusWord(), 0x80000000u);
            CHECK_EQ(NNodes(c), 0);
            CHECK_EQ(c.lobby.ClientStatusWord() >> 24, 0x80);
            CHECK_EQ(c.lobby.ClientStopSession(), 0);
            CHECK_EQ(c.lobby.ClientStatusWord() >> 24, 1);
        }
        for (int how = 0; how < 2; ++how) {  // Finalize and Initialize also leave 0x80
            Rig r(2);
            OpenHost(r[0], 3);
            CHECK(Enter(r, 1, 0));
            r.air.Flap(0, 1, 100000);
            r[1].lobby.Poll(true);
            r.air.Advance(2000);
            r[1].lobby.Poll(true);
            CHECK_EQ(r[1].lobby.client_state(), 0x80);
            if (how == 0) r[1].lobby.ClientFinalize();
            if (how == 1) r[1].lobby.ClientInitialize(nullptr, 16);
            CHECK_EQ(r[1].lobby.ClientStatusWord() >> 24, 1);
        }
        {  // a host that comes back before kHostGoneMs costs nothing
            Rig r(2);
            OpenHost(r[0], 3);
            CHECK(Enter(r, 1, 0));
            r.air.Flap(0, 1, 1900);
            r.Run(1900);
            r.Run(4000);
            CHECK_EQ(CSt(r[1]), 5);
        }
        {  // never in 9: the host closes, the client had the passphrase
            Rig r(2);
            OpenHost(r[0], 3);
            CHECK(Enter(r, 1, 0));
            CHECK(Finish(r, 0));
            r.Run(10000, 100);
            CHECK_EQ(CSt(r[1]), 9);
        }
    }
    {  // the roster: a client that comes in later shows within kRosterMs + one round; a failing ReadNode keeps the count
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(NNodes(r[1]), 2);
        r[1].net.FailReadNode(3, true);  // console 1 cannot read node 3
        CHECK(Enter(r, 2, 0));
        r.Run(300);
        u16 ids[4] = {};
        u32 total = 0;
        r[1].lobby.ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 3);
        CHECK_EQ(ids[2], 3);
        NodeEntry e;
        CHECK_EQ(r[1].lobby.ClientNodeInfo(3, &e), 0);
        CHECK_EQ(e.seed, 0);
        CHECK_STR(NameOf(e.name), "");
        CHECK_EQ(e.node, 3);
        CHECK_EQ(r[1].lobby.ClientNodeInfo(2, &e), 0);  // itself, readable
        CHECK_STR(NameOf(e.name), "Cli1");
        // a client that leaves vanishes from the roster too
        r.air.Leave(0, 3);
        r.Run(300);
        CHECK_EQ(NNodes(r[1]), 2);
    }
    {  // the final reading at GO: the roster of a client that joined in the last 250 ms is up to date when it freezes
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        const u8 go[11] = {3, 9, 'a', 'b', 'c', 'd', '1', '2', '3', '4', 0};
        r.air.Inject(1, 1, go, 11);
        r[1].lobby.Poll(true);  // the roster was read before client 2 existed for it; GO reads it again
        CHECK_EQ(r[1].lobby.client_state(), 9);
        CHECK_EQ(NNodes(r[1]), 3);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. The creator.

static void TestCreator() {
    std::printf("creator\n");
    {  // StartAccepting: the channels and the ORDER of the checks
        Rig r(3);
        Con& h = r[0];
        // channel first: even in state 0
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 5), R_OUT_OF_RANGE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 2), R_OUT_OF_RANGE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 12), R_OUT_OF_RANGE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 0x100), R_OUT_OF_RANGE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 0x03a12006), R_OUT_OF_RANGE);  // the service passes the low byte only
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);          // state 0
        CHECK_EQ(h.net.HostOpenCalls(), 0);
        h.lobby.ServerInitialize(3, 0, nullptr);
        h.net.FailHostOpen(true);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_BUSY);  // HostOpen false
        CHECK_EQ(h.lobby.ServerState(), 1);
        h.net.FailHostOpen(false);
        const u32 good[4] = {0, 1, 6, 11};
        for (int k = 0; k < 4; ++k) {
            Rig r2(1);
            r2[0].lobby.ServerInitialize(3, 0, nullptr);
            CHECK_EQ(r2[0].lobby.ServerStartAccepting(false, good[k]), 0);
            CHECK_EQ(r2[0].net.LastChannel(), good[k]);
            CHECK_EQ(r2[0].lobby.ServerState(), 2);
        }
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);  // already 2
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 5), R_OUT_OF_RANGE);   // the channel check still comes first
        CHECK_EQ(h.net.HostOpenCalls(), 2);                                // (one failed, one opened)
        // the announcement: the child title id (game id & 0xFFFFFF00 | child index), name, max nodes, accepting
        CHECK_EQ(h.net.LastAnnounce().child_tid_low, 0x0010C000);
        CHECK_EQ(h.net.LastAnnounce().max_nodes, 4);
        CHECK_EQ(h.net.LastAnnounce().accepting, 1);
        CHECK_STR(NameOf(h.net.LastAnnounce().name), "Host");
    }
    {  // the searcher holds the network: StartAccepting is kBusy with no call at all into the network
        Rig r(2);
        Con& x = r[0];
        x.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(x.lobby.ClientStartScan(0), 0);
        x.lobby.ServerInitialize(3, 0, nullptr);
        const int calls = x.net.AllCalls();
        CHECK_EQ(x.lobby.ServerStartAccepting(false, 6), R_BUSY);
        CHECK_EQ(x.net.AllCalls(), calls);
        CHECK_EQ(x.lobby.ServerState(), 1);
    }
    {  // automatic and manual acceptance
        for (int manual = 0; manual < 2; ++manual) {
            Rig r(3);
            OpenHost(r[0], 3, manual != 0);
            CHECK(Enter(r, 1, 0));
            r.Run(60);
            u32 w = 0;
            CHECK_EQ(r[0].lobby.ServerClientState(2, &w), 0);
            if (manual == 0) {
                CHECK_EQ(w, 0x05010002u);
            } else {
                CHECK_EQ(w, 0x40000002u);  // waiting: joined 0
                CHECK_EQ(NClients(r[0]), 1);  // listed even before acceptance
                CHECK_EQ(r[0].lobby.ServerAcceptClient(3), R_NO_DATA);       // unknown
                CHECK_EQ(r[0].lobby.ServerAcceptClient(1), R_NO_DATA);       // the host
                CHECK_EQ(r[0].lobby.ServerAcceptClient(0x0102), R_NO_DATA);  // not node 2
                CHECK_EQ(r[0].lobby.ServerAcceptClient(2), 0);
                CHECK_EQ(r[0].lobby.ServerClientState(2, &w), 0);
                CHECK_EQ(w, 0x05010002u);
                CHECK_EQ(r[0].lobby.ServerAcceptClient(2), R_NO_DATA);  // twice
            }
        }
    }
    {  // GetClientState for ids that are not tracked: kNoData (also the host, 0, out of range)
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        u32 w = 0xFFFF;
        const u16 ids[8] = {0, 1, 3, 4, 5, 0xFF, 0x0102, 0xFFFF};
        for (int k = 0; k < 8; ++k) {
            CHECK_EQ(r[0].lobby.ServerClientState(ids[k], &w), R_NO_DATA);
        }
        CHECK_EQ(w, 0xFFFF);
        CHECK_EQ(r[0].lobby.ServerClientState(2, &w), 0);
    }
    {  // StartDistribution with a client still waiting: Kick, START only to the accepted
        Rig r(3);
        OpenHost(r[0], 3, true);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        r.Run(60);
        CHECK_EQ(r[0].lobby.ServerAcceptClient(3), 0);  // node 3 accepted, node 2 not
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        CHECK_EQ(r[0].net.Kicked(2), 1);
        CHECK_EQ(r[0].net.Kicked(3), 0);
        CHECK_EQ(r[0].net.Sent(T_START, 2), 0);
        CHECK_EQ(r[0].net.Sent(T_START, 3), 1);
        CHECK_EQ(r[0].net.LastAnnounce().accepting, 0);
        r.Run(100);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        CHECK_EQ(NClients(r[0]), 1);  // the kicked one is out of the list
        CHECK_EQ(r[1].lobby.ClientStatusWord() >> 24, 5u);  // (it only learns by itself that it was thrown out)
    }
    {  // zero clients: StartDistribution -> 8 at once, SendPassphrase -> 9 in the same call
        Rig r(1);
        OpenHost(r[0], 3);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(r[0].lobby.ServerState(), 9);
        CHECK_EQ(r[0].net.HostCloseCalls(), 1);
        CHECK_EQ(NClients(r[0]), 0);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass2), R_INVALID_STATE);
        CHECK(!r[0].lobby.active());
    }
}

static void TestCreator2() {
    std::printf("creator (2)\n");
    {  // StartDistribution / SendPassphrase in every state; Finalize in every state
        Rig r(2);
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerStartDistribution(), R_INVALID_STATE);  // 0
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);
        h.lobby.ServerInitialize(3, 0, nullptr);
        CHECK_EQ(h.lobby.ServerStartDistribution(), R_INVALID_STATE);  // 1
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);
        h.lobby.ServerStartAccepting(false, 6);
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);  // 2
        CHECK(Enter(r, 1, 0));
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0);
        CHECK_EQ(h.lobby.ServerState(), 6);
        CHECK_EQ(h.lobby.ServerStartDistribution(), R_INVALID_STATE);  // 6
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);
        r.Run(2000);  // the grace passes: 8
        CHECK_EQ(h.lobby.ServerState(), 8);
        CHECK_EQ(h.lobby.ServerStartDistribution(), R_INVALID_STATE);  // 8
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(h.lobby.ServerState(), 9);  // (the client was thrown out: nobody to wait for)
        CHECK_EQ(h.lobby.ServerStartDistribution(), R_INVALID_STATE);  // 9
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass2), R_INVALID_STATE);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);
        CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
        CHECK_EQ(h.lobby.ServerState(), 9);
    }
    {  // Finalize in every state: always 0, state 0, the network closed once if it was open
        for (int st = 0; st < 5; ++st) {
            Rig r(2);
            Con& h = r[0];
            bool open = false;
            if (st >= 1) h.lobby.ServerInitialize(3, 0, nullptr);
            if (st >= 2) {
                h.lobby.ServerStartAccepting(false, 6);
                open = true;
            }
            if (st == 3) {
                CHECK(Enter(r, 1, 0));
                r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
                h.lobby.ServerStartDistribution();
            }
            if (st == 4) {
                h.lobby.ServerStartDistribution();  // no client: 8
            }
            CHECK_EQ(h.lobby.ServerFinalize(), 0);
            CHECK_EQ(h.lobby.ServerState(), 0);
            CHECK_EQ(h.net.HostCloseCalls(), open ? 1 : 0);
            CHECK(!h.lobby.active());
            CHECK_EQ(h.lobby.ServerFinalize(), 0);  // again
            CHECK_EQ(h.net.HostCloseCalls(), open ? 1 : 0);
            u16 ids[4];
            u32 total = 0;
            CHECK_EQ(h.lobby.ServerConnectingClients(ids, 4, &total), R_INVALID_STATE);
        }
        {  // after the round (9) too: HostClose already happened, Finalize does not repeat it
            Rig r(1);
            OpenHost(r[0], 3);
            r[0].lobby.ServerStartDistribution();
            r[0].lobby.ServerSendPassphrase(kPass1);
            CHECK_EQ(r[0].net.HostCloseCalls(), 1);
            CHECK_EQ(r[0].lobby.ServerFinalize(), 0);
            CHECK_EQ(r[0].net.HostCloseCalls(), 1);
            CHECK_EQ(r[0].lobby.ServerState(), 0);
        }
    }
    {  // 60 calls of SendPassphrase with different bytes while the GO_ACKs are in flight: ONE GO per client, the first
        // passphrase for everybody, the others kInvalidState
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        r.Run(60);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(100);
        CHECK_EQ(SSt(r[0]), 8);
        r.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        u8 pass[9];
        int ok = 0, refused = 0;
        for (int i = 0; i < 60; ++i) {
            for (int k = 0; k < 8; ++k) pass[k] = static_cast<u8>('a' + (i + k) % 20);
            pass[8] = 0;
            const u32 rc = r[0].lobby.ServerSendPassphrase(pass);
            if (rc == 0) {
                ++ok;
            } else if (rc == R_INVALID_STATE) {
                ++refused;
            }
        }
        CHECK_EQ(ok, 1);
        CHECK_EQ(refused, 59);
        CHECK_EQ(r[0].net.Sent(T_GO, 2), 1);
        CHECK_EQ(r[0].net.Sent(T_GO, 3), 1);
        r.Run(100);
        CHECK_EQ(CSt(r[1]), 9);
        CHECK_EQ(CSt(r[2]), 9);
        CHECK_STR(PassOf(r[1]), PassOf(r[2]));  // the same for both: the first generated
        CHECK_STR(PassOf(r[1]), "616263646566676800");  // "abcdefgh": the bytes of the FIRST call
    }
    {  // EndAccepting then StartAccepting: HostOpen once, HostUpdate twice; the clients inside stay
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(r[0].lobby.ServerEndAccepting(), 0);
        CHECK_EQ(r[0].lobby.ServerState(), 1);
        CHECK_EQ(r[0].net.LastAnnounce().accepting, 0);
        CHECK_EQ(NClients(r[0]), 1);
        CHECK_EQ(r[0].net.HostCloseCalls(), 0);
        CHECK_EQ(r[0].net.Kicked(2), 0);
        r.Run(200);
        CHECK_EQ(NClients(r[0]), 1);  // still tracked in state 1
        CHECK_EQ(r[0].lobby.ServerEndAccepting(), 0);  // again: nothing
        CHECK_EQ(r[0].net.HostUpdateCalls(), 1);
        CHECK_EQ(r[0].lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(r[0].net.HostOpenCalls(), 1);
        CHECK_EQ(r[0].net.HostUpdateCalls(), 2);
        CHECK_EQ(r[0].lobby.ServerState(), 2);
        CHECK_EQ(r[0].net.Announces(), 3);
        CHECK_EQ(r[0].net.AcceptingLog().size(), 3);
        CHECK(r[0].net.AcceptingLog()[0] == 1 && r[0].net.AcceptingLog()[1] == 0 && r[0].net.AcceptingLog()[2] == 1);
        // EndAccepting is never fatal in the other states
        Rig q(1);
        CHECK_EQ(q[0].lobby.ServerEndAccepting(), 0);
        q[0].lobby.ServerInitialize(3, 0, nullptr);
        CHECK_EQ(q[0].lobby.ServerEndAccepting(), 0);
        CHECK_EQ(q[0].lobby.ServerState(), 1);
        CHECK_EQ(q[0].net.HostUpdateCalls(), 0);
    }
    {  // a host that is no longer accepting is invisible to a searcher that was not there before
        Rig r(3);
        OpenHost(r[0], 3);
        r[0].lobby.ServerEndAccepting();
        Con& s = r[1];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(100);
        CHECK(!Sees(s, MacOf(r[0])));
        r[0].lobby.ServerStartAccepting(false, 6);
        r.Run(100);
        CHECK(Sees(s, MacOf(r[0])));
    }
}

static void TestCreator3() {
    std::printf("creator (3)\n");
    {  // DisconnectClient: Kick, the slot is "ejected" and does not come back while the node lingers in Members()
        Rig r(3);
        r.air.SetKickLag(100);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(NClients(r[0]), 1);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(5), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(1), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(0x0102), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(3), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(2), 0);
        CHECK_EQ(r[0].net.Kicked(2), 1);
        CHECK_EQ(NClients(r[0]), 0);  // out of the list at once
        CHECK_EQ(r.air.Present(0), 2);  // ... though the engine still has it for 100 ms
        r.air.Advance(20);
        r[0].lobby.Poll(true);
        CHECK_EQ(NClients(r[0]), 0);  // no new client is born from the lingering node
        CHECK_EQ(r[0].net.Kicked(2), 1);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(2), R_NO_DATA);  // twice
        r.Run(200);
        CHECK_EQ(r.air.Present(0), 1);
        CHECK_EQ(NClients(r[0]), 0);
        CHECK_EQ(CSt(r[1]), 5);  // ... the thrown-out client does not hear it as a state change by itself: its host is still there
        // (its engine says it left the network: MyNode() 0 -> 0x80 after kHostGoneMs)
        r.Run(2100);
        CHECK_EQ(CSt(r[1]), 0x80);
        // a node that leaves and comes back is a new client
        Rig q(3);
        OpenHost(q[0], 3);
        CHECK(Enter(q, 1, 0));
        q.air.Leave(0, 2);
        q.Run(100);
        CHECK_EQ(NClients(q[0]), 0);
        CHECK(Enter(q, 2, 0));
        CHECK_EQ(NClients(q[0]), 1);
    }
    {  // a client that leaves in phase 6 after the other confirmed: 8 in the same poll; same in the GO phase -> 9
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        r.Run(60);
        r.air.Drop(T_START_ACK, 3, FakeAir::kAny, FakeAir::kForever);  // node 3 never confirms
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(200);
        CHECK_EQ(SSt(r[0]), 6);
        r.air.Leave(0, 3);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        r.air.ClearRules();
        r.air.Drop(T_GO_ACK, 2, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        r.Run(60);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        r.air.Leave(0, 2);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 9);
    }
    {  // a node beyond the limit, or an id outside 1..4, is kicked and never stored; one that appears in phase 6 too
        Rig r(2);
        OpenHost(r[0], 2);  // max_clients 2: nodes 2 and 3 are allowed
        r.air.SetKickLag(100000);  // the nodes linger in Members() for as long as the test looks
        r.air.Advance(1000);
        r.air.Phantom(0, 4, 0x77, "Extra");  // the engine let a third client in (it cannot, by construction)
        r.air.Phantom(0, 7, 0x78, "Odd");    // an id outside 1..4
        r.Run(100);
        CHECK(r[0].net.Kicked(4) >= 1);
        CHECK(r[0].net.Kicked(7) >= 1);
        CHECK_EQ(NClients(r[0]), 0);
        NodeEntry e;
        CHECK_EQ(r[0].lobby.ServerClientInfo(4, &e), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerClientInfo(7, &e), R_NO_DATA);
        r.Run(1000);
        CHECK(r[0].net.Kicked(4) <= 8);  // repeated every 200 ms while it lingers, not at every poll ...
        CHECK(r[0].net.Kicked(7) <= 8);
        CHECK(r[0].net.Kicked(4) >= 3);  // ... but repeated (about 5 in 1100 ms)
        CHECK(r[0].net.Kicked(7) >= 3);
        // a legal node showing up after the distribution started is kicked
        Rig q(2);
        OpenHost(q[0], 3);
        q[0].lobby.ServerStartDistribution();
        q.air.Phantom(0, 3, 0x79, "Late");
        q.Run(100);
        CHECK(q[0].net.Kicked(3) >= 1);
        CHECK_EQ(NClients(q[0]), 0);
        // ... and in state 1 after EndAccepting
        Rig p(2);
        OpenHost(p[0], 3);
        p[0].lobby.ServerEndAccepting();
        p.air.Phantom(0, 2, 0x7A, "Late");
        p.Run(100);
        CHECK(p[0].net.Kicked(2) >= 1);
        CHECK_EQ(NClients(p[0]), 0);
    }
    {  // our own network lost (node 1 missing from Members for kHostGoneMs): back to 1, HostClose, table cleared
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Flap(0, 1, 100000);
        r[0].lobby.Poll(true);
        r.air.Advance(1999);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 2);
        CHECK_EQ(r[0].net.HostCloseCalls(), 0);
        r.air.Advance(1);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 1);
        CHECK_EQ(r[0].net.HostCloseCalls(), 1);
        CHECK_EQ(NClients(r[0]), 0);
        CHECK(!r[0].lobby.active());
    }
    {  // a failing ReadNode on the host does not hide a client
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r[0].net.FailReadNode(2, true);
        NodeEntry e;
        CHECK_EQ(r[0].lobby.ServerClientInfo(2, &e), 0);
        CHECK_EQ(e.node, 2);
        CHECK_EQ(e.seed, 0);
        CHECK_STR(NameOf(e.name), "");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. Time.

static void TestTimes() {
    std::printf("times\n");
    {  // kStartGraceMs at 1499 / 1500, and START resent every 200 ms (8 sends by 1499)
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        const u64 t0 = r.air.now();
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        CHECK_EQ(r[0].net.Sent(T_START, 2), 1);
        for (int ms = 1; ms <= 1499; ++ms) {
            r.air.Advance(1);
            r[0].lobby.Poll(true);
            if (ms == 199) CHECK_EQ(r[0].net.Sent(T_START, 2), 1);
            if (ms == 200) CHECK_EQ(r[0].net.Sent(T_START, 2), 2);
            if (ms == 399) CHECK_EQ(r[0].net.Sent(T_START, 2), 2);
            if (ms == 400) CHECK_EQ(r[0].net.Sent(T_START, 2), 3);
        }
        CHECK_EQ(r.air.now() - t0, 1499);
        CHECK_EQ(r[0].lobby.ServerState(), 6);
        CHECK_EQ(r[0].net.Kicked(2), 0);
        CHECK_EQ(r[0].net.Sent(T_START, 2), 8);
        r.air.Advance(1);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        CHECK_EQ(r[0].net.Kicked(2), 1);
        CHECK_EQ(r[0].net.Sent(T_START, 2), 8);
    }
    {  // kGoGraceMs: the GO_ACK never comes: 9 at exactly 2000 ms from the passphrase, the client has it
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(100);
        CHECK_EQ(SSt(r[0]), 8);
        r.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        r[1].lobby.Poll(true);  // the client takes the GO
        CHECK_EQ(r[1].lobby.client_state(), 9);
        r.air.Advance(1999);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 8);
        CHECK_EQ(r[0].net.HostCloseCalls(), 0);
        r.air.Advance(1);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.ServerState(), 9);
        CHECK_EQ(r[0].net.HostCloseCalls(), 1);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_EQ(NClients(r[0]), 0);
    }
    {  // GO resent every 200 ms
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        r.air.Drop(T_GO, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        for (int ms = 1; ms < 1000; ++ms) {
            r.air.Advance(1);
            r[0].lobby.Poll(true);
        }
        CHECK_EQ(r[0].net.Sent(T_GO, 2), 5);  // at 0, 200, 400, 600, 800
        CHECK_EQ(r[0].lobby.ServerState(), 8);
    }
}

static void TestTimes2() {
    std::printf("times (2)\n");
    {  // Pump is rate limited: a game that asks status and server info at 60 Hz costs <= 50 polls per second
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        const int m0 = r[0].net.MembersCalls();
        const int rx0 = r[0].net.ReceiveCalls();
        const int m1 = r[1].net.MembersCalls();
        for (int i = 0; i < 60; ++i) {  // 960 ms at 60 Hz (16 ms frames), every command pumps first
            r.air.Advance(16);
            (void)SSt(r[0]);
            (void)NClients(r[0]);
            (void)CSt(r[1]);
            (void)NNodes(r[1]);
        }
        CHECK(r[0].net.MembersCalls() - m0 <= 50);
        CHECK(r[0].net.ReceiveCalls() - rx0 <= 50);
        CHECK(r[1].net.MembersCalls() - m1 <= 50);
        CHECK(r[0].net.MembersCalls() - m0 >= 25);  // ... but it does run (16 ms frames: a poll every 32 ms)
        // a scanning game at 60 Hz
        Rig s(2);
        s.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        s[1].lobby.ClientInitialize(nullptr, 16);
        s[1].lobby.ClientStartScan(0);
        const int r0 = s[1].net.ScanReadCalls();
        const int rx = s[1].net.ReceiveCalls();
        for (int i = 0; i < 60; ++i) {
            s.air.Advance(16);
            s[1].lobby.Pump();
            GroupView v;
            bool first = true;
            while (s[1].lobby.ClientTitleInfoInOrder(first, &v) == 0) {
                first = false;
                s[1].lobby.ClientServerInfo(v.group->host, &v);
            }
        }
        CHECK(s[1].net.ReceiveCalls() - rx <= 50);
        CHECK(s[1].net.ScanReadCalls() - r0 <= 50 * 2);  // (one entry: two reads per poll)
    }
    {  // 3 minutes of status polling at 60 Hz in 5, 6 and 9 with a live client: the only sends are the resends
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        auto minutes = [&](int frames) {
            for (int i = 0; i < frames; ++i) {
                r.air.Advance(16);
                (void)CSt(r[1]);
                (void)SSt(r[0]);
                (void)NNodes(r[1]);
                (void)NClients(r[0]);
            }
        };
        const int sends0 = r[0].net.SendCalls() + r[1].net.SendCalls();
        minutes(11250);  // 3 minutes in 5
        CHECK_EQ(r[0].net.SendCalls() + r[1].net.SendCalls(), sends0);
        CHECK_EQ(CSt(r[1]), 5);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        minutes(20);
        const int sends1 = r[0].net.SendCalls() + r[1].net.SendCalls();  // START + START_ACK
        CHECK_EQ(sends1 - sends0, 2);
        minutes(11250);  // 3 minutes in 6 / 8
        CHECK_EQ(r[0].net.SendCalls() + r[1].net.SendCalls(), sends1);
        CHECK_EQ(CSt(r[1]), 6);
        CHECK_EQ(SSt(r[0]), 8);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        minutes(20);
        const int sends2 = r[0].net.SendCalls() + r[1].net.SendCalls();  // + GO + GO_ACK
        CHECK_EQ(sends2 - sends1, 2);
        minutes(11250);  // 3 minutes in 9
        CHECK_EQ(r[0].net.SendCalls() + r[1].net.SendCalls(), sends2);
        CHECK_EQ(CSt(r[1]), 9);
        CHECK_EQ(SSt(r[0]), 9);
    }
}

static void TestTimes3() {
    std::printf("times (3)\n");
    {  // 10 s with nothing but Pump every 50 ms (no IPC): the join completes, START and GO are delivered
        Rig r(2);
        OpenHost(r[0], 3);
        Con& s = r[1];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(100, 50);
        s.lobby.ClientStopScan();
        u32 res = 99;
        CHECK(s.lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
        r.Run(5000, 50);
        CHECK_EQ(s.lobby.client_state(), 5);  // joined without anybody asking
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(2000, 50);
        CHECK_EQ(s.lobby.client_state(), 6);
        CHECK_EQ(r[0].lobby.server_state(), 8);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        r.Run(3000, 50);
        CHECK_EQ(s.lobby.client_state(), 9);
        CHECK_EQ(r[0].lobby.server_state(), 9);
    }
    {  // a jump back of 5 s in the middle of a join: no spurious timeout
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        const u64 t0 = r.air.now();
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        r.air.Advance(1000);
        s.lobby.Poll(true);
        r.air.Set(t0 + 1000 - 5000);  // the clock goes back 5 s
        s.lobby.Poll(true);
        CHECK(s.lobby.join_active());
        r.air.Advance(2900);
        s.lobby.Poll(true);
        CHECK(s.lobby.join_active());  // (a spurious timeout would have come right after the jump)
        r.air.Set(t0 + 3000);
        s.lobby.Poll(true);
        CHECK(!s.lobby.join_active());
        CHECK(s.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_TIMEOUT);
        CHECK_EQ(s.net.DisconnectCalls(), 1);
    }
    {  // a jump forward of 60 s with three clients in 6: nobody is lost, at most one resend per client in that poll
        Rig r(4);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        CHECK(Enter(r, 3, 0));
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(100);
        CHECK_EQ(SSt(r[0]), 8);
        CHECK_EQ(CSt(r[1]), 6);
        r.air.Advance(60000);
        r.PollAll();
        CHECK_EQ(NClients(r[0]), 3);
        for (int i = 1; i <= 3; ++i) {
            CHECK_EQ(r[i].lobby.client_state(), 6);
        }
        CHECK_EQ(r[0].net.SentTotal(T_START), 3);  // no burst
        // and in the middle of a distribution where one confirmation is missing: one resend in that poll, no more
        Rig q(3);
        OpenHost(q[0], 3);
        CHECK(Enter(q, 1, 0));
        CHECK(Enter(q, 2, 0));
        q.air.Drop(T_START_ACK, 3, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(q[0].lobby.ServerStartDistribution(), 0);
        const int before = q[0].net.Sent(T_START, 3);
        q.air.Advance(1000);  // less than the grace
        q[0].lobby.Poll(true);
        CHECK_EQ(q[0].net.Sent(T_START, 3) - before, 1);
        q.air.Advance(1000);  // now past the grace: the silent client is removed, not resent to
        q[0].lobby.Poll(true);
        CHECK_EQ(q[0].lobby.ServerState(), 8);
        CHECK_EQ(q[0].net.Kicked(3), 1);
    }
    {  // a jump forward with a seeker in 5: the host is still there, no 0x80 and no loss
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Advance(60000);
        r.PollAll();
        CHECK_EQ(r[1].lobby.client_state(), 5);
        CHECK_EQ(NClients(r[0]), 1);
    }
}

/// A complete round with drops and several clients, as a text trace in time relative to the start: the same on every
/// absolute clock (start at 0xFFFFFF00 crosses the 32-bit wrap; above 2^32 the clock is a 64-bit number).
static std::string Trace(u64 start) {
    Rig r(4, start);
    std::string t;
    char b[96];
    auto rel = [&] { return static_cast<unsigned long long>(r.air.now() - start); };
    // a synthetic lobby seen and then lost: expiry at 1500 ms
    r.air.AddAir(AirMac(1), Ann(0x0010C000, "Ghost", 4, 1), 1);
    r[3].lobby.ClientInitialize(nullptr, 16);
    r[3].lobby.ClientStartScan(0);
    r[3].lobby.Poll(true);
    r.air.RemoveAir(0);
    int gone_at = -1;
    for (int ms = 0; ms < 2000 && gone_at < 0; ms += 1) {
        r.air.Advance(1);
        r[3].lobby.Poll(true);
        GroupView v;
        if (r[3].lobby.ClientServerInfo(AirMac(1), &v) != 0) gone_at = ms + 1;
    }
    std::snprintf(b, sizeof(b), "ttl=%d;", gone_at);
    t += b;
    r[3].lobby.ClientInitialize(nullptr, 16);  // clean
    OpenHost(r[0], 3);
    for (int i = 1; i <= 3; ++i) {
        const bool ok = Enter(r, i, 0);
        std::snprintf(b, sizeof(b), "join%d=%d@%llu;", i, ok ? 1 : 0, rel());
        t += b;
    }
    r.air.Drop(T_START_ACK, 3, FakeAir::kAny, 2);
    r.air.Drop(T_GO_ACK, 4, FakeAir::kAny, 1);
    std::snprintf(b, sizeof(b), "start=%u;", r[0].lobby.ServerStartDistribution());
    t += b;
    const u64 t1 = r.air.now();
    bool s8 = false, s9 = false;
    for (int ms = 0; ms < 6000 && !s9; ++ms) {
        r.air.Advance(1);
        r.PollAll();
        if (!s8 && r[0].lobby.ServerState() == 8) {
            s8 = true;
            std::snprintf(b, sizeof(b), "s8@%llu;", static_cast<unsigned long long>(r.air.now() - t1));
            t += b;
            r[0].lobby.ServerSendPassphrase(kPass1);
        }
        if (s8 && r[0].lobby.ServerState() == 9) {
            s9 = true;
            std::snprintf(b, sizeof(b), "s9@%llu;", static_cast<unsigned long long>(r.air.now() - t1));
            t += b;
        }
    }
    for (int i = 1; i <= 3; ++i) {
        std::snprintf(b, sizeof(b), "c%d=%08x,%s,start%d,go%d;", i, r[i].lobby.ClientStatusWord(), PassOf(r[i]).c_str(),
                      r[0].net.Sent(T_START, i + 1), r[0].net.Sent(T_GO, i + 1));
        t += b;
    }
    // the host lost for a seeker in 5: another round, 0x80 at exactly 2000 ms
    Rig q(2, start);
    OpenHost(q[0], 3);
    Enter(q, 1, 0);
    q.air.Flap(0, 1, 100000);
    q[1].lobby.Poll(true);
    const u64 t2 = q.air.now();
    int lost_at = -1;
    for (int ms = 1; ms < 4000 && lost_at < 0; ++ms) {
        q.air.Advance(1);
        q[1].lobby.Poll(true);
        if (q[1].lobby.client_state() == 0x80) lost_at = ms;
    }
    (void)t2;
    std::snprintf(b, sizeof(b), "lost=%d;", lost_at);
    t += b;
    // the grace: a client whose START_ACK never comes is thrown out at 1500 ms and the creator goes on to 8
    Rig g(2, start);
    OpenHost(g[0], 3);
    Enter(g, 1, 0);
    g.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
    g[0].lobby.ServerStartDistribution();
    int grace_at = -1;
    for (int ms = 1; ms < 3000 && grace_at < 0; ++ms) {
        g.air.Advance(1);
        g.PollAll();
        if (g[0].lobby.ServerState() == 8) grace_at = ms;
    }
    // the join against a lobby nobody runs: kTimeout at 3000 ms
    Rig j(2, start);
    SilentJoinSetup(j);
    u32 jres = 0;
    j[1].lobby.ClientJoinBegin(AirMac(1), &jres);
    int join_at = -1;
    for (int ms = 1; ms < 4000 && join_at < 0; ++ms) {
        j.air.Advance(1);
        j[1].lobby.Poll(true);
        if (!j[1].lobby.join_active()) join_at = ms;
    }
    // the roster: a client that comes in later shows in the first one's roster within kRosterMs
    Rig q3(3, start);
    OpenHost(q3[0], 3);
    Enter(q3, 1, 0);
    Enter(q3, 2, 0);
    int roster_at = -1;
    for (int ms = 0; ms < 1000 && roster_at < 0; ++ms) {
        q3.air.Advance(1);
        q3.PollAll();
        u16 ids[4];
        u32 total = 0;
        q3[1].lobby.ClientConnectingNodes(ids, 4, &total);
        if (total == 3) roster_at = ms;
    }
    std::snprintf(b, sizeof(b), "grace=%d;jt=%d;roster<250=%d;", grace_at, join_at, (roster_at >= 0 && roster_at <= 250) ? 1 : 0);
    t += b;
    return t;
}

static void TestClockOffsetsAndDeterminism() {
    std::printf("clock offsets, determinism\n");
    const std::string a = Trace(100000);
    const std::string again = Trace(100000);
    CHECK_STR(a, again);  // the same sequence, the same result
    std::printf("  trace: %s\n", a.c_str());
    CHECK(a.find("ttl=1500;") != std::string::npos);
    CHECK(a.find("lost=2000;") != std::string::npos);
    CHECK(a.find("grace=1500;") != std::string::npos);
    CHECK(a.find("jt=3000;") != std::string::npos);
    CHECK(a.find("roster<250=1;") != std::string::npos);
    CHECK(a.find("c1=09010002") != std::string::npos);
    CHECK(a.find("join1=1@") != std::string::npos);
    CHECK_STR(Trace(0xFFFFFF00ull), a);          // across the 32-bit wrap
    CHECK_STR(Trace(0xFFFFFFFFull - 20), a);     // just before it
    CHECK_STR(Trace(0x1'0000'0100ull), a);       // more than 2^32 ms
    CHECK_STR(Trace(0xFFFF'FFFF'FF00ull), a);    // a large 64-bit clock
}

// ---------------------------------------------------------------------------------------------------------------------
// 7. Loss, duplication, delay and reordering of the four messages.

struct Pair {
    Rig r;
    Pair() : r(3) {
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
    }
};

static void TestLoss() {
    std::printf("loss, duplication, delay\n");
    {  // START lost 1 time: arrives at the resend (200 ms); lost 3 times: at 600 ms; the client reaches 6
        for (int lost = 1; lost <= 3; lost += 2) {
            Pair p;
            Rig& r = p.r;
            r.air.Drop(T_START, FakeAir::kAny, FakeAir::kAny, lost);
            CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
            const u64 t0 = r.air.now();
            r.RunUntil([&] { return r[1].lobby.client_state() == 6; }, 3000, 1);
            CHECK_EQ(r[1].lobby.client_state(), 6);
            CHECK(r.air.now() - t0 >= static_cast<u64>(200 * lost));
            CHECK(r.air.now() - t0 < static_cast<u64>(330 * lost));
            r.Run(100);
            CHECK_EQ(r[0].lobby.server_state(), 8);
        }
    }
    {  // START lost for ever: the client is thrown out at 1500 ms, the host goes on to 8; the client ends in 0x80
        Pair p;
        Rig& r = p.r;
        r.air.Drop(T_START, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(1000);
        CHECK_EQ(r[1].lobby.client_state(), 5);
        CHECK_EQ(r[0].lobby.server_state(), 6);
        r.Run(600);
        CHECK_EQ(r[0].lobby.server_state(), 8);
        CHECK_EQ(r[0].net.Kicked(2), 1);
        r.Run(2200);
        CHECK_EQ(r[1].lobby.client_state(), 0x80);  // its engine dropped it: MyNode() 0 for kHostGoneMs
    }
    {  // START_ACK lost 3 times and then delivered: the client stays in 6, the host reaches 8 when one arrives
        Pair p;
        Rig& r = p.r;
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, 3);
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(500);
        CHECK_EQ(r[1].lobby.client_state(), 6);
        CHECK_EQ(r[0].lobby.server_state(), 6);
        r.Run(300);  // the resends at 200, 400, 600: the 4th ack (the 4th START arrives at 600) gets through
        CHECK_EQ(r[0].lobby.server_state(), 8);
        CHECK_EQ(r[1].net.Sent(T_START_ACK, 1), 4);
        CHECK_EQ(r[0].net.Kicked(2), 0);
    }
    {  // START_ACK lost for ever: the client is thrown out at 1500 ms (Kick), the host goes on
        Pair p;
        Rig& r = p.r;
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        const u64 t0 = r.air.now();
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.RunUntil([&] { return r[0].lobby.server_state() == 8; }, 3000, 1);
        CHECK(r.air.now() - t0 >= 1500 && r.air.now() - t0 <= 1520);  // (the poll runs every 20 ms)
        CHECK_EQ(r[0].net.Kicked(2), 1);
        CHECK_EQ(NClients(r[0]), 0);
    }
    {  // GO lost 1 / 3 times: delivered at a resend; the host reaches 9 only after the GO_ACK
        for (int lost = 1; lost <= 3; lost += 2) {
            Pair p;
            Rig& r = p.r;
            r[0].lobby.ServerStartDistribution();
            r.Run(100);
            r.air.Drop(T_GO, FakeAir::kAny, FakeAir::kAny, lost);
            CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
            const u64 t0 = r.air.now();
            r.RunUntil([&] { return r[0].lobby.server_state() == 9; }, 3000, 1);
            CHECK_EQ(r[0].lobby.server_state(), 9);
            CHECK(r.air.now() - t0 >= static_cast<u64>(200 * lost));
            CHECK(r.air.now() - t0 < static_cast<u64>(330 * lost));
            CHECK_STR(PassOf(r[1]), kPass1Hex);
        }
    }
    {  // GO lost for ever: 9 at kGoGraceMs; the client never had it and sees the host vanish: 0x80
        Pair p;
        Rig& r = p.r;
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        r.air.Drop(T_GO, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        const u64 t0 = r.air.now();
        r.RunUntil([&] { return r[0].lobby.server_state() == 9; }, 5000, 1);
        CHECK(r.air.now() - t0 >= 2000 && r.air.now() - t0 <= 2020);
        CHECK_EQ(r[1].lobby.client_state(), 6);
        r.Run(2100);
        CHECK_EQ(r[1].lobby.client_state(), 0x80);
        CHECK_STR(PassOf(r[1]), "000000000000000000");
    }
    {  // GO_ACK lost 1 / 3 times: the GO is resent, the client answers again (idempotent), the host reaches 9 on an ack
        for (int lost = 1; lost <= 3; lost += 2) {
            Pair p;
            Rig& r = p.r;
            r[0].lobby.ServerStartDistribution();
            r.Run(100);
            r.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, lost);
            CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
            r.RunUntil([&] { return r[0].lobby.server_state() == 9; }, 3000, 1);
            CHECK_EQ(r[0].lobby.server_state(), 9);
            CHECK_EQ(r[1].net.Sent(T_GO_ACK, 1), lost + 1);  // one per GO received
            CHECK_EQ(r[1].lobby.client_state(), 9);
        }
    }
    {  // GO_ACK lost for ever: the host reaches 9 at kGoGraceMs, the client has the passphrase
        Pair p;
        Rig& r = p.r;
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        r.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        r[0].lobby.ServerSendPassphrase(kPass1);
        const u64 t0 = r.air.now();
        r.RunUntil([&] { return r[0].lobby.server_state() == 9; }, 5000, 1);
        CHECK(r.air.now() - t0 >= 2000 && r.air.now() - t0 <= 2020);
        CHECK_EQ(r[1].lobby.client_state(), 9);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
    }
    {  // GO doubled by the network: two answers, one change
        Pair p;
        Rig& r = p.r;
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        r.air.Dup(T_GO, 1);
        r[0].lobby.ServerSendPassphrase(kPass1);
        r.Run(100);
        CHECK_EQ(r[1].lobby.client_state(), 9);
        CHECK_EQ(r[1].net.Sent(T_GO_ACK, 1), 2);
        CHECK_EQ(r[0].lobby.server_state(), 9);
    }
    {  // two START in swapped order do no harm; a delayed START arriving after the GO is answered and ignored
        Pair p;
        Rig& r = p.r;
        r.air.Delay(T_START, 250);  // the START is slow
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(100);
        CHECK_EQ(r[1].lobby.client_state(), 5);
        r.Run(300);
        CHECK_EQ(r[1].lobby.client_state(), 6);
        // the same message type reordered: Reorder swaps the next two
        Pair q;
        q.r.air.Reorder(T_START);
        q.r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, 1);
        q.r[0].lobby.ServerStartDistribution();
        q.r.Run(2000);
        CHECK_EQ(q.r[1].lobby.client_state(), 6);
        CHECK_EQ(q.r[0].lobby.server_state(), 8);
    }
    {  // an ack from a node that is not tracked is ignored (and counted); an ack out of its phase too
        Pair p;
        Rig& r = p.r;
        const u8 sack[2] = {2, 0};
        const u8 gack[2] = {4, 0};
        r.air.Inject(0, 3, sack, 2);  // "node 3" does not exist
        r.air.Inject(0, 4, gack, 2);
        r.air.Inject(0, 1, sack, 2);  // from the host itself
        r.air.Inject(0, 2, gack, 2);  // GO_ACK before any GO
        r.air.Inject(0, 2, sack, 2);  // START_ACK before any START (state 2)
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.bad_messages(), 5);
        CHECK_EQ(r[0].lobby.server_state(), 2);
        u32 w = 0;
        CHECK_EQ(r[0].lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w, 0x05010002u);
        // a message from the wrong peer: client 2 (node 3) "sends" GO and START to client 1
        Rig q(3);
        OpenHost(q[0], 3);
        CHECK(Enter(q, 1, 0));
        CHECK(Enter(q, 2, 0));
        const u8 go[11] = {3, 9, 'a', 'b', 'c', 'd', '1', '2', '3', '4', 0};
        const u8 start[2] = {1, 0};
        q.air.Inject(1, 3, go, 11);
        q.air.Inject(1, 3, start, 2);
        q[1].lobby.Poll(true);
        CHECK_EQ(q[1].lobby.client_state(), 5);
        CHECK_EQ(q[1].lobby.bad_messages(), 2);
        CHECK_STR(PassOf(q[1]), "000000000000000000");
    }
}

static void TestAdmissionTable() {
    std::printf("admission table\n");
    const u8 start[2] = {1, 0};
    const u8 sack[2] = {2, 0};
    const u8 go[11] = {3, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const u8 gack[2] = {4, 0};
    {  // a creator: GO and START are not for it; garbage is counted
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Inject(0, 2, start, 2);
        r.air.Inject(0, 2, go, 11);
        const u8 trunc[3] = {3, 9, 1};
        const u8 unk[2] = {9, 0};
        const u8 wrong_len[2] = {2, 3};
        r.air.Inject(0, 2, trunc, 3);
        r.air.Inject(0, 2, unk, 2);
        r.air.Inject(0, 2, wrong_len, 2);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.bad_messages(), 5);
        CHECK_EQ(r[0].lobby.server_state(), 2);
    }
    {  // an ack from a node that is not tracked changes nothing and is counted, in the phases where acks are listened to
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);  // the creator stays in 6
        CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
        r.Run(60);
        const u32 bad0 = r[0].lobby.bad_messages();
        r.air.Inject(0, 3, sack, 2);  // nodes 3 and 4: nobody there
        r.air.Inject(0, 4, sack, 2);
        r[0].lobby.Poll(true);
        CHECK_EQ(r[0].lobby.bad_messages() - bad0, 2);
        CHECK_EQ(r[0].lobby.server_state(), 6);
        Rig q(2);  // the GO phase: GO_ACK from nodes nobody holds
        OpenHost(q[0], 3);
        CHECK(Enter(q, 1, 0));
        CHECK_EQ(q[0].lobby.ServerStartDistribution(), 0);
        q.Run(100);
        q.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(q[0].lobby.ServerSendPassphrase(kPass1), 0);
        const u32 bad1 = q[0].lobby.bad_messages();
        q.air.Inject(0, 3, gack, 2);
        q.air.Inject(0, 4, gack, 2);
        q[0].lobby.Poll(true);
        CHECK_EQ(q[0].lobby.bad_messages() - bad1, 2);
        CHECK_EQ(q[0].lobby.server_state(), 8);
    }
    {  // a searcher in 2 and in Joining: START, GO and the acks are not for it
        Rig r(2);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "Ghost", 4, 1), 1);
        Con& s = r[1];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        r.air.Inject(1, 1, start, 2);
        r.air.Inject(1, 1, go, 11);
        r.air.Inject(1, 1, sack, 2);
        s.lobby.Poll(true);
        CHECK_EQ(s.lobby.bad_messages(), 3);
        CHECK_EQ(s.lobby.client_state(), 2);
        s.lobby.ClientStopScan();
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        r.air.Inject(1, 1, start, 2);
        r.air.Inject(1, 1, gack, 2);
        s.lobby.Poll(true);
        CHECK_EQ(s.lobby.bad_messages(), 5);
        CHECK_EQ(s.lobby.client_state(), 3);
        CHECK_EQ(s.net.SentTotal(T_START_ACK), 0);
        CHECK_EQ(s.net.SentTotal(T_GO_ACK), 0);
    }
    {  // a searcher in 5: the acks are not for it
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r.air.Inject(1, 1, sack, 2);
        r.air.Inject(1, 1, gack, 2);
        r[1].lobby.Poll(true);
        CHECK_EQ(r[1].lobby.bad_messages(), 2);
        CHECK_EQ(r[1].lobby.client_state(), 5);
    }
    {  // the counter saturates, never wraps
        Rig r(1);
        OpenHost(r[0], 3);
        for (int i = 0; i < 100; ++i) {
            r.air.Inject(0, 2, go, 11);
            if (i % 7 == 0) r[0].lobby.Poll(true);
        }
        r[0].lobby.Poll(true);
        r[0].lobby.Poll(true);
        CHECK(r[0].lobby.bad_messages() >= 8);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 8. OnGameUdsStarting.

static void TestGameUdsStarting() {
    std::printf("OnGameUdsStarting\n");
    {  // the creator in 2, 6, 8, 9
        for (int st = 0; st < 4; ++st) {
            Rig r(2);
            Con& h = r[0];
            OpenHost(h, 3);
            if (st >= 1) {
                CHECK(Enter(r, 1, 0));
            }
            if (st == 1) {
                r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
                h.lobby.ServerStartDistribution();  // 6
                CHECK_EQ(h.lobby.ServerState(), 6);
            }
            if (st == 2) {
                h.lobby.ServerStartDistribution();
                r.Run(100);
                CHECK_EQ(SSt(h), 8);
            }
            if (st == 3) {
                CHECK(Finish(r, 0));
                CHECK_EQ(h.lobby.ServerState(), 9);
                CHECK_EQ(h.net.HostCloseCalls(), 1);
            }
            const bool released = h.lobby.OnGameUdsStarting();
            if (st < 3) {
                CHECK(released);
                CHECK_EQ(h.lobby.ServerState(), 1);
                CHECK_EQ(h.net.HostCloseCalls(), 1);
                CHECK_EQ(NClients(h), 0);
                CHECK(!h.lobby.active());
                CHECK_EQ(r.air.Present(0), 0);
            } else {
                CHECK(!released);  // the network was already closed
                CHECK_EQ(h.lobby.ServerState(), 9);
                CHECK_EQ(h.net.HostCloseCalls(), 1);
            }
            CHECK(!h.lobby.OnGameUdsStarting());  // idempotent
            CHECK_EQ(h.net.HostCloseCalls(), st < 3 ? 1 : 1);
        }
        {  // in 1 with the network open (after EndAccepting) it is released as well
            Rig r(1);
            OpenHost(r[0], 3);
            r[0].lobby.ServerEndAccepting();
            CHECK(r[0].lobby.OnGameUdsStarting());
            CHECK_EQ(r[0].net.HostCloseCalls(), 1);
            CHECK_EQ(r[0].lobby.ServerState(), 1);
            CHECK(!r[0].lobby.OnGameUdsStarting());
        }
        {  // nothing initialised: nothing to release
            Rig r(1);
            CHECK(!r[0].lobby.OnGameUdsStarting());
            r[0].lobby.ServerInitialize(3, 0, nullptr);
            r[0].lobby.ClientInitialize(nullptr, 16);
            CHECK(!r[0].lobby.OnGameUdsStarting());
            CHECK_EQ(r[0].net.AllCalls(), 0);
        }
    }
    {  // the searcher in 2, Joining, 5, 6, 9
        for (int st = 0; st < 5; ++st) {
            Rig r(2);
            Con& s = r[1];
            if (st == 0) {  // scanning
                OpenHost(r[0], 3);
                s.lobby.ClientInitialize(nullptr, 16);
                s.lobby.ClientStartScan(0);
                r.Run(100);
            } else if (st == 1) {  // joining
                SilentJoinSetup(r);
                u32 res;
                s.lobby.ClientJoinBegin(AirMac(1), &res);
            } else {
                OpenHost(r[0], 3);
                CHECK(Enter(r, 1, 0));
                if (st >= 3) {
                    r[0].lobby.ServerStartDistribution();
                    r.Run(100);
                }
                if (st == 4) {
                    r[0].lobby.ServerSendPassphrase(kPass1);
                    r.Run(100);
                }
            }
            const u32 expect_state = st == 0 ? 2 : st == 1 ? 3 : st == 2 ? 5 : st == 3 ? 6 : 9;
            CHECK_EQ(s.lobby.client_state(), expect_state);
            const int disc = s.net.DisconnectCalls();
            const int scan_close = s.net.ScanCloseCalls();
            CHECK(s.lobby.OnGameUdsStarting());
            if (st == 0) {
                CHECK_EQ(s.net.ScanCloseCalls(), scan_close + 1);
                CHECK_EQ(s.net.DisconnectCalls(), disc);
                CHECK_STR(s.net.CloseOrder(), "S");
            } else {
                CHECK_EQ(s.net.DisconnectCalls(), disc + 1);
                CHECK_EQ(s.net.ScanCloseCalls(), scan_close);
            }
            if (st == 4) {
                CHECK_EQ(s.lobby.client_state(), 9);  // 9 stays 9 ...
                CHECK_EQ(s.lobby.ClientStatusWord(), 0x09010002u);
                CHECK_STR(PassOf(s), kPass1Hex);  // ... with its passphrase ...
                CHECK_EQ(NNodes(s), 2);           // ... and its roster
            } else {
                CHECK_EQ(s.lobby.client_state(), 1);
                CHECK_EQ(s.lobby.ClientStatusWord(), 0x01000000u);
            }
            CHECK(!s.lobby.active());
            CHECK(!s.lobby.OnGameUdsStarting());  // the second call: nothing
            CHECK_EQ(s.net.DisconnectCalls(), st == 0 ? disc : disc + 1);
            // Finalize afterwards does not touch the network again
            s.lobby.ClientFinalize();
            CHECK_EQ(s.net.DisconnectCalls(), st == 0 ? disc : disc + 1);
            if (st == 4) {
                CHECK_STR(PassOf(s), "000000000000000000");  // (Finalize clears it)
            }
        }
    }
    {  // the closing calls in order on one console: creator closed, then a seeker's Disconnect would follow
        Rig r(1);
        OpenHost(r[0], 3);
        CHECK(r[0].lobby.OnGameUdsStarting());
        CHECK_STR(r[0].net.CloseOrder(), "H");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 9. Two real threads under the caller's mutex.

static void TestThreads() {
    std::printf("threads\n");
    Rig r(2);
    OpenHost(r[0], 3);
    r[1].lobby.ClientInitialize(nullptr, 16);
    r[1].lobby.ClientStartScan(0);
    r.Run(100);
    r[1].lobby.ClientStopScan();
    std::mutex m;
    std::atomic<bool> held{false};
    r.air.WatchLock(&held);
    // 1. a game thread waits in a ClientJoinPoll loop (it sleeps OUTSIDE the mutex); another calls Pump and
    // OnGameUdsStarting. Whatever order they run in, the network is never entered by two at once.
    for (int round = 0; round < 2; ++round) {
        std::atomic<bool> go{false};
        std::atomic<bool> done{false};
        u32 result = 0xFFFF;
        u32 begin_res = 0;
        {
            std::lock_guard<std::mutex> l(m);
            held = true;
            r.air.Advance(50);
            r[1].lobby.ClientInitialize(nullptr, 16);
            r[1].lobby.ClientStartScan(0);
            r.air.Advance(60);
            r[1].lobby.Pump();
            r[1].lobby.ClientStopScan();
            CHECK(r[1].lobby.ClientJoinBegin(MacOf(r[0]), &begin_res) == JoinStep::Pending);
            held = false;
        }
        std::thread waiter([&] {
            go = true;
            while (true) {
                bool fin = false;
                {
                    std::lock_guard<std::mutex> l(m);
                    held = true;
                    r.air.Advance(1);
                    r[0].lobby.Pump();
                    fin = r[1].lobby.ClientJoinPoll(&result);
                    held = false;
                }
                if (fin) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            done = true;
        });
        std::thread other([&] {
            while (!go) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 300 && !done; ++i) {
                {
                    std::lock_guard<std::mutex> l(m);
                    held = true;
                    r[1].lobby.Pump();
                    r[0].lobby.Pump();
                    if (round == 1 && i == 3) {
                        r[1].lobby.OnGameUdsStarting();
                    }
                    held = false;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });
        waiter.join();
        other.join();
        if (round == 0) {
            CHECK_EQ(result, 0);  // the host is there: joined
        } else {
            // joined at once (round 0 left the host open) or cancelled by the other thread; both are legal outcomes, the
            // point is that the waiter ended and nothing overlapped
            CHECK(result == 0 || result == R_INVALID_STATE);
        }
        {
            std::lock_guard<std::mutex> l(m);
            held = true;
            r[1].lobby.ClientStopSession();
            r.Run(60);
            held = false;
        }
    }
    CHECK_EQ(r.air.overlaps(), 0);
    CHECK_EQ(r.air.unlocked_calls(), 0);
    // 2. a join that really is cancelled by the other thread: slow engine, explicit order
    Rig q(2);
    q.air.AddAir(AirMac(1), Ann(0x0010C000, "Ghost", 4, 1), 1);
    q[1].lobby.ClientInitialize(nullptr, 16);
    q[1].lobby.ClientStartScan(0);
    q.Run(60);
    q[1].lobby.ClientStopScan();
    std::atomic<bool> held2{false};
    u32 br = 0;
    CHECK(q[1].lobby.ClientJoinBegin(AirMac(1), &br) == JoinStep::Pending);
    q.air.WatchLock(&held2);
    std::atomic<int> polls{0};
    u32 result2 = 0xFFFF;
    std::thread waiter([&] {
        while (true) {
            bool fin = false;
            {
                std::lock_guard<std::mutex> l(m);
                held2 = true;
                q.air.Advance(1);
                fin = q[1].lobby.ClientJoinPoll(&result2);
                held2 = false;
            }
            ++polls;
            if (fin) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(30));
        }
    });
    std::thread canceller([&] {
        while (polls < 5) {
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> l(m);
        held2 = true;
        q[1].lobby.OnGameUdsStarting();
        held2 = false;
    });
    waiter.join();
    canceller.join();
    CHECK_EQ(result2, R_INVALID_STATE);  // the waiter learns the join was cancelled
    CHECK_EQ(q[1].lobby.client_state(), 1);
    CHECK_EQ(q[1].net.DisconnectCalls(), 1);
    CHECK_EQ(q.air.overlaps(), 0);
    CHECK_EQ(q.air.unlocked_calls(), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// 10. Who holds the network, who owns the tables.

static void TestPossession() {
    std::printf("possession of the network\n");
    {  // the creator in 2 with a client inside; the searcher's Initialize / Finalize / StopSession / StopScan leave it alone
        for (int how = 0; how < 4; ++how) {
            Rig r(2);
            Con& x = r[0];
            OpenHost(x, 3);
            CHECK(Enter(r, 1, 0));
            r.Run(60);
            if (how == 0) CHECK_EQ(x.lobby.ClientInitialize(nullptr, 16), 0);
            if (how == 1) CHECK_EQ(x.lobby.ClientFinalize(), 0);
            if (how == 2) CHECK_EQ(x.lobby.ClientStopSession(), 0);
            if (how == 3) CHECK_EQ(x.lobby.ClientStopScan(), 0);
            CHECK_EQ(x.net.HostCloseCalls(), 0);
            CHECK_EQ(x.net.DisconnectCalls(), 0);
            CHECK_EQ(x.net.ScanCloseCalls(), 0);
            CHECK_EQ(x.net.Kicked(2), 0);
            CHECK_EQ(x.lobby.ServerState(), 2);
            CHECK_EQ(NClients(x), 1);
            CHECK(x.lobby.active());
            CHECK_EQ(x.net.HostUpdateCalls(), 0);
        }
    }
    {  // the creator holds the network: the searcher cannot scan (kBusy, no call); an entry cannot be joined
        Rig r(2);
        Con& x = r[0];
        OpenHost(x, 3);
        x.lobby.ClientInitialize(nullptr, 16);
        const int calls = x.net.AllCalls();
        CHECK_EQ(x.lobby.ClientStartScan(0), R_BUSY);
        CHECK_EQ(x.net.AllCalls(), calls);
        u32 res = 0;
        CHECK(x.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Failed);  // no table: kNoData; the network untouched
        CHECK_EQ(x.net.ConnectBeginCalls(), 0);
        CHECK_EQ(x.lobby.client_state(), 1);
        CHECK_EQ(x.lobby.ServerState(), 2);
    }
    {  // the searcher in 5: ClientInitialize disconnects, state 1, tables empty
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(r[1].lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);  // (its creator side is not initialised)
        r[1].lobby.ServerInitialize(3, 0, nullptr);
        const int calls = r[1].net.AllCalls();
        CHECK_EQ(r[1].lobby.ServerStartAccepting(false, 6), R_BUSY);  // the searcher holds the network
        CHECK_EQ(r[1].net.AllCalls(), calls);
        CHECK_EQ(r[1].lobby.ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(r[1].net.DisconnectCalls(), 1);
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x01000000u);
        GroupView v;
        CHECK_EQ(r[1].lobby.ClientTitleInfoInOrder(true, &v), R_NO_DATA);
        CHECK_EQ(NNodes(r[1]), 0);
        CHECK_EQ(r[1].lobby.ServerStartAccepting(false, 6), 0);  // free now
    }
    {  // the sequence of Triple Deluxe: FKCL Initialize, scan, Finalize, then SRVR Initialize / StartAccepting
        Rig r(2);
        Con& x = r[0];
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        CHECK_EQ(x.lobby.ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(x.lobby.ClientStartScan(0x000400010010C000ull), 0);
        r.Run(60);
        CHECK_EQ(x.lobby.ClientStopScan(), 0);
        CHECK_EQ(x.lobby.ClientFinalize(), 0);
        CHECK_EQ(x.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(x.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(x.lobby.ServerState(), 2);
        // ... and the other way round: the creator finalises, then the searcher scans
        CHECK_EQ(x.lobby.ServerFinalize(), 0);
        CHECK_EQ(x.lobby.ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(x.lobby.ClientStartScan(0), 0);
        CHECK_EQ(x.net.HostCloseCalls(), 1);
    }
    {  // StopScan then StartAccepting discards the frozen table
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& x = r[0];
        x.lobby.ClientInitialize(nullptr, 16);
        x.lobby.ClientStartScan(0);
        r.Run(60);
        x.lobby.ClientStopScan();
        CHECK_EQ(CountEntries(x), 1);
        x.lobby.ServerInitialize(3, 0, nullptr);
        CHECK_EQ(x.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(CountEntries(x), 0);
        GroupView v;
        CHECK_EQ(x.lobby.ClientServerInfo(AirMac(1), &v), R_NO_DATA);
    }
    {  // the passphrase has an owner: the searcher's survives a creator's Initialize and Finalize in between
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Finish(r, 0));
        Con& x = r[1];
        CHECK_STR(PassOf(x), kPass1Hex);
        CHECK_EQ(x.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_STR(PassOf(x), kPass1Hex);
        CHECK_EQ(x.lobby.ServerFinalize(), 0);
        CHECK_STR(PassOf(x), kPass1Hex);
        CHECK_EQ(x.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_STR(PassOf(x), kPass1Hex);
        CHECK_EQ(x.lobby.ClientFinalize(), 0);  // its own owner clears it
        CHECK_STR(PassOf(x), "000000000000000000");
        // and the creator's survives the searcher's Finalize
        Rig q(2);
        OpenHost(q[0], 3);
        CHECK(Finish(q, 0, kPass2));
        q[0].lobby.ClientInitialize(nullptr, 16);
        q[0].lobby.ClientFinalize();
        CHECK_EQ(q[0].lobby.server_state(), 9);
        u8 p[9];
        q[0].lobby.ClientPassphrase(p);  // the searcher role has none of its own
        CHECK_EQ(p[0], 0);
    }
    {  // the creator's passphrase survives the searcher's teardown in the middle of the GO resends: the first GO is lost,
        // the console's searcher side is torn down, the resend still carries the passphrase
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        r.air.Drop(T_GO, FakeAir::kAny, FakeAir::kAny, 1);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(r[0].lobby.ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(r[0].lobby.ClientFinalize(), 0);
        r.Run(400);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_EQ(r[0].lobby.server_state(), 9);
    }
    {  // Initialize on a creator that is running: HostClose, new round possible
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(r[0].lobby.ServerInitialize(2, 1, nullptr), 0);
        CHECK_EQ(r[0].net.HostCloseCalls(), 1);
        CHECK_EQ(r[0].lobby.ServerState(), 1);
        CHECK_EQ(r[0].lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(r[0].net.LastAnnounce().max_nodes, 3);
        CHECK_EQ(r[0].net.LastAnnounce().child_tid_low, 0x0010C001);
    }
    {  // names: InitializeWithName replaces the default, the other role sees it
        Rig r(2);
        u16 nm[10];
        FakeAir::Name(nm, "Renamed");
        r[0].lobby.ServerInitialize(3, 0, nm);
        r[0].lobby.ServerStartAccepting(false, 6);
        CHECK_STR(NameOf(r[0].net.LastAnnounce().name), "Renamed");
        CHECK(Enter(r, 1, 0));
        NodeEntry e;
        CHECK_EQ(r[1].lobby.ClientNodeInfo(1, &e), 0);
        CHECK_STR(NameOf(e.name), "Renamed");
    }
}

static void TestTwoLobbies() {
    std::printf("two lobbies on one air\n");
    Rig r(4);
    OpenHost(r[0], 3);
    OpenHost(r[2], 3);
    // seeker 1 joins host 0, seeker 3 joins host 2; each sees BOTH lobbies in its table and picks by MAC
    for (int i : {1, 3}) {
        r[i].lobby.ClientInitialize(nullptr, 16);
        r[i].lobby.ClientStartScan(0);
    }
    r.Run(100);
    CHECK_EQ(CountEntries(r[1]), 2);
    CHECK_EQ(CountEntries(r[3]), 2);
    for (int i : {1, 3}) {
        r[i].lobby.ClientStopScan();
    }
    u32 res = 0;
    CHECK(r[1].lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
    CHECK(r[3].lobby.ClientJoinBegin(MacOf(r[2]), &res) == JoinStep::Pending);
    r.Run(200);
    CHECK_EQ(r[1].lobby.client_state(), 5);
    CHECK_EQ(r[3].lobby.client_state(), 5);
    CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x05010002u);
    CHECK_EQ(r[3].lobby.ClientStatusWord(), 0x05010002u);  // node 2 in ITS lobby
    CHECK_EQ(r.air.Present(0), 2);
    CHECK_EQ(r.air.Present(2), 2);
    // the two rounds run side by side with different passphrases
    CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
    r.Run(100);
    CHECK_EQ(CSt(r[1]), 6);
    CHECK_EQ(CSt(r[3]), 5);  // the other lobby is not touched
    CHECK_EQ(r[2].lobby.ServerStartDistribution(), 0);
    r.Run(100);
    CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
    CHECK_EQ(r[2].lobby.ServerSendPassphrase(kPass2), 0);
    r.Run(100);
    CHECK_STR(PassOf(r[1]), kPass1Hex);
    CHECK_STR(PassOf(r[3]), kPass2Hex);
    CHECK_EQ(r[0].lobby.ServerState(), 9);
    CHECK_EQ(r[2].lobby.ServerState(), 9);
    CHECK_EQ(r[0].lobby.bad_messages(), 0);
    CHECK_EQ(r[2].lobby.bad_messages(), 0);
    // an announcement from a lobby of another game or with max_nodes above 4 does not get in
    Rig q(1);
    q.air.AddAir(AirMac(1), Ann(0x0010C000, "TooBig", 5, 1), 1);
    q.air.AddAir(AirMac(2), Ann(0x0010C000, "Huge", 16, 1), 3);
    q[0].lobby.ClientInitialize(nullptr, 16);
    q[0].lobby.ClientStartScan(0);
    q.Run(100);
    CHECK_EQ(CountEntries(q[0]), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// 11. The matrix command x state.

enum { M0, M1, M2, MJ, M5, M6, M9, M80 };

/// A rig with console 1 as the searcher under test in the given state, console 0 as the host (not for 0, 1, 2, MJ's silent).
static std::unique_ptr<Rig> BuildSearcher(int st) {
    std::unique_ptr<Rig> r(new Rig(2));
    Rig& g = *r;
    Con& s = g[1];
    switch (st) {
    case M0:
        break;
    case M1:
        s.lobby.ClientInitialize(nullptr, 16);
        break;
    case M2:
        OpenHost(g[0], 3);
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        g.Run(100);
        break;
    case MJ: {
        OpenHost(g[0], 3);
        g[0].net.FailHostOpen(false);
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        g.Run(100);
        s.lobby.ClientStopScan();
        g[1].net.ConnectDelay(100000);  // the engine never admits
        u32 res;
        s.lobby.ClientJoinBegin(MacOf(g[0]), &res);
        break;
    }
    default:
        OpenHost(g[0], 3);
        Enter(g, 1, 0);
        if (st == M6 || st == M9) {
            g[0].lobby.ServerStartDistribution();
            g.Run(100);
        }
        if (st == M9) {
            g[0].lobby.ServerSendPassphrase(kPass1);
            g.Run(100);
        }
        if (st == M80) {
            g.air.Flap(0, 1, 100000);
            g.Run(2200);
        }
        break;
    }
    return r;
}

static void TestSearcherMatrix() {
    std::printf("searcher: command x state\n");
    const char* const names[8] = {"0", "1", "2", "Joining", "5", "6", "9", "0x80"};
    // GetMyStatus first word, by hand
    const u32 status[8] = {0x00000000u, 0x01000000u, 0x02000000u, 0x01000000u, 0x05010002u, 0x06010002u, 0x09010002u, 0x80000000u};
    const u32 raw[8] = {0, 1, 2, 3, 5, 6, 9, 0x80};
    for (int st = 0; st < 8; ++st) {
        std::printf("  state %s\n", names[st]);
        auto make = [&] { return BuildSearcher(st); };
        {
            auto r = make();
            Con& s = (*r)[1];
            CHECK_EQ(s.lobby.client_state(), raw[st]);
            CHECK_EQ(s.lobby.ClientStatusWord(), status[st]);
            CHECK_EQ(CSt(s), status[st] >> 24);
        }
        const Mac hm = AirMac(0);
        {  // list S: success in any state (Finalize, StopScan, StopSession, GetConnectingNodes, Passphrase, Initialize)
            auto r = make();
            Con& s = (*r)[1];
            u16 ids[4];
            u32 total = 77;
            u8 p[9];
            CHECK_EQ(s.lobby.ClientConnectingNodes(ids, 4, &total), 0);
            CHECK_EQ(total, (st == M5 || st == M6 || st == M9) ? 2u : 0u);
            CHECK_EQ(s.lobby.ClientPassphrase(p), 0);
            CHECK_EQ(s.lobby.ClientStopScan(), 0);
            CHECK_EQ(s.lobby.ClientStopSession(), 0);
            CHECK_EQ(s.lobby.ClientFinalize(), 0);
            CHECK_EQ(s.lobby.ClientFinalize(), 0);  // twice
            CHECK_EQ(s.lobby.ClientInitialize(nullptr, 16), 0);
            CHECK_EQ(s.lobby.ClientInitialize(nullptr, 16), 0);
            CHECK_EQ(s.lobby.ClientStatusWord(), 0x01000000u);
        }
        {  // after Finalize: state 1 (0 stays 0)
            auto r = make();
            Con& s = (*r)[1];
            s.lobby.ClientFinalize();
            CHECK_EQ(s.lobby.ClientStatusWord(), st == M0 ? 0x00000000u : 0x01000000u);
        }
        {  // list T: success or kNoData, NEVER kInvalidState, in any state
            auto r = make();
            Con& s = (*r)[1];
            GroupView v;
            NodeEntry e;
            const u32 res[6] = {s.lobby.ClientTitleInfoInOrder(true, &v), s.lobby.ClientTitleInfoInOrder(false, &v),
                                s.lobby.ClientServerInfo(AirMac(9), &v), s.lobby.ClientTitleInfo(AirMac(9), &v),
                                s.lobby.ClientDeleteScanInfo(AirMac(9)), s.lobby.ClientNodeInfo(7, &e)};
            for (int k = 0; k < 6; ++k) {
                CHECK(res[k] == 0 || res[k] == R_NO_DATA);
            }
            CHECK_EQ(res[2], R_NO_DATA);
            CHECK_EQ(res[3], R_NO_DATA);
            CHECK_EQ(res[4], R_NO_DATA);
            CHECK_EQ(res[5], R_NO_DATA);
        }
        {  // the table commands on the real host: by hand per state
            auto r = make();
            Con& s = (*r)[1];
            const Mac h = MacOf((*r)[0]);
            (void)hm;
            GroupView v;
            const u32 in_order = s.lobby.ClientTitleInfoInOrder(true, &v);
            const u32 info = s.lobby.ClientServerInfo(h, &v);
            const u32 title = s.lobby.ClientTitleInfo(h, &v);
            // 0, 1, 80: nothing; 2 and Joining: the table has it; 5, 6, 9: the session's host (not in the order list)
            const bool table = st == M2 || st == MJ;
            const bool sess = st == M5 || st == M6 || st == M9;
            CHECK_EQ(in_order, table ? 0u : R_NO_DATA);
            CHECK_EQ(info, (table || sess) ? 0u : R_NO_DATA);
            CHECK_EQ(title, (table || sess) ? 0u : R_NO_DATA);
            NodeEntry e;
            CHECK_EQ(s.lobby.ClientNodeInfo(1, &e), sess ? 0u : R_NO_DATA);
            CHECK_EQ(s.lobby.ClientNodeInfo(2, &e), sess ? 0u : R_NO_DATA);
            CHECK_EQ(s.lobby.ClientNodeInfo(3, &e), R_NO_DATA);
            const u32 del = s.lobby.ClientDeleteScanInfo(h);
            CHECK_EQ(del, st == M2 ? 0u : R_NO_DATA);  // only in 1 and 2 (and in 1 the table is empty here)
        }
        {  // StartScan: only from 1
            auto r = make();
            Con& s = (*r)[1];
            const u32 rc = s.lobby.ClientStartScan(0);
            CHECK_EQ(rc, st == M1 ? 0u : R_INVALID_STATE);
            if (st == M1) CHECK_EQ(s.lobby.ClientStatusWord(), 0x02000000u);
        }
        {  // JoinBegin: only from 1 (and there it needs the entry: here the table is empty)
            auto r = make();
            Con& s = (*r)[1];
            u32 res = 0;
            const JoinStep js = s.lobby.ClientJoinBegin(MacOf((*r)[0]), &res);
            CHECK(js == JoinStep::Failed);
            CHECK_EQ(res, st == M1 ? R_NO_DATA : R_INVALID_STATE);
        }
        {  // StopScan: only 2 -> 1; StopSession: 3, 5, 6, 9, 0x80 -> 1
            auto r = make();
            Con& s = (*r)[1];
            s.lobby.ClientStopScan();
            CHECK_EQ(s.lobby.client_state(), st == M2 ? 1u : raw[st]);
            auto r2 = make();
            Con& t = (*r2)[1];
            t.lobby.ClientStopSession();
            const bool leaves = st == MJ || st == M5 || st == M6 || st == M9 || st == M80;
            CHECK_EQ(t.lobby.client_state(), leaves ? 1u : raw[st]);
        }
        {  // JoinPoll without a join: true with kInvalidState (except Joining: still running)
            auto r = make();
            Con& s = (*r)[1];
            u32 res = 0;
            const bool done = s.lobby.ClientJoinPoll(&res);
            CHECK_EQ(done, st == MJ ? 0u : 1u);
            if (st != MJ) CHECK_EQ(res, R_INVALID_STATE);
        }
    }
    // a searcher with an entry in its table in state 1 (after StopScan): JoinBegin goes on
    {
        Rig r(2);
        OpenHost(r[0], 3);
        r[1].lobby.ClientInitialize(nullptr, 16);
        r[1].lobby.ClientStartScan(0);
        r.Run(100);
        r[1].lobby.ClientStopScan();
        u32 res = 0;
        CHECK(r[1].lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x01000000u);
    }
}

/// A rig with console 0 as the creator under test in the given state: 0, 1, 2, 6 (a client that never confirms), 8, 9.
static std::unique_ptr<Rig> BuildCreator(int st) {
    std::unique_ptr<Rig> r(new Rig(2));
    Rig& g = *r;
    Con& h = g[0];
    if (st >= 1) h.lobby.ServerInitialize(3, 0, nullptr);
    if (st >= 2) h.lobby.ServerStartAccepting(false, 6);
    if (st == 6) {
        Enter(g, 1, 0);
        g.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        h.lobby.ServerStartDistribution();
    }
    if (st >= 8) {
        h.lobby.ServerStartDistribution();
    }
    if (st == 9) {
        h.lobby.ServerSendPassphrase(kPass1);
    }
    return r;
}

static void TestCreatorMatrix() {
    std::printf("creator: command x state\n");
    const int states[6] = {0, 1, 2, 6, 8, 9};
    for (int k = 0; k < 6; ++k) {
        const int st = states[k];
        std::printf("  state %d\n", st);
        {
            auto r = BuildCreator(st);
            CHECK_EQ((*r)[0].lobby.ServerState(), static_cast<u32>(st));
        }
        {  // list S: always 0
            auto r = BuildCreator(st);
            Con& h = (*r)[0];
            CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
            CHECK_EQ(h.lobby.ServerState(), st == 2 ? 1u : static_cast<u32>(st));
            CHECK_EQ(h.lobby.ServerInitialize(3, 0, nullptr), 0);
            CHECK_EQ(h.lobby.ServerState(), 1);
            CHECK_EQ(h.lobby.ServerFinalize(), 0);
            CHECK_EQ(h.lobby.ServerState(), 0);
            CHECK_EQ(h.lobby.ServerFinalize(), 0);
        }
        {  // StartAccepting: only from 1; StartDistribution: only from 2; SendPassphrase: only from 8
            auto r = BuildCreator(st);
            CHECK_EQ((*r)[0].lobby.ServerStartAccepting(false, 6), st == 1 ? 0u : R_INVALID_STATE);
            auto r2 = BuildCreator(st);
            CHECK_EQ((*r2)[0].lobby.ServerStartDistribution(), st == 2 ? 0u : R_INVALID_STATE);
            auto r3 = BuildCreator(st);
            CHECK_EQ((*r3)[0].lobby.ServerSendPassphrase(kPass2), st == 8 ? 0u : R_INVALID_STATE);
        }
        {  // the commands that need an initialised creator: kInvalidState from 0
            auto r = BuildCreator(st);
            Con& h = (*r)[0];
            u16 ids[4];
            u32 total = 77;
            NodeEntry e;
            u32 w = 0;
            const u32 bad = st == 0 ? R_INVALID_STATE : 0;
            CHECK_EQ(h.lobby.ServerConnectingClients(ids, 4, &total), bad);
            CHECK_EQ(h.lobby.ServerClientInfo(1, &e), st == 0 ? R_INVALID_STATE : R_NO_DATA);
            CHECK_EQ(h.lobby.ServerClientInfo(2, &e), st == 0 ? R_INVALID_STATE : (st == 6 ? 0u : R_NO_DATA));
            CHECK_EQ(h.lobby.ServerClientState(2, &w), st == 0 ? R_INVALID_STATE : (st == 6 ? 0u : R_NO_DATA));
            CHECK_EQ(h.lobby.ServerAcceptClient(2), st == 0 ? R_INVALID_STATE : R_NO_DATA);  // (accepted already, or none)
            CHECK_EQ(h.lobby.ServerDisconnectClient(3), st == 0 ? R_INVALID_STATE : R_NO_DATA);
            if (st != 0) {
                CHECK_EQ(total, st == 6 ? 1u : 0u);
            }
        }
        {  // state 9: GetConnectingClients total 0, the network closed, nobody listed
            if (st == 9) {
                auto r = BuildCreator(st);
                CHECK_EQ(NClients((*r)[0]), 0);
                CHECK_EQ((*r)[0].net.HostCloseCalls(), 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 12. Azahar's defects, not copied; the seven, each with its name.

static void TestAzaharDefects() {
    std::printf("Azahar defects\n");
    {  // defect 1: no assertion on the StartScan channel value (the lobby takes only the title filter) or on the MAC filter
        Rig r(1);
        r[0].lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(r[0].lobby.ClientStartScan(0xDEADBEEF00000000ull), 0);  // any filter, nothing asserts
        CHECK_EQ(r[0].lobby.client_state(), 2);
    }
    {  // defect 2: a failed StartFakeSession leaves state 1, never a residual 5
        Rig r(2);
        SilentJoinSetup(r);
        u32 res = 0;
        r[1].lobby.ClientJoinBegin(AirMac(1), &res);
        r.Run(3100);
        CHECK(r[1].lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_TIMEOUT);
        CHECK_EQ(r[1].lobby.ClientStatusWord(), 0x01000000u);
        u16 ids[4];
        u32 total = 9;
        r[1].lobby.ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 0);
    }
    {  // defect 3: GetClientState with a node outside the vector answers kNoData
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        u32 w = 5;
        CHECK_EQ(r[0].lobby.ServerClientState(0, &w), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerClientState(1, &w), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerClientState(5, &w), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerClientState(0xFFFF, &w), R_NO_DATA);
    }
    {  // defect 4: the creator's Finalize puts the state back to 0
        Rig r(1);
        OpenHost(r[0], 3);
        r[0].lobby.ServerFinalize();
        CHECK_EQ(r[0].lobby.ServerState(), 0);
    }
    {  // defect 5: no -1 placeholder: the unknown / out-of-state answers are the codes of the module
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(r[0].lobby.ServerAcceptClient(9), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerDisconnectClient(9), R_NO_DATA);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);  // (state 2)
        r[0].lobby.ServerStartDistribution();
        r.Run(100);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(r[0].lobby.ServerSendPassphrase(kPass1), R_INVALID_STATE);
        CHECK(R_INVALID_STATE != 0xFFFFFFFFu && R_NO_DATA != 0xFFFFFFFFu);
    }
    {  // defect 6: the child index of Initialize counts: it is in the announcement (and so in the child title id)
        Rig r(1);
        r[0].lobby.ServerInitialize(3, 2, nullptr);
        r[0].lobby.ServerStartAccepting(false, 6);
        CHECK_EQ(r[0].net.LastAnnounce().child_tid_low, 0x0010C002);
        Rig q(1);
        q[0].lobby.ServerInitialize(3, 0x12345678, nullptr);  // only the low byte
        q[0].lobby.ServerStartAccepting(false, 6);
        CHECK_EQ(q[0].net.LastAnnounce().child_tid_low, 0x0010C078);
    }
    {  // defect 7 (copied on purpose): max_nodes is max clients + 1 and total_nodes counts the host
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK_EQ(r[0].net.LastAnnounce().max_nodes, 4);
        r[1].lobby.ClientInitialize(nullptr, 16);
        r[1].lobby.ClientStartScan(0);
        r.Run(100);
        GroupView v;
        CHECK_EQ(r[1].lobby.ClientServerInfo(MacOf(r[0]), &v), 0);
        CHECK_EQ(v.group->max_nodes, 4);
        CHECK_EQ(v.group->total_nodes, 1);  // only the host is there
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 13. Complete rounds in the order of the real games (SPEC sez. 7-8 for Battle Royale, GIOCO-TD-PR sez. 12 for Triple Deluxe).

struct HostGame {
    Con* c = nullptr;
    bool br = false;
    int want = 1;  // clients to wait for before "Start"
    int phase = 0;
    int frames = 0;
    u32 rng = 0x1234;
    bool have_pass = false;
    u8 first_pass[9] = {};
    std::string seen_names;  // the client names the game read, in order
    int bad_results = 0;     // a result the game would treat as an error
    bool done = false;
    bool all_zero_check = false;

    void Frame() {
        Con& h = *c;
        ++frames;
        switch (phase) {
        case 0: {  // Initialize(max clients 3 for TD, 4 for BR) / InitializeWithName
            u16 nm[10];
            FakeAir::Name(nm, "HostBR");
            if (h.lobby.ServerInitialize(br ? 4 : 3, 0, br ? nm : nullptr) != 0) ++bad_results;
            phase = 1;
            break;
        }
        case 1:
            if (h.lobby.ServerStartAccepting(false, 6) != 0) ++bad_results;
            phase = 2;
            break;
        case 2: {  // the lobby screen, every frame
            h.lobby.Pump();
            const u32 st = h.lobby.ServerState();
            if (st == 0 || st == 10) ++bad_results;
            if (st == 2) {
                u16 ids[4] = {};
                u32 total = 0;
                if (h.lobby.ServerConnectingClients(ids, 4, &total) != 0) ++bad_results;
                seen_names.clear();
                for (u32 i = 0; i < total && i < 4; ++i) {
                    NodeEntry e;
                    if (h.lobby.ServerClientInfo(ids[i], &e) == 0) {
                        seen_names += NameOf(e.name);
                        seen_names += ',';
                    }
                }
                if (static_cast<int>(total) >= want && frames > 60) {
                    if (h.lobby.ServerStartDistribution() != 0) ++bad_results;
                    phase = 3;
                }
            }
            break;
        }
        case 3: {  // after "Start": every frame GetServerState; at 8 the passphrase, EVERY frame
            h.lobby.Pump();
            const u32 st = h.lobby.ServerState();
            if (st == 0 || st == 10) ++bad_results;
            if (st == 8) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08x", rng++);
                u8 pass[9];
                std::memcpy(pass, buf, 9);  // 8 characters + NUL
                if (!have_pass) {
                    std::memcpy(first_pass, pass, 9);
                    have_pass = true;
                }
                (void)h.lobby.ServerSendPassphrase(pass);  // the result is ignored by the game
            }
            if (st == 9 && NClients(h) == 0) {  // "9 AND zero clients"
                if (h.lobby.ServerFinalize() != 0) ++bad_results;
                phase = 4;
                done = true;
            }
            break;
        }
        default:
            break;
        }
    }
};

struct ClientGame {
    Con* c = nullptr;
    bool br = false;
    int phase = 0;
    int frames = 0;
    int start_frame = 0;
    u32 tid_low = 0x0010C000;
    bool pass_ok = false;
    std::string pass;
    std::string names;  // the roster names, as the game read them in the last frame
    int bad_results = 0;
    int deleted = 0;
    bool done = false;
    Mac chosen = {};
    int max_nodes_seen = 0;
    int max_roster = 0;  // the most players the game ever read in a frame

    void Frame(FakeAir& air) {
        Con& s = *c;
        if (++frames < start_frame) return;
        switch (phase) {
        case 0: {
            u16 nm[10];
            FakeAir::Name(nm, "ClientBR");
            s.lobby.ClientInitialize(br ? nm : nullptr, 16);  // BR: InitializeWithName, TD: Initialize, name NULL
            phase = 1;
            break;
        }
        case 1: {  // the scan screen: StartScan with the filter (child index 0)
            const u64 filter = 0x0004000100000000ull | (tid_low & 0xFFFFFF00u);
            if (s.lobby.ClientStartScan(filter) != 0) ++bad_results;
            phase = 2;
            break;
        }
        case 2: {  // every frame: the list; TD ages the entries (>= 5000 ms) with DeleteScanInfo
            s.lobby.Pump();
            GroupView v;
            bool restart = true;
            int found = 0;
            Mac first = {};
            while (true) {
                const u32 rc = s.lobby.ClientTitleInfoInOrder(restart, &v);
                restart = false;
                if (rc == R_NO_DATA) break;
                if (rc != 0) {
                    ++bad_results;
                    break;
                }
                const Mac mac = v.group->host;
                GroupView si;
                if (s.lobby.ClientServerInfo(mac, &si) != 0) continue;
                const u64 age_ms = (air.ticks() - si.tick) / 268112ull;
                if (age_ms >= 5000) {
                    s.lobby.ClientDeleteScanInfo(mac);
                    ++deleted;
                    continue;
                }
                if (found++ == 0) first = mac;
                if (si.group->max_nodes > max_nodes_seen) max_nodes_seen = si.group->max_nodes;
            }
            if (found > 0 && frames > start_frame + 5) {
                chosen = first;
                s.lobby.ClientStopScan();
                u32 res = 0;
                if (s.lobby.ClientJoinBegin(chosen, &res) != JoinStep::Pending) ++bad_results;
                phase = 3;
            }
            break;
        }
        case 3: {  // StartFakeSession is one blocking call in the game: here the loop of PollPending
            u32 res = 0;
            if (s.lobby.ClientJoinPoll(&res)) {
                if (res != 0) ++bad_results;
                phase = 4;
            }
            break;
        }
        case 4: {  // the session screen, every frame
            s.lobby.Pump();
            const u32 w = s.lobby.ClientStatusWord();
            const u32 st = w >> 24;
            if (st == 0 || st == 0x80) ++bad_results;
            if (st == 3 || st == 4 || st == 5 || st == 6 || st == 9) {
                u16 ids[4] = {};
                u32 total = 0;
                s.lobby.ClientConnectingNodes(ids, 4, &total);
                names.clear();
                for (u32 i = 0; i < total && i < 4; ++i) {
                    NodeEntry e;
                    if (s.lobby.ClientNodeInfo(ids[i], &e) == 0) {
                        names += NameOf(e.name);
                        names += ',';
                    } else {
                        ++bad_results;
                    }
                }
                if (static_cast<int>(total) > max_roster) max_roster = static_cast<int>(total);
            }
            if (st == 9) {
                u8 p[9];
                s.lobby.ClientPassphrase(p);
                pass = PassOf(s);
                pass_ok = true;
                if (s.lobby.ClientFinalize() != 0) ++bad_results;
                phase = 5;
                done = true;
            }
            break;
        }
        default:
            break;
        }
    }
};

/// n clients (1..3) round with the sequence of Battle Royale (br) or Triple Deluxe, 16 ms frames.
static void PlayRound(int clients, bool br) {
    Rig r(1 + clients, 100000, br ? 0x001C2100u : 0x0010C000u);
    HostGame h;
    h.c = &r[0];
    h.br = br;
    h.want = clients;
    std::vector<ClientGame> cg(static_cast<size_t>(clients));
    for (int i = 0; i < clients; ++i) {
        cg[static_cast<size_t>(i)].c = &r[i + 1];
        cg[static_cast<size_t>(i)].br = br;
        cg[static_cast<size_t>(i)].tid_low = br ? 0x001C2100u : 0x0010C000u;
        cg[static_cast<size_t>(i)].start_frame = 10 + 40 * i;  // they arrive one after the other
    }
    bool all = false;
    for (int frame = 0; frame < 3000 && !all; ++frame) {
        r.air.Advance(16);
        h.Frame();
        for (auto& g : cg) g.Frame(r.air);
        for (auto& x : r.c) x->lobby.Pump();
        all = h.done;
        for (auto& g : cg) all = all && g.done;
    }
    CHECK(all);
    CHECK_EQ(h.bad_results, 0);
    CHECK(h.have_pass);
    char fp[32];
    std::snprintf(fp, sizeof(fp), "%02x%02x%02x%02x%02x%02x%02x%02x%02x", h.first_pass[0], h.first_pass[1], h.first_pass[2],
                  h.first_pass[3], h.first_pass[4], h.first_pass[5], h.first_pass[6], h.first_pass[7], h.first_pass[8]);
    int best_roster = 0;
    for (int i = 0; i < clients; ++i) {
        ClientGame& g = cg[static_cast<size_t>(i)];
        CHECK_EQ(g.bad_results, 0);
        CHECK(g.pass_ok);
        CHECK_STR(g.pass, fp);  // the FIRST passphrase the game generated, for everybody
        CHECK_EQ(g.deleted, 0);  // nothing aged out while scanning
        CHECK_EQ(g.max_nodes_seen, 4);
        CHECK(g.chosen == MacOf(r[0]));
        // the roster the game read at 9 holds the host and every client that was in
        CHECK(g.names.find(br ? "HostBR," : "Host,") == 0);
        CHECK(g.names.find(br ? "ClientBR," : kNames[i + 1]) != std::string::npos);
        // every client saw at least itself and the host; a client that finishes first has left the network when a slower one
        // reads, so only the best view has to hold everybody
        CHECK(g.max_roster >= 2);
        if (g.max_roster > best_roster) best_roster = g.max_roster;
    }
    CHECK_EQ(best_roster, 1 + clients);
    CHECK_EQ(r[0].lobby.server_state(), 0);  // Finalize at the end
    CHECK_EQ(r[0].net.HostCloseCalls(), 1);
    CHECK_EQ(r.air.Present(0), 0);
    for (int i = 0; i <= clients; ++i) {
        CHECK_EQ(r[i].lobby.bad_messages(), 0);
    }
}

static void TestFullRounds() {
    std::printf("complete rounds (Battle Royale and Triple Deluxe order)\n");
    for (int br = 0; br < 2; ++br) {
        for (int n = 1; n <= 3; ++n) {
            std::printf("  %s, %d client(s)\n", br ? "BR" : "TD", n);
            PlayRound(n, br != 0);
        }
    }
    {  // TD with a lossy network: every message type dropped once; the round still ends
        Rig r(3);
        r.air.Drop(T_START, FakeAir::kAny, FakeAir::kAny, 1);
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, 1);
        r.air.Drop(T_GO, FakeAir::kAny, FakeAir::kAny, 1);
        r.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, 1);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        CHECK(Finish(r, 0));
        CHECK_EQ(r[1].lobby.client_state(), 9);
        CHECK_EQ(r[2].lobby.client_state(), 9);
        CHECK_STR(PassOf(r[1]), PassOf(r[2]));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 13. Gaps found by the attack and the mutation run (DESIGN-FASE1.md sez. 16). Each block below kills mutants that the tests above
// let live, or pins a decision taken in the review. The comment of a block says what it holds.

// A way into the private saturating counter, for the one test of its saturation (an explicit instantiation is exempt from
// the access check; no production code is touched).
template <typename Tag, typename Tag::type M>
struct Robber {
    friend typename Tag::type Steal(Tag) { return M; }
};
struct BadMessagesTag {
    using type = u32 DlpLobby::*;
    friend type Steal(BadMessagesTag);
};
template struct Robber<BadMessagesTag, &DlpLobby::bad_messages_>;

// Names of ten characters: the whole name field of a Group is non-zero, so a copy that stops a word early shows.
static const char* const kLong[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZabcd", "efghijklmn"};

/// Codec: the edges the first test does not reach (the buffer id, the accepting byte, null out, unknown types, exact buffers).
static void TestCodecEdges() {
    std::printf("codec edges\n");
    // StaticBufferDesc keeps the whole 4-bit buffer id (bits 10-13) and nothing above
    CHECK_EQ(StaticBufferDesc(0x10, 8), (0x10u << 14) | (8u << 10) | 2u);
    CHECK_EQ(StaticBufferDesc(0x10, 15), (0x10u << 14) | (15u << 10) | 2u);
    CHECK_EQ(StaticBufferDesc(0x10, 0x1F), (0x10u << 14) | (15u << 10) | 2u);
    // EncodeAnnounce: the flags byte is 0 or 1 whatever non-zero "accepting" the caller holds; Decode gives 1
    HostAnnounce a = Ann(0x0010C000, "x", 4, 2);
    u8 out[29];
    EncodeAnnounce(a, out);
    CHECK_EQ(out[3], 1);
    a.accepting = 255;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[3], 1);
    a.accepting = 0;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[3], 0);
    a.accepting = 7;
    EncodeAnnounce(a, out);
    HostAnnounce r;
    std::memset(&r, 0, sizeof(r));
    CHECK(DecodeAnnounce(out, 29, &r));
    CHECK_EQ(r.accepting, 1);
    CHECK(!DecodeAnnounce(out, 29, nullptr));  // a null out is refused, not dereferenced
    // EncodeMessage with an unknown type writes nothing and returns 0 even when the body length equals the "unknown" marker
    // (a heap buffer of exactly 16 bytes: a write past it is an ASan report)
    {
        std::unique_ptr<u8[]> o(new u8[kMaxMessage]);
        std::memset(o.get(), 0x5A, kMaxMessage);
        std::unique_ptr<u8[]> body(new u8[255]);
        std::memset(body.get(), 0x11, 255);
        CHECK_EQ(EncodeMessage(static_cast<MsgType>(9), body.get(), 255, o.get()), 0);
        CHECK_EQ(EncodeMessage(static_cast<MsgType>(0), body.get(), 255, o.get()), 0);
        CHECK_EQ(EncodeMessage(static_cast<MsgType>(9), body.get(), 0, o.get()), 0);
        bool untouched = true;
        for (u32 i = 0; i < kMaxMessage; ++i) {
            untouched = untouched && o[i] == 0x5A;
        }
        CHECK(untouched);
    }
    // IsMessage / DecodeMessage: the same verdicts, on buffers of exactly the size given (never a byte beyond)
    {
        std::unique_ptr<u8[]> one(new u8[1]);
        one[0] = 1;
        CHECK(!IsMessage(one.get(), 1));  // one byte is not a message: the length byte is not there
        CHECK(!IsMessage(one.get(), 0));
        CHECK(!IsMessage(nullptr, 2));
        MsgType t;
        const u8* body = nullptr;
        CHECK(!DecodeMessage(one.get(), 1, &t, &body));
        const u8 start[2] = {1, 0};
        CHECK(IsMessage(start, 2));
        CHECK(DecodeMessage(start, 2, &t, &body));
        const u8 bad_len[2] = {1, 5};  // START with a length byte of 5 and no body
        CHECK(!IsMessage(bad_len, 2));
        const u8 trailing[3] = {1, 0, 0xAA};  // a byte after the message
        CHECK(!IsMessage(trailing, 3));
        CHECK(!DecodeMessage(trailing, 3, &t, &body));
        u8 go[11] = {3, 9, 1, 2, 3, 4, 5, 6, 7, 8, 0};
        CHECK(IsMessage(go, 11));
        CHECK(!IsMessage(go, 10));  // too short for the length it says
        CHECK(!IsMessage(go, 12));
        go[1] = 8;  // GO with the wrong length byte, the right size
        CHECK(!IsMessage(go, 11));
        CHECK(!DecodeMessage(go, 11, &t, &body));
        std::unique_ptr<u8[]> big(new u8[257]);
        std::memset(big.get(), 0, 257);
        big[0] = 9;  // an unknown type whose length byte is 0xFF and whose size is 2 + 0xFF
        big[1] = 0xFF;
        CHECK(!IsMessage(big.get(), 257));
        CHECK(!DecodeMessage(big.get(), 257, &t, &body));
    }
}

/// The scan table: the signal of a heard group is clamped to 3 (4 included); max_beacons 5 means 4 and writes nothing outside
/// the table; a lobby is still refreshed when it is heard at an index above `max_beacons`; StartScan again restarts the cursor;
/// GetTitleInfoInOrder gives the entry the age it really has.
static void TestScanGaps() {
    std::printf("scan gaps\n");
    {  // signal 4, 5 and 255 all read 3
        const int sig[3] = {4, 5, 255};
        for (int k = 0; k < 3; ++k) {
            Rig r(1);
            r.air.AddAir(AirMac(1), Ann(0x0010C000, "Sig", 4, 1), 1, static_cast<u8>(sig[k]));
            Con& s = r[0];
            s.lobby.ClientInitialize(nullptr, 16);
            s.lobby.ClientStartScan(0);
            r.Run(40);
            GroupView v;
            CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
            CHECK_EQ(v.group->signal, 3);
        }
        const int ok[4] = {0, 1, 2, 3};  // and the values 0..3 are kept as they are
        for (int k = 0; k < 4; ++k) {
            Rig r(1);
            r.air.AddAir(AirMac(1), Ann(0x0010C000, "Sig", 4, 1), 1, static_cast<u8>(ok[k]));
            Con& s = r[0];
            s.lobby.ClientInitialize(nullptr, 16);
            s.lobby.ClientStartScan(0);
            r.Run(40);
            GroupView v;
            CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
            CHECK_EQ(v.group->signal, ok[k]);
        }
    }
    {  // max_beacons 5 is 4: the fifth lobby, read while the table is full, is ignored and nothing is written beyond the table
        Rig r(1);
        for (int i = 0; i < 5; ++i) {
            r.air.AddAir(AirMac(i), Ann(0x0010C0AB, "EEEEEEEEEE", 4, 1), 1);
        }
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 5);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        r.Run(100);
        CHECK_EQ(CountEntries(s), 4);  // lobbies 0..3: the four places
        r.air.RemoveAir(0);            // lobby 0 leaves the air (its entry is still young): the fifth is now read at index 3
        r.Run(100);
        GroupView v;
        CHECK_EQ(CountEntries(s), 4);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(4), &v), R_NO_DATA);  // not in the table
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(0), &v), 0);          // lobby 0 still there
        CHECK_EQ(s.lobby.bad_messages(), 0);  // (a write past the table lands on the counters that follow it)
        r.Run(1700);                          // now lobby 0 has expired: the fifth takes its place
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(0), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(4), &v), 0);
        CHECK_EQ(s.lobby.bad_messages(), 0);
    }
    {  // max_beacons 1: the table holds A; A is heard at index 2 (after B and C): it is still refreshed, B and C never get in
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 1);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        r.air.RemoveAir(0);
        r.air.AddAir(AirMac(2), Ann(0x0010C000, "B", 4, 1), 1);
        r.air.AddAir(AirMac(3), Ann(0x0010C000, "C", 4, 1), 1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);  // heard third now
        r.Run(4000);                                              // far beyond the 1500 ms of expiry
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &v), 0);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(2), &v), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(3), &v), R_NO_DATA);
        CHECK_EQ(CountEntries(s), 1);
    }
    {  // StartScan after StopScan (no Initialize): the cursor of GetTitleInfoInOrder starts again
        Rig r(1);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);  // the cursor is at the end
        CHECK_EQ(s.lobby.ClientStopScan(), 0);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        r.Run(60);
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(false, &v), 0);  // restart = false, and still the first entry
        CHECK(v.group->host == AirMac(1));
    }
    {  // GetTitleInfoInOrder: the tick of a frozen entry is the tick of when it was heard (its age is not forgotten)
        Rig r(1, 9000);
        r.air.AddAir(AirMac(1), Ann(0x0010C000, "A", 4, 1), 1);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(40);
        s.lobby.ClientStopScan();  // frozen: nothing refreshes it
        r.air.Advance(1000);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK_EQ(v.tick, r.air.ticks() - 1000ull * 268112ull);
        GroupView w;
        CHECK_EQ(s.lobby.ClientServerInfo(AirMac(1), &w), 0);
        CHECK_EQ(w.tick, v.tick);
    }
}

/// Who owns the passphrase: after the creator completes, the searcher's view of the same object is zeros, also after the
/// creator's Finalize / Initialize; and a GO received later (the same object as a searcher) is what the searcher reads.
static void TestPassphraseOwner() {
    std::printf("passphrase owner\n");
    for (int how = 0; how < 3; ++how) {
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Finish(r, 0, kPass1));
        CHECK_EQ(r[0].lobby.ServerState(), 9);
        CHECK_STR(PassOf(r[1]), kPass1Hex);
        CHECK_STR(PassOf(r[0]), "000000000000000000");  // the creator's own GO is not the searcher's passphrase
        r.Run(200);
        CHECK_STR(PassOf(r[0]), "000000000000000000");
        if (how == 1) {
            CHECK_EQ(r[0].lobby.ServerFinalize(), 0);
            CHECK_STR(PassOf(r[0]), "000000000000000000");
        } else if (how == 2) {
            CHECK_EQ(r[0].lobby.ServerInitialize(3, 0, nullptr), 0);
            CHECK_STR(PassOf(r[0]), "000000000000000000");
        }
    }
    {  // the same console is a creator that completed, then a searcher that receives a GO from another host
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Finish(r, 0, kPass1));
        CHECK_STR(PassOf(r[0]), "000000000000000000");
        OpenHost(r[2], 3);
        CHECK(Enter(r, 0, 2));
        CHECK(Finish(r, 2, kPass2));
        CHECK_EQ(r[0].lobby.client_state(), 9);
        CHECK_STR(PassOf(r[0]), kPass2Hex);  // what it RECEIVED (the creator's flag was cleared by the GO)
    }
}

/// A join that is cancelled (by any of the ways) leaves nothing pending: the next ClientJoinPoll answers kInvalidState at once.
static void TestJoinCancel() {
    std::printf("join cancel\n");
    for (int how = 0; how < 3; ++how) {  // StopSession, Finalize, Initialize
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK(s.lobby.join_active());
        if (how == 0) {
            CHECK_EQ(s.lobby.ClientStopSession(), 0);
        } else if (how == 1) {
            CHECK_EQ(s.lobby.ClientFinalize(), 0);
        } else {
            CHECK_EQ(s.lobby.ClientInitialize(nullptr, 16), 0);
        }
        CHECK(!s.lobby.join_active());
        CHECK_EQ(s.lobby.client_state(), 1);
        CHECK_EQ(s.net.DisconnectCalls(), 1);
        res = 0;
        CHECK(s.lobby.ClientJoinPoll(&res));  // finished, at once
        CHECK_EQ(res, R_INVALID_STATE);
        CHECK(s.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_INVALID_STATE);
    }
    {  // an outcome nobody collected (kTimeout) is dropped by the next ClientJoinBegin
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        r.Run(3100);  // nobody asks: the join times out by itself and keeps its outcome
        CHECK_EQ(s.lobby.client_state(), 1);
        CHECK(!s.lobby.join_active());
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);  // the table is intact: again
        CHECK(s.lobby.OnGameUdsStarting());                                     // cancelled
        res = 0;
        CHECK(s.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_INVALID_STATE);  // not the old kTimeout
    }
    {  // a join that completed (state 5) and whose outcome nobody collected: the game starts nwm::UDS, the outcome is gone
        Rig r(2);
        OpenHost(r[0], 3);
        Con& s = r[1];
        s.lobby.ClientInitialize(nullptr, 16);
        s.lobby.ClientStartScan(0);
        r.Run(60);
        s.lobby.ClientStopScan();
        u32 res = 77;
        CHECK(s.lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
        CHECK(r.RunUntil([&] { return s.lobby.client_state() == 5; }, 3000));
        CHECK(s.lobby.OnGameUdsStarting());
        CHECK_EQ(s.lobby.client_state(), 1);
        res = 77;
        CHECK(s.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, R_INVALID_STATE);
    }
    {  // OnGameUdsStarting during a join: the frozen scan table STAYS (the same MAC can be tried again)
        Rig r(2);
        SilentJoinSetup(r);
        Con& s = r[1];
        u32 res = 0;
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
        CHECK(s.lobby.OnGameUdsStarting());
        CHECK_EQ(s.lobby.client_state(), 1);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfo(AirMac(1), &v), 0);
        CHECK_EQ(CountEntries(s), 1);
        CHECK(s.lobby.ClientJoinBegin(AirMac(1), &res) == JoinStep::Pending);
    }
}

/// The creator: a table that is not its own is never read as clients; StartAccepting clears the searcher's table and the
/// host-lost timer of an earlier round; "manual" is set AND cleared by each StartAccepting.
static void TestCreatorTables() {
    std::printf("creator tables\n");
    {  // a frozen scan table whose bytes would read as clients (byte 4 of the MAC = a track state 1)
        Rig r(1);
        const Mac ghost = Mac{0x00, 0x1F, 0x32, 0xA0, 0x01, 0x01};
        r.air.AddAir(ghost, Ann(0x0010C000, "Ghost", 4, 1), 1);
        Con& c = r[0];
        c.lobby.ClientInitialize(nullptr, 16);
        c.lobby.ClientStartScan(0);
        r.Run(60);
        c.lobby.ClientStopScan();
        CHECK_EQ(CountEntries(c), 1);
        CHECK_EQ(c.lobby.ServerInitialize(3, 0, nullptr), 0);
        // the creator holds no network yet: its commands on a node find nothing, and do nothing
        NodeEntry e;
        std::memset(&e, 0x77, sizeof(e));
        u32 w = 0;
        CHECK_EQ(c.lobby.ServerAcceptClient(2), R_NO_DATA);
        CHECK_EQ(c.lobby.ServerDisconnectClient(2), R_NO_DATA);
        CHECK_EQ(c.net.Kicked(2), 0);
        CHECK_EQ(c.lobby.ServerClientInfo(2, &e), R_NO_DATA);
        CHECK_EQ(e.node, 0);
        CHECK_EQ(c.lobby.ServerClientState(2, &w), R_NO_DATA);
        // taking the network discards the searcher's table: the first look, before any Poll, finds no client
        CHECK_EQ(c.lobby.ServerStartAccepting(false, 6), 0);
        u16 ids[4];
        u32 total = 99;
        CHECK_EQ(c.lobby.ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 0);
        for (u16 n = 2; n <= 4; ++n) {
            CHECK_EQ(c.lobby.ServerClientState(n, &w), R_NO_DATA);
        }
        CHECK_EQ(CountEntries(c), 0);
    }
    {  // the host-lost timer of an earlier stretch does not run on after StartAccepting
        Rig r(1);
        Con& h = r[0];
        OpenHost(h, 3);
        CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
        CHECK_EQ(h.lobby.ServerState(), 1);
        h.net.MyNodeZero(true);  // the engine loses our node (Members() still lists us)
        r.Run(1500);
        CHECK_EQ(SSt(h), 1);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), 0);  // the network is open: HostUpdate; the timer restarts
        CHECK_EQ(h.lobby.ServerState(), 2);
        r.Run(1000);  // 2500 ms since the node went, 1000 since StartAccepting
        CHECK_EQ(SSt(h), 2);
        CHECK_EQ(h.net.HostCloseCalls(), 0);
        r.Run(1100);  // 2100 ms since StartAccepting: lost
        CHECK_EQ(SSt(h), 1);
        CHECK_EQ(h.net.HostCloseCalls(), 1);
    }
    {  // StartAccepting(manual) then EndAccepting then StartAccepting(auto): the second round accepts by itself
        Rig r(2);
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0);
        CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), 0);  // (no Initialize in between)
        CHECK(Enter(r, 1, 0));
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 5);  // accepted, not 0x40
    }
    {  // EndAccepting then StartAccepting: the network stays open (HostUpdate, not HostOpen) and the table of clients is the same one
        Rig r(2);
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0);
        CHECK(Enter(r, 1, 0));
        CHECK_EQ(h.lobby.ServerAcceptClient(2), 0);
        CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
        CHECK_EQ(h.lobby.ServerState(), 1);
        CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0);
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);  // (no poll in between: nothing could have been re-learned)
        CHECK_EQ(w >> 24, 5);                           // still the accepted client, not a new one waiting at 0x40
        CHECK_EQ(h.net.HostOpenCalls(), 1);
        CHECK_EQ(h.net.HostUpdateCalls(), 2);
    }
    {  // and the other way: auto, then manual
        Rig r(2);
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(h.lobby.ServerEndAccepting(), 0);
        CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0);
        CHECK(Enter(r, 1, 0));
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 0x40);
    }
}

/// The creator's commands poll first: a client admitted a moment ago (less than kPollMs since the creator's last poll) is
/// already known; and the completion is recomputed in the same call.
static void TestCreatorCommandsPollFirst() {
    std::printf("creator commands poll first\n");
    for (int what = 0; what < 2; ++what) {  // 0 AcceptClient, 1 DisconnectClient
        Rig r(2);
        OpenHost(r[0], 3, what == 0);
        Con& c = r[1];
        c.lobby.ClientInitialize(nullptr, 16);
        c.lobby.ClientStartScan(0);
        r.Run(60);
        c.lobby.ClientStopScan();
        u32 res = 77;
        CHECK(c.lobby.ClientJoinBegin(MacOf(r[0]), &res) == JoinStep::Pending);
        r.air.Advance(5);
        CHECK(c.lobby.ClientJoinPoll(&res));  // the engine admitted us; the creator has not polled since
        CHECK_EQ(res, 0);
        if (what == 0) {
            CHECK_EQ(r[0].lobby.ServerAcceptClient(2), 0);
            u32 w = 0;
            CHECK_EQ(r[0].lobby.ServerClientState(2, &w), 0);
            CHECK_EQ(w >> 24, 5);
        } else {
            CHECK_EQ(r[0].lobby.ServerDisconnectClient(2), 0);
            CHECK_EQ(r[0].net.Kicked(2), 1);
        }
    }
    {  // SendPassphrase: a client that left a moment ago is not written to, and with nobody left the state is 9 at once
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0);
        CHECK(r.RunUntil([&] { return SSt(h) == 8; }, 2000));
        r.air.Leave(0, 2);  // the client falls off without a word
        const int sends = h.net.SendCalls();
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(h.lobby.ServerState(), 9);
        CHECK_EQ(h.net.SendCalls(), sends);
        CHECK_EQ(h.net.HostCloseCalls(), 1);
    }
    {  // DisconnectClient in phase 6 on the only client still owing START_ACK: 8 in the same call
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        r.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0);
        CHECK_EQ(h.lobby.ServerState(), 6);
        r.Run(100);
        CHECK_EQ(SSt(h), 6);
        CHECK_EQ(h.lobby.ServerDisconnectClient(2), 0);
        CHECK_EQ(h.lobby.ServerState(), 8);
    }
}

/// Poll: the first poll runs even when the clock is below kPollMs; at most kMaxRx messages per poll; the counter of refused
/// messages saturates; messages from nodes that cannot be clients are counted and change nothing.
static void TestPollEdges() {
    std::printf("poll edges\n");
    {  // a clock that starts at 5 ms: the very first Pump must run
        Rig r(1, 5);
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        s.lobby.Pump();
        CHECK(s.net.ScanReadCalls() >= 1);
        const int reads = s.net.ScanReadCalls();
        s.lobby.Pump();  // no time passed: rate limited
        CHECK_EQ(s.net.ScanReadCalls(), reads);
    }
    for (int role = 0; role < 2; ++role) {  // 0 searcher, 1 creator: nine messages queued, eight per poll
        Rig r(1);
        Con& c = r[0];
        if (role == 0) {
            c.lobby.ClientInitialize(nullptr, 16);
            CHECK_EQ(c.lobby.ClientStartScan(0), 0);
        } else {
            OpenHost(c, 3);
        }
        const u8 junk[2] = {9, 0};
        for (int i = 0; i < 9; ++i) {
            r.air.Inject(0, 1, junk, 2);
        }
        c.lobby.Poll(true);
        CHECK_EQ(c.lobby.bad_messages(), 8);
        c.lobby.Poll(true);
        CHECK_EQ(c.lobby.bad_messages(), 9);
        c.lobby.Poll(true);
        CHECK_EQ(c.lobby.bad_messages(), 9);
    }
    {  // the counter saturates at 0xFFFFFFFF
        Rig r(1);
        Con& c = r[0];
        c.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(c.lobby.ClientStartScan(0), 0);
        const u8 junk[2] = {9, 0};
        c.lobby.*Steal(BadMessagesTag()) = 0xFFFFFFFEu;
        r.air.Inject(0, 1, junk, 2);
        c.lobby.Poll(true);
        CHECK_EQ(c.lobby.bad_messages(), 0xFFFFFFFFu);
        r.air.Inject(0, 1, junk, 2);
        c.lobby.Poll(true);
        CHECK_EQ(c.lobby.bad_messages(), 0xFFFFFFFFu);  // it stays
    }
}

/// The creator's table of acknowledgements: who may say what, when. Everything refused is counted and changes nothing.
static void TestAckTable() {
    std::printf("ack table\n");
    const u8 start_ack[2] = {2, 0};
    const u8 go_ack[2] = {4, 0};
    {  // in state 2 an ack of either kind is out of place; nodes 0, 1, 5, 255 cannot be clients
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        u32 bad = h.lobby.bad_messages();
        r.air.Inject(0, 2, start_ack, 2);
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), ++bad);
        r.air.Inject(0, 2, go_ack, 2);
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), ++bad);
        const int from[4] = {0, 1, 5, 255};
        for (int k = 0; k < 4; ++k) {
            r.air.Inject(0, from[k], start_ack, 2);
            h.lobby.Poll(true);
            CHECK_EQ(h.lobby.bad_messages(), ++bad);
        }
        CHECK_EQ(h.lobby.ServerState(), 2);
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 5);
    }
    {  // manual mode: a client still waiting for acceptance cannot acknowledge anything
        Rig r(2);
        OpenHost(r[0], 3, true);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 0x40);
        u32 bad = h.lobby.bad_messages();
        r.air.Inject(0, 2, start_ack, 2);
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), ++bad);
        r.air.Inject(0, 2, go_ack, 2);
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), ++bad);
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 0x40);  // not promoted
    }
    {  // state 8 with the GO sent and one client still owing GO_ACK: a late START_ACK and a second GO_ACK are fine, not errors
        Rig r(3);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        Con& h = r[0];
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0);
        CHECK(r.RunUntil([&] { return SSt(h) == 8; }, 2000));
        r.air.Drop(T_GO_ACK, 3, FakeAir::kAny, FakeAir::kForever);  // node 3 never confirms
        CHECK_EQ(h.lobby.ServerSendPassphrase(kPass1), 0);
        r.Run(300);
        CHECK_EQ(h.lobby.ServerState(), 8);  // waiting for node 3
        u32 w = 0;
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 9);
        const u32 bad = h.lobby.bad_messages();
        r.air.Inject(0, 2, start_ack, 2);  // late: START_ACK in state 8
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), bad);
        r.air.Inject(0, 2, go_ack, 2);  // a duplicate GO_ACK from a client already at GO_ACK
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), bad);
        CHECK_EQ(h.lobby.ServerClientState(2, &w), 0);
        CHECK_EQ(w >> 24, 9);
        CHECK_EQ(h.lobby.ServerState(), 8);
    }
    {  // an ejected client that lingers in the engine cannot acknowledge, and does not come back as a client
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        r.air.SetKickLag(100000);
        CHECK_EQ(h.lobby.ServerDisconnectClient(2), 0);
        CHECK_EQ(NClients(h), 0);
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0);
        CHECK_EQ(h.lobby.ServerState(), 8);  // nobody owes START_ACK
        const u32 bad = h.lobby.bad_messages();
        r.air.Inject(0, 2, start_ack, 2);
        h.lobby.Poll(true);
        CHECK_EQ(h.lobby.bad_messages(), bad + 1);
        CHECK_EQ(NClients(h), 0);
    }
}

/// The creator loses its own network: state 1 and nothing more is done in that poll (no Kick of a node it no longer owns).
static void TestCreatorNetworkLost() {
    std::printf("creator network lost\n");
    Rig r(2);
    OpenHost(r[0], 3);
    CHECK(Enter(r, 1, 0));
    Con& h = r[0];
    h.net.MyNodeZero(true);
    r.Run(1900);
    CHECK_EQ(SSt(h), 2);
    r.Run(200);
    CHECK_EQ(SSt(h), 1);
    CHECK_EQ(h.net.HostCloseCalls(), 1);
    CHECK_EQ(h.net.Kicked(2), 0);  // the node 2 of the old network is not ejected after the network is gone
    CHECK(!h.lobby.active());
}

/// Re-sends of Kick: an ejected node that lingers is kicked again every kResendMs exactly; an id above 4 too; id 0 never.
static void TestKickTimings() {
    std::printf("kick timings\n");
    {  // a node ejected with DisconnectClient that the engine keeps for a while
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& h = r[0];
        r.air.SetKickLag(100000);
        CHECK_EQ(h.lobby.ServerDisconnectClient(2), 0);
        CHECK_EQ(h.net.Kicked(2), 1);
        int at[3] = {0, 0, 0};  // the count after 199, 200 and 399 ms
        for (int ms = 1; ms <= 400; ++ms) {
            r.air.Advance(1);
            h.lobby.Poll(true);
            if (ms == 199) at[0] = h.net.Kicked(2);
            if (ms == 200) at[1] = h.net.Kicked(2);
            if (ms == 399) at[2] = h.net.Kicked(2);
        }
        CHECK_EQ(at[0], 1);
        CHECK_EQ(at[1], 2);
        CHECK_EQ(at[2], 2);
        CHECK_EQ(h.net.Kicked(2), 3);  // at 400
    }
    {  // an id above 4 (a faulty engine): kicked at once, then every 200 ms exactly
        Rig r(1);
        OpenHost(r[0], 3);
        Con& h = r[0];
        r.air.Phantom(0, 7, 0x77, "P7");
        r.air.SetKickLag(100000);
        h.lobby.Poll(true);
        CHECK_EQ(h.net.Kicked(7), 1);
        int at[3] = {0, 0, 0};
        for (int ms = 1; ms <= 400; ++ms) {
            r.air.Advance(1);
            h.lobby.Poll(true);
            if (ms == 199) at[0] = h.net.Kicked(7);
            if (ms == 200) at[1] = h.net.Kicked(7);
            if (ms == 399) at[2] = h.net.Kicked(7);
        }
        CHECK_EQ(at[0], 1);
        CHECK_EQ(at[1], 2);
        CHECK_EQ(at[2], 2);
        CHECK_EQ(h.net.Kicked(7), 3);
    }
    {  // the ids 5 and 6 are kicked as well (the lowest id above 4 comes first; one Kick per poll at most)
        const int ids[2] = {5, 6};
        for (int k = 0; k < 2; ++k) {
            Rig r(1);
            OpenHost(r[0], 3);
            Con& h = r[0];
            h.net.ForceMembers({1, ids[k]});
            h.lobby.Poll(true);
            CHECK_EQ(h.net.Kicked(ids[k]), 1);
            CHECK_EQ(h.net.Kicked(ids[1 - k]), 0);
            CHECK_EQ(h.lobby.ServerState(), 2);
        }
    }
    {  // id 0 is not a node: never passed to the engine; the real client (node 2) is tracked as always
        Rig r(1);
        OpenHost(r[0], 3);
        Con& h = r[0];
        h.net.ForceMembers({1, 0, 2});
        for (int i = 0; i < 100; ++i) {
            r.air.Advance(20);
            h.lobby.Poll(true);
        }
        CHECK_EQ(h.net.Kicked(0), 0);
        CHECK_EQ(h.net.Kicked(1), 0);
        CHECK_EQ(h.net.Kicked(2), 0);
        CHECK_EQ(NClients(h), 1);
        CHECK_EQ(h.lobby.ServerState(), 2);
    }
}

/// The searcher's session: a host whose node the engine no longer reports (MyNode() = 0) is lost even if Members() still
/// lists it; the roster is re-read every kRosterMs exactly; a failing ReadNode leaves a zero entry; an empty answer keeps the
/// last roster; the player count is limited to what the host announced; an engine that over-reports is cut to four.
static void TestSessionGaps() {
    std::printf("session gaps\n");
    for (int st = 5; st <= 6; ++st) {  // host lost by MyNode() == 0, in 5 and in 6
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        if (st == 6) {
            CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
            r.Run(100);
            CHECK_EQ(CSt(s), 6);
        }
        s.net.MyNodeZero(true);
        r.Run(1900);
        CHECK_EQ(CSt(s), static_cast<u32>(st));
        r.Run(200);
        CHECK_EQ(CSt(s), 0x80);
    }
    for (int st = 5; st <= 6; ++st) {  // the roster is read every 250 ms exactly, in 5 and in 6
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        if (st == 6) {
            CHECK_EQ(r[0].lobby.ServerStartDistribution(), 0);
            r.Run(100);
            CHECK_EQ(CSt(s), 6);
        }
        std::vector<u64> reads;
        int last = s.net.ReadNodeCalls();
        for (int ms = 0; ms < 1300; ++ms) {
            r.air.Advance(1);
            s.lobby.Poll(true);
            if (s.net.ReadNodeCalls() != last) {
                last = s.net.ReadNodeCalls();
                reads.push_back(r.air.now());
            }
        }
        CHECK(reads.size() >= 5);
        for (size_t i = 1; i < reads.size(); ++i) {
            CHECK_EQ(reads[i] - reads[i - 1], 250);
        }
    }
    {  // four players: every searcher sees all four after a roster round
        Rig r(4);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        CHECK(Enter(r, 2, 0));
        CHECK(Enter(r, 3, 0));
        r.Run(400);
        for (int i = 1; i <= 3; ++i) {
            CHECK_EQ(NNodes(r[i]), 4);
            u16 ids[4] = {};
            u32 total = 0;
            CHECK_EQ(r[i].lobby.ClientConnectingNodes(ids, 4, &total), 0);
            CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 4);
        }
    }
    {  // an engine that reports more members than the array holds (7 for 4 written): cut to four, nothing read past them
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        r.Run(300);
        s.net.ForceMembers({2, 3, 4, 2, 3, 4, 2});  // (the host is not among the first four: Contains() must stop at four)
        r.Run(300);
        CHECK_EQ(NNodes(s), 4);
        s.net.UnforceMembers();
        r.Run(300);
        CHECK_EQ(NNodes(s), 2);
        CHECK_EQ(CSt(s), 5);
    }
    {  // an empty answer from the engine (one re-read) keeps the roster we had
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        r.Run(300);
        CHECK_EQ(NNodes(s), 2);
        s.net.ForceMembers({});
        r.Run(400);
        CHECK_EQ(NNodes(s), 2);
        CHECK_EQ(CSt(s), 5);
        s.net.UnforceMembers();
        r.Run(300);
        CHECK_EQ(NNodes(s), 2);
    }
    {  // a ReadNode that fails for a node read before: the entry goes to zero, the id stays
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        r.Run(300);
        NodeEntry e;
        CHECK_EQ(s.lobby.ClientNodeInfo(1, &e), 0);
        CHECK_EQ(e.seed, 0x1000);
        CHECK_STR(NameOf(e.name), "Host");
        s.net.FailReadNode(1, true);
        r.Run(300);
        std::memset(&e, 0x77, sizeof(e));
        CHECK_EQ(s.lobby.ClientNodeInfo(1, &e), 0);
        CHECK_EQ(e.node, 1);
        CHECK_EQ(e.seed, 0);
        CHECK_STR(NameOf(e.name), "");
        CHECK_EQ(NNodes(s), 2);
        s.net.FailReadNode(1, false);
        r.Run(300);
        CHECK_EQ(s.lobby.ClientNodeInfo(1, &e), 0);
        CHECK_EQ(e.seed, 0x1000);
    }
    {  // the players seen never exceed what the host announced (3), even if the engine lists four
        Rig r(2);
        OpenHost(r[0], 2);  // max 2 clients: max_nodes 3
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        r.air.Phantom(0, 3, 0x333, "P3");
        r.air.Phantom(0, 4, 0x444, "P4");
        r.air.Advance(300);
        s.lobby.Poll(true);  // (the creator is not polled: it would eject the phantoms)
        GroupView v;
        CHECK_EQ(s.lobby.ClientServerInfo(MacOf(r[0]), &v), 0);
        CHECK_EQ(v.group->max_nodes, 3);
        CHECK_EQ(v.group->total_nodes, 3);
        u16 ids[4] = {};
        u32 total = 0;
        CHECK_EQ(s.lobby.ClientConnectingNodes(ids, 4, &total), 0);
        CHECK_EQ(total, 4);  // the roster itself lists what the engine lists
    }
    {  // a node id of 0xFF is a valid id of the roster; above 0xFF is not
        Rig r(2);
        OpenHost(r[0], 3);
        CHECK(Enter(r, 1, 0));
        Con& s = r[1];
        r.Run(300);
        s.net.ForceMembers({1, 255});
        r.Run(300);
        NodeEntry e;
        CHECK_EQ(s.lobby.ClientNodeInfo(255, &e), 0);
        CHECK_EQ(e.node, 255);
        CHECK_EQ(s.lobby.ClientNodeInfo(0x1FF, &e), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientNodeInfo(0x100, &e), R_NO_DATA);
        CHECK_EQ(s.lobby.ClientNodeInfo(0x101, &e), R_NO_DATA);  // (a cast to a byte would say node 1)
        s.net.UnforceMembers();
    }
}

/// Joining the lobby that sits in scan slot k: the entry of the host moves to the front of the table whole, nothing of the
/// other entries is left to read as a roster, and the others do not reappear.
static void TestEnterSessionSwap() {
    std::printf("enter session (swap)\n");
    for (int slot = 0; slot < 4; ++slot) {
        Rig r(5);  // console 0 searches; consoles 1..4 are four hosts, heard in this order (slots 0..3)
        for (int h = 1; h <= 4; ++h) {
            u16 name[10];
            FakeAir::Name(name, kLong[h - 1]);
            CHECK_EQ(r[h].lobby.ServerInitialize(h == 3 ? 2 : 3, static_cast<u32>(0x10 + h), name), 0);
            CHECK_EQ(r[h].lobby.ServerStartAccepting(false, 6), 0);
        }
        Con& s = r[0];
        s.lobby.ClientInitialize(nullptr, 16);
        CHECK_EQ(s.lobby.ClientStartScan(0), 0);
        r.Run(100);
        CHECK_EQ(CountEntries(s), 4);
        GroupView v;
        CHECK_EQ(s.lobby.ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group->host == MacOf(r[1]));  // slot 0 is the first console
        s.lobby.ClientStopScan();
        const int hi = slot + 1;
        u32 res = 77;
        CHECK(s.lobby.ClientJoinBegin(MacOf(r[hi]), &res) == JoinStep::Pending);
        bool done = false;
        CHECK(r.RunUntil(
            [&] {
                if (!done) {
                    done = s.lobby.ClientJoinPoll(&res);
                }
                return done;
            },
            4000));
        CHECK_EQ(res, 0);
        CHECK_EQ(CSt(s), 5);
        CHECK_EQ(s.lobby.ClientServerInfo(MacOf(r[hi]), &v), 0);
        CHECK(v.group->host == MacOf(r[hi]));
        CHECK_STR(NameOf(v.group->name), kLong[slot]);
        CHECK_EQ(v.group->max_nodes, hi == 3 ? 3 : 4);
        CHECK_EQ(v.group->child_tid_low, 0x0010C000u + 0x10u + static_cast<u32>(hi));
        CHECK_EQ(v.group->total_nodes, 2);
        CHECK_EQ(v.group->signal, 3);
        for (int other = 1; other <= 4; ++other) {
            if (other != hi) {
                CHECK_EQ(s.lobby.ClientServerInfo(MacOf(r[other]), &v), R_NO_DATA);
            }
        }
        u16 ids[4] = {};
        u32 total = 0;
        CHECK_EQ(s.lobby.ClientConnectingNodes(ids, 4, &total), 0);
        CHECK_EQ(total, 2);
        CHECK(ids[0] == 1 && ids[1] == 2);
        NodeEntry e;
        CHECK_EQ(s.lobby.ClientNodeInfo(1, &e), 0);
        CHECK_STR(NameOf(e.name), kLong[slot]);
        CHECK_EQ(e.seed, 0x1000u + static_cast<u32>(hi));
    }
    {  // the roster is empty at the join (the engine says nothing yet): the bytes of the other scan entries are not nodes
        for (int slot = 1; slot < 4; ++slot) {
            Rig r(5);
            for (int h = 1; h <= 4; ++h) {
                u16 name[10];
                FakeAir::Name(name, kLong[h - 1]);
                CHECK_EQ(r[h].lobby.ServerInitialize(3, 0, name), 0);
                CHECK_EQ(r[h].lobby.ServerStartAccepting(false, 6), 0);
            }
            Con& s = r[0];
            s.lobby.ClientInitialize(nullptr, 16);
            CHECK_EQ(s.lobby.ClientStartScan(0), 0);
            r.Run(100);
            CHECK_EQ(CountEntries(s), 4);
            s.lobby.ClientStopScan();
            s.net.ForceMembers({});
            u32 res = 77;
            CHECK(s.lobby.ClientJoinBegin(MacOf(r[slot + 1]), &res) == JoinStep::Pending);
            CHECK(r.RunUntil([&] { return s.lobby.client_state() == 5; }, 4000));
            CHECK_EQ(NNodes(s), 0);
            s.net.UnforceMembers();
        }
    }
}

/// A seeker in the session whose host vanishes while the rest is as usual is covered elsewhere; here: the creator tolerates a
/// repeated StartAccepting error order (state 2 -> kInvalidState), and the order of the checks of StartAccepting.
static void TestDriverOrder() {
    std::printf("driver order\n");
    Rig r(1);
    Con& h = r[0];
    CHECK_EQ(h.lobby.ServerInitialize(3, 0, nullptr), 0);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 5), R_OUT_OF_RANGE);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), 0);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 6), R_INVALID_STATE);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 5), R_OUT_OF_RANGE);  // the channel is checked before the state
}

/// A creator that is the only member keeps Kick(node) off ids that are not its clients; the host record (node 1) of
/// ServerClientInfo comes back with node 1 and kNoData; out->node is 0 in every other return.
static void TestClientInfoNode() {
    std::printf("client info node\n");
    Rig r(2);
    Con& h = r[0];
    NodeEntry e;
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(2, &e), R_INVALID_STATE);  // state 0
    CHECK_EQ(e.node, 0);
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(1, &e), R_INVALID_STATE);
    CHECK_EQ(e.node, 0);
    OpenHost(h, 3);
    CHECK(Enter(r, 1, 0));
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(1, &e), R_NO_DATA);  // the host: the record is filled
    CHECK_EQ(e.node, 1);
    CHECK_EQ(e.seed, 0x1000);
    CHECK_STR(NameOf(e.name), "Host");
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(3, &e), R_NO_DATA);  // nobody there
    CHECK_EQ(e.node, 0);
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(0x0102, &e), R_NO_DATA);
    CHECK_EQ(e.node, 0);
    std::memset(&e, 0x77, sizeof(e));
    CHECK_EQ(h.lobby.ServerClientInfo(2, &e), 0);
    CHECK_EQ(e.node, 2);
}

/// Kicking: the kBusy of ClientJoinBegin cannot be reached (the creator holding the network means an empty table): the
/// answer is kNoData and the engine is not touched.
static void TestJoinWithCreatorActive() {
    std::printf("join with the creator active\n");
    Rig r(1);
    Con& c = r[0];
    const Mac ghost = Mac{0x00, 0x1F, 0x32, 0xA0, 0xB0, 0x09};
    r.air.AddAir(ghost, Ann(0x0010C000, "Ghost", 4, 1), 1);
    c.lobby.ClientInitialize(nullptr, 16);
    c.lobby.ClientStartScan(0);
    r.Run(60);
    c.lobby.ClientStopScan();
    OpenHost(c, 3);  // takes the network: the frozen table is discarded
    const int calls = c.net.AllCalls();
    u32 res = 0;
    CHECK(c.lobby.ClientJoinBegin(ghost, &res) == JoinStep::Failed);
    CHECK_EQ(res, R_NO_DATA);
    CHECK_EQ(c.net.AllCalls(), calls);
    CHECK_EQ(c.net.ConnectBeginCalls(), 0);
    CHECK_EQ(c.lobby.ClientStartScan(0), R_BUSY);
    CHECK_EQ(c.net.AllCalls(), calls);
}

int main() {
    TestCodec();
    TestSizes();
    TestScanTable();
    TestCursorAndExpiry();
    TestViewTick();
    TestJoin();
    TestSession();
    TestCreator();
    TestCreator2();
    TestCreator3();
    TestTimes();
    TestTimes2();
    TestTimes3();
    TestClockOffsetsAndDeterminism();
    TestLoss();
    TestAdmissionTable();
    TestGameUdsStarting();
    TestThreads();
    TestPossession();
    TestTwoLobbies();
    TestSearcherMatrix();
    TestCreatorMatrix();
    TestAzaharDefects();
    TestFullRounds();
    TestCodecEdges();
    TestScanGaps();
    TestPassphraseOwner();
    TestJoinCancel();
    TestCreatorTables();
    TestCreatorCommandsPollFirst();
    TestPollEdges();
    TestAckTable();
    TestCreatorNetworkLost();
    TestKickTimings();
    TestSessionGaps();
    TestEnterSessionSwap();
    TestDriverOrder();
    TestClientInfoNode();
    TestJoinWithCreatorActive();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
