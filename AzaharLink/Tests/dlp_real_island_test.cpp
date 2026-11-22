// MARIO PARTY: ISLAND TOUR as the HOST of the real Download Play (2026-11-14, stage H1 of analysis/island-tour-2026-11-13/plugin/PROGETTO-FINALE.md):
// the hub AS THE PLUGIN HAS IT for the Download Play column of the game in azl/title_rules.hpp (RealDlp, the hash of the MAC, four players) - a DlpHub whose lobby runs on the
// adapter RealDlpNet, a network thread, the game's IPC words served by DlpHub::Serve on a game thread - held to the capture of two Azahar that
// played it (analysis/island-tour-2026-11-13/kit-azahar; its bytes are Tests/dlp_real_island_golden.inc, cut out by
// Tests/make_dlp_real_island_goldens.py). The rig is the one of Tests/dlp_real_hub_test.cpp: engines joined by the in-memory room of
// Tests/mock_room.hpp, real time, the Azahar-peer rule checker on every frame our engine sends; whoever joins is the model of Azahar's Download
// Play client (Tests/azahar_dlp_peer.hpp: Azahar's code transcribed), because the game has no Download Play client of its own - a guest is the
// system's Download Play application, which has the child already (nothing is downloaded) or asks for it.
// The game is driven as its code reads (analysis/island-tour-2026-11-13/host/RAPPORTO.md): dlp:SRVR IsChild at boot, Initialize (0x01, NO name)
// for 3 clients with the child index 0, GetDupNoticeNeed, StartAccepting BY HAND on channel 0, every frame GetConnectingClients and for each
// client GetClientInfo and GetClientState; when the player confirms, AcceptClient for every client in 0x40 and StartDistribution IN THE SAME
// FRAME; at state 8 SendWirelessRebootPassphrase ONCE; at state 9 it waits for GetConnectingClients = 0, then Finalize and its own network
// (wlancommid 000F8210, id 1, four nodes), where it waits for every player.
//
//   G  the capture against the codec, no network, field by field: where the announcement and the packets of OUR host are the capture's and
//      where they differ (the sizes, the name and the icon of the child are zero in ours: stage H2; the sequence and the id of the host's
//      packets, which Azahar's host leaves as they were because it accepts and starts in one step)
//   H  the console hosts, THREE Azahar clients come in and wait; the game accepts and starts in the same frame, with the answers of the clients
//      0, 100 and 250 ms away and the acceptance at once or after a wait: the states the game reads, ONE StartDistribution and ONE final to each,
//      the passphrase given once, state 9 with no client left to wait for; EVERY frame the host sent is compared with the capture's form; then
//      the game's own network from the same address, and the three come back in as the child would
//   C  a client that asks for the CONTENT (it has no child): it is ejected and nothing of the child travels; what the game reads meanwhile -
//      it counted that client among its players - with two other clients and alone
//   R  EVERY REGION of the game (2026-11-14: the rows of USA, Japan, Taiwan and Korea have the values of the European one): the hub of each of
//      the five ids announces the child of ITS OWN title (00040001 above its low word), on every frame, and its lobby goes to its end; what
//      the code of the other four regions does is not read and is not held here
//   L  the LIMITS of the row, held as they are today (a remedy changes these lines on purpose): the game sends StartDistribution and the
//      passphrase ONCE and drops their result, so when the hub refuses one - no Tick of the network thread for DlpHub::kAliveMs: HOME, an
//      applet, a thread held up - the lobby stays where it is until the player leaves it and comes back; and the game's "back" while it
//      distributes (its closeDistribute: EndAccepting and StartAccepting in state 6), which Azahar's host follows and this hub does not
//   W  (1.1.4, H5) the host WAITS in its state 9 for the guests of the Download Play application that went to start their child: the room's
//      lists given by hand as the plugin's OnRoomInfo gives them; three consoles out and back (3 -> 2 -> 1 -> 0, the game's GetClientInfo and
//      GetClientState of every id it reads answered), three Azahar that stay (5 s), a console that never comes back with two Azahar (10 s), one
//      alone (15 s), Finalize while it waits; nothing on the wire meanwhile, and the hub idle again at the end. Part H does not turn it on.
// Every request word is written as the game's wrappers write it: a BYTE or a HALFWORD stored into a word that keeps what the last command
// left there (Initialize and StartAccepting store bytes, AcceptClient, GetConnectingClients, GetClientInfo and GetClientState halfwords).
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_island_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp
//       dlph_wire.o <the core sources of Tests/run.sh>
#pragma GCC diagnostic ignored "-Wunused-function"  // (the rig's helpers this test does not use)

#include <algorithm>
#include <deque>
#include <mutex>

#include "dlp_net_rig.hpp"

#include "azahar_dlp_peer.hpp"
#include "azahar_peer.hpp"
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"
#include "dlp_real_luigi_golden.inc"
#include "dlp_real_island_golden.inc"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u64 kIslandTid = 0x00040000000F8200ULL;    // Mario Party: Island Tour, Europe: its row of azl/title_rules.hpp
static constexpr u64 kIslandChild = 0x00040001000F8200ULL;  // what its host announces: the child, index 0
static constexpr u32 kIslandGameNet = 0x000F8210;           // the game's own network: (unique id 0xF82 << 8) | 0x10, read from its code
static constexpr u32 kIslandClients = 3;                    // what the game asks of Initialize
static constexpr u32 kIslandPlayers = 4;                    // the ceiling of the row (a row that does not say it: four)
static constexpr DlpKind kSv = DlpKind::Srvr;
static constexpr u32 kDlpInvalidState = 0xE0A0AC01u;        // what the module answers to a command outside its state
// The least checks a part makes (see Ran): about half of what it makes on a free machine, far above what is left if a part returns early.
static constexpr int kLeastCapture = 100;
static constexpr int kLeastHost = 1000;
static constexpr int kLeastContent = 800;
static constexpr int kLeastContentAlone = 350;
static constexpr int kLeastLimit = 1500;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static const DlpRealGolden& Island(const char* id) {
    for (const DlpRealGolden& g : kDlpRealIslandDialog) {
        if (std::strcmp(g.id, id) == 0) {
            return g;
        }
    }
    std::fprintf(stderr, "no golden %s\n", id);
    std::abort();
}
/// `p` sealed with `seed` is the golden, byte for byte.
static bool Same(std::vector<u8> p, const DlpRealGolden& g, const u8* seed) {
    if (p.size() != g.size) {
        return false;
    }
    dlp_real::Seal(seed, p.data(), p.size());
    return std::memcmp(p.data(), g.bytes, g.size) == 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The console that hosts: the memory of the game, the hub on the adapter of the real Download Play, the network thread.

class GMem {
public:
    u32 Alloc(u32 size) {
        std::lock_guard<std::mutex> lk(m_);
        Region r;
        r.addr = next_;
        r.data.assign(size == 0 ? 1 : size, 0xCC);
        next_ += (size + 0x40 + 15) & ~15u;
        regions_.push_back(std::move(r));
        return regions_.back().addr;
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = &Thunk;
        return r;
    }
    u8* Find(u32 addr, u32 size = 1) {
        std::lock_guard<std::mutex> lk(m_);
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.data.size()) {
                return r.data.data() + (addr - r.addr);
            }
        }
        return nullptr;
    }
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        return static_cast<GMem*>(ctx)->Find(addr, size);
    }

private:
    struct Region {
        u32 addr = 0;
        std::vector<u8> data;
    };
    std::mutex m_;
    std::deque<Region> regions_;  // (a deque: a pointer into one region stays valid while others are added)
    u32 next_ = 0x08000000;
};

/// What MakeRealDlpHub of Sources/ctr/plugin.cpp gives the hub of this row (Tests/plugin_dlp_test.cpp runs that function for the row itself):
/// the game's title id, a seed that is not the console's own, a name (the plugin puts the profile's: the game sends Initialize with none), and
/// the ceiling of four players for the lobby AND for the adapter.
static LobbyConfig IslandCfg(u8 last, const char* name, u64 tid = kIslandTid) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = tid;  // (the title of the row: the European one unless part R asks for another region's)
    c.friend_seed = 0xF000u + last;
    Put(c.name, name);
    c.max_players = kIslandPlayers;
    return c;
}

struct Host {
    Host(MockRoom& room, u8 last, const char* name, u64 tid = kIslandTid)
        : con(room, last), real(con.engine.get()), hub(con.engine.get(), &con.fake_kernel, IslandCfg(last, name, tid), mem.Resolver(), &real) {
        real.SetIdentity(Me(0xF000u + last, name));
        real.SetPlayers(kIslandPlayers);
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    Console con;
    GMem mem;
    RealDlpNet real;
    DlpHub hub;
};

/// The engine is the game's again: not initialised, not held (the hub and the adapter agree), no probe, the filter off, no bind of the adapter.
static bool EngFree(Host& h) {
    bool ok = !h.e().initialized() && !h.hub.EngineHeld() && !h.e().client_foreign_filter_enforced() && !h.real.Holds() && !h.real.Probing();
    for (u32 id : {RealDlpNet::kHostBind, RealDlpNet::kProbeBind, RealDlpNet::kSessionBind}) {
        UdsEngine::UnbindInfo info;
        info.channel = 0;
        h.e().Unbind(id, &info);
        ok = ok && info.channel == 0;
    }
    return ok;
}

/// The network thread of the console: the engine's Tick, then the hub's (RoomClient's tick callback in plugin.cpp).
class NetThread {
public:
    explicit NetThread(Host& h) : host_(h), thread_([this] { Run(); }) {}
    ~NetThread() {
        stop_ = true;
        thread_.join();
    }
    u64 rounds() const {
        return rounds_;
    }
    /// The thread is held up: not one Tick of the engine or of the hub until Resume() (on a console: HOME, an applet, a call that blocks).
    void HoldUp() {
        held_ = true;
    }
    void Resume() {
        held_ = false;
    }

private:
    void Run() {
        while (!stop_) {
            if (!held_) {
                host_.e().Tick(platform::NowMs());
                host_.hub.Tick();
                ++rounds_;
            }
            Sleep(3);
        }
    }
    Host& host_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> held_{false};
    std::atomic<u64> rounds_{0};
    std::thread thread_;
};

/// Every frame the console sent, whole, with who it was from.
class Tap {
public:
    struct Out {
        WifiPacket::Type type;
        Mac from, dest;
        std::vector<u8> data;
    };
    struct Dlp {
        u8 channel;
        u16 dest;
        std::vector<u8> p;
    };
    explicit Tap(Console& c) {
        c.sniff = [this](const WifiPacket& w) {
            std::lock_guard<std::mutex> l(m_);
            v_.push_back({w.type, w.transmitter_address, w.destination_address, w.data});
        };
    }
    std::vector<Out> All() {
        std::lock_guard<std::mutex> l(m_);
        return v_;
    }
    std::size_t Mark() {
        std::lock_guard<std::mutex> l(m_);
        return v_.size();
    }
    static bool AsDlp(const Out& o, Dlp* d) {
        EtherType t;
        SecureDataHeader h;
        if (o.type != WifiPacket::Type::Data || !GetFrameEtherType(o.data, &t) || t != EtherType::SecureData || !ParseSecureDataHeader(o.data, &h) ||
            h.ActualDataSize() > o.data.size()) {
            return false;
        }
        d->channel = h.data_channel;
        d->dest = h.dest_node_id;
        d->p.assign(o.data.end() - h.ActualDataSize(), o.data.end());
        return true;
    }

private:
    std::mutex m_;
    std::vector<Out> v_;
};

/// The network tag of a beacon (dd .. 00 1f 32 15: the OUI, type 21, then the wlancommid BE32 at +4 of the OUI, id8 +8, total +0x10, max +0x11):
/// a pointer to the OUI, or null.
static const u8* NetTag(const std::vector<u8>& beacon) {
    for (std::size_t p = 12; p + 2 <= beacon.size(); p += 2u + beacon[p + 1]) {
        if (beacon[p] == 0xDD && beacon[p + 1] >= 0x34 && p + 2u + beacon[p + 1] <= beacon.size() && beacon[p + 2] == 0x00 && beacon[p + 3] == 0x1F &&
            beacon[p + 4] == 0x32 && beacon[p + 5] == 21) {
            return &beacon[p + 2];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// The game: its IPC words through DlpHub::Serve, on this thread.

struct Rep {
    u32 w[16] = {};
    u32 res() const {
        return w[1];
    }
};

/// A request word as the game's wrappers leave it (read from its code): the argument is stored as a BYTE (strb: Initialize at 0x1AE094 and
/// 0x1AE0A0, StartAccepting at 0x1AE138 and 0x1AE140) or as a HALFWORD (strh: AcceptClient, GetConnectingClients, GetClientInfo,
/// GetClientState), and the rest of the word is whatever the last command left in the thread's buffer.
static u32 AsByte(u32 v) {
    return 0xC0DE5A00u | (v & 0xFFu);  // the game stores a byte: three stale bytes above it
}
static u32 AsHalf(u32 v) {
    return 0xC0DE0000u | (v & 0xFFFFu);  // the game stores a halfword: two stale bytes above it
}

class Game {
public:
    explicit Game(Host& h) : h_(h) {
        cli_ = h.mem.Alloc(16);
    }
    Rep Call(u32 header, std::initializer_list<u32> args) {
        u32 buf[64 + 8];
        for (u32 i = 0; i < 64; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < 8; ++i) {
            buf[64 + i] = 0xFEEDFACE;
        }
        buf[0] = header;
        u32 i = 1;
        for (u32 a : args) {
            buf[i++] = a;
        }
        Rep r;
        const DlpOutcome out = h_.hub.Serve(kSv, buf, nullptr);
        CHECK(out == DlpOutcome::Done);
        std::memcpy(r.w, buf, sizeof(r.w));
        for (u32 k = 0; k < 8; ++k) {
            CHECK(buf[64 + k] == 0xFEEDFACE);
        }
        ++calls_[(header >> 16) & 0x1F];
        return r;
    }
    Rep IsChild() { return Call(0x000E0000, {}); }
    /// Initialize (0x01): [1] the shared memory, [2] the clients, [3] a word of the game's, [4] the child's index, then two words and the handles. No name.
    Rep Initialize(u32 clients, u32 child_index) { return Call(0x00010183, {0x85000, AsByte(clients), 0xE, AsByte(child_index), 0x40000, 2, 0x04000000, 0x11, 0x12}); }
    Rep DupNoticeNeed() { return Call(0x00100000, {}); }
    Rep Finalize() { return Call(0x00020000, {}); }
    Rep StartAccepting(u32 manual, u32 channel) { return Call(0x00050080, {AsByte(manual), AsByte(channel)}); }
    Rep EndAccepting() { return Call(0x00060000, {}); }
    Rep StartDistribution() { return Call(0x00070000, {}); }
    Rep SendPassphrase(const u8* nine) {
        const u32 a = static_cast<u32>(nine[0]) | (static_cast<u32>(nine[1]) << 8) | (static_cast<u32>(nine[2]) << 16) | (static_cast<u32>(nine[3]) << 24);
        const u32 b = static_cast<u32>(nine[4]) | (static_cast<u32>(nine[5]) << 8) | (static_cast<u32>(nine[6]) << 16) | (static_cast<u32>(nine[7]) << 24);
        return Call(0x000800C0, {a, b, static_cast<u32>(nine[8])});
    }
    Rep AcceptClient(u32 node) { return Call(0x00090040, {AsHalf(node)}); }
    Rep ConnectingClients() { return Call(0x000B0042, {AsHalf(4), (8u << 4) | 0xC, cli_}); }
    Rep ClientInfo(u32 node) { return Call(0x000C0040, {AsHalf(node)}); }
    Rep ClientState(u32 node) { return Call(0x000D0040, {AsHalf(node)}); }
    u32 ServerState() { return Call(0x00030000, {}).w[2]; }
    u32 ClientsTotal() {
        const Rep r = ConnectingClients();
        return r.res() == 0 ? r.w[2] : 0xFFFFFFFFu;
    }
    u16 ClientId(u32 i) { return LoadLE16(h_.mem.Find(cli_, 8) + 2 * i); }
    u32 calls(u32 id) const { return calls_[id & 0x1F]; }

    /// One frame of the game's lobby: the server's state, GetConnectingClients, and GetClientInfo and GetClientState of each client. What it read,
    /// as one line: "state 2, 3 clients: 2=40 3=40 4=40" (the external state of each, hex). `waiting` gets the ids of the clients in 0x40.
    std::string Frame(std::vector<u32>* waiting = nullptr) {
        const u32 state = ServerState();
        const Rep list = ConnectingClients();
        const u32 total = list.res() == 0 ? list.w[2] : 0;
        char head[64];
        std::snprintf(head, sizeof(head), "state %u, %u client%s:", static_cast<unsigned>(state), static_cast<unsigned>(total), total == 1 ? "" : "s");
        std::string line = head;
        if (waiting != nullptr) {
            waiting->clear();
        }
        for (u32 i = 0; i < total && i < 4; ++i) {
            const u32 node = ClientId(i);
            const Rep info = ClientInfo(node);
            const Rep st = ClientState(node);
            char one[32];
            if (info.res() != 0 || st.res() != 0) {
                std::snprintf(one, sizeof(one), " %u=gone", static_cast<unsigned>(node));
            } else {
                std::snprintf(one, sizeof(one), " %u=%02x", static_cast<unsigned>(node), static_cast<unsigned>(st.w[2] >> 24));
                if (waiting != nullptr && (st.w[2] >> 24) == 0x40) {
                    waiting->push_back(node);
                }
            }
            line += one;
        }
        return line;
    }

private:
    Host& h_;
    u32 cli_ = 0;
    u32 calls_[32] = {};
};

/// The lines of the frames of the game as they change, each with the time it was first read.
struct Seen {
    explicit Seen(const char* what) : what_(what), t0_(platform::NowMs()) {}
    void Add(const std::string& line) {
        if (lines_.empty() || lines_.back() != line) {
            lines_.push_back(line);
            std::printf("   %s +%4u ms: %s\n", what_, static_cast<unsigned>(platform::NowMs() - t0_), line.c_str());
        }
    }
    bool Has(const std::string& line) const {
        return std::find(lines_.begin(), lines_.end(), line) != lines_.end();
    }
    const char* what_;
    u64 t0_;
    std::vector<std::string> lines_;
};

/// The game's frames (16 ms apart) until `done(line)` or `ms` ran out; every line that changes goes to `seen`.
template <typename F>
static bool Frames(Game& g, Seen& seen, u32 ms, F done) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        const std::string line = g.Frame();
        seen.Add(line);
        if (done(line)) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(16);
    }
}

static void CheckModel(AzaharDlpClient& m) {
    for (const std::string& v : m.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(m.violations().empty());
    CHECK(!m.session_broken);
}

/// A part made at least `least` checks: a part that returns at once, or whose loop runs no time, is a failure and not a quieter green.
/// (The frames are real time, so the count of a part moves from run to run: `least` is about half of what the part makes on a free machine.)
static void Ran(const char* part, int before, int least) {
    const int made = g_checks - before;
    std::printf("   (%s: %d checks, at least %d wanted)\n", part, made, least);
    CHECK(made >= least);
}

/// An Azahar client comes in as Azahar's Download Play does: it hears the host, reads the announcement as a spectator, says goodbye, joins.
static bool ModelJoin(AzaharDlpClient& m, const Mac& host, AzaharDlpClient::Probe* pr) {
    if (!WaitFor(4000, [&] { return m.Heard(host); })) {
        return false;
    }
    *pr = m.ProbeHost(host);
    return pr->all && m.StartSession(host);
}

/// The game up to its lobby: IsChild at boot (not a child: the hub answers, since the row gives the game one), Initialize with no name for three
/// clients and the child 0, GetDupNoticeNeed, StartAccepting by hand on channel 0. The network the room carries has four nodes.
static void OpenIsland(Host& h, Game& g) {
    const Rep child = g.IsChild();
    CHECK_EQ(child.res(), 0u);
    CHECK_EQ(child.w[2], 0u);  // the game loads as the parent
    CHECK_EQ(g.Initialize(kIslandClients, 0).res(), 0u);
    CHECK_EQ(g.DupNoticeNeed().res(), 0u);
    CHECK_EQ(g.ServerState(), 1u);
    CHECK_EQ(g.StartAccepting(1, 0).res(), 0u);
    CHECK_EQ(g.ServerState(), 2u);
    CHECK(h.hub.EngineHeld());
    CHECK_EQ(h.real.players(), kIslandPlayers);
    CHECK_EQ(h.e().GetConnectionStatus().max_nodes, 4u);  // "hosting network (max 4 nodes ...)", as the capture's beacon says
}

/// The player confirms, as the game's code does it: AcceptClient for every client it read in 0x40 and StartDistribution, in ONE frame - no
/// read of a state, no wait, nothing else between them (Luigi's Mansion waits for the client to leave 0x40 first; this game does not).
static void Confirm(Game& g, const std::vector<u32>& waiting) {
    for (const u32 node : waiting) {
        CHECK_EQ(g.AcceptClient(node).res(), 0u);
    }
    CHECK_EQ(g.StartDistribution().res(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// G. The capture against the codec, field by field.

static void G_Capture() {
    std::printf("G: the capture of two Azahar (Mario Party: Island Tour) against the codec, field by field\n");
    CHECK_EQ(kDlpRealIslandChild, kIslandChild);
    CHECK_EQ(kDlpRealIslandMaxNodes, kIslandPlayers);
    CHECK_EQ(kDlpRealIslandLobbyNet, 0x00002810u);
    CHECK_EQ(kDlpRealIslandGameNet, kIslandGameNet);
    CHECK_EQ(kDlpRealIslandGameNet, ((static_cast<u32>(kIslandTid >> 8) & 0xFFFFFu) << 8) | 0x10u);  // (unique id << 8) | 0x10, as the game computes it
    CHECK_EQ(kDlpRealIslandGameNetId, 1u);
    CHECK_EQ(kDlpRealIslandGameNetNodes, kIslandPlayers);
    CHECK(kDlpRealIslandGameNetSameHost);
    CHECK(kDlpRealIslandLobbySeedsZero);  // (why the row has no console seed: the seeds of the lobby were zero and the game played)
    // the announcement: ours with the capture's seed, frame by frame
    u8 buf[dlp_real::kBufferBytes];
    static const std::size_t kLen[5] = {768, 1448, 1448, 1448, 1464};
    for (unsigned i = 0; i < 5; ++i) {
        const DlpRealGolden& g = kDlpRealIslandAnnounce[i];
        CHECK_EQ(g.size, kLen[i]);
        CHECK_EQ(dlp_real::AnnounceFrame(buf, i, kIslandChild, kIslandPlayers, kDlpRealIslandSeed), g.size);
        const dlp_real::Packet p = dlp_real::ParseAnnounce(g.bytes, g.size);
        CHECK(p.kind == dlp_real::Kind::Announce);
        CHECK_EQ(p.index, i);
        CHECK(std::memcmp(buf, g.bytes, 8) == 0);            // SAME: the type 01, 02, the length, 02 00
        CHECK(std::memcmp(buf + 12, g.bytes + 12, 4) == 0);  // SAME: the index, 05 00 00
        bool body_zero = true;
        for (std::size_t k = (i == 0 ? 0x38 : 0x10); k < (i == 0 ? 0x2F8u : g.size); ++k) {
            body_zero &= buf[k] == 0;
        }
        CHECK(body_zero);  // ours: zeros there
        if (i == 0) {
            CHECK(std::memcmp(buf + 0x10, g.bytes + 0x10, 0x28) == 0);  // SAME: the child 00040001000F8200, 00 10, 01 01, 00 03 ff c0, max_nodes 4, 23 zeros
            CHECK(std::memcmp(buf + 0x2F8, g.bytes + 0x2F8, 8) == 0);   // SAME: the tail
            CHECK_EQ(dlp_real::AnnounceChildTid(g.bytes), kIslandChild);
            CHECK_EQ(dlp_real::AnnounceMaxNodes(g.bytes), kIslandPlayers);
            // DIFFERENT (stage H2): the two sizes of the child, its short and long name, the first part of its icon - zero in ours
            CHECK_EQ(LoadBE32(g.bytes + 0x38), kDlpRealIslandTransferSize);
            CHECK_EQ(LoadBE32(g.bytes + 0x3C), kDlpRealIslandRequiredSize);
            CHECK(kDlpRealIslandTransferSize != 0 && kDlpRealIslandRequiredSize > kDlpRealIslandTransferSize);
            CHECK(LoadBE32(g.bytes + 0x40) != 0);  // (the sum of the words the capture had there: a name and an icon were there)
            CHECK_EQ(g.redacted[0], 0x40u);
            CHECK_EQ(g.redacted[1], 0x2F8u);
        } else if (i <= 3) {
            // DIFFERENT (stage H2): the rest of the icon - zero in ours
            CHECK(LoadBE32(g.bytes + 0x10) != 0);
            CHECK_EQ(g.redacted[0], 0x10u);
            CHECK_EQ(g.redacted[1], g.size);
        } else {
            CHECK(std::memcmp(buf, g.bytes, g.size) == 0);  // SAME: frame 4 is zeros in Azahar's too, the same bytes, checksum included
        }
    }
    std::printf("   announcement: frame 0 the same up to +0x37 and from +0x2F8 (child %016llx, 00 10, 01 01, 0003ffc0, max_nodes %u); +0x38 the sizes %08x / %08x and "
                "+0x40..0x2F7 name and icon are in the capture, ZERO in ours; frames 1-3 (icon) ZERO in ours; frame 4 the same bytes\n",
                static_cast<unsigned long long>(kDlpRealIslandChild), static_cast<unsigned>(kDlpRealIslandMaxNodes),
                static_cast<unsigned>(kDlpRealIslandTransferSize), static_cast<unsigned>(kDlpRealIslandRequiredSize));

    // the dialogue: the capture's order, and every packet as our builder makes it with the capture's seed, sequence and id
    static const char* const kOrder[9] = {"A1", "A3", "A2", "A4", "A5", "A6", "A7", "A8", "A9"};
    CHECK_EQ(sizeof(kDlpRealIslandDialog) / sizeof(kDlpRealIslandDialog[0]), 9u);
    for (unsigned i = 0; i < 9; ++i) {
        CHECK(std::strcmp(kDlpRealIslandDialog[i].id, kOrder[i]) == 0);
    }
    CHECK(std::strcmp(Island("A1").time, Island("A3").time) == 0);  // the Auth and StartDistribution in the same instant: accepted and started in one step
    CHECK(kDlpRealIslandWaitMs > 100000);                         // ... after the client waited 105 s with nothing on channel 2
    for (const DlpRealGolden& g : kDlpRealIslandDialog) {
        const bool from_host = g.src_node == 1;
        const dlp_real::Packet p = from_host ? dlp_real::ParseFromHost(g.bytes, g.size) : dlp_real::ParseFromClient(g.bytes, g.size);
        CHECK(p.kind != dlp_real::Kind::None);  // our codec takes every packet of the capture
        CHECK(!p.needs_content);                // (nothing is downloaded: the guest has the child)
        const u8 rid[2] = {g.bytes[0x0E], g.bytes[0x0F]};
        const u8 seq = g.bytes[0x0D];
        std::vector<u8> want(g.bytes, g.bytes + g.size);
        std::size_t n = 0;
        if (std::strcmp(g.id, "A1") == 0) {
            n = dlp_real::Auth(buf, kDlpRealIslandSeed);
        } else if (std::strcmp(g.id, "A3") == 0) {
            n = dlp_real::StartDist(buf, kDlpRealIslandSeed, seq, rid);
            // DIFFERENT in value: Azahar's host of this game sends it before it has the client's AuthAck - sequence 0 and two bytes nobody set -
            // where ours sends it after the AuthAck: sequence 1 and the client's id 01 02 (part H holds what ours sends)
            CHECK_EQ(seq, 0u);
            CHECK(!(rid[0] == 0x01 && rid[1] == 0x02));
        } else if (std::strcmp(g.id, "A5") == 0) {
            n = dlp_real::Finish(buf, kDlpRealIslandSeed, seq, rid);
            CHECK_EQ(seq, 1u);  // (ours: 2)
        } else if (std::strcmp(g.id, "A7") == 0) {
            n = dlp_real::Final(buf, kDlpRealIslandSeed, seq, rid, kDlpRealIslandPass);
            CHECK_EQ(seq, 2u);  // (ours: 3)
            CHECK(std::memcmp(g.bytes + dlp_real::kPassphraseAt, kDlpRealIslandPass, 9) == 0);  // (the stand-in: the capture's passphrase is not in the file)
            CHECK_EQ(g.bytes[dlp_real::kPassphraseAt + 7], static_cast<u8>('v'));               // the game's eighth character
            CHECK_EQ(g.bytes[0x1D], 9u);
        } else if (std::strcmp(g.id, "A8") == 0) {
            n = dlp_real::Notice(buf, kDlpRealIslandSeed, seq, rid);
            CHECK_EQ(seq, 2u);  // (ours: 3)
        } else {  // a client's answer: what OUR client would answer to the host's packet it answers
            const char* to = std::strcmp(g.id, "A2") == 0 ? "A1" : std::strcmp(g.id, "A4") == 0 ? "A3" : std::strcmp(g.id, "A6") == 0 ? "A5" : "A8";
            const DlpRealGolden& asked = Island(to);
            n = dlp_real::Reply(buf, kDlpRealIslandSeed, dlp_real::ParseFromHost(asked.bytes, asked.size));
            if (std::strcmp(g.id, "A6") == 0) {
                // (declared: +0x16..0x19 is the block Azahar acknowledges, a member its client never initialises; the host does not read it. Ours is zero.)
                std::memset(want.data() + 0x16, 0, 4);
                dlp_real::Seal(kDlpRealIslandSeed, want.data(), want.size());
            }
        }
        if (n != want.size() || std::memcmp(buf, want.data(), n) != 0) {
            std::printf("   %s t=%s node %u -> %u differs: ours %s\n", g.id, g.time, g.src_node, g.dst_node, Hex(buf, n).c_str());
        }
        CHECK(n == want.size() && std::memcmp(buf, want.data(), n) == 0);
    }
    CHECK_EQ(Island("A2").bytes[0x16], 0x01u);  // the id the client gives in its AuthAck ...
    CHECK_EQ(Island("A2").bytes[0x17], 0x02u);  // ... which Azahar's host of this game never puts in its packets (it dropped that AuthAck)
    std::printf("   dialogue: the nine packets are our builders' with the capture's seed, sequence and id; the capture's host sends A3 / A5 / A7 / A8 with the "
                "sequence 0 / 1 / 2 / 2 and the id %02x %02x, ours with 1 / 2 / 3 / 3 and the client's 01 02\n",
                Island("A3").bytes[0x0E], Island("A3").bytes[0x0F]);
}

// ---------------------------------------------------------------------------------------------------------------------
// H. The console hosts, three Azahar clients wait, the game accepts and starts in the same frame.

/// What the host put on the wire, compared with the capture's form. `mark_start` = the frames sent before the game's AcceptClient.
struct WireCount {
    u32 announce = 0, late_announce = 0, a1 = 0, a3 = 0, a5 = 0, a7 = 0, a8 = 0, other = 0, early = 0, content = 0;
    u32 a3_to[3] = {}, a7_to[3] = {}, after_a1_to[3] = {};
};
static WireCount CheckWire(Tap& wire, std::size_t mark_start) {
    WireCount c;
    u8 buf[dlp_real::kBufferBytes];
    const std::vector<Tap::Out> out = wire.All();
    for (std::size_t k = 0; k < out.size(); ++k) {
        Tap::Dlp d;
        if (!Tap::AsDlp(out[k], &d)) {
            continue;
        }
        if (d.channel == RealDlpNet::kChannelAnnounce) {
            // the announcement: to everybody, the frame our builder makes for this child and four nodes (compared with the capture's in part G)
            CHECK_EQ(d.dest, 0xFFFFu);
            const unsigned idx = d.p.size() > 12 ? d.p[12] : 99;
            const std::size_t n = dlp_real::AnnounceFrame(buf, idx, kIslandChild, kIslandPlayers, dlp_real::kTxSeed);
            CHECK(n != 0 && n == d.p.size() && std::memcmp(buf, d.p.data(), n) == 0);
            ++c.announce;
            c.late_announce += k >= mark_start ? 1u : 0u;
            continue;
        }
        CHECK_EQ(d.channel, RealDlpNet::kChannelDialogue);
        CHECK(d.dest >= 2 && d.dest <= 4);
        if (d.channel != RealDlpNet::kChannelDialogue || d.dest < 2 || d.dest > 4 || d.p.size() < dlp_real::kHeaderBytes) {
            ++c.other;
            continue;
        }
        const u32 slot = d.dest - 2u;
        const u8 type = d.p[0];
        const char* id = type == 2 ? "A1" : type == 3 ? "A3" : type == 5 ? "A5" : type == 6 ? (d.p.size() == dlp_real::kFinalBytes ? "A7" : "A8") : nullptr;
        c.content += type == 4 ? 1u : 0u;  // (a fragment of the child: this mode never sends one)
        if (id == nullptr) {
            std::printf("   a packet of the host that the capture does not have: %s\n", Hex(d.p.data(), d.p.size()).c_str());
            CHECK(false);
            ++c.other;
            continue;
        }
        const DlpRealGolden& g = Island(id);
        // what ours says where the capture's host says something else: the sequence (+0x0D) and the client's id (+0x0E..0F)
        const u8 want_seq = type == 2 ? 0 : type == 3 ? 1 : type == 5 ? 2 : 3;
        CHECK_EQ(d.p[0x0D], want_seq);
        CHECK_EQ(d.p[0x0E], type == 2 ? 0x00u : 0x01u);
        CHECK_EQ(d.p[0x0F], type == 2 ? 0x00u : 0x02u);
        // ... and with the capture's three bytes there it is the capture's packet, byte for byte (the passphrase is the stand-in: the test's game gave it)
        std::vector<u8> p = d.p;
        std::memcpy(p.data() + 0x0D, g.bytes + 0x0D, 3);
        if (!Same(p, g, kDlpRealIslandSeed)) {
            std::printf("   %s to node %u is not the capture's form: %s\n", id, static_cast<unsigned>(d.dest), Hex(d.p.data(), d.p.size()).c_str());
        }
        CHECK(Same(p, g, kDlpRealIslandSeed));
        if (type == 2) {
            ++c.a1;
        } else {
            ++c.after_a1_to[slot];
            c.early += k < mark_start ? 1u : 0u;  // (anything but the Auth before the game accepted)
            c.a3 += type == 3 ? 1u : 0u;
            c.a3_to[slot] += type == 3 ? 1u : 0u;
            c.a5 += type == 5 ? 1u : 0u;
            c.a7 += id[1] == '7' ? 1u : 0u;
            c.a7_to[slot] += id[1] == '7' ? 1u : 0u;
            c.a8 += id[1] == '8' ? 1u : 0u;
        }
    }
    return c;
}

static void RunHost(u32 rtt, u32 think_ms) {
    std::printf("H: the console hosts Island Tour, three Azahar clients; answers %u ms away, the player confirms %s\n", static_cast<unsigned>(rtt),
                think_ms == 0 ? "the moment the third is listed" : "after a wait");
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy");
    Tap wire(h.con);
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines;
    for (u8 i = 0; i < kIslandClients; ++i) {
        const std::string name = "Az" + std::to_string(i + 2);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        cl.back()->rtt_ms = rtt;
        engines.push_back(&cl.back()->e());
    }
    {
        Ticker ticker(engines);  // (the models' engines; the console's own is ticked by its network thread, as in the plugin)
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        OpenIsland(h, g);
        Seen seen("the game");
        seen.Add(g.Frame());
        CHECK(seen.Has("state 2, 0 clients:"));
        // the three come in one at a time; the game lists each in 0x40 (waiting to be accepted) and accepts nobody yet
        for (u8 i = 0; i < kIslandClients; ++i) {
            AzaharDlpClient::Probe pr;
            CHECK(ModelJoin(*cl[i], h.mac(), &pr));
            CHECK(pr.all);
            CHECK(pr.ms_to_all <= 1000);
            CHECK_EQ(pr.child_tid, kIslandChild);  // what the guest's Download Play looks for in its own titles
            CHECK_EQ(pr.max_nodes, kIslandPlayers);
            CHECK_EQ(Str(pr.name), std::string("Daisy"));
            std::string want = "state 2, " + std::to_string(i + 1) + " client" + (i == 0 ? "" : "s") + ":";
            for (u8 k = 0; k <= i; ++k) {
                want += " " + std::to_string(k + 2) + "=40";
            }
            CHECK(Frames(g, seen, 3000, [&](const std::string& line) { return line == want; }));
        }
        CHECK_EQ(g.ClientInfo(2).res(), 0u);
        CHECK_EQ(g.calls(0x09), 0u);
        if (think_ms != 0) {
            // the player takes its time (in the capture 105 s): the clients stay in 0x40, and the host says nothing more to them
            // (first the Auth of the last one is answered: no frame of the host for longer than it resends the Auth)
            std::size_t quiet = wire.Mark();
            CHECK(WaitFor(3000, [&] {
                Sleep(RealDlpNet::kAuthResendMs + 100);
                std::size_t dlp = 0;
                for (const Tap::Out& o : wire.All()) {
                    Tap::Dlp d;
                    dlp += Tap::AsDlp(o, &d) ? 1u : 0u;
                }
                const bool still = dlp == quiet;
                quiet = dlp;
                return still;
            }));
            const std::size_t quiet_mark = wire.Mark();
            const u64 until = platform::NowMs() + think_ms;
            while (platform::NowMs() < until) {
                seen.Add(g.Frame());
                Sleep(16);
            }
            CHECK(seen.Has("state 2, 3 clients: 2=40 3=40 4=40"));
            CHECK_EQ(seen.lines_.size(), 4u);  // (nothing else was ever read: 0, 1, 2, 3 clients waiting)
            u32 said = 0;
            const std::vector<Tap::Out> out = wire.All();
            for (std::size_t k = quiet_mark; k < out.size(); ++k) {
                Tap::Dlp d;
                said += Tap::AsDlp(out[k], &d) ? 1u : 0u;
            }
            CHECK_EQ(said, 0u);  // (as in the capture: nothing on channel 2 while the clients wait - and no announcement of ours, nobody is looking)
            std::printf("   while the player waits %u ms: %u data frames from the host to the clients that wait\n", static_cast<unsigned>(think_ms), static_cast<unsigned>(said));
        }
        // the player confirms: AcceptClient for every client in 0x40 and StartDistribution, in ONE frame of the game
        std::vector<u32> waiting;
        const std::string before = g.Frame(&waiting);
        seen.Add(before);
        CHECK_EQ(waiting.size(), static_cast<std::size_t>(kIslandClients));
        const u32 players = static_cast<u32>(waiting.size()) + 1;  // what the game will wait for in its own network
        const std::size_t mark_start = wire.Mark();
        CHECK_EQ(h.real.counters().dist, 0u);
        const u64 t_start = platform::NowMs();
        Confirm(g, waiting);
        CHECK_EQ(g.calls(0x09), kIslandClients);  // H: three AcceptClient ...
        CHECK_EQ(g.calls(0x07), 1u);              // H: ... and ONE StartDistribution, before the game read anything again
        const std::size_t mark_dist = wire.Mark();
        // "in distribution" for the game (its states 3..7; ours shows 6), then 8 with the three still there
        CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));
        const u32 to8 = static_cast<u32>(platform::NowMs() - t_start);
        CHECK(to8 < DlpLobby::kStartGraceMs);  // (every client answered: nobody was thrown out at the lobby's grace)
        CHECK(seen.Has("state 8, 3 clients: 2=07 3=07 4=07"));
        CHECK_EQ(h.real.counters().content, 0u);
        // the passphrase, ONCE (the game's: seven hex digits and 'v'; here the stand-in of the golden data)
        const u64 t_pass = platform::NowMs();
        CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kGoGraceMs + 1500, [&](const std::string& line) { return line.compare(0, 8, "state 9,") == 0; }));
        const u32 to9 = static_cast<u32>(platform::NowMs() - t_pass);
        CHECK(to9 < DlpLobby::kGoGraceMs);  // (every client answered the notice: the lobby did not wait for its grace)
        CHECK_EQ(g.calls(0x08), 1u);  // H: the passphrase was given ONCE
        CHECK_EQ(g.ServerState(), 9u);
        CHECK(seen.Has("state 9, 0 clients:"));  // at 9 the game waits for GetConnectingClients = 0: it is 0 at the first read
        CHECK_EQ(g.ClientsTotal(), 0u);
        std::printf("   state 8 %u ms after the confirmation, state 9 %u ms after the passphrase\n", static_cast<unsigned>(to8), static_cast<unsigned>(to9));
        // every client has the final, with the passphrase the game gave
        for (auto& m : cl) {
            CHECK(WaitFor(3000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
            const std::vector<u8> pass = m->passphrase();
            CHECK(pass.size() == 9 && std::memcmp(pass.data(), kDlpRealIslandPass, 9) == 0);
            CHECK_EQ(m->received(4, 0) + m->received(4, 1), 0u);  // nothing of the child was sent
            CheckModel(*m);
        }
        // the wire: every frame the host sent has the capture's form
        const WireCount w = CheckWire(wire, mark_start);
        std::printf("   on the wire: %u frames of the announcement, A1 %u, A3 %u, A5 %u, A7 %u, A8 %u\n", w.announce, w.a1, w.a3, w.a5, w.a7, w.a8);
        CHECK(w.announce >= 15);        // a group of five for each of the three that looked
        CHECK_EQ(w.early, 0u);          // before the game accepted: the Auth and nothing else (ours sends it at the admission, Azahar's host at the accept)
        CHECK_EQ(w.other, 0u);
        CHECK_EQ(w.content, 0u);  // H: not one fragment of the child on the wire
        CHECK(w.a1 >= kIslandClients);
        for (u32 s = 0; s < kIslandClients; ++s) {
            CHECK(w.a3_to[s] >= 1 && w.a7_to[s] >= 1);
            if (rtt == 0) {
                CHECK_EQ(w.a3_to[s], 1u);  // ONE StartDistribution to each
                CHECK_EQ(w.a7_to[s], 1u);  // ONE final to each
            }
        }
        {
            // no frame of the announcement once the distribution started (Azahar's host stops there too)
            u32 late = 0;
            const std::vector<Tap::Out> out = wire.All();
            for (std::size_t k = mark_dist; k < out.size(); ++k) {
                Tap::Dlp d;
                late += Tap::AsDlp(out[k], &d) && d.channel == RealDlpNet::kChannelAnnounce ? 1u : 0u;
            }
            CHECK_EQ(late, 0u);
        }
        CHECK_EQ(h.real.counters().bad, 0u);
        CHECK_EQ(h.hub.cancelled(), 0u);
        CHECK_EQ(h.hub.failed_fast(), 0u);
        {
            Mac none = {};
            CHECK(!h.real.TakeContentGuest(&none));  // H3: three guests with their child - nobody to name on the screen
            CHECK_EQ(h.real.ChildrenAwaited(), 0u);  // H5: the wait is off in this part (and the room lists nobody): nobody awaited
        }

        // the same frame of the game: Finalize, then its own network (nwm::UDS: the hub gives the engine back at its Initialize)
        CHECK_EQ(g.Finalize().res(), 0u);
        CHECK_EQ(g.ServerState(), 0u);
        h.hub.OnGameUdsStarting();
        CHECK(EngFree(h));
        u32 ev = 0;
        CHECK_EQ(h.e().Initialize(MakeNode(0x1D0, "Daisy", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        NetworkInfo ni;
        std::memset(&ni, 0, sizeof(ni));
        StoreBE32(ni.wlan_comm_id_be, kIslandGameNet);
        ni.id = static_cast<u8>(kDlpRealIslandGameNetId);
        ni.max_nodes = static_cast<u8>(kDlpRealIslandGameNetNodes);
        CHECK_EQ(h.e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
        // the three come back as the child does: out of the Download Play, a scan for the game's network AT THE ADDRESS THE DOWNLOAD PLAY'S HOST HAD
        // (the child compares the two), and in as a client
        for (u8 i = 0; i < kIslandClients; ++i) {
            AzaharDlpClient& m = *cl[i];
            m.StopSession();
            m.e().Shutdown();
            const std::string name = "Az" + std::to_string(i + 2);
            CHECK_EQ(m.e().Initialize(MakeNode(0xC00u + i, name.c_str(), 0), kTestSharedMemory, kTestVersion, &ev), 0u);
            std::vector<u8> scan(0x4000);
            ScanParams sp(h.mac());
            sp.wlan_comm_id = kIslandGameNet;
            sp.id8 = static_cast<u8>(kDlpRealIslandGameNetId);
            NetworkInfo found;
            std::memset(&found, 0, sizeof(found));
            const bool listed = WaitFor(3000, [&] {
                return m.e().RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), sp) == 0 && FirstScannedNetwork(scan, &found);
            });
            CHECK(listed);
            if (!listed) {
                continue;
            }
            CHECK(found.host_mac_address == h.mac());  // the address announced in the Download Play is the BSSID of the game's network
            CHECK_EQ(found.wlan_comm_id(), kIslandGameNet);
            CHECK_EQ(found.id, kDlpRealIslandGameNetId);
            CHECK_EQ(found.max_nodes, kDlpRealIslandGameNetNodes);
            CHECK_EQ(m.e().ConnectToNetwork(reinterpret_cast<const u8*>(&found), sizeof(found), static_cast<u8>(ConnectionType::Client)), 0u);
        }
        u16 me = 0, mask = 0;
        CHECK(WaitFor(3000, [&] { return h.e().NodeSnapshot(&me, &mask) && mask == 0x0F; }));
        CHECK_EQ(me, 1u);
        CHECK_EQ(mask, (1u << players) - 1u);  // every player the game counted is in its network
        CHECK(!h.hub.EngineHeld());          // (the hub stays out of the game's own session)
        // the beacons of the two networks, as the room carried them: one transmitter, the address the clients joined the Download Play at
        Sleep(250);  // (two more beacons of the game's network: one every 102 ms)
        u32 lobby_beacons = 0, game_beacons = 0;
        for (const Tap::Out& o : wire.All()) {
            if (o.type != WifiPacket::Type::Beacon) {
                continue;
            }
            const u8* tag = NetTag(o.data);
            CHECK(tag != nullptr);
            CHECK(o.from == h.mac());
            if (tag == nullptr) {
                continue;
            }
            const u32 wlan = LoadBE32(tag + 4);
            CHECK(wlan == kDlpRealIslandLobbyNet || wlan == kIslandGameNet);
            CHECK_EQ(tag[0x11], 4u);  // four nodes at most, in both
            if (wlan == kDlpRealIslandLobbyNet) {
                ++lobby_beacons;
                CHECK_EQ(tag[0x33], 0u);  // no application data: what Azahar's Download Play client needs
            } else {
                ++game_beacons;
                CHECK_EQ(tag[8], kDlpRealIslandGameNetId);
            }
        }
        std::printf("   beacons of the host: %u of the Download Play, %u of the game's network, one transmitter\n", static_cast<unsigned>(lobby_beacons), static_cast<unsigned>(game_beacons));
        CHECK(lobby_beacons >= 2 && game_beacons >= 2);
        for (auto& m : cl) {
            m->e().DisconnectNetwork();
        }
        h.e().Shutdown();
    }
    cl.clear();
    CHECK_RULES();  // no frame of the room broke a rule of Azahar's (Tests/azahar_rules.hpp)
}

static void H_HostThree() {
    int c = g_checks;
    RunHost(0, 0);
    Ran("H, answers at once", c, kLeastHost);
    c = g_checks;
    RunHost(0, 1500);
    Ran("H, the player waits", c, kLeastHost);
    c = g_checks;
    RunHost(100, 0);
    Ran("H, answers 100 ms away", c, kLeastHost);
    c = g_checks;
    RunHost(250, 0);
    Ran("H, answers 250 ms away", c, kLeastHost);
}

// ---------------------------------------------------------------------------------------------------------------------
// C. A client that asks for the content.

static void RunContent(bool alone) {
    std::printf("C: a client that asks for the child's content, %s\n", alone ? "alone" : "with two clients that have the child");
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy");
    Tap wire(h.con);
    const u32 n = alone ? 1u : kIslandClients;
    const u32 asks = alone ? 0u : 1u;  // (the second of the three: node 3)
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines;
    for (u8 i = 0; i < n; ++i) {
        const std::string name = "Az" + std::to_string(i + 2);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        engines.push_back(&cl.back()->e());
    }
    cl[asks]->want_content = true;
    {
        Ticker ticker(engines);
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        OpenIsland(h, g);
        Seen seen("the game");
        for (u8 i = 0; i < n; ++i) {
            AzaharDlpClient::Probe pr;
            CHECK(ModelJoin(*cl[i], h.mac(), &pr));
        }
        std::vector<u32> waiting;
        CHECK(WaitFor(3000, [&] {
            g.Frame(&waiting);
            return waiting.size() == n;
        }));
        seen.Add(g.Frame());
        // (a client that will ask for the content is a client like the others until the game starts: the game cannot tell)
        const u32 players = static_cast<u32>(waiting.size()) + 1;
        const std::size_t mark_start = wire.Mark();
        const u64 t_start = platform::NowMs();
        Confirm(g, waiting);
        CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));
        const u32 to8 = static_cast<u32>(platform::NowMs() - t_start);
        // the client that asked is ejected (the adapter's count says why), the game no longer lists it
        const u16 asked_node = static_cast<u16>(asks + 2);
        CHECK_EQ(h.real.counters().content, 1u);
        {
            // H3 (1.1.4): the guest that asked, for the plugin's notice on the screen - its MAC (the plugin names it from the room's list), once
            Mac asked_mac = {};
            CHECK(h.real.TakeContentGuest(&asked_mac));
            CHECK(asked_mac == cl[asks]->mac());
            CHECK(!h.real.TakeContentGuest(&asked_mac));
        }
        CHECK_EQ(h.real.PeerState(static_cast<u8>(asked_node)) == RealDlpNet::kEjected || h.real.PeerState(static_cast<u8>(asked_node)) == RealDlpNet::kFree, true);
        CHECK(to8 < DlpLobby::kStartGraceMs);  // (the game does not wait for the lobby's grace: the node is gone at once)
        CHECK(seen.Has(alone ? "state 8, 0 clients:" : "state 8, 2 clients: 2=07 4=07"));
        CHECK_EQ(g.ClientInfo(asked_node).res(), R_NO_DATA);
        CHECK(WaitFor(2000, [&] { return cl[asks]->e().status() != NetworkStatus::ConnectedAsClient; }));  // Azahar's side: out of the network
        CHECK(cl[asks]->state() != AzaharDlpClient::kComplete);
        // the game goes on as if nothing happened: the passphrase once, 9
        CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kGoGraceMs + 1500, [&](const std::string& line) { return line.compare(0, 8, "state 9,") == 0; }));
        CHECK_EQ(g.calls(0x08), 1u);  // C: the passphrase was given ONCE
        CHECK(seen.Has("state 9, 0 clients:"));  // C: the game reads 9 with nobody left to wait for, as if every player had the passphrase
        u32 with_pass = 0;
        for (u8 i = 0; i < n; ++i) {
            if (i == asks) {
                continue;
            }
            CHECK(WaitFor(3000, [&] { return cl[i]->state() == AzaharDlpClient::kComplete; }));
            const std::vector<u8> pass = cl[i]->passphrase();
            with_pass += pass.size() == 9 && std::memcmp(pass.data(), kDlpRealIslandPass, 9) == 0 ? 1u : 0u;
            CheckModel(*cl[i]);
        }
        CHECK(cl[asks]->passphrase().empty());                                   // the client that asked never gets the passphrase
        CHECK_EQ(cl[asks]->received(4, 0) + cl[asks]->received(4, 1), 0u);       // ... and not one fragment of the child
        CheckModel(*cl[asks]);
        // what the game is left with: it counted n + 1 players when the player confirmed, and n of them have the passphrase of its network
        CHECK_EQ(players, n + 1);
        CHECK_EQ(with_pass, n - 1);
        std::printf("   the game counted %u players at the confirmation; %u client%s got the passphrase: its own network will wait for a player that cannot come\n",
                    static_cast<unsigned>(players), static_cast<unsigned>(with_pass), with_pass == 1 ? "" : "s");
        // the wire: the capture's form up to the answer that asks; after the game accepted, the client that asked gets its StartDistribution and nothing else
        const WireCount w = CheckWire(wire, mark_start);
        CHECK_EQ(w.early, 0u);
        CHECK_EQ(w.other, 0u);
        CHECK_EQ(w.content, 0u);  // C: not one fragment of the child on the wire, though a client asked for it
        CHECK(w.a3_to[asks] >= 1);
        CHECK_EQ(w.after_a1_to[asks], w.a3_to[asks]);  // no Finish, no final, no notice to it
        CHECK_EQ(w.a7_to[asks], 0u);
        CHECK_EQ(w.a7, n - 1);
        CHECK_EQ(g.Finalize().res(), 0u);
        h.hub.OnGameUdsStarting();
        CHECK(EngFree(h));
    }
    cl.clear();
    CHECK_RULES();
}

static void C_WantsContent() {
    int c = g_checks;
    RunContent(false);
    Ran("C, with two other clients", c, kLeastContent);
    c = g_checks;
    RunContent(true);
    Ran("C, alone", c, kLeastContentAlone);
}

// ---------------------------------------------------------------------------------------------------------------------
// L. The limits of the row, held as they are (the day a remedy comes these lines change on purpose, not by accident).

/// The lobby with ONE Azahar client that waits in 0x40 (answers `rtt` ms away): what the three scenarios below start from.
static void OneWaits(Host& h, Game& g, AzaharDlpClient& m, std::vector<u32>* waiting) {
    OpenIsland(h, g);
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(m, h.mac(), &pr));
    CHECK_EQ(pr.child_tid, kIslandChild);
    CHECK(WaitFor(3000, [&] {
        g.Frame(waiting);
        return waiting->size() == 1;
    }));
}

/// The way out of a lobby that stays where it is: the player leaves it and comes back (Finalize, then Initialize and StartAccepting again).
static void LeaveAndComeBack(Host& h, Game& g) {
    CHECK_EQ(g.Finalize().res(), 0u);
    CHECK_EQ(g.ServerState(), 0u);
    OpenIsland(h, g);  // L: ... and the hub accepts again
    CHECK_EQ(g.Finalize().res(), 0u);
    h.hub.OnGameUdsStarting();
    CHECK(EngFree(h));
}

/// L1 and L2. The game sends StartDistribution (its code, 0x17E3F8-0x17E41C) and the passphrase (0x17E5A8-0x17E5B0) ONCE and does not look
/// at their result: its own state goes on (5, then 8) whatever the answer was. Both commands need the network thread (AcceptClient is
/// answered on the game's thread), and a request the network thread cannot serve - no Tick for DlpHub::kAliveMs, here; on a console also
/// HOME or an applet in that instant, which close the gate of the swap - is answered "wireless off" at once. Then nothing moves any more:
/// the game waits for a state the hub will never reach. (Luigi's Mansion sends its passphrase again every frame: it does not have this.)
static void RunRefused(bool the_start) {
    std::printf("L%d: no Tick of the network thread for %u ms when the game sends its ONE %s\n", the_start ? 2 : 1, static_cast<unsigned>(DlpHub::kAliveMs + 300),
                the_start ? "StartDistribution" : "passphrase");
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy");
    AzaharDlpClient guest(room, 0x12, "Az2");
    std::vector<UdsEngine*> engines = {&guest.e()};
    {
        Ticker ticker(engines);
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        std::vector<u32> waiting;
        OneWaits(h, g, guest, &waiting);
        Seen seen(the_start ? "L2 game" : "L1 game");
        if (!the_start) {
            Confirm(g, waiting);
            CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));
        }
        net.HoldUp();
        Sleep(DlpHub::kAliveMs + 300);
        const u32 refused = h.hub.failed_fast();
        if (the_start) {
            CHECK_EQ(g.AcceptClient(waiting.at(0)).res(), 0u);                       // L2: the accept is answered on the game's thread ...
            CHECK_EQ(g.StartDistribution().res(), DlpHub::kResultWirelessOff);       // L2: ... and the ONE StartDistribution is refused
        } else {
            CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), DlpHub::kResultWirelessOff);  // L1: the ONE passphrase is refused
        }
        CHECK_EQ(h.hub.failed_fast(), refused + 1);  // (the hub counts it: "dlp: dlp:SRVR command 0x.. answered "wireless off"" in the console's log)
        net.Resume();
        // the game never sends it again: it reads, and what it reads does not move (4 s here; on a console until the player leaves the lobby)
        const char* const stuck = the_start ? "state 2," : "state 8,";
        const bool moved = Frames(g, seen, 4000, [&](const std::string& line) { return line.compare(0, 8, stuck) != 0; });
        CHECK(!moved);  // L: the lobby stays where it was: 2 after a refused StartDistribution, 8 after a refused passphrase
        CHECK_EQ(g.ServerState(), the_start ? 2u : 8u);
        CHECK(guest.passphrase().empty());                    // L: the guest never gets a passphrase
        CHECK(guest.state() != AzaharDlpClient::kComplete);   // L: ... and never leaves for the child
        CHECK_EQ(g.calls(0x07), 1u);                          // L: ONE StartDistribution in all (accepted in L1, refused in L2)
        CHECK_EQ(g.calls(0x08), the_start ? 0u : 1u);         // L: ONE passphrase in L1 (refused), none in L2
        LeaveAndComeBack(h, g);
    }
    CHECK_RULES();
    std::printf("   the lobby stayed at state %u; after Finalize, Initialize and StartAccepting the hub accepts again\n", the_start ? 2u : 8u);
}

/// L3. The game's closeDistribute (its lobby's "back" while it distributes; its code, 0x17E360): with its own state 6 it calls EndAccepting
/// (0x06) and then StartAccepting by hand on channel 0. Azahar's host goes back to "initialised" from any state and accepts again; this hub's
/// EndAccepting does something only while it accepts (state 2) and its StartAccepting wants state 1: the first answers 0 and changes
/// nothing, the second is refused, the distribution goes on, and a passphrase the game sends afterwards reaches the guest - who leaves
/// for the child while the host's player is back at its list. (What the game shows then is not known. The window is the time the hub is in
/// state 6: 0.2 to 1.5 s.)
static void RunCloseDistribute() {
    std::printf("L3: the game's closeDistribute while the hub distributes: EndAccepting, then StartAccepting by hand\n");
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy");
    AzaharDlpClient guest(room, 0x12, "Az2");
    guest.rtt_ms = 250;  // (answers 250 ms away: the hub stays in state 6 for about a second)
    std::vector<UdsEngine*> engines = {&guest.e()};
    {
        Ticker ticker(engines);
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        std::vector<u32> waiting;
        OneWaits(h, g, guest, &waiting);
        Seen seen("L3 game");
        Confirm(g, waiting);
        CHECK(Frames(g, seen, 1000, [&](const std::string& line) { return line.compare(0, 8, "state 6,") == 0; }));
        CHECK_EQ(g.EndAccepting().res(), 0u);  // L3: EndAccepting while it distributes answers 0 ...
        const u32 after_end = g.ServerState();
        CHECK(after_end == 6 || after_end == 8);  // L3: ... and changes nothing (Azahar's host would be back at 1)
        CHECK_EQ(g.StartAccepting(1, 0).res(), kDlpInvalidState);  // L3: StartAccepting outside state 1 is refused (Azahar's host accepts again)
        CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));  // L3: the distribution went on
        CHECK(seen.Has("state 8, 1 client: 2=07"));
        // whatever the game shows now, a passphrase it sends reaches the guest, who leaves for the child
        CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kGoGraceMs + 1500, [&](const std::string& line) { return line.compare(0, 8, "state 9,") == 0; }));
        CHECK(WaitFor(3000, [&] { return guest.state() == AzaharDlpClient::kComplete; }));  // L3: the guest got the final all the same
        CheckModel(guest);
        CHECK_EQ(g.Finalize().res(), 0u);
        h.hub.OnGameUdsStarting();
        CHECK(EngFree(h));
    }
    std::printf("   EndAccepting answered 0 and changed nothing, StartAccepting was refused, the guest got the final\n");
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// R. Every region of the game (2026-11-14, "every region of a game": the ids of its row of azl/title_rules.hpp for USA, Japan, Taiwan and Korea have the values of
// the European row). What could have been the European id's in the hub is tried here for each of the five ids: the child the host announces.

struct IslandRegion {
    u64 tid;
    const char* region;
};
// The five title ids of the game, as its row of azl/title_rules.hpp has them (3DS Link's catalogue, the family ATS). Only the European game was read and
// captured: for the other four this part holds what the PLUGIN does with their id, not what their code does.
static constexpr IslandRegion kIslandRegions[] = {
    {0x00040000000F8200ULL, "Europe"}, {0x00040000000F8100ULL, "USA"},   {0x00040000000DD400ULL, "Japan"},
    {0x0004000000119300ULL, "Taiwan"}, {0x0004000000119B00ULL, "Korea"},
};
static constexpr int kLeastRegion = 500;

/// The hub as the plugin has it for the row of ONE region (the same values, the game's own title id), the game driven as the European code
/// reads, one Azahar guest: the child it reads in the announcement is 00040001 above the low word of THAT title with the index 0 - what the
/// guest's Download Play looks for among its own titles - on every frame of the announcement; the Download Play's network is the same for
/// every region (00002810); and the lobby goes to its end as the European one does.
static void RunRegion(const IslandRegion& r) {
    const u64 child = dlp_real::kChildTidHigh | (static_cast<u32>(r.tid) & 0xFFFFFF00u);
    std::printf("R: the console hosts Island Tour, %s (title %016llx): it announces the child %016llx\n", r.region, static_cast<unsigned long long>(r.tid),
                static_cast<unsigned long long>(child));
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy", r.tid);
    Tap wire(h.con);
    AzaharDlpClient guest(room, 0x12, "Az2");
    std::vector<UdsEngine*> engines = {&guest.e()};
    {
        Ticker ticker(engines);
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        OpenIsland(h, g);
        AzaharDlpClient::Probe pr;
        CHECK(ModelJoin(guest, h.mac(), &pr));
        CHECK(pr.all);
        CHECK_EQ(pr.child_tid, child);  // R: the guest reads the child of the HOST's region
        CHECK_EQ(pr.child_tid == kIslandChild, r.tid == kIslandTid);  // R: ... which is the European child for the European game and for no other
        CHECK_EQ(pr.max_nodes, kIslandPlayers);
        Seen seen(r.region);
        std::vector<u32> waiting;
        CHECK(WaitFor(3000, [&] {
            seen.Add(g.Frame(&waiting));
            return waiting.size() == 1;
        }));
        CHECK(seen.Has("state 2, 1 client: 2=40"));
        // the game's confirmation and the rest of the lobby, as for the European id
        CHECK_EQ(g.AcceptClient(waiting.empty() ? 2u : waiting[0]).res(), 0u);
        CHECK_EQ(g.StartDistribution().res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));
        CHECK(seen.Has("state 8, 1 client: 2=07"));
        CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kGoGraceMs + 1500, [&](const std::string& line) { return line.compare(0, 8, "state 9,") == 0; }));
        CHECK(WaitFor(3000, [&] { return guest.state() == AzaharDlpClient::kComplete; }));
        const std::vector<u8> pass = guest.passphrase();
        CHECK(pass.size() == 9 && std::memcmp(pass.data(), kDlpRealIslandPass, 9) == 0);
        CHECK_EQ(guest.received(4, 0) + guest.received(4, 1), 0u);  // R: nothing of the child was sent
        CHECK_EQ(h.real.counters().content, 0u);
        CHECK_EQ(h.real.counters().bad, 0u);
        CheckModel(guest);
        // the wire: every frame of the announcement is the one our builder makes for the child of THIS region, and frame 0 says that child
        u8 buf[dlp_real::kBufferBytes];
        u32 announce = 0, frame0 = 0, lobby_beacons = 0;
        for (const Tap::Out& o : wire.All()) {
            if (o.type == WifiPacket::Type::Beacon) {
                const u8* tag = NetTag(o.data);
                CHECK(tag != nullptr);
                if (tag != nullptr) {
                    CHECK_EQ(LoadBE32(tag + 4), kDlpRealIslandLobbyNet);  // R: the Download Play's own network, the same in every region
                    ++lobby_beacons;
                }
                continue;
            }
            Tap::Dlp d;
            if (!Tap::AsDlp(o, &d) || d.channel != RealDlpNet::kChannelAnnounce) {
                continue;
            }
            const unsigned idx = d.p.size() > 12 ? d.p[12] : 99;
            const std::size_t n = dlp_real::AnnounceFrame(buf, idx, child, kIslandPlayers, dlp_real::kTxSeed);
            CHECK(n != 0 && n == d.p.size() && std::memcmp(buf, d.p.data(), n) == 0);  // R: the announcement of this region's child, byte for byte
            if (idx == 0 && n == d.p.size()) {
                CHECK_EQ(dlp_real::AnnounceChildTid(d.p.data()), child);
                ++frame0;
            }
            ++announce;
        }
        CHECK(announce >= 5);
        CHECK(frame0 >= 1);
        CHECK(lobby_beacons >= 1);
        CHECK_EQ(g.Finalize().res(), 0u);
        h.hub.OnGameUdsStarting();
        CHECK(EngFree(h));
    }
    CHECK_RULES();
}

static void R_Regions() {
    constexpr std::size_t kRegions = sizeof(kIslandRegions) / sizeof(kIslandRegions[0]);
    CHECK_EQ(kRegions, 5u);
    CHECK_EQ(kIslandRegions[0].tid, kIslandTid);
    // five titles, five children: no two regions announce the same child (a guest never takes a host of another region for its own)
    for (std::size_t i = 0; i < kRegions; ++i) {
        CHECK((kIslandRegions[i].tid >> 32) == 0x00040000u);
        CHECK_EQ(static_cast<u32>(kIslandRegions[i].tid) & 0xFFu, 0u);
        for (std::size_t j = i + 1; j < kRegions; ++j) {
            CHECK((static_cast<u32>(kIslandRegions[i].tid) >> 8) != (static_cast<u32>(kIslandRegions[j].tid) >> 8));
        }
    }
    for (const IslandRegion& r : kIslandRegions) {
        const int c = g_checks;
        RunRegion(r);
        Ran(r.region, c, kLeastRegion);
    }
}

static void L_Limits() {
    int c = g_checks;
    RunRefused(false);
    Ran("L1, the passphrase refused", c, kLeastLimit);
    c = g_checks;
    RunRefused(true);
    Ran("L2, the start refused", c, kLeastLimit);
    c = g_checks;
    RunCloseDistribute();
    Ran("L3, the game's closeDistribute", c, kLeastLimit);
}

// ---------------------------------------------------------------------------------------------------------------------
// W. (1.1.4, H5) The host waits in its state 9 for the guests that went to start their child.

static constexpr int kLeastWait = 150;
static constexpr u32 kWaitMaxMs = 15000;     // the user's caps (REGOLE-IMPLEMENTAZIONE.md), written out
static constexpr u32 kWaitStayedMs = 10000;
static constexpr u32 kWaitLeaveMs = 5000;

/// How a part of W goes after the lobby's 9: who leaves the room when, who comes back when (ms after the first read of 9; 0 = never).
struct WPlan {
    const char* what;
    u32 n;               // the clients
    u32 out_ms[3];       // when each leaves the room (its Download Play application ends); 0 = it stays (an emulator)
    u32 back_ms[3];      // when its child is in the room; 0 = never
    u32 end_lo, end_hi;  // the end of the wait the game reads (GetConnectingClients 0), ms after the first read of 9
    int finalize_at;     // W7: Finalize while the wait lasts (ms after 9), -1 = after the end
};

static void RunWait(const WPlan& w) {
    std::printf("W: %s\n", w.what);
    g_rules.Reset();
    MockRoom room;
    Host h(room, 1, "Daisy");
    h.real.SetChildWait(true);  // (what MakeRealDlpHub gives a game that hosts the real Download Play: Tests/plugin_dlp_test.cpp)
    Tap wire(h.con);
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines;
    for (u8 i = 0; i < w.n; ++i) {
        const std::string name = "Az" + std::to_string(i + 2);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        engines.push_back(&cl.back()->e());
    }
    // the room as the plugin's OnRoomInfo gives it: everybody plays, the guests in the system's Download Play application
    std::vector<Mac> all = {h.mac()}, guests;
    for (auto& m : cl) {
        all.push_back(m->mac());
        guests.push_back(m->mac());
    }
    h.e().OnRoomMembers(all);
    h.e().SetDownloadPlayMembers(guests);
    {
        Ticker ticker(engines);
        NetThread net(h);
        CHECK(WaitFor(2000, [&] { return net.rounds() >= 2; }));
        Game g(h);
        OpenIsland(h, g);
        Seen seen("the game");
        for (u8 i = 0; i < w.n; ++i) {
            AzaharDlpClient::Probe pr;
            CHECK(ModelJoin(*cl[i], h.mac(), &pr));
        }
        std::vector<u32> waiting;
        CHECK(WaitFor(3000, [&] {
            g.Frame(&waiting);
            return waiting.size() == w.n;
        }));
        Confirm(g, waiting);
        CHECK(Frames(g, seen, DlpLobby::kStartGraceMs + 1000, [&](const std::string& line) { return line.compare(0, 8, "state 8,") == 0; }));
        CHECK_EQ(g.SendPassphrase(kDlpRealIslandPass).res(), 0u);
        CHECK(Frames(g, seen, DlpLobby::kGoGraceMs + 1500, [&](const std::string& line) { return line.compare(0, 8, "state 9,") == 0; }));
        const u64 t9 = platform::NowMs();
        std::string want9 = "state 9, " + std::to_string(w.n) + " client" + (w.n == 1 ? "" : "s") + ":";
        for (u32 k = 0; k < w.n; ++k) {
            want9 += " " + std::to_string(k + 2) + "=09";
        }
        CHECK(seen.Has(want9));  // W1/W5: every guest awaited, each answered as one that has the passphrase
        CHECK_EQ(h.real.ChildrenAwaited(), (1u << w.n) - 1u);
        for (u32 k = 0; k < w.n; ++k) {
            const Rep info = g.ClientInfo(k + 2);
            const Rep st = g.ClientState(k + 2);
            CHECK_EQ(info.res(), 0u);  // W5
            CHECK_EQ(st.res(), 0u);
            CHECK_EQ(st.w[2], 0x09010000u | (k + 2));
        }
        CHECK_EQ(g.ClientInfo(w.n + 2).res(), R_NO_DATA);  // (an id that is not awaited: as before)
        for (auto& m : cl) {
            CHECK(WaitFor(3000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
        }
        const std::size_t mark9 = wire.Mark();
        const u32 posted9 = h.hub.served_posted();
        // the room's lists as the plan says, every 50 ms (the room sends one at each change); the game reads every 16 ms
        u32 ended = 0;
        bool finalized = false;
        u32 last_total = w.n;
        bool down_only = true;
        for (;;) {
            const u32 at = static_cast<u32>(platform::NowMs() - t9);
            std::vector<Mac> now = {h.mac()};
            for (u32 k = 0; k < w.n; ++k) {
                const bool out = w.out_ms[k] != 0 && at >= w.out_ms[k] && (w.back_ms[k] == 0 || at < w.back_ms[k]);
                if (!out) {
                    now.push_back(cl[k]->mac());
                }
            }
            h.e().OnRoomMembers(now);
            if (w.finalize_at >= 0 && !finalized && at >= static_cast<u32>(w.finalize_at)) {
                // W7: the player leaves the lobby while the wait lasts: state 0 as today, and the game's own network finds the engine free
                CHECK_EQ(g.Finalize().res(), 0u);
                CHECK_EQ(g.ServerState(), 0u);
                h.hub.OnGameUdsStarting();
                CHECK(EngFree(h));
                CHECK(h.hub.served_posted() > posted9);  // (the hub was still active: the request went to the network thread)
                finalized = true;
                break;
            }
            const u32 total = g.ClientsTotal();
            seen.Add(g.Frame());
            down_only = down_only && total <= last_total;
            last_total = total;
            if (total == 0) {
                ended = at;
                break;
            }
            if (at > kWaitMaxMs + 2000) {
                break;
            }
            Sleep(16);
        }
        if (!finalized) {
            std::printf("   the game read 0 after %u ms in state 9 (wanted %u..%u)\n", ended, w.end_lo, w.end_hi);
            CHECK(ended >= w.end_lo && ended <= w.end_hi);
            CHECK(ended < 20000);  // (the game's own cap in its state 9)
            CHECK(down_only);      // W1: 3 -> 2 -> 1 -> 0, never up again
            CHECK_EQ(h.real.ChildrenAwaited(), 0u);
            // W8: the wait is over, the hub is idle again: the game's own network starts with no request to the network thread
            Sleep(100);
            CHECK_EQ(g.Finalize().res(), 0u);
            const u32 posted = h.hub.served_posted();
            h.hub.OnGameUdsStarting();
            CHECK_EQ(h.hub.served_posted(), posted);
            CHECK(EngFree(h));
        }
        // W6: nothing of the host on the wire while it waited - no frame at all (the Download Play's network is gone) - and no request refused
        u32 after9 = 0;
        const std::vector<Tap::Out> out = wire.All();
        for (std::size_t k = mark9; k < out.size(); ++k) {
            after9 += out[k].type == WifiPacket::Type::Data ? 1u : 0u;
        }
        CHECK_EQ(after9, 0u);
        CHECK_EQ(h.hub.cancelled(), 0u);
        CHECK_EQ(h.hub.failed_fast(), 0u);
        CHECK_EQ(h.real.counters().content, 0u);
        for (auto& m : cl) {
            CheckModel(*m);
        }
    }
    cl.clear();
    CHECK_RULES();
}

static void W_ChildWait() {
    // W1: three consoles leave the room 300 ms after 9 and their children come back at +1000 / +1500 / +2000
    const WPlan w1 = {"W1, three consoles go to their child and come back", 3, {300, 300, 300}, {1000, 1500, 2000}, 1900, 2600, -1};
    // W2: nobody leaves (three Azahar): each holds nothing after 5 s
    const WPlan w2 = {"W2, three Azahar stay in the room", 3, {0, 0, 0}, {0, 0, 0}, kWaitLeaveMs - 300, kWaitLeaveMs + 600, -1};
    // W3: one console leaves and never comes back, two Azahar stay: the cap with an Azahar
    const WPlan w3 = {"W3, a console that never comes back and two Azahar", 3, {300, 0, 0}, {0, 0, 0}, kWaitStayedMs - 300, kWaitStayedMs + 600, -1};
    // W4: one console alone, never back: the longest wait, within the game's 20 s
    const WPlan w4 = {"W4, one console that never comes back", 1, {300, 0, 0}, {0, 0, 0}, kWaitMaxMs - 300, kWaitMaxMs + 600, -1};
    // W7: the player leaves the lobby while the wait lasts
    const WPlan w7 = {"W7, Finalize while the wait lasts", 2, {300, 300}, {0, 0}, 0, 0, 2000};
    for (const WPlan* w : {&w1, &w2, &w3, &w4, &w7}) {
        const int c = g_checks;
        RunWait(*w);
        Ran(w->what, c, kLeastWait);
    }
}

int main() {
    const int c = g_checks;
    G_Capture();
    Ran("G", c, kLeastCapture);
    H_HostThree();
    C_WantsContent();
    R_Regions();
    L_Limits();
    W_ChildWait();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
