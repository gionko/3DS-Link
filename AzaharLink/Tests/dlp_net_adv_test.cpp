// The fake Download Play lobby over the REAL UDS engine, the adversarial half (phase 2 of analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE2.md): what the
// review and the mutation testing of 2026-10-29 asked of Tests/dlp_net_test.cpp. Same rig (Tests/dlp_net_rig.hpp), real time.
//
// Scenarios: the lobby's calls on an engine a GAME has (nothing of the game is touched); failures that give the engine back, a lobby that was never
// heard or heard too long ago, the attempt that ends without a connection; the ring around the lobby (an unread message does not outlive it, a message
// never shows in a bind, the channel, a Kick that reaches the client); a searcher that holds the engine joins another lobby; a creator with six nodes
// (the adapter keeps to four); names of ten units; a spectator in the lobby's network; what the engine files from a beacon and for how long (ids, the
// bytes behind a short announcement, the age, the network id, the attribute that closes the admission); the limits at their edges and a short frame; a
// half-open stranger and the order of the checks; the roster of a client when ANOTHER lobby of the same game admits somebody (it must not change) and
// when a peer of its own lobby leaves or is kicked (it must change); a lobby heard no more once its host beacons its game's network; the room's member
// lists (a host or a client that leaves the room) and a service that stops (sleep) under a lobby; a creator that closes its admission and opens it
// again; clients that are never accepted; two clients that join in one round trip.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_net_adv_test.cpp Sources/azl/dlp_lobby_net.cpp <the core sources of Tests/run.sh> -o /tmp/dlp_net_adv_test
#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

/// The roster a client's lobby reads, as "Name1,Name2,..." in node order.
static std::string RosterOf(Player& p) {
    u16 ids[4] = {};
    u32 total = 0;
    p.lobby.ClientConnectingNodes(ids, 4, &total);
    std::string s;
    for (u32 i = 0; i < total && i < 4; ++i) {
        NodeEntry ne;
        if (p.lobby.ClientNodeInfo(ids[i], &ne) != 0) {
            s += "?";
        } else {
            s += Str(ne.name);
        }
        if (i + 1 < total) {
            s += ",";
        }
    }
    return s;
}
static u32 ClientsOf(Player& host) {
    u16 ids[4] = {};
    u32 total = 0;
    host.lobby.ServerConnectingClients(ids, 4, &total);
    return total;
}

/// X1: the lobby's calls reach an engine that a GAME has (its host, with a client): nothing of the game is touched.
static void XGameOwnsEngine() {
    std::printf("x1: the lobby's calls on an engine a game has\n");
    g_rules.Reset();
    MockRoom room;
    Player g(room, 1, "G"), h(room, 2, "H"), k(room, 3, "K");
    const Players ps = {&g, &h, &k};
    Ticker ticker(Engines(ps));
    u32 ev = 0;
    CHECK_EQ(g.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(g.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    CHECK_EQ(h.e().Initialize(MakeNode(0xB1, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, g.mac().data(), 6);
    info[6] = 7;
    SentFrames sent(h.con);
    CHECK_EQ(h.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
    bool auth = false;
    for (const WifiPacket& p : sent.All()) {
        if (p.type == WifiPacket::Type::Authentication) {
            auth = true;
            CHECK_EQ(p.channel, 7u);  // the connect takes the channel the game's info names
        }
    }
    CHECK(auth);
    u16 node = 0, mask = 0;
    CHECK(g.e().NodeSnapshot(&node, &mask));
    CHECK_EQ(mask, 3u);
    // the lobby's creator calls, with the game's session up: a Kick, an update of the announcement, a second HostOpen
    g.net.Kick(2);
    g.net.HostUpdate(Ann(4, 0));
    Settle(ps, 300);
    CHECK(g.e().NodeSnapshot(&node, &mask));
    CHECK_EQ(mask, 3u);  // the game's client is still there
    CHECK(h.e().status() == NetworkStatus::ConnectedAsClient);
    CHECK_EQ(k.e().Initialize(MakeNode(0xC1, "GameThird", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(k.e().ConnectToNetwork(info, sizeof(info), 1), 0u);  // the game's admission is still open
    CHECK(!g.net.HostOpen(Ann(), 6, Me(1, "G")));                 // the game has the service
    CHECK(!g.net.ConnectBegin(h.mac(), Me(1, "G")));
    CHECK(!g.net.ScanOpen());
    g.net.HostClose();
    g.net.Disconnect();
    CHECK(g.e().initialized());  // not shut down
    CHECK(g.e().status() == NetworkStatus::ConnectedAsHost);
    CHECK(!g.net.engine_held());
    CHECK_EQ(g.net.MyNode(), 1u);  // (the snapshot is the engine's, and the game's host is node 1)
    CHECK(h.e().status() == NetworkStatus::ConnectedAsClient);
}

/// X2: failures give the engine back; a lobby that was never heard, heard too long ago, or heard no more; an attempt that ends.
static void XFailures() {
    std::printf("x2: failures, stale and unknown lobbies\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), c(room, 3, "Cleo");
    const Players ps = {&a, &c};
    Ticker ticker(Engines(ps));
    const Identity me = Me(0xF001, "Alice");
    CHECK(!a.net.HostOpen(Ann(1), 6, me));  // BeginHostingLobby refuses one node: the engine goes back
    CHECK(EngineClean(a));
    CHECK(!a.net.HostOpen(Ann(17), 6, me));  // ... and no lobby is ever announced for more than four players
    CHECK(EngineClean(a));
    CHECK(!a.net.HostOpen(Ann(5), 6, me));
    CHECK(EngineClean(a));
    CHECK_EQ(c.net.MyNode(), 0u);
    CHECK(c.net.ScanOpen());
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(), app);
    const u32 L = UdsEngine::kLobbyWlanCommId;
    const u8 I = UdsEngine::kLobbyId8;
    SentFrames sent(c.con);
    c.e().OnWifiPacket(MakeBeacon(kFake1, L, I, 1, 4, app, kAnnounceSize, 9));
    c.e().OnWifiPacket(MakeBeacon(kFake2, L, I, 1, 4, app, kAnnounceSize, 10));
    Mac stranger = kFake1;
    stranger[5] = 0x40;
    CHECK(!c.net.ConnectBegin(stranger, Me(0xF003, "Cleo")));  // a lobby that was never heard
    CHECK(EngineClean(c));
    CHECK(c.net.ConnectBegin(kFake1, Me(0xF003, "Cleo")));
    bool auth = false;
    for (const WifiPacket& p : sent.All()) {
        if (p.type == WifiPacket::Type::Authentication && p.destination_address == kFake1) {
            auth = true;
            CHECK_EQ(p.channel, 9u);  // the beacon's channel
        }
    }
    CHECK(auth);
    CHECK_EQ(c.net.ConnectPoll(), 0);  // nobody answers
    c.e().DisconnectNetwork();         // the attempt ends without a connection
    CHECK_EQ(c.net.ConnectPoll(), -1);
    c.net.Disconnect();
    CHECK(EngineClean(c));
    // a lobby not heard for longer than the engine keeps it (3 s) cannot be joined, and the first beacon heard after that sweeps it away
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    CHECK(!c.net.ConnectBegin(kFake2, Me(0xF003, "Cleo")));
    CHECK(EngineClean(c));
    c.e().OnWifiPacket(MakeBeacon(kFake1, L, I, 1, 4, app, kAnnounceSize, 9));
    UdsEngine::HeardLobby heard;
    u32 listed = 0;
    for (u32 i = 0; c.e().HeardLobbyAt(i, 0xFFFFFFFFu, &heard); ++i) {
        ++listed;
        CHECK(heard.host == kFake1);  // kFake2 is gone, however old an age the caller accepts
    }
    CHECK_EQ(listed, 1u);
}

/// X3: the ring around the lobby, the channel, the Kick.
static void XInbox() {
    std::printf("x3: inbox, binds, channel\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK_EQ(heard.channel, 9u);  // the channel the creator asked for
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    CHECK(b.net.ScanOpen());  // held by the lobby: still allowed
    u32 event = 0;
    CHECK_EQ(b.e().Bind(1, 0x2000, UdsEngine::kLobbyChannel, 0xFFFF, &event), 0u);
    CHECK(WaitFor(2000, [&] {
        u16 n = 0, m = 0;
        return a.e().NodeSnapshot(&n, &m) && m == 3;
    }));
    CHECK(a.net.Send(2, kMsg, 2));
    u8 from = 0, data[16];
    u32 size = 0;
    CHECK(WaitFor(2000, [&] { return b.net.Receive(&from, data, 16, &size); }));
    CHECK_EQ(from, 1u);
    CHECK_EQ(size, 2u);
    std::vector<u8> pulled;
    u16 src = 0;
    CHECK_EQ(PullInto(b.e(), 1, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 0u);  // a lobby message never shows in a bind
    // a message nobody read when the lobby ends does not outlive it
    CHECK(a.net.Send(2, kMsg, 2));
    room.WaitIdle();
    b.net.Disconnect();
    CHECK(!b.e().PopLobbyMessage(&from, data, 16, &size));
    // Kick: a creator's ejection reaches the client
    Player c(room, 3, "Cleo");
    Ticker ticker2({&c.e()});
    CHECK(c.net.ScanOpen());
    CHECK(WaitFor(3000, [&] { return c.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(c.net.ConnectBegin(a.mac(), Me(0xF003, "Cleo")));
    CHECK(WaitFor(4000, [&] { return c.net.ConnectPoll() == 1; }));
    a.net.Kick(c.net.MyNode());
    CHECK(WaitFor(3000, [&] { return c.e().status() == NetworkStatus::NotConnected; }));
}

/// X4: a searcher whose network is gone but still holds the engine joins another lobby.
static void XRejoin() {
    std::printf("x4: rejoin while the engine is held\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), a2(room, 2, "Carol"), b(room, 3, "Bob");
    const Players ps = {&a1, &a2, &b};
    Ticker ticker(Engines(ps));
    CHECK(a1.net.HostOpen(Ann(4, 1, "One"), 6, Me(0xF001, "Alice")));
    CHECK(a2.net.HostOpen(Ann(4, 1, "Two"), 7, Me(0xF002, "Carol")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(1, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a1.mac(), Me(0xF003, "Bob")));
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    // a second HostOpen / ConnectBegin while the lobby is up fails and ends nothing
    CHECK(!b.net.HostOpen(Ann(), 6, Me(0xF003, "Bob")));
    CHECK(b.e().status() == NetworkStatus::ConnectedAsClient);
    CHECK(b.net.engine_held());
    CHECK(!a1.net.ConnectBegin(a2.mac(), Me(0xF001, "Alice")));
    CHECK(a1.e().status() == NetworkStatus::ConnectedAsHost);
    CHECK(a1.net.engine_held());
    a1.net.HostClose();
    CHECK(WaitFor(4000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    CHECK(b.net.engine_held());
    CHECK(b.net.ConnectBegin(a2.mac(), Me(0xF003, "Bob")));  // the engine is held already: true
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    CHECK_EQ(b.net.MyNode(), 2u);
}

/// X5: a creator that admits more than four (a hostile or newer one): the adapter keeps to four everywhere.
static void XHostile() {
    std::printf("x5: a creator with six nodes\n");
    g_rules.Reset();
    MockRoom room;
    Player h(room, 1, "Host"), c1(room, 2, "C1"), c2(room, 3, "C2"), c3(room, 4, "C3"), c4(room, 5, "C4"), c5(room, 6, "C5");
    Player* cs[5] = {&c1, &c2, &c3, &c4, &c5};
    const Players ps = {&h, &c1, &c2, &c3, &c4, &c5};
    Ticker ticker(Engines(ps));
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(4), app);
    const u16 who[10] = {u16('H')};
    CHECK_EQ(h.e().InitializeForLobby(0xF001, who), 0u);
    CHECK_EQ(h.e().BeginHostingLobby(UdsEngine::kLobbyWlanCommId, UdsEngine::kLobbyId8, 6, 8, app, kAnnounceSize), 0u);
    for (Player* c : cs) {
        CHECK(c->net.ScanOpen());
        UdsEngine::HeardLobby heard;
        CHECK(WaitFor(3000, [&] { return c->e().HeardLobbyAt(0, 1000, &heard); }));
        CHECK(c->net.ConnectBegin(h.mac(), Me(0xF010 + c->mac()[5], "C")));
        CHECK(WaitFor(4000, [&] { return c->net.ConnectPoll() == 1; }));
    }
    CHECK(WaitFor(4000, [&] {
        u16 n = 0, m = 0;
        return c1.e().NodeSnapshot(&n, &m) && m == 0x3F;
    }));
    u8 nodes[kMaxPlayers];
    CHECK_EQ(c1.net.Members(nodes), 4u);  // the bound
    CHECK_EQ(nodes[3], 4u);
    CHECK_EQ(h.net.Members(nodes), 4u);
    CHECK(!h.net.Send(5, kMsg, 2));  // node 5 is there, and no lobby message goes to it
    CHECK(!h.net.Send(2, kMsg, 0));
    u8 big[17] = {};
    CHECK(!h.net.Send(2, big, 17));
    CHECK(h.net.Send(2, kMsg, 2));
    CHECK(h.net.Send(4, kMsg, 2));
}

/// X6: names of ten units, ReadNode(0), the signal.
static void XNames() {
    std::printf("x6: ten-unit names, node 0, signal\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "ABCDEFGHIJ"), b(room, 2, "KLMNOPQRST"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));
    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    NodeEntry ne;
    CHECK(Until(ps, 3000, [&] { return ClientsOf(a) == 1; }));
    CHECK_EQ(a.lobby.ServerClientInfo(2, &ne), 0u);
    CHECK(Str(ne.name) == "KLMNOPQRST");
    CHECK(Until(ps, 3000, [&] { return RosterOf(b) == "ABCDEFGHIJ,KLMNOPQRST"; }));
    CHECK(!a.net.ReadNode(0, &ne));
    CHECK(a.net.ReadNode(1, &ne));
    // C hears it: three bars
    CHECK(c.net.ScanOpen());
    HeardGroup hg;
    CHECK(WaitFor(3000, [&] { return c.net.ScanRead(0, &hg); }));
    CHECK_EQ(hg.signal, 3u);
}

/// X7: a spectator in the lobby's network: the lobby's frames are not its.
static void XSpectator() {
    std::printf("x7: a spectator\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), s(room, 2, "Spec");
    const Players ps = {&a, &s};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    const u16 who[10] = {u16('S')};
    CHECK_EQ(s.e().InitializeForLobby(0xF009, who), 0u);
    s.e().SetLobbyInbox(true);  // (this also allocates what the engine keeps for a lobby)
    u32 event = 0;
    CHECK_EQ(s.e().Bind(1, 0x2000, UdsEngine::kLobbyChannel, 0xFFFF, &event), 0u);
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return s.e().HeardLobbyAt(0, 1000, &heard); }));
    u8 info[0x3C] = {};
    std::memcpy(info, a.mac().data(), 6);
    info[6] = 9;
    CHECK_EQ(s.e().ConnectBegin(info, sizeof(info), 2), 0u);
    CHECK(WaitFor(4000, [&] { return s.e().ConnectPoll() == 1; }));
    u16 node = 0xEE, mask = 0;
    CHECK(s.e().NodeSnapshot(&node, &mask));
    u8 from = 0, data[16];
    u32 size = 0;
    a.e().OnWifiPacket(LobbyFrame(s.mac(), a.mac(), 1, 0, 900, kMsg, 2));
    CHECK(!a.e().PopLobbyMessage(&from, data, 16, &size));  // a spectator speaks to no lobby
    s.e().OnWifiPacket(LobbyFrame(a.mac(), s.mac(), node, 1, 901, kMsg, 2));
    CHECK(!s.e().PopLobbyMessage(&from, data, 16, &size));  // ... and none speaks to it
    std::vector<u8> pulled;
    u16 src = 0;
    CHECK_EQ(PullInto(s.e(), 1, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 0u);  // nor does it reach its bind
}

/// X8: what the engine files from a beacon, and how long.
static void XBeacons() {
    std::printf("x8: beacons, attributes, ids\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), a2(room, 2, "Carol"), c(room, 3, "Cleo");
    const Players ps = {&a1, &a2, &c};
    Ticker ticker(Engines(ps));
    const u32 L = UdsEngine::kLobbyWlanCommId;
    const u8 I = UdsEngine::kLobbyId8;
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(), app);
    UdsEngine& e = c.e();
    CHECK(c.net.ScanOpen());
    UdsEngine::HeardLobby h;
    // the id8 alone does not make a lobby, nor does the wlancommid alone
    e.OnWifiPacket(MakeBeacon(kFake1, L + 1, I, 1, 4, app, kAnnounceSize));
    e.OnWifiPacket(MakeBeacon(kFake1, L, I + 1, 1, 4, app, kAnnounceSize));
    CHECK(!e.HeardLobbyAt(0, 1000, &h));
    // the bytes behind a shorter announcement are zero
    u8 fill[kAnnounceSize];
    std::memset(fill, 0xEE, sizeof(fill));
    e.OnWifiPacket(MakeBeacon(kFake2, L, I, 1, 4, fill, kAnnounceSize));
    e.OnWifiPacket(MakeBeacon(kFake2, L, I, 1, 4, fill, 5));
    CHECK(e.HeardLobbyAt(0, 1000, &h));
    CHECK_EQ(h.size, 5u);
    bool zero = true;
    for (u32 i = 5; i < UdsEngine::kHeardDataMax; ++i) {
        zero = zero && h.data[i] == 0;
    }
    CHECK(zero);
    // age 0 is fresh at age 0 (the same millisecond)
    int listed = 0;
    for (int i = 0; i < 50; ++i) {
        e.OnWifiPacket(MakeBeacon(kFake2, L, I, 1, 4, app, kAnnounceSize));
        listed += e.HeardLobbyAt(0, 0, &h) ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(listed > 0);
    // the adapter lists a lobby for 400 ms after its beacon (the host beacons every 102 ms)
    e.OnWifiPacket(MakeBeacon(kFake2, L, I, 1, 4, app, kAnnounceSize));
    HeardGroup hg;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(c.net.ScanRead(0, &hg));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(!c.net.ScanRead(0, &hg));
    // the lobby's beacons carry a network id of their own, and the attribute that closes the admission
    SentFrames s1(a1.con), s2(a2.con);
    CHECK(a1.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    CHECK(a2.net.HostOpen(Ann(), 6, Me(0xF002, "Carol")));
    Settle(ps, 300);
    constexpr std::size_t kIdAt = offsetof(NetworkInfo, network_id_be) - offsetof(NetworkInfo, oui_value);
    constexpr std::size_t kAttrAt = offsetof(NetworkInfo, attributes_be) - offsetof(NetworkInfo, oui_value);
    auto last_beacon = [](SentFrames& s, u32* id, u32* attr) {
        bool any = false;
        for (const WifiPacket& p : s.All()) {
            if (p.type != WifiPacket::Type::Beacon) {
                continue;
            }
            const BeaconTags tags = FindBeaconTags(p.data);
            if (!tags.network || tags.network_size <= kIdAt + 4) {
                continue;
            }
            *id = LoadBE32(tags.network + kIdAt);
            *attr = LoadBE16(tags.network + kAttrAt);
            any = true;
        }
        return any;
    };
    u32 id1 = 0, id2 = 0, at1 = 99, at2 = 99;
    CHECK(last_beacon(s1, &id1, &at1));
    CHECK(last_beacon(s2, &id2, &at2));
    CHECK(id1 != 0 && id2 != 0 && id1 != id2);
    CHECK_EQ(at1 & 0x7, 0u);
    a1.net.HostUpdate(Ann(4, 0));
    Settle(ps, 300);
    CHECK(last_beacon(s1, &id1, &at1));
    CHECK_EQ(at1 & 0x7, 0x2u);  // UDSNETATTR_DISABLE_CONNECT
    a1.net.HostUpdate(Ann(4, 1));
    Settle(ps, 300);
    CHECK(last_beacon(s1, &id1, &at1));
    CHECK_EQ(at1 & 0x7, 0u);
}

/// X9: the engine's own limits at their edges, and a short frame.
static void XLimits() {
    std::printf("x9: limits, short frame\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    u8 big[200];
    std::memset(big, 7, sizeof(big));
    const u16 who[10] = {u16('A')};
    CHECK_EQ(a.e().InitializeForLobby(1, who), 0u);
    CHECK_EQ(a.e().BeginHostingLobby(0x2810, 0x55, 6, 16, big, 200), 0u);  // 16 nodes, 200 bytes: the edges
    CHECK_EQ(a.e().Shutdown(), 0u);
    // a frame whose header says 16 bytes and that carries 6
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    Player c(room, 3, "Cleo");
    Ticker ticker2({&c.e()});
    CHECK(c.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return c.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(c.net.ConnectBegin(a.mac(), Me(0xF003, "Cleo")));
    CHECK(WaitFor(4000, [&] { return c.net.ConnectPoll() == 1; }));
    u8 body[16];
    std::memset(body, 3, sizeof(body));
    WifiPacket p = LobbyFrame(a.mac(), c.mac(), 2, 1, 950, body, 16);
    std::vector<u8> shorter(p.data.begin(), p.data.end() - 10);
    p.data = shorter;
    c.e().OnWifiPacket(p);
    u8 from = 0, data[16];
    u32 size = 0;
    if (c.e().PopLobbyMessage(&from, data, 16, &size)) {
        CHECK(size <= 6u);  // what is there, never what the header claims
    }
}

/// X10: a stranger that authenticated and never joined; a refused frame does not use up the dedupe key of a legitimate one; a lobby after a game's
/// session starts from a clean node.
static void XStrangers() {
    std::printf("x10: half-open stranger, dedupe order, clean node\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), s(room, 9, "Strg");
    const Players ps = {&a, &b, &s};
    Ticker ticker(Engines(ps));
    // the engine had a game's node (id 7, 9) before the lobby
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xAA, "Old", 7), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(b.e().Initialize(MakeNode(0xBB, "Old", 9), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(a.e().Shutdown(), 0u);
    CHECK_EQ(b.e().Shutdown(), 0u);
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    NodeInfo ni;
    CHECK_EQ(a.e().GetNodeInformation(1, &ni), 0u);
    CHECK_EQ(ni.network_node_id, 1u);
    CHECK_EQ(a.e().GetNodeInformation(2, &ni), 0u);
    CHECK_EQ(ni.network_node_id, 2u);
    CHECK(WaitFor(3000, [&] {
        u16 n = 0, m = 0;
        return b.e().NodeSnapshot(&n, &m) && m == 3;
    }));
    CHECK_EQ(b.e().GetNodeInformation(2, &ni), 0u);
    CHECK_EQ(ni.network_node_id, 2u);
    // S authenticates to A (half-open: no EAPoL-Start) and then speaks as "node 0"
    WifiPacket auth;
    auth.type = WifiPacket::Type::Authentication;
    auth.transmitter_address = s.mac();
    auth.destination_address = a.mac();
    auth.channel = 9;
    auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
    a.e().OnWifiPacket(auth);
    u8 from = 0, data[16];
    u32 size = 0;
    a.e().OnWifiPacket(LobbyFrame(s.mac(), a.mac(), 1, 0, 960, kMsg, 2));
    CHECK(!a.e().PopLobbyMessage(&from, data, 16, &size));
    // a stranger's frame with the (node, sequence) of a legitimate one that follows
    b.e().OnWifiPacket(LobbyFrame(s.mac(), b.mac(), 2, 1, 971, kMsg, 2));  // refused: another MAC claiming to be the host
    CHECK(!b.e().PopLobbyMessage(&from, data, 16, &size));
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 971, kMsg, 2));  // the real host, the same number
    CHECK(b.e().PopLobbyMessage(&from, data, 16, &size));
    CHECK_EQ(from, 1u);
}

/// X11: ANOTHER lobby of the game in the room admits somebody: a client's roster is its own lobby's, and the node ids are the same in both.
static void XRosters() {
    std::printf("x11: the roster of a client when another lobby admits somebody\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), b1(room, 2, "Bob"), a2(room, 3, "Carol"), b2(room, 4, "Dan"), b3(room, 5, "Eve");
    const Players ps = {&a1, &b1, &a2, &b2, &b3};
    Ticker ticker(Engines(ps));
    OpenLobby(a1);
    OpenLobby(a2);
    CHECK(FindGroup(ps, b1, a1));
    u32 res = 0;
    CHECK(Join(ps, b1, a1, &res));
    CHECK_EQ(res, 0u);
    CHECK(Until(ps, 3000, [&] { return RosterOf(b1) == "Alice,Bob"; }));
    // the other lobby fills up, with more players than ours has
    CHECK(FindGroup(ps, b2, a2));
    CHECK(FindGroup(ps, b3, a2));
    CHECK(Join(ps, b2, a2, &res));
    CHECK_EQ(res, 0u);
    CHECK(Join(ps, b3, a2, &res));
    CHECK_EQ(res, 0u);
    CHECK(Until(ps, 3000, [&] { return RosterOf(b2) == "Carol,Dan,Eve" && RosterOf(b3) == "Carol,Dan,Eve"; }));
    Settle(ps, 600);
    CHECK(RosterOf(b1) == "Alice,Bob");  // not "Carol,Dan,Eve"
    CHECK_EQ(ClientsOf(a1), 1u);
    u8 nodes[kMaxPlayers];
    CHECK_EQ(b1.net.Members(nodes), 2u);
    CHECK_EQ(b2.net.Members(nodes), 3u);
    CHECK_RULES();
}

/// X12: a peer of the client's own lobby leaves or is kicked: its roster follows, and the unread messages of the one that left go with it.
static void XDeparture() {
    std::printf("x12: a peer leaves or is kicked\n");
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
    for (Player* g : guests) {
        CHECK(Join(ps, *g, a, &res));
        CHECK_EQ(res, 0u);
    }
    CHECK(Until(ps, 4000, [&] { return RosterOf(b) == "Alice,Bob,Cleo,Dave" && RosterOf(c) == RosterOf(b) && RosterOf(d) == RosterOf(b); }));
    // C leaves: B and D see it go (the creator sends no other news of a departure than the Logoff the engine refreshes them with)
    CHECK_EQ(c.lobby.ClientStopSession(), 0u);
    CHECK(Until(ps, 4000, [&] { return RosterOf(b) == "Alice,Bob,Dave" && RosterOf(d) == "Alice,Bob,Dave"; }));
    u8 nodes[kMaxPlayers];
    CHECK_EQ(b.net.Members(nodes), 3u);
    CHECK_EQ(nodes[0], 1u);
    CHECK_EQ(nodes[1], 2u);
    CHECK_EQ(nodes[2], 4u);
    // D is kicked by the creator: B follows
    a.net.Kick(4);
    CHECK(Until(ps, 4000, [&] { return RosterOf(b) == "Alice,Bob"; }));
    CHECK(Until(ps, 4000, [&] { return CliState(d) == 0x80; }));
    CHECK_RULES();
}

/// X13: the unread messages of a node that leaves go with it (the next console with its id must not be credited with its answers).
static void XPurge() {
    std::printf("x13: the unread messages of a node that left\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), c(room, 3, "Cleo");
    const Players ps = {&a, &c};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    CHECK(c.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return c.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(c.net.ConnectBegin(a.mac(), Me(0xF003, "Cleo")));
    CHECK(WaitFor(4000, [&] { return c.net.ConnectPoll() == 1; }));
    CHECK(WaitFor(2000, [&] {
        u16 n = 0, m = 0;
        return a.e().NodeSnapshot(&n, &m) && m == 3;
    }));
    CHECK(c.net.Send(1, kMsg, 2));  // a message the creator does not read
    room.WaitIdle();
    c.net.Disconnect();
    CHECK(WaitFor(3000, [&] {
        u16 n = 0, m = 0;
        return a.e().NodeSnapshot(&n, &m) && m == 1;
    }));
    u8 from = 0, data[16];
    u32 size = 0;
    CHECK(!a.net.Receive(&from, data, 16, &size));
}

/// X14: a lobby heard no more once its host beacons its game's network (a stale list must not join a game).
static void XStaleSlot() {
    std::printf("x14: the lobby's host beacons its game's network\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    a.net.HostClose();
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(a.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    // the game's first beacon is heard: the lobby of that MAC is forgotten, and a join to it is refused
    CHECK(WaitFor(2000, [&] { return !b.e().HeardLobbyAt(0, 3000, &heard); }));
    CHECK(!b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(EngineClean(b));
    u16 node = 0, mask = 0;
    CHECK(a.e().NodeSnapshot(&node, &mask));
    CHECK_EQ(mask, 1u);  // nobody got into the game's network
}

/// X15: the room's member lists: a host or a client that leaves the room.
static void XPresence() {
    std::printf("x15: a member leaves the room\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), b1(room, 2, "Bob"), a2(room, 3, "Carol"), b2(room, 4, "Dan");
    const Players ps = {&a1, &b1, &a2, &b2};
    Ticker ticker(Engines(ps));
    OpenLobby(a1);
    OpenLobby(a2);
    CHECK(FindGroup(ps, b1, a1));
    CHECK(FindGroup(ps, b2, a2));
    u32 res = 0;
    CHECK(Join(ps, b1, a1, &res));
    CHECK(Join(ps, b2, a2, &res));
    CHECK(Until(ps, 3000, [&] { return ClientsOf(a1) == 1 && ClientsOf(a2) == 1; }));
    // the room lists everybody, then the first list without a console: after the debounce it has left
    const std::vector<Mac> all = {a1.mac(), b1.mac(), a2.mac(), b2.mac()};
    for (Player* p : ps) {
        p->e().OnRoomMembers(all);
    }
    const std::vector<Mac> no_host1 = {b1.mac(), a2.mac(), b2.mac()};    // A1 left the room
    const std::vector<Mac> no_client2 = {a1.mac(), b1.mac(), a2.mac()};  // B2 left the room
    b1.e().OnRoomMembers(no_host1);
    a2.e().OnRoomMembers(no_client2);
    CHECK(Until(ps, 7000, [&] { return CliState(b1) == 0x80; }));  // the client whose host left the room
    CHECK(Until(ps, 7000, [&] { return ClientsOf(a2) == 0; }));    // the creator whose client left the room
    CHECK(EngineClean(b1));
}

/// X15b: the service stops (sleep) under a client and under a creator: the lobby ends the way the games expect, and gives the engine back.
static void XSleep() {
    std::printf("x15b: the service stops under a lobby\n");
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
    CHECK(Until(ps, 3000, [&] { return ClientsOf(a) == 1; }));
    CHECK(b.e().StopService(kServiceSleep));
    CHECK(Until(ps, 5000, [&] { return CliState(b) == 0x80; }));
    CHECK(EngineClean(b));  // the lobby's end gave the service back, and the stop with it: a game's Initialize works
    u32 ev = 0;
    CHECK_EQ(b.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(b.e().Shutdown(), 0u);
    CHECK(a.e().StopService(kServiceSleep));
    CHECK(Until(ps, 5000, [&] { return a.lobby.ServerState() == 1; }));  // its own network is gone: back to 1
    CHECK(EngineClean(a));
    CHECK_EQ(a.lobby.ServerStartAccepting(false, 6), 0u);  // ... and a new lobby opens
    CHECK(a.e().status() == NetworkStatus::ConnectedAsHost);
}

/// X16: a creator that closes its admission and opens it again; clients that are never accepted; two clients that join in one round trip.
static void XAdmission() {
    std::printf("x16: close and reopen, never accepted, joins in one round trip\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave");
    const Players ps = {&a, &b, &c, &d};
    Ticker ticker(Engines(ps));
    // closed, then open again: a new searcher gets in
    OpenLobby(a);
    CHECK_EQ(a.lobby.ServerEndAccepting(), 0u);
    CHECK_EQ(a.lobby.ServerStartAccepting(false, 6), 0u);
    CHECK(FindGroup(ps, b, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK_EQ(res, 0u);
    // two clients that begin to join in the same round trip get two nodes and each other's names
    CHECK(FindGroup(ps, c, a));
    CHECK(FindGroup(ps, d, a));
    c.lobby.ClientStopScan();
    d.lobby.ClientStopScan();
    u32 rc = 0xFFFFFFFF, rd = 0xFFFFFFFF;
    CHECK(c.lobby.ClientJoinBegin(a.mac(), &rc) == JoinStep::Pending);
    CHECK(d.lobby.ClientJoinBegin(a.mac(), &rd) == JoinStep::Pending);
    bool done_c = false, done_d = false;
    CHECK(Until(ps, 5000, [&] {
        done_c = done_c || c.lobby.ClientJoinPoll(&rc);
        done_d = done_d || d.lobby.ClientJoinPoll(&rd);
        return done_c && done_d;
    }));
    CHECK_EQ(rc, 0u);
    CHECK_EQ(rd, 0u);
    CHECK(CliNode(c) != CliNode(d));
    CHECK(Until(ps, 4000, [&] { return RosterOf(c) == "Alice,Bob,Cleo,Dave" && RosterOf(d) == RosterOf(c) && RosterOf(b) == RosterOf(c); }));
    CHECK_RULES();
}

/// X17: clients the creator never accepts (manual acceptance) are put out when the distribution starts; the accepted one finishes.
static void XNeverAccepted() {
    std::printf("x17: manual acceptance, a client never accepted\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));
    CHECK_EQ(a.lobby.ServerInitialize(3, 0, nullptr), 0u);
    CHECK_EQ(a.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(FindGroup(ps, b, a));
    CHECK(FindGroup(ps, c, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK(Join(ps, c, a, &res));
    CHECK(Until(ps, 3000, [&] { return ClientsOf(a) == 2; }));
    CHECK_EQ(a.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(a.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(ps, 5000, [&] { return CliState(c) == 0x80; }));  // never accepted: put out
    CHECK(Until(ps, 5000, [&] { return a.lobby.ServerState() == 8; }));
    CHECK_EQ(a.lobby.ServerSendPassphrase(kNine), 0u);
    CHECK(Until(ps, 5000, [&] { return a.lobby.ServerState() == 9 && CliState(b) == 9; }));
    CHECK_RULES();
}

int main() {
    std::printf("dlp_net_adv_test: the fake Download Play lobby over the UDS engine, adversarial\n");
    XGameOwnsEngine();
    XFailures();
    XInbox();
    XRejoin();
    XHostile();
    XNames();
    XSpectator();
    XBeacons();
    XLimits();
    XStrangers();
    XRosters();
    XDeparture();
    XPurge();
    XStaleSlot();
    XPresence();
    XSleep();
    XAdmission();
    XNeverAccepted();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
