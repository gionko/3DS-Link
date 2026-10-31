// The fake Download Play lobby over a REAL azahar-room server (see Tests/integration.sh): the same consoles as Tests/dlp_net_test.cpp, but
// every one is a genuine RoomClient (ENet) + UdsEngine + EngineLobbyNet + DlpLobby, so the frames take the room's wire, its routing and
// its delays. The only thing not real is the game (this program plays what Triple Deluxe's lobby asks of its commands).
//
//   dlp_room_probe <room-host> <room-port>
//
// A test that cannot get a player into the room says so and stops (a room that is not there is not a failure of the lobby).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_lobby_net.hpp"
#include "azl/room_client.hpp"
#include "azahar_rules.hpp"
#include "support.hpp"

using namespace azl;

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kTidLow = 0x0010C000;
static const u8 kNine[9] = {'1', 'a', '2', 'b', '3', 'c', '4', 'd', 0};

template <typename Pred>
static bool WaitFor(Pred pred, int timeout_ms = 5000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static void Put(u16* dst, const char* s) {
    for (u32 i = 0; i < kNameUnits; ++i) {
        dst[i] = static_cast<u16>(static_cast<u8>(s[i]));
        if (!s[i]) {
            for (u32 j = i + 1; j < kNameUnits; ++j) {
                dst[j] = 0;
            }
            return;
        }
    }
}
static std::string Str(const u16* n) {
    std::string s;
    for (u32 i = 0; i < kNameUnits && n[i]; ++i) {
        s += static_cast<char>(n[i]);
    }
    return s;
}

struct Node {
    Node(const std::string& host, u16 port, const char* nick, u8 index) : name(nick) {
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.console_id_hash = std::string("lobby-probe-") + nick;
        config.game_name = "Test Game";
        config.game_id = 0x0004000000164800ULL;
        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (!room.joined()) {
                return;
            }
            p.transmitter_address = room.mac();
            g_rules.Inspect(p);
            room.SendWifiPacket(p);
        };
        transport.get_mac = [this] { return room.mac(); };
        engine.reset(new UdsEngine(&kernel, transport));
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetTickCallback([this](u64 now_ms) { engine->Tick(now_ms); });
        net.reset(new EngineLobbyNet(engine.get()));
        LobbyConfig c;
        std::memset(&c, 0, sizeof(c));
        c.game_tid = kTidLow;
        c.friend_seed = 0xF000u + index;
        Put(c.name, nick);
        lobby.reset(new DlpLobby(net.get(), c));
    }
    ~Node() {
        room.Stop();
    }
    bool Join() {
        room.Start(config);
        const bool joined = WaitFor([this] {
            return room.state() == RoomClient::State::Joined ||
                   (room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None);
        }, 8000) && room.joined();
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    UdsEngine& e() {
        return *engine;
    }
    Mac mac() {
        return room.mac();
    }
    std::string name;
    RoomClient::Config config;
    RoomClient room;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
    std::unique_ptr<EngineLobbyNet> net;
    std::unique_ptr<DlpLobby> lobby;
};

using Nodes = std::vector<Node*>;

template <typename F>
static bool Until(const Nodes& ns, u32 ms, F done) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        for (Node* n : ns) {
            n->lobby->Pump();
        }
        if (done()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static u32 CliState(Node& n) {
    return n.lobby->ClientStatusWord() >> 24;
}
static u32 CliNode(Node& n) {
    return n.lobby->ClientStatusWord() & 0xFF;
}
static bool Clean(Node& n) {
    return !n.e().initialized() && !n.net->engine_held();
}
static u32 Clients(Node& host) {
    u16 ids[4];
    u32 total = 0;
    host.lobby->ServerConnectingClients(ids, 4, &total);
    return total;
}

static void OpenLobby(Node& host) {
    CHECK_EQ(host.lobby->ServerInitialize(3, 0, nullptr), 0u);
    CHECK_EQ(host.lobby->ServerStartAccepting(false, 6), 0u);
}

static bool Find(const Nodes& ns, Node& guest, Node& host) {
    CHECK_EQ(guest.lobby->ClientInitialize(nullptr, 4), 0u);
    CHECK_EQ(guest.lobby->ClientStartScan(0), 0u);
    return Until(ns, 6000, [&] {
        GroupView v;
        return guest.lobby->ClientTitleInfo(host.mac(), &v) == 0;
    });
}

static bool Join(const Nodes& ns, Node& guest, Node& host, u32* result) {
    guest.lobby->ClientStopScan();
    *result = 0xFFFFFFFF;
    if (guest.lobby->ClientJoinBegin(host.mac(), result) == JoinStep::Failed) {
        return true;
    }
    return Until(ns, 5000, [&] { return guest.lobby->ClientJoinPoll(result); });
}

/// Starts the creator's distribution and gives the passphrase when the game would: every console ends at 9.
static void FinishLobby(const Nodes& ns, Node& host, const Nodes& guests) {
    CHECK_EQ(host.lobby->ServerStartDistribution(), 0u);
    CHECK(Until(ns, 5000, [&] { return host.lobby->ServerState() == 8; }));
    CHECK_EQ(host.lobby->ServerSendPassphrase(kNine), 0u);
    CHECK(Until(ns, 8000, [&] {
        if (host.lobby->ServerState() != 9) {
            return false;
        }
        for (Node* g : guests) {
            if (CliState(*g) != 9) {
                return false;
            }
        }
        return true;
    }));
    CHECK(Clean(host));
    for (Node* g : guests) {
        u8 pass[9] = {};
        g->lobby->ClientPassphrase(pass);
        CHECK(std::memcmp(pass, kNine, 9) == 0);
        CHECK_EQ(g->lobby->ClientFinalize(), 0u);  // (what the game does before its own UDS)
        CHECK(Clean(*g));
    }
}

static bool TwoInRoom(const std::string& host, u16 port, Node& a, Node& b) {
    (void)host;
    (void)port;
    if (!a.Join() || !b.Join()) {
        std::fprintf(stderr, "  (cannot continue without both players in the room)\n");
        CHECK(false);  // (a room that cannot be reached proves nothing: it is a failure, not a pass)
        return false;
    }
    return true;
}

static void TestLobbyThenGameUds(const std::string& host, u16 port) {
    std::printf("whole lobby over the real room, then the game's UDS\n");
    g_rules.Reset();
    Node a(host, port, "LobbyA1", 1), b(host, port, "LobbyB1", 2);
    if (!TwoInRoom(host, port, a, b)) {
        return;
    }
    const Nodes ns = {&a, &b};
    OpenLobby(a);
    CHECK(Find(ns, b, a));
    GroupView v;
    CHECK_EQ(b.lobby->ClientTitleInfo(a.mac(), &v), 0u);
    CHECK_EQ(v.group->max_nodes, 4u);
    CHECK(Str(v.group->name) == "LobbyA1");
    u32 res = 0;
    CHECK(Join(ns, b, a, &res));
    CHECK_EQ(res, 0u);
    CHECK_EQ(CliState(b), 5u);
    CHECK_EQ(CliNode(b), 2u);
    CHECK(Until(ns, 3000, [&] { return Clients(a) == 1; }));
    NodeEntry ne;
    CHECK_EQ(a.lobby->ServerClientInfo(2, &ne), 0u);
    CHECK(Str(ne.name) == "LobbyB1");
    FinishLobby(ns, a, {&b});

    // the game's own UDS on the same engines, over the same room, with its own ids
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(a.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    std::vector<u8> out(0x4000);
    ScanParams sp;
    sp.wlan_comm_id = 0x0010CAFE;
    sp.id8 = 1;
    sp.channel_mask = 0x0001;
    sp.dwell_ms = 250;
    u32 entries = 0;
    CHECK(WaitFor([&] {
        b.e().RecvBeaconBroadcastData(out.data(), static_cast<u32>(out.size()), sp);
        entries = LoadLE32(out.data() + 8);
        return entries > 0;
    }, 6000));
    CHECK_EQ(entries, 1u);
    u8 info[0x3C] = {};
    std::memcpy(info, a.mac().data(), 6);
    info[6] = 11;
    CHECK_EQ(b.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
    NodeInfo got;
    CHECK_EQ(b.e().GetNodeInformation(1, &got), 0u);
    CHECK(Str(got.username) == "GameHost");
    CHECK_EQ(got.friend_code_seed, 0xA1A1u);
    CHECK_RULES();
}

static void TestFourPlayers(const std::string& host, u16 port) {
    std::printf("four players over the real room\n");
    g_rules.Reset();
    Node a(host, port, "Four1", 1), b(host, port, "Four2", 2), c(host, port, "Four3", 3), d(host, port, "Four4", 4);
    if (!a.Join() || !b.Join() || !c.Join() || !d.Join()) {
        std::fprintf(stderr, "  (cannot continue without the four players in the room)\n");
        CHECK(false);
        return;
    }
    const Nodes ns = {&a, &b, &c, &d};
    OpenLobby(a);
    const Nodes guests = {&b, &c, &d};
    for (Node* g : guests) {
        CHECK(Find(ns, *g, a));
    }
    u32 res = 0;
    for (Node* g : guests) {
        CHECK(Join(ns, *g, a, &res));
        CHECK_EQ(res, 0u);
    }
    CHECK_EQ(CliNode(b), 2u);
    CHECK_EQ(CliNode(c), 3u);
    CHECK_EQ(CliNode(d), 4u);
    CHECK(Until(ns, 4000, [&] { return Clients(a) == 3; }));
    for (Node* g : guests) {
        u16 ids[4];
        u32 total = 0;
        CHECK(Until(ns, 4000, [&] {
            g->lobby->ClientConnectingNodes(ids, 4, &total);
            return total == 4;
        }));
    }
    FinishLobby(ns, a, guests);
    CHECK_RULES();
}

static void TestLeaveAndGone(const std::string& host, u16 port) {
    std::printf("a client leaves, the creator goes (real room)\n");
    g_rules.Reset();
    Node a(host, port, "Gone1", 1), b(host, port, "Gone2", 2), c(host, port, "Gone3", 3);
    if (!a.Join() || !b.Join() || !c.Join()) {
        std::fprintf(stderr, "  (cannot continue without the three players in the room)\n");
        CHECK(false);
        return;
    }
    const Nodes ns = {&a, &b, &c};
    OpenLobby(a);
    CHECK(Find(ns, b, a));
    CHECK(Find(ns, c, a));
    u32 res = 0;
    CHECK(Join(ns, b, a, &res));
    CHECK(Join(ns, c, a, &res));
    CHECK(Until(ns, 4000, [&] { return Clients(a) == 2; }));
    CHECK_EQ(b.lobby->ClientStopSession(), 0u);
    CHECK(Clean(b));
    CHECK(Until(ns, 4000, [&] { return Clients(a) == 1; }));
    CHECK_EQ(a.lobby->ServerFinalize(), 0u);
    CHECK(Clean(a));
    CHECK(Until(ns, 5000, [&] { return CliState(c) == 0x80; }));
    CHECK(Clean(c));
    CHECK_RULES();
}

static std::string RosterOf(Node& n) {
    u16 ids[4] = {};
    u32 total = 0;
    n.lobby->ClientConnectingNodes(ids, 4, &total);
    std::string r;
    for (u32 i = 0; i < total && i < 4; ++i) {
        NodeEntry ne;
        r += n.lobby->ClientNodeInfo(ids[i], &ne) == 0 ? Str(ne.name) : "?";
        r += i + 1 < total ? "," : "";
    }
    return r;
}

static void TestTwoLobbies(const std::string& host, u16 port) {
    std::printf("two lobbies of one game in the real room: nobody's roster is rewritten by the other\n");
    g_rules.Reset();
    Node a1(host, port, "TwoA1", 1), b1(host, port, "TwoB1", 2), a2(host, port, "TwoA2", 3), b2(host, port, "TwoB2", 4), b3(host, port, "TwoB3", 5);
    if (!a1.Join() || !b1.Join() || !a2.Join() || !b2.Join() || !b3.Join()) {
        std::fprintf(stderr, "  (cannot continue without the five players in the room)\n");
        CHECK(false);
        return;
    }
    const Nodes ns = {&a1, &b1, &a2, &b2, &b3};
    OpenLobby(a1);
    OpenLobby(a2);
    CHECK(Find(ns, b1, a1));
    u32 res = 0;
    CHECK(Join(ns, b1, a1, &res));
    CHECK_EQ(res, 0u);
    CHECK(Until(ns, 4000, [&] { return RosterOf(b1) == "TwoA1,TwoB1"; }));
    CHECK(Find(ns, b2, a2));
    CHECK(Find(ns, b3, a2));
    CHECK(Join(ns, b2, a2, &res));
    CHECK(Join(ns, b3, a2, &res));
    CHECK(Until(ns, 4000, [&] { return RosterOf(b2) == "TwoA2,TwoB2,TwoB3" && RosterOf(b3) == RosterOf(b2); }));
    Until(ns, 800, [] { return false; });
    CHECK(RosterOf(b1) == "TwoA1,TwoB1");
    CHECK_EQ(Clients(a1), 1u);
    CHECK_EQ(Clients(a2), 2u);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <room-host> <room-port>\n", argv[0]);
        return 2;
    }
    const std::string host = argv[1];
    const u16 port = static_cast<u16>(std::atoi(argv[2]));
    std::printf("dlp_room_probe: the fake Download Play lobby over the real room (%s:%u)\n", host.c_str(), port);
    TestLobbyThenGameUds(host, port);
    TestFourPlayers(host, port);
    TestLeaveAndGone(host, port);
    TestTwoLobbies(host, port);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
