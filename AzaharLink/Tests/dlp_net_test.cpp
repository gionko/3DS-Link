// The fake Download Play lobby over the REAL UDS engine (phase 2 of analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE2.md): the adapter
// azl/dlp_lobby_net.hpp (LobbyNet realised on UdsEngine) and the accessors the engine got for it (BeginHostingLobby,
// HeardLobbyAt, ConnectBegin / ConnectPoll, the lobby's message ring, NodeSnapshot, InitializeForLobby).
//
// Consoles are engines joined by the in-memory room of Tests/mock_room.hpp (the same wire, the same frame encoding and the same
// Azahar-peer rule checker as test_main), each with an EngineLobbyNet and a DlpLobby on top; a thread ticks the engines (the
// beacons) and the test pumps the lobbies, as the plugin's network thread does. The clock is the real one: the lobby's times
// (a join gives up at 3000 ms) are the real times, so a scenario that waits for a timeout takes that long.
//
// Scenarios: the creator is heard by the searcher (and by nobody that looks for a game's network); the join; the whole sequence
// of Triple Deluxe's creator and searcher (START, GO, passphrase) and then, at once, the game's own UDS between the same two
// engines with its own ids, name and seed (the engine is clean after the lobby); an admission that is closed or full (the join
// times out and gives the engine back); four players; a client that leaves and a creator that goes; the game that starts nwm::UDS
// while the lobby is up (OnGameUdsStarting) and the game that has the engine first (the lobby is refused, the game untouched); two
// lobbies in one room (nothing crosses); the messages of strangers (another transmitter, another node, a broadcast, a size outside
// 1..16) and of copies; the ring of 8; calls that allocate nothing; the engine's own guards.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_net_test.cpp Sources/azl/dlp_lobby_net.cpp <the core sources of Tests/run.sh> -o /tmp/dlp_net_test
#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static void TestAnnounceAndScan() {
    std::printf("announce and scan\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));

    OpenLobby(a, 3, 6);
    CHECK(a.net.engine_held());
    CHECK(a.e().status() == NetworkStatus::ConnectedAsHost);
    CHECK(FindGroup(ps, b, a));

    GroupView v;
    CHECK_EQ(b.lobby.ClientTitleInfoInOrder(true, &v), 0u);
    CHECK(v.group->host == a.mac());
    CHECK_EQ(v.group->max_nodes, 4u);
    CHECK_EQ(v.group->total_nodes, 1u);
    CHECK_EQ(v.group->child_tid_low, kTidLow);
    CHECK(Str(v.group->name) == "Alice");
    CHECK_EQ(b.lobby.ClientTitleInfoInOrder(false, &v), R_NO_DATA);  // one lobby
    CHECK(!b.net.engine_held());                                     // looking needs no engine

    // The lobby is not a network of any game: a game's own scan (the engine of C, initialised as a game's would be) does not list it,
    // a scan for anything (wlancommid and id8 all ones) does.
    u32 ev = 0;
    CHECK_EQ(c.e().Initialize(MakeNode(0xC0C0, "Cleo", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    std::vector<u8> out(0x4000);
    ScanParams game;
    game.channel_mask = 0x0001;
    game.dwell_ms = 150;
    game.wlan_comm_id = 0x0010C000;
    game.id8 = 0x01;
    CHECK_EQ(c.e().RecvBeaconBroadcastData(out.data(), static_cast<u32>(out.size()), game), 0u);
    CHECK_EQ(LoadLE32(out.data() + 8), 0u);
    ScanParams any;
    any.channel_mask = 0x0001;
    any.dwell_ms = 150;
    CHECK_EQ(c.e().RecvBeaconBroadcastData(out.data(), static_cast<u32>(out.size()), any), 0u);
    CHECK_EQ(LoadLE32(out.data() + 8), 1u);

    // ... and with the game's session up, C's lobby is refused (the engine is the game's)
    CHECK_EQ(c.lobby.ServerInitialize(3, 0, nullptr), 0u);
    CHECK_EQ(c.lobby.ServerStartAccepting(false, 0), R_BUSY);
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 4), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(0), R_BUSY);
    CHECK(c.e().initialized());  // untouched
    CHECK(!c.net.engine_held());

    // A lobby that stops announcing is forgotten: the creator closes, the searcher's table empties after the entry's time
    CHECK_EQ(a.lobby.ServerFinalize(), 0u);
    CHECK(EngineClean(a));
    CHECK(Until(ps, 3500, [&] {
        GroupView g;
        return b.lobby.ClientTitleInfo(a.mac(), &g) != 0;
    }));
    CHECK_RULES();
}

static void TestJoin() {
    std::printf("join\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    CHECK_EQ(CliState(b), 5u);
    CHECK_EQ(CliNode(b), 2u);
    CHECK(b.net.engine_held());
    CHECK(b.e().status() == NetworkStatus::ConnectedAsClient);

    // the creator tracks the client
    u16 ids[4] = {};
    u32 total = 0;
    CHECK(Until(ps, 2000, [&] {
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 1;
    }));
    CHECK_EQ(ids[0], 2u);
    NodeEntry ne;
    CHECK_EQ(a.lobby.ServerClientInfo(2, &ne), 0u);
    CHECK(Str(ne.name) == "Bob");
    CHECK_EQ(ne.seed, 0xF002u);

    // the client's roster: the host and itself, with the names and seeds the engine carries
    CHECK(Until(ps, 2000, [&] {
        b.lobby.ClientConnectingNodes(ids, 4, &total);
        return total == 2;
    }));
    CHECK_EQ(ids[0], 1u);
    CHECK_EQ(ids[1], 2u);
    CHECK_EQ(b.lobby.ClientNodeInfo(1, &ne), 0u);
    CHECK(Str(ne.name) == "Alice");
    CHECK_EQ(ne.seed, 0xF001u);
    CHECK_EQ(b.lobby.ClientNodeInfo(2, &ne), 0u);
    CHECK(Str(ne.name) == "Bob");
    CHECK_RULES();
}

/// The whole of Triple Deluxe's lobby between two consoles, then the game's own UDS on the same two engines.
static void TestFullSequenceThenGameUds() {
    std::printf("whole lobby, then the game's UDS\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    u16 ids[4];
    u32 total = 0;
    CHECK(Until(ps, 2000, [&] {
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 1;
    }));

    CHECK_EQ(a.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(ps, 3000, [&] { return a.lobby.ServerState() == 8; }));
    CHECK_EQ(CliState(b), 6u);
    CHECK_EQ(a.lobby.ServerSendPassphrase(kNine), 0u);
    CHECK(Until(ps, 4000, [&] { return a.lobby.ServerState() == 9 && CliState(b) == 9; }));
    u8 pass[9] = {};
    CHECK_EQ(b.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, kNine, 9) == 0);
    // The creator saw state 9 only after its network was closed (and the engine is its game's again). The searcher at 9 is final: its
    // network is gone with the creator's, but it keeps the roster and the passphrase - and the engine - until its game finalises
    // (what Triple Deluxe does before its own UDS) or the plugin runs OnGameUdsStarting.
    CHECK(EngineClean(a));
    CHECK(Until(ps, 2000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    CHECK(b.net.engine_held());
    CHECK_EQ(CliState(b), 9u);
    CHECK_EQ(b.lobby.ClientConnectingNodes(ids, 4, &total), 0u);
    CHECK_EQ(total, 2u);
    CHECK_EQ(a.lobby.ServerConnectingClients(ids, 4, &total), 0u);
    CHECK_EQ(total, 0u);
    CHECK(!a.lobby.OnGameUdsStarting());  // nothing left to give back
    CHECK(b.lobby.OnGameUdsStarting());   // the searcher gives the engine back, and stays at 9 with its passphrase
    CHECK(EngineClean(b));
    CHECK_EQ(CliState(b), 9u);
    CHECK_EQ(b.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, kNine, 9) == 0);
    CHECK(!b.lobby.OnGameUdsStarting());  // the second call returns false

    // The game's nwm::UDS now, on the same engines, with ids, names and seeds of its own: Initialize finds a clean service.
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(a.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    // (a host has been beaconing for a while when a game scans for it: the first beacon is 102 ms away)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::vector<u8> out(0x4000);
    ScanParams sp;
    sp.wlan_comm_id = 0x0010CAFE;
    sp.id8 = 1;
    sp.channel_mask = 0x0001;
    sp.dwell_ms = 250;
    CHECK_EQ(b.e().RecvBeaconBroadcastData(out.data(), static_cast<u32>(out.size()), sp), 0u);
    CHECK_EQ(LoadLE32(out.data() + 8), 1u);  // the game's network only: the lobby's old beacons are heard, and left out
    u8 info[0x3C] = {};
    std::memcpy(info, a.mac().data(), 6);
    info[6] = 11;
    CHECK_EQ(b.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
    NodeInfo got;
    CHECK_EQ(b.e().GetNodeInformation(1, &got), 0u);
    CHECK_EQ(got.friend_code_seed, 0xA1A1u);  // not the lobby's identity (Alice, 0xF001)
    CHECK(Str(got.username) == "GameHost");
    CHECK_EQ(b.e().GetNodeInformation(2, &got), 0u);
    CHECK_EQ(got.friend_code_seed, 0xB2B2u);
    CHECK(Str(got.username) == "GameGuest");
    // and data goes both ways on the game's own channels, while no lobby message ever shows in a bind
    const u32 bind_a = 1, bind_b = 1;
    u32 event = 0;
    CHECK_EQ(a.e().Bind(bind_a, 0x2000, 3, 0xFFFF, &event), 0u);
    CHECK_EQ(b.e().Bind(bind_b, 0x2000, 3, 0xFFFF, &event), 0u);
    const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(b.e().SendTo(1, 3, 5, 0, hello, 5), 0u);
    room.WaitIdle();
    std::vector<u8> pulled;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(a.e(), bind_a, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 5u);
    CHECK_EQ(src, 2u);
    CHECK(std::memcmp(pulled.data(), hello, 5) == 0);
    CHECK_EQ(PullInto(b.e(), bind_b, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    CHECK_RULES();
}

static void TestAdmissionClosedAndFull() {
    std::printf("admission closed, creator full\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave"), e(room, 5, "Erin");
    const Players ps = {&a, &b, &c, &d, &e};
    Ticker ticker(Engines(ps));

    // (1) a creator with room for ONE client, which has it: the searcher that still has the group in a frozen list joins to no avail
    OpenLobby(a, 1, 0);
    CHECK(FindGroup(ps, c, a));
    c.lobby.ClientStopScan();  // the table is frozen, the group stays in it
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    CHECK(Until(ps, 2000, [&] {
        u16 ids[4];
        u32 total = 0;
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 1;
    }));
    g_rules.Reset();  // (C sends an EAPoL-Start to a host that is full - what a stale list does - and the checker, written for Azahar hosts, counts it)
    const u64 t0 = platform::NowMs();
    CHECK(Join(ps, c, a, &res, 6000));
    const u64 took = platform::NowMs() - t0;
    CHECK_EQ(res, R_TIMEOUT);  // the engine stays silent: the join gives up at 3000 ms and the engine goes back to the game
    CHECK(took >= 2900 && took < 4500);
    CHECK_EQ(CliState(c), 1u);
    CHECK(EngineClean(c));
    GroupView v;
    CHECK_EQ(c.lobby.ClientTitleInfo(a.mac(), &v), 0u);  // the table is as it was: the same MAC can be tried again
    CHECK_EQ(b.lobby.ClientStopSession(), 0u);           // the seat is free again: the same stale entry now gets in
    CHECK(Until(ps, 3000, [&] {
        u16 ids[4];
        u32 total = 1;
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 0;
    }));
    CHECK(Join(ps, c, a, &res));
    CHECK_EQ(res, 0u);
    CHECK_EQ(CliNode(c), 2u);

    // (2) a creator that stopped taking players (EndAccepting): its group is no longer offered, and a stale entry gets no answer
    OpenLobby(d, 3, 0);
    CHECK(FindGroup(ps, e, d));
    e.lobby.ClientStopScan();
    CHECK_EQ(d.lobby.ServerEndAccepting(), 0u);
    Settle(ps, 300);
    CHECK(Join(ps, e, d, &res, 6000));
    CHECK_EQ(res, R_TIMEOUT);
    CHECK(EngineClean(e));
    CHECK_EQ(e.lobby.ClientStartScan(0), 0u);  // a new search does not list it
    CHECK(!Until(ps, 1500, [&] {
        GroupView g;
        return e.lobby.ClientTitleInfo(d.mac(), &g) == 0;
    }));
}

static void TestFourPlayers() {
    std::printf("four players\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave");
    const Players ps = {&a, &b, &c, &d};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    Player* guests[3] = {&b, &c, &d};
    for (Player* g : guests) {
        CHECK(FindGroup(ps, *g, a));
    }
    u32 res = 0;
    for (Player* g : guests) {  // (one after the other: the order the nodes are numbered in)
        CHECK(Join(ps, *g, a, &res));
        CHECK_EQ(res, 0u);
    }
    CHECK_EQ(CliNode(b), 2u);
    CHECK_EQ(CliNode(c), 3u);
    CHECK_EQ(CliNode(d), 4u);
    u16 ids[4] = {};
    u32 total = 0;
    CHECK(Until(ps, 3000, [&] {
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 3;
    }));
    CHECK_EQ(ids[0], 2u);
    CHECK_EQ(ids[1], 3u);
    CHECK_EQ(ids[2], 4u);
    // a guest's roster has all four, and the creator's identity is the first
    for (Player* g : guests) {
        CHECK(Until(ps, 3000, [&] {
            g->lobby.ClientConnectingNodes(ids, 4, &total);
            return total == 4;
        }));
        NodeEntry ne;
        CHECK_EQ(g->lobby.ClientNodeInfo(4, &ne), 0u);
        CHECK(Str(ne.name) == "Dave");
    }
    CHECK_EQ(a.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(ps, 3000, [&] { return a.lobby.ServerState() == 8; }));
    CHECK_EQ(a.lobby.ServerSendPassphrase(kNine), 0u);
    CHECK(Until(ps, 5000, [&] {
        return a.lobby.ServerState() == 9 && CliState(b) == 9 && CliState(c) == 9 && CliState(d) == 9;
    }));
    for (Player* g : guests) {
        u8 pass[9] = {};
        g->lobby.ClientPassphrase(pass);
        CHECK(std::memcmp(pass, kNine, 9) == 0);
    }
    CHECK(EngineClean(a));
    for (Player* g : guests) {  // (the guests' games finalise the searcher: the engine is theirs again)
        CHECK(g->net.engine_held());
        CHECK_EQ(g->lobby.ClientFinalize(), 0u);
        CHECK(EngineClean(*g));
    }
    CHECK_RULES();
}

static void TestLeaveAndGone() {
    std::printf("a client leaves, the creator goes\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    CHECK(FindGroup(ps, c, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK(Join(ps, c, a, &res));
    u16 ids[4];
    u32 total = 0;
    CHECK(Until(ps, 3000, [&] {
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 2;
    }));

    // B leaves (the game's StopSession): its goodbye reaches the creator and the engine is B's own again
    CHECK_EQ(b.lobby.ClientStopSession(), 0u);
    CHECK(EngineClean(b));
    CHECK_EQ(CliState(b), 1u);
    CHECK(Until(ps, 3000, [&] {
        a.lobby.ServerConnectingClients(ids, 4, &total);
        return total == 1;
    }));
    CHECK_EQ(ids[0], 3u);

    // the creator closes: C finds the host gone (0x80, the games' own error), the engine is C's again
    CHECK_EQ(a.lobby.ServerFinalize(), 0u);
    CHECK(EngineClean(a));
    CHECK(Until(ps, 4000, [&] { return CliState(c) == 0x80; }));
    CHECK(EngineClean(c));
    CHECK_EQ(c.lobby.ClientStopSession(), 0u);
    CHECK_EQ(CliState(c), 1u);
    CHECK_RULES();
}

static void TestGameStartsUds() {
    std::printf("the game starts nwm::UDS while the lobby is up\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    // the creator's game goes on to its UDS: the plugin runs this before the game's Initialize reaches the engine
    CHECK(a.lobby.OnGameUdsStarting());
    CHECK(EngineClean(a));
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(a.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    NodeInfo got;
    CHECK_EQ(a.e().GetNodeInformation(1, &got), 0u);
    CHECK(Str(got.username) == "GameHost");  // the lobby's name is gone
    u8 none_from = 0, none_data[16];
    u32 none_size = 0;
    CHECK(!a.e().PopLobbyMessage(&none_from, none_data, 16, &none_size));  // nothing is kept for a game
    // B, still in the lobby, finds its host gone
    CHECK(Until(ps, 4000, [&] { return CliState(b) == 0x80; }));
    CHECK(EngineClean(b));
    // B's game goes on to UDS too: the lobby has nothing to give back
    CHECK(!b.lobby.OnGameUdsStarting());
    CHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK(!b.net.engine_held());
    g_rules.Reset();
}

static void TestTwoLobbies() {
    std::printf("two lobbies in one room\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), b1(room, 2, "Bob"), a2(room, 3, "Carol"), b2(room, 4, "Dan");
    const Players ps = {&a1, &b1, &a2, &b2};
    Ticker ticker(Engines(ps));

    OpenLobby(a1);
    OpenLobby(a2);
    CHECK(FindGroup(ps, b1, a1));
    CHECK(FindGroup(ps, b2, a2));
    // both searchers hear both lobbies
    GroupView v;
    CHECK_EQ(b1.lobby.ClientTitleInfo(a2.mac(), &v), 0u);
    CHECK_EQ(b2.lobby.ClientTitleInfo(a1.mac(), &v), 0u);
    u32 r1 = 0, r2 = 0;
    CHECK(Join(ps, b1, a1, &r1));
    CHECK(Join(ps, b2, a2, &r2));
    CHECK_EQ(r1, 0u);
    CHECK_EQ(r2, 0u);
    CHECK_EQ(CliNode(b1), 2u);  // (the same node ids in both networks)
    CHECK_EQ(CliNode(b2), 2u);
    CHECK_EQ(a1.lobby.ServerStartDistribution(), 0u);
    CHECK_EQ(a2.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(ps, 3000, [&] { return a1.lobby.ServerState() == 8 && a2.lobby.ServerState() == 8; }));
    static const u8 kOther[9] = {'f', 'f', 'e', 'e', 'd', 'd', 'c', 'c', 0};
    CHECK_EQ(a1.lobby.ServerSendPassphrase(kNine), 0u);
    CHECK_EQ(a2.lobby.ServerSendPassphrase(kOther), 0u);
    CHECK(Until(ps, 5000, [&] { return CliState(b1) == 9 && CliState(b2) == 9; }));
    u8 p1[9], p2[9];
    b1.lobby.ClientPassphrase(p1);
    b2.lobby.ClientPassphrase(p2);
    CHECK(std::memcmp(p1, kNine, 9) == 0);
    CHECK(std::memcmp(p2, kOther, 9) == 0);
    CHECK_RULES();
}

static void TestInboxAndStrangers() {
    std::printf("the lobby's messages: strangers, copies, the ring\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), s(room, 9, "Stranger");
    const Players ps = {&a, &b, &s};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    Settle(ps, 300);
    auto receive = [&](Player& p, u8* from, u8* data, u32* size) { return p.net.Receive(from, data, 16, size); };
    u8 from = 0, data[16];
    u32 size = 0;
    while (receive(b, &from, data, &size)) {  // (what the lobby itself sent)
    }
    const u8 body[2] = {1, 0};  // START, no body

    // From the host's MAC, node 1 -> node 2: delivered, once (the same sequence number twice is the copy of a radio_like=2 sender)
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 500, body, 2));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 500, body, 2));
    CHECK(receive(b, &from, data, &size));
    CHECK_EQ(from, 1u);
    CHECK_EQ(size, 2u);
    CHECK_EQ(data[0], 1u);
    CHECK(!receive(b, &from, data, &size));
    // Refused: another transmitter claiming node 1, a node that is not the host, a broadcast, another node's frame, 0 and 17 bytes
    u8 big[17] = {};
    b.e().OnWifiPacket(LobbyFrame(s.mac(), b.mac(), 2, 1, 501, body, 2));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 3, 502, body, 2));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), kBroadcastMac, 0xFFFF, 1, 503, body, 2));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 3, 1, 504, body, 2));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 505, body, 0));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 506, big, 17));
    CHECK(!receive(b, &from, data, &size));
    // The same at the creator: a message from the client's MAC and node is taken, from the stranger's MAC claiming node 2 it is not
    a.e().OnWifiPacket(LobbyFrame(b.mac(), a.mac(), 1, 2, 600, body, 2));
    a.e().OnWifiPacket(LobbyFrame(s.mac(), a.mac(), 1, 2, 601, body, 2));
    a.e().OnWifiPacket(LobbyFrame(b.mac(), a.mac(), 1, 3, 602, body, 2));
    CHECK(receive(a, &from, data, &size));
    CHECK_EQ(from, 2u);
    CHECK(!receive(a, &from, data, &size));
    // The ring keeps eight: the oldest goes
    for (u16 i = 0; i < 12; ++i) {
        const u8 numbered[3] = {3, 1, static_cast<u8>(i)};
        b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, static_cast<u16>(700 + i), numbered, 3));
    }
    u32 n = 0;
    u8 first = 0xFF, last = 0;
    while (receive(b, &from, data, &size)) {
        CHECK_EQ(size, 3u);
        first = n == 0 ? data[2] : first;
        last = data[2];
        ++n;
    }
    CHECK_EQ(n, 8u);
    CHECK_EQ(first, 4u);
    CHECK_EQ(last, 11u);
    // A frame is cut to the caller's room
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 800, big, 12));
    CHECK(b.e().PopLobbyMessage(&from, data, 5, &size));
    CHECK_EQ(size, 5u);
    // With the inbox off nothing is kept (the lobby's own release)
    b.e().SetLobbyInbox(false);
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 801, body, 2));
    CHECK(!receive(b, &from, data, &size));
}

static void TestNoAllocation() {
    std::printf("the reading calls allocate nothing\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));

    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    Settle(ps, 400);
    HeardGroup hg;
    u8 nodes[kMaxPlayers];
    NodeEntry ne;
    u8 from, data[16];
    u32 size;
    bool any = false;
    {
        CountNews counted;
        for (int i = 0; i < 20; ++i) {
            any |= b.net.ScanRead(0, &hg);  // (a client hears the host's beacons; a host does not hear its own)
            a.net.MyNode();
            a.net.Members(nodes);
            a.net.ReadNode(1, &ne);
            a.net.ReadNode(2, &ne);
            b.net.ConnectPoll();
            b.net.MyNode();
            b.net.Members(nodes);
            b.net.Receive(&from, data, 16, &size);
            b.net.NowMs();
            b.net.NowTicks();
        }
        CHECK_EQ(counted.count(), 0u);
    }
    CHECK(any);
    // The searcher's own scan of a lobby, in a console with no engine of its own yet, allocates nothing either
    Player c(room, 3, "Cleo");
    Ticker ticker2({&c.e()});
    c.net.ScanOpen();
    bool found = false;
    {
        CountNews counted;
        for (int i = 0; i < 20; ++i) {
            found |= c.net.ScanRead(0, &hg);
        }
        CHECK_EQ(counted.count(), 0u);
    }
    for (int i = 0; i < 100 && !found; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        found = c.net.ScanRead(0, &hg);
    }
    CHECK(found);
    CHECK(hg.host == a.mac());
    CHECK_EQ(hg.total_nodes, 2u);
    CHECK_EQ(hg.announce.max_nodes, 4u);
    CHECK_EQ(hg.announce.accepting, 1u);
    CHECK(Str(hg.announce.name) == "Alice");
}

static void TestEngineGuards() {
    std::printf("the engine's own guards\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice");
    UdsEngine& e = a.e();
    const u8 data[4] = {1, 2, 3, 4};
    // not initialised: nothing starts, the answers are the firmware's
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 6, 4, data, 4), res::kWrongState);
    CHECK_EQ(e.ConnectBeginLobby(a.mac()), res::kWrongState);
    CHECK_EQ(e.ConnectPoll(), -1);
    u16 node = 77, mask = 77;
    CHECK(!e.NodeSnapshot(&node, &mask));
    CHECK_EQ(node, 0u);
    CHECK_EQ(mask, 0u);
    const u16 who[10] = {u16(65)};
    CHECK_EQ(e.InitializeForLobby(1, who), 0u);
    CHECK_EQ(e.InitializeForLobby(1, who), res::kWrongState);  // taken: the second asks nothing of it
    CHECK(e.initialized());
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 6, 1, data, 4), res::kBadId);
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 6, 17, data, 4), res::kBadId);
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 6, 4, data, 201), res::kTooLarge);
    CHECK_EQ(e.ConnectBeginLobby(a.mac()), res::kNoHostFound);  // no beacon of that host was ever heard
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 0, 4, data, 4), 0u);
    CHECK(e.NodeSnapshot(&node, &mask));
    CHECK_EQ(node, 1u);
    CHECK_EQ(mask, 1u);
    CHECK_EQ(e.BeginHostingLobby(0x2810, 0x55, 6, 4, data, 4), res::kWrongState);  // already hosting
    UdsEngine::HeardLobby heard;
    CHECK(!e.HeardLobbyAt(0, 1000, &heard));  // (no beacon has come in: the room never echoes a host's own, so this only pins "nothing is listed")
    CHECK_EQ(e.Shutdown(), 0u);
    CHECK(!e.initialized());
    CHECK(!e.NodeSnapshot(&node, &mask));
    // the engine used as a game's after all that: Initialize finds nothing of the lobby
    u32 ev = 0;
    CHECK_EQ(e.Initialize(MakeNode(2, "G", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(e.ConnectPoll(), -1);
}

static void TestHeardLobbies() {
    std::printf("the engine remembers the lobbies it hears\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice");
    UdsEngine& e = a.e();
    const u32 L = UdsEngine::kLobbyWlanCommId;
    const u8 I = UdsEngine::kLobbyId8;
    u8 app[kAnnounceSize] = {};
    HostAnnounce ann;
    std::memset(&ann, 0, sizeof(ann));
    ann.child_tid_low = kTidLow;
    ann.max_nodes = 4;
    ann.accepting = 1;
    Put(ann.name, "Host");
    EncodeAnnounce(ann, app);
    auto mac = [](u8 last) { return Mac{0x00, 0x1F, 0x32, 0x00, 0x77, last}; };
    UdsEngine::HeardLobby h;

    CHECK(!e.HeardLobbyAt(0, 1000, &h));  // nothing yet (and no Initialize needed to ask)
    CHECK(e.PrepareLobby());  // (before it a game's engine files nothing: it does not even look at a beacon)
    e.OnWifiPacket(MakeBeacon(mac(7), 0x0010C000, 1, 1, 4, app, kAnnounceSize));
    CHECK(!e.HeardLobbyAt(0, 1000, &h));
    for (u8 i = 1; i <= 4; ++i) {
        e.OnWifiPacket(MakeBeacon(mac(i), L, I, i, 4, app, kAnnounceSize, static_cast<u8>(i)));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    for (u32 i = 0; i < 4; ++i) {  // in the order they came, with what their beacons say
        CHECK(e.HeardLobbyAt(i, 1000, &h));
        CHECK(h.host == mac(static_cast<u8>(i + 1)));
        CHECK_EQ(h.channel, i + 1);
        CHECK_EQ(h.total_nodes, i + 1);
        CHECK_EQ(h.max_nodes, 4u);
        CHECK_EQ(h.size, kAnnounceSize);
        CHECK(std::memcmp(h.data, app, kAnnounceSize) == 0);
    }
    CHECK(!e.HeardLobbyAt(4, 1000, &h));
    // a beacon of a lobby we know changes its entry where it is
    e.OnWifiPacket(MakeBeacon(mac(3), L, I, 2, 4, app, kAnnounceSize, 6));
    CHECK(e.HeardLobbyAt(2, 1000, &h));
    CHECK(h.host == mac(3));
    CHECK_EQ(h.total_nodes, 2u);
    CHECK_EQ(h.channel, 6u);
    // a fifth lobby takes the place of the one heard longest ago (mac 1: mac 3 was heard again just now)
    e.OnWifiPacket(MakeBeacon(mac(5), L, I, 1, 4, app, kAnnounceSize));
    CHECK(e.HeardLobbyAt(0, 1000, &h));
    CHECK(h.host == mac(5));
    for (u32 i = 0; i < 4; ++i) {
        CHECK(e.HeardLobbyAt(i, 1000, &h));
        CHECK(!(h.host == mac(1)));
    }
    CHECK(!e.HeardLobbyAt(4, 1000, &h));
    // the age: a lobby not heard for longer than the caller asks is not there
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(!e.HeardLobbyAt(0, 30, &h));
    e.OnWifiPacket(MakeBeacon(mac(5), L, I, 1, 4, app, kAnnounceSize));
    CHECK(e.HeardLobbyAt(0, 30, &h));
    CHECK(h.host == mac(5));
    CHECK(!e.HeardLobbyAt(1, 30, &h));  // (the others are 60 ms old)
    CHECK(e.HeardLobbyAt(1, 1000, &h));
    // what is not a lobby is not filed: a game's network, a beacon with no application data, another id8
    e.OnWifiPacket(MakeBeacon(mac(8), 0x0010C000, 1, 1, 4, app, kAnnounceSize));
    e.OnWifiPacket(MakeBeacon(mac(9), L, I, 1, 4, app, 0));
    e.OnWifiPacket(MakeBeacon(mac(10), L, 0x54, 1, 4, app, kAnnounceSize));
    for (u32 i = 0; e.HeardLobbyAt(i, 1000, &h); ++i) {
        CHECK(!(h.host == mac(8)) && !(h.host == mac(9)) && !(h.host == mac(10)));
    }
    // a long application data is cut to what the engine hands out
    u8 big[100];
    for (u32 i = 0; i < 100; ++i) {
        big[i] = static_cast<u8>(i);
    }
    e.OnWifiPacket(MakeBeacon(mac(5), L, I, 1, 4, big, 100));
    CHECK(e.HeardLobbyAt(0, 1000, &h));
    CHECK(h.host == mac(5));
    CHECK_EQ(h.size, UdsEngine::kHeardDataMax);
    CHECK(std::memcmp(h.data, big, UdsEngine::kHeardDataMax) == 0);
    // ... and the adapter turns only OUR announcements into lobbies: this one is none (no magic)
    HeardGroup hg;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!a.net.ScanRead(0, &hg) || !(hg.host == mac(5)));
}

static void TestClock() {
    std::printf("the clock\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice");
    // NowMs and NowTicks are one source (the console's: svcGetSystemTick): over 30 ms they agree to a millisecond or two. (An absolute
    // reading divided by AZL_TICKS_PER_MS drifts: the constant is rounded, for thresholds and not for the clock.)
    for (int i = 0; i < 5; ++i) {
        const u64 ms0 = a.net.NowMs();
        const u64 ticks0 = a.net.NowTicks();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const u64 ms1 = a.net.NowMs();
        const u64 ticks1 = a.net.NowTicks();
        CHECK(ms1 >= ms0 + 28 && ms1 <= ms0 + 200);
        const u64 by_ticks = (ticks1 - ticks0) / AZL_TICKS_PER_MS;
        CHECK(by_ticks + 2 >= ms1 - ms0 && by_ticks <= ms1 - ms0 + 2);
    }
}

int main() {
    std::printf("dlp_net_test: the fake Download Play lobby over the UDS engine\n");
    TestClock();
    TestEngineGuards();
    TestHeardLobbies();
    TestAnnounceAndScan();
    TestJoin();
    TestFullSequenceThenGameUds();
    TestAdmissionClosedAndFull();
    TestFourPlayers();
    TestLeaveAndGone();
    TestGameStartsUds();
    TestTwoLobbies();
    TestInboxAndStrangers();
    TestNoAllocation();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
