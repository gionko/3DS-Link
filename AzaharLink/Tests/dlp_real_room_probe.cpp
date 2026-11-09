// The REAL Download Play (1.1.3, DlpGameMode::RealDlp: Luigi's Mansion) over a REAL azahar-room server (Tests/integration.sh): design
// analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md, section 7.5 (I1-I3). Every console is a genuine RoomClient (ENet) + UdsEngine
// + RealDlpNet + DlpLobby, so the frames of the announcement (1.4 KB each) and of the dialogue take the room's wire, its routing and its
// delays; the only thing not real is the game (this program plays what NETCODE.md reads in Luigi's code). It is the companion of
// Tests/dlp_room_probe.cpp (the dialect's lobby over the same room): a binary of its own because it needs the in-memory rig of the models
// (Tests/dlp_net_rig.hpp, Tests/azahar_dlp_peer.hpp), whose names would collide with that probe's.
//
//   I1  our host and our searcher of the real Download Play, two engines, two ENet members: the 5 frames of the announcement (768, 1448,
//       1448, 1448, 1464 bytes) reach the searcher WHOLE (every frame the host sent on channel 1, with its length) while its probe is
//       connected, the probe verifies the host (title, max_nodes, name), the dialogue A1..A9 runs, both lobbies end at 9 with the FIRST
//       passphrase, both engines are given back (not held, not initialised, filter off, no bind left)
//   I2  our host and a model of Azahar's Download Play client (Tests/azahar_dlp_peer.hpp) behind an ENet member of its own: the model's
//       probe reads the 5 frames within 1000 ms of its bind, a spectator is never in GetConnectingClients, the session to 9 with the first
//       passphrase, and no frame of ours would make Azahar crash or misread (the model's violations)
//   I3  20 rounds of I1 in a row with the same two members: the searcher is node 2 every time (no ghost node left by the round before),
//       no bind left, the filter off at the end, the memory of the process stable (ASan's count of allocated bytes, rounds 3..20)
//
//   dlp_real_room_probe <room-host> <room-port>
//
// A test that cannot get a player into the room says so and FAILS (a room that is not there proves nothing).
#include <algorithm>
#include <cstddef>

// (the rig's helpers for the dialect's players are not used here: no warning for each of them)
#pragma GCC diagnostic ignored "-Wunused-function"
#include "dlp_net_rig.hpp"

#include "azahar_dlp_peer.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/room_client.hpp"

// ASan's count of the bytes allocated now (sanitizer/allocator_interface.h, which the image of the tests does not carry)
extern "C" std::size_t __sanitizer_get_current_allocated_bytes();

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kLuigiLow = 0x001D1A00;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void Pass(u8 k, u8 out[9]) {
    static const char* hex = "0123456789abcdef";
    for (u32 i = 0; i < 8; ++i) {
        out[i] = static_cast<u8>(hex[(k * 7 + i * 3) & 15]);
    }
    out[8] = 0;
}

/// A frame of the UDS data path: its channel and its payload (as Tests/dlp_real_net_test.cpp's Wire reads it).
static bool Payload(const WifiPacket& w, u8* channel, std::vector<u8>* p) {
    EtherType t;
    SecureDataHeader h;
    if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData ||
        !ParseSecureDataHeader(w.data, &h) || h.ActualDataSize() > w.data.size()) {
        return false;
    }
    *channel = h.data_channel;
    p->assign(w.data.end() - h.ActualDataSize(), w.data.end());
    return true;
}

/// One console in the real room: its RoomClient, its engine (ticked by the room client's thread), and a record of the frames of channel 1
/// it sent and received (the announcement).
struct RoomConsole {
    RoomConsole(const std::string& host, u16 port, const char* nick) {
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.console_id_hash = std::string("real-dlp-probe-") + nick;
        config.game_name = "Test Game";
        config.game_id = 0x0004000000164800ULL;
        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (!room.joined()) {
                return;
            }
            p.transmitter_address = room.mac();
            g_rules.Inspect(p);
            Note(p, &sent1);
            room.SendWifiPacket(p);
        };
        transport.get_mac = [this] { return room.mac(); };
        engine.reset(new UdsEngine(&kernel, transport));
        target = engine.get();
        // (the callbacks are set once, before the room client runs; I2 points `target` at the model's engine instead)
        room.SetWifiCallback([this](const WifiPacket& p) {
            Note(p, &got1);
            if (UdsEngine* e = target.load()) {
                e->OnWifiPacket(p);
            }
        });
        room.SetTickCallback([this](u64 now_ms) {
            if (UdsEngine* e = target.load()) {
                e->Tick(now_ms);
            }
        });
    }
    ~RoomConsole() {
        room.Stop();
    }
    bool Join() {
        room.Start(config);
        const bool joined = WaitFor(8000, [this] {
            return room.state() == RoomClient::State::Joined ||
                   (room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None);
        }) && room.joined();
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    void Note(const WifiPacket& w, std::vector<u32>* sizes) {
        u8 ch = 0;
        std::vector<u8> p;
        if (Payload(w, &ch, &p) && ch == RealDlpNet::kChannelAnnounce) {
            std::lock_guard<std::mutex> l(m);
            sizes->push_back(static_cast<u32>(p.size()));
        }
    }
    std::vector<u32> Sizes(std::vector<u32>* v) {
        std::lock_guard<std::mutex> l(m);
        return *v;
    }
    void ClearSizes() {
        std::lock_guard<std::mutex> l(m);
        sent1.clear();
        got1.clear();
    }
    RoomClient::Config config;
    RoomClient room;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
    std::atomic<UdsEngine*> target{nullptr};  // the engine the room's frames and ticks go to
    std::mutex m;
    std::vector<u32> sent1, got1;  // payload lengths of the frames of channel 1
};

/// A console of Luigi's Mansion in the real room: the adapter of the real Download Play and the lobby on it.
struct Luigi {
    Luigi(const std::string& host, u16 port, const char* nick, u8 last)
        : name(nick), con(host, port, nick), net(con.engine.get()), lobby(&net, Player::Cfg(kLuigiLow, last, nick)) {
        net.SetIdentity(Me(0xF000u + last, nick));
    }
    UdsEngine& e() {
        return *con.engine;
    }
    Mac mac() {
        return con.room.mac();
    }
    /// The network thread's turn: the adapter, then the lobby (the hub's order, design D10).
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    u32 Status() const {
        return lobby.ClientStatusWord() >> 24;
    }
    u32 Total() {
        u16 ids[4];
        u32 total = 0;
        lobby.ServerConnectingClients(ids, 4, &total);
        return total;
    }
    u32 ClientExt(u8 node) {
        u32 w = 0;
        return lobby.ServerClientState(node, &w) == 0 ? (w >> 24) : 0xFFu;
    }
    std::string name;
    RoomConsole con;
    RealDlpNet net;
    DlpLobby lobby;
};

template <typename F>
static bool PumpUntil(std::vector<Luigi*> ls, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        for (Luigi* l : ls) {
            l->Pump();
        }
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(2);
    }
}

/// The engine is nobody's: not held, not initialised, no probe, the filter off, no bind of the adapter left (Shutdown freed them).
static bool Clean(Luigi& l, const char* who) {
    bool ok = !l.net.Holds() && !l.e().initialized() && !l.e().client_foreign_filter_enforced() && !l.net.Probing();
    UdsEngine::UnbindInfo info;
    for (u32 bind : {RealDlpNet::kHostBind, RealDlpNet::kProbeBind, RealDlpNet::kSessionBind}) {
        info.channel = 0;
        ok = ok && l.e().Unbind(bind, &info) == 0 && info.channel == 0;
    }
    if (!ok) {
        std::printf("   not clean (%s): held %d, initialised %d, filter %d, probing %d\n", who, l.net.Holds(), l.e().initialized(),
                    l.e().client_foreign_filter_enforced(), l.net.Probing());
    }
    return ok;
}

static std::string Join(const std::vector<u32>& v) {
    std::string s;
    for (u32 x : v) {
        s += (s.empty() ? "" : ",") + std::to_string(x);
    }
    return s;
}

/// The frames of channel 1 the searcher received are exactly those the host sent (same count, same lengths, in order), every length one
/// of the group's, and at least one whole group.
static void CheckAnnouncementWhole(Luigi& host, Luigi& searcher) {
    // (a frame still on the wire when the probe ended reaches the searcher a little later: wait for the counts to meet)
    WaitFor(2000, [&] { return host.con.Sizes(&host.con.sent1).size() == searcher.con.Sizes(&searcher.con.got1).size(); });
    const std::vector<u32> sent = host.con.Sizes(&host.con.sent1);
    const std::vector<u32> got = searcher.con.Sizes(&searcher.con.got1);
    if (sent != got) {
        std::printf("   announcement: sent %s / received %s\n", Join(sent).c_str(), Join(got).c_str());
    }
    CHECK(sent == got);
    u32 n768 = 0, n1448 = 0, n1464 = 0;
    for (u32 s : got) {
        CHECK(s == dlp_real::kFrame0Bytes || s == 1448 || s == dlp_real::kFrame4Bytes);
        n768 += s == dlp_real::kFrame0Bytes;
        n1448 += s == 1448;
        n1464 += s == dlp_real::kFrame4Bytes;
    }
    CHECK(n768 >= 1 && n1448 >= 3 && n1464 >= 1);
}

/// One round of I1 between `h` (host) and `c` (searcher); prints its times when `verbose`. The searcher's node id, or 0.
static u32 RoundI1(Luigi& h, Luigi& c, bool verbose) {
    const std::vector<Luigi*> both = {&h, &c};
    h.con.ClearSizes();
    c.con.ClearSizes();
    const u16 verified0 = c.net.counters().verified;
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(h.net.Holds());
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    const u64 t0 = platform::NowMs();
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    GroupView v;
    const bool listed = PumpUntil(both, 6000, [&] { return c.lobby.ClientTitleInfo(h.mac(), &v) == 0; });
    CHECK(listed);
    if (!listed) {
        c.lobby.ClientFinalize();
        h.lobby.ServerFinalize();
        return 0;
    }
    const u32 t_list = static_cast<u32>(platform::NowMs() - t0);
    CHECK_EQ(Str(v.group->name), h.name);
    CHECK_EQ(v.group->child_tid_low, kLuigiLow);
    CHECK_EQ(v.group->max_nodes, 2u);
    CHECK_EQ(static_cast<u32>(c.net.counters().verified - verified0), 1u);
    CHECK(PumpUntil(both, 2000, [&] { return !c.net.Probing(); }));
    CHECK_EQ(h.Total(), 0u);  // (the spectator of the probe is never a client: NETCODE point 3)
    c.lobby.ClientStopScan();
    CheckAnnouncementWhole(h, c);
    u32 r = 0xFFFFFFFF;
    const u64 t_join = platform::NowMs();
    CHECK(c.lobby.ClientJoinBegin(h.mac(), &r) == JoinStep::Pending);
    CHECK(PumpUntil(both, 5000, [&] { return c.lobby.ClientJoinPoll(&r); }));
    CHECK_EQ(r, 0u);
    const u32 node = c.lobby.ClientStatusWord() & 0xFF;
    CHECK(PumpUntil(both, 4000, [&] { return h.ClientExt(2) == kPeerWaiting && h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    const u64 t_sd = platform::NowMs();
    CHECK(PumpUntil(both, 4000, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
    const u32 sd_ms = static_cast<u32>(platform::NowMs() - t_sd);
    u8 k = 1;
    u64 next = 0;
    const u64 t_begin = platform::NowMs();
    CHECK(PumpUntil(both, 4000, [&] {
        if (h.lobby.server_state() == kSrvPassphrase && platform::NowMs() >= next) {
            u8 nine[9];
            Pass(k++, nine);
            h.lobby.ServerSendPassphrase(nine);  // (Luigi: BeginGame every 33 ms with a new passphrase while 8)
            next = platform::NowMs() + 33;
        }
        return h.lobby.server_state() == kSrvComplete && c.Status() == 9;
    }));
    const u32 go_ms = static_cast<u32>(platform::NowMs() - t_begin);
    CHECK_EQ(h.Total(), 0u);  // (in 9 the game wants GetConnectingClients at 0: NETCODE point 6)
    u8 pass[9] = {}, first[9];
    Pass(1, first);
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, first, 9) == 0);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);  // (on the game's thread, no Pump: NETCODE point 7)
    CHECK(!h.net.Holds());
    h.lobby.ServerFinalize();
    CHECK(Clean(c, "searcher"));
    CHECK(Clean(h, "host"));
    CHECK_EQ(h.net.counters().bad, 0u);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    if (verbose) {
        std::printf("   listed after %u ms (%zu frames of the announcement, all received whole), join %u ms, StartDistribution -> 8 %u ms, "
                    "BeginGame -> 9 %u ms, node %u\n",
                    t_list, h.con.Sizes(&h.con.sent1).size(), static_cast<u32>(t_sd - t_join), sd_ms, go_ms, node);
    }
    return node;
}

static bool TwoIn(Luigi& a, Luigi& b) {
    if (!a.con.Join() || !b.con.Join()) {
        std::fprintf(stderr, "  (cannot continue without both players in the room)\n");
        CHECK(false);  // (a room that cannot be reached proves nothing: it is a failure, not a pass)
        return false;
    }
    return true;
}

static void TestI1(const std::string& host, u16 port) {
    std::printf("I1: our host and our searcher of the real Download Play over the real room\n");
    g_rules.Reset();
    Luigi h(host, port, "RealHost", 1), c(host, port, "RealSeek", 2);
    if (!TwoIn(h, c)) {
        return;
    }
    CHECK_EQ(RoundI1(h, c, true), 2u);
    CHECK_RULES();
}

static void TestI2(const std::string& host, u16 port) {
    std::printf("I2: our host and a model of Azahar's Download Play client behind its own ENet member\n");
    g_rules.Reset();
    Luigi h(host, port, "RealHst2", 1);
    // the model's radio: an ENet member of the real room; its own console sits alone in an in-memory room and its frames are forwarded
    RoomConsole bridge(host, port, "AzaharM");
    if (!h.con.Join() || !bridge.Join()) {
        std::fprintf(stderr, "  (cannot continue without both players in the room)\n");
        CHECK(false);
        return;
    }
    MockRoom mock;
    const Mac model_mac = bridge.room.mac();
    std::unique_ptr<AzaharDlpClient> m(new AzaharDlpClient(mock, 2, "Azahar", 11, &model_mac));
    m->con.sniff = [&bridge](const WifiPacket& p) {
        WifiPacket copy = p;
        bridge.room.SendWifiPacket(copy);
    };
    bridge.target = &m->e();
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(PumpUntil({&h}, 4000, [&] { return m->Heard(h.mac()); }));
    // the probe (synchronous in the model: a thread of its own, the host pumped meanwhile)
    AzaharDlpClient::Probe pr;
    u32 max_total = 0;
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            pr = m->ProbeHost(h.mac());
            done = true;
        });
        while (!done) {
            h.Pump();
            max_total = std::max(max_total, h.Total());
            Sleep(2);
        }
        t.join();
    }
    CHECK(pr.connected);
    CHECK(pr.all);
    CHECK(!pr.ignored);
    CHECK(pr.ms_to_all <= 1000);
    CHECK_EQ(pr.child_tid, kLuigiChild);
    CHECK_EQ(pr.max_nodes, 2u);
    CHECK_EQ(Str(pr.name), h.name);
    CHECK_EQ(max_total, 0u);
    std::printf("   probe: 5/5 in %u ms from the bind, %u frames taken\n", pr.ms_to_all, m->probe_frames.load());
    CHECK(PumpUntil({&h}, 2000, [&] { return h.e().SpectatorCount() == 0; }));
    // the session
    bool joined = false;
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            joined = m->StartSession(h.mac());
            done = true;
        });
        while (!done) {
            h.Pump();
            Sleep(2);
        }
        t.join();
    }
    CHECK(joined);
    CHECK(PumpUntil({&h}, 3000, [&] { return h.Total() == 1; }));
    CHECK(PumpUntil({&h}, 4000, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.ClientExt(2), static_cast<u32>(kPeerWaiting));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    const u64 t_sd = platform::NowMs();
    CHECK(PumpUntil({&h}, 4000, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
    const u32 sd_ms = static_cast<u32>(platform::NowMs() - t_sd);
    CHECK_EQ(m->state(), static_cast<u32>(AzaharDlpClient::kWaitingForServerReady));
    const u64 t_begin = platform::NowMs();
    u64 next = t_begin;
    u32 ok = 0;
    for (u8 k = 1; h.lobby.server_state() == kSrvPassphrase && platform::NowMs() - t_begin < 4000;) {
        if (platform::NowMs() >= next) {
            u8 nine[9];
            Pass(k++, nine);
            ok += h.lobby.ServerSendPassphrase(nine) == 0;
            next += 33;
        }
        h.Pump();
        Sleep(2);
    }
    const u32 go_ms = static_cast<u32>(platform::NowMs() - t_begin);
    std::printf("   StartDistribution -> 8 %u ms, BeginGame -> 9 %u ms (BeginGame answered 0 x%u)\n", sd_ms, go_ms, ok);
    CHECK_EQ(ok, 1u);
    CHECK_EQ(h.lobby.server_state(), static_cast<u32>(kSrvComplete));
    CHECK_EQ(h.Total(), 0u);
    CHECK(PumpUntil({&h}, 2000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
    u8 first[9];
    Pass(1, first);
    CHECK(m->passphrase() == std::vector<u8>(first, first + 9));
    h.lobby.ServerFinalize();
    CHECK(Clean(h, "host"));
    CHECK_EQ(h.net.counters().bad, 0u);
    m->StopSession();
    for (const std::string& s : m->violations()) {
        std::printf("   model violation: %s\n", s.c_str());
    }
    CHECK(m->violations().empty());
    CHECK(!m->session_broken);
    bridge.room.Stop();  // (the room client's thread has ended: no delivery to the model after this)
    bridge.target = nullptr;
    m.reset();
    CHECK_RULES();
}

static void TestI3(const std::string& host, u16 port) {
    std::printf("I3: 20 rounds of I1 in a row, the same two members\n");
    g_rules.Reset();
    Luigi h(host, port, "RealHst3", 1), c(host, port, "RealSek3", 2);
    if (!TwoIn(h, c)) {
        return;
    }
    std::size_t mem_after2 = 0, mem_most = 0;
    u32 ghosts = 0;
    for (u32 round = 1; round <= 20; ++round) {
        const u32 node = RoundI1(h, c, round == 1 || round == 20);
        ghosts += node != 2;
        const std::size_t now = __sanitizer_get_current_allocated_bytes();
        if (round == 2) {
            mem_after2 = now;
        }
        if (round >= 3) {
            mem_most = std::max(mem_most, now);
        }
        if (g_failures != 0) {
            std::printf("   stopped at round %u\n", round);
            break;
        }
    }
    CHECK_EQ(ghosts, 0u);
    std::printf("   allocated bytes after round 2: %zu, the most after rounds 3..20: %zu\n", mem_after2, mem_most);
    CHECK(mem_most <= mem_after2 + 64 * 1024);  // (stable: a leak of a round would add up over 18 rounds)
    CHECK(!c.e().client_foreign_filter_enforced());
    CHECK(!h.e().client_foreign_filter_enforced());
    CHECK_RULES();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <room-host> <room-port>\n", argv[0]);
        return 2;
    }
    const std::string host = argv[1];
    const u16 port = static_cast<u16>(std::atoi(argv[2]));
    std::printf("dlp_real_room_probe: the real Download Play over the real room (%s:%u)\n", host.c_str(), port);
    TestI1(host, port);
    TestI2(host, port);
    TestI3(host, port);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
