// The SEARCHER of the real Download Play (1.1.3, phase F4 of analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md): our DlpLobby
// on the adapter RealDlpNet (azl/dlp_real_net.hpp), as the client of a game of that mode, in front of a model of Azahar's Download Play
// HOST (Tests/azahar_dlp_host.hpp, Azahar's dlp_srvr.cpp transcribed, the capture's bytes). Same rig as Tests/dlp_real_net_test.cpp
// (engines joined by the in-memory room of Tests/mock_room.hpp, real time, the Azahar-peer rule checker on every frame). The client's game
// is Luigi's Mansion (2018) as NETCODE.md reads its code: InitializeWithName with 2 beacons, StartScan with the filter of its child,
// GetTitleInfoInOrder until it finds one, StopScan, StartFakeSession, GetMyStatus until 9, then GetWirelessRebootPassphrase (0x0F) and
// Finalize (0x02) a millisecond later ON THE GAME'S THREAD (no network thread: the engine must be given back there). The host's game on
// Azahar accepts by hand 1.6 s after the client is in, starts the distribution when the client is accepted, and calls BeginGame every
// 33 ms with a NEW passphrase while its state is 8 (Azahar answers -1 "Already sent passphrase" after the first).
//
// Scenarios (design 7.3, with the corrections of NETCODE.md):
//   R2   client + Azahar host (two seeds: its id8, network id and attribute word random): the game's list has unique id 0x001D1A within
//        2.5 s of StartScan (the probe read the 5 frames; the engine given back after it, the filter off); the join within 3 s;
//        GetMyStatus 5 -> 6 -> 9 (never 7: our lobby's values, NETCODE point 1); the passphrase is the FIRST BeginGame's; the Auth answered
//        before StartDistribution whatever the lobby's state (point 5); the filter on during the session; Finalize on the game thread gives
//        the engine back whole (filter off)
//   R2c  as R2, the host ends its network 1 ms after the final, with no Deauthentication, and 170 ms later beacons its game's network from
//        the same MAC: the lobby stays 9 with the passphrase; the engine's table of beacons forgets the host and the session is not touched;
//        no ghost in the game's network; a clean Release at Finalize
//   R5b  losses with an Azahar host, which sends StartDistribution and the final ONCE: the first lost -> the client stays 5 (the host waits
//        in NeedsDistributeAck), the second lost -> the client stays 6 with no passphrase (the host has ended its network): the state is
//        printed, nothing crashes, StopSession / Finalize give the engine back whole
//   R6   two Azahar hosts of Luigi (2 and 4 nodes) and one of another game in the room, all announcing all the time on channel 1 as node 1:
//        each probe reads its own host only (the max_nodes of each in the list; the other game not listed: ScanAbsorb), the foreign
//        frames dropped by the filter, each host probed ONCE per scan (not again at every round) and again at a new StartScan (the game
//        scans again every 10 s: NETCODE point 7); a session played to 9 while the others go on announcing
//   R5c  frames of the announcement lost on their way to the probe: one group short (the probe closes with a later group), every frame 3
//        for 2 s (the probe gives up at kProbeCollectMs with a part, the host is probed again at once and listed)
//   R6f  a host listed by a second searcher fills up (its beacon total == max): it leaves that list
//   R9c  a host whose announcement has a malformed frame, and one that announces nothing (EndHosting): ignored for the rest of the scan
//        (probed once), never listed
//   R12c a host that never answers the spectator's connection: tried again after kProbeRetryMs, never listed, never ignored
//   R3a  our client in front of OUR host (the adapter of phase F3, no hub): the two lobbies to 9 with the first passphrase (F5 repeats it
//        through the hub, R3)
//   Rel  StartScan "busy" while a game has nwm::UDS (nothing taken); the engine given back whole (not held, not initialised, the filter
//        off) when the game stops the scan, or its nwm::UDS starts,
//        in the middle of a probe and of a session (design R8, R10 on the client's side)
//
//   RC1-RC7 (1.1.4, G1): the system's Download Play application as the searcher, through dlp:CLNT (below, before main).
//
//   1.1.4, route B (spec 8 D-B5): the plugin never lets the Download Play application join (clnt::kSystemAppJoins false). This file is built twice:
//   dlp_real_client_test with -DAZL_CLNT_SYSTEM_JOINS=1 runs everything as before (G1's join path: RC1, RC3, RC4, RC8 join with StartSession), and
//   dlp_real_client_test_default (the plugin's build) runs RC1b (the ready host of RC1 listed "OK" and refused, no frame of the client), RC2, RC5, RC6.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_client_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o <the core sources of Tests/run.sh>
// (the final review of 1.1.4: built with no -D - dlp_real_client_test_default - this binary stands for the plugin's build; it must be route B's)
#ifndef AZL_CLNT_SYSTEM_JOINS
#define AZL_TEST_PLUGIN_DEFAULT 1
#endif
#include "dlp_net_rig.hpp"

#include <algorithm>

#include "azahar_dlp_host.hpp"
#include "azl/dlp_ipc.hpp"  // (1.1.4, G1: DlpService and the book of dlp:CLNT, the scenarios RC1-RC8)
#include "azl/dlp_real_net.hpp"
#include "azl/uds_frames.hpp"
#include "dlp_real_clnt_golden.inc"    // (1.1.4, RC8: the four answers of the capture's Download Play application, Tests/make_dlp_real_clnt_goldens.py)
#include "dlp_real_island_golden.inc"  // (1.1.4, RC8: the host's side of the same capture, what the model of Azahar's host sends)
#ifdef AZL_TEST_PLUGIN_DEFAULT
static_assert(!azl::clnt::kSystemAppJoins, "the plugin's build: the system's Download Play application never joins (route B, K10 / D-B5)");
#endif

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kLuigiLow = 0x001D1A00;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;
static constexpr u64 kOtherChild = 0x0004000100123400ULL;

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

/// The searcher: a console, the adapter, the lobby on it (the game's tid = Luigi's).
struct RClient {
    RClient(MockRoom& room, u8 last, const char* name)
        : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(kLuigiLow, last, name)) {
        net.SetIdentity(Me(0xF000u + last, name));
    }
    UdsEngine& e() {
        return *con.engine;
    }
    /// The network thread's turn: the adapter first, then the lobby (as the hub will do it, design D10). Notes the status shown.
    void Pump() {
        net.Pump();
        lobby.Pump();
        Note();
    }
    u32 Status() const {
        return lobby.ClientStatusWord() >> 24;
    }
    void Note() {
        const u32 s = Status();
        if (trace.empty() || trace.back() != s) {
            trace.push_back(s);
        }
    }
    std::string Trace() const {
        std::string t;
        for (u32 s : trace) {
            t += (t.empty() ? "" : " ") + std::to_string(s);
        }
        return t;
    }
    Console con;
    RealDlpNet net;
    DlpLobby lobby;
    std::vector<u32> trace;
};

template <typename F>
static bool PumpUntil(RClient& c, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        c.Pump();
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(2);
    }
}

/// The engine is nobody's: not held, not initialised, no probe, the client's foreign-lobby filter off (decision Q8: off on every path).
static void CheckClean(RClient& c, const char* where) {
    const bool ok = !c.net.Holds() && !c.e().initialized() && !c.e().client_foreign_filter_enforced() && !c.net.Probing();
    if (!ok) {
        std::printf("   not clean %s: held %d, initialised %d, filter %d, probing %d\n", where, c.net.Holds(), c.e().initialized(),
                    c.e().client_foreign_filter_enforced(), c.net.Probing());
    }
    CHECK(ok);
    UdsEngine::UnbindInfo info;
    CHECK_EQ(c.e().Unbind(RealDlpNet::kSessionBind, &info), 0u);  // (no such bind any more: Shutdown freed it)
    CHECK_EQ(info.channel, 0u);
    CHECK_EQ(c.e().Unbind(RealDlpNet::kProbeBind, &info), 0u);
    CHECK_EQ(info.channel, 0u);
}

static void CheckHost(AzaharDlpHost& h) {
    for (const std::string& v : h.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(h.violations().empty());
    CHECK_EQ(h.size_skipped.load(), 0u);
}

/// The game on the Azahar host (Luigi's Mansion on an emulator): AcceptClient 1.6 s after the client is in, StartDistribution once the
/// client is accepted, BeginGame every 33 ms with a new passphrase while the state is 8 (the first is Pass(1)).
class HostGame {
public:
    explicit HostGame(AzaharDlpHost& h, bool start = true) : h_(h), start_(start), t_([this] { Run(); }) {}
    ~HostGame() {
        stop_ = true;
        t_.join();
    }
    std::atomic<bool> accepted{false}, distributing{false};
    std::atomic<u32> begin_ok{0}, begin_refused{0};

private:
    bool Wait(u32 ms) {
        const u64 end = platform::NowMs() + ms;
        while (!stop_ && platform::NowMs() < end) {
            Sleep(2);
        }
        return !stop_;
    }
    void Run() {
        while (!stop_ && h_.client_state(2) == AzaharDlpHost::NotJoined) {
            Sleep(2);
        }
        if (!Wait(1600) || h_.AcceptClient(2) != 0) {
            return;
        }
        accepted = true;
        while (!stop_ && h_.client_state(2) != AzaharDlpHost::Accepted) {
            Sleep(2);
        }
        if (stop_ || !start_) {
            return;
        }
        h_.StartDistribution();
        distributing = true;
        while (!stop_ && h_.state() != AzaharDlpHost::kNeedToSendPassphrase) {
            Sleep(2);
        }
        const u64 t0 = platform::NowMs();
        for (u8 k = 1; !stop_ && h_.state() == AzaharDlpHost::kNeedToSendPassphrase && !h_.ended() && platform::NowMs() - t0 < 4000; ++k) {
            u8 nine[9];
            Pass(k, nine);
            (h_.BeginGame(nine) == 0 ? begin_ok : begin_refused)++;
            Sleep(33);
        }
    }
    AzaharDlpHost& h_;
    bool start_;
    std::atomic<bool> stop_{false};
    std::thread t_;
};

/// Luigi's searcher up to the host in its list: ms from StartScan (0xFFFFFFFF = not found within `ms`).
static u32 Find(RClient& c, const Mac& host, u32 ms = 4000) {
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    const u64 t0 = platform::NowMs();
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    c.Note();
    GroupView v;
    if (!PumpUntil(c, ms, [&] { return c.lobby.ClientTitleInfo(host, &v) == 0; })) {
        return 0xFFFFFFFFu;
    }
    return static_cast<u32>(platform::NowMs() - t0);
}

/// StopScan + StartFakeSession: the result, *ms the time of the join.
static u32 JoinHost(RClient& c, const Mac& host, u32* ms) {
    c.lobby.ClientStopScan();
    u32 r = 0xFFFFFFFF;
    const u64 t0 = platform::NowMs();
    if (c.lobby.ClientJoinBegin(host, &r) == JoinStep::Pending) {
        PumpUntil(c, 5000, [&] { return c.lobby.ClientJoinPoll(&r); });
    }
    *ms = static_cast<u32>(platform::NowMs() - t0);
    c.Note();
    return r;
}

/// The statuses after the join: 5, 6, 9 in this order, never 7 (NETCODE point 1: our lobby's values; the game waits for the 9 only).
static bool SessionTrace(const RClient& c) {
    std::vector<u32> after;
    bool in = false;
    for (u32 s : c.trace) {
        in |= s == 5;
        if (in) {
            after.push_back(s);
        }
    }
    return after == std::vector<u32>({5, 6, 9});
}

/// The end of the client's game: 0x0F and 0x02 on the game's thread, no Pump.
static void GameEnds(RClient& c, const u8* want_pass) {
    u8 pass[9];
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    if (want_pass != nullptr) {
        CHECK(std::memcmp(pass, want_pass, 9) == 0);
    }
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CheckClean(c, "after Finalize");
}

// ---------------------------------------------------------------------------------------------------------------------

static void R2_Sequence(u32 seed, bool silent_end) {
    std::printf("%s: Luigi's searcher and an Azahar host (seed %u)\n", silent_end ? "R2c" : "R2", seed);
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, seed);
    RClient c(room, 2, "Client");
    std::atomic<u32> on6{0}, elsewhere{0};  // (the frames the client sends to the host: on the channel of its beacon, 6 - NETCODE point 4)
    c.con.sniff = [&](const WifiPacket& w) {
        if (w.destination_address == h.mac()) {
            (w.channel == 6 ? on6 : elsewhere)++;
        }
    };
    Ticker ticker({&h.e(), &c.e()});
    if (silent_end) {
        h.destroy_after_final_ms = 1;
        h.game_beacon_after_ms = 170;
    }
    h.StartHosting(true, 6);
    std::printf("   host: id8 %02x, network id %08x, attributes %04x (random: nothing of them is read)\n", h.id8, h.network_id, h.attributes);
    // the list
    const u32 found = Find(c, h.mac());
    std::printf("   in the list %u ms after StartScan (%u probe, %u frames' groups sent by the host)\n", found, c.net.counters().probes,
                h.groups.load());
    CHECK(found <= 2500);
    GroupView v;
    CHECK_EQ(c.lobby.ClientTitleInfoInOrder(true, &v), 0u);
    CHECK(v.group->host == h.mac());
    CHECK_EQ(v.group->child_tid_low >> 8, 0x1D1Au);  // (the unique id GetTitleInfoInOrder gives the game)
    CHECK_EQ(v.group->max_nodes, 2u);
    CHECK_EQ(v.group->total_nodes, 1u);
    CHECK_EQ(Str(v.group->name), std::string("AzHost"));
    CHECK_EQ(c.lobby.ClientTitleInfoInOrder(false, &v), static_cast<u32>(R_NO_DATA));
    CHECK_EQ(c.Status(), 2u);
    CHECK(PumpUntil(c, 500, [&] { return !c.net.Probing(); }));
    CHECK(!c.net.Holds());  // (the probe took the engine for itself only)
    CHECK(!c.e().client_foreign_filter_enforced());
    CHECK_EQ(c.net.counters().probes, 1u);
    CHECK_EQ(c.net.HeardState(h.mac()), RealDlpNet::kHeardVerified);
    // the join, the host's game, the session
    HostGame game(h);
    u32 join_ms = 0;
    CHECK_EQ(JoinHost(c, h.mac(), &join_ms), 0u);
    std::printf("   joined in %u ms\n", join_ms);
    CHECK(join_ms <= 3000);
    CHECK_EQ(c.Status(), 5u);
    CHECK(c.e().client_foreign_filter_enforced());
    CHECK(PumpUntil(c, 3000, [&] { return game.accepted.load(); }));
    CHECK(PumpUntil(c, 1000, [&] { return h.client_state(2) == AzaharDlpHost::Accepted || game.distributing; }));  // (the Auth answered at once, in 5)
    const u64 t_acc = platform::NowMs();
    CHECK(PumpUntil(c, 4000, [&] { return c.Status() == 9; }));
    std::printf("   statuses %s; accepted -> 9 in %u ms; host: A1 x%u, A3 x%u, A5 x%u, A7 x%u, A8 x%u; BeginGame 0 x%u, -1 x%u\n", c.Trace().c_str(),
                static_cast<u32>(platform::NowMs() - t_acc), h.sent("A1"), h.sent("A3"), h.sent("A5"), h.sent("A7"), h.sent("A8"),
                game.begin_ok.load(), game.begin_refused.load());
    CHECK(SessionTrace(c));
    CHECK_EQ(h.sent("A3"), 1u);
    CHECK_EQ(h.sent("A7"), 1u);
    CHECK(PumpUntil(c, 1500, [&] { return h.ended(); }));
    CHECK_EQ(game.begin_ok.load(), 1u);
    CHECK(h.received(2) >= 1 && h.received(3) == 1 && h.received(5) >= 1);
    u8 first[9];
    Pass(1, first);
    if (silent_end) {
        // the host is silent, then beacons its game's network from the same MAC
        PumpUntil(c, 600, [] { return false; });
        CHECK_EQ(c.Status(), 9u);
        CHECK(c.e().status() == NetworkStatus::ConnectedAsClient);  // (the session is not touched by the table of beacons)
        UdsEngine::HeardLobby heard;
        bool listed = false;
        for (u32 i = 0; c.e().HeardLobbyAt(i, 5000, &heard); ++i) {
            listed |= heard.host == h.mac();
        }
        CHECK(!listed);  // (its beacon is its game's now: it is no lobby)
        u16 me = 0, mask = 0;
        CHECK(h.e().NodeSnapshot(&me, &mask));
        CHECK_EQ(mask, 1u);  // (no ghost in the game's network: the searcher never joined it)
        u8 ids[4];
        u32 total = 0;
        CHECK_EQ(c.lobby.ClientConnectingNodes(ids, 4, &total), 0u);
        CHECK_EQ(total, 2u);  // (the roster the GO froze: the host and us)
        CHECK_EQ(h.received(6), 0u);  // (the host ended at the final: no notice was answered)
    }
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_EQ(c.net.counters().probes, 1u);
    CHECK(on6 > 0);
    CHECK_EQ(elsewhere.load(), 0u);
    GameEnds(c, first);
    CheckHost(h);
    CHECK_RULES();
}

static void R5b_Losses(bool lose_final) {
    std::printf("R5b: an Azahar host and the %s lost on its way\n", lose_final ? "final" : "StartDistribution");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 3);
    RClient c(room, 2, "Client");
    std::atomic<u32> lost{0};
    c.con.drop = [&](const WifiPacket& w) {
        EtherType t;
        SecureDataHeader sh;
        if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData || !ParseSecureDataHeader(w.data, &sh) ||
            sh.data_channel != 2 || sh.ActualDataSize() < 16 || sh.ActualDataSize() > w.data.size()) {
            return false;
        }
        const u8* p = w.data.data() + (w.data.size() - sh.ActualDataSize());
        const bool hit = lose_final ? (p[0] == 6 && p[12] == 1) : p[0] == 3;
        if (hit) {
            ++lost;
        }
        return hit;
    };
    Ticker ticker({&h.e(), &c.e()});
    h.StartHosting(true, 6);
    CHECK(Find(c, h.mac()) <= 2500);
    HostGame game(h);
    u32 join_ms = 0;
    CHECK_EQ(JoinHost(c, h.mac(), &join_ms), 0u);
    if (!lose_final) {
        CHECK(PumpUntil(c, 4000, [&] { return game.distributing.load(); }));
        PumpUntil(c, 3000, [] { return false; });
        std::printf("   3 s after StartDistribution: client %u (statuses %s), host %u with the client in %u; %u lost\n", c.Status(), c.Trace().c_str(),
                    h.state(), h.client_state(2), lost.load());
        CHECK_EQ(c.Status(), 5u);  // (no START: Azahar sends it once)
        CHECK_EQ(h.client_state(2), static_cast<u8>(AzaharDlpHost::NeedsDistributeAck));
        CHECK_EQ(c.lobby.ClientStopSession(), 0u);  // (the game gives up)
        CheckClean(c, "after StopSession");
        CHECK(PumpUntil(c, 1500, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; }));  // (it saw us go)
        CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    } else {
        CHECK(PumpUntil(c, 6000, [&] { return h.ended(); }));
        PumpUntil(c, 2500, [] { return false; });
        std::printf("   2.5 s after the host's end: client %u (statuses %s), %u lost, %u notices answered\n", c.Status(), c.Trace().c_str(), lost.load(),
                    h.received(6));
        CHECK_EQ(c.Status(), 6u);  // (started, never complete: the host sent the final once and is gone without a word)
        u8 pass[9];
        CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
        CHECK_EQ(pass[0], 0u);  // (no passphrase)
        CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    }
    CHECK_EQ(lost.load(), 1u);
    CheckClean(c, "after Finalize");
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CheckHost(h);
    CHECK_RULES();
}

/// A host that fills up while a second searcher lists it: its beacon says total == max, the list drops it (a full host cannot be chosen).
static void R6f_Full() {
    std::printf("R6f: a host listed by a second searcher fills up\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 51);
    RClient c(room, 2, "Client");
    RClient c2(room, 3, "Other");
    Ticker ticker({&h.e(), &c.e(), &c2.e()});
    h.StartHosting(false, 6);
    GroupView v;
    for (RClient* s : {&c, &c2}) {
        CHECK_EQ(s->lobby.ClientInitialize(nullptr, 2), 0u);
        CHECK_EQ(s->lobby.ClientStartScan(kLuigiChild), 0u);
    }
    CHECK(PumpUntil(c2, 4000, [&] {  // (both pumped: an entry nobody refreshes would expire by itself and prove nothing)
        c.Pump();
        return c.lobby.ClientTitleInfo(h.mac(), &v) == 0 && c2.lobby.ClientTitleInfo(h.mac(), &v) == 0;
    }));
    PumpUntil(c2, 300, [&] {
        c.Pump();
        return false;
    });
    CHECK_EQ(c2.lobby.ClientTitleInfo(h.mac(), &v), 0u);  // (still listed while the host has room)
    u32 join_ms = 0;
    CHECK_EQ(JoinHost(c, h.mac(), &join_ms), 0u);
    const u64 t0 = platform::NowMs();
    CHECK(PumpUntil(c2, 2500, [&] {
        c.Pump();
        return c2.lobby.ClientTitleInfo(h.mac(), &v) != 0;
    }));
    std::printf("   gone from the other searcher's list %u ms after the join\n", static_cast<u32>(platform::NowMs() - t0));
    c2.lobby.ClientStopScan();
    CheckClean(c2, "the other searcher after StopScan");
    CHECK_EQ(c2.lobby.ClientFinalize(), 0u);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CheckClean(c, "after Finalize");
    CheckHost(h);
    CHECK_RULES();
}

static void R6_Three() {
    std::printf("R6: two Azahar hosts of Luigi and one of another game in the room, all announcing\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h1(room, 1, "Luigi2", kLuigiChild, 1, 11);
    AzaharDlpHost h2(room, 3, "Luigi4", kLuigiChild, 3, 12);
    AzaharDlpHost h3(room, 4, "Other", kOtherChild, 1, 13);
    RClient c(room, 2, "Client");
    Ticker ticker({&h1.e(), &h2.e(), &h3.e(), &c.e()});
    h1.StartHosting(true, 6);
    h2.StartHosting(true, 11);
    h3.StartHosting(true, 1);
    for (int round = 1; round <= 2; ++round) {
        const u32 probes0 = c.net.counters().probes;
        CHECK_EQ(c.lobby.ClientInitialize(nullptr, 4), 0u);
        CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
        CHECK(PumpUntil(c, 6000, [&] {
            return c.net.HeardState(h1.mac()) == RealDlpNet::kHeardVerified && c.net.HeardState(h2.mac()) == RealDlpNet::kHeardVerified &&
                   c.net.HeardState(h3.mac()) == RealDlpNet::kHeardVerified;
        }));
        PumpUntil(c, 2000, [] { return false; });  // (and nothing is probed again)
        std::printf("   round %d: %u probes\n", round, c.net.counters().probes - probes0);
        CHECK_EQ(c.net.counters().probes - probes0, 3u);
        GroupView v;
        CHECK_EQ(c.lobby.ClientTitleInfo(h1.mac(), &v), 0u);
        CHECK_EQ(v.group->max_nodes, 2u);
        CHECK_EQ(Str(v.group->name), std::string("Luigi2"));
        CHECK_EQ(c.lobby.ClientTitleInfo(h2.mac(), &v), 0u);
        CHECK_EQ(v.group->max_nodes, 4u);
        CHECK_EQ(Str(v.group->name), std::string("Luigi4"));
        CHECK_EQ(c.lobby.ClientTitleInfo(h3.mac(), &v), static_cast<u32>(R_NO_DATA));  // (another game: probed once, not listed)
        CHECK(!c.net.Holds());
        c.lobby.ClientStopScan();
        CheckClean(c, "after StopScan");
        CHECK_EQ(c.net.HeardState(h3.mac()), RealDlpNet::kHeardNone);  // (the cache goes with the scan)
    }
    // a session with the first host while the two others go on announcing
    HostGame game(h1);
    u32 join_ms = 0;
    CHECK_EQ(JoinHost(c, h1.mac(), &join_ms), 0u);
    CHECK(PumpUntil(c, 6000, [&] { return c.Status() == 9; }));
    std::printf("   session: statuses %s\n", c.Trace().c_str());
    u8 first[9];
    Pass(1, first);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    GameEnds(c, first);
    CheckHost(h1);
    CheckHost(h2);
    CheckHost(h3);
    CHECK_RULES();
}

/// Frames of the announcement lost on their way to the probe: the frame 3 of the first two groups (the probe closes with a later group),
/// or every frame 3 for 2 s (the probe gives up at kProbeCollectMs with a part of the mask, the host is probed again at once and read).
static void R5c_ProbeLosses(bool long_loss) {
    std::printf("R5c: frames of the announcement lost (%s)\n", long_loss ? "every frame 3 for 2 s" : "the frame 3 of two groups");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 41);
    RClient c(room, 2, "Client");
    std::atomic<u32> lost{0};
    std::atomic<u64> until{0};
    c.con.drop = [&](const WifiPacket& w) {
        EtherType t;
        SecureDataHeader sh;
        if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData || !ParseSecureDataHeader(w.data, &sh) ||
            sh.data_channel != 1 || sh.ActualDataSize() < 16 || sh.ActualDataSize() > w.data.size()) {
            return false;
        }
        const u8* p = w.data.data() + (w.data.size() - sh.ActualDataSize());
        const bool hit = p[12] == 3 && (long_loss ? platform::NowMs() < until.load() : lost.load() < 2);  // (the first group may come before the bind)
        if (hit) {
            ++lost;
        }
        return hit;
    };
    Ticker ticker({&h.e(), &c.e()});
    h.StartHosting(true, 6);
    until = platform::NowMs() + 2000;
    const u32 found = Find(c, h.mac());
    std::printf("   in the list after %u ms, %u probes, %u frames lost\n", found, c.net.counters().probes, lost.load());
    CHECK(found <= (long_loss ? 4000u : 2000u));
    CHECK_EQ(c.net.counters().probes, long_loss ? 2u : 1u);
    CHECK(lost.load() >= 1);
    CHECK_EQ(c.net.counters().ignored_hosts, 0u);
    c.lobby.ClientStopScan();
    CheckClean(c, "after StopScan");
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CheckHost(h);
    CHECK_RULES();
}

/// A host whose announcement is malformed (a frame's size field is not its length), one that announces nothing (EndHosting: Azahar stops
/// the announcement and keeps the network), and one that never answers a connection.
static void R9c_R12c() {
    std::printf("R9c/R12c: a malformed announcement, a host that announces nothing, a host that does not answer\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost bad(room, 1, "Bad", kLuigiChild, 1, 21);
    AzaharDlpHost quiet(room, 3, "Quiet", kLuigiChild, 1, 22);
    RClient c(room, 2, "Client");
    Console mute(room, 5);  // its beacon is heard, its node answers nothing
    Ticker ticker({&bad.e(), &quiet.e(), &c.e()});
    bad.corrupt_frame = 2;
    bad.StartHosting(true, 6);
    quiet.StartHosting(true, 6);
    quiet.EndHosting();  // (before its first group: 1 ms after StartHosting)
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    const u64 ts = platform::NowMs();
    CHECK(PumpUntil(c, 5000, [&] {
        return c.net.HeardState(bad.mac()) == RealDlpNet::kHeardIgnored && c.net.HeardState(quiet.mac()) == RealDlpNet::kHeardIgnored;
    }));
    std::printf("   both ignored after %u ms, %u probes\n", static_cast<u32>(platform::NowMs() - ts), c.net.counters().probes);
    CHECK_EQ(c.net.counters().probes, 2u);
    CHECK_EQ(c.net.counters().ignored_hosts, 2u);
    // the silent host: a beacon of the Download Play's kind every 100 ms, from a member whose engine is never up
    const u32 probes0 = c.net.counters().probes;
    const u64 t0 = platform::NowMs();
    u64 next = t0;
    u32 retried_at = 0;
    while (platform::NowMs() - t0 < 9000) {
        if (platform::NowMs() >= next) {
            static const u8 kNone[1] = {0};  // (no application data; MakeBeacon's memcpy wants a pointer)
            WifiPacket b = MakeBeacon(mute.mac, 0x2810, 0x11, 1, 2, kNone, 0);
            g_rules.Inspect(b);  // (the rule checker sees the beacons its consoles send: this one is put in the room by hand)
            room.Send(mute.index, b);
            next += 100;
        }
        c.Pump();
        if (retried_at == 0 && c.net.counters().probes >= probes0 + 2) {
            retried_at = static_cast<u32>(platform::NowMs() - t0);
        }
        Sleep(2);
    }
    std::printf("   the silent host: %u probes in 9 s (at 0 and again at %u ms); ignored %u in all\n", c.net.counters().probes - probes0, retried_at,
                c.net.counters().ignored_hosts);
    CHECK_EQ(c.net.counters().ignored_hosts, 2u);  // (the silent host is never ignored: tried again)
    CHECK_EQ(c.net.counters().verified, 0u);
    CHECK_EQ(c.net.counters().probes - probes0, 2u);  // (3 s, then again 5 s later; the two ignored ones never again)
    CHECK(retried_at >= RealDlpNet::kProbeConnectMs + RealDlpNet::kProbeRetryMs - 200);
    CHECK_EQ(c.net.HeardState(bad.mac()), RealDlpNet::kHeardIgnored);
    CHECK_EQ(c.net.HeardState(quiet.mac()), RealDlpNet::kHeardIgnored);
    GroupView v;
    CHECK_EQ(c.lobby.ClientTitleInfoInOrder(true, &v), static_cast<u32>(R_NO_DATA));
    c.lobby.ClientStopScan();
    CheckClean(c, "after StopScan");
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK_RULES();
}

/// Our client in front of our host (phase F3's adapter under its lobby, Luigi's host as the console log shows it).
static void R3a_Ours() {
    std::printf("R3a: our searcher and our host, both of the real Download Play\n");
    g_rules.Reset();
    MockRoom room;
    Console hc(room, 1);
    RealDlpNet hn(hc.engine.get());
    DlpLobby hl(&hn, Player::Cfg(kLuigiLow, 1, "Host"));
    RClient c(room, 2, "Client");
    Ticker ticker({hc.engine.get(), &c.e()});
    CHECK_EQ(hl.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(hl.ServerStartAccepting(true, 6), 0u);
    auto pump = [&] {
        hn.Pump();
        hl.Pump();
    };
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    const u64 t0 = platform::NowMs();
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    GroupView v;
    CHECK(PumpUntil(c, 4000, [&] {
        pump();
        return c.lobby.ClientTitleInfo(hc.mac, &v) == 0;
    }));
    std::printf("   in the list after %u ms\n", static_cast<u32>(platform::NowMs() - t0));
    CHECK_EQ(Str(v.group->name), std::string("Host"));
    CHECK_EQ(v.group->child_tid_low, kLuigiLow);
    CHECK_EQ(v.group->max_nodes, 2u);
    c.lobby.ClientStopScan();
    u32 r = 0xFFFFFFFF;
    CHECK(c.lobby.ClientJoinBegin(hc.mac, &r) == JoinStep::Pending);
    CHECK(PumpUntil(c, 4000, [&] {
        pump();
        return c.lobby.ClientJoinPoll(&r);
    }));
    CHECK_EQ(r, 0u);
    u32 w = 0;
    CHECK(PumpUntil(c, 3000, [&] {
        pump();
        return hl.ServerClientState(2, &w) == 0 && (w >> 24) == kPeerWaiting && hn.PeerState(2) == RealDlpNet::kAuthed;
    }));
    CHECK_EQ(hl.ServerAcceptClient(2), 0u);
    CHECK_EQ(hl.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(c, 3000, [&] {
        pump();
        return hl.server_state() == kSrvPassphrase;
    }));
    u8 k = 1;
    u64 next = 0;
    CHECK(PumpUntil(c, 3000, [&] {
        pump();
        if (hl.server_state() == kSrvPassphrase && platform::NowMs() >= next) {
            u8 nine[9];
            Pass(k++, nine);
            hl.ServerSendPassphrase(nine);
            next = platform::NowMs() + 33;
        }
        return hl.server_state() == kSrvComplete && c.Status() == 9;
    }));
    std::printf("   statuses %s; host %u\n", c.Trace().c_str(), hl.server_state());
    CHECK(SessionTrace(c));
    u8 first[9];
    Pass(1, first);
    CHECK(!hn.Holds());
    GameEnds(c, first);
    hl.ServerFinalize();
    CHECK_EQ(hn.counters().bad, 0u);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_RULES();
}

/// The engine given back whole in the middle of a probe and of a session, on the game's thread.
static void Rel_Paths() {
    std::printf("Rel: StopScan / OnGameUdsStarting / StopSession in the middle of a probe and of a session\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 31);
    RClient c(room, 2, "Client");
    Ticker ticker({&h.e(), &c.e()});
    h.StartHosting(false, 6);  // (automatic acceptance: the session goes on by itself up to the client's 5)
    // a game's own session up: StartScan is "busy" and nothing of it is touched (the adapter takes no engine a game has)
    u32 ev = 0;
    CHECK_EQ(c.e().Initialize(MakeNode(0xC1, "Game", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), static_cast<u32>(R_BUSY));
    PumpUntil(c, 100, [] { return false; });
    CHECK(!c.net.Holds());
    CHECK_EQ(c.net.counters().probes, 0u);
    CHECK(c.e().initialized());
    CHECK_EQ(c.e().Shutdown(), 0u);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    for (int how = 0; how < 4; ++how) {
        CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
        CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
        CHECK(PumpUntil(c, 3000, [&] { return c.net.Probing() && c.net.Holds(); }));
        CHECK(c.e().client_foreign_filter_enforced());
        if (how % 2 == 1) {
            PumpUntil(c, 40, [] { return false; });  // (into the collection: the host's next group is up to 500 ms away)
        }
        if (how < 2) {
            CHECK_EQ(c.lobby.ClientStopScan(), 0u);
        } else {
            CHECK(c.lobby.OnGameUdsStarting());
        }
        CheckClean(c, how < 2 ? "after StopScan in a probe" : "after OnGameUdsStarting in a probe");
        c.Pump();  // (no probe starts again: the lobby closed its scan)
        CHECK(!c.net.Holds());
        CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    }
    for (int how = 0; how < 2; ++how) {
        CHECK(Find(c, h.mac()) <= 2500);
        u32 join_ms = 0;
        CHECK_EQ(JoinHost(c, h.mac(), &join_ms), 0u);
        CHECK(PumpUntil(c, 1000, [&] { return h.client_state(2) == AzaharDlpHost::Accepted; }));
        CHECK(c.e().client_foreign_filter_enforced());
        // (the model's radio is our engine: as the host of a network that is no Download Play's of ours, it swallows a goodbye that comes
        //  within 3 s of a node's new authentication - the dialect's deauth_guard for Azahar's guests, who say goodbye twice; Azahar's nwm
        //  has no such guard. The probe's goodbye and the client's authentication are milliseconds apart here: the client stays past it.)
        PumpUntil(c, 3200, [] { return false; });
        if (how == 0) {
            CHECK(c.lobby.OnGameUdsStarting());
        } else {
            CHECK_EQ(c.lobby.ClientStopSession(), 0u);
        }
        CheckClean(c, how == 0 ? "after OnGameUdsStarting in a session" : "after StopSession");
        CHECK_EQ(c.lobby.ClientFinalize(), 0u);
        CHECK(PumpUntil(c, 1500, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; }));
    }
    // the game-thread calls of the adapter allocate nothing (the lobby's commands while scanning)
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    CHECK(PumpUntil(c, 3000, [&] { return c.net.HeardState(h.mac()) == RealDlpNet::kHeardVerified; }));
    {
        CountNews news;
        HeardGroup hg;
        CHECK(c.net.ScanRead(0, &hg));
        CHECK(c.net.ScanOpen());
        u8 nodes[kMaxPlayers];
        c.net.Members(nodes);
        u8 from = 0, msg[16];
        u32 size = 0;
        CHECK(!c.net.Receive(&from, msg, sizeof(msg), &size));
        CHECK_EQ(news.count(), 0u);
    }
    c.lobby.ClientStopScan();
    CheckClean(c, "at the end");
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CheckHost(h);
    CHECK_RULES();
}

/// RV3 (review of 1.1.3, integration points 2-3): a host whose beacon says it is full is never probed - it answers no spectator (Azahar's
/// even asserts on a late EAPoL-Start), and a probe would hold the engine 3 s every 5 s - nor filed in the cache; a place frees, the
/// next round probes it.
static void RV3_FullNotProbed() {
    std::printf("RV3: a full host is not probed\n");
    g_rules.Reset();
    MockRoom room;
    RClient c(room, 2, "Client");
    Console mute(room, 5);  // its beacon is heard, its node answers nothing
    Ticker ticker({&c.e()});
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    auto beaconing = [&](u8 total, u32 ms) {
        const u64 t0 = platform::NowMs();
        u64 next = t0;
        while (platform::NowMs() - t0 < ms) {
            if (platform::NowMs() >= next) {
                static const u8 kNone[1] = {0};
                WifiPacket b = MakeBeacon(mute.mac, 0x2810, 0x11, total, 2, kNone, 0);
                g_rules.Inspect(b);
                room.Send(mute.index, b);
                next += 100;
            }
            c.Pump();
            Sleep(2);
        }
    };
    beaconing(2, 1500);
    CHECK_EQ(c.net.counters().probes, 0u);
    CHECK(!c.net.Probing());
    CHECK(!c.net.Holds());
    CHECK_EQ(c.net.HeardState(mute.mac), RealDlpNet::kHeardNone);
    beaconing(1, 400);  // (a place frees: probed at once)
    CHECK_EQ(c.net.counters().probes, 1u);
    CHECK(c.net.Probing());
    c.lobby.ClientStopScan();
    CheckClean(c, "after StopScan");
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK_RULES();
}

/// RV4 (review of 1.1.3, test point 3): the host's StartDistribution and its final both in hand before the lobby reads (here the lobby is
/// not pumped at all: the adapter answers the host by itself): Receive gives the START first, then the GO with the first passphrase.
static void RV4_StartBeforeGo() {
    std::printf("RV4: StartDistribution and the final read together: START, then GO\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 4);
    RClient c(room, 2, "Client");
    Ticker ticker({&h.e(), &c.e()});
    h.StartHosting(true, 6);
    CHECK(Find(c, h.mac()) <= 2500);
    HostGame game(h);
    u32 join_ms = 0;
    CHECK_EQ(JoinHost(c, h.mac(), &join_ms), 0u);
    const u64 t0 = platform::NowMs();
    while (!h.ended() && platform::NowMs() - t0 < 6000) {
        c.net.Pump();  // (the adapter only)
        Sleep(2);
    }
    CHECK(h.ended());
    for (u32 i = 0; i < 50; ++i) {
        c.net.Pump();
        Sleep(2);
    }
    u8 msg[64];
    u8 from = 0;
    u32 size = 0;
    CHECK(c.net.Receive(&from, msg, sizeof(msg), &size));
    CHECK_EQ(msg[0], static_cast<u8>(MsgType::Start));
    CHECK(c.net.Receive(&from, msg, sizeof(msg), &size));
    CHECK_EQ(msg[0], static_cast<u8>(MsgType::Go));
    CHECK(!c.net.Receive(&from, msg, sizeof(msg), &size));
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CheckClean(c, "after Finalize");
    CheckHost(h);
    CHECK_RULES();
}


// ---------------------------------------------------------------------------------------------------------------------
// 1.1.4 (G1 of analysis/piano-1.1.4-2026-11-15/specifiche/1-G1.md section 4.4): the SYSTEM's Download Play application as the searcher. The same
// adapter and the same lobby, the lobby of ANY game (LobbyConfig::any_title, eight players), the book of dlp:CLNT fed by the adapter's probes, and
// DlpService driven with the raw words of dlp:CLNT in the order the application sends them (RAPPORTO section 1). The network thread's turn is
// net.Pump(); lobby.Pump(). The children the app prepared are a table given to the adapter (K3), with a ready row for Island Tour's child.
//   RC1  an Azahar host of Mario Party: Island Tour (child 00040001000F8200), its child ready here: listed within 2.5 s with "<name> OK", StopScan,
//        PrepareForSystemDownload 0, StartSession joins within 3 s, the state 5 all through a long wait (12 s; 105 s with AZL_LONG=1, the capture
//        296.9 -> 402.1), then 7 (the distribution shown as "nothing to download") and 9 with the FIRST passphrase, the seam to G3 armed with the
//        row's prepared title and the passphrase ready, StopSession, GetServerInfo still the same, Finalize, the engine clean
//   RC2  two scans: the host delivered once in each, then "no data", never 0xE0A0AC01
//   RC3  the host ends while the application waits in its lobby: 0x80 within kHostGoneMs + 500 ms; GetServerInfo later the same record
//   RC4  HOME in 5 (ClntLeave): 0x80 at once, the host sees the node leave before any StartDistribution
//   RC5  a child that is not prepared: StartSession 0 and 0x80, no client connection at all (the host never sees a node)
//   RC6  a host of another game (Luigi's Mansion) beside it: listed too (any game), with "NO"
//   RC7  four hosts for five scans: every host delivered in every scan, the book never refuses one it knows
//   RC8  the four ANSWERS of the application (A2 the AuthAck, A4 the answer to StartDistribution without content, A6 to the Finish, A9 to the notice)
//        against the capture's (Tests/dlp_real_clnt_golden.inc, specifiche/1-G1.md 4.4), the model of Azahar's host sending the capture's own packets
//        (Tests/dlp_real_island_golden.inc): every packet ours sends on channel 2 is one of the four, byte for byte but for the host's sequence and id
//        (+0x0D..0F, which ours echoes from the host's packet: the capture's host sent A3 / A5 / A8 with the sequence 0 / 1 / 2 and an id nobody set,
//        the model with the client's own) and +0x16..0x19 of A6 (a member Azahar's client never initialises; ours is zero), sealed with the seed 0

static constexpr u64 kIslandChild = 0x00040001000F8200ULL;
static constexpr u64 kIslandGame = 0x00040000000F8200ULL;
static constexpr u64 kIslandPrepared = 0x000400000F8F8200ULL;  // (the user's formula, written out: 0x0F800000 | 0x000F8200)

static u32 ClW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 ClW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}

/// The children of these scenarios: Island Tour's child ready (no block sizes: whatever the host announces, it is this version) - or with the
/// verdict `v`.
struct ClntKids {
    dlp_children::Table table;
    u8 block[dlp_children::kBlockBytes] = {};
    explicit ClntKids(dlp_children::Verdict v = dlp_children::Verdict::Ready) {
        dlp_children::Clear(&table);
        dlp_children::Row& r = table.index.rows[0];
        r.child = kIslandChild;
        r.game = kIslandGame;
        r.prepared = kIslandPrepared;
        r.where = 'S';
        table.index.count = 1;
        table.index_found = true;
        table.verdict[0] = static_cast<u8>(v);
        dlp_children::PutBlock(&table, 0, block);
    }
};

/// The Download Play application: a console, the adapter with the book, the lobby of any game, the service; two buffers of the "game" for the
/// records (GetServerInfo's and GetTitleInfoInOrder's static buffer 0).
struct ClntApp {
    static constexpr u32 kSrvAddr = 0x08000000, kTitleAddr = 0x08100000;
    ClntApp(MockRoom& room, u8 last, const dlp_children::Table* kids)
        : con(room, last), net(con.engine.get()), lobby(&net, Cfg(last)) {
        net.SetIdentity(Me(0xD000u + last, "DlpApp"));
        net.SetPlayers(8);
        net.SetClntBook(&book);
        net.SetChildren(kids);
        svc.SetLobby(&lobby);
    }
    static LobbyConfig Cfg(u8 last) {
        LobbyConfig c = Player::Cfg(0x00022100, last, "DlpApp", 8);
        c.any_title = 1;
        return c;
    }
    UdsEngine& e() {
        return *con.engine;
    }
    static u8* Map(void* ctx, u32 a, u32 n) {
        ClntApp* s = static_cast<ClntApp*>(ctx);
        if (a == kSrvAddr && n <= sizeof(s->srv)) {
            return s->srv;
        }
        if (a == kTitleAddr && n <= sizeof(s->title)) {
            return s->title;
        }
        return nullptr;
    }
    DlpOutcome Call(u32 header, std::initializer_list<u32> args, u32 addr = 0, u32 cap = 0) {
        for (u32 i = 0; i < 32; ++i) {
            w[i] = 0xC0DE0000u + i;
        }
        w[0] = header;
        u32 i = 1;
        for (const u32 a : args) {
            w[i++] = a;
        }
        u32 sb[2] = {(cap << 14) | 2u, addr};
        const IpcResolver r = {this, &Map};
        const DlpOutcome out = svc.Dispatch(DlpKind::Clnt, w, addr != 0 ? sb : nullptr, r);
        CHECK(out == DlpOutcome::JoinPending || (w[0] >> 16) == (header >> 16) || w[0] == 0x40u);
        return out;
    }
    u32 Init() { Call(0x000100C3, {0x232000, 16, 0x200000, 0x04000000, 0x111, 0x222}); return w[1]; }
    u32 Finalize() { Call(0x00020000, {}); return w[1]; }
    u32 Scan() { Call(0x00050180, {1, 0, 0, 0, 0, 0}); return w[1]; }
    u32 StopScan() { Call(0x00060000, {}); return w[1]; }
    u32 ServerInfo(const Mac& m) { Call(0x00070080, {ClW1(m), ClW2(m)}, kSrvAddr, sizeof(srv)); return w[1]; }
    u32 InOrder() { Call(0x00090040, {0}, kTitleAddr, sizeof(title)); return w[1]; }
    u32 Prepare(const Mac& m) { Call(0x000B0100, {ClW1(m), ClW2(m), 0x000F8200, 0x00040001}); return w[1]; }
    u32 Shown() { Call(0x000E0000, {}); return w[2] >> 24; }
    u32 StopSession() { Call(0x00120000, {}); return w[1]; }
    u32 Pass(u8 out[9]) {
        Call(0x00110000, {});
        std::memcpy(out, &w[2], 9);
        return w[1];
    }
    /// StartSession, and PollPending (the hub's join wait) with the network thread's turns until it ends: its result, *pending whether it began.
    u32 Start(const Mac& m, bool* pending) {
        *pending = Call(0x000D0100, {ClW1(m), ClW2(m), 0x000F8200, 0x00040001}) == DlpOutcome::JoinPending;
        if (!*pending) {
            return w[1];
        }
        const u64 end = platform::NowMs() + 5000;
        while (!svc.PollPending(w) && platform::NowMs() < end) {
            Pump();
            Sleep(2);
        }
        CHECK_EQ(w[0], 0x000D0040u);
        return w[1];
    }
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    /// The small line of the last GetServerInfo, as text.
    std::string Line() const {
        std::string s;
        for (u32 i = 0; i < kNameUnits; ++i) {
            const u16 u = LoadLE16(srv + 0x1C + 2 * i);
            if (u != 0) {
                s += static_cast<char>(u < 0x80 ? u : '?');
            }
        }
        return s;
    }
    Console con;
    RealDlpNet net;
    DlpClntBook book;
    DlpLobby lobby;
    DlpService svc;
    u32 w[32];
    u8 srv[0x298] = {};
    u8 title[0x13A8] = {};
};

template <typename F>
static bool PumpApp(ClntApp& a, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        a.Pump();
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(2);
    }
}

static void ClntClean(ClntApp& a, const char* where) {
    const bool ok = !a.net.Holds() && !a.e().initialized() && !a.e().client_foreign_filter_enforced() && !a.net.Probing();
    if (!ok) {
        std::printf("   not clean %s\n", where);
    }
    CHECK(ok);
}

/// The application up to `host` in its list: Initialize, StartScan, GetTitleInfoInOrder until it comes (ms, or 0xFFFFFFFF).
static u32 ClntFind(ClntApp& a, const Mac& host, u32 ms = 4000) {
    CHECK_EQ(a.Init(), 0u);
    const u64 t0 = platform::NowMs();
    CHECK_EQ(a.Scan(), 0u);
    bool found = false;
    PumpApp(a, ms, [&] {
        const u32 r = a.InOrder();
        CHECK(r != R_INVALID_STATE);
        found = found || (r == 0 && std::memcmp(a.title + 8, host.data(), 6) == 0);
        return found;
    });
    return found ? static_cast<u32>(platform::NowMs() - t0) : 0xFFFFFFFFu;
}

static u32 LongWaitMs() {
    const char* l = std::getenv("AZL_LONG");
    return l != nullptr && l[0] == '1' ? 105000u : 12000u;
}

static void RC1_IslandTour() {
    std::printf("RC1: the Download Play application and an Azahar host of Mario Party: Island Tour, its child prepared here\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 11);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(true, 6);
    const u32 found = ClntFind(a, h.mac());
    std::printf("   in the list %u ms after StartScan\n", found);
    CHECK(found <= 2500);
    CHECK_EQ(LoadLE32(a.title + 0x00), 0x000F82u);   // the unique id of the child
    CHECK_EQ(LoadLE32(a.title + 0x04), 0u);
    const DlpClntBook::Title* t = a.book.TitleOf(h.mac());
    CHECK(t != nullptr);  // (the probe's frame 0: the sizes and the name the host announces)
    if (t != nullptr) {
        std::printf("   frame 0: sizes 0x%08x / 0x%08x\n", t->transfer, t->required);
    }
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    std::printf("   the line under the host's name: '%s'\n", a.Line().c_str());
    CHECK(a.Line() == "Gionko OK");
    CHECK_EQ(a.srv[8], 4u);
    CHECK_EQ(a.srv[9], 1u);
    PumpApp(a, 3000, [] { return false; });
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Prepare(h.mac()), 0u);
    HostGame game(h, false);  // (the host's game accepts the client, 1.6 s after it is in, and starts nothing)
    const u64 tj = platform::NowMs();
    bool pending = false;
    CHECK_EQ(a.Start(h.mac(), &pending), 0u);
    CHECK(pending);
    std::printf("   joined in %u ms\n", static_cast<u32>(platform::NowMs() - tj));
    CHECK(platform::NowMs() - tj <= 3000);
    CHECK_EQ(a.Shown(), 5u);
    CHECK_EQ(a.book.armed_prepared(), kIslandPrepared);  // the seam to G3: the row's prepared title
    CHECK(!a.book.passphrase_ready());
    // the long wait in the lobby (the host's player chooses), state 5 all along
    const u32 wait = LongWaitMs();
    bool always5 = true;
    const u64 tw = platform::NowMs();
    while (platform::NowMs() - tw < wait) {
        a.Pump();
        if (a.Shown() != 5) {
            always5 = false;
        }
        Sleep(20);
    }
    std::printf("   %u s in the lobby: always 5 = %d\n", wait / 1000, always5);
    CHECK(always5);
    CHECK(game.accepted.load());
    // the host starts: 7 (shown for 6), then 9 with the first passphrase
    CHECK(PumpApp(a, 2000, [&] { return h.client_state(2) == AzaharDlpHost::Accepted; }));
    h.StartDistribution();
    bool saw7 = false;
    u8 first[9];
    Pass(1, first);
    bool sent = false;
    CHECK(PumpApp(a, 6000, [&] {
        const u32 s = a.Shown();
        saw7 = saw7 || s == 7;
        if (!sent && h.state() == AzaharDlpHost::kNeedToSendPassphrase) {
            sent = h.BeginGame(first) == 0;
        }
        return s == 9;
    }));
    CHECK(saw7);
    CHECK(a.book.passphrase_ready());
    u8 pass[9];
    CHECK_EQ(a.Pass(pass), 0u);
    CHECK_EQ(a.w[0], 0x00110100u);
    CHECK(std::memcmp(pass, first, 9) == 0);
    u8 before[0x298];
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    std::memcpy(before, a.srv, sizeof(before));
    CHECK_EQ(a.StopSession(), 0u);
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    CHECK(std::memcmp(before, a.srv, 0x290) == 0);  // the same record after StopSession, but for the tick
    CHECK_EQ(a.book.armed_prepared(), kIslandPrepared);  // (after the passphrase the seam stays armed until Finalize: the jump is after it)
    CHECK_EQ(a.Finalize(), 0u);
    CHECK_EQ(a.book.armed_prepared(), 0u);
    CHECK_EQ(a.net.counters().host_bad, 0u);
    ClntClean(a, "after Finalize");
    CheckHost(h);
    CHECK_RULES();
}

/// RC1b (route B): RC1's host and its ready child, as the plugin builds dlp:CLNT: listed within 2.5 s with "<name> OK" and title_long's tail
/// " - join it from the 3DS Link app"; StartSession answers 0 at once (no JoinPending), 0x80 next, the refusal noted as Ready, the seam never
/// armed, no probe and no connection of the client: the host never sees a node (no A1 sent), and again after a new scan.
static void RC1b_ReadyRefused() {
    std::printf("RC1b: route B - the Download Play application and an Azahar host of Island Tour, its child ready here: listed OK, refused\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 16);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(true, 6);
    const u32 found = ClntFind(a, h.mac());
    std::printf("   in the list %u ms after StartScan\n", found);
    CHECK(found <= 2500);
    const char* tail = " - join it from the 3DS Link app";
    const u32 at = 10 + 6;  // ("Hosted by " and "Gionko")
    for (u32 i = 0; tail[i] != '\0'; ++i) {
        CHECK_EQ(LoadLE16(a.title + 0xA0 + 2 * (at + i)), static_cast<u16>(tail[i]));
    }
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    std::printf("   the line under the host's name: '%s'\n", a.Line().c_str());
    CHECK(a.Line() == "Gionko OK");
    for (int attempt = 0; attempt < 2; ++attempt) {
        CHECK_EQ(a.StopScan(), 0u);
        CHECK_EQ(a.Prepare(h.mac()), 0u);
        PumpApp(a, 300, [&] { return !a.net.Probing(); });
        const u16 probes = a.net.counters().probes;
        bool pending = true;
        CHECK_EQ(a.Start(h.mac(), &pending), 0u);
        CHECK(!pending);
        CHECK_EQ(a.w[0], 0x000D0040u);
        CHECK_EQ(a.Shown(), 0x80u);
        CHECK_EQ(a.book.refusals(), static_cast<u32>(attempt + 1));
        CHECK(a.book.refused_verdict() == dlp_children::Verdict::Ready);
        CHECK_EQ(a.book.armed_prepared(), 0u);
        CHECK(!a.book.passphrase_ready());
        PumpApp(a, 1500, [] { return false; });
        CHECK_EQ(h.client_state(2), static_cast<u8>(AzaharDlpHost::NotJoined));  // the host never saw a client
        CHECK_EQ(h.sent("A1"), 0u);
        CHECK_EQ(a.net.counters().probes, probes);
        CHECK(!a.net.Holds());
        CHECK_EQ(a.StopSession(), 0u);
        CHECK_EQ(a.Shown(), 1u);
        CHECK_EQ(a.Scan(), 0u);  // (the application scans again and the user tries once more)
        CHECK(PumpApp(a, 3000, [&] { return a.InOrder() == 0; }));
    }
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    ClntClean(a, "after the refusals");
    CheckHost(h);
    CHECK_RULES();
}

static void RC2_TwoScans() {
    std::printf("RC2: two scans: the host delivered once in each, then 'no data', never 'invalid state'\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 12);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(true, 6);
    CHECK(ClntFind(a, h.mac()) <= 2500);
    for (int scan = 0; scan < 2; ++scan) {
        if (scan == 1) {
            CHECK_EQ(a.Scan(), 0u);  // (the application scans again: a scan under way is restarted)
            CHECK(PumpApp(a, 3000, [&] { return a.InOrder() == 0; }));
        }
        for (int k = 0; k < 5; ++k) {
            const u32 r = a.InOrder();
            CHECK_EQ(r, static_cast<u32>(R_NO_DATA));
            a.Pump();
        }
    }
    CHECK_EQ(a.book.records(), 1u);
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    ClntClean(a, "after Finalize");
    CheckHost(h);
    CHECK_RULES();
}

static void RC3_HostGone() {
    std::printf("RC3: the host ends while the application waits in its lobby: 0x80, and GetServerInfo still answers\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 13);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(true, 6);
    CHECK(ClntFind(a, h.mac()) <= 2500);
    CHECK_EQ(a.StopScan(), 0u);
    bool pending = false;
    CHECK_EQ(a.Start(h.mac(), &pending), 0u);
    CHECK_EQ(a.Shown(), 5u);
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);  // (in the session: the record of the host joined, the book's refreshed with it)
    u8 before[0x298];
    std::memcpy(before, a.srv, sizeof(before));
    CHECK_EQ(before[9], 2u);  // (the host and us)
    // (Azahar's host ends its network with no Deauthentication and leaves the room: the engine drops the network once the room's lists have been
    // without the host for 3 s, then the lobby's kHostGoneMs)
    const std::vector<Mac> both = {h.mac(), a.con.mac}, alone = {a.con.mac};
    a.e().OnRoomMembers(both);
    a.e().OnRoomMembers(both);
    h.Finalize();
    const u64 t0 = platform::NowMs();
    CHECK(PumpApp(a, 3000 + DlpLobby::kHostGoneMs + 1500, [&] {
        a.e().OnRoomMembers(alone);
        return a.Shown() == 0x80;
    }));
    std::printf("   0x80 %u ms after the host ended\n", static_cast<u32>(platform::NowMs() - t0));
    CHECK_EQ(a.book.armed_prepared(), 0u);
    const char* l = std::getenv("AZL_LONG");
    PumpApp(a, l != nullptr && l[0] == '1' ? 10000 : 3000, [] { return false; });
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    CHECK(std::memcmp(before, a.srv, 0x290) == 0);  // the record the application was given, but for the tick
    CHECK_EQ(a.StopSession(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    ClntClean(a, "after Finalize");
    CHECK_RULES();
}

static void RC4_Home() {
    std::printf("RC4: HOME in the lobby (ClntLeave): 0x80 at once, the node leaves before any StartDistribution\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 14);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(false, 6);
    CHECK(ClntFind(a, h.mac()) <= 2500);
    CHECK_EQ(a.StopScan(), 0u);
    bool pending = false;
    CHECK_EQ(a.Start(h.mac(), &pending), 0u);
    CHECK_EQ(a.Shown(), 5u);
    CHECK(PumpApp(a, 2000, [&] { return h.client_state(2) != AzaharDlpHost::NotJoined; }));
    PumpApp(a, 3500, [] { return false; });  // (a Deauthentication within 3 s of the Authentication is the engine's late copy: GB-01)
    a.svc.ClntLeave();
    CHECK_EQ(a.Shown(), 0x80u);
    CHECK(!a.net.Holds() && a.e().status() != NetworkStatus::ConnectedAsClient);  // (left: the engine given back)
    const bool host_saw = PumpApp(a, 4000, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; });
    u16 me = 0, mask = 0;
    h.e().NodeSnapshot(&me, &mask);
    std::printf("   the host's view after the leave: client %s, its network's nodes 0x%x\n", host_saw ? "gone" : "still there", mask);
    CHECK(host_saw);
    CHECK_EQ(h.sent("A3"), 0u);
    CHECK_EQ(a.book.armed_prepared(), 0u);
    CHECK_EQ(a.StopSession(), 0u);
    CHECK_EQ(a.Shown(), 1u);
    CHECK_EQ(a.Finalize(), 0u);
    ClntClean(a, "after the leave");
    CheckHost(h);
    CHECK_RULES();
}

static void RC5_NotPrepared() {
    std::printf("RC5: a child that is not prepared here: StartSession 0 and 0x80, no client connection at all\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 15);
    ClntKids kids(dlp_children::Verdict::NotPrepared);
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(false, 6);
    CHECK(ClntFind(a, h.mac()) <= 2500);
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    CHECK(a.Line() == "Gionko NO");
    CHECK_EQ(a.StopScan(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    const u16 probes = a.net.counters().probes;
    bool pending = true;
    CHECK_EQ(a.Start(h.mac(), &pending), 0u);
    CHECK(!pending);
    CHECK_EQ(a.Shown(), 0x80u);
    PumpApp(a, 1500, [] { return false; });
    CHECK_EQ(h.client_state(2), static_cast<u8>(AzaharDlpHost::NotJoined));  // the host never saw a client
    CHECK_EQ(h.sent("A1"), 0u);
    CHECK_EQ(a.net.counters().probes, probes);
    CHECK(!a.net.Holds());
    CHECK_EQ(a.book.refusals(), 1u);
    CHECK_EQ(a.Finalize(), 0u);
    ClntClean(a, "after the refusal");
    CheckHost(h);
    CHECK_RULES();
}

static void RC6_AnotherGame() {
    std::printf("RC6: a host of another game beside it (Luigi's Mansion): listed too, with NO\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 16);
    AzaharDlpHost luigi(room, 3, "Luigi", kLuigiChild, 1, 17);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&h.e(), &luigi.e(), &a.e()});
    h.StartHosting(true, 6);
    luigi.StartHosting(true, 6);
    CHECK_EQ(a.Init(), 0u);
    CHECK_EQ(a.Scan(), 0u);
    std::vector<u32> ids;
    PumpApp(a, 6000, [&] {
        if (a.InOrder() == 0) {
            ids.push_back(LoadLE32(a.title));
        }
        return ids.size() >= 2;
    });
    CHECK_EQ(ids.size(), 2u);
    CHECK(std::find(ids.begin(), ids.end(), 0x000F82u) != ids.end() && std::find(ids.begin(), ids.end(), 0x001D1Au) != ids.end());
    CHECK_EQ(a.ServerInfo(luigi.mac()), 0u);
    CHECK(a.Line() == "Luigi NO");
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    CHECK(a.Line() == "Gionko OK");
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    ClntClean(a, "after Finalize");
    CHECK_RULES();
}

static void RC7_FourHosts() {
    std::printf("RC7: four hosts for five scans: every host in every scan, the book never refuses one it knows\n");
    g_rules.Reset();
    MockRoom room;
    std::vector<std::unique_ptr<AzaharDlpHost>> hosts;
    const char* names[4] = {"H1", "H2", "H3", "H4"};
    for (u8 i = 0; i < 4; ++i) {
        hosts.push_back(std::make_unique<AzaharDlpHost>(room, static_cast<u8>(10 + i), names[i], kIslandChild, 3, 20u + i));
    }
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    Ticker ticker({&hosts[0]->e(), &hosts[1]->e(), &hosts[2]->e(), &hosts[3]->e(), &a.e()});
    for (auto& h : hosts) {
        h->StartHosting(true, 6);
    }
    CHECK_EQ(a.Init(), 0u);
    for (int scan = 0; scan < 5; ++scan) {
        CHECK_EQ(a.Scan(), 0u);
        u32 got = 0;
        PumpApp(a, 9000, [&] {
            if (a.InOrder() == 0) {
                ++got;
            }
            return got >= 4;
        });
        std::printf("   scan %d: %u hosts delivered, %u records\n", scan + 1, got, a.book.records());
        CHECK_EQ(got, 4u);
        CHECK_EQ(a.book.records(), 4u);
    }
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    ClntClean(a, "after Finalize");
    CHECK_RULES();
}

/// The capture's host (Mario Party: Island Tour, the good session of 2026-11-13) as the model's wire: its announcement and its five kinds of packet.
static const AzaharDlpWire kAzaharDlpIslandWire = {kDlpRealIslandAnnounce,    &kDlpRealIslandDialog[0], &kDlpRealIslandDialog[1], &kDlpRealIslandDialog[4],
                                                   &kDlpRealIslandDialog[6], &kDlpRealIslandDialog[7], kDlpRealIslandSeed};

/// The SecureData packets a console sends on data channel 2 (the dialogue), in order.
class DialogueOut {
public:
    explicit DialogueOut(Console& c) {
        c.sniff = [this](const WifiPacket& w) {
            EtherType t;
            SecureDataHeader h;
            if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData || !ParseSecureDataHeader(w.data, &h) ||
                h.ActualDataSize() > w.data.size() || h.data_channel != 2) {
                return;
            }
            std::lock_guard<std::mutex> l(m_);
            v_.push_back({h.dest_node_id, std::vector<u8>(w.data.end() - h.ActualDataSize(), w.data.end())});
        };
    }
    struct Out {
        u16 dest;
        std::vector<u8> p;
    };
    std::vector<Out> All() {
        std::lock_guard<std::mutex> l(m_);
        return v_;
    }
    u32 Count(u8 type) {
        std::lock_guard<std::mutex> l(m_);
        u32 n = 0;
        for (const Out& o : v_) {
            n += !o.p.empty() && o.p[0] == type ? 1u : 0u;
        }
        return n;
    }

private:
    std::mutex m_;
    std::vector<Out> v_;
};

static std::string HexOf(const u8* p, std::size_t n) {
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

static void RC8_AnswersAsCaptured() {
    std::printf("RC8: the application's four answers against the capture's (A2, A4, A6, A9), the model of Azahar's host sending the capture's packets\n");
    g_rules.Reset();
    // the golden data: the four answers, from node 2 to node 1, the same bytes the capture of the host's side has (two scripts, one capture)
    static const char* const kIds[4] = {"A2", "A4", "A6", "A9"};
    static const unsigned kIsland[4] = {2, 3, 5, 8};  // (their places in kDlpRealIslandDialog)
    CHECK_EQ(kDlpRealClntPassReplaced, 0u);           // (none of the four carries the passphrase)
    CHECK(std::memcmp(kDlpRealClntSeed, kDlpRealIslandSeed, 4) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        const DlpRealGolden& g = kDlpRealClntAnswers[i];
        const DlpRealGolden& t = kDlpRealIslandDialog[kIsland[i]];
        CHECK(std::strcmp(g.id, kIds[i]) == 0 && std::strcmp(t.id, kIds[i]) == 0);
        CHECK(g.src_node == 2 && g.dst_node == 1 && g.channel == 2);
        CHECK(g.size == t.size && std::memcmp(g.bytes, t.bytes, g.size) == 0);
    }
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 31, kAzaharDlpIslandWire);
    ClntKids kids;
    ClntApp a(room, 2, &kids.table);
    DialogueOut out(a.con);
    Ticker ticker({&h.e(), &a.e()});
    h.StartHosting(true, 6);
    CHECK(ClntFind(a, h.mac()) <= 2500);
    CHECK_EQ(a.ServerInfo(h.mac()), 0u);
    CHECK(a.Line() == "Gionko OK");
    CHECK_EQ(a.StopScan(), 0u);
    CHECK_EQ(a.Prepare(h.mac()), 0u);
    {
        HostGame game(h, false);  // (the host's game accepts the client 1.6 s after it is in)
        bool pending = false;
        CHECK_EQ(a.Start(h.mac(), &pending), 0u);
        CHECK(pending);
        CHECK(PumpApp(a, 4000, [&] { return h.client_state(2) == AzaharDlpHost::Accepted; }));
    }
    h.StartDistribution();
    u8 nine[9];
    Pass(1, nine);
    bool sent = false, saw9 = false;
    CHECK(PumpApp(a, 6000, [&] {
        if (!sent && h.state() == AzaharDlpHost::kNeedToSendPassphrase) {
            sent = h.BeginGame(nine) == 0;
        }
        saw9 = saw9 || a.Shown() == 9;  // (the model's host ends its network at its next poll after the final: what comes after is not this test's)
        return saw9 && out.Count(6) >= 1;   // ... and ours answered a notice (A9)
    }));
    CHECK(PumpApp(a, 2000, [&] { return h.state() == AzaharDlpHost::kComplete; }));
    CHECK_EQ(a.StopSession(), 0u);
    CHECK_EQ(a.Finalize(), 0u);
    PumpApp(a, 300, [&] { return !a.net.Probing(); });
    ClntClean(a, "after Finalize");

    // every packet of ours on channel 2: one of the four, to the host, the capture's bytes but for what the host put in its own packet
    u32 seen[4] = {};
    u8 my_id[2] = {0, 0};
    for (const DialogueOut::Out& o : out.All()) {
        CHECK_EQ(o.dest, 1u);
        const u8 type = o.p.empty() ? 0 : o.p[0];
        const int k = type == 2 ? 0 : type == 3 ? 1 : type == 5 ? 2 : type == 6 ? 3 : -1;
        if (k < 0) {
            std::printf("   a packet of ours that the capture's application does not send: %s\n", HexOf(o.p.data(), o.p.size()).c_str());
            CHECK(false);
            continue;
        }
        const DlpRealGolden& g = kDlpRealClntAnswers[k];
        ++seen[k];
        std::vector<u8> p = o.p;
        std::vector<u8> zero = o.p;
        dlp_real::Seal(dlp_real::kTxSeed, zero.data(), zero.size());
        CHECK(zero == o.p);  // ours is sealed with the seed 00 00 00 00
        if (k == 0) {
            my_id[0] = p.size() > 0x17 ? p[0x16] : 0;  // (the id ours gives itself in its AuthAck: the model's host puts it in its later packets)
            my_id[1] = p.size() > 0x17 ? p[0x17] : 0;
        } else if (p.size() > 0x0F) {
            CHECK(p[0x0E] == my_id[0] && p[0x0F] == my_id[1]);  // ours echoes the id of the host's packet: the model's is ours
            CHECK(p[0x0D] >= 1);                              // ... and its sequence (the model's host counts from the AuthAck: 1, 2, 3 ...)
            std::memcpy(p.data() + 0x0D, g.bytes + 0x0D, 3);  // the capture's host: kDlpRealClntAsked
            CHECK(g.bytes[0x0D] == kDlpRealClntAsked[k][0] && g.bytes[0x0E] == kDlpRealClntAsked[k][1] && g.bytes[0x0F] == kDlpRealClntAsked[k][2]);
        }
        if (k == 2 && p.size() == g.size) {
            CHECK(p[0x16] == 0 && p[0x17] == 0 && p[0x18] == 0 && p[0x19] == 0);  // (ours: zero where Azahar's client leaves its stack)
            std::memcpy(p.data() + 0x16, g.bytes + 0x16, 4);
        }
        const bool same = p.size() == g.size && (dlp_real::Seal(kDlpRealClntSeed, p.data(), p.size()), std::memcmp(p.data(), g.bytes, g.size) == 0);
        if (!same) {
            std::printf("   %s of ours is not the capture's: %s\n", g.id, HexOf(o.p.data(), o.p.size()).c_str());
        }
        CHECK(same);
    }
    std::printf("   ours sent A2 x%u, A4 x%u, A6 x%u, A9 x%u: each the capture's packet but for the host's sequence and id\n", seen[0], seen[1], seen[2], seen[3]);
    for (unsigned k = 0; k < 4; ++k) {
        CHECK(seen[k] >= 1);
    }
    CHECK_EQ(my_id[0], 0x01u);  // the id the capture's application gives too
    CHECK_EQ(my_id[1], 0x02u);
    CheckHost(h);
    CHECK_RULES();
}

int main() {
    std::printf("dlp_real_client_test: the searcher of the real Download Play in front of Azahar's host\n");
#if !AZL_CLNT_SYSTEM_JOINS
    // (dlp_real_client_test_default, route B: the plugin's dlp:CLNT - never a join; the searcher's scenarios run in the build with G1's join path)
    std::printf("route B: clnt::kSystemAppJoins false - RC1b, RC2, RC5, RC6\n");
    RC1b_ReadyRefused();
    RC2_TwoScans();
    RC5_NotPrepared();
    RC6_AnotherGame();
    (void)&R2_Sequence;
    (void)&R5b_Losses;
    (void)&R5c_ProbeLosses;
    (void)&R6f_Full;
    (void)&R6_Three;
    (void)&R9c_R12c;
    (void)&R3a_Ours;
    (void)&Rel_Paths;
    (void)&RV3_FullNotProbed;
    (void)&RV4_StartBeforeGo;
    (void)&RC1_IslandTour;
    (void)&RC3_HostGone;
    (void)&RC4_Home;
    (void)&RC7_FourHosts;
    (void)&RC8_AnswersAsCaptured;
#else
    CHECK(clnt::kSystemAppJoins);
    R2_Sequence(1, false);
    R2_Sequence(2, false);
    R2_Sequence(3, true);
    R5b_Losses(false);
    R5b_Losses(true);
    R5c_ProbeLosses(false);
    R5c_ProbeLosses(true);
    R6f_Full();
    R6_Three();
    R9c_R12c();
    R3a_Ours();
    Rel_Paths();
    RV3_FullNotProbed();
    RV4_StartBeforeGo();
    RC1_IslandTour();
    RC2_TwoScans();
    RC3_HostGone();
    RC4_Home();
    RC5_NotPrepared();
    RC6_AnotherGame();
    RC7_FourHosts();
    RC8_AnswersAsCaptured();
    (void)&RC1b_ReadyRefused;
#endif
    // (rig helpers this binary has no use for)
    (void)&Join;
    (void)&FindGroup;
    (void)&OpenLobby;
    (void)&CliNode;
    (void)&CliState;
    (void)&LobbyFrame;
    (void)&EngineClean;
    (void)&Engines;
    (void)&Settle;
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
