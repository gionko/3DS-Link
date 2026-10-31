// The fake Download Play lobby over the REAL UDS engine: who leaves and who comes back (found by the churn fuzz of the second review round,
// 2026-10-29). Same rig as Tests/dlp_net_test.cpp (Tests/dlp_net_rig.hpp), real time.
//
// Scenarios: a console that lost its session silently, rejoins, and a fellow's departure refresh reaches it while it is still Connecting (it must not
// take the refresh for its admission); a console that leaves, rejoins and leaves again inside 3 s (the creator must not keep it as a ghost: the late
// second goodbye guard is for Azahar's guests, not for a lobby); a half-open stranger gets no refresh; a client that loses a peer says nothing; a message
// nobody read in the lobby that ended is not the next lobby's; a lobby closed to new players (accepting 0) admits nobody; and a churn fuzz (random
// joins, leaves, kicks and quick rejoins on one creator with three consoles) after which every client shows the creator's roster and hears exactly
// the right messages.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_net_churn_test.cpp Sources/azl/dlp_lobby_net.cpp <the core sources of Tests/run.sh> -o /tmp/dlp_net_churn_test
#include <random>

#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}


static int LogoffsTo(const std::vector<WifiPacket>& frames, const Mac* to) {
    int n = 0;
    for (const WifiPacket& p : frames) {
        if (p.type != WifiPacket::Type::Data) continue;
        EtherType t;
        if (!GetFrameEtherType(p.data, &t) || t != EtherType::EAPoL) continue;
        u16 magic = 0;
        if (!GetEapolFrameType(p.data, &magic) || magic != kEapolLogoffMagic) continue;
        if (to ? p.destination_address == *to : true) ++n;
    }
    return n;
}

static u16 MaskOf(Player& p) {
    u16 n = 0, m = 0;
    p.e().NodeSnapshot(&n, &m);
    return m;
}

// T-A: join, leave, join again within 3 s, leave again within 3 s: the creator keeps the node of a console that is gone
static void TA_Ghost(u32 gap_ms) {
    std::printf("T-A: join / leave / join / leave, each within %u ms\n", gap_ms);
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(3000, [&] { return b.net.ConnectPoll() == 1; }));
    b.net.Disconnect();  // leave #1
    Settle(ps, gap_ms);
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(3000, [&] { return b.net.ConnectPoll() == 1; }));
    CHECK_EQ(MaskOf(a), 3u);
    Settle(ps, gap_ms);
    b.net.Disconnect();  // leave #2
    Settle(ps, 600);
    std::printf("   the creator's node mask 600 ms after the last leave: 0x%x (1 = right, 3 = a ghost of Bob)\n", MaskOf(a));
    CHECK_EQ(MaskOf(a), 1u);
}

// G1: a stranger that authenticated and never sent its Start (half-open) must not get the departure's Logoff (it would take it for its admission)
static void G1_HalfOpen() {
    std::printf("G1: no Logoff to a half-open entry\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), s(room, 9, "Strg");
    const Players ps = {&a, &b, &c, &s};
    SentFrames sent(a.con);
    Ticker ticker(Engines(ps));
    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    CHECK(FindGroup(ps, c, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK(Join(ps, c, a, &res));
    WifiPacket auth;
    auth.type = WifiPacket::Type::Authentication;
    auth.transmitter_address = s.mac();
    auth.destination_address = a.mac();
    auth.channel = 6;
    auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
    a.e().OnWifiPacket(auth);  // S is half-open on the creator now
    Settle(ps, 300);
    const std::vector<WifiPacket> before = sent.All();
    CHECK_EQ(c.lobby.ClientStopSession(), 0u);  // a console leaves: the others get the refresh
    Settle(ps, 600);
    const std::vector<WifiPacket> after = sent.All();
    std::vector<WifiPacket> delta(after.begin() + before.size(), after.end());
    std::printf("   Logoffs after the departure: to Bob %d, to the half-open stranger %d\n", LogoffsTo(delta, &b.mac()), LogoffsTo(delta, &s.mac()));
    CHECK_EQ(LogoffsTo(delta, &s.mac()), 0);
    CHECK(LogoffsTo(delta, &b.mac()) >= 1);
}

// G3: a CLIENT that loses a peer (the peer left the room) says nothing to anybody
static void G3_ClientSilent() {
    std::printf("G3: a client that loses a peer sends no Logoff\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    SentFrames sent(b.con);
    Ticker ticker(Engines(ps));
    OpenLobby(a);
    CHECK(FindGroup(ps, b, a));
    CHECK(FindGroup(ps, c, a));
    u32 res = 0;
    CHECK(Join(ps, b, a, &res));
    CHECK(Join(ps, c, a, &res));
    CHECK(WaitFor(3000, [&] { return MaskOf(b) == 7u; }));
    const std::vector<Mac> all = {a.mac(), b.mac(), c.mac()};
    const std::vector<Mac> no_c = {a.mac(), b.mac()};
    b.e().OnRoomMembers(all);
    b.e().OnRoomMembers(no_c);  // C left the room: after the debounce B's engine drops its node
    const std::vector<WifiPacket> before = sent.All();
    CHECK(Until(ps, 7000, [&] { return MaskOf(b) == 3u; }));
    const std::vector<WifiPacket> after = sent.All();
    std::vector<WifiPacket> delta(after.begin() + before.size(), after.end());
    std::printf("   Logoffs sent by the client B: %d\n", LogoffsTo(delta, nullptr));
    CHECK_EQ(LogoffsTo(delta, nullptr), 0);
}

// G4: a message nobody read in the lobby that ended is not the new lobby's
static void G4_RejoinRing() {
    std::printf("G4: the ring of the old lobby does not follow a rejoin\n");
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
    CHECK(WaitFor(2000, [&] { return MaskOf(a1) == 3u; }));
    CHECK(a1.net.Send(2, kMsg, 2));  // unread by B
    room.WaitIdle();
    a1.net.HostClose();
    CHECK(WaitFor(4000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    CHECK(b.net.engine_held());
    CHECK(b.net.ConnectBegin(a2.mac(), Me(0xF003, "Bob")));
    CHECK(WaitFor(4000, [&] { return b.net.ConnectPoll() == 1; }));
    u8 from = 0, data[16];
    u32 size = 0;
    const bool stale = b.net.Receive(&from, data, 16, &size);
    std::printf("   a message from the old lobby is delivered in the new one: %s\n", stale ? "YES" : "no");
    CHECK(!stale);
}

// R: the creator's stale entry of a console that lost its session silently, a departure, and the console already Connecting again: it takes the refresh for its admission
static void R_Race() {
    std::printf("R: a silent loss, the rejoin, and another player's departure in between\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    const Players ps = {&a, &b, &c};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    CHECK(c.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard) && c.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(3000, [&] { return b.net.ConnectPoll() == 1; }));
    CHECK(c.net.ConnectBegin(a.mac(), Me(0xF003, "Cleo")));
    CHECK(WaitFor(3000, [&] { return c.net.ConnectPoll() == 1; }));
    CHECK(WaitFor(2000, [&] { return MaskOf(a) == 7u && MaskOf(b) == 7u; }));
    // B's session ends with no goodbye (the room link blinked): the creator still has B
    CHECK(b.e().OnTransportDown(NetworkStatusChangeReason::ConnectionLost));
    CHECK(b.e().status() == NetworkStatus::NotConnected);
    CHECK_EQ(MaskOf(a), 7u);
    // B's frames reach the room 30 ms late (a slow link); C leaves while B is already Connecting
    b.con.sniff = [](const WifiPacket&) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); };
    std::thread leaver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        c.net.Disconnect();
    });
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    leaver.join();
    b.con.sniff = nullptr;
    Settle(ps, 1500);
    const NetworkStatus st = b.e().status();
    std::printf("   Bob: %s, node mask 0x%x; the creator's mask 0x%x\n", st == NetworkStatus::ConnectedAsClient ? "CONNECTED as client" : st == NetworkStatus::Connecting ? "connecting" : "idle", MaskOf(b), MaskOf(a));
    const bool ghost = st == NetworkStatus::ConnectedAsClient && MaskOf(a) == 1u;
    std::printf("   Bob believes he is in the lobby and the creator has no Bob: %s\n", ghost ? "YES" : "no");
    CHECK(!ghost);
}

// C: a lobby closed to new players (accepting 0) admits nobody; opened again, it admits the same console
static void C_Closed() {
    std::printf("C: a lobby that is closed to new players\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob");
    const Players ps = {&a, &b};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(4, 0, "Closed"), 6, Me(0xF001, "Alice")));
    CHECK(b.net.ScanOpen());
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    Settle(ps, 1500);
    CHECK_EQ(b.net.ConnectPoll(), 0);
    CHECK_EQ(MaskOf(a), 1u);
    b.net.Disconnect();
    a.net.HostUpdate(Ann(4, 1, "Open"));
    Settle(ps, 600);
    CHECK(b.net.ScanOpen());
    CHECK(WaitFor(3000, [&] { return b.e().HeardLobbyAt(0, 1000, &heard); }));
    CHECK(b.net.ConnectBegin(a.mac(), Me(0xF002, "Bob")));
    CHECK(WaitFor(3000, [&] { return b.net.ConnectPoll() == 1; }));
    CHECK_EQ(MaskOf(a), 3u);
}

static u32 g_diverged = 0;

static bool IsClient(Player& p) {
    return p.e().status() == NetworkStatus::ConnectedAsClient;
}

// every console that is a client of the host must show the host's roster (same mask, same identities)
static bool Consistent(Player& host, Player** cs, int n, std::string* why) {
    u16 hn = 0, hm = 0;
    if (!host.e().NodeSnapshot(&hn, &hm)) {
        *why = "host not in a network";
        return false;
    }
    int connected_on_host = 0;
    for (int i = 0; i < n; ++i) {
        Player& c = *cs[i];
        if (!IsClient(c)) continue;
        ++connected_on_host;
        u16 cn = 0, cm = 0;
        c.e().NodeSnapshot(&cn, &cm);
        if (cm != hm) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "client %d (node %u) mask 0x%x != host mask 0x%x", i, cn, cm, hm);
            *why = buf;
            return false;
        }
        for (u16 id = 1; id <= 4; ++id) {
            if (!(hm & (1u << (id - 1)))) continue;
            NodeInfo a, b;
            if (host.e().GetNodeInformation(id, &a) != 0 || c.e().GetNodeInformation(id, &b) != 0 ||
                a.friend_code_seed != b.friend_code_seed || std::memcmp(a.username, b.username, sizeof(a.username)) != 0) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "client %d: identity of node %u differs from the host's", i, id);
                *why = buf;
                return false;
            }
        }
    }
    if (__builtin_popcount(hm) != 1 + connected_on_host) {
        // a host node that is not a client of this host (or the other way round)
        char buf[160];
        std::snprintf(buf, sizeof(buf), "host mask 0x%x counts %d, clients really connected: %d", hm, __builtin_popcount(hm), connected_on_host);
        *why = buf;
        return false;
    }
    return true;
}

static void Fuzz(u32 seed, int steps) {
    std::printf("fuzz seed %u\n", seed);
    g_rules.Reset();
    std::mt19937 rng(seed);
    MockRoom room;
    Player a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave");
    Player* cs[3] = {&b, &c, &d};
    const Players ps = {&a, &b, &c, &d};
    Ticker ticker(Engines(ps));
    CHECK(a.net.HostOpen(Ann(), 6, Me(0xF001, "Alice")));
    for (Player* p : cs) CHECK(p->net.ScanOpen());
    UdsEngine::HeardLobby h;
    CHECK(WaitFor(3000, [&] { return d.e().HeardLobbyAt(0, 1000, &h) && b.e().HeardLobbyAt(0, 1000, &h) && c.e().HeardLobbyAt(0, 1000, &h); }));
    for (int step = 0; step < steps; ++step) {
        const int ops = 1 + static_cast<int>(rng() % 3);
        std::string log;
        for (int k = 0; k < ops; ++k) {
            const int who = static_cast<int>(rng() % 3);
            Player& p = *cs[who];
            const u32 what = rng() % 6;
            char t[48];
            if (what <= 1) {  // join (or rejoin without a goodbye if it was kicked and still holds the engine)
                if (!IsClient(p) && p.e().status() != NetworkStatus::Connecting) {
                    const bool ok = p.net.ConnectBegin(a.mac(), Me(0xF010 + who, who == 0 ? "Bob" : who == 1 ? "Cleo" : "Dave"));
                    std::snprintf(t, sizeof(t), " join(%d)=%d", who, ok);
                    log += t;
                }
            } else if (what == 2) {  // leave
                p.net.Disconnect();
                std::snprintf(t, sizeof(t), " leave(%d)", who);
                log += t;
            } else if (what == 3) {  // kick by node
                a.net.Kick(static_cast<u8>(2 + rng() % 3));
                log += " kick";
            } else if (what == 4) {  // leave and at once join again
                p.net.Disconnect();
                const bool ok = p.net.ConnectBegin(a.mac(), Me(0xF010 + who, who == 0 ? "Bob" : who == 1 ? "Cleo" : "Dave"));
                std::snprintf(t, sizeof(t), " bounce(%d)=%d", who, ok);
                log += t;
            } else {  // nothing, a short pause
                std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 30));
            }
        }
        // a Connecting console that the host answered becomes a client; the rest settles
        Settle(ps, 700);
        // (a console still Connecting after 700 ms is waiting for a full lobby: give it its 3 s)
        std::string why;
        bool ok = Consistent(a, cs, 3, &why);
        if (!ok) {
            Settle(ps, 1500);  // one more look, in case the round was slow
            ok = Consistent(a, cs, 3, &why);
        }
        if (!ok) {
            ++g_diverged;
            std::printf("  step %d:%s -> DIVERGED: %s\n", step, log.c_str(), why.c_str());
            continue;
        }
        // messages: drain, then the host speaks to every client and every client to the host; each hears exactly the right one from the right node
        u8 from = 0, data[16];
        u32 size = 0;
        for (Player* p : cs) while (p->net.Receive(&from, data, 16, &size)) {}
        while (a.net.Receive(&from, data, 16, &size)) {}
        for (int i = 0; i < 3; ++i) {
            if (!IsClient(*cs[i])) continue;
            const u8 node = cs[i]->net.MyNode();
            const u8 to_client[2] = {static_cast<u8>(0xA0 + step), node};
            const u8 to_host[2] = {static_cast<u8>(0xB0 + step), node};
            a.net.Send(node, to_client, 2);
            cs[i]->net.Send(1, to_host, 2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 3; ++i) {
            if (!IsClient(*cs[i])) continue;
            const u8 node = cs[i]->net.MyNode();
            u32 got = 0;
            bool right = true;
            while (cs[i]->net.Receive(&from, data, 16, &size)) {
                ++got;
                right = right && from == 1 && size == 2 && data[0] == 0xA0 + step && data[1] == node;
            }
            if (got != 1 || !right) {
                ++g_diverged;
                std::printf("  step %d:%s -> client %d (node %u) heard %u message(s), right=%d\n", step, log.c_str(), i, node, got, right);
            }
        }
        std::vector<int> seen;
        while (a.net.Receive(&from, data, 16, &size)) {
            if (size != 2 || data[0] != 0xB0 + step || data[1] != from) {
                ++g_diverged;
                std::printf("  step %d:%s -> host heard a wrong message from node %u\n", step, log.c_str(), from);
            }
            seen.push_back(from);
        }
        int expect = 0;
        for (int i = 0; i < 3; ++i) expect += IsClient(*cs[i]) ? 1 : 0;
        if (static_cast<int>(seen.size()) != expect) {
            ++g_diverged;
            std::printf("  step %d:%s -> host heard %zu messages, expected %d\n", step, log.c_str(), seen.size(), expect);
        }
    }
    CHECK_RULES();
}


int main() {
    std::printf("dlp_net_churn_test: who leaves and who comes back\n");
    R_Race();
    TA_Ghost(500);
    TA_Ghost(1500);
    G1_HalfOpen();
    G3_ClientSilent();
    G4_RejoinRing();
    C_Closed();
    for (u32 seed : {1u, 2u, 3u}) {
        Fuzz(seed, 22);
    }
    CHECK_EQ(g_diverged, 0u);
    // (rig helpers this binary has no use for)
    (void)&LobbyFrame;
    (void)&MakeBeacon;
    (void)&EngineClean;
    (void)&CliNode;
    (void)&CliState;
    (void)&Str;
    std::printf("diverged steps: %u\n%d checks, %d failures\n", g_diverged, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
