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
//   I4  (1.1.4, H5) the host's wait for the children with the room's own lists (Plugin::OnRoomInfo's wiring): a guest whose entry says the
//       Download Play application is awaited in the lobby's 9; a console's member that leaves and comes back with the child (same MAC) ends
//       it at once, an Azahar that stays holds it 5 s, an Azahar that says it plays nothing for a second is seen out and back
//
//   I5  (1.1.4, route B: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md 6.5) the 3DS Link app's Download Play page - its own
//       Core (ConfigApp/source/dlp_join.cpp) on a RoomClient with the page's identity (kAppTid, "Download Play (3DS Link)", "3ds-<mac>-dlp", the
//       console's MAC and nickname) - joins OUR host of Island Tour (H5's wait on, the room's lists filtered by the plugin's own AwaitsChildOf, cut
//       out of plugin.cpp), to the lobby's 9; the hand-over (ns:s faked), the polite leave of D-B10 (two rounds, then RoomClient::Stop), then the
//       prepared child's plugin: a RoomClient with the plugin's identity ("3ds-<mac>", the same MAC and nickname, the prepared title) driven by
//       RejoinPolicy with HoldIdentity(8000) as Plugin::Tick drives it
//       I5a the child 500 ms after the app's Stop: no collision, in at its first attempt; the room answered the leave (LeaveAcknowledged); the
//           host's wait sees the app out and its child back; the time from Stop to the app gone from the host's list, measured
//       I5b the app stays listed: it leaves 3 s after the child began to try. The child is refused (a collision), keeps the console's MAC and
//           name (never a room-assigned MAC, never the suffix), and is in at its first attempt after the app is gone; the host sees it back
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
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/link_policy.hpp"
#include "azl/room_client.hpp"
#include "azl/room_members.hpp"
#include "azl/title_rules.hpp"
#include "ctr/text_search_guard.hpp"
#include "dlp_join.hpp"  // (I5: the app's Core, ConfigApp/source/dlp_join.cpp)

// AwaitsChildOf as the plugin has it (the slice of plugin.cpp Tests/plugin_dlp_extract.py cuts: the H5 filter of OnRoomInfo)
namespace azl {
namespace ctr {
namespace {
#include "plugin_dlp_slice.inc"
}  // namespace
}  // namespace ctr
}  // namespace azl

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
        u8 ids[4];
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

// ---- I4 (1.1.4, H5): the host's wait for the children, with the room's own lists ---------------------------------------------------------
static constexpr u64 kDownloadPlayEur = 0x0004001000022100ULL;  // the system's Download Play application (Europe): what a guest's room entry says there
static constexpr u64 kChildOfTheGame = 0x00040001001D1A00ULL;   // what it says once its child runs (here Luigi's: the probe's game)

/// The host's RoomInfo as Plugin::OnRoomInfo gives it to the engine: the members that play, and those in the Download Play application.
static void WireRoomInfo(Luigi& h) {
    UdsEngine* const e = h.con.engine.get();
    h.con.room.SetRoomInfoCallback([e](const room::RoomInfo& info) {
        std::vector<Mac> plays, dlplay;
        for (const room::MemberInfo& m : info.members) {
            if (!m.game_name.empty() || m.game_id != 0) {
                plays.push_back(m.mac);
            }
            if (m.game_id == kDownloadPlayEur) {
                dlplay.push_back(m.mac);
            }
        }
        e->OnRoomMembers(plays);
        e->SetDownloadPlayMembers(dlplay);
    });
}

/// I4: our host (the wait on) and a model of Azahar's client whose room entry says "Download Play", behind its own ENet member, to the lobby's 9;
/// then, by `how`: 0 a console - its member leaves the room and a new one comes back with the same MAC and the child's game (the plugin of the
/// child); 1 an Azahar - it stays and its entry says the child's game; 2 an Azahar that says it plays nothing for a second, then the child.
/// What the host's game reads (GetConnectingClients) and when it reaches 0, with the room's real lists and delays.
static void RunI4(const std::string& host, u16 port, int how) {
    static const char* const kHow[] = {"a console: out of the room and back with its child", "an Azahar: it stays, its entry says the child",
                                       "an Azahar that says it plays nothing for a second, then the child"};
    std::printf("I4: the host waits for the child of %s\n", kHow[how]);
    g_rules.Reset();
    Luigi h(host, port, how == 0 ? "WaitHst0" : how == 1 ? "WaitHst1" : "WaitHst2", 1);
    h.net.SetChildWait(true);
    WireRoomInfo(h);
    std::unique_ptr<RoomConsole> bridge(new RoomConsole(host, port, "AzDlplay"));
    bridge->config.game_name = "Download Play";
    bridge->config.game_id = kDownloadPlayEur;
    const bool in_room = h.con.Join() && bridge->Join();
    CHECK(in_room);  // (a room that cannot be reached proves nothing: a failure)
    if (!in_room) {
        return;
    }
    MockRoom mock;
    const Mac model_mac = bridge->room.mac();
    std::unique_ptr<AzaharDlpClient> m(new AzaharDlpClient(mock, 2, "Azahar", 11, &model_mac));
    RoomConsole* const relay = bridge.get();
    m->con.sniff = [relay](const WifiPacket& p) {
        WifiPacket copy = p;
        relay->room.SendWifiPacket(copy);
    };
    bridge->target = &m->e();
    // the room's lists reach the host: the guest is listed in the Download Play application
    CHECK(PumpUntil({&h}, 4000, [&] { return h.e().InRoom(model_mac) && h.e().InDownloadPlay(model_mac); }));
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(PumpUntil({&h}, 4000, [&] { return m->Heard(h.mac()); }));
    AzaharDlpClient::Probe pr;
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            pr = m->ProbeHost(h.mac());
            done = true;
        });
        while (!done) {
            h.Pump();
            Sleep(2);
        }
        t.join();
    }
    CHECK(pr.all);
    CHECK(PumpUntil({&h}, 2000, [&] { return h.e().SpectatorCount() == 0; }));
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
    CHECK(PumpUntil({&h}, 4000, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil({&h}, 4000, [&] { return h.lobby.server_state() == kSrvPassphrase; }));
    u8 first[9];
    Pass(1, first);
    CHECK_EQ(h.lobby.ServerSendPassphrase(first), 0u);
    CHECK(PumpUntil({&h}, 3000, [&] { return h.lobby.server_state() == kSrvComplete; }));
    const u64 t9 = platform::NowMs();
    CHECK_EQ(h.Total(), 1u);  // state 9: the guest of the Download Play application is awaited
    CHECK_EQ(h.ClientExt(2), 0x09u);
    CHECK(PumpUntil({&h}, 2000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
    m->StopSession();
    // the jump to the child, as the plan says
    std::unique_ptr<RoomConsole> child;
    u32 out_seen_ms = 0;
    if (how == 0) {
        bridge->room.Stop();  // the Download Play application ends: its plugin leaves the room
        bridge->target = nullptr;
        CHECK(PumpUntil({&h}, 4000, [&] { return !h.e().InRoom(model_mac); }));
        out_seen_ms = static_cast<u32>(platform::NowMs() - t9);
        CHECK_EQ(h.Total(), 1u);
        // the child starts and its plugin joins, while the host's network thread goes on (it steps the wait)
        bool child_in = false;
        {
            std::atomic<bool> done{false};
            std::thread t([&] {
                Sleep(500);
                child.reset(new RoomConsole(host, port, "AzDlplay"));
                child->config.game_name = "Luigi's Mansion";
                child->config.game_id = kChildOfTheGame;
                child->config.preferred_mac = model_mac;  // (the plugin of the child joins with the console's own MAC)
                child_in = child->Join();
                done = true;
            });
            while (!done) {
                h.Pump();
                Sleep(2);
            }
            t.join();
        }
        CHECK(child_in);
        CHECK(child->room.mac() == model_mac);
    } else if (how == 1) {
        bridge->room.UpdateGameInfo("Luigi's Mansion", kChildOfTheGame);
    } else {
        bridge->room.UpdateGameInfo("", 0);
        CHECK(PumpUntil({&h}, 4000, [&] { return !h.e().InRoom(model_mac); }));
        out_seen_ms = static_cast<u32>(platform::NowMs() - t9);
        PumpUntil({&h}, 1000, [] { return false; });
        bridge->room.UpdateGameInfo("Luigi's Mansion", kChildOfTheGame);
    }
    CHECK(PumpUntil({&h}, 17000, [&] { return h.Total() == 0; }));
    const u32 waited = static_cast<u32>(platform::NowMs() - t9);
    dlp_real::ChildWait::End end = {};
    CHECK(h.net.TakeChildWaitEnd(&end));
    std::printf("   state 9 with the guest awaited: out of the room seen at +%u ms, the game reads 0 at +%u ms (the wait: %u ms, %u back, %u stayed, %u not back)\n",
                out_seen_ms, waited, static_cast<unsigned>(end.waited_ms), static_cast<unsigned>(end.back), static_cast<unsigned>(end.stayed),
                static_cast<unsigned>(end.missing));
    if (how == 1) {
        CHECK_EQ(end.stayed, 1u);  // an Azahar never leaves the room: it holds the host 5 s, no more
        CHECK(waited >= 4700 && waited <= 6000);
    } else {
        CHECK_EQ(end.back, 1u);  // seen out, then back: the end with it
        CHECK(waited < 8000);
    }
    CHECK_EQ(end.missing, 0u);
    CHECK_EQ(h.net.ChildrenAwaited(), 0u);
    h.lobby.ServerFinalize();
    CHECK(Clean(h, "host"));
    CHECK_EQ(h.net.counters().bad, 0u);
    for (const std::string& s : m->violations()) {
        std::printf("   model violation: %s\n", s.c_str());
    }
    CHECK(m->violations().empty());
    if (child) {
        child->room.Stop();
    }
    bridge->room.Stop();
    bridge->target = nullptr;
    m.reset();
    CHECK_RULES();
}

static void TestI4(const std::string& host, u16 port) {
    RunI4(host, port, 0);
    RunI4(host, port, 1);
    RunI4(host, port, 2);
}

// ---- I5 (1.1.4, route B, spec 8 6.5): the 3DS Link app's page, then the prepared child's plugin, over the real room ---------------------------
static constexpr u64 kIslandGame = 0x00040000000F8200ULL;
static constexpr u64 kIslandChild = 0x00040001000F8200ULL;
static constexpr u64 kIslandPrepared = 0x000400000F8F8200ULL;  // (the user's formula, written out)
static constexpr u16 kIslandVersion = 1040;
static constexpr u32 kChildHoldMs = 8000;  // (Sources/ctr/plugin.cpp's kChildHoldIdMs: what the prepared child gives HoldIdentity)

/// Island Tour's child Ready in the app's table, through the app's own BuildTable (the index text, its record, AM and the plugin there, a block
/// whose sizes are 0: any host's).
static std::unique_ptr<dlpjoin::Children> IslandChildren(std::vector<u64>* missing) {
    namespace dc = azl::dlp_children;
    CHECK(dlpguest::Register(kIslandPrepared, dlpguest::kFormApplication));
    dc::Row r{};
    r.child = kIslandChild;
    r.game = kIslandGame;
    r.prepared = kIslandPrepared;
    r.version = kIslandVersion;
    r.where = 'S';
    char line[dc::kRowBytes + 1];
    CHECK(dc::FormatRow(r, line) != 0);
    const std::string text = std::string(dc::kIndexHeader) + "\n" + line;
    dlpguest::Guest g;
    g.game = kIslandGame;
    g.child = kIslandChild;
    g.prepared = kIslandPrepared;
    g.child_version = kIslandVersion;
    g.state = dlpguest::GuestState::Installed;
    std::unique_ptr<dlpjoin::Children> ch(new dlpjoin::Children);
    CHECK(dlpjoin::BuildTable(
        text.data(), text.size(), {g},
        [](u64, u16* v) {
            *v = kIslandVersion;
            return dlpguest::Presence::Present;
        },
        [](u64) { return true; },
        [](u64, u8* out) {
            std::memset(out, 0, dc::kBlockBytes);
            return true;
        },
        ch.get(), missing));
    CHECK_EQ(dlpjoin::ReadyCount(ch->table), 1u);
    return ch;
}

static std::string Hex12(const Mac& m) {
    char s[16];
    std::snprintf(s, sizeof(s), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return s;
}

/// The page of the app as Session::Start wires it (dlp_join.cpp): the engine on the room, the Core ticked by the room's thread after the engine,
/// the room's lists to the engine, the identity of D-B4. Its Leave is Session::Stop's: LeaveRounds, then RoomClient::Stop (the polite disconnect).
struct AppPage {
    AppPage(const std::string& host, u16 port, const char* nick, const Mac& console) : console_(console) {
        children = IslandChildren(&missing);
        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (!room.joined()) {
                return;
            }
            p.transmitter_address = room.mac();
            g_rules.Inspect(p);
            room.SendWifiPacket(p);
        };
        transport.get_mac = [this] { return room.joined() ? room.mac() : console_; };
        engine.reset(new UdsEngine(&kernel, transport));
        std::u16string n;
        for (const char* p = nick; *p != '\0'; ++p) {
            n.push_back(static_cast<char16_t>(*p));
        }
        block = ::operator new(sizeof(dlpjoin::Core));
        core = dlpjoin::MakeCore(block, sizeof(dlpjoin::Core), engine.get(), console_, n, &children->table, missing);
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetWifiPrefilter([this](const u8* message, std::size_t size) { return engine->TryDiscardEncoded(message, size); });
        room.SetTickCallback([this](u64 now_ms) {
            engine->Tick(now_ms);
            core->Step(now_ms);
        });
        room.SetRoomInfoCallback([this](const room::RoomInfo& info) {
            std::vector<Mac> plays;
            for (const room::MemberInfo& m : info.members) {
                if (RoomMemberPlays(m.game_name, m.game_id)) {
                    plays.push_back(m.mac);
                }
            }
            engine->OnRoomMembers(plays);
        });
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.preferred_mac = console_;  // (D-B4: the console's own MAC - the child comes back with it)
        config.console_id_hash = "3ds-" + Hex12(console_) + "-dlp";
        config.game_name = "Download Play (3DS Link)";
        config.game_id = title_rules::kAppTid;
    }
    ~AppPage() {
        room.Stop();
        dlpjoin::EndCore(core);
        ::operator delete(block);
    }
    bool Join() {
        room.Start(config);
        const bool joined = WaitFor(8000, [this] {
            return room.state() == RoomClient::State::Joined || (room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None);
        }) && room.joined();
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    /// Session::Stop: the two rounds after the release, then the room left politely. True when the rounds came.
    bool Leave() {
        const bool rounds = dlpjoin::LeaveRounds(*core, &platform::NowMs, &platform::SleepMs);
        room.Stop();
        return rounds;
    }
    Mac console_;
    std::unique_ptr<dlpjoin::Children> children;
    std::vector<u64> missing;
    RoomClient::Config config;
    RoomClient room;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
    void* block = nullptr;
    dlpjoin::Core* core = nullptr;
};

/// The prepared child's plugin in the room: Plugin::Connect's identity (EffectiveNickname with its suffix rule, "3ds-<mac>", the console's MAC or
/// none) driven by RejoinPolicy as Plugin::Tick drives it, HoldIdentity on as EarlyInit sets it for the prepared child.
struct PluginChild {
    PluginChild(const std::string& host, u16 port, const char* nick, const Mac& console, u64 launch_ms)
        : host_(host), port_(port), nick_(nick), console_(console), launch_ms_(launch_ms) {
        policy.HoldIdentity(kChildHoldMs);
    }
    ~PluginChild() {
        room.Stop();
    }
    /// One call of Plugin::Tick's part that joins the room.
    void Tick(u64 now) {
        if (room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None && errors.size() < attempt_ms.size()) {
            errors.push_back(room.error());  // (how the last attempt ended)
        }
        if (room.joined() && joined_ms == 0) {
            joined_ms = now;
            joined_attempt = static_cast<u32>(attempt_ms.size());
            policy.NoteJoined();
        }
        RejoinPolicy::Input in;
        in.want_connected = true;
        in.state = room.state();
        in.error = room.error();
        in.now_ms = now;
        in.launch_ms = launch_ms_;
        in.nickname_automatic = true;  // (the console's name: the case where the policy may suffix it)
        const RejoinPolicy::Step step = policy.Update(in);
        alive = alive && step.alive;
        held += step.note == RejoinPolicy::Note::HeldIdentity;
        changed += step.note == RejoinPolicy::Note::MacTaken || step.note == RejoinPolicy::Note::NameTaken;
        if (step.clear_error) {
            room.ClearError();
        }
        if (step.connect) {
            RoomClient::Config rc;
            rc.host = host_;
            rc.port = port_;
            rc.nickname = Nickname();
            rc.preferred_mac = policy.prefer_real_mac() ? console_ : kNoPreferredMac;
            rc.console_id_hash = "3ds-" + Hex12(console_);
            rc.game_name = "Island Tour";
            rc.game_id = kIslandPrepared;
            attempt_ms.push_back(now);
            room.Start(rc);
        }
    }
    /// Plugin::EffectiveNickname for a console's own name.
    std::string Nickname() const {
        std::string name = nick_;
        if (policy.nickname_suffixed()) {
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "-%02X%02X", console_[4], console_[5]);
            name += suffix;
        }
        return name;
    }
    std::string host_;
    u16 port_;
    std::string nick_;
    Mac console_;
    u64 launch_ms_;
    RejoinPolicy policy;
    RoomClient room;
    std::vector<u64> attempt_ms;            // when each attempt began
    std::vector<RoomClient::Error> errors;  // how each refused attempt ended
    u64 joined_ms = 0;
    u32 joined_attempt = 0;  // 1-based
    u32 held = 0, changed = 0;
    bool alive = true;
};

static s32 g_ns_rc = 0;
static u8 g_ns_bytes[16];
static s32 FakeNsS(const u8 (&info)[16]) {  // (ns:s SetWirelessRebootInfo: the console's; here the 16 bytes are kept for the check)
    std::memcpy(g_ns_bytes, info, sizeof(g_ns_bytes));
    return g_ns_rc;
}

/// I5: `lingers` false = I5a (the child 500 ms after the app's Stop), true = I5b (the app leaves 3 s after the child began to try).
static void RunI5(const std::string& host, u16 port, bool lingers) {
    std::printf("I5%s: the 3DS Link app's page joins our host, hands over and leaves; the prepared child's plugin comes in %s\n", lingers ? "b" : "a",
                lingers ? "while the room still lists the app" : "500 ms after the app's Stop");
    g_rules.Reset();
    const char* const nick = lingers ? "PageB5b" : "PageB5a";  // (the console's nickname: the app's and the child's)
    const Mac console = {0x00, 0x1F, 0x32, 0xB5, 0x00, static_cast<u8>(lingers ? 0x0B : 0x0A)};  // (a made-up console MAC)
    // (what the host's room thread writes: declared before the host, so they outlive its thread)
    std::mutex mm;
    std::vector<room::MemberInfo> members;
    u32 wrong_identity = 0;  // members with the console's nickname suffixed, or the prepared title under another MAC
    // our host of Island Tour, H5's wait on, its room's lists as Plugin::OnRoomInfo gives them (AwaitsChildOf: the plugin's own)
    RoomConsole hc(host, port, lingers ? "IslandHostB" : "IslandHostA");
    hc.config.game_name = "Island Tour";
    hc.config.game_id = kIslandGame;
    RealDlpNet hn(hc.engine.get());
    DlpLobby hl(&hn, Player::Cfg(0x000F8200, 1, "Daisy", 4));
    hn.SetIdentity(Me(0xF001, "Daisy"));
    hn.SetPlayers(4);
    hn.SetChildWait(true);  // (what MakeRealDlpHub gives a game that hosts the real Download Play)
    UdsEngine* const he = hc.engine.get();
    hc.room.SetRoomInfoCallback([&, he](const room::RoomInfo& info) {
        std::vector<Mac> plays, dlp;
        for (const room::MemberInfo& m : info.members) {
            if (RoomMemberPlays(m.game_name, m.game_id)) {
                plays.push_back(m.mac);
            }
            if (azl::ctr::AwaitsChildOf(m.game_id)) {
                dlp.push_back(m.mac);
            }
        }
        he->OnRoomMembers(plays);
        he->SetDownloadPlayMembers(dlp);
        std::lock_guard<std::mutex> l(mm);
        members = info.members;
        for (const room::MemberInfo& m : info.members) {
            wrong_identity += (m.nickname != nick && m.nickname.rfind(std::string(nick) + "-", 0) == 0) ||
                              (m.game_id == kIslandPrepared && m.mac != console);
        }
    });
    auto listed = [&](const Mac& mac, u64 game_id, const char* name) {
        std::lock_guard<std::mutex> l(mm);
        for (const room::MemberInfo& m : members) {
            if (m.mac == mac && m.game_id == game_id && m.nickname == name) {
                return true;
            }
        }
        return false;
    };
    auto app_listed = [&] {
        std::lock_guard<std::mutex> l(mm);
        for (const room::MemberInfo& m : members) {
            if (m.game_id == title_rules::kAppTid) {
                return true;
            }
        }
        return false;
    };
    std::unique_ptr<AppPage> app(new AppPage(host, port, nick, console));
    const bool in_room = hc.Join() && app->Join();
    CHECK(in_room);  // (a room that cannot be reached proves nothing: a failure)
    if (!in_room) {
        return;
    }
    CHECK(app->room.mac() == console);  // (D-B4: the app is in the room with the console's own MAC)
    std::mutex hm;
    std::atomic<bool> hstop{false};
    std::thread hthread([&] {
        while (!hstop) {
            {
                std::lock_guard<std::mutex> l(hm);
                hn.Pump();
                hl.Pump();
            }
            Sleep(3);
        }
    });
    auto host_total = [&] {
        std::lock_guard<std::mutex> l(hm);
        u8 ids[8];
        u32 total = 0;
        hl.ServerConnectingClients(ids, 8, &total);
        return total;
    };
    // the room's lists reach the host: the page (kAppTid) is one the host waits for
    CHECK(WaitFor(4000, [&] { return he->InRoom(console) && he->InDownloadPlay(console) && listed(console, title_rules::kAppTid, nick); }));
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerInitialize(3, 0, nullptr), 0u);
        CHECK_EQ(hl.ServerStartAccepting(true, 6), 0u);
    }
    // the page lists the host as Join, the user presses A
    const u64 t_scan = platform::NowMs();
    const bool row = WaitFor(8000, [&] {
        for (const dlpjoin::Host& h : app->core->Hosts()) {
            if (h.g.host == hc.room.mac() && dlpjoin::ShowOf(h) == dlpjoin::Show::Join && h.prepared == kIslandPrepared) {
                return true;
            }
        }
        return false;
    });
    CHECK(row);
    const u32 row_ms = static_cast<u32>(platform::NowMs() - t_scan);
    app->core->AskJoin(hc.room.mac());
    CHECK(WaitFor(6000, [&] {
        std::lock_guard<std::mutex> l(hm);
        u32 w = 0;
        return hl.ServerClientState(2, &w) == 0 && (w >> 24) == kPeerWaiting && hn.PeerState(2) == RealDlpNet::kAuthed;
    }));
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerAcceptClient(2), 0u);
        CHECK_EQ(hl.ServerStartDistribution(), 0u);
    }
    CHECK(WaitFor(4000, [&] {
        std::lock_guard<std::mutex> l(hm);
        return hl.server_state() == kSrvPassphrase;
    }));
    u8 nine[9];
    Pass(1, nine);
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerSendPassphrase(nine), 0u);
    }
    CHECK(WaitFor(4000, [&] {
        std::lock_guard<std::mutex> l(hm);
        return hl.server_state() == kSrvComplete;
    }));
    const u64 t9 = platform::NowMs();
    CHECK(WaitFor(3000, [&] { return (app->core->Word() >> 24) == kCliComplete; }));
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hn.ChildrenAwaited(), 1u);  // state 9: the page's member is awaited (AwaitsChildOf(kAppTid))
    }
    // the hand-over (ns:s faked): the host's MAC, the FIRST passphrase, [15] = 0, the prepared title of the row
    g_ns_rc = 0;
    std::memset(g_ns_bytes, 0xFF, sizeof(g_ns_bytes));
    s32 rc = -1;
    const u64 pass_ms = app->core->passphrase_ms();
    const u64 jump = dlpjoin::HandOver(*app->core, &FakeNsS, &rc);
    CHECK_EQ(rc, 0);
    CHECK_EQ(jump, kIslandPrepared);
    const Mac host_mac = hc.room.mac();
    CHECK(std::memcmp(g_ns_bytes, host_mac.data(), 6) == 0 && std::memcmp(g_ns_bytes + 6, nine, 9) == 0 && g_ns_bytes[15] == 0);
    std::memset(g_ns_bytes, 0, sizeof(g_ns_bytes));
    std::unique_ptr<PluginChild> child;
    u64 app_gone_ms = 0, stop_ms = 0, gone_seen_ms = 0;
    bool rounds = false, ack = false;
    auto leave = [&] {
        stop_ms = platform::NowMs();
        rounds = app->Leave();
        ack = app->room.LeaveAcknowledged();
        app_gone_ms = platform::NowMs();
    };
    if (!lingers) {
        leave();
        CHECK(WaitFor(3000, [&] { return !app_listed(); }));
        gone_seen_ms = platform::NowMs();
        while (platform::NowMs() < stop_ms + 500) {
            Sleep(5);
        }
        child.reset(new PluginChild(host, port, nick, console, platform::NowMs()));
    } else {
        Sleep(200);
        child.reset(new PluginChild(host, port, nick, console, platform::NowMs()));
    }
    const u64 launch = platform::NowMs();
    for (;;) {
        const u64 now = platform::NowMs();
        child->Tick(now);
        if (lingers && stop_ms == 0 && now >= launch + 3000) {
            CHECK(child->joined_ms == 0);  // (the app is still listed: the child is not in yet)
            leave();
        }
        if (lingers && stop_ms != 0 && gone_seen_ms == 0 && !app_listed()) {
            gone_seen_ms = platform::NowMs();
        }
        if (child->joined_ms != 0 || !child->alive || now > launch + kChildHoldMs + 4000) {
            break;
        }
        Sleep(10);
    }
    CHECK(rounds);  // (D-B10: two rounds of the network thread after the release, before the room was left)
    CHECK(ack);     // (the room answered the polite disconnect: LeaveAcknowledged)
    CHECK(child->alive);
    CHECK(child->joined_ms != 0);
    CHECK(child->room.mac() == console);  // the console's own MAC, never a room-assigned one
    CHECK(child->policy.prefer_real_mac());
    CHECK(!child->policy.nickname_suffixed());  // the console's name, never the suffix
    CHECK_EQ(child->changed, 0u);
    CHECK(WaitFor(3000, [&] { return listed(console, kIslandPrepared, nick); }));  // (the room lists the child as the app was: MAC and name)
    if (!lingers) {
        CHECK_EQ(child->joined_attempt, 1u);  // no collision: in at its first attempt
        CHECK(child->errors.empty());
        CHECK_EQ(child->held, 0u);
    } else {
        CHECK(child->joined_attempt >= 2u);
        CHECK(!child->errors.empty());
        u32 collisions = 0;
        for (RoomClient::Error e : child->errors) {
            collisions += e == RoomClient::Error::NameCollision || e == RoomClient::Error::MacCollision;
        }
        CHECK_EQ(collisions, static_cast<u32>(child->errors.size()));  // refused for the app still listed (the name first: the room's order)
        CHECK_EQ(child->held, 1u);  // HeldIdentity said once
        if (child->joined_attempt >= 2) {
            // in at its first attempt after the app is gone, within the hold
            CHECK(child->attempt_ms[child->joined_attempt - 1] >= app_gone_ms);
            CHECK(child->attempt_ms[child->joined_attempt - 2] < app_gone_ms);
            CHECK(child->attempt_ms[child->joined_attempt - 1] - launch < kChildHoldMs);
        }
    }
    {
        std::lock_guard<std::mutex> l(mm);
        CHECK_EQ(wrong_identity, 0u);
    }
    // the host's wait: the app seen out and its child back
    CHECK(WaitFor(dlp_real::ChildWait::kMaxMs + 2000, [&] { return host_total() == 0; }));
    const u32 waited = static_cast<u32>(platform::NowMs() - t9);
    dlp_real::ChildWait::End end = {};
    CHECK(WaitFor(500, [&] {
        std::lock_guard<std::mutex> l(hm);
        return hn.TakeChildWaitEnd(&end);
    }));
    const u32 exit_after_pass = pass_ms != 0 && app_gone_ms > pass_ms ? static_cast<u32>(app_gone_ms - pass_ms) : 0;
    std::printf("   the host listed as Join after %u ms; from the passphrase to the room exit %u ms; Stop %u ms (rounds %d, the room answered %d); "
                "the app gone from the host's list %u ms after Stop; the child in at attempt %u (%zu refused, held %u), %u ms after its launch; "
                "the host's game reads 0 at +%u ms (back %u, stayed %u, missing %u)\n",
                row_ms, exit_after_pass, static_cast<u32>(app_gone_ms - stop_ms), rounds, ack,
                gone_seen_ms != 0 ? static_cast<u32>(gone_seen_ms - stop_ms) : 0, child->joined_attempt, child->errors.size(), child->held,
                static_cast<u32>(child->joined_ms - launch), waited, static_cast<unsigned>(end.back), static_cast<unsigned>(end.stayed),
                static_cast<unsigned>(end.missing));
    CHECK(exit_after_pass != 0 && exit_after_pass < dlp_real::ChildWait::kLeaveMs);  // (D-B1.6: out before the host takes the guest for an Azahar)
    CHECK(gone_seen_ms != 0);
    CHECK_EQ(end.back, 1u);  // route B: the page seen out, its child back with the same MAC: the end with it
    CHECK_EQ(end.stayed, 0u);
    CHECK_EQ(end.missing, 0u);
    CHECK(waited < dlp_real::ChildWait::kLeaveMs + 4000);
    CHECK_EQ(hn.counters().content, 0u);  // (the content is never asked)
    hstop = true;
    hthread.join();
    hl.ServerFinalize();
    CHECK_EQ(hn.counters().bad, 0u);
    child.reset();
    app.reset();
    hc.room.Stop();  // (its thread ends before the adapter and the lobby go)
    CHECK_RULES();
}

static void TestI5(const std::string& host, u16 port) {
    RunI5(host, port, false);
    RunI5(host, port, true);
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
    TestI4(host, port);
    TestI5(host, port);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
