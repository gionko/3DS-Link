// The fake Download Play lobby over the REAL UDS engine, the scenarios that the second mutation round (2026-10-29, 69 fresh mutants of the current
// code) showed missing from Tests/dlp_net_test.cpp and Tests/dlp_net_adv_test.cpp. Same rig (Tests/dlp_net_rig.hpp), real time.
//
// Scenarios: the table of lobbies heard (a game's engine files nothing, PrepareLobby / ScanOpen keep the table, one slot per host, a game's beacon of
// another transmitter clears nothing, a beacon that is no announcement does not hide the next one, a lobby is listed for 400 ms, a lobby silent for
// 1.7 s is still in the table and can be joined); an engine no lobby ever used (Receive, SetLobbyInbox(false)); the ring (a message of 16 bytes, the
// caller's capacity, SetLobbyInbox re-arms it, the unread messages of a node that leaves, with the ring wrapped and the messages interleaved); the ring
// that a lobby ends with unread messages does not reach the next lobby (as a client, as a creator); the lobby's data channel number belongs to a game
// when no lobby is up; who gets the Logoff when a node leaves (one unicast, the Logoff names the receiver; not a half-open stranger, not a spectator);
// a game's host sends none; a lobby client sends none when a fellow node leaves the room; a Logoff / a node map of another network while Connecting /
// connected; a management frame; Send(0); the creator's goodbye (Deauthentications); the game's own connect path (gate before the NetworkInfo is
// touched, the type, the channel 0 that keeps the old one, the node table, the timeout); the identity a client announces; the network id; the debug
// capture of the lobby's messages; a lobby silent for 24.8 days (the clock is moved through the linker); no memory for the lobby's state (an allocation
// fails through the linker).
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_net_extra_test.cpp Sources/azl/dlp_lobby_net.cpp <the core sources of Tests/run.sh>
//       -Wl,--wrap=_ZN3azl8platform5NowMsEv -Wl,--wrap=malloc -o /tmp/dlp_net_extra_test
//   (the two --wrap options are Y15's and Y16's: they let the test move the clock and fail an allocation; without them those two fail on "hooked")
#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kL = UdsEngine::kLobbyWlanCommId;
static constexpr u8 kI = UdsEngine::kLobbyId8;

/// A beacon of a lobby with a valid announcement.
static WifiPacket LobbyBeacon(const Mac& from, u8 channel = 9) {
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(), app);
    return MakeBeacon(from, kL, kI, 1, 4, app, kAnnounceSize, channel);
}
/// Is the lobby of this host in the engine's table (heard at most 1 s ago)?
static bool Heard(Player& p, const Mac& host) {
    UdsEngine::HeardLobby h;
    for (u32 i = 0; p.e().HeardLobbyAt(i, 1000, &h); ++i) {
        if (h.host == host) {
            return true;
        }
    }
    return false;
}
/// The searcher hears the creator and joins its lobby with the adapter alone (no DlpLobby).
static bool JoinRaw(Player& host, Player& guest, u64 seed, const char* name) {
    if (!guest.net.ScanOpen()) {
        return false;
    }
    if (!WaitFor(3000, [&] { return Heard(guest, host.mac()); })) {
        return false;
    }
    if (!guest.net.ConnectBegin(host.mac(), Me(seed, name))) {
        return false;
    }
    return WaitFor(4000, [&] { return guest.net.ConnectPoll() == 1; });
}
static bool HostSees(Player& host, u16 mask) {
    u16 n = 0, m = 0;
    return host.e().NodeSnapshot(&n, &m) && m == mask;
}
static void SendTag(MockRoom& room, Player& p, u8 to, u8 tag) {
    const u8 m[2] = {tag, 0};
    CHECK(p.net.Send(to, m, 2));
    room.WaitIdle();
}
static bool IsLogoff(const WifiPacket& p) {
    EtherType t;
    u16 type = 0;
    return p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &t) && t == EtherType::EAPoL &&
           GetEapolFrameType(p.data, &type) && type == kEapolLogoffMagic;
}
static int Logoffs(SentFrames& sent) {
    int n = 0;
    for (const WifiPacket& p : sent.All()) {
        n += IsLogoff(p) ? 1 : 0;
    }
    return n;
}

/// Y1: the table of lobbies heard.
static void YHeardTable() {
    std::printf("y1: the table of lobbies heard\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "A"), b(room, 2, "B"), c(room, 3, "C"), d(room, 4, "D");
    UdsEngine::HeardLobby h;
    HeardGroup hg;
    // an engine no lobby has prepared files nothing: the beacon heard before PrepareLobby is not there after it
    a.e().OnWifiPacket(LobbyBeacon(kFake1));
    CHECK(!a.e().HeardLobbyAt(0, 0xFFFFFFFFu, &h));
    CHECK(a.net.ScanOpen());
    CHECK(!a.e().HeardLobbyAt(0, 0xFFFFFFFFu, &h));
    // PrepareLobby and ScanOpen are idempotent: what was heard stays (a second search does not start from nothing)
    a.e().OnWifiPacket(LobbyBeacon(kFake1));
    CHECK(a.e().HeardLobbyAt(0, 1000, &h));
    CHECK(a.e().PrepareLobby());
    CHECK(a.net.ScanOpen());
    CHECK(a.e().HeardLobbyAt(0, 1000, &h));
    CHECK(h.host == kFake1);
    // one slot per host, however many beacons it sends while slots are free
    for (int i = 0; i < 5; ++i) {
        a.e().OnWifiPacket(LobbyBeacon(kFake1));
    }
    u32 listed = 0;
    for (u32 i = 0; a.e().HeardLobbyAt(i, 0xFFFFFFFFu, &h); ++i) {
        ++listed;
    }
    CHECK_EQ(listed, 1u);
    // a game's beacon from ANOTHER transmitter clears nothing of what was heard
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(), app);
    a.e().OnWifiPacket(MakeBeacon(kFake2, 0x0010C000, 1, 1, 4, app, kAnnounceSize, 9));
    CHECK(a.e().HeardLobbyAt(0, 1000, &h));
    CHECK(h.host == kFake1);
    // a beacon of the lobby's ids that is no announcement does not hide the lobbies listed after it
    CHECK(b.net.ScanOpen());
    u8 junk[kAnnounceSize];
    std::memset(junk, 0xEE, sizeof(junk));
    b.e().OnWifiPacket(MakeBeacon(kFake1, kL, kI, 1, 4, junk, kAnnounceSize, 9));
    b.e().OnWifiPacket(LobbyBeacon(kFake2, 10));
    CHECK(b.net.ScanRead(0, &hg));
    CHECK(hg.host == kFake2);
    CHECK(!b.net.ScanRead(1, &hg));
    // the adapter lists a lobby for 400 ms: still there at 300
    CHECK(c.net.ScanOpen());
    bool sampled = false;
    for (int attempt = 0; attempt < 4 && !sampled; ++attempt) {
        const u64 t0 = platform::NowMs();
        c.e().OnWifiPacket(LobbyBeacon(kFake1));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (platform::NowMs() - t0 <= 370) {
            sampled = true;
            CHECK(c.net.ScanRead(0, &hg));
        }
    }
    CHECK(sampled);
    // a lobby silent for 1.4 s is not swept by the next beacon heard (the sweep is 3 s), and at 1.7 s it can still be joined
    CHECK(d.net.ScanOpen());
    d.e().OnWifiPacket(LobbyBeacon(kFake1, 9));
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    d.e().OnWifiPacket(LobbyBeacon(kFake2, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool has1 = false;
    for (u32 i = 0; d.e().HeardLobbyAt(i, 0xFFFFFFFFu, &h); ++i) {
        has1 = has1 || h.host == kFake1;
    }
    CHECK(has1);
    CHECK(d.net.ConnectBegin(kFake1, Me(0xF005, "D")));
    CHECK(d.e().status() == NetworkStatus::Connecting);
    d.net.Disconnect();
    CHECK(EngineClean(d));
}

/// Y2: an engine no lobby ever used pays nothing and breaks on nothing.
static void YFreshEngine() {
    std::printf("y2: an engine no lobby used\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "A");
    u8 from = 0, data[16];
    u32 size = 0;
    CHECK(!a.net.Receive(&from, data, 16, &size));
    CHECK(!a.e().PopLobbyMessage(&from, data, 16, &size));
    {
        CountNews counted;
        a.e().SetLobbyInbox(false);  // switching it off allocates nothing
        CHECK_EQ(counted.count(), 0u);
    }
    {
        CountNews counted;
        a.e().SetLobbyInbox(true);  // switching it on does (the state is lazy)
        CHECK(counted.count() > 0u);
    }
}

/// Y3: the ring: a message of 16 bytes, the caller's room, SetLobbyInbox re-arms it, and what a node that leaves takes with it.
static void YRing() {
    std::printf("y3: the ring: 16 bytes, capacity, re-arm, purge with the ring wrapped\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave");
    const Players ps = {&a, &b, &c, &d};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    CHECK(JoinRaw(a, c, 0xF003, "Cleo"));
    CHECK(JoinRaw(a, d, 0xF004, "Dave"));
    CHECK(WaitFor(3000, [&] { return HostSees(a, 0xF); }));
    NodeEntry ne;  // the adapter says which node an entry is (the creator reads a client, a client reads itself)
    CHECK(a.net.ReadNode(3, &ne));
    CHECK_EQ(ne.node, 3u);
    CHECK_EQ(ne.seed, 0xF003u);
    CHECK(Str(ne.name) == "Cleo");
    CHECK(b.net.ReadNode(2, &ne));
    CHECK_EQ(ne.node, 2u);
    u8 full[16];
    for (u32 i = 0; i < 16; ++i) {
        full[i] = static_cast<u8>(0x40 + i);
    }
    u8 from = 0, data[16] = {};
    u32 size = 0;
    // the largest message the adapter sends arrives whole
    CHECK(b.net.Send(1, full, 16));
    room.WaitIdle();
    CHECK(a.net.Receive(&from, data, 16, &size));
    CHECK_EQ(size, 16u);
    CHECK_EQ(from, 2u);
    CHECK(std::memcmp(data, full, 16) == 0);
    // the caller's room is the cut, and the message leaves the ring whole
    CHECK(b.net.Send(1, full, 16));
    room.WaitIdle();
    u8 small[5] = {};
    CHECK(a.net.Receive(&from, small, 5, &size));
    CHECK_EQ(size, 5u);
    CHECK(std::memcmp(small, full, 5) == 0);
    CHECK(!a.net.Receive(&from, data, 16, &size));
    // SetLobbyInbox(true) empties the ring, and leaves it on
    SendTag(room, b, 1, '0');
    a.e().SetLobbyInbox(true);
    CHECK(!a.net.Receive(&from, data, 16, &size));
    SendTag(room, b, 1, '0');
    CHECK(a.net.Receive(&from, data, 16, &size));
    // move the read position to 6, then leave five messages unread: they sit in 6, 7, 0, 1, 2
    a.e().SetLobbyInbox(true);
    for (int i = 0; i < 6; ++i) {
        SendTag(room, b, 1, '0');
        CHECK(a.net.Receive(&from, data, 16, &size));
    }
    SendTag(room, b, 1, '1');
    SendTag(room, c, 1, '2');
    SendTag(room, b, 1, '3');
    SendTag(room, d, 1, '4');
    SendTag(room, c, 1, '5');
    c.net.Disconnect();  // node 3 leaves: its '2' and '5' go, the others stay in the order they came
    CHECK(WaitFor(3000, [&] { return HostSees(a, 0xB); }));
    u8 tags[8] = {}, froms[8] = {};
    u32 n = 0;
    while (n < 8 && a.net.Receive(&from, data, 16, &size)) {
        CHECK_EQ(size, 2u);
        tags[n] = data[0];
        froms[n] = from;
        ++n;
    }
    CHECK_EQ(n, 3u);
    CHECK_EQ(tags[0], '1');
    CHECK_EQ(tags[1], '3');
    CHECK_EQ(tags[2], '4');
    CHECK_EQ(froms[0], 2u);
    CHECK_EQ(froms[1], 2u);
    CHECK_EQ(froms[2], 4u);
}

/// Y4: what a lobby leaves unread does not reach the next lobby of the same engine (a searcher that holds the engine, then joins or hosts again).
static void YStaleRing() {
    std::printf("y4: unread messages of a finished lobby, in the next one\n");
    g_rules.Reset();
    MockRoom room;
    Player a1(room, 1, "Alice"), a2(room, 2, "Carol"), b(room, 3, "Bob");
    const Players ps = {&a1, &a2, &b};
    Ticker ticker(Engines(ps));
    CHECK(a1.net.HostOpen(Ann(4, 1, "One"), 6, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a1, b, 0xF003, "Bob"));
    CHECK(WaitFor(3000, [&] { return HostSees(a1, 3); }));
    SendTag(room, a1, 2, 'x');
    SendTag(room, a1, 2, 'y');  // two messages Bob never reads
    a1.net.HostClose();
    CHECK(WaitFor(4000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    CHECK(b.net.engine_held());
    // Bob joins another lobby with the engine he holds: nothing of the first one is in his ring
    CHECK(a2.net.HostOpen(Ann(4, 1, "Two"), 7, Me(0xF002, "Carol")));
    CHECK(WaitFor(3000, [&] { return Heard(b, a2.mac()); }));
    CHECK(b.net.ConnectBegin(a2.mac(), Me(0xF003, "Bob")));
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    u8 from = 0, data[16];
    u32 size = 0;
    CHECK(!b.net.Receive(&from, data, 16, &size));
    // ... and the same when he becomes a creator with it
    CHECK(WaitFor(3000, [&] { return HostSees(a2, 3); }));
    SendTag(room, a2, 2, 'x');
    SendTag(room, a2, 2, 'y');
    a2.net.HostClose();
    CHECK(WaitFor(4000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    CHECK(b.net.HostOpen(Ann(4, 1, "Bob"), 8, Me(0xF003, "Bob")));
    CHECK(!b.net.Receive(&from, data, 16, &size));
}

/// A game's own UDS between two engines, data on the channel number the lobby uses (when no lobby is up it is any game's to use).
static void GameOnLobbyChannel(MockRoom& room, Player& h, Player& g) {
    u32 ev = 0;
    CHECK_EQ(h.e().Initialize(MakeNode(0xA1A1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK(!h.con.fake_kernel.IsSignaled(ev));  // (a new session: what the last one left signalled is cleared)
    CHECK_EQ(g.e().Initialize(MakeNode(0xB2B2, "GameGuest", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK(!g.con.fake_kernel.IsSignaled(ev));
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(h.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, h.mac().data(), 6);
    info[6] = 11;
    CHECK_EQ(g.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
    u32 event = 0;
    CHECK_EQ(h.e().Bind(1, 0x2000, UdsEngine::kLobbyChannel, 0xFFFF, &event), 0u);
    CHECK_EQ(g.e().Bind(1, 0x2000, UdsEngine::kLobbyChannel, 0xFFFF, &event), 0u);
    const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(g.e().SendTo(1, UdsEngine::kLobbyChannel, 5, 0, hello, 5), 0u);
    CHECK_EQ(h.e().SendTo(2, UdsEngine::kLobbyChannel, 5, 0, hello, 5), 0u);
    room.WaitIdle();
    std::vector<u8> pulled;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(h.e(), 1, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 5u);
    CHECK_EQ(src, 2u);
    CHECK_EQ(PullInto(g.e(), 1, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 5u);
    CHECK_EQ(src, 1u);
    CHECK_EQ(g.e().Shutdown(), 0u);
    CHECK_EQ(h.e().Shutdown(), 0u);
}

/// Y5: the lobby's data channel number is a game's own when no lobby is up: engines no lobby touched, engines a lobby gave back, an engine whose inbox
/// was switched on before it was ever initialised.
static void YGameChannel() {
    std::printf("y5: a game's frames on the lobby's channel number\n");
    g_rules.Reset();
    MockRoom room;
    Player p1(room, 1, "P1"), p2(room, 2, "P2"), q1(room, 3, "Q1"), q2(room, 4, "Q2"), r1(room, 5, "R1"), r2(room, 6, "R2");
    const Players ps = {&p1, &p2, &q1, &q2, &r1, &r2};
    Ticker ticker(Engines(ps));
    GameOnLobbyChannel(room, p1, p2);
    // engines a lobby used and gave back
    CHECK(q1.net.HostOpen(Ann(), 9, Me(0xF001, "Q1")));
    CHECK(JoinRaw(q1, q2, 0xF002, "Q2"));
    CHECK(WaitFor(3000, [&] { return HostSees(q1, 3); }));
    q2.net.Disconnect();
    q1.net.HostClose();
    CHECK(EngineClean(q1));
    CHECK(EngineClean(q2));
    GameOnLobbyChannel(room, q1, q2);
    // engines whose inbox was on while they were not initialised
    r1.e().SetLobbyInbox(true);
    r2.e().SetLobbyInbox(true);
    r1.e().Shutdown();
    r2.e().Shutdown();
    GameOnLobbyChannel(room, r1, r2);
}

/// Y6: who gets the Logoff when a node leaves: one unicast per client, naming the receiver; not a stranger half-way through the handshake, not a spectator.
static void YRefresh() {
    std::printf("y6: the Logoff of a departure: who gets it\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), s(room, 9, "Strg"), sp(room, 8, "Spec");
    const Players ps = {&a, &b, &c, &s, &sp};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    CHECK(JoinRaw(a, c, 0xF003, "Cleo"));
    // a spectator
    const u16 who[10] = {u16('S')};
    CHECK_EQ(sp.e().InitializeForLobby(0xF009, who), 0u);
    sp.e().SetLobbyInbox(true);
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return sp.e().HeardLobbyAt(0, 1000, &heard); }));
    u8 info[0x3C] = {};
    std::memcpy(info, a.mac().data(), 6);
    info[6] = 9;
    CHECK_EQ(sp.e().ConnectBegin(info, sizeof(info), 2), 0u);
    CHECK(WaitFor(4000, [&] { return sp.net.ConnectPoll() == 1; }));
    // a stranger that authenticated and never sent its Start
    WifiPacket auth;
    auth.type = WifiPacket::Type::Authentication;
    auth.transmitter_address = s.mac();
    auth.destination_address = a.mac();
    auth.channel = 9;
    auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
    a.e().OnWifiPacket(auth);
    CHECK(WaitFor(3000, [&] { return HostSees(a, 7); }));
    CHECK(!a.net.Send(0, kMsg, 2));  // node 0 is the spectators' slot, and no lobby message goes there
    Settle(ps, 500);
    SentFrames sent(a.con);
    b.net.Disconnect();  // Bob leaves
    CHECK(WaitFor(3000, [&] { return HostSees(a, 5); }));
    Settle(ps, 600);
    int logoffs = 0;
    bool to_c = false;
    for (const WifiPacket& p : sent.All()) {
        if (!IsLogoff(p)) {
            continue;
        }
        ++logoffs;
        if (p.destination_address == c.mac()) {
            to_c = true;
            EapolLogoff lo;
            CHECK(ParseEapolLogoffFrame(p.data, &lo));
            CHECK_EQ(lo.assigned_node_id, 3u);  // it names the receiver (the real console's unicast does)
            CHECK_EQ(lo.connected_nodes, 2u);
        }
    }
    CHECK(to_c);
    CHECK_EQ(logoffs, 1);  // Cleo's, alone: not the stranger's, not the spectator's, not a broadcast
}

/// A game's host with two clients (no lobby in the engine at all).
static void GameHostWithClients(Player& g, Player& h, Player& k) {
    u32 ev = 0;
    CHECK_EQ(g.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(h.e().Initialize(MakeNode(0xB1, "GameB", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(k.e().Initialize(MakeNode(0xC1, "GameC", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(g.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, g.mac().data(), 6);
    info[6] = 11;
    CHECK_EQ(h.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
    CHECK_EQ(k.e().ConnectToNetwork(info, sizeof(info), 1), 0u);
}

/// Y7: a game's host says nothing to its clients when a node leaves (the engine's behaviour for games does not change).
static void YGameHostQuiet() {
    std::printf("y7: a game's host sends no Logoff when a node leaves\n");
    g_rules.Reset();
    MockRoom room;
    Player g(room, 1, "G"), h(room, 2, "H"), k(room, 3, "K");
    const Players ps = {&g, &h, &k};
    Ticker ticker(Engines(ps));
    GameHostWithClients(g, h, k);
    CHECK(WaitFor(3000, [&] { return HostSees(g, 7); }));
    Settle(ps, 300);
    SentFrames sent(g.con);
    CHECK_EQ(h.e().DisconnectNetwork(), 0u);
    CHECK(WaitFor(3000, [&] { return HostSees(g, 5); }));
    Settle(ps, 600);
    CHECK_EQ(Logoffs(sent), 0);
}

/// Y8: a lobby CLIENT sends nothing when a fellow node leaves the room (only the host tells the others).
static void YClientQuiet() {
    std::printf("y8: a client sends no Logoff when a fellow leaves the room\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    CHECK(JoinRaw(a, c, 0xF003, "Cleo"));
    CHECK(WaitFor(3000, [&] { return HostSees(a, 7); }));
    Settle(ps, 500);
    const std::vector<Mac> all = {a.mac(), b.mac(), c.mac()};
    for (Player* p : ps) {
        p->e().OnRoomMembers(all);
    }
    SentFrames sent(b.con);
    const std::vector<Mac> no_c = {a.mac(), b.mac()};  // Cleo left the room
    a.e().OnRoomMembers(no_c);
    b.e().OnRoomMembers(no_c);
    CHECK(Until(ps, 7000, [&] { return HostSees(a, 3); }));  // after the debounce the host has dropped her
    Settle(ps, 600);
    CHECK_EQ(Logoffs(sent), 0);
}

/// Y9: a Logoff or a node map of ANOTHER network, while Connecting and while connected.
static void YForeign() {
    std::printf("y9: a Logoff and a node map of another network\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), c(room, 3, "Cleo"), s(room, 9, "Strg");
    const Players ps = {&a, &c, &s};
    Ticker ticker(Engines(ps));
    // (1) Cleo is Connecting to a host that never answers (a lobby heard in a beacon only); another network's Logoff names her slot
    CHECK(c.net.ScanOpen());
    c.e().OnWifiPacket(LobbyBeacon(kFake1, 9));
    CHECK(c.net.ConnectBegin(kFake1, Me(0xF003, "Cleo")));
    CHECK_EQ(c.net.ConnectPoll(), 0);
    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(0x77, "Fake", 1);
    nodes[1] = MakeNode(0xF003, "Cleo", 2);
    WifiPacket logoff;
    logoff.type = WifiPacket::Type::Data;
    logoff.transmitter_address = s.mac();  // not the host she asked
    logoff.destination_address = c.mac();
    logoff.channel = 9;
    logoff.data = GenerateEapolLogoffFrame(2, nodes, 4, 2);
    c.e().OnWifiPacket(logoff);
    CHECK_EQ(c.net.ConnectPoll(), 0);  // still waiting for her own host
    logoff.transmitter_address = kFake1;  // the host she asked, but it has not answered her Authentication yet: a Logoff answers a Start, which she has not sent
    c.e().OnWifiPacket(logoff);
    CHECK_EQ(c.net.ConnectPoll(), 0);
    WifiPacket assoc;
    assoc.type = WifiPacket::Type::AssociationResponse;
    assoc.transmitter_address = kFake1;
    assoc.destination_address = c.mac();
    assoc.channel = 9;
    assoc.data = GenerateAssocResponseFrame(0, 1, 0);
    c.e().OnWifiPacket(assoc);  // the host answers: she sends her Start
    CHECK_EQ(c.net.ConnectPoll(), 0);
    logoff.transmitter_address = s.mac();  // (now that she is waiting for her Logoff, the stranger's is refused by its transmitter, not by the wait)
    c.e().OnWifiPacket(logoff);
    CHECK_EQ(c.net.ConnectPoll(), 0);
    logoff.transmitter_address = kFake1;
    c.e().OnWifiPacket(logoff);  // and the Logoff that admits her is hers
    CHECK_EQ(c.net.ConnectPoll(), 1);
    // (2) connected: a node map from another network does not become her table (she would send to the nodes it names)
    SentFrames sent(c.con);
    WifiPacket map;
    map.type = WifiPacket::Type::NodeMap;
    map.transmitter_address = s.mac();
    map.destination_address = kBroadcastMac;
    map.channel = 9;
    map.data = GenerateNodeMapPacket({NodeMapEntry{s.mac(), 3}});
    c.e().OnWifiPacket(map);
    CHECK(c.net.Send(3, kMsg, 2));  // (node 3 is not in her network: nothing goes out)
    room.WaitIdle();
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Data, s.mac()), 0);
    (void)a;
}

/// Y10: a management frame on the lobby's channel is dropped like any other.
static void YManagement() {
    std::printf("y10: a management frame on the lobby's channel\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    Settle(ps, 300);
    u8 from = 0, data[16];
    u32 size = 0;
    while (b.net.Receive(&from, data, 16, &size)) {
    }
    WifiPacket p = LobbyFrame(a.mac(), b.mac(), 2, 1, 1234, kMsg, 2);
    p.data[kLlcHeaderSize + 6] = 1;  // is_management
    b.e().OnWifiPacket(p);
    CHECK(!b.net.Receive(&from, data, 16, &size));
    p = LobbyFrame(a.mac(), b.mac(), 2, 1, 1235, kMsg, 2);  // the same frame, not a management one
    b.e().OnWifiPacket(p);
    CHECK(b.net.Receive(&from, data, 16, &size));
}

/// Y11: the creator's goodbye is a burst of Deauthentications (the lobby's engine runs at the newest version).
static void YGoodbye() {
    std::printf("y11: the creator's Deauthentications\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 2, "Alice"), b(room, 1, "Bob");  // (the FNV-1a hash of MAC ...:02 is even: the odd network id below is the engine's doing)
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    // the game that had the engine before turned the goodbye off (command 0x1C): the next user of the engine starts with it on
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xAA, "Old", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(a.e().SetDeauthOnDestroy(false), 0u);
    CHECK_EQ(a.e().Shutdown(), 0u);
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    SentFrames sent(a.con);
    Settle(ps, 300);
    constexpr std::size_t kIdAt = offsetof(NetworkInfo, network_id_be) - offsetof(NetworkInfo, oui_value);
    u32 beacons = 0;
    for (const WifiPacket& p : sent.All()) {
        if (p.type == WifiPacket::Type::Beacon) {
            const BeaconTags tags = FindBeaconTags(p.data);
            CHECK(tags.network != nullptr && tags.network_size > kIdAt + 4);
            if (tags.network != nullptr && tags.network_size > kIdAt + 4) {
                CHECK_EQ(LoadBE32(tags.network + kIdAt) & 1u, 1u);  // (the id is never 0: bit 0 is set on the hash of the MAC)
                ++beacons;
            }
        }
    }
    CHECK(beacons > 0u);
    a.net.HostClose();
    room.WaitIdle();
    CHECK(sent.Count(WifiPacket::Type::Deauthentication) >= 1);
    CHECK(Until(ps, 2000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));  // and the client is out well before the 3 s of silence
}

/// Y12: the game's own connect path (ConnectToNetwork = ConnectBegin + the wait): gate before the NetworkInfo is touched, type, channel 0.
static void YGameConnect() {
    std::printf("y12: ConnectBegin of a game\n");
    g_rules.Reset();
    MockRoom room;
    Player g(room, 1, "G"), x(room, 2, "X");
    const Players ps = {&g, &x};
    Ticker ticker(Engines(ps));
    u32 ev = 0;
    CHECK_EQ(g.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(g.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    u8 ch = 0;
    CHECK_EQ(g.e().GetChannel(&ch), 0u);
    CHECK_EQ(ch, 11u);  // (a NetworkInfo of channel 0 hosts on the default one)
    // a host that is up refuses to connect, and its NetworkInfo is as it was
    u8 other[0x3C] = {};
    std::memcpy(other, x.mac().data(), 6);
    other[6] = 9;
    CHECK_EQ(g.e().ConnectBegin(other, sizeof(other), 1), res::kWrongState);
    u8 ch2 = 0;
    CHECK_EQ(g.e().GetChannel(&ch2), 0u);
    CHECK_EQ(ch2, 11u);
    CHECK(g.e().status() == NetworkStatus::ConnectedAsHost);
    // a type that is neither client nor spectator is refused and starts nothing
    CHECK_EQ(x.e().Initialize(MakeNode(0xB1, "GameB", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, g.mac().data(), 6);
    info[6] = 7;
    CHECK_EQ(x.e().ConnectBegin(info, sizeof(info), 7), res::kBadArgument);
    CHECK(x.e().status() == NetworkStatus::NotConnected);
    // a NetworkInfo of channel 0 keeps the channel of the last network (7): the Authentication goes out on it
    SentFrames sent(x.con);
    CHECK_EQ(x.e().ConnectBegin(info, sizeof(info), 1), 0u);
    x.e().DisconnectNetwork();
    info[6] = 0;
    CHECK_EQ(x.e().ConnectBegin(info, sizeof(info), 1), 0u);
    bool seen_zero_channel_auth = false;
    int auths = 0;
    for (const WifiPacket& p : sent.All()) {
        if (p.type == WifiPacket::Type::Authentication) {
            ++auths;
            CHECK_EQ(p.channel, 7u);
            seen_zero_channel_auth = seen_zero_channel_auth || p.channel == 0;
        }
    }
    CHECK(auths >= 2);
    CHECK(!seen_zero_channel_auth);
    x.e().DisconnectNetwork();
    // the node table of a game starts with the game's own node
    Player y(room, 5, "Y");
    CHECK_EQ(y.e().Initialize(MakeNode(0xC1, "GameC", 5), kTestSharedMemory, kTestVersion, &ev), 0u);
    NodeInfo own;
    CHECK_EQ(y.e().GetNodeInformation(5, &own), 0u);
    CHECK_EQ(own.friend_code_seed, 0xC1u);
    // a host that nobody hears ends the (blocking) ConnectToNetwork with the timeout, and the engine is idle again
    u8 lost[0x3C] = {};
    std::memcpy(lost, kFake1.data(), 6);
    lost[6] = 9;
    Player z(room, 6, "Z");  // (an engine that never met the game's host above: its admission Logoff would be taken for the answer)
    CHECK_EQ(z.e().Initialize(MakeNode(0xD1, "GameD", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(z.e().ConnectToNetwork(lost, sizeof(lost), 1), res::kConnectTimeout);
    CHECK(z.e().status() == NetworkStatus::NotConnected);
}

/// Y13: the lobby's node is the lobby's: nothing is left over from the game that had the engine before (the identity of the EAPoL-Start, the node table).
static void YCleanNode() {
    std::printf("y13: the identity a client announces\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    u32 ev = 0;
    CHECK_EQ(b.e().Initialize(MakeNode(0xBB, "Old", 9), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(b.e().Shutdown(), 0u);
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    SentFrames sent(b.con);
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    int starts = 0;
    for (const WifiPacket& p : sent.All()) {
        EtherType t;
        u16 type = 0;
        if (p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &t) && t == EtherType::EAPoL &&
            GetEapolFrameType(p.data, &type) && type == kEapolStartMagic) {
            EapolStart start;
            CHECK(ParseEapolStartFrame(p.data, &start));
            CHECK_EQ(start.node.friend_code_seed, 0xF002u);
            CHECK(Str(start.node.username) == "Bob");
            ++starts;
        }
    }
    CHECK(starts > 0);
    // ... and the engine's own node table, before the lobby has numbered anybody, holds no node id of the game that had it
    Player d(room, 4, "Dave");
    CHECK_EQ(d.e().Initialize(MakeNode(0xDD, "Old", 9), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(d.e().Shutdown(), 0u);
    const u16 who[10] = {u16('D')};
    CHECK_EQ(d.e().InitializeForLobby(0xF004, who), 0u);
    NodeInfo none;
    CHECK_EQ(d.e().GetNodeInformation(9, &none), res::kNodeNotFound);
    CHECK_EQ(d.e().Shutdown(), 0u);
}

/// Y14: the debug capture shows the messages the lobby accepted, and only those (not a stranger's, not the copy of a radio_like=2 sender).
static void YCapture() {
    std::printf("y14: the debug capture of the lobby's messages\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), s(room, 9, "Strg");
    const Players ps = {&a, &b, &s};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    CHECK(JoinRaw(a, b, 0xF002, "Bob"));
    Settle(ps, 300);
    CHECK(DebugInit());
    DebugReset();
    g_debug_level = 1;
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 2001, kMsg, 2));  // taken
    b.e().OnWifiPacket(LobbyFrame(s.mac(), b.mac(), 2, 1, 2002, kMsg, 2));  // refused: another transmitter claims node 1
    b.e().OnWifiPacket(LobbyFrame(a.mac(), b.mac(), 2, 1, 2001, kMsg, 2));  // the copy of the first
    g_debug_level = 0;
    int rx = 0;
    DebugRecord rec;
    u8 buf[1700];
    while (DebugPop(&rec, buf, sizeof(buf))) {
        if (rec.kind == kDbgFrame && rec.dir == kDirRx && rec.channel == UdsEngine::kLobbyChannel) {
            ++rx;
            CHECK_EQ(rec.src, 1u);
            CHECK_EQ(rec.dest, 2u);
            CHECK_EQ(rec.size, 2u);
        }
    }
    CHECK_EQ(rx, 1);
    DebugReset();
}

// The test moves the engine's clock through the linker (-Wl,--wrap=_ZN3azl8platform5NowMsEv, the symbol of azl::platform::NowMs): a lobby heard
// 2^31 ms ago (24.8 days of silence) must not read as heard in the future. Without the option the clock does not move and Y15 says so.
extern "C" u64 __real__ZN3azl8platform5NowMsEv() __attribute__((weak));
static std::atomic<u64> g_clock_shift{0};
extern "C" u64 __wrap__ZN3azl8platform5NowMsEv() {
    return __real__ZN3azl8platform5NowMsEv() + g_clock_shift.load(std::memory_order_relaxed);
}

/// Y15: a lobby silent for more than 2^31 ms: gone for the list, for the join and for the sweep (the ages are unsigned).
static void YClockWrap() {
    std::printf("y15: a lobby silent for 24.8 days\n");
    g_rules.Reset();
    MockRoom room;
    Player c(room, 3, "Cleo");
    constexpr u64 kLeap = 0x80000000ull + 5000;
    const u64 before = platform::NowMs();
    g_clock_shift = kLeap;
    const bool hooked = platform::NowMs() - before >= kLeap;
    g_clock_shift = 0;
    CHECK(hooked);  // (link with -Wl,--wrap=_ZN3azl8platform5NowMsEv)
    if (!hooked) {
        return;
    }
    CHECK(c.net.ScanOpen());
    UdsEngine::HeardLobby h;
    HeardGroup hg;
    c.e().OnWifiPacket(LobbyBeacon(kFake1, 9));
    CHECK(c.e().HeardLobbyAt(0, 1000, &h));
    g_clock_shift = kLeap;  // 2^31 ms and 5 s later, no beacon in between: a signed age would be negative, and the lobby a ghost
    CHECK(!c.e().HeardLobbyAt(0, 1000, &h));
    CHECK(!c.net.ScanRead(0, &hg));
    CHECK(!c.net.ConnectBegin(kFake1, Me(0xF003, "Cleo")));
    CHECK(EngineClean(c));
    c.e().OnWifiPacket(LobbyBeacon(kFake2, 10));  // the next beacon heard sweeps it, whatever age the caller accepts
    u32 listed = 0;
    bool old = false;
    for (u32 i = 0; c.e().HeardLobbyAt(i, 0xFFFFFFFFu, &h); ++i) {
        ++listed;
        old = old || h.host == kFake1;
    }
    CHECK_EQ(listed, 1u);
    CHECK(!old);
    g_clock_shift = 0;
}

// The test makes the next allocation of a lobby's size fail through the linker (-Wl,--wrap=malloc: the rig's operator new calls std::malloc).
extern "C" void* __real_malloc(std::size_t) __attribute__((weak));
static thread_local unsigned t_fail_alloc = 0;  // how many of the next mallocs of 200..600 bytes of this thread fail
extern "C" void* __wrap_malloc(std::size_t n) {
    if (t_fail_alloc != 0 && n >= 200 && n <= 600) {
        --t_fail_alloc;
        return nullptr;
    }
    return __real_malloc(n);
}

/// Y16: no memory for the state a lobby keeps (4 lobbies heard, 8 messages: 344 bytes): a refusal, never a crash, and the engine is given back.
static void YNoMemory() {
    std::printf("y16: no memory for the lobby's state\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "A"), b(room, 2, "B"), c(room, 3, "C");
    void* (*volatile allocate)(std::size_t) = &std::malloc;  // (volatile: the pair malloc / free is not optimised away)
    t_fail_alloc = 1;
    void* probe = allocate(344);
    const bool hooked = probe == nullptr;
    std::free(probe);
    t_fail_alloc = 0;
    CHECK(hooked);  // (link with -Wl,--wrap=malloc)
    if (!hooked) {
        return;
    }
    // the scan cannot be opened without the table, and can be when the memory is back
    t_fail_alloc = 1;
    CHECK(!a.e().PrepareLobby());
    CHECK_EQ(t_fail_alloc, 0u);
    t_fail_alloc = 1;
    CHECK(!a.net.ScanOpen());
    CHECK(a.net.ScanOpen());
    // a creator without memory is refused and the engine is the game's again; the next try works
    t_fail_alloc = 1;
    CHECK(!b.net.HostOpen(Ann(), 9, Me(0xF002, "B")));
    CHECK_EQ(t_fail_alloc, 0u);
    CHECK(EngineClean(b));
    CHECK(b.net.HostOpen(Ann(), 9, Me(0xF002, "B")));
    // the engine's own answer
    const u16 who[10] = {u16('C')};
    const u8 app[4] = {1, 2, 3, 4};
    CHECK_EQ(c.e().InitializeForLobby(1, who), 0u);
    t_fail_alloc = 1;
    CHECK_EQ(c.e().BeginHostingLobby(0x2810, 0x55, 6, 4, app, 4), res::kOutOfMemory);
    CHECK(c.e().status() == NetworkStatus::NotConnected);
    t_fail_alloc = 0;
}

/// Y17: the node table a service starts with is empty: a node map that reached the idle engine of an earlier session is not remembered by the next
/// one (a spectator never gets a node map of its own network, so what it holds is what it would trust: here a stranger of the room).
static void YStaleNodeMap() {
    std::printf("y17: a node map heard by an idle engine does not outlive its session\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), s(room, 2, "Spec"), t(room, 9, "Strg");
    const Players ps = {&a, &s, &t};
    Ticker ticker(Engines(ps));
    u32 ev = 0;
    CHECK_EQ(s.e().Initialize(MakeNode(0x51, "Old", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    WifiPacket map;
    map.type = WifiPacket::Type::NodeMap;
    map.transmitter_address = t.mac();
    map.destination_address = kBroadcastMac;
    map.channel = 9;
    map.data = GenerateNodeMapPacket({NodeMapEntry{t.mac(), 3}});
    s.e().OnWifiPacket(map);  // an idle engine takes it
    CHECK_EQ(s.e().Shutdown(), 0u);
    CHECK(a.net.HostOpen(Ann(), 9, Me(0xF001, "Alice")));
    const u16 who[10] = {u16('S')};
    CHECK_EQ(s.e().InitializeForLobby(0xF009, who), 0u);
    s.e().SetLobbyInbox(true);
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return s.e().HeardLobbyAt(0, 1000, &heard); }));
    u8 info[0x3C] = {};
    std::memcpy(info, a.mac().data(), 6);
    info[6] = 9;
    CHECK_EQ(s.e().ConnectBegin(info, sizeof(info), 2), 0u);
    CHECK(WaitFor(4000, [&] { return s.e().ConnectPoll() == 1; }));
    u32 event = 0;
    CHECK_EQ(s.e().Bind(1, 0x2000, 5, 0xFFFF, &event), 0u);
    const u8 body[4] = {1, 2, 3, 4};
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.transmitter_address = t.mac();  // a stranger of the room, not a node of the lobby's network
    p.destination_address = kBroadcastMac;
    p.channel = 9;
    p.data = GenerateDataPayload(body, 4, 5, kBroadcastNetworkNodeId, 3, 0xC001);
    s.e().OnWifiPacket(p);
    std::vector<u8> pulled;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(s.e(), 1, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 0u);
}

int main() {
    std::printf("dlp_net_extra_test: the scenarios of the second mutation round\n");
    YHeardTable();
    YFreshEngine();
    YRing();
    YStaleRing();
    YGameChannel();
    YRefresh();
    YGameHostQuiet();
    YClientQuiet();
    YForeign();
    YManagement();
    YGoodbye();
    YGameConnect();
    YCleanNode();
    YCapture();
    YClockWrap();
    YNoMemory();
    YStaleNodeMap();
    // (rig helpers this binary has no use for)
    (void)&Join;
    (void)&FindGroup;
    (void)&OpenLobby;
    (void)&CliNode;
    (void)&CliState;
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
