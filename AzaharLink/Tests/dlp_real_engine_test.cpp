// The UDS engine's half of the REAL Download Play (1.1.3, phase F2 of analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md):
// the beacons of the real Download Play filed as lobbies (E1, SetLobbyBeaconKind), the network the engine composes for it
// (BeginHostingDlp / ConnectBeginDlp, the flag dlp_net_), the spectators counted (SpectatorCount) and the rules of D16 in that network.
// Same rig as Tests/dlp_net_test.cpp (Tests/dlp_net_rig.hpp: engines joined by the in-memory room, real time); the consoles of the
// real Download Play are engines driven the way the adapter of F3/F4 (RealDlpNet) will drive them: InitializeForLobby, then
// BeginHostingDlp or ConnectBeginDlp, binds on channel 2, Shutdown to end (with the foreign-lobby filter on while a session is up,
// decision Q8 of the design).
//
// Scenarios:
//   E1   the table of lobbies heard by kind: Azahar's beacons (random id8, network id and attributes, no application data, also a tag
//        too short for the size byte) filed only under RealDlp, the dialect's only under Ours, a host that turns to its game's network
//        forgotten, a kind change empties the table, an engine no lobby prepared files nothing (no null memcpy: UBSan)
//   D11  the host's beacon (ids, 52-byte tag, no data, network id, channel), the gates and arguments of both new calls, the inbox
//        stays off (a frame on the lobby's channel goes to a bind), Shutdown clears the network's rules
//   D12  spectators counted and never nodes (GetConnectingClients sees the mask: 1 with a spectator in, 1 after its goodbye),
//        a broadcast eject and the end of the network, the room's member list, a network that is full answers no spectator (R12)
//   CH   the host on channel 6 admits a client and a spectator whose frames come on channel 11 (Azahar's client, capture 1c)
//   R7   spectator -> goodbye -> client within 100 ms and within 2 s -> goodbye within 3 s: no ghost (D16c); client -> spectator ->
//        goodbye: no ghost spectator; the game's own network keeps the guard (unchanged)
//   R7b  a Connecting client takes no Logoff before the host's answer (D16a) and no Logoff that names node 0 (D16b); a spectator
//        does; a game's client still adopts it (unchanged); with real engines a spectator and a client that join together
//   R11b the churn and extra scenarios in the real Download Play's network: join / leave / join / leave within 3 s, the silent loss
//        and the rejoin, the creator's goodbye, a Logoff and a node map of another network, a stale node map, and two churn fuzzes
//        (two nodes, as Luigi's Mansion: rosters and messages; four nodes: no ghost, no node 0)
//   R13  (the client's half) a searcher of the dialect does not see a real Download Play's host and cannot join it, a searcher of
//        the real Download Play does not see a lobby of the dialect
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_engine_test.cpp Sources/azl/dlp_lobby_net.cpp <the core sources of Tests/run.sh> -o /tmp/dlp_real_engine_test
#include <random>

#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

using Kind = UdsEngine::LobbyBeaconKind;
static constexpr u32 kDlpComm = UdsEngine::kLobbyWlanCommId;
static constexpr u8 kChData = 2;  // the dialogue's channel of the real Download Play
static constexpr u32 kBindId = 1;

/// A console of the real Download Play: its engine, driven as RealDlpNet will drive it.
struct DNode {
    DNode(MockRoom& room, u8 last, const char* name) : con(room, last), seed(0xD000u + last) { Put(name_, name); }
    UdsEngine& e() { return *con.engine; }
    const Mac& mac() const { return con.mac; }
    /// Hosts the real Download Play's network (max nodes, channel), with a bind of channel 2 for every node.
    bool Host(u8 max_nodes = 2, u8 channel = 6) {
        if (e().InitializeForLobby(seed, name_) != 0) {
            return false;
        }
        if (e().BeginHostingDlp(max_nodes, channel) != 0) {
            e().Shutdown();
            return false;
        }
        u32 ev = 0;
        return e().Bind(kBindId, 0x2000, kChData, 0xFFFF, &ev) == 0;
    }
    /// Joins `host` (client or spectator) on `channel`: the foreign filter on, a bind of channel 2 for the host's frames.
    bool Join(const Mac& host, u8 channel = 6, ConnectionType type = ConnectionType::Client) {
        if (e().initialized()) {
            Leave();
        }
        if (e().InitializeForLobby(seed, name_) != 0) {
            return false;
        }
        e().SetClientForeignFilterEnforced(true);
        u32 ev = 0;
        if (e().Bind(kBindId, 0x2000, kChData, 1, &ev) != 0) {
            Leave();
            return false;
        }
        if (e().ConnectBeginDlp(host, channel, type) != 0) {
            Leave();
            return false;
        }
        return true;
    }
    int Poll() { return e().ConnectPoll(); }
    /// Release: Shutdown (the goodbye, the binds) and the filter off whatever happened.
    void Leave() {
        e().Shutdown();
        e().SetClientForeignFilterEnforced(false);
    }
    u16 MyNode() {
        u16 n = 0, m = 0;
        e().NodeSnapshot(&n, &m);
        return n;
    }
    u16 Mask() {
        u16 n = 0, m = 0;
        e().NodeSnapshot(&n, &m);
        return m;
    }
    bool IsClient() { return e().status() == NetworkStatus::ConnectedAsClient; }
    Console con;
    u64 seed;
    u16 name_[kNameUnits];
};

/// Ticks the engines of the real Download Play's consoles (the beacons).
static std::vector<UdsEngine*> Es(std::initializer_list<DNode*> ns) {
    std::vector<UdsEngine*> v;
    for (DNode* n : ns) {
        v.push_back(&n->e());
    }
    return v;
}

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// A beacon of the real Download Play as Azahar sends it: its wlancommid, `id8`, `network_id` and `attributes` as given (Azahar
/// leaves them uninitialised), `app_n` bytes of application data (0 for Azahar's).
static WifiPacket DlpBeacon(const Mac& from, u8 id8, u32 network_id, u16 attributes, u8 total, u8 max, u8 channel,
                            u32 wlan = kDlpComm, u8 app_n = 0) {
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    std::memcpy(ni.oui_value, kNintendoOui, 3);
    ni.oui_type = 21;
    StoreBE32(ni.wlan_comm_id_be, wlan);
    ni.id = id8;
    StoreBE32(ni.network_id_be, network_id);
    StoreBE16(ni.attributes_be, attributes);
    ni.total_nodes = total;
    ni.max_nodes = max;
    ni.channel = channel;
    ni.application_data_size = app_n;
    for (u8 i = 0; i < app_n; ++i) {
        ni.application_data[i] = static_cast<u8>(0x40 + i);
    }
    std::vector<NodeInfo> nodes(max);
    nodes[0] = MakeNode(0x77, "Host", 1);
    WifiPacket p;
    p.type = WifiPacket::Type::Beacon;
    p.data = GenerateBeaconFrame(ni, nodes);
    p.transmitter_address = from;
    p.destination_address = kBroadcastMac;
    p.channel = channel;
    return p;
}

static bool HeardHost(UdsEngine& e, const Mac& host, UdsEngine::HeardLobby* out = nullptr) {
    UdsEngine::HeardLobby h;
    for (u32 i = 0; e.HeardLobbyAt(i, 1000, &h); ++i) {
        if (h.host == host) {
            if (out) {
                *out = h;
            }
            return true;
        }
    }
    return false;
}
static u32 HeardCount(UdsEngine& e) {
    UdsEngine::HeardLobby h;
    u32 n = 0;
    while (e.HeardLobbyAt(n, 0xFFFFFFFFu, &h)) {
        ++n;
    }
    return n;
}

static WifiPacket Logoff(const Mac& from, const Mac& to, u16 assigned, const std::vector<NodeInfo>& nodes, u8 max, u8 total) {
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.transmitter_address = from;
    p.destination_address = to;
    p.channel = 9;
    p.data = GenerateEapolLogoffFrame(assigned, nodes, max, total);
    return p;
}
static WifiPacket Assoc(const Mac& from, const Mac& to) {
    WifiPacket p;
    p.type = WifiPacket::Type::AssociationResponse;
    p.transmitter_address = from;
    p.destination_address = to;
    p.channel = 9;
    p.data = GenerateAssocResponseFrame(0, 1, 0);
    return p;
}

/// CHECK_RULES without "start-to-full", for the scenarios where two consoles go for the last place of a host: the checker learns a
/// host's node count from its Logoffs only, and the real Download Play's host sends none when a node leaves (D17), so it still counts
/// a node that is gone; and the true race - two Authentications answered while one place was free, then two Starts - is the engine's
/// handshake since 1.0 (a game's ConnectBegin does the same), not this phase's. The ones left out are counted and printed.
static void CheckRulesButFull(const char* where) {
    ++g_checks;
    u32 full = 0;
    std::string other;
    for (const AzaharRules::Violation& v : g_rules.violations()) {
        if (v.rule == "start-to-full") {
            ++full;
        } else {
            other += "    " + v.rule + ": " + v.detail + "\n";
        }
    }
    std::printf("   %s: %u EAPoL-Start to a host the checker counts as full (left out, see the comment)\n", where, full);
    if (!other.empty()) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL %s: Azahar-peer rules broken:\n%s", where, other.c_str());
    }
}

static int DeauthsTo(SentFrames& sent, const Mac& to) {
    return sent.CountTo(WifiPacket::Type::Deauthentication, to);
}

/// Drains the bind of channel 2: how many frames, each must be `size` bytes from `src` with `tag` first.
static u32 Drain(DNode& n, u16 want_src, u8 tag, bool* right) {
    u32 got = 0;
    for (;;) {
        std::vector<u8> out;
        u32 size = 0;
        u16 src = 0;
        if (PullInto(n.e(), kBindId, &out, &size, &src) != 0 || size == 0) {
            return got;
        }
        ++got;
        if (right) {
            *right = *right && src == want_src && size == 2 && out[0] == tag;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------

/// E1: which beacons are lobbies, by kind.
static void E1_Kinds() {
    std::printf("E1: the beacons of the real Download Play are lobbies only under RealDlp\n");
    g_rules.Reset();
    MockRoom room;
    DNode x(room, 1, "X"), y(room, 2, "Y");
    // an engine no lobby prepared files nothing, of either kind (and does not even parse it)
    x.e().SetLobbyBeaconKind(Kind::RealDlp);
    x.e().OnWifiPacket(DlpBeacon(kFake1, 0x9C, 0x12345678, 0xBEEF, 1, 2, 6));
    CHECK_EQ(HeardCount(x.e()), 0u);
    x.e().SetLobbyBeaconKind(Kind::Ours);
    CHECK(y.e().PrepareLobby());
    // Ours (the default): Azahar's beacon is no lobby
    y.e().OnWifiPacket(DlpBeacon(kFake1, 0x9C, 0x12345678, 0xBEEF, 1, 2, 6));
    CHECK(!HeardHost(y.e(), kFake1));
    // RealDlp: filed, with whatever id8 / network id / attributes (64 random ones, the 0x55 and 0x00 of the capture included)
    y.e().SetLobbyBeaconKind(Kind::RealDlp);
    std::mt19937 rng(7);
    for (int i = 0; i < 66; ++i) {
        const u8 id8 = i == 0 ? 0x55 : i == 1 ? 0x00 : static_cast<u8>(rng());
        y.e().OnWifiPacket(DlpBeacon(kFake1, id8, rng(), static_cast<u16>(rng()), 1, 2, 6));
        UdsEngine::HeardLobby h;
        CHECK(HeardHost(y.e(), kFake1, &h));
        if (i == 65) {
            CHECK_EQ(h.size, 0u);
            CHECK_EQ(h.channel, 6u);
            CHECK_EQ(h.total_nodes, 1u);
            CHECK_EQ(h.max_nodes, 2u);
            bool zero = true;
            for (u8 b : h.data) {
                zero = zero && b == 0;
            }
            CHECK(zero);
        }
    }
    CHECK_EQ(HeardCount(y.e()), 1u);  // one slot per host
    // the dialect's beacon (with its announcement) is not of this kind; its host is forgotten
    u8 app[kAnnounceSize];
    EncodeAnnounce(Ann(), app);
    y.e().OnWifiPacket(MakeBeacon(kFake2, kDlpComm, UdsEngine::kLobbyId8, 1, 4, app, kAnnounceSize, 9));
    CHECK(!HeardHost(y.e(), kFake2));
    y.e().OnWifiPacket(DlpBeacon(kFake2, 0x55, 1, 0, 1, 2, 9));
    CHECK(HeardHost(y.e(), kFake2));
    y.e().OnWifiPacket(MakeBeacon(kFake2, kDlpComm, UdsEngine::kLobbyId8, 1, 4, app, kAnnounceSize, 9));
    CHECK(!HeardHost(y.e(), kFake2));
    // one byte of application data is enough to be no lobby of this kind
    y.e().OnWifiPacket(DlpBeacon(kFake2, 0x55, 1, 0, 1, 2, 9, kDlpComm, 1));
    CHECK(!HeardHost(y.e(), kFake2));
    // a tag too short to hold the size byte counts as no data (filed); the same beacon is no lobby of the dialect
    WifiPacket cut = DlpBeacon(kFake2, 0x31, 99, 0, 1, 3, 9);
    {
        const BeaconTags tags = FindBeaconTags(cut.data);
        CHECK(tags.network != nullptr);
        const std::size_t at = static_cast<std::size_t>(tags.network - cut.data.data());
        cut.data[at - 1] = 0x20;
        cut.data.resize(at + 0x20);
        const BeaconTags again = FindBeaconTags(cut.data);
        const u8* d = nullptr;
        u32 n = 0;
        CHECK(again.network != nullptr && again.network_size == 0x20u && !GetBeaconApplicationData(again, &d, &n));
    }
    y.e().OnWifiPacket(cut);
    UdsEngine::HeardLobby hc;
    CHECK(HeardHost(y.e(), kFake2, &hc));
    CHECK_EQ(hc.max_nodes, 3u);
    // a tag too short for the node counts is nothing (no read past it)
    WifiPacket tiny = DlpBeacon(kFake2, 0x31, 99, 0, 1, 3, 9);
    {
        const BeaconTags tags = FindBeaconTags(tiny.data);
        const std::size_t at = static_cast<std::size_t>(tags.network - tiny.data.data());
        tiny.data[at - 1] = 0x11;  // (max_nodes would be at +0x11)
        tiny.data.resize(at + 0x11);
    }
    y.e().OnWifiPacket(tiny);
    CHECK(!HeardHost(y.e(), kFake2));
    // the host that turns to its game's network (another wlancommid, the capture's 001d1a10/55) is forgotten: only the table
    CHECK(HeardHost(y.e(), kFake1));
    y.e().OnWifiPacket(DlpBeacon(kFake1, 0x55, 0x3d7b8148, 0, 2, 2, 6, 0x001D1A10));
    CHECK(!HeardHost(y.e(), kFake1));
    // a change of kind empties the table; Ours files the dialect's again
    y.e().OnWifiPacket(DlpBeacon(kFake1, 0x55, 5, 0, 1, 2, 6));
    CHECK(HeardHost(y.e(), kFake1));
    y.e().SetLobbyBeaconKind(Kind::Ours);
    CHECK_EQ(HeardCount(y.e()), 0u);
    y.e().OnWifiPacket(DlpBeacon(kFake1, 0x55, 5, 0, 1, 2, 6));
    CHECK(!HeardHost(y.e(), kFake1));
    y.e().OnWifiPacket(MakeBeacon(kFake2, kDlpComm, UdsEngine::kLobbyId8, 1, 4, app, kAnnounceSize, 9));
    CHECK(HeardHost(y.e(), kFake2));
    y.e().SetLobbyBeaconKind(Kind::Ours);  // (the same kind again keeps the table)
    CHECK(HeardHost(y.e(), kFake2));
}

/// D11: the network the engine composes, and the gates of the two calls.
static void D11_Calls() {
    std::printf("D11: BeginHostingDlp / ConnectBeginDlp\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 2, "Host"), c(room, 3, "Cli"), s(room, 4, "Spc");
    Ticker ticker(Es({&h, &c, &s}));
    // gates: not initialised, then bad arguments, then not idle
    CHECK_EQ(h.e().BeginHostingDlp(2, 6), res::kWrongState);
    CHECK_EQ(c.e().ConnectBeginDlp(h.mac(), 6, ConnectionType::Client), res::kWrongState);
    CHECK_EQ(h.e().InitializeForLobby(h.seed, h.name_), 0u);
    CHECK_EQ(h.e().BeginHostingDlp(1, 6), res::kBadId);
    CHECK_EQ(h.e().BeginHostingDlp(kUdsMaxNodes + 1, 6), res::kBadId);
    CHECK(h.e().status() == NetworkStatus::NotConnected);
    CHECK_EQ(c.e().InitializeForLobby(c.seed, c.name_), 0u);
    CHECK_EQ(c.e().ConnectBeginDlp(h.mac(), 6, static_cast<ConnectionType>(0)), res::kBadArgument);
    CHECK_EQ(c.e().ConnectBeginDlp(h.mac(), 6, static_cast<ConnectionType>(3)), res::kBadArgument);
    CHECK(c.e().status() == NetworkStatus::NotConnected);
    c.Leave();
    SentFrames sent(h.con);
    CHECK_EQ(h.e().BeginHostingDlp(2, 6), 0u);
    CHECK_EQ(h.e().BeginHostingDlp(2, 6), res::kWrongState);  // (a host is not idle)
    CHECK_EQ(h.e().ConnectBeginDlp(kFake1, 6, ConnectionType::Client), res::kWrongState);
    u32 ev = 0;
    CHECK_EQ(h.e().Bind(kBindId, 0x2000, kChData, 0xFFFF, &ev), 0u);
    Sleep(350);
    // the beacon: the Download Play's ids, a 52-byte tag (what Azahar's client parses: <= 52), no data, max 2, total 1, channel 6
    constexpr std::size_t kIdAt = offsetof(NetworkInfo, network_id_be) - offsetof(NetworkInfo, oui_value);
    constexpr std::size_t kAttrAt = offsetof(NetworkInfo, attributes_be) - offsetof(NetworkInfo, oui_value);
    constexpr std::size_t kTotalAt = offsetof(NetworkInfo, total_nodes) - offsetof(NetworkInfo, oui_value);
    constexpr std::size_t kMaxAt = offsetof(NetworkInfo, max_nodes) - offsetof(NetworkInfo, oui_value);
    u32 fnv = 2166136261u;
    for (u8 b : h.mac()) {
        fnv = (fnv ^ b) * 16777619u;
    }
    u32 beacons = 0;
    for (const WifiPacket& p : sent.All()) {
        if (p.type != WifiPacket::Type::Beacon) {
            continue;
        }
        ++beacons;
        const BeaconTags tags = FindBeaconTags(p.data);
        CHECK(tags.network != nullptr);
        if (!tags.network) {
            continue;
        }
        CHECK_EQ(tags.network_size, 52u);
        CHECK_EQ(LoadBE32(tags.network + 4), kDlpComm);
        CHECK_EQ(tags.network[8], UdsEngine::kLobbyId8);
        CHECK_EQ(LoadBE32(tags.network + kIdAt), fnv | 1u);
        CHECK_EQ(LoadBE16(tags.network + kAttrAt), 0u);
        CHECK_EQ(tags.network[kTotalAt], 1u);
        CHECK_EQ(tags.network[kMaxAt], 2u);
        const u8* d = nullptr;
        u32 n = 99;
        CHECK(GetBeaconApplicationData(tags, &d, &n));
        CHECK_EQ(n, 0u);
        CHECK_EQ(p.channel, 6u);
    }
    CHECK(beacons >= 2u);
    // a searcher of the real Download Play hears it; it joins as a client: node 2, the host's mask 3
    CHECK(c.e().PrepareLobby());
    c.e().SetLobbyBeaconKind(Kind::RealDlp);
    UdsEngine::HeardLobby heard;
    CHECK(WaitFor(1500, [&] { return HeardHost(c.e(), h.mac(), &heard); }));
    CHECK_EQ(heard.channel, 6u);
    CHECK_EQ(heard.size, 0u);
    CHECK(c.Join(h.mac(), heard.channel));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK_EQ(c.MyNode(), 2u);
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
    // the inbox stays off: a frame on the lobby's channel is a bind's, never the lobby's ring
    CHECK_EQ(h.e().Bind(2, 0x800, UdsEngine::kLobbyChannel, 0xFFFF, &ev), 0u);
    const u8 m[2] = {0x5A, 2};
    CHECK_EQ(c.e().SendTo(1, UdsEngine::kLobbyChannel, 2, 0, m, 2), 0u);
    room.WaitIdle();
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(h.e(), 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 2u);
    CHECK_EQ(src, 2u);
    u8 from = 0, buf[16];
    CHECK(!h.e().PopLobbyMessage(&from, buf, 16, &size));
    // channel 2 both ways
    const u8 hi[2] = {0xA1, 0};
    CHECK_EQ(h.e().SendTo(2, kChData, 2, 0, hi, 2), 0u);
    const u8 lo[2] = {0xB1, 0};
    CHECK_EQ(c.e().SendTo(1, kChData, 2, 0, lo, 2), 0u);
    room.WaitIdle();
    bool right = true;
    CHECK_EQ(Drain(c, 1, 0xA1, &right), 1u);
    CHECK_EQ(Drain(h, 2, 0xB1, &right), 1u);
    CHECK(right);
    // a spectator joins as Azahar's searchers do (ConnectBeginDlp of type Spectator)
    CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
    Sleep(400);
    CHECK_EQ(s.Poll(), 0);  // (the network is full: max 2, the host and the client; a spectator is not answered)
    s.Leave();
    // Shutdown ends the network's rules: the next game on this engine adopts a Logoff of node 0 again (as before 1.1.3)
    h.Leave();
    CHECK(!h.e().initialized());
    CHECK(WaitFor(2000, [&] { return c.e().status() == NetworkStatus::NotConnected; }));
    c.Leave();
    CHECK_RULES();  // (before the host made by hand below: its MAC is no room member, and the game's goodbye goes to it)
    CHECK_EQ(c.e().Initialize(MakeNode(0xC1, "Game", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, kFake1.data(), 6);
    info[6] = 9;
    CHECK_EQ(c.e().ConnectBegin(info, sizeof(info), 1), 0u);
    c.e().OnWifiPacket(Assoc(kFake1, c.mac()));
    std::vector<NodeInfo> nodes(2);
    nodes[0] = MakeNode(0x77, "Fake", 1);
    c.e().OnWifiPacket(Logoff(kFake1, kBroadcastMac, 0, nodes, 2, 1));
    CHECK_EQ(c.e().ConnectPoll(), 1);
    CHECK_EQ(c.e().Shutdown(), 0u);
}

/// D12: spectators are counted, and never nodes.
static void D12_Spectators() {
    std::printf("D12: the spectators of the host\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), s1(room, 2, "Sp1"), s2(room, 3, "Sp2"), c(room, 4, "Cli");
    Ticker ticker(Es({&h, &s1, &s2, &c}));
    CHECK_EQ(h.e().SpectatorCount(), 0u);  // (not even initialised)
    CHECK(h.Host(2, 6));
    CHECK_EQ(h.e().SpectatorCount(), 0u);
    CHECK(s1.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s1.Poll() == 1; }));
    CHECK(s1.e().status() == NetworkStatus::ConnectedAsSpectator);
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK_EQ(h.Mask(), 1u);  // a spectator is no node: GetConnectingClients sees nobody (the netcode of Luigi's Mansion)
    CHECK_EQ(s1.e().SpectatorCount(), 0u);  // (a spectator hosts nothing)
    CHECK(s2.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s2.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 2u; }));
    CHECK_EQ(h.Mask(), 1u);
    s1.Leave();  // one goodbye
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK_EQ(h.Mask(), 1u);
    // a client next to the spectator: node 2, the mask 3, the count still 1
    CHECK(c.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK_EQ(c.MyNode(), 2u);
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
    CHECK_EQ(h.e().SpectatorCount(), 1u);
    c.Leave();
    CHECK(WaitFor(1000, [&] { return h.Mask() == 1u; }));
    CHECK_EQ(h.e().SpectatorCount(), 1u);
    // the room's member list: a spectator that left the room is gone after the debounce
    h.e().OnRoomMembers({h.mac(), s1.mac(), s2.mac(), c.mac()});
    h.e().OnRoomMembers({h.mac(), s1.mac(), c.mac()});
    CHECK(WaitFor(6000, [&] { return h.e().SpectatorCount() == 0u; }));
    h.e().ResetRoomPresence();
    s2.Leave();
    // a broadcast eject takes the spectators with it; the end of the network leaves none
    CHECK(s2.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s2.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK_EQ(h.e().EjectClient(kBroadcastNetworkNodeId), 0u);
    CHECK_EQ(h.e().SpectatorCount(), 0u);
    s2.Leave();
    CHECK(s2.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s2.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK_EQ(h.e().DestroyNetwork(), 0u);
    CHECK_EQ(h.e().SpectatorCount(), 0u);
    s2.Leave();
    h.Leave();
    CHECK_RULES();
}

/// CH: Luigi's Mansion hosts the Download Play on channel 6 and Azahar's client sends on channel 11 (capture 1c): the host takes them.
static void CH_Channels() {
    std::printf("CH: a host on channel 6, a client and a spectator on channel 11\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), c(room, 2, "Cli"), s(room, 3, "Spc");
    Ticker ticker(Es({&h, &c, &s}));
    SentFrames from_c(c.con);
    CHECK(h.Host(4, 6));
    CHECK(s.Join(h.mac(), 11, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s.Poll() == 1; }));
    CHECK(c.Join(h.mac(), 11));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u && h.e().SpectatorCount() == 1u; }));
    const u8 lo[2] = {0xB2, 0};
    CHECK_EQ(c.e().SendTo(1, kChData, 2, 0, lo, 2), 0u);
    room.WaitIdle();
    bool right = true;
    CHECK_EQ(Drain(h, 2, 0xB2, &right), 1u);
    CHECK(right);
    u32 on11 = 0, other = 0;
    for (const WifiPacket& p : from_c.All()) {
        (p.channel == 11 ? on11 : other)++;
    }
    CHECK(on11 >= 3u);  // (Authentication, EAPoL-Start, the data frame: all of them on channel 11)
    CHECK_EQ(other, 0u);
    c.Leave();
    CHECK(WaitFor(1000, [&] { return h.Mask() == 1u; }));
    // the channel given is the one the client sends on (1, not the engine's default 11)
    from_c.Clear();
    CHECK(c.Join(h.mac(), 1));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    u32 on1 = 0, not1 = 0;
    for (const WifiPacket& p : from_c.All()) {
        (p.channel == 1 ? on1 : not1)++;
    }
    CHECK(on1 >= 2u);  // (Authentication, EAPoL-Start)
    CHECK_EQ(not1, 0u);
    c.Leave();
    CHECK(WaitFor(1000, [&] { return h.Mask() == 1u; }));
    s.Leave();
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 0u; }));
    h.Leave();
    CHECK_RULES();
}

/// R7: the searcher's spectator goodbye, its join as a client and its single goodbye (D16c).
static void R7_Goodbyes(u32 gap_ms) {
    std::printf("R7: spectator -> goodbye -> client within %u ms -> goodbye within 3 s\n", gap_ms);
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), g(room, 2, "Guest");
    Ticker ticker(Es({&h, &g}));
    SentFrames sent(g.con);
    CHECK(h.Host(2, 6));
    CHECK(g.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return g.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    Sleep(200);
    g.Leave();
    Sleep(gap_ms);
    CHECK(g.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return g.Poll() == 1; }));  // (the Authentication was answered)
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
    CHECK_EQ(h.e().SpectatorCount(), 0u);
    Sleep(300);
    const int before = DeauthsTo(sent, h.mac());
    g.Leave();  // the client's single goodbye, well within the 3 s of the guard
    room.WaitIdle();
    CHECK_EQ(DeauthsTo(sent, h.mac()) - before, 1);
    CHECK(WaitFor(600, [&] { return h.Mask() == 1u; }));
    std::printf("   the host's mask after the client's single goodbye: 0x%x (1 = right, 3 = a ghost)\n", h.Mask());
    // the other way round: the client leaves, comes back as a spectator at once, and leaves: no ghost spectator
    CHECK(g.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return g.Poll() == 1; }));
    g.Leave();
    Sleep(gap_ms);
    CHECK(g.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return g.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    g.Leave();
    CHECK(WaitFor(600, [&] { return h.e().SpectatorCount() == 0u; }));
    CHECK_EQ(h.Mask(), 1u);
    h.Leave();
    CHECK_RULES();
}

/// R7 control: a GAME's own network keeps the guard of the late second goodbye (Azahar's game guests send two): unchanged.
static void R7_GameUnchanged() {
    std::printf("R7 control: a game's network still swallows the first goodbye after a quick rejoin\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), g(room, 2, "Guest");
    Ticker ticker(Es({&h, &g}));
    u32 ev = 0;
    CHECK_EQ(h.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(2, 0);
    ni.channel = 6;
    CHECK_EQ(h.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    CHECK_EQ(g.e().Initialize(MakeNode(0xA2, "GameGst", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, h.mac().data(), 6);
    info[6] = 6;
    CHECK_EQ(g.e().ConnectBegin(info, sizeof(info), 2), 0u);
    CHECK(WaitFor(3000, [&] { return g.e().ConnectPoll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));  // (counted in a game's network too: nothing reads it there)
    CHECK_EQ(g.e().DisconnectNetwork(), 0u);
    Sleep(100);
    CHECK_EQ(g.e().ConnectBegin(info, sizeof(info), 1), 0u);
    CHECK(WaitFor(3000, [&] { return g.e().ConnectPoll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
    CHECK_EQ(g.e().DisconnectNetwork(), 0u);
    Sleep(600);
    CHECK_EQ(h.Mask(), 3u);  // the guard took the goodbye, as it does since 1.0 for Azahar's two
    CHECK_EQ(g.e().Shutdown(), 0u);
    CHECK_EQ(h.e().Shutdown(), 0u);
}

/// R7b: the Logoffs a client that is Connecting must not take (D16a, D16b), against a host made by hand.
static void R7b_Logoffs() {
    std::printf("R7b: a Connecting client and the Logoff of a spectator's admission\n");
    g_rules.Reset();
    MockRoom room;
    DNode c(room, 3, "Cleo"), s(room, 4, "Spec");
    std::vector<NodeInfo> spec_nodes(2);
    spec_nodes[0] = MakeNode(0x77, "Fake", 1);
    std::vector<NodeInfo> mine = spec_nodes;
    mine[1] = MakeNode(c.seed, "Cleo", 2);
    // the real Download Play's client
    CHECK(c.Join(kFake1, 9));
    c.e().OnWifiPacket(Logoff(kFake1, c.mac(), 2, mine, 2, 2));
    CHECK_EQ(c.Poll(), 0);  // (D16a: the host has not answered the Authentication)
    c.e().OnWifiPacket(Assoc(kFake1, c.mac()));
    c.e().OnWifiPacket(Logoff(kFake1, kBroadcastMac, 0, spec_nodes, 2, 1));
    CHECK_EQ(c.Poll(), 0);  // (D16b: node 0 is a spectator's admission)
    c.e().OnWifiPacket(Logoff(kFake1, c.mac(), 2, mine, 2, 2));
    CHECK_EQ(c.Poll(), 1);
    CHECK_EQ(c.MyNode(), 2u);
    c.Leave();
    // a spectator of the real Download Play takes the Logoff of node 0: it is its own
    CHECK(s.Join(kFake1, 9, ConnectionType::Spectator));
    s.e().OnWifiPacket(Logoff(kFake1, kBroadcastMac, 0, spec_nodes, 2, 1));
    CHECK_EQ(s.Poll(), 0);  // (not before the host's answer)
    s.e().OnWifiPacket(Assoc(kFake1, s.mac()));
    s.e().OnWifiPacket(Logoff(kFake1, kBroadcastMac, 0, spec_nodes, 2, 1));
    CHECK_EQ(s.Poll(), 1);
    CHECK(s.e().status() == NetworkStatus::ConnectedAsSpectator);
    s.Leave();
    // a GAME's client still adopts it, as before 1.1.3 (its rules are not the real Download Play's)
    u32 ev = 0;
    CHECK_EQ(c.e().Initialize(MakeNode(c.seed, "Cleo", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, kFake1.data(), 6);
    info[6] = 9;
    CHECK_EQ(c.e().ConnectBegin(info, sizeof(info), 1), 0u);
    c.e().OnWifiPacket(Assoc(kFake1, c.mac()));
    c.e().OnWifiPacket(Logoff(kFake1, kBroadcastMac, 0, spec_nodes, 2, 1));
    CHECK_EQ(c.e().ConnectPoll(), 1);
    CHECK_EQ(c.e().Shutdown(), 0u);
}

/// R7b with real engines: a spectator and a client that come in together; the client always gets its own node.
static void R7b_Together() {
    std::printf("R7b: a spectator and a client join the host at the same moment (12 rounds)\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), c(room, 2, "Cli"), s(room, 3, "Spc");
    Ticker ticker(Es({&h, &c, &s}));
    CHECK(h.Host(2, 6));
    u32 node0 = 0;
    for (int round = 0; round < 12; ++round) {
        std::thread spec([&] {
            if (round & 1) {
                Sleep(1);
            }
            CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
        });
        CHECK(c.Join(h.mac(), 6));
        spec.join();
        CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
        node0 += c.MyNode() == 0 ? 1u : 0u;
        CHECK_EQ(c.MyNode(), 2u);
        CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
        c.Leave();
        s.Leave();
        CHECK(WaitFor(1000, [&] { return h.Mask() == 1u && h.e().SpectatorCount() == 0u; }));
    }
    std::printf("   rounds in which the client became node 0: %u\n", node0);
    CHECK_EQ(node0, 0u);
    h.Leave();
    CheckRulesButFull("R7b together");
}

/// R11b (T-A of the churn test): join / leave / join / leave within `gap_ms`: the host keeps no ghost.
static void R11b_Ghost(u32 gap_ms) {
    std::printf("R11b T-A: join / leave / join / leave, each within %u ms\n", gap_ms);
    g_rules.Reset();
    MockRoom room;
    DNode a(room, 1, "Alice"), b(room, 2, "Bob");
    Ticker ticker(Es({&a, &b}));
    CHECK(a.Host(4, 6));
    CHECK(b.Join(a.mac(), 6));
    CHECK(WaitFor(3000, [&] { return b.Poll() == 1; }));
    b.Leave();
    Sleep(gap_ms);
    CHECK(b.Join(a.mac(), 6));
    CHECK(WaitFor(3000, [&] { return b.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return a.Mask() == 3u; }));
    Sleep(gap_ms);
    b.Leave();
    Sleep(600);
    std::printf("   the creator's node mask 600 ms after the last leave: 0x%x (1 = right, 3 = a ghost of Bob)\n", a.Mask());
    CHECK_EQ(a.Mask(), 1u);
    a.Leave();
    CHECK_RULES();
}

/// R11b (R of the churn test): a silent loss, the rejoin, and another client's departure in between.
static void R11b_Race() {
    std::printf("R11b R: a silent loss, the rejoin, and another client's departure in between\n");
    g_rules.Reset();
    MockRoom room;
    DNode a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo");
    Ticker ticker(Es({&a, &b, &c}));
    CHECK(a.Host(4, 6));
    CHECK(b.Join(a.mac(), 6));
    CHECK(WaitFor(3000, [&] { return b.Poll() == 1; }));
    CHECK(c.Join(a.mac(), 6));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK(WaitFor(2000, [&] { return a.Mask() == 7u; }));
    CHECK(b.e().OnTransportDown(NetworkStatusChangeReason::ConnectionLost));
    CHECK(b.e().status() == NetworkStatus::NotConnected);
    CHECK_EQ(a.Mask(), 7u);
    b.con.sniff = [](const WifiPacket&) { Sleep(30); };
    std::thread leaver([&] {
        Sleep(8);
        c.Leave();
    });
    CHECK_EQ(b.e().ConnectBeginDlp(a.mac(), 6, ConnectionType::Client), 0u);  // (the engine is still the lobby's: no new Initialize)
    leaver.join();
    b.con.sniff = nullptr;
    Sleep(1500);
    const bool ghost = b.IsClient() && (a.Mask() & (1u << (b.MyNode() ? b.MyNode() - 1 : 0))) == 0;
    std::printf("   Bob: %s node %u, the creator's mask 0x%x\n", b.IsClient() ? "client" : "not a client", b.MyNode(), a.Mask());
    CHECK(!ghost);
    CHECK(!b.IsClient() || b.MyNode() != 0);
    b.Leave();
    a.Leave();
}

/// R11b (Y11 of the extra test): the creator's goodbye is a burst of Deauthentications and the client is out at once.
static void R11b_Goodbye() {
    std::printf("R11b Y11: the creator's goodbye\n");
    g_rules.Reset();
    MockRoom room;
    DNode a(room, 2, "Alice"), b(room, 1, "Bob");
    Ticker ticker(Es({&a, &b}));
    u32 ev = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0xAA, "Old", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(a.e().SetDeauthOnDestroy(false), 0u);  // (the game before turned it off: the next user starts with it on)
    CHECK_EQ(a.e().Shutdown(), 0u);
    CHECK(a.Host(2, 9));
    CHECK(b.Join(a.mac(), 9));
    CHECK(WaitFor(3000, [&] { return b.Poll() == 1; }));
    SentFrames sent(a.con);
    a.Leave();
    room.WaitIdle();
    CHECK(sent.Count(WifiPacket::Type::Deauthentication) >= 1);
    CHECK(WaitFor(2000, [&] { return b.e().status() == NetworkStatus::NotConnected; }));
    b.Leave();
    CHECK_RULES();
}

/// R11b (Y9 of the extra test): a Logoff and a node map of ANOTHER network, while Connecting and while connected.
static void R11b_Foreign() {
    std::printf("R11b Y9: a Logoff and a node map of another network\n");
    g_rules.Reset();
    MockRoom room;
    DNode c(room, 3, "Cleo"), s(room, 9, "Strg");
    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(0x77, "Fake", 1);
    nodes[1] = MakeNode(c.seed, "Cleo", 2);
    CHECK(c.Join(kFake1, 9));
    c.e().OnWifiPacket(Logoff(s.mac(), c.mac(), 2, nodes, 4, 2));
    CHECK_EQ(c.Poll(), 0);
    c.e().OnWifiPacket(Assoc(kFake1, c.mac()));
    c.e().OnWifiPacket(Logoff(s.mac(), c.mac(), 2, nodes, 4, 2));  // (a stranger's, refused by its transmitter)
    CHECK_EQ(c.Poll(), 0);
    c.e().OnWifiPacket(Logoff(kFake1, c.mac(), 2, nodes, 4, 2));
    CHECK_EQ(c.Poll(), 1);
    SentFrames sent(c.con);
    WifiPacket map;
    map.type = WifiPacket::Type::NodeMap;
    map.transmitter_address = s.mac();
    map.destination_address = kBroadcastMac;
    map.channel = 9;
    map.data = GenerateNodeMapPacket({NodeMapEntry{s.mac(), 3}});
    c.e().OnWifiPacket(map);
    CHECK_EQ(c.e().SendTo(3, kChData, 2, 0, kMsg, 2), 0u);  // (node 3 is not in her network: nothing goes to the stranger)
    room.WaitIdle();
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Data, s.mac()), 0);
    // a data frame of the stranger for her node does not reach her bind (the filter is on in the real Download Play's session)
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.transmitter_address = s.mac();
    p.destination_address = kBroadcastMac;
    p.channel = 9;
    const u8 body[2] = {0xEE, 0};
    p.data = GenerateDataPayload(body, 2, kChData, 2, 1, 0xC001);
    c.e().OnWifiPacket(p);
    CHECK_EQ(Drain(c, 1, 0, nullptr), 0u);
    p.transmitter_address = kFake1;  // the same frame from the host is hers
    p.data = GenerateDataPayload(body, 2, kChData, 2, 1, 0xC002);
    c.e().OnWifiPacket(p);
    CHECK_EQ(Drain(c, 1, 0, nullptr), 1u);
    c.Leave();
}

/// R11b (Y17 of the extra test): a node map heard by an idle engine does not outlive its session; a spectator of the real
/// Download Play takes no stranger's frame.
static void R11b_StaleNodeMap() {
    std::printf("R11b Y17: a stale node map, a spectator and a stranger\n");
    g_rules.Reset();
    MockRoom room;
    DNode a(room, 1, "Alice"), s(room, 2, "Spec"), t(room, 9, "Strg");
    Ticker ticker(Es({&a, &s, &t}));
    u32 ev = 0;
    CHECK_EQ(s.e().Initialize(MakeNode(0x51, "Old", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    WifiPacket map;
    map.type = WifiPacket::Type::NodeMap;
    map.transmitter_address = t.mac();
    map.destination_address = kBroadcastMac;
    map.channel = 9;
    map.data = GenerateNodeMapPacket({NodeMapEntry{t.mac(), 3}});
    s.e().OnWifiPacket(map);
    CHECK_EQ(s.e().Shutdown(), 0u);
    CHECK(a.Host(4, 9));
    CHECK(s.Join(a.mac(), 9, ConnectionType::Spectator));
    CHECK(WaitFor(4000, [&] { return s.Poll() == 1; }));
    CHECK_EQ(s.e().Bind(2, 0x2000, 5, 0xFFFF, &ev), 0u);
    const u8 body[4] = {1, 2, 3, 4};
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.transmitter_address = t.mac();
    p.destination_address = kBroadcastMac;
    p.channel = 9;
    p.data = GenerateDataPayload(body, 4, 5, kBroadcastNetworkNodeId, 3, 0xC001);
    s.e().OnWifiPacket(p);
    std::vector<u8> pulled;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(s.e(), 2, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    // the host's own broadcast on that channel does reach the spectator (as the announcement will on channel 1)
    CHECK_EQ(a.e().SendTo(kBroadcastNetworkNodeId, 5, 4, 0, body, 4), 0u);
    room.WaitIdle();
    CHECK_EQ(PullInto(s.e(), 2, &pulled, &size, &src), 0u);
    CHECK_EQ(size, 4u);
    CHECK_EQ(src, 1u);
    s.Leave();
    a.Leave();
    CHECK_RULES();
}

static u32 g_diverged = 0;

/// The churn fuzz in the real Download Play's network: random joins, leaves, kicks and bounces of three consoles on one host.
/// `max_nodes` 2 is Luigi's Mansion (one client): every client must show the host's roster and hear exactly the right message.
/// With 4 the host's side is held (no ghost, no node 0, one node per console): the clients that stay do not learn of a fellow
/// that left (D17, a condition for the games of 3-4 players, not 1.1.3's).
static void Fuzz(u32 seed, int steps, u8 max_nodes) {
    std::printf("R11b fuzz seed %u, max %u nodes\n", seed, max_nodes);
    g_rules.Reset();
    std::mt19937 rng(seed);
    MockRoom room;
    DNode a(room, 1, "Alice"), b(room, 2, "Bob"), c(room, 3, "Cleo"), d(room, 4, "Dave");
    DNode* cs[3] = {&b, &c, &d};
    Ticker ticker(Es({&a, &b, &c, &d}));
    CHECK(a.Host(max_nodes, 6));
    for (int step = 0; step < steps; ++step) {
        const int ops = 1 + static_cast<int>(rng() % 3);
        std::string log;
        for (int k = 0; k < ops; ++k) {
            const int who = static_cast<int>(rng() % 3);
            DNode& p = *cs[who];
            const u32 what = rng() % 6;
            char t[48];
            if (what <= 1) {
                if (!p.IsClient() && p.e().status() != NetworkStatus::Connecting) {
                    const bool ok = p.Join(a.mac(), 6);
                    std::snprintf(t, sizeof(t), " join(%d)=%d", who, ok);
                    log += t;
                }
            } else if (what == 2) {
                p.Leave();
                std::snprintf(t, sizeof(t), " leave(%d)", who);
                log += t;
            } else if (what == 3) {
                a.e().EjectClient(static_cast<u16>(2 + rng() % 3));
                log += " kick";
            } else if (what == 4) {
                p.Leave();
                const bool ok = p.Join(a.mac(), 6);
                std::snprintf(t, sizeof(t), " bounce(%d)=%d", who, ok);
                log += t;
            } else {
                Sleep(rng() % 30);
            }
        }
        Sleep(700);
        std::string why;
        const auto check = [&]() -> bool {
            u16 hn = 0, hm = 0;
            if (!a.e().NodeSnapshot(&hn, &hm)) {
                why = "host not in a network";
                return false;
            }
            int clients = 0;
            u16 seen = 1;
            for (int i = 0; i < 3; ++i) {
                DNode& n = *cs[i];
                if (!n.IsClient()) {
                    continue;
                }
                ++clients;
                const u16 id = n.MyNode();
                char buf[160];
                if (id < 2 || id > 4 || !(hm & (1u << (id - 1))) || (seen & (1u << (id - 1)))) {
                    std::snprintf(buf, sizeof(buf), "client %d has node %u, host mask 0x%x", i, id, hm);
                    why = buf;
                    return false;
                }
                seen |= static_cast<u16>(1u << (id - 1));
                if (max_nodes == 2) {
                    NodeInfo x, y;
                    if (n.Mask() != hm || a.e().GetNodeInformation(id, &x) != 0 || n.e().GetNodeInformation(id, &y) != 0 ||
                        x.friend_code_seed != y.friend_code_seed) {
                        std::snprintf(buf, sizeof(buf), "client %d: mask 0x%x or identity differs from the host's 0x%x", i, n.Mask(), hm);
                        why = buf;
                        return false;
                    }
                }
            }
            if (__builtin_popcount(hm) != 1 + clients) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "host mask 0x%x counts %d, clients really connected: %d", hm, __builtin_popcount(hm), clients);
                why = buf;
                return false;
            }
            return true;
        };
        bool ok = check();
        if (!ok) {
            Sleep(1500);
            ok = check();
        }
        if (!ok) {
            ++g_diverged;
            std::printf("  step %d:%s -> DIVERGED: %s\n", step, log.c_str(), why.c_str());
            continue;
        }
        if (a.e().SpectatorCount() != 0) {
            ++g_diverged;
            std::printf("  step %d:%s -> the host counts %u spectators\n", step, log.c_str(), a.e().SpectatorCount());
        }
        // messages on channel 2: drain, then each side speaks once; each hears exactly the right one from the right node
        for (DNode* n : cs) {
            if (n->e().initialized()) {
                Drain(*n, 0, 0, nullptr);
            }
        }
        Drain(a, 0, 0, nullptr);
        int expect = 0;
        for (int i = 0; i < 3; ++i) {
            if (!cs[i]->IsClient()) {
                continue;
            }
            ++expect;
            const u16 node = cs[i]->MyNode();
            const u8 to_client[2] = {static_cast<u8>(0xA0 + step), static_cast<u8>(node)};
            const u8 to_host[2] = {static_cast<u8>(0xB0 + step), static_cast<u8>(node)};
            a.e().SendTo(node, kChData, 2, 0, to_client, 2);
            cs[i]->e().SendTo(1, kChData, 2, 0, to_host, 2);
        }
        Sleep(150);
        for (int i = 0; i < 3; ++i) {
            if (!cs[i]->IsClient()) {
                continue;
            }
            bool right = true;
            const u32 got = Drain(*cs[i], 1, static_cast<u8>(0xA0 + step), &right);
            if (got != 1 || !right) {
                ++g_diverged;
                std::printf("  step %d:%s -> client %d (node %u) heard %u frame(s), right=%d\n", step, log.c_str(), i, cs[i]->MyNode(), got, right);
            }
        }
        u32 heard = 0;
        for (;;) {
            std::vector<u8> out;
            u32 size = 0;
            u16 src = 0;
            if (PullInto(a.e(), kBindId, &out, &size, &src) != 0 || size == 0) {
                break;
            }
            ++heard;
            if (size != 2 || out[0] != 0xB0 + step || out[1] != src) {
                ++g_diverged;
                std::printf("  step %d:%s -> the host heard a wrong frame from node %u\n", step, log.c_str(), src);
            }
        }
        if (static_cast<int>(heard) != expect) {
            ++g_diverged;
            std::printf("  step %d:%s -> the host heard %u frames, expected %d\n", step, log.c_str(), heard, expect);
        }
    }
    for (DNode* n : cs) {
        n->Leave();
    }
    a.Leave();
    if (max_nodes == 2) {
        CheckRulesButFull("fuzz, max 2 nodes");  // (three consoles for one place)
    } else {
        CHECK_RULES();
    }
}

/// R13 (the client's half): the two dialects do not see each other.
static void R13_Dialects() {
    std::printf("R13: a lobby of the dialect and a real Download Play's host do not see each other\n");
    g_rules.Reset();
    MockRoom room;
    Player lh(room, 1, "LobbyH"), ls(room, 2, "LobbyS");
    DNode dh(room, 3, "RealH"), ds(room, 4, "RealS");
    Ticker ticker({&lh.e(), &ls.e(), &dh.e(), &ds.e()});
    CHECK(lh.net.HostOpen(Ann(), 6, Me(0xF001, "LobbyH")));
    CHECK(dh.Host(2, 6));
    CHECK(ls.net.ScanOpen());  // (the dialect's searcher: kind Ours)
    CHECK(ds.e().PrepareLobby());
    ds.e().SetLobbyBeaconKind(Kind::RealDlp);
    CHECK(WaitFor(2000, [&] { return HeardHost(ls.e(), lh.mac()) && HeardHost(ds.e(), dh.mac()); }));
    Sleep(500);
    CHECK(!HeardHost(ls.e(), dh.mac()));
    CHECK(!HeardHost(ds.e(), lh.mac()));
    // the dialect's searcher cannot join the real Download Play's host (its table does not have it)
    CHECK(!ls.net.ConnectBegin(dh.mac(), Me(0xF002, "LobbyS")));
    CHECK(ls.e().status() != NetworkStatus::Connecting);
    ls.net.Disconnect();
    lh.net.HostClose();
    dh.Leave();
    CHECK_RULES();
}

/// Counts the EAPoL-Logoffs that reach a console (its Console::tap).
static std::function<void(const WifiPacket&)> CountLogoffs(std::atomic<int>* n) {
    return [n](const WifiPacket& p) {
        EtherType et;
        u16 magic = 0;
        if (p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &et) && et == EtherType::EAPoL &&
            GetEapolFrameType(p.data, &magic) && magic == kEapolLogoffMagic) {
            ++*n;
        }
    };
}

/// RV1 (review of 1.1.3, protocol point 1): the Logoff of an admission in the real Download Play's network stays in the network. Azahar's
/// nwm adopts ANY Logoff while it is Connecting (nwm_uds.cpp:277-317 at 0ab6399c: no look at sender, association or id): a bystander that
/// is joining - here its AssociationResponse is lost, so it stays Connecting and half-open on the host, Azahar's window - must get none.
/// A spectator's goes to the spectator alone, a client's to every member (the node lists of the clients in stay whole). A game's network
/// broadcasts as before (control).
static void RV1_LogoffsStayInTheNetwork() {
    std::printf("RV1: the Logoffs of the real Download Play's admissions reach its members only\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), by(room, 2, "Bystd"), s(room, 3, "Spec"), c(room, 4, "Cli"), c2(room, 5, "Cli2");
    std::atomic<int> to_by{0}, to_s{0}, to_c{0};
    by.con.drop = [](const WifiPacket& p) { return p.type == WifiPacket::Type::AssociationResponse; };
    by.con.tap = CountLogoffs(&to_by);
    s.con.tap = CountLogoffs(&to_s);
    c.con.tap = CountLogoffs(&to_c);
    Ticker ticker(Es({&h, &by, &s, &c, &c2}));
    CHECK(h.Host(3, 6));
    CHECK(by.Join(h.mac(), 6));
    Sleep(200);
    CHECK(by.e().status() == NetworkStatus::Connecting);
    CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK(c.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK_EQ(c.MyNode(), 2u);
    room.WaitIdle();
    const int s_after_c = to_s.load();
    CHECK(c2.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return c2.Poll() == 1; }));
    CHECK_EQ(c2.MyNode(), 3u);
    CHECK(WaitFor(1000, [&] { return c.Mask() == 7u; }));  // (the client in learnt the newcomer from its Logoff)
    room.WaitIdle();
    std::printf("   Logoffs: bystander %d, spectator %d, first client %d\n", to_by.load(), to_s.load(), to_c.load());
    CHECK_EQ(to_by.load(), 0);  // (none: an Azahar console joining would have taken the spectator's as its own node 0)
    CHECK_EQ(s_after_c, 2);     // (its own, then the first client's)
    CHECK_EQ(to_s.load(), 3);
    CHECK_EQ(to_c.load(), 2);   // (its own, then the second client's)
    by.Leave();
    c2.Leave();
    c.Leave();
    s.Leave();
    h.Leave();
    // control: a game's network broadcasts the Logoff of an admission, as since 1.0
    std::atomic<int> to_by2{0};
    DNode gh(room, 6, "GHost"), gg(room, 7, "GGst"), by2(room, 8, "Bys2");
    by2.con.tap = CountLogoffs(&to_by2);
    Ticker ticker2(Es({&gh, &gg, &by2}));
    u32 ev = 0;
    CHECK_EQ(gh.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(2, 0);
    ni.channel = 6;
    CHECK_EQ(gh.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    CHECK_EQ(gg.e().Initialize(MakeNode(0xA2, "GameGst", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, gh.mac().data(), 6);
    info[6] = 6;
    CHECK_EQ(gg.e().ConnectBegin(info, sizeof(info), 1), 0u);
    CHECK(WaitFor(3000, [&] { return gg.e().ConnectPoll() == 1; }));
    room.WaitIdle();
    CHECK_EQ(to_by2.load(), 1);
    CHECK_EQ(gg.e().Shutdown(), 0u);
    CHECK_EQ(gh.e().Shutdown(), 0u);
}

/// RV2 (review of 1.1.3, protocol point 2): the real Download Play's host that no longer takes clients (UpdateNetworkAttribute 0x2, its
/// StartDistribution) still takes a SPECTATOR, as Azahar's host does (its UpdateNetworkAttribute is a stub): an Azahar searcher refused
/// would wait 3 s inside its emulation and ask again ~100 ms later, for as long as the host beacons. A client stays out; the bits that close
/// the door to spectators (EjectSpectators) or to everyone (0x4) keep the spectators out too; a game's closed network takes nobody (control).
static void RV2_ClosedHostTakesSpectators() {
    std::printf("RV2: a closed host of the real Download Play still takes spectators, never a client\n");
    g_rules.Reset();
    MockRoom room;
    DNode h(room, 1, "Host"), s(room, 2, "Spec"), c(room, 3, "Cli");
    Ticker ticker(Es({&h, &s, &c}));
    CHECK(h.Host(2, 6));
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x2, 1), 0u);
    CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
    CHECK(WaitFor(3000, [&] { return s.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 1u; }));
    CHECK(c.Join(h.mac(), 6));
    Sleep(600);
    CHECK(c.Poll() != 1);
    CHECK_EQ(h.Mask(), 1u);
    c.Leave();
    s.Leave();
    CHECK(WaitFor(1000, [&] { return h.e().SpectatorCount() == 0u; }));
    // closed to everyone: no spectator either
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x4, 1), 0u);
    CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
    Sleep(600);
    CHECK(s.Poll() != 1);
    CHECK_EQ(h.e().SpectatorCount(), 0u);
    s.Leave();
    // closed to spectators (EjectSpectators), clients still refused
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x4, 0), 0u);
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x2, 1), 0u);
    CHECK_EQ(h.e().EjectSpectators(), 0u);
    CHECK(s.Join(h.mac(), 6, ConnectionType::Spectator));
    Sleep(600);
    CHECK(s.Poll() != 1);
    s.Leave();
    // open again: the client comes in
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x3, 0), 0u);
    CHECK(c.Join(h.mac(), 6));
    CHECK(WaitFor(3000, [&] { return c.Poll() == 1; }));
    CHECK(WaitFor(1000, [&] { return h.Mask() == 3u; }));
    // closed again, the member authenticates again (a dropped link): its association stays while the host is closed
    CHECK_EQ(h.e().UpdateNetworkAttribute(0x2, 1), 0u);
    WifiPacket reauth;
    reauth.type = WifiPacket::Type::Authentication;
    reauth.transmitter_address = c.mac();
    reauth.destination_address = h.mac();
    reauth.channel = 6;
    reauth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
    h.e().OnWifiPacket(reauth);
    Sleep(100);
    CHECK_EQ(h.Mask(), 3u);
    c.Leave();
    h.Leave();
    // control: a game's closed network takes no spectator (as before 1.1.3)
    DNode gh(room, 6, "GHost"), gs(room, 7, "GSpec");
    Ticker ticker2(Es({&gh, &gs}));
    u32 ev = 0;
    CHECK_EQ(gh.e().Initialize(MakeNode(0xA1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(2, 0);
    ni.channel = 6;
    CHECK_EQ(gh.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    CHECK_EQ(gh.e().UpdateNetworkAttribute(0x2, 1), 0u);
    CHECK_EQ(gs.e().Initialize(MakeNode(0xA2, "GameSpc", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, gh.mac().data(), 6);
    info[6] = 6;
    CHECK_EQ(gs.e().ConnectBegin(info, sizeof(info), 2), 0u);
    Sleep(600);
    CHECK(gs.e().ConnectPoll() != 1);
    CHECK_EQ(gh.e().SpectatorCount(), 0u);
    CHECK_EQ(gs.e().Shutdown(), 0u);
    CHECK_EQ(gh.e().Shutdown(), 0u);
}

int main() {
    std::printf("dlp_real_engine_test: the engine's half of the real Download Play\n");
    E1_Kinds();
    D11_Calls();
    D12_Spectators();
    CH_Channels();
    R7_Goodbyes(100);
    R7_Goodbyes(2000);
    R7_GameUnchanged();
    R7b_Logoffs();
    R7b_Together();
    R11b_Ghost(500);
    R11b_Ghost(1500);
    R11b_Race();
    R11b_Goodbye();
    R11b_Foreign();
    R11b_StaleNodeMap();
    for (u32 seed : {1u, 2u, 3u}) {
        Fuzz(seed, 22, 2);
    }
    for (u32 seed : {4u, 5u}) {
        Fuzz(seed, 22, 4);
    }
    R13_Dialects();
    RV1_LogoffsStayInTheNetwork();
    RV2_ClosedHostTakesSpectators();
    CHECK_EQ(g_diverged, 0u);
    // (rig helpers this binary has no use for)
    (void)&Join;
    (void)&FindGroup;
    (void)&OpenLobby;
    (void)&CliNode;
    (void)&CliState;
    (void)&Str;
    (void)&LobbyFrame;
    (void)&EngineClean;
    (void)&Settle;
    (void)&Engines;
    std::printf("diverged steps: %u\n%d checks, %d failures\n", g_diverged, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
