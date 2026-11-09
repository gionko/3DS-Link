// The HOST of the real Download Play (1.1.3, phase F3 of analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md): our DlpLobby on
// the adapter RealDlpNet (azl/dlp_real_net.hpp), in front of a model of Azahar's Download Play client (Tests/azahar_dlp_peer.hpp,
// Azahar's dlp_clt_base.cpp transcribed). Same rig as Tests/dlp_real_engine_test.cpp (engines joined by the in-memory room of
// Tests/mock_room.hpp, real time, the Azahar-peer rule checker on every frame our engines send); the host's game is Luigi's Mansion
// (2018) as the console log shows it: one client, manual acceptance, channel 6, AcceptClient when the client is in, StartDistribution,
// then SendWirelessRebootPassphrase every 33 ms with a NEW passphrase while the state is 8 (NETCODE.md, point 2).
//
// Scenarios (design 7.3):
//   R1   the whole sequence with a client at RTT 0: the model reads the 5 frames within 1000 ms of its bind; GetConnectingClients stays 0
//        while it is a spectator and after its goodbye (NETCODE point 3); the Auth before the acceptance (point 5); the client 7 at the
//        ack of StartDistribution, 9 with the FIRST passphrase; the lobby 9 within 2000 ms of the BeginGame, with GetConnectingClients 0
//        (point 6) and the engine given back; StartDistribution -> final within 600 ms; no violation of Azahar's rules
//   R1b  no frame of channel 1 without a spectator; a group at once when one comes (5 frames, their lengths, ONE per Pump), then one
//        every 500 ms; none once it has left, none while the host no longer accepts
//   R4   the same sequence at RTT 100 / 300 / 500 (it passes) and 700 (the outcome and the START -> START_ACK time printed: the
//        threshold is annotated, the lobby's graces are not touched)
//   R5   losses: a frame 1..4 of a group (the probe closes with the next group); the first A3 (resent at 400 ms); the final with the
//        notices of its first 500 ms (resent: the client gets it); every final (sent 4 times, the lobby ends at its 2000 ms)
//   R6b  a third console in a game's session with binds of channel 1: what it receives of the announcement with and without a spectator
//   R7   spectator -> goodbye -> client within 100 ms and within 2 s (same MAC) -> goodbye within 3 s: the Auth is answered, the client's
//        only goodbye is not swallowed (D16c), no ghost in GetConnectingClients, the dialogue starts again from the Auth
//   R9   malformed packets from a client (short, size field wrong, unknown type, a host's packet, too long for the buffer, a notice ack
//        without its 9, an ack in a state that does not take it): counted, never acted on, the bind never stuck, the sequence goes on;
//        a client that asks for the content is ejected
//   R12  a full network: a second Azahar searcher gets no answer (timeout after 3 s), no announcement, the game goes on
//   R14  the lobby's START and GO repeated every 200 ms, and the game's BeginGame every 33 ms with new passphrases: A3 does not start
//        again, the finals are not counted again, the passphrase is the first; a GO to a client that has not finished: no final until it has
//   +    the game-thread calls of the adapter (Send, Receive, Members, the clock) allocate nothing; a node that leaves frees its dialogue
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_net_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o <the core sources of Tests/run.sh>
#include "dlp_net_rig.hpp"

#include "azahar_dlp_peer.hpp"
#include "azl/dlp_real_net.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kLuigiLow = 0x001D1A00;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;
static constexpr u32 kInvalid = 0xE0A0AC01;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// The host: a console, the adapter, the lobby on it.
struct RHost {
    RHost(MockRoom& room, u8 last, const char* name) : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(kLuigiLow, last, name)) {}
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    /// The network thread's turn: the adapter first, then the lobby (as the hub will do it, design D10).
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    u32 Total() {
        u16 ids[4];
        u32 total = 0xFF;
        lobby.ServerConnectingClients(ids, 4, &total);
        return total;
    }
    u32 ClientExt(u16 node) {
        u32 w = 0;
        return lobby.ServerClientState(node, &w) == 0 ? (w >> 24) : 0xFF;
    }
    Console con;
    RealDlpNet net;
    DlpLobby lobby;
};

/// What the host put on the wire, as DLP packets (the payload of its SecureData frames).
class Wire {
public:
    struct Sent {
        u64 ms;
        u8 channel;
        u16 dest;
        std::vector<u8> p;
    };
    explicit Wire(Console& c) {
        c.sniff = [this](const WifiPacket& w) {
            Sent s;
            if (!Decode(w, &s)) {
                return;
            }
            std::lock_guard<std::mutex> l(m_);
            v_.push_back(std::move(s));
        };
    }
    static bool Decode(const WifiPacket& w, Sent* s) {
        EtherType t;
        SecureDataHeader h;
        if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData ||
            !ParseSecureDataHeader(w.data, &h) || h.ActualDataSize() > w.data.size()) {
            return false;
        }
        s->ms = platform::NowMs();
        s->channel = h.data_channel;
        s->dest = h.dest_node_id;
        s->p.assign(w.data.end() - h.ActualDataSize(), w.data.end());
        return true;
    }
    /// Packets of `channel` (and of `type` / `idx` if given).
    u32 Count(u8 channel, int type = -1, int idx = -1) {
        std::lock_guard<std::mutex> l(m_);
        u32 n = 0;
        for (const Sent& s : v_) {
            n += s.channel == channel && (type < 0 || (s.p.size() > 12 && s.p[0] == type)) && (idx < 0 || (s.p.size() > 12 && s.p[12] == idx));
        }
        return n;
    }
    std::vector<Sent> All() {
        std::lock_guard<std::mutex> l(m_);
        return v_;
    }
    void Clear() {
        std::lock_guard<std::mutex> l(m_);
        v_.clear();
    }

private:
    std::mutex m_;
    std::vector<Sent> v_;
};

/// The DLP packet a frame carries, for the drop filters: channel, type, index (false = no DLP packet).
static bool DlpOf(const WifiPacket& w, u8* channel, u8* type, u8* idx) {
    Wire::Sent s;
    if (!Wire::Decode(w, &s) || s.p.size() < 16) {
        return false;
    }
    *channel = s.channel;
    *type = s.p[0];
    *idx = s.p[12];
    return true;
}

template <typename F>
static bool PumpUntil(RHost& h, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        h.Pump();
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(2);
    }
}
/// Runs `f` (a call of the model that blocks) while the host is pumped.
template <typename F>
static void Pumping(RHost& h, F f, u32* max_total = nullptr) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        f();
        done = true;
    });
    while (!done) {
        h.Pump();
        if (max_total) {
            const u32 total = h.Total();
            *max_total = total > *max_total ? total : *max_total;
        }
        Sleep(2);
    }
    t.join();
}

/// Luigi's host up to the state 2 (one client, manual acceptance, channel 6).
static void OpenLuigi(RHost& h, u32 max_clients = 1) {
    CHECK_EQ(h.lobby.ServerInitialize(max_clients, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(h.net.Holds());
}

static void Pass(u8 k, u8 out[9]) {
    static const char* hex = "0123456789abcdef";
    for (u32 i = 0; i < 8; ++i) {
        out[i] = static_cast<u8>(hex[(k * 7 + i * 3) & 15]);
    }
    out[8] = 0;
}

/// The model's probe of the host, the host pumped meanwhile; the most GetConnectingClients said during it.
static AzaharDlpClient::Probe ProbeOf(RHost& h, AzaharDlpClient& m, u32* max_total) {
    CHECK(PumpUntil(h, 3000, [&] { return m.Heard(h.mac()); }));
    AzaharDlpClient::Probe pr;
    Pumping(h, [&] { pr = m.ProbeHost(h.mac()); }, max_total);
    return pr;
}

struct Outcome {
    bool authed_before_accept = false;
    bool start_acked = false;
    u32 start_to_ack_ms = 0;  // StartDistribution -> START_ACK in the lobby
    u32 sd_to_final_ms = 0;   // StartDistribution -> the client took the final
    u32 begin_to_9_ms = 0;    // the first BeginGame -> the lobby's 9
    u32 begin_results_ok = 0, begin_results_invalid = 0;
    u32 client_at_ack = 0;     // the client's state when the lobby got the START_ACK (7: it took the StartDistribution)
};

/// Luigi's sequence after the client's join: AcceptClient, StartDistribution, BeginGame every 33 ms while 8. First passphrase = Pass(1).
static Outcome RunGame(RHost& h, AzaharDlpClient& m) {
    Outcome o;
    o.authed_before_accept = PumpUntil(h, 4000, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; });
    CHECK_EQ(h.ClientExt(2), static_cast<u32>(kPeerWaiting));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    const u64 t_sd = platform::NowMs();
    o.start_acked = PumpUntil(h, 3000, [&] {
        return h.ClientExt(2) == kPeerDistributed || h.lobby.server_state() != kSrvDistributing;
    }) && h.ClientExt(2) == kPeerDistributed;
    o.start_to_ack_ms = static_cast<u32>(platform::NowMs() - t_sd);
    o.client_at_ack = m.state();
    CHECK(PumpUntil(h, 3000, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
    // the game: BeginGame every 33 ms with a new passphrase while the state is 8
    const u64 t_begin = platform::NowMs();
    u64 next = t_begin;
    for (u8 k = 1; h.lobby.server_state() == kSrvPassphrase && platform::NowMs() - t_begin < 4000;) {
        if (platform::NowMs() >= next) {
            u8 nine[9];
            Pass(k++, nine);
            const u32 r = h.lobby.ServerSendPassphrase(nine);
            o.begin_results_ok += r == 0;
            o.begin_results_invalid += r == kInvalid;
            next += 33;
        }
        h.Pump();
        Sleep(2);
    }
    o.begin_to_9_ms = static_cast<u32>(platform::NowMs() - t_begin);
    const u64 fin = m.first_ms(6, 1);
    o.sd_to_final_ms = fin >= t_sd ? static_cast<u32>(fin - t_sd) : 0xFFFFFFFFu;
    return o;
}

static void CheckModel(AzaharDlpClient& m) {
    for (const std::string& v : m.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(m.violations().empty());
    CHECK(!m.session_broken);
}

// ---------------------------------------------------------------------------------------------------------------------

static void R1_Sequence(u32 rtt, bool must_pass) {
    std::printf("R1/R4: Luigi's host and an Azahar client, RTT %u ms\n", rtt);
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    m.rtt_ms = rtt;
    Wire wire(h.con);
    Ticker ticker({&h.e(), &m.e()});
    OpenLuigi(h);
    u32 max_total = 0;
    const AzaharDlpClient::Probe pr = ProbeOf(h, m, &max_total);
    CHECK(pr.connected);
    CHECK(pr.all);
    CHECK(!pr.ignored);
    CHECK(pr.ms_to_all <= 1000);
    CHECK_EQ(pr.child_tid, kLuigiChild);
    CHECK_EQ(pr.max_nodes, 2u);
    CHECK_EQ(Str(pr.name), std::string("Host"));
    CHECK_EQ(max_total, 0u);  // (a spectator is never in GetConnectingClients: NETCODE point 3)
    CHECK(PumpUntil(h, 1000, [&] { return h.e().SpectatorCount() == 0; }));
    CHECK_EQ(h.Total(), 0u);
    std::printf("   probe: 5/5 in %u ms from the bind, %u frames taken\n", pr.ms_to_all, m.probe_frames.load());
    // the announcement stops with the spectator
    const u32 ann = h.net.counters().announce;
    PumpUntil(h, 700, [] { return false; });
    CHECK_EQ(h.net.counters().announce, ann);
    // the session
    bool joined = false;
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    CHECK_EQ(m.state(), AzaharDlpClient::kJoined);
    CHECK(PumpUntil(h, 2000, [&] { return h.Total() == 1; }));
    const Outcome o = RunGame(h, m);
    std::printf("   START -> START_ACK %u ms, StartDistribution -> final %u ms, BeginGame -> 9 %u ms; BeginGame answered 0 x%u, invalid x%u\n",
                o.start_to_ack_ms, o.sd_to_final_ms, o.begin_to_9_ms, o.begin_results_ok, o.begin_results_invalid);
    CHECK(o.authed_before_accept);
    CHECK(!o.start_acked || o.client_at_ack == AzaharDlpClient::kWaitingForServerReady);
    CHECK_EQ(o.begin_results_ok, 1u);
    CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
    CHECK_EQ(h.Total(), 0u);  // (in 9 the game wants GetConnectingClients at 0 before it ends the hosting: NETCODE point 6)
    CHECK(!h.net.Holds());
    CHECK(!h.e().initialized());
    if (must_pass) {
        CHECK(o.start_acked);
        CHECK(o.begin_to_9_ms <= 2100);
        CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
        u8 first[9];
        Pass(1, first);
        CHECK(m.passphrase() == std::vector<u8>(first, first + 9));
        if (rtt == 0) {
            CHECK(o.sd_to_final_ms <= 600);
            CHECK(o.begin_to_9_ms <= 600);  // (the GO_ACK came: the lobby did not wait for its grace)
        }
    } else {
        std::printf("   RTT %u: %s (the START grace is %u ms)\n", rtt,
                    o.start_acked ? "the client made it" : "the client was expelled at the START grace, as with the dialect", DlpLobby::kStartGraceMs);
        if (!o.start_acked) {
            CHECK(m.state() != AzaharDlpClient::kComplete);
        }
    }
    // the client's own packets were all what it answers; our host's all the right lengths
    for (const Wire::Sent& s : wire.All()) {
        if (s.channel == 2) {
            const dlp_real::Packet p = dlp_real::ParseFromHost(s.p.data(), s.p.size());
            CHECK(p.kind != dlp_real::Kind::None);
        }
    }
    CHECK_EQ(h.net.counters().bad, 0u);
    m.StopSession();
    CheckModel(m);
    CHECK_RULES();
}

static void R1b_Announcement() {
    std::printf("R1b: the announcement only with a spectator\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    Console spec(room, 3), spec2(room, 4);
    Wire wire(h.con);
    Ticker ticker({&h.e(), spec.engine.get(), spec2.engine.get()});
    OpenLuigi(h);
    PumpUntil(h, 1200, [] { return false; });
    CHECK_EQ(wire.Count(1), 0u);
    CHECK_EQ(h.net.counters().announce, 0u);
    // a spectator: a group at once, ONE frame per Pump
    u16 name[kNameUnits] = {'S', 'p'};
    CHECK_EQ(spec.engine->InitializeForLobby(0x5E, name), 0u);
    // (review of 1.1.3: the host may count the spectator and send the group's first frame inside the wait below, before t_in is read:
    // the first group is checked between the connect and 100 ms after the wait, never as an unsigned difference that wraps)
    const u64 t_conn = platform::NowMs();
    CHECK_EQ(spec.engine->ConnectBeginDlp(h.mac(), 11, ConnectionType::Spectator), 0u);
    CHECK(PumpUntil(h, 3000, [&] { return spec.engine->ConnectPoll() == 1; }));
    const u64 t_in = platform::NowMs();
    u32 most_per_pump = 0;
    const u64 end = t_in + 1700;
    while (platform::NowMs() < end) {
        const u32 before = h.net.counters().announce;
        h.Pump();
        const u32 d = h.net.counters().announce - before;
        most_per_pump = d > most_per_pump ? d : most_per_pump;
        Sleep(RealDlpNet::kPumpMs + 1);
    }
    CHECK_EQ(most_per_pump, 1u);
    const std::vector<Wire::Sent> all = wire.All();
    std::vector<u64> groups;
    u32 frames = 0;
    for (const Wire::Sent& s : all) {
        if (s.channel != 1) {
            continue;
        }
        ++frames;
        CHECK_EQ(s.dest, 0xFFFFu);
        const dlp_real::Packet p = dlp_real::ParseAnnounce(s.p.data(), s.p.size());
        CHECK(p.kind == dlp_real::Kind::Announce);
        if (p.index == 0) {
            groups.push_back(s.ms);
            CHECK_EQ(dlp_real::AnnounceChildTid(s.p.data()), kLuigiChild);
            CHECK_EQ(dlp_real::AnnounceMaxNodes(s.p.data()), 2u);
        }
    }
    std::printf("   %zu groups (%u frames) in 1.7 s; the first %lld ms after the spectator came\n", groups.size(), frames,
                groups.empty() ? 0LL : static_cast<long long>(groups[0]) - static_cast<long long>(t_in));
    CHECK(!groups.empty() && groups[0] >= t_conn && groups[0] <= t_in + 100);
    CHECK(groups.size() >= 3 && groups.size() <= 5);
    for (std::size_t i = 1; i < groups.size(); ++i) {
        CHECK(groups[i] - groups[i - 1] >= 480 && groups[i] - groups[i - 1] <= 600);
    }
    // the 5 frames of a group in order, with their lengths
    u32 order = 0;
    bool lengths = true;
    for (const Wire::Sent& s : all) {
        if (s.channel == 1) {
            static const std::size_t len[5] = {768, 1448, 1448, 1448, 1464};
            lengths = lengths && s.p.size() > 12 && s.p[12] == order % 5 && s.p.size() == len[order % 5];
            ++order;
        }
    }
    CHECK(lengths);
    // a second spectator right after a group began: its group follows the one under way, not the next period
    CHECK(PumpUntil(h, 700, [&] { return h.net.counters().announce % 5 == 1; }));  // (frame 0 of a group just went)
    u16 name2[kNameUnits] = {'S', 'q'};
    CHECK_EQ(spec2.engine->InitializeForLobby(0x5F, name2), 0u);
    CHECK_EQ(spec2.engine->ConnectBeginDlp(h.mac(), 11, ConnectionType::Spectator), 0u);
    CHECK(PumpUntil(h, 3000, [&] { return spec2.engine->ConnectPoll() == 1; }));
    const u64 t_in2 = platform::NowMs();
    const u32 zeros = wire.Count(1, 1, 0);
    CHECK(PumpUntil(h, 250, [&] { return wire.Count(1, 1, 0) > zeros; }));
    std::printf("   a second spectator: a group %u ms after it came\n", static_cast<u32>(platform::NowMs() - t_in2));
    CHECK_EQ(spec2.engine->DisconnectNetwork(), 0u);
    CHECK(PumpUntil(h, 1000, [&] { return h.e().SpectatorCount() == 1; }));
    spec2.engine->Shutdown();
    // it leaves: no more
    CHECK_EQ(spec.engine->DisconnectNetwork(), 0u);
    CHECK(PumpUntil(h, 1000, [&] { return h.e().SpectatorCount() == 0; }));
    PumpUntil(h, 50, [] { return false; });
    const u32 after = wire.Count(1);
    PumpUntil(h, 900, [] { return false; });
    CHECK_EQ(wire.Count(1), after);
    // a spectator while the host no longer accepts (HostUpdate, accepting 0): it is let in - Azahar's host never closes, and an Azahar
    // searcher refused would wait 3 s inside its emulation and ask again (review of 1.1.3, protocol point 2) - but it reads nothing;
    // a client is refused (the beacon's attribute 0x2)
    const u32 ann_closed = h.net.counters().announce;
    CHECK_EQ(h.lobby.ServerEndAccepting(), 0u);
    CHECK_EQ(spec.engine->ConnectBeginDlp(h.mac(), 11, ConnectionType::Spectator), 0u);
    CHECK(PumpUntil(h, 3000, [&] { return spec.engine->ConnectPoll() == 1; }));
    CHECK(PumpUntil(h, 1000, [&] { return h.e().SpectatorCount() == 1; }));
    PumpUntil(h, 900, [] { return false; });
    CHECK_EQ(wire.Count(1), after);
    CHECK_EQ(h.net.counters().announce, ann_closed);
    CHECK_EQ(spec2.engine->InitializeForLobby(0x5F, name2), 0u);
    CHECK_EQ(spec2.engine->ConnectBeginDlp(h.mac(), 11, ConnectionType::Client), 0u);
    bool ever_in = false;  // (the engine refuses it: never a node, not even one the lobby would throw out a Pump later)
    PumpUntil(h, 700, [&] {
        u16 me = 0, mask = 0;
        h.e().NodeSnapshot(&me, &mask);
        ever_in = ever_in || (mask & 2u) != 0 || spec2.engine->ConnectPoll() == 1;
        return false;
    });
    CHECK(!ever_in);
    CHECK(spec2.engine->ConnectPoll() != 1);
    CHECK_EQ(h.Total(), 0u);
    spec2.engine->Shutdown();
    // accepting again (HostUpdate, accepting 1): the spectator still in gets a group at once
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(PumpUntil(h, 150, [&] { return wire.Count(1, 1, 0) > 0 && wire.Count(1) > after; }));
    spec.engine->Shutdown();
    h.lobby.ServerFinalize();
    CHECK(!h.net.Holds());
    CHECK_RULES();
}

/// R5: frames lost on the way to the model.
static void R5_Losses() {
    std::printf("R5: losses (a frame of a group, the first A3, the first final and its notices, every final)\n");
    for (u8 lost_idx = 1; lost_idx <= 4; ++lost_idx) {
        g_rules.Reset();
        MockRoom room;
        RHost h(room, 1, "Host");
        AzaharDlpClient m(room, 2, "Azahar");
        std::atomic<bool> dropped{false};
        m.con.drop = [&](const WifiPacket& w) {
            u8 ch, type, idx;
            if (!dropped && DlpOf(w, &ch, &type, &idx) && ch == 1 && idx == lost_idx) {
                dropped = true;
                return true;
            }
            return false;
        };
        Ticker ticker({&h.e(), &m.e()});
        OpenLuigi(h);
        u32 max_total = 0;
        const AzaharDlpClient::Probe pr = ProbeOf(h, m, &max_total);
        CHECK(dropped);
        CHECK(pr.all);
        CHECK(pr.ms_to_all >= 300 && pr.ms_to_all <= 1000);  // (with the next group)
        std::printf("   frame %u lost: 5/5 in %u ms\n", lost_idx, pr.ms_to_all);
        h.lobby.ServerFinalize();
        CheckModel(m);
        CHECK_RULES();
    }
    // the first A3; the first final with the notices of its first 500 ms; every final; the first Auth; the first Finish
    for (int c = 0; c < 5; ++c) {
        g_rules.Reset();
        MockRoom room;
        RHost h(room, 1, "Host");
        AzaharDlpClient m(room, 2, "Azahar");
        std::atomic<u64> first_final{0};
        std::atomic<u32> dropped{0};
        m.con.drop = [&](const WifiPacket& w) {
            u8 ch, type, idx;
            if (!DlpOf(w, &ch, &type, &idx) || ch != 2) {
                return false;
            }
            bool d = false;
            if (c == 0) {
                d = type == 3 && dropped == 0;
            } else if (c == 1 && type == 6) {
                if (idx == 1 && first_final == 0) {
                    first_final = platform::NowMs();
                }
                d = first_final != 0 && platform::NowMs() - first_final < 450;
            } else if (c == 2) {
                d = type == 6;
            } else if (c == 3) {
                d = type == 2 && dropped == 0;
            } else if (c == 4) {
                d = type == 5 && dropped == 0;
            }
            dropped += d ? 1 : 0;
            return d;
        };
        Ticker ticker({&h.e(), &m.e()});
        OpenLuigi(h);
        u32 max_total = 0;
        CHECK(ProbeOf(h, m, &max_total).all);
        bool joined = false;
        Pumping(h, [&] { joined = m.StartSession(h.mac()); });
        CHECK(joined);
        const Outcome o = RunGame(h, m);
        CHECK(dropped > 0);
        CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
        if (c == 0) {
            CHECK(o.start_acked);
            CHECK(h.net.counters().dist >= 2);
            CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
            std::printf("   first A3 lost: START_ACK after %u ms, %u A3 sent\n", o.start_to_ack_ms, h.net.counters().dist);
        } else if (c == 1) {
            CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
            CHECK(h.net.counters().final >= 2);
            std::printf("   first final lost (with %u packets): the client completed, %u finals sent, 9 after %u ms\n", dropped.load(),
                        h.net.counters().final, o.begin_to_9_ms);
        } else if (c >= 3) {
            CHECK(o.authed_before_accept);
            CHECK(o.start_acked);
            CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
            if (c == 3) {
                CHECK(h.net.counters().auth >= 2);
            } else {
                CHECK(h.net.counters().finish >= 2);
            }
            std::printf("   first %s lost: %u sent, START_ACK after %u ms\n", c == 3 ? "Auth" : "Finish",
                        c == 3 ? h.net.counters().auth : h.net.counters().finish, o.start_to_ack_ms);
        } else {
            CHECK_EQ(h.net.counters().final, RealDlpNet::kFinalTries);
            CHECK(o.begin_to_9_ms >= DlpLobby::kGoGraceMs && o.begin_to_9_ms <= DlpLobby::kGoGraceMs + 300);
            CHECK(m.state() != AzaharDlpClient::kComplete);  // (documented: the client stays in 7 until its game gives up)
            std::printf("   every final lost: %u finals sent, the lobby 9 at its grace (%u ms), the client in %u\n",
                        h.net.counters().final, o.begin_to_9_ms, m.state());
        }
        m.StopSession();
        CheckModel(m);
        CHECK_RULES();
    }
}

/// R6b: a game's session in the same room, with binds of channel 1.
static void R6b_ThirdConsole() {
    std::printf("R6b: a game's session in the room hears the announcement only while a spectator is in\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    Console gh(room, 5), gc(room, 6);
    Ticker ticker({&h.e(), &m.e(), gh.engine.get(), gc.engine.get()});
    u32 ev = 0;
    CHECK_EQ(gh.engine->Initialize(MakeNode(0xB1, "GameHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(2, 0);
    ni.channel = 6;
    CHECK_EQ(gh.engine->BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
    CHECK_EQ(gc.engine->Initialize(MakeNode(0xB2, "GameGst", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    u8 info[0x3C] = {};
    std::memcpy(info, gh.mac.data(), 6);
    info[6] = 6;
    CHECK_EQ(gc.engine->ConnectBegin(info, sizeof(info), 1), 0u);
    CHECK(WaitFor(3000, [&] { return gc.engine->ConnectPoll() == 1; }));
    CHECK_EQ(gc.engine->Bind(7, 0x2000, 1, 1, &ev), 0u);       // (the game's own host is node 1 too)
    CHECK_EQ(gc.engine->Bind(8, 0x2000, 1, 0xFFFF, &ev), 0u);  // (and from anybody)
    CHECK_EQ(gh.engine->Bind(7, 0x2000, 1, 0xFFFF, &ev), 0u);
    auto drain = [&](Console& c, u32 bind) {
        u32 frames = 0;
        std::vector<u8> buf(0x800);
        for (;;) {
            u32 n = 0;
            u16 src = 0;
            if (c.engine->PullPacket(bind, 0x800, buf.data(), 0x800, &n, &src) != 0 || n == 0) {
                return frames;
            }
            ++frames;
        }
    };
    OpenLuigi(h);
    PumpUntil(h, 1200, [] { return false; });
    CHECK_EQ(drain(gc, 7) + drain(gc, 8) + drain(gh, 7), 0u);
    u32 max_total = 0;
    CHECK(ProbeOf(h, m, &max_total).all);
    const u32 c7 = drain(gc, 7), c8 = drain(gc, 8), h7 = drain(gh, 7);
    std::printf("   during one probe (%u frames of the announcement sent): the game's client took %u (bind from node 1) + %u (from anybody), "
                "the game's host %u\n",
                h.net.counters().announce, c7, c8, h7);
    CHECK_EQ(h7, 0u);  // (a host drops the frames of other networks)
    PumpUntil(h, 1200, [] { return false; });
    CHECK_EQ(drain(gc, 7) + drain(gc, 8) + drain(gh, 7), 0u);
    h.lobby.ServerFinalize();
    gc.engine->Shutdown();
    gh.engine->Shutdown();
    CheckModel(m);
    CHECK_RULES();
}

/// R7: spectator -> goodbye -> client (same MAC) within `gap_ms` -> goodbye within 3 s.
static void R7_Rejoin(u32 gap_ms) {
    std::printf("R7: spectator, goodbye, client after %u ms, goodbye\n", gap_ms);
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    Ticker ticker({&h.e(), &m.e()});
    OpenLuigi(h);
    u32 max_total = 0;
    CHECK(ProbeOf(h, m, &max_total).all);
    PumpUntil(h, gap_ms, [] { return false; });
    bool joined = false;
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));  // (the Auth is answered)
    CHECK_EQ(h.Total(), 1u);
    PumpUntil(h, 800, [] { return false; });
    m.StopSession();  // (one Deauthentication)
    CHECK(PumpUntil(h, 1500, [&] { return h.Total() == 0; }));  // (not swallowed: no ghost)
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kFree);
    u16 me = 0, mask = 0;
    h.e().NodeSnapshot(&me, &mask);
    CHECK_EQ(mask, 1u);
    // it comes back: the dialogue starts from the Auth
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    const u32 auths = h.net.counters().auth;
    CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK(h.net.counters().auth > auths);
    // the game puts it out (DisconnectClient): its dialogue ends at once, then the node goes
    CHECK_EQ(h.lobby.ServerDisconnectClient(2), 0u);
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kEjected);
    CHECK(PumpUntil(h, 1500, [&] { return h.Total() == 0 && h.net.PeerState(2) == RealDlpNet::kFree; }));
    m.StopSession();
    h.lobby.ServerFinalize();
    CheckModel(m);
    CHECK_RULES();
}

/// R9: what a client may send that is not what the host takes.
static void R9_Malformed() {
    std::printf("R9: malformed packets from a client, and a client that wants the content\n");
    {
        g_rules.Reset();
        MockRoom room;
        RHost h(room, 1, "Host");
        AzaharDlpClient m(room, 2, "Azahar");
        Ticker ticker({&h.e(), &m.e()});
        OpenLuigi(h);
        u32 max_total = 0;
        CHECK(ProbeOf(h, m, &max_total).all);
        bool joined = false;
        Pumping(h, [&] { joined = m.StartSession(h.mac()); });
        CHECK(joined);
        CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
        auto raw = [&](std::vector<u8> p) { CHECK_EQ(m.e().SendTo(1, 2, static_cast<u32>(p.size()), 0, p.data(), p.size()), 0u); };
        auto hdr = [](u8 type, u32 len, u8 idx) {
            std::vector<u8> p(len, 0);
            p[0] = type;
            p[1] = 2;
            if (len >= 16) {
                p[4] = static_cast<u8>(len >> 8);
                p[5] = static_cast<u8>(len);
                p[6] = 2;
                p[12] = idx;
            }
            return p;
        };
        std::vector<u8> wrong_size = hdr(2, 0x18, 0);
        wrong_size[5] = 0x19;
        std::vector<u8> no_nine = hdr(6, 0x18, 0);
        no_nine[0x10] = 1;  // (+0x14 is not 9)
        std::vector<u8> huge = hdr(2, 1478, 0);
        const u16 bad0 = h.net.counters().bad, ign0 = h.net.counters().ignored;
        raw(std::vector<u8>(10, 0x02));  // shorter than the header
        raw(wrong_size);                 // the size field is not the length
        raw(hdr(9, 0x18, 0));            // unknown type
        raw(hdr(2, 0x14, 1));            // an Auth (a host's packet) from a client
        raw(huge);                       // longer than the adapter's buffer: taken out cut, refused
        raw(no_nine);                    // a notice ack without its 9
        std::vector<u8> ack = hdr(2, 0x18, 0);
        ack[0x10] = 1;
        raw(ack);                        // a second AuthAck: a well-formed late copy
        std::vector<u8> nine = hdr(6, 0x18, 0);
        nine[0x10] = 1;
        nine[0x14] = 9;
        raw(nine);                       // a notice ack before any final: well-formed, out of its state
        std::vector<u8> fin_content = hdr(5, 0x1C, 0);
        fin_content[0x10] = 1;
        fin_content[0x15] = 1;
        raw(fin_content);                // a Finish ack (still downloading) before any Finish
        PumpUntil(h, 300, [] { return false; });
        std::printf("   refused %u, ignored %u\n", h.net.counters().bad - bad0, h.net.counters().ignored - ign0);
        CHECK_EQ(h.net.counters().bad - bad0, 6u);
        CHECK_EQ(h.net.counters().ignored - ign0, 3u);
        CHECK_EQ(h.net.PeerState(2), RealDlpNet::kAuthed);
        u8 from = 0, msg[16];
        u32 size = 0;
        CHECK(!h.net.Receive(&from, msg, sizeof(msg), &size));  // (no ack earned)
        // the bind is not stuck: the sequence goes on to the end
        const Outcome o = RunGame(h, m);
        CHECK(o.start_acked);
        CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
        CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
        m.StopSession();
        CheckModel(m);
        CHECK_RULES();
    }
    {
        g_rules.Reset();
        MockRoom room;
        RHost h(room, 1, "Host");
        AzaharDlpClient m(room, 2, "Azahar");
        m.want_content = true;
        Ticker ticker({&h.e(), &m.e()});
        OpenLuigi(h);
        u32 max_total = 0;
        CHECK(ProbeOf(h, m, &max_total).all);
        bool joined = false;
        Pumping(h, [&] { joined = m.StartSession(h.mac()); });
        CHECK(joined);
        const Outcome o = RunGame(h, m);
        CHECK(!o.start_acked);
        CHECK_EQ(h.net.counters().content, 1u);
        CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
        CHECK(m.e().status() != NetworkStatus::ConnectedAsClient);  // (ejected)
        std::printf("   a client that wants the content: ejected, the lobby 9 after %u ms of BeginGame\n", o.begin_to_9_ms);
        m.StopSession();
        CheckModel(m);
        CHECK_RULES();
    }
    {
        // a Finish ack that says "still downloading", while the host waits for it: no START_ACK; the right one then goes on
        g_rules.Reset();
        MockRoom room;
        RHost h(room, 1, "Host");
        AzaharDlpClient m(room, 2, "Azahar");
        m.answer_finish = false;
        Ticker ticker({&h.e(), &m.e()});
        OpenLuigi(h);
        bool joined = false;
        Pumping(h, [&] { joined = m.StartSession(h.mac()); });
        CHECK(joined);
        CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
        CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
        CHECK(PumpUntil(h, 800, [&] { return h.net.PeerState(2) == RealDlpNet::kFinWait; }));
        std::vector<u8> a6(0x1C, 0);
        a6[0] = 5;
        a6[1] = 2;
        a6[5] = 0x1C;
        a6[6] = 2;
        a6[0x10] = 1;
        a6[0x14] = 1;
        a6[0x15] = 1;  // needs_content
        CHECK_EQ(m.e().SendTo(1, 2, 0x1C, 0, a6.data(), a6.size()), 0u);
        PumpUntil(h, 150, [] { return false; });
        CHECK_EQ(h.net.PeerState(2), RealDlpNet::kFinWait);
        CHECK_EQ(h.ClientExt(2), static_cast<u32>(kPeerStarting));
        m.answer_finish = true;
        CHECK(PumpUntil(h, 1000, [&] { return h.ClientExt(2) == kPeerDistributed; }));
        h.lobby.ServerFinalize();
        m.StopSession();
        CheckModel(m);
        CHECK_RULES();
    }
}

/// R12: the network is full; a second Azahar searcher.
static void R12_Full() {
    std::printf("R12: a second searcher when the network is full\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient a(room, 2, "AzaharA"), b(room, 3, "AzaharB");
    Ticker ticker({&h.e(), &a.e(), &b.e()});
    OpenLuigi(h);
    u32 max_total = 0;
    CHECK(ProbeOf(h, a, &max_total).all);
    bool joined = false;
    Pumping(h, [&] { joined = a.StartSession(h.mac()); });
    CHECK(joined);
    CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    const u32 ann = h.net.counters().announce;
    const AzaharDlpClient::Probe pb = ProbeOf(h, b, &max_total);
    CHECK(!pb.connected);
    CHECK_EQ(b.timeouts.load(), 1u);
    CHECK_EQ(h.net.counters().announce, ann);
    CHECK_EQ(max_total, 1u);
    const Outcome o = RunGame(h, a);
    CHECK(o.start_acked);
    CHECK(PumpUntil(h, 1000, [&] { return a.state() == AzaharDlpClient::kComplete; }));
    a.StopSession();
    CheckModel(a);
    CheckModel(b);
    CHECK_RULES();
}

/// R14: the lobby's resends and the game's BeginGame storm.
static void R14_Idempotent() {
    std::printf("R14: START and GO repeated, BeginGame every 33 ms with new passphrases\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    m.answer_dist = false;
    Ticker ticker({&h.e(), &m.e()});
    OpenLuigi(h);
    u32 max_total = 0;
    CHECK(ProbeOf(h, m, &max_total).all);
    bool joined = false;
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    // a GO before the client is ready: kept, no final yet
    static const u8 kStart[2] = {1, 0};
    u8 go[11] = {3, 9};
    Pass(40, go + 2);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    const u64 t_sd = platform::NowMs();
    CHECK(PumpUntil(h, 200, [&] { return h.net.PeerState(2) == RealDlpNet::kDistWait; }));
    {
        CountNews news;  // (the game-thread calls allocate nothing)
        for (int i = 0; i < 20; ++i) {
            CHECK(h.net.Send(2, kStart, 2));
        }
        CHECK(h.net.Send(2, go, sizeof(go)));
        u8 from = 0, msg[16];
        u32 size = 0;
        CHECK(!h.net.Receive(&from, msg, sizeof(msg), &size));
        u8 nodes[kMaxPlayers];
        CHECK_EQ(h.net.Members(nodes), 2u);
        (void)h.net.NowMs();
        (void)h.net.NowTicks();
        CHECK_EQ(news.count(), 0u);
    }
    PumpUntil(h, 1100 - static_cast<u32>(platform::NowMs() - t_sd), [] { return false; });
    const u32 dist = h.net.counters().dist;
    std::printf("   1.1 s of the lobby's START every 200 ms: %u A3 (the adapter's own 400 ms)\n", dist);
    CHECK_EQ(dist, 3u);
    CHECK_EQ(h.net.counters().final, 0u);
    m.answer_dist = true;
    CHECK(PumpUntil(h, 1500, [&] { return h.ClientExt(2) == kPeerDistributed || h.lobby.server_state() != kSrvDistributing; }));
    CHECK_EQ(h.ClientExt(2), static_cast<u32>(kPeerDistributed));
    CHECK(PumpUntil(h, 500, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
    // the adapter's first passphrase is the GO given above (40): the lobby's own is refused by the adapter, and the game's are refused by the lobby
    m.answer_notice = false;  // (so that the finals are all resent: their count must not start again)
    const u64 t_begin = platform::NowMs();
    u64 next = t_begin;
    for (u8 k = 1; platform::NowMs() - t_begin < 2600;) {
        if (platform::NowMs() >= next && h.lobby.server_state() == kSrvPassphrase) {
            u8 nine[9];
            Pass(k++, nine);
            h.lobby.ServerSendPassphrase(nine);
            u8 again[11] = {3, 9};
            Pass(static_cast<u8>(100 + k), again + 2);
            h.net.Send(2, again, sizeof(again));  // (and a GO with yet another passphrase, straight to the adapter)
            next += 33;
        }
        h.Pump();
        Sleep(2);
    }
    std::printf("   %u finals sent, the client has \"%s\"\n", h.net.counters().final, m.passphrase().empty() ? "" : reinterpret_cast<const char*>(m.passphrase().data()));
    CHECK_EQ(h.net.counters().final, RealDlpNet::kFinalTries);
    CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
    CHECK(m.state() == AzaharDlpClient::kComplete);
    u8 first[9];
    Pass(40, first);
    CHECK(m.passphrase() == std::vector<u8>(first, first + 9));
    m.StopSession();
    CheckModel(m);
    CHECK_RULES();
}

/// R14b: the adapter alone (no lobby to end it at its grace), two clients: the finals stop at kFinalTries, the notices go on, a START to a
/// client already past the Auth does not start the distribution again, a client the lobby gave no GO gets no final.
static void R14b_FinalsBounded() {
    std::printf("R14b: the adapter alone, a client that never answers the notice and one that gets no GO\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar"), m3(room, 3, "AzaharC");
    m.answer_notice = false;
    Ticker ticker({&h.e(), &m.e(), &m3.e()});
    OpenLuigi(h, 2);
    bool joined = false, joined3 = false;
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    Pumping(h, [&] { joined3 = m3.StartSession(h.mac()); });
    CHECK(joined3);
    auto pump_net = [&](u32 ms, auto cond) {
        const u64 end = platform::NowMs() + ms;
        while (!cond() && platform::NowMs() < end) {
            h.net.Pump();
            Sleep(2);
        }
        return cond();
    };
    CHECK(pump_net(1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed && h.net.PeerState(3) == RealDlpNet::kAuthed; }));
    static const u8 kStart[2] = {1, 0};
    CHECK(h.net.Send(2, kStart, 2));
    CHECK(h.net.Send(3, kStart, 2));
    CHECK(pump_net(1500, [&] { return h.net.PeerState(2) == RealDlpNet::kReady && h.net.PeerState(3) == RealDlpNet::kReady; }));
    u8 from = 0, msg[16];
    u32 size = 0;
    u32 acks = 0, from_mask = 0;
    while (h.net.Receive(&from, msg, sizeof(msg), &size)) {
        CHECK_EQ(size, 2u);
        CHECK_EQ(msg[0], 2u);  // START_ACK
        from_mask |= 1u << from;
        ++acks;
    }
    CHECK_EQ(acks, 2u);
    CHECK_EQ(from_mask, (1u << 2) | (1u << 3));
    const u32 dist = h.net.counters().dist;
    CHECK(h.net.Send(2, kStart, 2));  // (a START once the client is ready: nothing)
    u8 go[11] = {3, 9};
    Pass(7, go + 2);
    CHECK(h.net.Send(2, go, sizeof(go)));  // (node 2 only)
    pump_net(3200, [] { return false; });
    std::printf("   %u finals, %u notices in 3.2 s\n", h.net.counters().final, h.net.counters().notice);
    CHECK_EQ(h.net.counters().final, RealDlpNet::kFinalTries);
    CHECK(h.net.counters().notice >= 20);
    CHECK_EQ(h.net.counters().dist, dist);
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kGoWait);
    CHECK_EQ(h.net.PeerState(3), RealDlpNet::kReady);
    CHECK(!h.net.Receive(&from, msg, sizeof(msg), &size));  // (no GO_ACK without an A9)
    CHECK(m.state() == AzaharDlpClient::kComplete);
    CHECK(m3.state() == AzaharDlpClient::kWaitingForServerReady);
    CHECK_EQ(m3.received(6, 1), 0u);
    h.lobby.ServerFinalize();
    CHECK(!h.net.Holds());
    m.StopSession();
    m3.StopSession();
    CheckModel(m);
    CheckModel(m3);
    CHECK_RULES();
}

/// The engine is nobody's after the lobby ends in every state, the adapter's calls without the engine refuse cleanly.
static void Release_Paths() {
    std::printf("Release: HostClose / Finalize in every state of the host, and the searcher's calls of phase F4\n");
    g_rules.Reset();
    MockRoom room;
    RHost h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    Ticker ticker({&h.e(), &m.e()});
    // a game's session already up: HostOpen refuses and touches nothing
    u32 ev = 0;
    CHECK_EQ(h.e().Initialize(MakeNode(0xC1, "Game", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), static_cast<u32>(R_BUSY));
    CHECK(!h.net.Holds());
    CHECK(h.e().initialized());
    CHECK_EQ(h.e().Shutdown(), 0u);
    // the searcher's calls with nobody to find (phase F4: Tests/dlp_real_client_test.cpp): nothing listed, no join, the engine not kept
    CHECK(h.net.ScanOpen());
    HeardGroup hg;
    CHECK(!h.net.ScanRead(0, &hg));
    CHECK(!h.net.ConnectBegin(m.mac(), Me(1, "x")));  // (no beacon of that MAC: "not found")
    CHECK_EQ(h.net.ConnectPoll(), -1);
    h.net.ScanClose();
    CHECK(!h.net.Holds());
    CHECK(!h.e().initialized());
    // the host closed in 2 (with a client in), in 6, in 8: the engine is given back, its bind with it
    for (int stop = 0; stop < 3; ++stop) {
        CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
        CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
        bool joined = false;
        Pumping(h, [&] { joined = m.StartSession(h.mac()); });
        CHECK(joined);
        CHECK(PumpUntil(h, 1500, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
        if (stop >= 1) {
            CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
            CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
        }
        if (stop == 2) {
            CHECK(PumpUntil(h, 2000, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
        }
        CHECK(h.lobby.OnGameUdsStarting());  // (the game's nwm::UDS Initialize, no network thread: RunHere)
        CHECK(!h.net.Holds());
        CHECK(!h.e().initialized());
        CHECK_EQ(h.net.PeerState(2), RealDlpNet::kFree);
        u8 from = 0, msg[16];
        u32 size = 0;
        CHECK(!h.net.Receive(&from, msg, sizeof(msg), &size));
        CHECK(!h.net.Send(2, msg, 2));
        h.Pump();  // (nothing: not hosting)
        CHECK(!h.e().initialized());
        m.StopSession();
        h.lobby.ServerFinalize();
    }
    CheckModel(m);
    CHECK_RULES();
}

int main() {
    std::printf("dlp_real_net_test: the host of the real Download Play in front of Azahar's client\n");
    R1_Sequence(0, true);
    R1b_Announcement();
    for (u32 rtt : {100u, 300u, 500u}) {
        R1_Sequence(rtt, true);
    }
    R1_Sequence(700, false);
    R5_Losses();
    R6b_ThirdConsole();
    R7_Rejoin(100);
    R7_Rejoin(2000);
    R9_Malformed();
    R12_Full();
    R14_Idempotent();
    R14b_FinalsBounded();
    Release_Paths();
    // (rig helpers this binary has no use for)
    (void)&Join;
    (void)&FindGroup;
    (void)&OpenLobby;
    (void)&CliNode;
    (void)&CliState;
    (void)&LobbyFrame;
    (void)&EngineClean;
    (void)&Engines;
    (void)&MakeBeacon;
    (void)&Settle;
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
