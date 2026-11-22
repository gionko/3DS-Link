// The fake Download Play lobby with EIGHT players, and the titles of four beside it (2026-11-13: "a game that plays in eight is played in eight").
// The tables of the lobby hold host + 7 clients (azl/dlp_types.hpp: kMaxPlayers), and every title has a ceiling of its own: what LobbyConfig::max_players
// says, FOUR when it says nothing (TitlePlayers). Sources/ctr/plugin.cpp gives eight to the five rows of Mario Kart 7 and to nobody else
// (Tests/plugin_dlp_test.cpp holds the rows and the wiring). What this file holds:
//   1. the ceiling as a value: 0 is four, the bounds, the configuration of every size (TestCeiling);
//   2. what a game gets when it asks dlp:SRVR Initialize for 0, 1 ... 8, 15, 255 clients, for a title of four and for one of eight, and the bytes of
//      Mario Kart 7's announcement (TestAskedClients);
//   3. what a searcher lists: a title of four never a lobby of five, a title of eight every lobby up to eight, nobody one of nine (TestSearcher);
//   4. A WHOLE EVENING OF A TITLE OF FOUR, line by line as the code of BEFORE the tables grew gave it (the lines below were written by that code:
//      analysis/lobby-otto-2026-11-13/impl/strumenti compiles the block [four-trace] against the photograph of that tree): the fifth player that is
//      never answered, two nodes above the ceiling listed by a faulty engine, a node above with the lobby full, a place taken again, START, GO. For
//      a configuration that says nothing, for one that says 4, and (the ejection of the nodes above apart, which is the title's) for a title of eight
//      whose game asks for three clients (TestTitleOfFour). Tests/dlp_cap_guard_test.cpp is the guard of the titles of four; this is one more look,
//      at the cases it does not try;
//   5. the session of EIGHT on the in-memory air, with the words of the games: the search, seven that enter, the ninth that is never answered, the
//      vectors of the ids of every size, a node above the ceiling, one that leaves and a place taken again, START and GO to seven, the count of its
//      own lobby that every client in the session reads on the way up to eight and down again, a client that never answers START, a node that is
//      not a client and is never written to, the host that goes (TestEightOnTheAir, TestEightCountsItself, TestEightLosesOne, TestEightHostGoes);
//   6. the same session on the REAL engine (Tests/dlp_net_rig.hpp: engines joined by the in-memory room): seven that enter TOGETHER, the ninth, one that
//      leaves and the ninth in its place, START, GO, the network the room carries (8 nodes for Mario Kart 7's seven clients, 4 for a title of four that is asked the same), a searcher of four
//      that does not list the lobby of eight - and THE RING OF THE LOBBY'S MESSAGES (UdsEngine::kLobbyInbox, 8): how full it gets with seven clients,
//      also with the creator's network thread stalled and with the seven clients stalled, counted frame by frame (TestEightOnTheEngine,
//      TestEightStalls, TestFourOnTheEngine).
//   7. the lobby and its service on a HOSTILE network and under a hostile game, for a title of eight, of four and for the ceilings between and at
//      the bounds: a fixed seed, a fixed number of commands, the invariants held after every command and the sanitizers on the tables
//      (TestEightHostile; the adversarial tests of the lobby only know titles of four).
// Parts 1-5 and 7 run on the simulated clock (part 7 takes about 20 s); part 6 in real time (about 10 s).
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=address,undefined Tests/dlp_eight_test.cpp
//       Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o <CORE> <enet objects>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define private public  // (the test reads the engine's ring of lobby messages and the lobby's ceiling: no other way to count what the ring loses)
#define protected public
#include "azl/dlp_ipc.hpp"
#include "azl/dlp_lobby_net.hpp"
#include "azl/dlp_real_net.hpp"
#undef private
#undef protected

#include "dlp_fakenet.hpp"
#include "dlp_net_rig.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

namespace {

// ---- [four-trace: this block is compiled, unchanged, against the tree of BEFORE the lobby held eight players too: analysis/lobby-otto-2026-11-13/impl/strumenti] ----
// One console on the in-memory air (Tests/dlp_fakenet.hpp), driven ONLY with the words of dlp:FKCL / dlp:SRVR as a game sends them (the lobby polls
// where it is called: the mode of the pure tests). Nothing here names a constant of the code in proof, an array of ids or LobbyConfig::max_players.
constexpr u32 kGameBase = 0x08000000;
static const DlpKind kFk = DlpKind::Fkcl;
static const DlpKind kSv = DlpKind::Srvr;

struct Box {
    FakeLobbyNet net;
    DlpLobby lobby;
    DlpService svc;
    std::vector<u8> mem;  // the game's memory, at kGameBase
    u32 w[64];
    Box(FakeAir& air, int i, const LobbyConfig& c) : net(air, i), lobby(&net, c), mem(0x2000, 0xCC) {
        svc.SetLobby(&lobby);
    }
    static u8* Map(void* ctx, u32 address, u32 size) {
        Box* b = static_cast<Box*>(ctx);
        if (address < kGameBase || address - kGameBase > b->mem.size() || size > b->mem.size() - (address - kGameBase)) {
            return nullptr;
        }
        return b->mem.data() + (address - kGameBase);
    }
    DlpOutcome Ask(DlpKind kind, u32 id, std::initializer_list<u32> args, const u32* sbuf = nullptr) {
        for (u32& x : w) {
            x = 0xC0DE0000u;
        }
        w[0] = id << 16;
        u32 i = 1;
        for (const u32 a : args) {
            w[i++] = a;
        }
        IpcResolver r;
        r.ctx = this;
        r.map = &Map;
        return svc.Dispatch(kind, w, sbuf, r);
    }
    static void NameWords(const char* s, u32 out[5]) {
        u16 n[10];
        FakeAir::Name(n, s);
        for (int i = 0; i < 5; ++i) {
            out[i] = static_cast<u32>(n[2 * i]) | (static_cast<u32>(n[2 * i + 1]) << 16);
        }
    }
    // dlp:SRVR (the words of Mario Kart 7's creator on a console: 0x0F req 00089000 00000007 0000004a 00000000 00040000 00000002)
    u32 SvInit(u32 clients, const char* name) {
        u32 n[5];
        NameWords(name, n);
        Ask(kSv, 0x0F, {0x00089000, clients, 0x4A, 0, 0x00040000, 2, n[0], n[1], n[2], n[3], n[4], 0});
        return w[1];
    }
    u32 SvInitPlain(u32 clients) {
        Ask(kSv, 0x01, {0x00089000, clients, 0x4A, 0, 0x00040000, 2});
        return w[1];
    }
    u32 SvAccept(u32 manual, u32 channel) {
        Ask(kSv, 0x05, {manual, channel});
        return w[1];
    }
    u32 SvState() {
        Ask(kSv, 0x03, {});
        return w[2];
    }
    /// GetConnectingClients with a vector of `elements` u16 (the games ask with 15): "total:id,id,..."
    std::string SvClients(u32 elements = 15) {
        std::memset(mem.data(), 0xCC, mem.size());
        Ask(kSv, 0x0B, {elements, ((2 * elements) << 4) | 0xC, kGameBase});
        return IdList(elements);
    }
    u32 SvClientState(u32 node) {
        Ask(kSv, 0x0D, {node});
        return w[1] != 0 ? w[1] : w[2];
    }
    u32 SvStart() {
        Ask(kSv, 0x07, {});
        return w[1];
    }
    u32 SvPass(const u8* nine) {
        u32 p[3] = {0, 0, 0};
        std::memcpy(p, nine, 9);
        Ask(kSv, 0x08, {p[0], p[1], p[2]});
        return w[1];
    }
    u32 SvFinalize() {
        Ask(kSv, 0x02, {});
        return w[1];
    }
    // dlp:FKCL (Mario Kart 7's searcher: 0x11 req 0000f000 00000004 <name>, 0x05 req 00000421 <title id low> 00040001)
    u32 FkInit(const char* name) {
        u32 n[5];
        NameWords(name, n);
        Ask(kFk, 0x11, {0x0000F000, 4, n[0], n[1], n[2], n[3], n[4], 0});
        return w[1];
    }
    u32 FkScan(u32 tid_low) {
        Ask(kFk, 0x05, {0x00000421, tid_low, 0x00040001, 0, 0});
        return w[1];
    }
    /// The list as the game reads it: GetTitleInfoInOrder until "no data", and GetServerInfo of every entry: "mac5 max/joined" per lobby.
    std::string FkList() {
        std::string s;
        const u32 sb_title[2] = {(0x13A8u << 14) | 2, kGameBase};
        const u32 sb_server[2] = {(0x298u << 14) | 2, kGameBase};
        for (u32 i = 0; i < 16; ++i) {
            Ask(kFk, 0x09, {i == 0 ? 1u : 0u}, sb_title);
            if (w[1] != 0) {
                break;
            }
            u8 mac[6];
            std::memcpy(mac, mem.data() + 8, 6);
            const u32 w1 = static_cast<u32>(mac[0]) | (static_cast<u32>(mac[1]) << 8) | (static_cast<u32>(mac[2]) << 16) | (static_cast<u32>(mac[3]) << 24);
            const u32 w2 = static_cast<u32>(mac[4]) | (static_cast<u32>(mac[5]) << 8);
            Ask(kFk, 0x07, {w1, w2}, sb_server);
            char line[48];
            std::snprintf(line, sizeof(line), "%s%02x %u/%u", s.empty() ? "" : " ", mac[5], w[1] == 0 ? mem[8] : 0xEEu, w[1] == 0 ? mem[9] : 0xEEu);
            s += line;
        }
        return s.empty() ? "-" : s;
    }
    DlpOutcome FkJoin(const Mac& host, u32 tid_low) {
        const u32 w1 = static_cast<u32>(host[0]) | (static_cast<u32>(host[1]) << 8) | (static_cast<u32>(host[2]) << 16) | (static_cast<u32>(host[3]) << 24);
        const u32 w2 = static_cast<u32>(host[4]) | (static_cast<u32>(host[5]) << 8);
        return Ask(kFk, 0x0B, {w1, w2, tid_low, 0x00040001});
    }
    u32 FkStatus() {
        Ask(kFk, 0x0C, {});
        return w[2];
    }
    /// GetConnectingNodes with a vector of `elements` u16: "total:id,id,..."
    std::string FkNodes(u32 elements = 15) {
        std::memset(mem.data(), 0xCC, mem.size());
        const u32 sb[2] = {((2 * elements) << 14) | 2, kGameBase};
        Ask(kFk, 0x0D, {elements}, sb);
        return IdList(elements);
    }
    std::string FkPass() {
        Ask(kFk, 0x0F, {});
        return std::string(reinterpret_cast<const char*>(w + 2), 8);
    }
    u32 FkStop() {
        Ask(kFk, 0x10, {});
        return w[1];
    }
    u32 FkFinalize() {
        Ask(kFk, 0x02, {});
        return w[1];
    }
    /// "total:ids" out of the reply of the two commands of the ids, and that nothing was written past what the reply says
    std::string IdList(u32 elements) {
        if (w[1] != 0) {
            char e[24];
            std::snprintf(e, sizeof(e), "err %08x", w[1]);
            return e;
        }
        const u32 total = w[2];
        const u32 written = total < elements ? total : elements;
        std::string s = std::to_string(total) + ":";
        u32 shown = 0;
        for (u32 i = 0; i < 64; ++i) {
            const u32 id = static_cast<u32>(mem[2 * i]) | (static_cast<u32>(mem[2 * i + 1]) << 8);
            if (id == 0xCCCC) {
                break;
            }
            s += (shown++ ? "," : "") + std::to_string(id);
        }
        if (shown > written) {
            s += " OVERRUN";
        }
        return s;
    }
};

struct Trace {
    std::vector<std::string> lines;
    void Add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        char line[512];
        va_list ap;
        va_start(ap, format);
        std::vsnprintf(line, sizeof(line), format, ap);
        va_end(ap);
        lines.push_back(line);
    }
};

static void RunAir(FakeAir& air, std::vector<Box*>& all, u32 ms) {
    for (u32 t = 0; t < ms; t += 5) {
        air.Advance(5);
        for (Box* b : all) {
            b->lobby.Pump();
        }
    }
}
/// StartFakeSession as the front-end does it: the request, then PollPending until the outcome (at most `ms`).
static u32 JoinNow(FakeAir& air, std::vector<Box*>& all, Box& guest, Box& host, u32 tid_low, u32 ms) {
    guest.Ask(kFk, 0x06, {});  // StopScan: the games stop looking before they enter
    if (guest.FkJoin(host.net.mac(), tid_low) == DlpOutcome::Done) {
        return guest.w[1];
    }
    for (u32 t = 0; t < ms; t += 5) {
        if (guest.svc.PollPending(guest.w)) {
            return guest.w[1];
        }
        air.Advance(5);
        for (Box* b : all) {
            b->lobby.Pump();
        }
    }
    return 0xDEADDEAD;
}
static std::string Kicks(Box& host) {
    std::string s;
    for (int node = 2; node <= 7; ++node) {
        s += (node > 2 ? "," : "") + std::to_string(host.net.Kicked(node));
    }
    return s;
}
static std::string SentTo(Box& from, int type) {
    std::string s;
    for (int node = 1; node <= 7; ++node) {
        s += (node > 1 ? "," : "") + std::to_string(from.net.Sent(type, node));
    }
    return s;
}

/// A whole evening of a title of FOUR players, with what goes wrong in a room: `make` builds the configuration of console `i` (the caller decides what
/// it says about the ceiling of the title), the creator's game asks for `asked` clients. Six consoles: the creator, four that look for it (the fourth is
/// the fifth player), one that comes late. Every answer to a game and every count of what the lobby asked of the network goes into the trace.
template <typename MakeCfg>
static void TitleOfFour(Trace& tr, MakeCfg make, u32 tid_low, u32 asked) {
    FakeAir air;
    std::vector<std::unique_ptr<Box>> own;
    std::vector<Box*> all;
    static const char* const kNames[6] = {"Host", "Anna", "Bruno", "Carla", "Dario", "Elena"};
    for (int i = 0; i < 6; ++i) {
        own.emplace_back(new Box(air, i, make(i, kNames[i])));
        all.push_back(own.back().get());
    }
    Box& h = *all[0];
    static const u8 kNine[9] = {'7', 'e', '5', 'b', '0', 'c', '1', 'a', 0};

    // the creator opens; a faulty engine lists two nodes above the title's ceiling for a while (nobody is in yet: they are among the first members)
    // (one call per statement, here and below: the order in which the arguments of a call are evaluated is not the order they are written in)
    const u32 r_init = h.SvInit(asked, "Host");
    const u32 r_accept = h.SvAccept(0, 6);
    const u32 s_open = h.SvState();
    tr.Add("init %08x accept %08x state %u", r_init, r_accept, s_open);
    u8 ann[kAnnounceSize];
    EncodeAnnounce(h.net.LastAnnounce(), ann);
    std::string hex;
    for (u32 i = 0; i < 9; ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", ann[i]);
        hex += b;
    }
    tr.Add("announce %s ch %d opens %d", hex.c_str(), h.net.LastChannel(), h.net.HostOpenCalls());
    air.SetKickLag(100000);
    air.Phantom(0, 5, 0x5555, "Ghost5");
    air.Phantom(0, 6, 0x6666, "Ghost6");
    RunAir(air, all, 450);
    std::string cl = h.SvClients();
    tr.Add("two nodes above: kicks %s clients %s", Kicks(h).c_str(), cl.c_str());
    air.Leave(0, 5);
    RunAir(air, all, 450);
    cl = h.SvClients();
    tr.Add("one left: kicks %s clients %s", Kicks(h).c_str(), cl.c_str());
    air.Leave(0, 6);
    air.SetKickLag(0);
    RunAir(air, all, 250);

    // four consoles look for it and list it
    for (int i = 1; i <= 4; ++i) {
        const u32 r_i = all[i]->FkInit(kNames[i]);
        const u32 r_s = all[i]->FkScan(tid_low);
        tr.Add("%s init %08x scan %08x", kNames[i], r_i, r_s);
    }
    RunAir(air, all, 300);
    for (int i = 1; i <= 4; ++i) {
        tr.Add("%s lists %s", kNames[i], all[i]->FkList().c_str());
    }
    // three enter, one after the other
    for (int i = 1; i <= 3; ++i) {
        const u32 r = JoinNow(air, all, *all[i], h, tid_low, 3500);
        const u32 status = all[i]->FkStatus();
        tr.Add("%s joins %08x status %08x", kNames[i], r, status);
        RunAir(air, all, 40);
    }
    RunAir(air, all, 300);
    cl = h.SvClients();
    u32 st[5];
    int k = 0;
    for (const u32 node : {2u, 3u, 4u, 5u, 8u}) {
        st[k++] = h.SvClientState(node);
    }
    tr.Add("host clients %s states %08x %08x %08x %08x %08x", cl.c_str(), st[0], st[1], st[2], st[3], st[4]);
    for (int i = 1; i <= 3; ++i) {
        tr.Add("%s nodes %s", kNames[i], all[i]->FkNodes().c_str());
    }
    tr.Add("Dario lists %s", all[4]->FkList().c_str());
    // the fifth player knocks: nobody answers, the join gives up by itself
    const u64 t0 = air.now();
    const u32 fifth = JoinNow(air, all, *all[4], h, tid_low, 4000);
    const u64 waited = air.now() - t0;
    const u32 fifth_status = all[4]->FkStatus();
    tr.Add("the fifth joins %08x after %llu ms status %08x", fifth, static_cast<unsigned long long>(waited), fifth_status);
    cl = h.SvClients();
    tr.Add("host clients %s kicks %s present %d", cl.c_str(), Kicks(h).c_str(), air.Present(0));
    // with the lobby full, a faulty engine lists a node above the ceiling: it is past the members the lobby reads
    air.SetKickLag(100000);
    air.Phantom(0, 5, 0x5555, "Ghost5");
    RunAir(air, all, 450);
    cl = h.SvClients();
    tr.Add("a node above, lobby full: kicks %s clients %s", Kicks(h).c_str(), cl.c_str());
    air.Leave(0, 5);
    air.SetKickLag(0);
    // one leaves, the one that came late takes its place
    const u32 r_stop = all[2]->FkStop();
    tr.Add("Bruno stops %08x", r_stop);
    RunAir(air, all, 300);
    cl = h.SvClients();
    std::string seen = all[1]->FkNodes();
    tr.Add("host clients %s Anna nodes %s", cl.c_str(), seen.c_str());
    const u32 r_ei = all[5]->FkInit("Elena");
    const u32 r_es = all[5]->FkScan(tid_low);
    tr.Add("Elena init %08x scan %08x", r_ei, r_es);
    RunAir(air, all, 300);
    tr.Add("Elena lists %s", all[5]->FkList().c_str());
    const u32 late = JoinNow(air, all, *all[5], h, tid_low, 3500);
    const u32 late_status = all[5]->FkStatus();
    tr.Add("Elena joins %08x status %08x", late, late_status);
    RunAir(air, all, 300);
    cl = h.SvClients();
    seen = all[3]->FkNodes();
    tr.Add("host clients %s Carla nodes %s", cl.c_str(), seen.c_str());
    // START, GO, the passphrase
    const u32 r_start = h.SvStart();
    const u32 s_start = h.SvState();
    tr.Add("start %08x state %u", r_start, s_start);
    RunAir(air, all, 100);
    const u32 s_started = h.SvState();
    tr.Add("after START: state %u START to %s acks %s|%s|%s", s_started, SentTo(h, 1).c_str(), SentTo(*all[1], 2).c_str(), SentTo(*all[5], 2).c_str(),
           SentTo(*all[3], 2).c_str());
    const u32 c1 = all[1]->FkStatus();
    const u32 c5 = all[5]->FkStatus();
    const u32 c3 = all[3]->FkStatus();
    tr.Add("clients %08x %08x %08x", c1, c5, c3);
    const u32 r_pass = h.SvPass(kNine);
    const u32 s_pass = h.SvState();
    tr.Add("pass %08x state %u", r_pass, s_pass);
    RunAir(air, all, 100);
    const u32 s_done = h.SvState();
    tr.Add("after GO: state %u GO to %s closes %d", s_done, SentTo(h, 3).c_str(), h.net.HostCloseCalls());
    for (const int i : {1, 5, 3}) {
        const u32 status = all[i]->FkStatus();
        const std::string pass = all[i]->FkPass();
        const std::string nodes = all[i]->FkNodes();
        tr.Add("%s status %08x pass %s nodes %s", kNames[i], status, pass.c_str(), nodes.c_str());
    }
    cl = h.SvClients();
    tr.Add("host clients %s kicks %s bad %u", cl.c_str(), Kicks(h).c_str(), h.lobby.bad_messages());
    // everybody goes
    const u32 f0 = h.SvFinalize();
    const u32 f1 = all[1]->FkFinalize();
    const u32 f5 = all[5]->FkFinalize();
    const u32 f3 = all[3]->FkFinalize();
    tr.Add("finalize %08x %08x %08x %08x", f0, f1, f5, f3);
    tr.Add("orders %s|%s|%s|%s|%s|%s", h.net.CloseOrder().c_str(), all[1]->net.CloseOrder().c_str(), all[2]->net.CloseOrder().c_str(),
           all[3]->net.CloseOrder().c_str(), all[4]->net.CloseOrder().c_str(), all[5]->net.CloseOrder().c_str());
    tr.Add("calls %d %d %d %d %d %d", h.net.AllCalls(), all[1]->net.AllCalls(), all[2]->net.AllCalls(), all[3]->net.AllCalls(), all[4]->net.AllCalls(),
           all[5]->net.AllCalls());
}
// ---- [end of four-trace] ----

constexpr u32 kKirbyLow = 0x0010C000;  // Kirby Triple Deluxe: a title of four
constexpr u32 kMk7Low = 0x00030700;    // Mario Kart 7 (Europe): the title of eight

/// The configuration of a console as every test written before the lobby held eight builds it: zeroed, and the three fields of then.
static LobbyConfig OldCfg(u32 tid_low, int i, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = tid_low;
    c.friend_seed = 0xF00D0000u + static_cast<u32>(i);
    FakeAir::Name(c.name, name);
    return c;
}
static LobbyConfig CfgOf(u32 tid_low, int i, const char* name, u8 max_players) {
    LobbyConfig c = OldCfg(tid_low, i, name);
    c.max_players = max_players;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. The ceiling as a value.

static void TestCeiling() {
    std::printf("the ceiling of a title: 0 is four\n");
    CHECK_EQ(kMaxPlayers, 8u);
    CHECK_EQ(kMaxClients, 7u);
    CHECK_EQ(kTitlePlayers, 4u);
    CHECK_EQ(kMaxGroups, 4u);
    // what a configuration says -> the ceiling, written out by hand
    static const u8 kWant[12] = {4, 2, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8};
    for (u32 said = 0; said < 12; ++said) {
        CHECK_EQ(TitlePlayers(said), kWant[said]);
    }
    CHECK_EQ(TitlePlayers(15), 8u);
    CHECK_EQ(TitlePlayers(16), 8u);
    CHECK_EQ(TitlePlayers(255), 8u);
    CHECK_EQ(TitlePlayers(0x100), 8u);
    CHECK_EQ(TitlePlayers(0xFFFFFFFFu), 8u);
    // the field is the last byte of a structure that did not grow, and a zeroed structure has it at zero
    CHECK_EQ(sizeof(LobbyConfig), 40u);
    LobbyConfig zero;
    std::memset(&zero, 0, sizeof(zero));
    CHECK_EQ(zero.max_players, 0u);
    LobbyConfig braces = {};
    CHECK_EQ(braces.max_players, 0u);
    // the lobby of every configuration: its ceiling, and that the byte it shares with three flags keeps both
    for (u32 said = 0; said <= 255; ++said) {
        FakeAir air;
        FakeLobbyNet net(air, 0);
        DlpLobby lobby(&net, CfgOf(kMk7Low, 0, "P", static_cast<u8>(said)));
        const u32 want = said == 0 ? 4u : (said < 2 ? 2u : (said > 8 ? 8u : said));
        CHECK_EQ(lobby.max_players(), want);
        lobby.SetExternalPoll(true);
        lobby.SetOnNet(true);
        CHECK_EQ(lobby.max_players(), want);
        lobby.SetOnNet(false);
        lobby.SetExternalPoll(false);
        CHECK_EQ(lobby.max_players(), want);
        // ... through a whole life of the searcher and of the creator (the teardowns clear flags of that byte)
        CHECK_EQ(lobby.ClientInitialize(nullptr, 4), 0u);
        CHECK_EQ(lobby.ClientStartScan(0), 0u);
        CHECK_EQ(lobby.ClientFinalize(), 0u);
        CHECK_EQ(lobby.ServerInitialize(255, 0, nullptr), 0u);
        CHECK_EQ(lobby.ServerStartAccepting(false, 6), 0u);
        CHECK_EQ(net.LastAnnounce().max_nodes, want);
        CHECK_EQ(lobby.ServerFinalize(), 0u);
        CHECK(lobby.OnGameUdsStarting() == false);
        CHECK_EQ(lobby.max_players(), want);
        CHECK_EQ(lobby.flags2_ & 0x0F, 0u);  // (the three flags are down again)
    }
    {  // the configuration of before, untouched: four
        FakeAir air;
        FakeLobbyNet net(air, 0);
        DlpLobby lobby(&net, OldCfg(kKirbyLow, 0, "P"));
        CHECK_EQ(lobby.max_players(), 4u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. What a game gets when it asks for N clients.

static void TestAskedClients() {
    std::printf("the clients a game asks for, by the ceiling of its title\n");
    static const u32 kAsked[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 255, 0x100, 0x107, 0xFFFFFF07u};
    // the nodes announced (the byte at +8 of the 29), written by hand: a title of four as before (Tests/dlp_cap_guard_test.cpp), a title of eight
    static const u8 kFour[14] = {2, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 2, 4, 4};
    static const u8 kEight[14] = {2, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 2, 8, 8};
    struct Case {
        u8 said;         // LobbyConfig::max_players
        const u8* want;  // per asked
        u8 top;          // for the ceilings in between: min(max(asked & 0xFF, 1), top - 1) + 1
    };
    static const Case kCases[] = {{0, kFour, 4}, {4, kFour, 4}, {8, kEight, 8}, {9, kEight, 8}, {255, kEight, 8}, {1, nullptr, 2},
                                  {2, nullptr, 2}, {3, nullptr, 3}, {5, nullptr, 5}, {6, nullptr, 6}, {7, nullptr, 7}};
    for (const Case& c : kCases) {
        for (int k = 0; k < 14; ++k) {
            for (const bool with_name : {false, true}) {
                FakeAir air;
                Box b(air, 0, CfgOf(kMk7Low, 0, "Cfg", c.said));
                CHECK_EQ(with_name ? b.SvInit(kAsked[k], "Host") : b.SvInitPlain(kAsked[k]), 0u);  // always a success: the cut is silent
                CHECK_EQ(b.SvAccept(0, 11), 0u);
                const u32 low = kAsked[k] & 0xFF;
                const u32 by_rule = (low < 1 ? 1 : (low > static_cast<u32>(c.top) - 1 ? static_cast<u32>(c.top) - 1 : low)) + 1;
                CHECK_EQ(b.net.LastAnnounce().max_nodes, c.want != nullptr ? c.want[k] : by_rule);
                CHECK_EQ(b.net.LastAnnounce().max_nodes, by_rule);
                CHECK_EQ(b.net.HostOpenCalls(), 1);
                CHECK_EQ(b.net.LastChannel(), 11);
            }
        }
    }
    // Mario Kart 7's creator as the console of 2026-11-13 shows it (dlp:SRVR 0x0F, 7 clients): the 29 bytes in the air
    FakeAir air;
    Box mk7(air, 0, CfgOf(kMk7Low, 0, "3DS Link", 8));
    CHECK_EQ(mk7.SvInit(7, "Host"), 0u);
    CHECK_EQ(mk7.SvAccept(0, 11), 0u);
    u8 got[kAnnounceSize];
    EncodeAnnounce(mk7.net.LastAnnounce(), got);
    static const u8 kWant[kAnnounceSize] = {0x44, 0x4C, 0x01, 0x01, 0x00, 0x07, 0x03, 0x00, 0x08, 'H', 0, 'o', 0, 's', 0, 't', 0,
                                            0,    0,    0,    0,    0,    0,    0,    0,    0,    0,   0, 0};
    CHECK(std::memcmp(got, kWant, kAnnounceSize) == 0);
    // ... and the same game on a title of four (the configuration of before): 4 nodes, the byte of 1.1.4
    Box old(air, 1, OldCfg(kMk7Low, 1, "3DS Link"));
    CHECK_EQ(old.SvInit(7, "Host"), 0u);
    CHECK_EQ(old.SvAccept(0, 11), 0u);
    EncodeAnnounce(old.net.LastAnnounce(), got);
    CHECK_EQ(got[8], 4u);
    for (u32 i = 0; i < kAnnounceSize; ++i) {
        if (i != 8) {
            CHECK_EQ(got[i], kWant[i]);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. What a searcher lists.

static void TestSearcher() {
    std::printf("the lobbies a searcher lists, by the ceiling of its title\n");
    static const u32 kMax[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 255};
    for (const u8 said : {0, 4, 8, 6, 2}) {
        const u32 top = said == 0 ? 4u : said;
        for (const u32 max : kMax) {
            for (const u32 total : {1u, 2u, 8u}) {
                FakeAir air;
                std::vector<Box*> all;
                Box b(air, 0, CfgOf(kMk7Low, 0, "Seek", said));
                all.push_back(&b);
                HostAnnounce a = {};
                a.child_tid_low = kMk7Low;
                a.max_nodes = static_cast<u8>(max);
                a.accepting = 1;
                FakeAir::Name(a.name, "Other");
                air.AddAir(Mac{0x40, 0xF4, 0x07, 0x99, 0x99, 0x77}, a, static_cast<u8>(total));
                CHECK_EQ(b.FkInit("Seek"), 0u);
                CHECK_EQ(b.FkScan(kMk7Low), 0u);
                RunAir(air, all, 200);
                const bool listed = max >= 2 && max <= top && total <= max;
                char want[32];
                std::snprintf(want, sizeof(want), "77 %u/%u", max, total);
                CHECK_EQ(b.FkList(), std::string(listed ? want : "-"));
                // a lobby that is not listed cannot be entered: "no data", at once, and nothing is asked of the network
                if (!listed) {
                    b.Ask(kFk, 0x06, {});
                    CHECK(b.FkJoin(Mac{0x40, 0xF4, 0x07, 0x99, 0x99, 0x77}, kMk7Low) == DlpOutcome::Done);
                    CHECK_EQ(b.w[1], 0xC880AFEFu);
                    CHECK_EQ(b.net.ConnectBeginCalls(), 0);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. A whole evening of a title of four, as before.

/// The lines the code of BEFORE gave (analysis/lobby-otto-2026-11-13/prima, 2026-11-13) for 7, 3 and 1 clients asked. Not to be touched to make the
/// test pass: a line that changes is a title of four that changed.
static const char* const kBefore7[] = {
    "init 00000000 accept 00000000 state 2",
    "announce 444c010100c0100004 ch 6 opens 1",
    "two nodes above: kicks 0,0,0,3,0,0 clients 0:",
    "one left: kicks 0,0,0,3,2,0 clients 0:",
    "Anna init 00000000 scan 00000000",
    "Bruno init 00000000 scan 00000000",
    "Carla init 00000000 scan 00000000",
    "Dario init 00000000 scan 00000000",
    "Anna lists 10 4/1",
    "Bruno lists 10 4/1",
    "Carla lists 10 4/1",
    "Dario lists 10 4/1",
    "Anna joins 00000000 status 05010002",
    "Bruno joins 00000000 status 05010003",
    "Carla joins 00000000 status 05010004",
    "host clients 3:2,3,4 states 05010002 05010003 05010004 c880afef c880afef",
    "Anna nodes 4:1,2,3,4",
    "Bruno nodes 4:1,2,3,4",
    "Carla nodes 4:1,2,3,4",
    "Dario lists 10 4/4",
    "the fifth joins c920affe after 3000 ms status 01000000",
    "host clients 3:2,3,4 kicks 0,0,0,3,2,0 present 4",
    "a node above, lobby full: kicks 0,0,0,3,2,0 clients 3:2,3,4",
    "Bruno stops 00000000",
    "host clients 2:2,4 Anna nodes 3:1,2,4",
    "Elena init 00000000 scan 00000000",
    "Elena lists 10 4/3",
    "Elena joins 00000000 status 05010003",
    "host clients 3:2,3,4 Carla nodes 4:1,2,3,4",
    "start 00000000 state 6",
    "after START: state 8 START to 0,1,1,1,0,0,0 acks 1,0,0,0,0,0,0|1,0,0,0,0,0,0|1,0,0,0,0,0,0",
    "clients 06010002 06010003 06010004",
    "pass 00000000 state 8",
    "after GO: state 9 GO to 0,1,1,1,0,0,0 closes 1",
    "Anna status 09010002 pass 7e5b0c1a nodes 4:1,2,3,4",
    "Elena status 09010003 pass 7e5b0c1a nodes 4:1,2,3,4",
    "Carla status 09010004 pass 7e5b0c1a nodes 4:1,2,3,4",
    "host clients 0: kicks 0,0,0,3,2,0 bad 0",
    "finalize 00000000 00000000 00000000 00000000",
    "orders H|SD|SD|SD|SD|SD",
    "calls 657 549 441 553 1313 103",
};
static const char* const kBefore3[] = {
    "init 00000000 accept 00000000 state 2",
    "announce 444c010100c0100004 ch 6 opens 1",
    "two nodes above: kicks 0,0,0,3,0,0 clients 0:",
    "one left: kicks 0,0,0,3,2,0 clients 0:",
    "Anna init 00000000 scan 00000000",
    "Bruno init 00000000 scan 00000000",
    "Carla init 00000000 scan 00000000",
    "Dario init 00000000 scan 00000000",
    "Anna lists 10 4/1",
    "Bruno lists 10 4/1",
    "Carla lists 10 4/1",
    "Dario lists 10 4/1",
    "Anna joins 00000000 status 05010002",
    "Bruno joins 00000000 status 05010003",
    "Carla joins 00000000 status 05010004",
    "host clients 3:2,3,4 states 05010002 05010003 05010004 c880afef c880afef",
    "Anna nodes 4:1,2,3,4",
    "Bruno nodes 4:1,2,3,4",
    "Carla nodes 4:1,2,3,4",
    "Dario lists 10 4/4",
    "the fifth joins c920affe after 3000 ms status 01000000",
    "host clients 3:2,3,4 kicks 0,0,0,3,2,0 present 4",
    "a node above, lobby full: kicks 0,0,0,3,2,0 clients 3:2,3,4",
    "Bruno stops 00000000",
    "host clients 2:2,4 Anna nodes 3:1,2,4",
    "Elena init 00000000 scan 00000000",
    "Elena lists 10 4/3",
    "Elena joins 00000000 status 05010003",
    "host clients 3:2,3,4 Carla nodes 4:1,2,3,4",
    "start 00000000 state 6",
    "after START: state 8 START to 0,1,1,1,0,0,0 acks 1,0,0,0,0,0,0|1,0,0,0,0,0,0|1,0,0,0,0,0,0",
    "clients 06010002 06010003 06010004",
    "pass 00000000 state 8",
    "after GO: state 9 GO to 0,1,1,1,0,0,0 closes 1",
    "Anna status 09010002 pass 7e5b0c1a nodes 4:1,2,3,4",
    "Elena status 09010003 pass 7e5b0c1a nodes 4:1,2,3,4",
    "Carla status 09010004 pass 7e5b0c1a nodes 4:1,2,3,4",
    "host clients 0: kicks 0,0,0,3,2,0 bad 0",
    "finalize 00000000 00000000 00000000 00000000",
    "orders H|SD|SD|SD|SD|SD",
    "calls 657 549 441 553 1313 103",
};
static const char* const kBefore1[] = {
    "init 00000000 accept 00000000 state 2",
    "announce 444c010100c0100002 ch 6 opens 1",
    "two nodes above: kicks 0,0,0,3,0,0 clients 0:",
    "one left: kicks 0,0,0,3,2,0 clients 0:",
    "Anna init 00000000 scan 00000000",
    "Bruno init 00000000 scan 00000000",
    "Carla init 00000000 scan 00000000",
    "Dario init 00000000 scan 00000000",
    "Anna lists 10 2/1",
    "Bruno lists 10 2/1",
    "Carla lists 10 2/1",
    "Dario lists 10 2/1",
    "Anna joins 00000000 status 05010002",
    "Bruno joins c920affe status 01000000",
    "Carla joins c920affe status 01000000",
    "host clients 1:2 states 05010002 c880afef c880afef c880afef c880afef",
    "Anna nodes 2:1,2",
    "Bruno nodes 0:",
    "Carla nodes 0:",
    "Dario lists 10 2/2",
    "the fifth joins c920affe after 3000 ms status 01000000",
    "host clients 1:2 kicks 0,0,0,3,2,0 present 2",
    "a node above, lobby full: kicks 0,0,0,6,2,0 clients 1:2",
    "Bruno stops 00000000",
    "host clients 1:2 Anna nodes 2:1,2",
    "Elena init 00000000 scan 00000000",
    "Elena lists 10 2/2",
    "Elena joins c920affe status 01000000",
    "host clients 1:2 Carla nodes 0:",
    "start 00000000 state 6",
    "after START: state 8 START to 0,1,0,0,0,0,0 acks 1,0,0,0,0,0,0|0,0,0,0,0,0,0|0,0,0,0,0,0,0",
    "clients 06010002 01000000 01000000",
    "pass 00000000 state 8",
    "after GO: state 9 GO to 0,1,0,0,0,0,0 closes 1",
    "Anna status 09010002 pass 7e5b0c1a nodes 2:1,2",
    "Elena status 01000000 pass  nodes 0:",
    "Carla status 01000000 pass  nodes 0:",
    "host clients 0: kicks 0,0,0,6,2,0 bad 0",
    "finalize 00000000 00000000 00000000 00000000",
    "orders H|SD|SD|SD|SD|SD",
    "calls 1547 1449 1256 1712 2213 1250",
};

static void HoldTrace(const char* what, const Trace& tr, const char* const* want, std::size_t n, bool but_the_kicks) {
    CHECK_EQ(tr.lines.size(), n);
    auto strip = [&](std::string s) {
        // (a title of eight ejects the nodes between its game's ask and its ceiling one by one, a title of four all with one timer: the count of the
        // Kick calls is the title's; everything else of the evening is the same)
        const std::size_t at = s.find("kicks ");
        if (but_the_kicks && at != std::string::npos) {
            std::size_t end = at + 6;
            while (end < s.size() && (s[end] == ',' || (s[end] >= '0' && s[end] <= '9'))) {
                ++end;
            }
            s = s.substr(0, at) + "kicks *" + s.substr(end);
        }
        return s;
    };
    for (std::size_t i = 0; i < n && i < tr.lines.size(); ++i) {
        if (strip(tr.lines[i]) != strip(want[i])) {
            std::fprintf(stderr, "  %s, line %zu:\n    now:    %s\n    before: %s\n", what, i + 1, tr.lines[i].c_str(), want[i]);
        }
        CHECK(strip(tr.lines[i]) == strip(want[i]));
    }
}

static void TestTitleOfFour() {
    std::printf("a whole evening of a title of four players, line by line as before the tables grew\n");
    struct Run {
        u32 asked;
        const char* const* want;
        std::size_t n;
    };
    static const Run kRuns[3] = {{7, kBefore7, sizeof(kBefore7) / sizeof(kBefore7[0])},
                                 {3, kBefore3, sizeof(kBefore3) / sizeof(kBefore3[0])},
                                 {1, kBefore1, sizeof(kBefore1) / sizeof(kBefore1[0])}};
    for (const Run& r : kRuns) {
        CHECK(r.n >= 40);
        {  // the configuration of before: zeroed, three fields (the block's own caller in the tool)
            Trace tr;
            TitleOfFour(tr, [](int i, const char* name) { return OldCfg(kKirbyLow, i, name); }, kKirbyLow, r.asked);
            HoldTrace("zeroed", tr, r.want, r.n, false);
        }
        for (const u8 said : {0, 4}) {  // 0 written out, and 4 written out: the same title
            Trace tr;
            TitleOfFour(tr, [said](int i, const char* name) { return CfgOf(kKirbyLow, i, name, said); }, kKirbyLow, r.asked);
            HoldTrace(said == 0 ? "0" : "4", tr, r.want, r.n, false);
        }
    }
    // a title of EIGHT whose game asks for three clients (or one) is a lobby of four (of two): the same evening, the Kick calls apart
    for (const Run& r : {kRuns[1], kRuns[2]}) {
        Trace tr;
        TitleOfFour(tr, [](int i, const char* name) { return CfgOf(kKirbyLow, i, name, 8); }, kKirbyLow, r.asked);
        HoldTrace("8", tr, r.want, r.n, true);
        // ... and its Kick calls: the two nodes above the game's ask have a slot each (2..8), so each is ejected at once and every 200 ms
        // (450 ms: three times each), where the title of four ejects one of them per 200 ms with its one timer ("kicks 0,0,0,3,0,0" above)
        CHECK(tr.lines.size() > 3 && tr.lines[2].find("kicks 0,0,0,3,3,0") != std::string::npos);
    }
    // the lines of before say what they must (read here once, so that an empty or a wrong table cannot pass for a match)
    CHECK(std::string(kBefore7[1]) == "announce 444c010100c0100004 ch 6 opens 1");
    CHECK(std::string(kBefore7[2]) == "two nodes above: kicks 0,0,0,3,0,0 clients 0:");
    CHECK(std::string(kBefore7[20]).find("the fifth joins c920affe after 3000 ms") == 0);
    CHECK(std::string(kBefore3[1]) == "announce 444c010100c0100004 ch 6 opens 1");
    CHECK(std::string(kBefore1[1]) == "announce 444c010100c0100002 ch 6 opens 1");
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. The session of eight on the in-memory air.

struct Eight {
    FakeAir air;
    std::vector<std::unique_ptr<Box>> own;
    std::vector<Box*> all;
    /// `n` consoles of Mario Kart 7 (ceiling 8): 0 the creator, the others look for it.
    explicit Eight(int n, u8 said = 8) {
        static const char* const kNames[10] = {"Host", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "Ninth", "Late"};
        for (int i = 0; i < n; ++i) {
            own.emplace_back(new Box(air, i, CfgOf(kMk7Low, i, kNames[i], said)));
            all.push_back(own.back().get());
        }
    }
    Box& h() {
        return *all[0];
    }
    void Run(u32 ms) {
        RunAir(air, all, ms);
    }
    /// The creator opens for `asked` clients; consoles 1..`guests` look for it, list it and enter one after the other.
    void Fill(u32 asked, int guests) {
        CHECK_EQ(h().SvInit(asked, "Host"), 0u);
        CHECK_EQ(h().SvAccept(0, 11), 0u);
        CHECK_EQ(h().SvState(), 2u);
        for (int i = 1; i <= guests; ++i) {
            CHECK_EQ(all[i]->FkInit("P"), 0u);
            CHECK_EQ(all[i]->FkScan(kMk7Low), 0u);
        }
        Run(300);
        for (int i = 1; i <= guests; ++i) {
            CHECK_EQ(JoinNow(air, all, *all[i], h(), kMk7Low, 3500), 0u);
            CHECK_EQ(all[i]->FkStatus(), 0x05010000u | static_cast<u32>(i + 1));
            Run(40);
        }
        Run(300);
    }
};

static void TestEightOnTheAir() {
    std::printf("eight players on the air: the search, seven enter, the ninth, the vectors of ids, START, GO\n");
    Eight e(10);
    Box& h = e.h();
    CHECK_EQ(h.SvInit(7, "Host"), 0u);
    CHECK_EQ(h.SvAccept(0, 11), 0u);
    CHECK_EQ(h.net.LastAnnounce().max_nodes, 8u);
    // eight consoles look for it (the ninth too) and list it: 8 places, one taken
    for (int i = 1; i <= 8; ++i) {
        CHECK_EQ(e.all[i]->FkInit("P"), 0u);
        CHECK_EQ(e.all[i]->FkScan(kMk7Low), 0u);
    }
    e.Run(300);
    for (int i = 1; i <= 8; ++i) {
        CHECK_EQ(e.all[i]->FkList(), std::string("10 8/1"));
    }
    // seven enter: nodes 2..8, in the order they came
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(JoinNow(e.air, e.all, *e.all[i], h, kMk7Low, 3500), 0u);
        CHECK_EQ(e.all[i]->FkStatus(), 0x05010000u | static_cast<u32>(i + 1));
        e.Run(40);
    }
    e.Run(300);
    CHECK_EQ(e.air.Present(0), 8);
    // the creator's vector of clients, of every size a game may give (Mario Kart 7 and Kirby give 15)
    CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(h.SvClients(7), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(h.SvClients(8), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(h.SvClients(6), std::string("7:2,3,4,5,6,7"));
    CHECK_EQ(h.SvClients(4), std::string("7:2,3,4,5"));
    CHECK_EQ(h.SvClients(3), std::string("7:2,3,4"));
    CHECK_EQ(h.SvClients(1), std::string("7:2"));
    CHECK_EQ(h.SvClients(0), std::string("7:"));
    CHECK_EQ(h.SvClients(60), std::string("7:2,3,4,5,6,7,8"));
    for (u32 node = 2; node <= 8; ++node) {
        CHECK_EQ(h.SvClientState(node), 0x05010000u | node);
    }
    CHECK_EQ(h.SvClientState(1), 0xC880AFEFu);
    CHECK_EQ(h.SvClientState(9), 0xC880AFEFu);
    CHECK_EQ(h.SvClientState(0x0102), 0xC880AFEFu);
    // every client's vector of nodes, and the record of every player
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(e.all[i]->FkNodes(15), std::string("8:1,2,3,4,5,6,7,8"));
    }
    Box& c = *e.all[4];
    CHECK_EQ(c.FkNodes(8), std::string("8:1,2,3,4,5,6,7,8"));
    CHECK_EQ(c.FkNodes(7), std::string("8:1,2,3,4,5,6,7"));
    CHECK_EQ(c.FkNodes(4), std::string("8:1,2,3,4"));
    CHECK_EQ(c.FkNodes(1), std::string("8:1"));
    CHECK_EQ(c.FkNodes(0), std::string("8:"));
    CHECK_EQ(c.FkNodes(60), std::string("8:1,2,3,4,5,6,7,8"));
    for (u32 node = 1; node <= 8; ++node) {
        c.Ask(kFk, 0x0E, {node});  // GetNodeInfo
        CHECK_EQ(c.w[1], 0u);
        CHECK_EQ(c.w[2 + 9], node);                                      // +0x24: the node id
        CHECK_EQ(c.w[2], node == 1 ? 0xF00D0000u : 0xF00D0000u + node - 1);  // the friend code seed the node entered with
    }
    c.Ask(kFk, 0x0E, {9});
    CHECK_EQ(c.w[1], 0xC880AFEFu);
    // the ninth: it lists the lobby as full (8/8: the games hide it), and if it knocks nobody answers - its join gives up by itself at 3000 ms
    Box& ninth = *e.all[8];
    CHECK_EQ(ninth.FkList(), std::string("10 8/8"));
    const u64 t0 = e.air.now();
    CHECK_EQ(JoinNow(e.air, e.all, ninth, h, kMk7Low, 4000), 0xC920AFFEu);
    CHECK_EQ(e.air.now() - t0, 3000u);
    CHECK_EQ(ninth.FkStatus(), 0x01000000u);
    CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
    for (int node = 2; node <= 9; ++node) {
        CHECK_EQ(h.net.Kicked(node), 0);  // nobody was thrown out: the engine never let it in
    }
    // a faulty engine lists a node 9 with the lobby full: it is past the eight members the lobby reads
    e.air.SetKickLag(100000);
    e.air.Phantom(0, 9, 0x9999, "Ghost9");
    e.Run(450);
    CHECK_EQ(h.net.Kicked(9), 0);
    CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
    // one leaves (node 4): the creator and the others see it go; now the faulty engine's node 9 is among the eight read, and it is thrown out
    // at once and every 200 ms (450 ms: three times), with no slot of its own
    CHECK_EQ(e.all[3]->FkStop(), 0u);
    e.Run(450);
    CHECK_EQ(h.net.Kicked(9), 3);
    CHECK_EQ(h.SvClients(15), std::string("6:2,3,5,6,7,8"));
    CHECK_EQ(e.all[1]->FkNodes(15), std::string("8:1,2,3,5,6,7,8,9"));  // (what the engine lists: the roster is the engine's)
    e.air.Leave(0, 9);
    e.air.SetKickLag(0);
    e.Run(300);
    CHECK_EQ(e.all[1]->FkNodes(15), std::string("7:1,2,3,5,6,7,8"));
    // the one that came late lists 8/7 and takes the place that was left (the lowest free id, as the engine gives it)
    Box& late = *e.all[9];
    CHECK_EQ(late.FkInit("Late"), 0u);
    CHECK_EQ(late.FkScan(kMk7Low), 0u);
    e.Run(300);
    CHECK_EQ(late.FkList(), std::string("10 8/7"));
    CHECK_EQ(JoinNow(e.air, e.all, late, h, kMk7Low, 3500), 0u);
    CHECK_EQ(late.FkStatus(), 0x05010004u);
    e.Run(300);
    CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(late.FkNodes(15), std::string("8:1,2,3,4,5,6,7,8"));
    // START: one to each of the seven, each answers once, the creator is at 8 and the clients at 6
    CHECK_EQ(h.SvStart(), 0u);
    CHECK_EQ(h.SvState(), 6u);
    CHECK_EQ(h.net.LastAnnounce().accepting, 0u);
    e.Run(100);
    CHECK_EQ(h.SvState(), 8u);
    for (int node = 2; node <= 8; ++node) {
        CHECK_EQ(h.net.Sent(1, node), 1);
        CHECK_EQ(h.SvClientState(static_cast<u32>(node)), 0x07010000u | static_cast<u32>(node));
    }
    CHECK_EQ(h.net.Sent(1, 1) + h.net.Sent(1, 9), 0);
    static const int kIn[7] = {1, 2, 9, 4, 5, 6, 7};  // the consoles in the session, by node 2..8
    for (int k = 0; k < 7; ++k) {
        CHECK_EQ(e.all[kIn[k]]->FkStatus(), 0x06010000u | static_cast<u32>(k + 2));
        CHECK_EQ(e.all[kIn[k]]->net.Sent(2, 1), 1);
    }
    // GO with the passphrase: one to each, the creator at 9 with its network closed, the clients at 9 with the nine bytes and the roster
    static const u8 kNine[9] = {'7', 'e', '5', 'b', '0', 'c', '1', 'a', 0};
    CHECK_EQ(h.SvPass(kNine), 0u);
    e.Run(100);
    CHECK_EQ(h.SvState(), 9u);
    CHECK_EQ(h.net.HostCloseCalls(), 1);
    for (int node = 2; node <= 8; ++node) {
        CHECK_EQ(h.net.Sent(3, node), 1);
    }
    for (int k = 0; k < 7; ++k) {
        Box& b = *e.all[kIn[k]];
        CHECK_EQ(b.FkStatus(), 0x09010000u | static_cast<u32>(k + 2));
        CHECK_EQ(b.FkPass(), std::string("7e5b0c1a"));
        CHECK_EQ(b.FkNodes(15), std::string("8:1,2,3,4,5,6,7,8"));
        CHECK_EQ(b.net.Sent(4, 1), 1);
        CHECK_EQ(b.lobby.bad_messages(), 0u);
    }
    CHECK_EQ(h.lobby.bad_messages(), 0u);
    CHECK_EQ(h.SvClients(15), std::string("0:"));
    for (int node = 2; node <= 8; ++node) {
        CHECK_EQ(h.net.Kicked(node), 0);
    }
    CHECK_EQ(e.air.overlaps(), 0);
}

/// FKCL GetServerInfo of `host` as the game of a console IN a session asks it - the record of the lobby it is in: "max/players now" (the bytes +8 and +9
/// of the record: the games show them as the places of the lobby), or "err xxxxxxxx".
static std::string ServerInfoOf(Box& b, const Mac& host) {
    std::memset(b.mem.data(), 0xCC, b.mem.size());
    const u32 sb_server[2] = {(0x298u << 14) | 2, kGameBase};
    const u32 w1 = static_cast<u32>(host[0]) | (static_cast<u32>(host[1]) << 8) | (static_cast<u32>(host[2]) << 16) | (static_cast<u32>(host[3]) << 24);
    const u32 w2 = static_cast<u32>(host[4]) | (static_cast<u32>(host[5]) << 8);
    b.Ask(kFk, 0x07, {w1, w2}, sb_server);
    char line[32];
    if (b.w[1] != 0) {
        std::snprintf(line, sizeof(line), "err %08x", b.w[1]);
    } else {
        std::snprintf(line, sizeof(line), "%u/%u", b.mem[kServerInfoMaxClients], b.mem[kServerInfoJoined]);
    }
    return line;
}

/// What a client IN the session of eight reads about its own lobby: the count DlpLobby::ReadRoster keeps for the host it joined (every 250 ms), which
/// GetServerInfo / GetTitleInfo hand to the game. With two to four players in, the guard of the titles of four holds it; here it is read by EVERY client
/// already in after EVERY entry up to eight, and on the way down through four (the review of 2026-11-13: a count that stopped at four, one that was not
/// refreshed after the fifth entry and one that jumped to eight passed every test).
static void TestEightCountsItself() {
    std::printf("eight players on the air: every client in the session reads how many are in, up to eight and down again\n");
    Eight e(8);
    Box& h = e.h();
    const Mac host = h.net.mac();
    CHECK_EQ(h.SvInit(7, "Host"), 0u);
    CHECK_EQ(h.SvAccept(0, 11), 0u);
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(e.all[i]->FkInit("P"), 0u);
        CHECK_EQ(e.all[i]->FkScan(kMk7Low), 0u);
    }
    e.Run(300);
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(JoinNow(e.air, e.all, *e.all[i], h, kMk7Low, 3500), 0u);
        CHECK_EQ(e.all[i]->FkStatus(), 0x05010000u | static_cast<u32>(i + 1));
        CHECK_EQ(ServerInfoOf(*e.all[i], host), "8/" + std::to_string(i + 1));  // at once: the roster is read as the session is entered
        e.Run(600);
        for (int k = 1; k <= i; ++k) {
            CHECK_EQ(ServerInfoOf(*e.all[k], host), "8/" + std::to_string(i + 1));  // every client already in, after every entry
            CHECK_EQ(e.all[k]->FkNodes(15).substr(0, 2), std::to_string(i + 1) + ":");
        }
    }
    // the record is the one of the host joined and of nobody else
    CHECK_EQ(ServerInfoOf(*e.all[1], e.all[2]->net.mac()), std::string("err c880afef"));
    // one leaves (the console of node 4): the six that stay read 8/7
    CHECK_EQ(e.all[3]->FkStop(), 0u);
    e.Run(600);
    for (const int k : {1, 2, 4, 5, 6, 7}) {
        CHECK_EQ(ServerInfoOf(*e.all[k], host), std::string("8/7"));
    }
    CHECK_EQ(ServerInfoOf(*e.all[3], host), std::string("err c880afef"));  // (the one that left has no session and no list)
    // two more leave: 8/5; one more: 8/4 - the count comes down through four as it went up
    CHECK_EQ(e.all[7]->FkStop(), 0u);
    CHECK_EQ(e.all[6]->FkStop(), 0u);
    e.Run(600);
    for (const int k : {1, 2, 4, 5}) {
        CHECK_EQ(ServerInfoOf(*e.all[k], host), std::string("8/5"));
    }
    CHECK_EQ(e.all[5]->FkStop(), 0u);
    e.Run(600);
    for (const int k : {1, 2, 4}) {
        CHECK_EQ(ServerInfoOf(*e.all[k], host), std::string("8/4"));
    }
    // ... and up again: the one that left comes back and takes the lowest free id; everybody reads 8/5
    CHECK_EQ(e.all[3]->FkScan(kMk7Low), 0u);
    e.Run(300);
    CHECK_EQ(e.all[3]->FkList(), std::string("10 8/4"));
    CHECK_EQ(JoinNow(e.air, e.all, *e.all[3], h, kMk7Low, 3500), 0u);
    e.Run(600);
    for (const int k : {1, 2, 3, 4}) {
        CHECK_EQ(ServerInfoOf(*e.all[k], host), std::string("8/5"));
    }
    CHECK_EQ(h.SvClients(15), std::string("4:2,3,4,5"));
}

static void TestEightLosesOne() {
    std::printf("eight players on the air: a client that never answers START, manual acceptance, the creator that throws one out\n");
    {
        Eight e(8);
        e.Fill(7, 7);
        Box& h = e.h();
        CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
        e.air.Drop(2, 6, 1, FakeAir::kForever);  // the START_ACK of node 6 never arrives
        CHECK_EQ(h.SvStart(), 0u);
        e.Run(1400);
        CHECK_EQ(h.SvState(), 6u);  // still waiting for it: START again every 200 ms, to it alone
        CHECK_EQ(h.net.Sent(1, 6), 8);
        for (const int node : {2, 3, 4, 5, 7, 8}) {
            CHECK_EQ(h.net.Sent(1, node), 1);
        }
        e.Run(200);
        CHECK_EQ(h.SvState(), 8u);  // 1500 ms: it is thrown out, the other six go on
        CHECK_EQ(h.net.Kicked(6), 1);
        CHECK_EQ(h.SvClients(15), std::string("6:2,3,4,5,7,8"));
        static const u8 kNine[9] = {'0', '1', '2', '3', '4', '5', '6', '7', 0};
        CHECK_EQ(h.SvPass(kNine), 0u);
        e.Run(100);
        CHECK_EQ(h.SvState(), 9u);
        for (const int i : {1, 2, 3, 4, 6, 7}) {
            CHECK_EQ(e.all[i]->FkStatus() >> 24, 9u);
            CHECK_EQ(e.all[i]->FkPass(), std::string("01234567"));
        }
        e.Run(2000);
        CHECK_EQ(e.all[5]->FkStatus() >> 24, 0x80u);  // the one thrown out: its host is gone for it (2000 ms after the Kick)
    }
    {  // manual acceptance: the seven wait (0x40), six are accepted, the seventh is thrown out by StartDistribution
        Eight e(8);
        Box& h = e.h();
        CHECK_EQ(h.SvInit(7, "Host"), 0u);
        CHECK_EQ(h.SvAccept(1, 11), 0u);
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(e.all[i]->FkInit("P"), 0u);
            CHECK_EQ(e.all[i]->FkScan(kMk7Low), 0u);
        }
        e.Run(300);
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(JoinNow(e.air, e.all, *e.all[i], h, kMk7Low, 3500), 0u);
            e.Run(40);
        }
        e.Run(100);
        CHECK_EQ(h.SvClients(15), std::string("7:2,3,4,5,6,7,8"));
        for (u32 node = 2; node <= 8; ++node) {
            CHECK_EQ(h.SvClientState(node), 0x40000000u | node);
        }
        for (u32 node = 2; node <= 7; ++node) {
            h.Ask(kSv, 0x09, {node});  // AcceptClient
            CHECK_EQ(h.w[1], 0u);
        }
        h.Ask(kSv, 0x09, {9});
        CHECK_EQ(h.w[1], 0xC880AFEFu);
        h.Ask(kSv, 0x0A, {3});  // DisconnectClient: node 3 goes
        CHECK_EQ(h.w[1], 0u);
        CHECK_EQ(h.net.Kicked(3), 1);
        e.Run(100);
        CHECK_EQ(h.SvClients(15), std::string("6:2,4,5,6,7,8"));
        CHECK_EQ(h.SvStart(), 0u);
        CHECK_EQ(h.net.Kicked(8), 1);  // never accepted
        e.Run(100);
        CHECK_EQ(h.SvState(), 8u);
        CHECK_EQ(h.SvClients(15), std::string("5:2,4,5,6,7"));
        for (const int node : {2, 4, 5, 6, 7}) {
            CHECK_EQ(h.net.Sent(1, node), 1);
        }
        CHECK_EQ(h.net.Sent(1, 3) + h.net.Sent(1, 8), 0);
    }
    {  // a title of eight whose game asks for five clients: a lobby of six, the seventh console is never answered
        Eight e(8);
        e.Fill(5, 5);
        Box& h = e.h();
        CHECK_EQ(h.net.LastAnnounce().max_nodes, 6u);
        CHECK_EQ(h.SvClients(15), std::string("5:2,3,4,5,6"));
        CHECK_EQ(e.all[6]->FkInit("P"), 0u);
        CHECK_EQ(e.all[6]->FkScan(kMk7Low), 0u);
        e.Run(300);
        CHECK_EQ(e.all[6]->FkList(), std::string("10 6/6"));
        CHECK_EQ(JoinNow(e.air, e.all, *e.all[6], h, kMk7Low, 4000), 0xC920AFFEu);
        // a node 7 and a node 9 from a faulty engine: 7 is above what the game asked and has a slot (ejected at once, and every 200 ms), 9 is above
        // the ceiling of the title and has none (the shared timer)
        e.air.SetKickLag(100000);
        e.air.Phantom(0, 7, 0x7777, "Ghost7");
        e.air.Phantom(0, 9, 0x9999, "Ghost9");
        e.Run(450);
        CHECK_EQ(h.net.Kicked(7), 3);
        CHECK_EQ(h.net.Kicked(9), 3);
        CHECK_EQ(h.SvClients(15), std::string("5:2,3,4,5,6"));
        CHECK_EQ(h.SvClientState(7), 0xC880AFEFu);
        // START and GO with the two still listed (node 7 holds its slot as "ejected", which is not a client): nothing is ever written to them, the
        // five go on and end
        CHECK_EQ(h.SvStart(), 0u);
        e.Run(100);
        CHECK_EQ(h.SvState(), 8u);
        static const u8 kNine[9] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0};
        CHECK_EQ(h.SvPass(kNine), 0u);
        e.Run(100);
        CHECK_EQ(h.SvState(), 9u);
        for (int node = 2; node <= 6; ++node) {
            CHECK_EQ(h.net.Sent(1, node), 1);
            CHECK_EQ(h.net.Sent(3, node), 1);
        }
        for (const int node : {7, 9}) {
            CHECK_EQ(h.net.Sent(1, node) + h.net.Sent(3, node), 0);  // (no START and no GO to a node that is not a client)
        }
        for (int i = 1; i <= 5; ++i) {
            CHECK_EQ(e.all[i]->FkStatus() >> 24, 9u);
            CHECK_EQ(e.all[i]->FkPass(), std::string("abcdefgh"));
        }
    }
}

static void TestEightHostGoes() {
    std::printf("eight players on the air: the creator goes\n");
    for (const bool vanish : {false, true}) {
        Eight e(8);
        e.Fill(7, 7);
        Box& h = e.h();
        if (vanish) {
            e.air.Vanish(0);  // the console fell off the room: no word to anybody
        } else {
            CHECK_EQ(h.SvFinalize(), 0u);  // the game left its menu
            CHECK_EQ(h.net.HostCloseCalls(), 1);
        }
        e.Run(1900);
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(e.all[i]->FkStatus() >> 24, 5u);  // not yet: the host may come back for 2000 ms
        }
        e.Run(200);
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(e.all[i]->FkStatus(), 0x80000000u);  // lost: the games' error dialog
            CHECK_EQ(e.all[i]->FkNodes(15), std::string("0:"));
            CHECK_EQ(e.all[i]->net.DisconnectCalls(), 1);
            CHECK_EQ(e.all[i]->FkStop(), 0u);
            CHECK_EQ(e.all[i]->FkStatus(), 0x01000000u);
        }
        if (vanish) {
            CHECK_EQ(h.SvState(), 1u);  // its own network is lost: back to 1 after the same 2000 ms
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. On the real engine: seven that enter together, the ninth, START, GO, and the ring of the lobby's messages.

/// Counts, frame by frame, what reaches a console's ring of lobby messages: how full it was when each message came (the high-water mark), and how
/// many came with the ring full (each of those pushes the oldest message out). Attached before the traffic starts.
struct RingWatch {
    std::atomic<u32> frames{0};
    std::atomic<u32> high{0};
    std::atomic<u32> lost{0};
    void Attach(Player& p) {
        UdsEngine* engine = &p.e();
        p.con.tap = [this, engine](const WifiPacket& packet) {
            SecureDataHeader sd;
            EtherType type;
            if (packet.type != WifiPacket::Type::Data || !GetFrameEtherType(packet.data, &type) || type != EtherType::SecureData ||
                !ParseSecureDataHeader(packet.data, &sd) || sd.data_channel != UdsEngine::kLobbyChannel) {
                return;
            }
            u32 held = 0;
            {
                platform::LockGuard<platform::ContendedMutex> lock(engine->mu_);
                held = engine->lobby_ ? engine->lobby_->rx_count : 0;
            }
            ++frames;
            if (held >= UdsEngine::kLobbyInbox) {
                ++lost;
            }
            const u32 after = held < UdsEngine::kLobbyInbox ? held + 1 : UdsEngine::kLobbyInbox;  // (what the ring holds once this one is in)
            u32 seen = high.load();
            while (after > seen && !high.compare_exchange_weak(seen, after)) {
            }
        };
    }
};

struct EightEngines {
    MockRoom room;
    std::vector<std::unique_ptr<Player>> own;
    Players ps;
    std::unique_ptr<Ticker> ticker;
    RingWatch host_ring;
    RingWatch client_ring;
    /// Console 0 is the creator (MAC ...01), 1..n-1 the others; all of them Mario Kart 7 with the ceiling `said`.
    explicit EightEngines(int n, u8 said = 8) {
        static const char* const kNames[10] = {"Host", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "Ninth", "Late"};
        for (int i = 0; i < n; ++i) {
            own.emplace_back(new Player(room, static_cast<u8>(i + 1), kNames[i], kMk7Low, said));
            ps.push_back(own.back().get());
        }
        host_ring.Attach(*ps[0]);
        client_ring.Attach(*ps[1]);
        ticker.reset(new Ticker(Engines(ps)));
    }
    Player& h() {
        return *ps[0];
    }
    /// Pumps the consoles of `who` only (the others' network threads are stalled) until done() or `ms`.
    template <typename F>
    bool PumpOnly(const Players& who, u32 ms, F done) {
        return Until(who, ms, done);
    }
    static std::string Roster(Player& p) {
        u8 ids[kMaxPlayers] = {};
        u32 total = 0;
        p.lobby.ClientConnectingNodes(ids, kMaxPlayers, &total);
        std::string s = std::to_string(total) + ":";
        for (u32 i = 0; i < total && i < kMaxPlayers; ++i) {
            s += (i ? "," : "") + std::to_string(ids[i]);
        }
        return s;
    }
    static std::string Clients(Player& p) {
        u8 ids[kMaxClients] = {};
        u32 total = 0;
        p.lobby.ServerConnectingClients(ids, kMaxClients, &total);
        std::string s = std::to_string(total) + ":";
        for (u32 i = 0; i < total && i < kMaxClients; ++i) {
            s += (i ? "," : "") + std::to_string(ids[i]);
        }
        return s;
    }
    /// The creator opens for seven; consoles 1..7 hear it, then ALL SEVEN ask to enter in the same round. False = they are not all in (the checks
    /// that failed are counted): the caller's scenario has nothing to stand on.
    bool SevenEnterTogether() {
        bool entered = true;
        OpenLobby(h(), 7, 11);
        CHECK_EQ(h().e().GetConnectionStatus().max_nodes, 8u);  // the network the room carries: "hosting network (max 8 nodes ...)"
        for (int i = 1; i <= 7; ++i) {
            CHECK(FindGroup(ps, *ps[i], h()));
        }
        u32 res[8] = {};
        bool done[8] = {};
        for (int i = 1; i <= 7; ++i) {
            ps[i]->lobby.ClientStopScan();
            res[i] = 0xFFFFFFFF;
            done[i] = ps[i]->lobby.ClientJoinBegin(h().mac(), &res[i]) == JoinStep::Failed;
        }
        CHECK(Until(ps, 6000, [&] {
            bool all = true;
            for (int i = 1; i <= 7; ++i) {
                done[i] = done[i] || ps[i]->lobby.ClientJoinPoll(&res[i]);
                all = all && done[i];
            }
            return all;
        }));
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(res[i], 0u);
            CHECK_EQ(CliState(*ps[i]), 5u);
            entered = entered && res[i] == 0 && CliState(*ps[i]) == 5;
        }
        if (!entered) {
            return false;  // (counted above: the caller stops its scenario)
        }
        CHECK(Until(ps, 4000, [&] {
            if (Clients(h()) != "7:2,3,4,5,6,7,8") {
                return false;
            }
            for (int i = 1; i <= 7; ++i) {
                if (Roster(*ps[i]) != "8:1,2,3,4,5,6,7,8") {
                    return false;
                }
            }
            return true;
        }));
        u16 node = 0, mask = 0;
        CHECK(h().e().NodeSnapshot(&node, &mask));
        CHECK_EQ(mask, 0x00FFu);
        // every node id 2..8 is one console's, and each console has another
        u32 seen = 0;
        for (int i = 1; i <= 7; ++i) {
            const u32 n = CliNode(*ps[i]);
            CHECK(n >= 2 && n <= 8);
            seen |= 1u << n;
        }
        CHECK_EQ(seen, 0x1FCu);
        // the eight players by name, as a client's game reads them (GetNodeInfo of every node)
        std::string names;
        for (u16 n = 1; n <= 8; ++n) {
            NodeEntry ne = {};
            CHECK_EQ(ps[1]->lobby.ClientNodeInfo(n, &ne), 0u);
            CHECK_EQ(ne.node, n);
            names += Str(ne.name) + " ";
        }
        CHECK(names.find("Host ") == 0);
        for (const char* name : {"P2 ", "P3 ", "P4 ", "P5 ", "P6 ", "P7 ", "P8 "}) {
            CHECK(names.find(name) != std::string::npos);
        }
        return true;
    }
};

static void TestEightOnTheEngine() {
    std::printf("eight players on the real engine: seven enter together, the ninth, one leaves and the ninth takes its place, START, GO; the ring\n");
    g_rules.Reset();
    EightEngines e(9);
    Player& h = e.h();
    if (!e.SevenEnterTogether()) {
        return;
    }
    // the ninth console hears the lobby, full (8 of 8), and its join is never answered: it gives up by itself and gives the engine back
    Player& ninth = *e.ps[8];
    CHECK(FindGroup(e.ps, ninth, h));
    GroupView v;
    const bool ninth_lists = ninth.lobby.ClientTitleInfo(h.mac(), &v) == 0;
    CHECK(ninth_lists);
    if (!ninth_lists) {
        return;  // (nothing below can be read without the entry: the failure is counted, the test goes on with its next part)
    }
    CHECK_EQ(v.group->max_nodes, 8u);
    CHECK(Until(e.ps, 2000, [&] { return ninth.lobby.ClientTitleInfo(h.mac(), &v) == 0 && v.group->total_nodes == 8; }));
    u32 res = 0;
    CHECK(Join(e.ps, ninth, h, &res, 5000));
    CHECK(res == R_TIMEOUT || res == 0xC880AC05u);  // (the engine's host refuses a ninth node: "not found" at once, or silence until the 3000 ms)
    CHECK(res != 0);
    CHECK_EQ(CliState(ninth), 1u);
    CHECK(EngineClean(ninth));
    CHECK_EQ(EightEngines::Clients(h), std::string("7:2,3,4,5,6,7,8"));
    u16 node = 0, mask = 0;
    CHECK(h.e().NodeSnapshot(&node, &mask));
    CHECK_EQ(mask, 0x00FFu);
    // one of the seven leaves: the creator and the six that stay see it go (the engine's host tells the other clients of a lobby who is left) ...
    Player& gone = *e.ps[3];
    const u32 freed = CliNode(gone);
    CHECK_EQ(gone.lobby.ClientStopSession(), 0u);
    CHECK(EngineClean(gone));
    std::string left7 = "7:", left6 = "6:";
    for (u32 n = 1; n <= 8; ++n) {
        if (n != freed) {
            left7 += (left7.size() > 2 ? "," : "") + std::to_string(n);
            if (n != 1) {
                left6 += (left6.size() > 2 ? "," : "") + std::to_string(n);
            }
        }
    }
    Players in;  // the seven consoles in the session from here on: the six that stayed, and the ninth
    for (int i = 1; i <= 8; ++i) {
        if (i != 3) {
            in.push_back(e.ps[i]);
        }
    }
    CHECK(Until(e.ps, 4000, [&] {
        if (EightEngines::Clients(h) != left6) {
            return false;
        }
        for (Player* p : in) {
            if (p != &ninth && EightEngines::Roster(*p) != left7) {
                return false;
            }
        }
        return true;
    }));
    // ... and the ninth, that was never answered, now lists 8/7 and takes the place that was left
    CHECK(FindGroup(e.ps, ninth, h));
    CHECK(Until(e.ps, 2000, [&] { return ninth.lobby.ClientTitleInfo(h.mac(), &v) == 0 && v.group->total_nodes == 7; }));
    CHECK(Join(e.ps, ninth, h, &res, 5000));
    CHECK_EQ(res, 0u);
    CHECK_EQ(CliNode(ninth), freed);
    CHECK(Until(e.ps, 4000, [&] {
        if (EightEngines::Clients(h) != "7:2,3,4,5,6,7,8") {
            return false;
        }
        for (Player* p : in) {
            if (EightEngines::Roster(*p) != "8:1,2,3,4,5,6,7,8") {
                return false;
            }
        }
        return true;
    }));
    // START to seven, then GO: everybody at 9 with the passphrase, nobody thrown out
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(e.ps, 3000, [&] { return h.lobby.ServerState() == 8; }));
    CHECK_EQ(EightEngines::Clients(h), std::string("7:2,3,4,5,6,7,8"));
    for (Player* p : in) {
        CHECK(Until(e.ps, 2000, [&] { return CliState(*p) == 6; }));
    }
    CHECK_EQ(CliState(gone), 1u);
    CHECK_EQ(h.lobby.ServerSendPassphrase(kNine), 0u);
    CHECK(Until(e.ps, 4000, [&] {
        if (h.lobby.ServerState() != 9) {
            return false;
        }
        for (Player* p : in) {
            if (CliState(*p) != 9) {
                return false;
            }
        }
        return true;
    }));
    for (Player* p : in) {
        u8 pass[9] = {};
        CHECK_EQ(p->lobby.ClientPassphrase(pass), 0u);
        CHECK(std::memcmp(pass, kNine, 9) == 0);
        CHECK_EQ(EightEngines::Roster(*p), std::string("8:1,2,3,4,5,6,7,8"));
        CHECK_EQ(p->lobby.bad_messages(), 0u);
    }
    CHECK_EQ(h.lobby.bad_messages(), 0u);
    CHECK(EngineClean(h));
    // the ring: 14 answers reached the creator (7 START_ACK, 7 GO_ACK), a few more if one was sent again; never more than seven wait there at once
    std::printf("  the creator's ring: %u messages, at most %u waiting at once (the ring holds %u), %u pushed out; a client's: %u, at most %u, %u pushed out\n",
                e.host_ring.frames.load(), e.host_ring.high.load(), UdsEngine::kLobbyInbox, e.host_ring.lost.load(), e.client_ring.frames.load(),
                e.client_ring.high.load(), e.client_ring.lost.load());
    CHECK(e.host_ring.frames.load() >= 14);
    CHECK(e.host_ring.high.load() <= 7);
    CHECK_EQ(e.host_ring.lost.load(), 0u);
    CHECK_EQ(e.client_ring.lost.load(), 0u);
    CHECK_RULES();
}

static void TestEightStalls() {
    std::printf("eight players on the real engine: the creator's network thread stalls, then the seven clients' do\n");
    g_rules.Reset();
    {  // the creator is not pumped for 700 ms after START: the seven answers wait in its ring (7 of 8), nothing is lost, nobody is thrown out
        EightEngines e(8);
        Player& h = e.h();
        if (!e.SevenEnterTogether()) {
            return;
        }
        Players clients(e.ps.begin() + 1, e.ps.end());
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
        e.PumpOnly(clients, 700, [] { return false; });
        std::printf("  creator stalled 700 ms after START: %u messages in its ring at most (it holds %u), %u pushed out\n", e.host_ring.high.load(),
                    UdsEngine::kLobbyInbox, e.host_ring.lost.load());
        CHECK(e.host_ring.high.load() <= 7);
        CHECK_EQ(e.host_ring.lost.load(), 0u);
        CHECK(Until(e.ps, 2000, [&] { return h.lobby.ServerState() == 8; }));
        CHECK_EQ(EightEngines::Clients(h), std::string("7:2,3,4,5,6,7,8"));
        CHECK_EQ(h.lobby.ServerSendPassphrase(kNine), 0u);
        e.PumpOnly(clients, 700, [] { return false; });
        CHECK_EQ(e.host_ring.lost.load(), 0u);
        CHECK(Until(e.ps, 3000, [&] { return h.lobby.ServerState() == 9; }));
        for (int i = 1; i <= 7; ++i) {
            CHECK_EQ(CliState(*e.ps[i]), 9u);
        }
    }
    {  // the seven clients are not pumped for 700 ms after START (they load the same screen): the creator sends START again at 200, 400 and 600 ms,
        // each client answers every one of them when it wakes - up to 28 answers in one breath for a ring of 8. What is pushed out is asked again
        // 200 ms later: everybody is in at state 8, long before the 1500 ms that throw a silent client out
        EightEngines e(8);
        Player& h = e.h();
        if (!e.SevenEnterTogether()) {
            return;
        }
        const Players host_only = {&h};
        CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
        e.PumpOnly(host_only, 700, [] { return false; });
        CHECK_EQ(h.lobby.ServerState(), 6u);
        const u64 woke = platform::NowMs();
        CHECK(Until(e.ps, 2000, [&] { return h.lobby.ServerState() == 8; }));
        const u64 took = platform::NowMs() - woke;
        std::printf("  seven clients stalled 700 ms after START: %u answers reached the creator, its ring full at %u, %u pushed out; state 8 came %llu ms"
                    " after they woke; a client's ring: at most %u waiting, %u pushed out\n",
                    e.host_ring.frames.load(), e.host_ring.high.load(), e.host_ring.lost.load(), static_cast<unsigned long long>(took),
                    e.client_ring.high.load(), e.client_ring.lost.load());
        CHECK(took <= 750);  // (the answers of the burst, or of ONE more round of START: well inside the 1500 ms of the grace)
        CHECK_EQ(EightEngines::Clients(h), std::string("7:2,3,4,5,6,7,8"));  // nobody was thrown out
        CHECK_EQ(e.client_ring.lost.load(), 0u);                             // a client's ring holds the four STARTs
        CHECK_EQ(h.lobby.ServerSendPassphrase(kNine), 0u);
        CHECK(Until(e.ps, 4000, [&] {
            for (int i = 1; i <= 7; ++i) {
                if (CliState(*e.ps[i]) != 9) {
                    return false;
                }
            }
            return h.lobby.ServerState() == 9;
        }));
    }
    CHECK_RULES();
}

static void TestFourOnTheEngine() {
    std::printf("a title of four on the real engine, beside the title of eight; the real Download Play stays at four\n");
    g_rules.Reset();
    {  // the same request (seven clients, what Mario Kart 7 asks) on a title of four: the network the room carries has 4 nodes, as 1.1.4's
        MockRoom room;
        Player four(room, 1, "Four", kMk7Low), zero(room, 2, "Zero", kMk7Low, 0), eight(room, 3, "Eight", kMk7Low, 8), six(room, 4, "Six", kMk7Low, 6);
        const Players ps = {&four, &zero, &eight, &six};
        Ticker ticker(Engines(ps));
        struct Want {
            Player* p;
            u32 asked;
            u32 nodes;
        };
        const Want want[] = {{&four, 7, 4}, {&zero, 7, 4}, {&eight, 7, 8}, {&six, 7, 6},   {&four, 255, 4}, {&eight, 255, 8},
                             {&four, 3, 4}, {&eight, 3, 4}, {&eight, 4, 5}, {&four, 0, 2}, {&eight, 0, 2},  {&eight, 1, 2}};
        for (const Want& w : want) {
            OpenLobby(*w.p, w.asked, 11);
            CHECK_EQ(w.p->e().GetConnectionStatus().max_nodes, w.nodes);
            CHECK_EQ(w.p->lobby.ServerFinalize(), 0u);
            CHECK(EngineClean(*w.p));
        }
    }
    {  // a searcher of a title of four (the configuration of before) does not list a lobby of eight of its own game, and cannot enter it; it lists and
        // enters the lobby of the same title of eight when the game asked for three clients (a lobby of four)
        MockRoom room;
        Player host(room, 1, "Host", kMk7Low, 8), old(room, 2, "Old", kMk7Low), now(room, 3, "Now", kMk7Low, 8);
        const Players ps = {&host, &old, &now};
        Ticker ticker(Engines(ps));
        OpenLobby(host, 7, 11);
        CHECK(FindGroup(ps, now, host));  // the console of eight lists it ...
        CHECK_EQ(old.lobby.ClientInitialize(nullptr, 4), 0u);
        CHECK_EQ(old.lobby.ClientStartScan(0), 0u);
        Settle(ps, 1200);  // ... the one of four never does (a host beacons every 102 ms)
        GroupView v;
        CHECK_EQ(old.lobby.ClientTitleInfoInOrder(true, &v), R_NO_DATA);
        CHECK_EQ(old.lobby.ClientTitleInfo(host.mac(), &v), R_NO_DATA);
        u32 res = 0;
        CHECK(Join(ps, old, host, &res));
        CHECK_EQ(res, R_NO_DATA);
        CHECK(EngineClean(old));
        CHECK_EQ(host.lobby.ServerFinalize(), 0u);
        Settle(ps, 600);
        OpenLobby(host, 3, 11);
        CHECK(FindGroup(ps, old, host));
        const bool old_lists = old.lobby.ClientTitleInfo(host.mac(), &v) == 0;
        CHECK(old_lists);
        if (old_lists) {
            CHECK_EQ(v.group->max_nodes, 4u);
        }
        CHECK(Join(ps, old, host, &res));
        CHECK_EQ(res, 0u);
        CHECK_EQ(CliState(old), 5u);
    }
    {  // beacons made by hand, as another console might send them: a lobby that says 9 or 16 nodes is nobody's (the format ends at 8), one of 5 or 8
        // is listed by a title of eight and never by a title of four, one of 2 or 4 by both
        MockRoom room;
        Player four(room, 1, "Four", kMk7Low), eight(room, 2, "Eight", kMk7Low, 8);
        const Players ps = {&four, &eight};
        Ticker ticker(Engines(ps));
        for (Player* p : ps) {
            CHECK_EQ(p->lobby.ClientInitialize(nullptr, 4), 0u);
            CHECK_EQ(p->lobby.ClientStartScan(0), 0u);
        }
        for (const u32 max : {9u, 16u, 2u, 5u, 4u, 8u}) {
            HostAnnounce a = {};
            a.child_tid_low = kMk7Low;
            a.max_nodes = static_cast<u8>(max);
            a.accepting = 1;
            FakeAir::Name(a.name, "Made");
            u8 app[kAnnounceSize];
            EncodeAnnounce(a, app);
            Mac from = kFake1;
            from[5] = static_cast<u8>(0x80 + max);
            for (int round = 0; round < 3; ++round) {
                for (Player* p : ps) {
                    p->e().OnWifiPacket(MakeBeacon(from, UdsEngine::kLobbyWlanCommId, UdsEngine::kLobbyId8, 1, static_cast<u8>(max), app, kAnnounceSize, 11));
                }
                Settle(ps, 60);
            }
            GroupView v;
            CHECK_EQ(four.lobby.ClientTitleInfo(from, &v), max >= 2 && max <= 4 ? 0u : R_NO_DATA);
            const u32 by_eight = eight.lobby.ClientTitleInfo(from, &v);
            CHECK_EQ(by_eight, max >= 2 && max <= 8 ? 0u : R_NO_DATA);
            if (by_eight == 0) {
                CHECK_EQ(v.group->max_nodes, max);
            }
        }
    }
    {  // the adapter of the real Download Play that nobody told (Luigi's Mansion): its ceiling is four whatever its table and the lobby's hold
       // (the table holds host + 7 clients since Mario Kart 7 speaks the real Download Play: Tests/dlp_real_mk7_test.cpp)
        MockRoom room;
        Console con(room, 9);
        RealDlpNet real(con.engine.get());
        CHECK_EQ(RealDlpNet::kRealPlayers, 8u);
        CHECK_EQ(RealDlpNet::kRealClients, 7u);
        CHECK_EQ(sizeof(real.peers_) / sizeof(real.peers_[0]), 7u);
        CHECK_EQ(real.players(), 4u);
        const Identity me = Me(0xF009, "Luigi");
        for (const u32 nodes : {5u, 6u, 8u, 9u, 16u, 1u, 0u}) {
            CHECK(!real.HostOpen(Ann(nodes), 6, me));
            CHECK(!real.Holds());
            CHECK(!con.engine->initialized());
        }
        for (const u32 nodes : {2u, 3u, 4u}) {
            CHECK(real.HostOpen(Ann(nodes), 6, me));
            CHECK_EQ(con.engine->GetConnectionStatus().max_nodes, nodes);
            real.HostClose();
            CHECK(!con.engine->initialized());
        }
        CHECK(real.HostOpen(Ann(4), 6, me));
        for (u8 node = 0; node < 12; ++node) {
            CHECK_EQ(real.PeerState(node), 0u);  // (no peer: and no dialogue of a node above the ceiling of four)
            CHECK(!real.Send(node, kMsg, 2) || (node >= 2 && node <= 4));
        }
        real.HostClose();
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// 7. The lobby and its service on a HOSTILE network and under a hostile game, for every kind of ceiling a title may have (the adversarial tests of the
// lobby, Tests/dlp_adv_lobby_test.cpp and dlp_adv_ipc_test.cpp, build every lobby from a zeroed configuration: they never walk the places 4..7 of the
// tables of a title of eight). The network lies: Members gives the ids 0, 8, 9, 15, 16, 255, doubles, not ascending, a count up to 255; Receive gives
// messages from any id, with a size that lies; ScanRead gives lobbies of any size. The game lies too: every command of both services with words out of
// a pool of hostile values. A fixed seed and a fixed number of commands: the same run every time. Held after every command: the ceiling never changes;
// an announcement never says more nodes than the ceiling; a message goes only to node 1 (a searcher) or to a client 2..ceiling (a creator); Kick is
// never asked for node 0; the totals and the ids the game reads are inside the ceiling, ascending, and never more than its vector holds; and nothing of
// the game's memory is written outside what the resolver granted. The sanitizers hold the tables.

namespace hostile {

static int g_shown = 0;
#define HOSTILE(cond, ...)                                                                             \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        if (!(cond)) {                                                                                 \
            ++g_failures;                                                                              \
            if (++hostile::g_shown <= 40) {                                                            \
                std::fprintf(stderr, "  FAIL %s:%d (hostile): %s | ", __FILE__, __LINE__, #cond);     \
                std::fprintf(stderr, __VA_ARGS__);                                                     \
                std::fprintf(stderr, "\n");                                                            \
            }                                                                                          \
        }                                                                                              \
    } while (0)

struct Rng {
    u64 s;
    u32 Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    u32 Below(u32 n) {
        return Next() % n;
    }
    bool One(u32 in) {
        return Below(in) == 0;
    }
};

static const u8 kIds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 255, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr u32 kTid = 0x00030700;
constexpr u32 kMem = 0x1800;  // the game's memory, at kGameBase

class HostileNet final : public LobbyNet {
public:
    HostileNet(Rng* rng, u32 ceiling) : r_(rng), ceiling_(ceiling) {}
    u64 NowMs() override {
        if (r_->One(3)) {
            now_ += (!calm_ && r_->One(40)) ? 2500 : r_->Below(60);
        }
        return now_;
    }
    u64 NowTicks() override {
        return now_ * 268111;
    }
    bool HostOpen(const HostAnnounce& a, u8, const Identity&) override {
        HOSTILE(a.max_nodes >= 2 && a.max_nodes <= ceiling_, "HostOpen max_nodes %u, ceiling %u", a.max_nodes, ceiling_);
        most_announced_ = a.max_nodes > most_announced_ ? a.max_nodes : most_announced_;
        host_ = !r_->One(10);
        client_ = false;
        return host_;
    }
    void HostUpdate(const HostAnnounce& a) override {
        HOSTILE(a.max_nodes >= 2 && a.max_nodes <= ceiling_, "HostUpdate max_nodes %u, ceiling %u", a.max_nodes, ceiling_);
    }
    void HostClose() override {
        host_ = false;
    }
    void Kick(u8 node) override {
        HOSTILE(node != 0, "Kick(0)");
        ++kicks_;
    }
    bool ScanOpen() override {
        return !r_->One(12);
    }
    void ScanClose() override {}
    bool ScanRead(u32 index, HeardGroup* out) override {
        if (index >= 6 || r_->One(4)) {
            return false;
        }
        std::memset(out, 0, sizeof(*out));
        out->host = Mac{0, 0x1F, 0x32, 0, 0, static_cast<u8>(1 + r_->Below(6))};
        out->announce.max_nodes = kIds[r_->Below(sizeof(kIds))];
        out->total_nodes = kIds[r_->Below(sizeof(kIds))];
        out->signal = static_cast<u8>(r_->Below(256));
        out->announce.accepting = r_->One(6) ? 0 : 1;
        out->announce.child_tid_low = r_->One(8) ? r_->Next() : (kTid | r_->Below(3));
        for (u32 i = 0; i < kNameUnits; ++i) {
            out->announce.name[i] = static_cast<u16>(r_->Next());
        }
        return true;
    }
    bool ConnectBegin(const Mac&, const Identity&) override {
        client_ = !r_->One(8);
        host_ = false;
        return client_;
    }
    int ConnectPoll() override {
        const u32 x = r_->Below(10);
        return calm_ ? (x < 8 ? 1 : 0) : (x < 5 ? 1 : (x < 7 ? -1 : 0));
    }
    void Disconnect() override {
        client_ = false;
    }
    u8 MyNode() override {
        return (!calm_ && r_->One(3)) ? kIds[r_->Below(sizeof(kIds))] : (host_ ? 1 : 2);
    }
    u32 Members(u8 (&nodes)[kMaxPlayers]) override {
        const u32 mode = calm_ ? r_->Below(6) : r_->Below(10);
        u32 n = 0;
        if (mode < 5) {  // plausible: ascending from 1, some missing (in the calm mood the host is always there)
            for (u8 id = 1; id <= 12 && n < kMaxPlayers; ++id) {
                if ((calm_ && id == 1) || !r_->One(4)) {
                    nodes[n++] = id;
                }
            }
            return n;
        }
        for (u32 i = 0; i < kMaxPlayers; ++i) {
            nodes[i] = kIds[r_->Below(sizeof(kIds))];
        }
        if (mode < 8) {
            return r_->Below(kMaxPlayers + 1);
        }
        return mode == 8 ? 255 : 9 + r_->Below(8);  // a count the array cannot hold
    }
    bool ReadNode(u8 node, NodeEntry* out) override {
        if (r_->One(5)) {
            return false;
        }
        out->seed = r_->Next();
        for (u32 i = 0; i < kNameUnits; ++i) {
            out->name[i] = static_cast<u16>('A' + node % 26);
        }
        out->node = r_->One(6) ? kIds[r_->Below(sizeof(kIds))] : node;
        return true;
    }
    bool Send(u8 to, const u8* data, u32 size) override {
        HOSTILE(size >= 2 && size <= kMaxMessage, "Send size %u", size);
        if (host_) {
            HOSTILE(to >= 2 && to <= ceiling_, "the creator sends to node %u, ceiling %u", to, ceiling_);
            most_sent_to_ = to > most_sent_to_ ? to : most_sent_to_;
        } else {
            HOSTILE(to == 1, "a searcher sends to node %u", to);
        }
        HOSTILE(IsMessage(data, size), "Send of something that is no message");
        ++sends_;
        return !r_->One(7);
    }
    bool Receive(u8* from, u8* data, u32 capacity, u32* size) override {
        if (r_->One(3)) {
            return false;
        }
        *from = (calm_ && !host_ && !r_->One(4)) ? 1 : kIds[r_->Below(sizeof(kIds))];
        const u32 kind = r_->Below(12);
        u32 n = 0;
        if (kind < 8) {  // a well-formed message of a type 1..4
            const u8 type = static_cast<u8>(1 + r_->Below(4));
            const u32 body = MessageBodySize(type);
            data[0] = type;
            data[1] = static_cast<u8>(body);
            for (u32 i = 0; i < body; ++i) {
                data[2 + i] = static_cast<u8>(r_->Next());
            }
            n = 2 + body;
        } else {
            n = r_->Below(capacity + 1);
            for (u32 i = 0; i < n; ++i) {
                data[i] = static_cast<u8>(r_->Next());
            }
        }
        *size = r_->One(20) ? r_->Below(64) : n;  // (a size that lies: the lobby must not read past its buffer)
        return true;
    }
    u64 now_ = 5000;
    u32 kicks_ = 0, sends_ = 0, most_announced_ = 0, most_sent_to_ = 0;
    bool calm_ = false;  // the mood: in the calm one a session lasts (the host stays, the join succeeds), the ids are as hostile as ever

private:
    Rng* r_;
    u32 ceiling_;
    bool host_ = false, client_ = false;
};

struct Grant {
    u32 at, size;
};
struct Game {
    std::vector<u8> mem;
    std::vector<u8> ok;
    std::vector<Grant> grants;
    Game() : mem(kMem, 0xCC), ok(kMem, 0) {}
    static u8* Map(void* ctx, u32 address, u32 size) {
        Game* g = static_cast<Game*>(ctx);
        if (address < kGameBase || address - kGameBase > kMem || size > kMem - (address - kGameBase)) {
            return nullptr;
        }
        g->grants.push_back({address - kGameBase, size});
        return g->mem.data() + (address - kGameBase);
    }
    /// Every byte outside what was granted is still 0xCC; then the memory is made new.
    void Verify(u32 id, bool sv) {
        std::memset(ok.data(), 0, kMem);
        for (const Grant& gr : grants) {
            std::memset(ok.data() + gr.at, 1, gr.size);
        }
        bool clean = true;
        u32 where = 0;
        for (u32 i = 0; i < kMem && clean; ++i) {
            if (!ok[i] && mem[i] != 0xCC) {
                clean = false;
                where = i;
            }
        }
        HOSTILE(clean, "%s 0x%02x wrote the game's memory at +0x%x outside what it was granted", sv ? "SRVR" : "FKCL", id, where);
        std::memset(mem.data(), 0xCC, kMem);
        grants.clear();
    }
};

static const u32 kWords[] = {0,          1,          2,          3,          4,          5,          6,          7,          8,       9,
                             15,         16,         17,         255,        256,        0xFFFF,     0x10000,    0x10002,    0x7FFFFFFF, 0xFFFFFFFF,
                             0x80000000, 0x00089000, 0x1EC,      0xEC,       0xFC,       0x1C,       0xC,        kGameBase,  kGameBase + 8,  kGameBase + kMem - 2,
                             kGameBase + kMem, kGameBase - 1, kTid, 0x00040001, 0x0000F000, 0x421,   1,          6,          11,      0x00320032};

/// What the game of one run read: how often each total of clients (0..7) and of nodes (0..8), and the largest announcement and Send target.
struct Seen {
    unsigned clients[8] = {};
    unsigned nodes[9] = {};
    u32 most_announced = 0;
    u32 most_sent_to = 0;
};

static Seen Run(u32 said, u32 commands_to_run, u64 seed) {
    Rng rng{seed * 0x9E3779B97F4A7C15ull + said + 1};
    const u32 ceiling = TitlePlayers(said);
    HostileNet net(&rng, ceiling);
    LobbyConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.game_tid = 0x0004000000000000ull | kTid;
    cfg.friend_seed = 0x1234;
    cfg.max_players = static_cast<u8>(said);
    DlpLobby lobby(&net, cfg);
    DlpService svc;
    svc.SetLobby(&lobby);
    Game game;
    IpcResolver res;
    res.ctx = &game;
    res.map = &Game::Map;
    Seen seen;
    u32 w[72];
    for (u32 commands = 1; commands <= commands_to_run; ++commands) {
        if ((commands % 1500) == 0) {
            net.calm_ = !net.calm_;
        }
        const bool sv = rng.One(2);
        u32 id = rng.One(40) ? rng.Below(0x20) : 1 + rng.Below(sv ? 0x10 : 0x11);
        if (net.calm_ && (id == 0x02 || (!sv && (id == 0x01 || id == 0x10 || id == 0x11))) && !rng.One(12)) {
            id = sv ? 0x0B : 0x0D;  // (in the calm mood the game seldom ends a role: the sessions live long enough to fill)
        }
        for (u32& x : w) {
            x = 0xC0DE0000u;
        }
        w[0] = (id << 16) | rng.Below(0x400);
        for (u32 i = 1; i < 16; ++i) {
            w[i] = rng.One(6) ? rng.Next() : kWords[rng.Below(sizeof(kWords) / sizeof(kWords[0]))];
        }
        // shape the commands that matter so that they often get through
        u32 sbuf[2] = {0, 0};
        const u32* sb = nullptr;
        u32 vec_elems = 0, vec_bytes = 0, vec_at = 0;
        const u32 shape = rng.Below(10);
        if (sv && (id == 0x01 || id == 0x0F) && shape < 8) {
            w[2] = rng.One(3) ? 7 : kIds[rng.Below(sizeof(kIds))];
            w[4] = 0;
        } else if (sv && id == 0x05 && shape < 8) {
            w[1] = rng.Below(2);
            w[2] = 11;
        } else if (sv && id == 0x0B && shape < 9) {
            vec_elems = rng.One(3) ? 15 : kIds[rng.Below(sizeof(kIds))];
            vec_bytes = rng.One(4) ? rng.Below(40) : 2 * vec_elems;
            vec_at = kGameBase + 0x100;
            w[1] = vec_elems;
            w[2] = (vec_bytes << 4) | 0xC;
            w[3] = vec_at;
        } else if (!sv && id == 0x0D && shape < 9) {
            vec_elems = rng.One(3) ? 15 : kIds[rng.Below(sizeof(kIds))];
            vec_bytes = rng.One(4) ? rng.Below(40) : 2 * vec_elems;
            vec_at = kGameBase + 0x100;
            w[1] = vec_elems;
            sbuf[0] = (vec_bytes << 14) | 2;
            sbuf[1] = vec_at;
            sb = sbuf;
        } else if (!sv && (id == 0x07 || id == 0x08 || id == 0x0A || id == 0x0B) && shape < 9) {
            w[1] = 0x00321F00;  // a MAC of the hostile scan: 00 1F 32 00 00 xx
            w[2] = static_cast<u32>(1 + rng.Below(6)) << 8;
            sbuf[0] = ((id == 0x07 ? 0x298u : 0x13A8u) << 14) | 2;
            sbuf[1] = kGameBase + 0x400;
            sb = sbuf;
        } else if (!sv && id == 0x09 && shape < 9) {
            w[1] = rng.Below(2);
            sbuf[0] = (0x13A8u << 14) | 2;
            sbuf[1] = kGameBase + 0x400;
            sb = sbuf;
        } else if (!sv && id == 0x05 && shape < 8) {
            w[2] = kTid;
            w[3] = 0x00040001;
        } else if (rng.One(4)) {
            sbuf[0] = rng.One(2) ? rng.Next() : ((rng.Below(0x1400) << 14) | 2);
            sbuf[1] = rng.One(2) ? kGameBase + rng.Below(kMem) : rng.Next();
            sb = sbuf;
        }
        const DlpOutcome out = svc.Dispatch(sv ? DlpKind::Srvr : DlpKind::Fkcl, w, sb, res);
        if (out == DlpOutcome::JoinPending) {
            for (u32 k = 0; k < 400 && !svc.PollPending(w); ++k) {
                net.now_ += 20;
            }
        } else if (w[1] == 0) {
            if (sv && id == 0x0B && vec_at != 0) {
                const u32 total = w[2];
                HOSTILE(total <= ceiling - 1, "GetConnectingClients total %u, ceiling %u", total, ceiling);
                HOSTILE(total <= lobby.max_clients_, "GetConnectingClients total %u, the game asked for %u clients", total, lobby.max_clients_);
                const u32 may = std::min(std::min(total, vec_elems), vec_bytes / 2);
                u32 last = 1;
                for (u32 i = 0; i < 40; ++i) {
                    const u32 v = LoadLE16(game.mem.data() + 0x100 + 2 * i);
                    if (i < may) {
                        HOSTILE(v >= 2 && v <= ceiling && v > last, "client id %u at %u (ceiling %u, the one before %u)", v, i, ceiling, last);
                        last = v;
                    } else {
                        HOSTILE(v == 0xCCCC, "GetConnectingClients wrote element %u of a vector of %u (%u bytes), total %u", i, vec_elems, vec_bytes, total);
                    }
                }
                if (total < 8) {
                    ++seen.clients[total];
                }
            } else if (!sv && id == 0x0D && vec_at != 0) {
                const u32 total = w[2];
                HOSTILE(total <= ceiling, "GetConnectingNodes total %u, ceiling %u", total, ceiling);
                const u32 may = std::min(std::min(total, vec_elems), vec_bytes / 2);
                for (u32 i = may; i < 40; ++i) {
                    HOSTILE(LoadLE16(game.mem.data() + 0x100 + 2 * i) == 0xCCCC, "GetConnectingNodes wrote element %u of a vector of %u (%u bytes), total %u", i,
                            vec_elems, vec_bytes, total);
                }
                if (total < 9) {
                    ++seen.nodes[total];
                }
            } else if (!sv && id == 0x07 && sb == sbuf && sbuf[1] == kGameBase + 0x400 && (sbuf[0] >> 14) == 0x298) {
                const u8* rec = game.mem.data() + 0x400;
                HOSTILE(rec[8] >= 2 && rec[8] <= ceiling, "GetServerInfo max %u, ceiling %u", rec[8], ceiling);
                HOSTILE(rec[9] <= rec[8], "GetServerInfo joined %u of %u", rec[9], rec[8]);
            }
        }
        for (const Grant& gr : game.grants) {
            if (vec_at != 0 && gr.at == 0x100) {
                HOSTILE(gr.size <= vec_bytes, "a grant of %u bytes for a vector of %u", gr.size, vec_bytes);
            }
        }
        game.Verify(id, sv);
        HOSTILE(lobby.max_players() == ceiling, "the ceiling became %u (it was %u)", lobby.max_players(), ceiling);
        HOSTILE(lobby.max_clients_ >= 1 && lobby.max_clients_ <= ceiling - 1, "max_clients_ %u, ceiling %u", lobby.max_clients_, ceiling);
        if (rng.One(3)) {
            net.now_ += rng.Below(300);
            lobby.Pump();
        }
        if (rng.One(net.calm_ ? 4000 : 500)) {
            lobby.OnGameUdsStarting();
        }
        if (rng.One(700)) {
            svc.OnSessionClosed(rng.One(2) ? DlpKind::Fkcl : DlpKind::Srvr);
        }
    }
    seen.most_announced = net.most_announced_;
    seen.most_sent_to = net.most_sent_to_;
    std::printf("  ceiling said %3u (= %u): %u commands, %u sends, %u kicks, %u bad messages; clients", said, ceiling, commands_to_run, net.sends_, net.kicks_,
                lobby.bad_messages());
    for (u32 i = 0; i < 8; ++i) {
        std::printf(" %u:%u", i, seen.clients[i]);
    }
    std::printf("; nodes");
    for (u32 i = 0; i < 9; ++i) {
        std::printf(" %u:%u", i, seen.nodes[i]);
    }
    std::printf("\n");
    return seen;
}

}  // namespace hostile

static void TestEightHostile() {
    std::printf("a hostile network and a hostile game, for every ceiling of a title\n");
    constexpr u32 kCommands = 60000;
    constexpr u64 kSeed = 20261113;
    for (const u32 said : {8u, 255u, 7u, 5u, 0u, 4u, 2u, 1u}) {
        const u32 ceiling = TitlePlayers(said);
        const hostile::Seen seen = hostile::Run(said, kCommands, kSeed);
        // the run reached what it is for: the game read a lobby FULL to the ceiling of its title, as a creator and as a client, the creator announced
        // the ceiling and wrote to its last client - and nothing past it (the tables of a title of eight were walked to their last place)
        CHECK(seen.clients[ceiling - 1] > 0);
        CHECK(seen.nodes[ceiling] > 0);
        CHECK_EQ(seen.most_announced, ceiling);
        CHECK_EQ(seen.most_sent_to, ceiling);
        for (u32 total = ceiling; total < 8; ++total) {
            CHECK_EQ(seen.clients[total], 0u);
        }
        for (u32 total = ceiling + 1; total < 9; ++total) {
            CHECK_EQ(seen.nodes[total], 0u);
        }
    }
    hostile::Run(8, kCommands, 777);  // (the title of eight once more, another seed)
}

}  // namespace

int main() {
    std::printf("dlp_eight_test: the Download Play lobby with eight players, and the titles of four beside it\n");
    TestCeiling();
    TestAskedClients();
    TestSearcher();
    TestTitleOfFour();
    TestEightOnTheAir();
    TestEightCountsItself();
    TestEightLosesOne();
    TestEightHostGoes();
    TestEightOnTheEngine();
    TestEightStalls();
    TestFourOnTheEngine();
    TestEightHostile();
    static_cast<void>(&LobbyFrame);  // (a helper of the rig this file has no use for)
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
