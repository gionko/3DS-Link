// D6 (1.1.4): the games whose players all have the game - Kirby Triple Deluxe, Kirby Planet Robobot, Mario Tennis Open - on the REAL
// Download Play (the RealDlp mode: our DlpLobby on the adapter RealDlpNet, azl/dlp_real_net.hpp), each held to the capture of Azahar
// emulators that played IT through a room (analysis/piano-1.1.4-2026-11-15/d6-cattura; its bytes are Tests/dlp_real_<game>_golden.inc, cut
// out by Tests/make_dlp_real_lobby_games_goldens.py: the title, the icon, the passphrase, the names and the addresses are not in them). A row
// of azl/title_rules.hpp moves from Lobby to RealDlp ONLY with its own capture (5-L4.md, section 2; the register: "one row at a time, each
// with its capture"): the table of this test is that rule. A game whose golden file is not there is SKIPPED, loudly, with the reason, and
// its row must not be RealDlp; a game whose golden is there runs every part, whatever its row says (the row is the owner's decision).
// The rig is the one of Tests/dlp_real_mk7_test.cpp: engines joined by the in-memory room of Tests/mock_room.hpp, real time, the Azahar-peer
// rule checker on every frame, the models of Azahar's Download Play client and host (Tests/azahar_dlp_peer.hpp, azahar_dlp_host.hpp) with
// the game's behaviour of the capture (the acceptance, the clients' goodbye).
//
//   G  the capture against the codec, no network: the five frames of the announcement and EVERY packet of the session are what our builders
//      make with the capture's seed (the announcement's sizes, title and icon aside: ours are zero, design 2.2 option c; the block an Azahar
//      client never initialises in its Finish ack); every client answered StartDistribution WITHOUT the content
//   A  a console HOSTS with the capture's Azahar clients: the announcement to each as a spectator, the admission, StartDistribution, the
//      passphrase, the lobby 9; EVERY frame the host sent is the capture's form
//   B  a console SEARCHES and finds an Azahar host of the game: GetTitleInfoInOrder (the child's unique id), GetServerInfo (max 4), joins as a
//      fake client next to an Azahar client, START, the passphrase; every answer it sent is the capture's form
//   C  a console hosts, an Azahar client, a console client and another Azahar client in (four nodes, the game's four players); the last one
//      leaves before the start: the host and the clients that remain know at once (design D17), the next one takes the place, START and GO
//   F  Kirby (Triple Deluxe, Planet Robobot) only: the hub on the adapter with the network thread GONE - Finalize and StopSession answer 0
//      (the game throws a fatal error at a negative one: dlp_hub.hpp, TD 0x12f7b4 and 0x12f9f0, PR 0x10a9e8), for a creator that accepts
//      and for a searcher that scans
//   S  the games whose row has the console's seed (Mario Tennis Open) only: the seed set on the lobby is the one every node list gives - the
//      host's GetClientInfo, the searcher's GetNodeInfo of the host and of itself
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_lobby_games_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp
//       Sources/azl/dlp_real_wire.cpp dlph_wire.o <the core sources of Tests/run.sh>
#include <deque>

#include "dlp_net_rig.hpp"

#include "azahar_dlp_host.hpp"
#include "azahar_dlp_peer.hpp"
#include "azl/dlp_hub.hpp"
#include "azl/dlp_ipc.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/title_rules.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

/// A game as its capture shows it: written by Tests/make_dlp_real_lobby_games_goldens.py at the end of the game's golden file.
struct DlpRealLobbyGolden {
    const char* name;
    u32 low;                       // the application's low word (the region that was captured)
    u64 child;                     // frame 0, +0x10: the child its host announces
    unsigned max_nodes;            // frame 0, +0x20
    u32 transfer_size, required_size;  // frame 0, +0x38 / +0x3C (zero in our announcement)
    const std::uint8_t* seed;      // [4] the seed of the capture's checksums
    const std::uint8_t* pass;      // [9] the STAND-IN of the passphrase, sealed into the finals with that seed
    const DlpRealGolden* announce;  // [5]
    const DlpRealGolden* dialog;    // every packet of channel 2 of the session, in the capture's order
    std::size_t dialog_n;
    unsigned clients;              // the Azahar clients of the session
    bool auto_accept;              // the Auth left at once (the game's acceptance is automatic)
    u32 game_net;                  // the wlancommid of the game's own network after the final
    unsigned game_net_nodes;
    int final_to_bye_ms;           // from the final to the first client's goodbye (-1: none)
};

// The golden files that exist: a game's file comes with its capture (make_dlp_real_lobby_games_goldens.py <td|pr|mto>) and nothing else.
#if __has_include("dlp_real_kirby_td_golden.inc")
#include "dlp_real_kirby_td_golden.inc"
#define AZL_D6_TD (&kDlpRealKirbyTdGame)
#else
#define AZL_D6_TD nullptr
#endif
#if __has_include("dlp_real_robobot_golden.inc")
#include "dlp_real_robobot_golden.inc"
#define AZL_D6_PR (&kDlpRealRobobotGame)
#else
#define AZL_D6_PR nullptr
#endif
#if __has_include("dlp_real_tennis_golden.inc")
#include "dlp_real_tennis_golden.inc"
#define AZL_D6_MTO (&kDlpRealTennisGame)
#else
#define AZL_D6_MTO nullptr
#endif

/// The games of D6, with what their captures of 2026-11-16 said when they have no golden (the row stays Lobby; the reason is printed).
struct LobbyGame {
    const char* name;
    u32 low;
    bool kirby;  // F: the game throws a fatal error at a negative Finalize
    const DlpRealLobbyGolden* golden;
    const char* without;
};
static const LobbyGame kGames[] = {
    {"Kirby Triple Deluxe", 0x0010C000, true, AZL_D6_TD,
     "no golden: in the capture of three Azahar (2026-11-16) the host announced and its clients read the title every second, but the game never "
     "listed the host - a defect of Azahar's client side; the row stays Lobby (it plays between consoles)"},
    {"Kirby Planet Robobot", 0x00183600, true, AZL_D6_PR, "no golden"},
    {"Mario Tennis Open", 0x0007C800, false, AZL_D6_MTO,
     "no golden: in the capture of two Azahar (2026-11-16) the Download Play went through to the passphrase and the game's network, then the "
     "game stopped at the start of the match (the friend code seed of Azahar's Download Play network, as with Mario Kart 7); the row stays "
     "Lobby until the owner decides"},
};

static constexpr u64 kApp = 0x0004000000000000ULL;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// The first packet `id` of the session to or from `node` (0: any node), or null.
static const DlpRealGolden* Find(const DlpRealLobbyGolden& g, const char* id, unsigned node = 0) {
    for (std::size_t k = 0; k < g.dialog_n; ++k) {
        const DlpRealGolden& p = g.dialog[k];
        if (std::strcmp(p.id, id) == 0 && (node == 0 || p.src_node == node || p.dst_node == node)) {
            return &p;
        }
    }
    return nullptr;
}
static const DlpRealGolden& Need(const DlpRealLobbyGolden& g, const char* id, unsigned node = 0) {
    const DlpRealGolden* p = Find(g, id, node);
    if (p == nullptr) {
        p = Find(g, id);
    }
    if (p == nullptr) {
        std::fprintf(stderr, "%s: no golden %s\n", g.name, id);
        std::abort();
    }
    return *p;
}
/// `p` sealed with `seed` is the golden, byte for byte.
static bool Same(std::vector<u8> p, const DlpRealGolden& g, const u8* seed) {
    if (p.size() != g.size) {
        return false;
    }
    dlp_real::Seal(seed, p.data(), p.size());
    return std::memcmp(p.data(), g.bytes, g.size) == 0;
}
static std::vector<u8> Bytes(const DlpRealGolden& g) {
    return std::vector<u8>(g.bytes, g.bytes + g.size);
}
/// The capture's notice and its answer, or Luigi's capture's when the session has none (then compared with Luigi's seed).
static const DlpRealGolden& NoticeOf(const DlpRealLobbyGolden& g, const char* id, const u8** seed) {
    const DlpRealGolden* p = Find(g, id);
    *seed = p != nullptr ? g.seed : kDlpRealLuigiSeed;
    if (p != nullptr) {
        return *p;
    }
    for (const DlpRealGolden& l : kDlpRealLuigiDialog) {
        if (std::strcmp(l.id, id) == 0) {
            return l;
        }
    }
    std::abort();
}

// ---------------------------------------------------------------------------------------------------------------------
// A console of the game: the adapter told the ceiling of the row (four), the lobby on it, and the game's reads through the IPC words.

class GameMem {
public:
    u32 Alloc(u32 size) {
        regions_.push_back({next_, std::vector<u8>(size, 0xCC)});
        next_ += (size + 0x4F) & ~15u;
        return regions_.back().addr;
    }
    u8* Find(u32 addr, u32 size) {
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.bytes.size()) {
                return r.bytes.data() + (addr - r.addr);
            }
        }
        return nullptr;
    }
    void Refill(u32 addr) {
        for (Region& r : regions_) {
            if (r.addr == addr) {
                std::memset(r.bytes.data(), 0xCC, r.bytes.size());
            }
        }
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = [](void* ctx, u32 addr, u32 size) { return static_cast<GameMem*>(ctx)->Find(addr, size); };
        return r;
    }

private:
    struct Region {
        u32 addr;
        std::vector<u8> bytes;
    };
    std::deque<Region> regions_;
    u32 next_ = 0x08000000;
};

struct Box {
    Box(MockRoom& room, u8 last, const char* name, u32 tid_low)
        : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(tid_low, last, name, 4)) {
        net.SetIdentity(Me(0xF000u + last, name));
        net.SetPlayers(4);
        svc.SetLobby(&lobby);
        a_ids = mem.Alloc(64);
        a_server = mem.Alloc(kServerInfoSize);
        a_title = mem.Alloc(kTitleInfoSize);
        Put(name16, name);
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    std::string Ids() {
        const u8* p = mem.Find(a_ids, 64);
        std::string s;
        for (u32 i = 0; i < 32 && p[2 * i] != 0xCC; ++i) {
            s += (s.empty() ? "" : ",") + std::to_string(LoadLE16(p + 2 * i));
        }
        return s;
    }
    /// dlp:SRVR GetConnectingClients (a vector of 3): "total:ids".
    std::string Clients() {
        u32 cmd[16] = {0x000B0042, 3, (6u << 4) | 0xC, a_ids};
        mem.Refill(a_ids);
        svc.Dispatch(DlpKind::Srvr, cmd, nullptr, mem.Resolver());
        return cmd[1] != 0 ? "error" : std::to_string(cmd[2]) + ":" + Ids();
    }
    /// dlp:FKCL GetConnectingNodes (16 elements): "total:ids".
    std::string Nodes() {
        const u32 sb[2] = {(32u << 14) | 2, a_ids};
        u32 cmd[16] = {0x000D0040, 16};
        mem.Refill(a_ids);
        svc.Dispatch(DlpKind::Fkcl, cmd, sb, mem.Resolver());
        return cmd[1] != 0 ? "error" : std::to_string(cmd[2]) + ":" + Ids();
    }
    u32 TitleInOrder(bool restart) {
        const u32 sb[2] = {(kTitleInfoSize << 14) | 2, a_title};
        u32 cmd[16] = {0x00090040, restart ? 1u : 0u};
        mem.Refill(a_title);
        svc.Dispatch(DlpKind::Fkcl, cmd, sb, mem.Resolver());
        return cmd[1];
    }
    const u8* Title() {
        return mem.Find(a_title, kTitleInfoSize);
    }
    u32 ServerInfo(const Mac& m) {
        const u32 sb[2] = {(kServerInfoSize << 14) | 2, a_server};
        u32 cmd[16] = {0x00070080, LoadLE32(m.data()), static_cast<u32>(m[4] | (m[5] << 8))};
        mem.Refill(a_server);
        svc.Dispatch(DlpKind::Fkcl, cmd, sb, mem.Resolver());
        return cmd[1];
    }
    const u8* Server() {
        return mem.Find(a_server, kServerInfoSize);
    }
    u32 Status() const {
        return lobby.ClientStatusWord() >> 24;
    }
    Console con;
    RealDlpNet net;
    DlpLobby lobby;
    DlpService svc;
    GameMem mem;
    u32 a_ids = 0, a_server = 0, a_title = 0;
    u16 name16[kNameUnits];
};
using Boxes = std::vector<Box*>;

template <typename F>
static bool PumpUntil(const Boxes& bs, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        for (Box* b : bs) {
            b->Pump();
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
static void PumpFor(const Boxes& bs, u32 ms) {
    PumpUntil(bs, ms, [] { return false; });
}
template <typename F>
static void While(const Boxes& bs, F f) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        f();
        done = true;
    });
    while (!done) {
        for (Box* b : bs) {
            b->Pump();
        }
        Sleep(2);
    }
    t.join();
}

/// Every frame a console sent, whole.
class Tap {
public:
    struct Out {
        WifiPacket::Type type;
        Mac dest;
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
            v_.push_back({w.type, w.destination_address, w.data});
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
    static bool AsLogoff(const Out& o, EapolLogoff* lo) {
        EtherType t;
        return o.type == WifiPacket::Type::Data && GetFrameEtherType(o.data, &t) && t == EtherType::EAPoL && ParseEapolLogoffFrame(o.data, lo);
    }

private:
    std::mutex m_;
    std::vector<Out> v_;
};

static u16 MaskOf(UdsEngine& e) {
    u16 node = 0, m = 0;
    if (!e.NodeSnapshot(&node, &m)) {
        m = 0;
    }
    return m;
}
static u16 NodeOf(UdsEngine& e) {
    u16 node = 0, m = 0;
    if (!e.NodeSnapshot(&node, &m)) {
        node = 0;
    }
    return node;
}

/// An Azahar client comes in as Azahar's Download Play does: it hears the host, reads the announcement as a spectator, says goodbye, joins.
static bool ModelJoin(const Boxes& bs, AzaharDlpClient& m, const Mac& host, AzaharDlpClient::Probe* pr) {
    if (!PumpUntil(bs, 4000, [&] { return m.Heard(host); })) {
        return false;
    }
    bool ok = false;
    While(bs, [&] {
        *pr = m.ProbeHost(host);
        ok = pr->all && m.StartSession(host);
    });
    return ok;
}
static void CheckModel(AzaharDlpClient& m) {
    for (const std::string& v : m.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(m.violations().empty());
    CHECK(!m.session_broken);
}
/// The game's acceptance: automatic in the capture, or AcceptClient by hand.
static void Accept(const Boxes& bs, Box& h, const DlpRealLobbyGolden& g, u16 node) {
    if (!g.auto_accept) {
        CHECK(PumpUntil(bs, 2000, [&] { return h.Clients().find(std::to_string(node)) != std::string::npos; }));
        CHECK_EQ(h.lobby.ServerAcceptClient(node), 0u);
    }
    CHECK(PumpUntil(bs, 2000, [&] { return h.net.PeerState(static_cast<u8>(node)) == RealDlpNet::kAuthed; }));
}
/// The console's searcher: InitializeWithName for 4 lobbies, StartScan with the child's title id, then GetTitleInfoInOrder until the host is there.
static bool FindHost(const Boxes& bs, Box& c, const Mac& host, u64 child, u32 ms = 5000) {
    CHECK_EQ(c.lobby.ClientInitialize(c.name16, 4), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(child), 0u);
    GroupView v;
    return PumpUntil(bs, ms, [&] { return c.lobby.ClientTitleInfo(host, &v) == 0; });
}
static u32 JoinHost(const Boxes& bs, Box& c, const Mac& host) {
    c.lobby.ClientStopScan();
    u32 r = 0xFFFFFFFF;
    if (c.lobby.ClientJoinBegin(host, &r) == JoinStep::Pending) {
        PumpUntil(bs, 5000, [&] { return c.lobby.ClientJoinPoll(&r); });
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// G. The capture against the codec.

static void G_Capture(const DlpRealLobbyGolden& g) {
    std::printf("G: %s, the capture against the codec\n", g.name);
    CHECK_EQ(g.child, dlp_real::kChildTidHigh | g.low);
    CHECK_EQ(g.max_nodes, 4u);
    CHECK(g.game_net != 0x00002810u && g.game_net_nodes >= 2 && g.game_net_nodes <= 4);
    CHECK(g.clients >= 1 && g.clients <= 3);
    u8 buf[dlp_real::kBufferBytes];
    static const std::size_t kLen[5] = {768, 1448, 1448, 1448, 1464};
    for (unsigned i = 0; i < 5; ++i) {
        const DlpRealGolden& a = g.announce[i];
        CHECK_EQ(a.size, kLen[i]);
        CHECK_EQ(dlp_real::AnnounceFrame(buf, i, g.child, 4, g.seed), a.size);
        const dlp_real::Packet p = dlp_real::ParseAnnounce(a.bytes, a.size);
        CHECK(p.kind == dlp_real::Kind::Announce);
        CHECK_EQ(p.index, i);
        CHECK(std::memcmp(buf, a.bytes, 8) == 0);
        CHECK(std::memcmp(buf + 12, a.bytes + 12, 4) == 0);
        if (i == 0) {
            CHECK(std::memcmp(buf + 0x10, a.bytes + 0x10, 0x28) == 0);  // the child's title id, 00 10, 01 01, 00 03 ff c0, max_nodes 4, zeros
            CHECK(std::memcmp(buf + 0x2F8, a.bytes + 0x2F8, 8) == 0);
            CHECK_EQ(dlp_real::AnnounceChildTid(a.bytes), g.child);
            CHECK_EQ(LoadBE32(a.bytes + 0x38), g.transfer_size);
            CHECK_EQ(LoadBE32(a.bytes + 0x3C), g.required_size);
        } else if (i == 4) {
            CHECK(std::memcmp(buf, a.bytes, a.size) == 0);  // frame 4 is zeros in Azahar's too: the same bytes, checksum included
        }
    }
    // the dialogue: every packet is the one our builder makes for that sequence and id with the capture's seed; a client's answer is ours to
    // the host's last packet of the kind it answers
    u32 kinds[11] = {};
    const DlpRealGolden* last[4][5] = {};  // [node - 2][Auth, StartDist, Finish, Notice, Final]
    for (std::size_t k = 0; k < g.dialog_n; ++k) {
        const DlpRealGolden& p = g.dialog[k];
        const bool from_host = p.src_node == 1;
        const unsigned node = from_host ? p.dst_node : p.src_node;
        CHECK(node >= 2 && node <= 4);
        if (node < 2 || node > 4) {
            continue;
        }
        const dlp_real::Packet q = from_host ? dlp_real::ParseFromHost(p.bytes, p.size) : dlp_real::ParseFromClient(p.bytes, p.size);
        CHECK(q.kind != dlp_real::Kind::None);
        ++kinds[static_cast<u8>(q.kind)];
        const u8 rid[2] = {p.bytes[0x0E], p.bytes[0x0F]};
        const u8 seq = p.bytes[0x0D];
        std::vector<u8> want = Bytes(p);
        std::size_t n = 0;
        int slot = -1;
        if (std::strcmp(p.id, "A1") == 0) {
            n = dlp_real::Auth(buf, g.seed);
            slot = 0;
        } else if (std::strcmp(p.id, "A3") == 0) {
            CHECK_EQ(seq, 1u);  // (what our host sends: 1, 2, 3)
            n = dlp_real::StartDist(buf, g.seed, seq, rid);
            slot = 1;
        } else if (std::strcmp(p.id, "A5") == 0) {
            CHECK_EQ(seq, 2u);
            n = dlp_real::Finish(buf, g.seed, seq, rid);
            slot = 2;
        } else if (std::strcmp(p.id, "A8") == 0) {
            n = dlp_real::Notice(buf, g.seed, seq, rid);
            slot = 3;
        } else if (std::strcmp(p.id, "A7") == 0) {
            CHECK_EQ(seq, 3u);
            CHECK(std::memcmp(p.bytes + dlp_real::kPassphraseAt, g.pass, 9) == 0);  // (the stand-in: the capture's passphrase is not in the file)
            n = dlp_real::Final(buf, g.seed, seq, rid, g.pass);
            slot = 4;
        } else {
            const int asked = std::strcmp(p.id, "A2") == 0 ? 0 : std::strcmp(p.id, "A4") == 0 ? 1 : std::strcmp(p.id, "A6") == 0 ? 2 : 3;
            const DlpRealGolden* h = last[node - 2][asked];
            CHECK(h != nullptr);
            if (h == nullptr) {
                continue;
            }
            n = dlp_real::Reply(buf, g.seed, dlp_real::ParseFromHost(h->bytes, h->size));
            if (std::strcmp(p.id, "A4") == 0) {
                CHECK(!q.needs_content);  // the fake client: it has the game, it asks for nothing
                CHECK_EQ(p.size, dlp_real::kDistAckBytes);
            }
            if (std::strcmp(p.id, "A6") == 0) {
                // (declared: +0x16..0x19 is the block Azahar acknowledges, a member its fake client never initialises; the host does not read it)
                std::memset(want.data() + 0x16, 0, 4);
                dlp_real::Seal(g.seed, want.data(), want.size());
            }
        }
        if (slot >= 0) {
            last[node - 2][slot] = &p;
        }
        if (n != want.size() || std::memcmp(buf, want.data(), n) != 0) {
            std::printf("   %s t=%s node %u -> %u differs: ours %s\n", p.id, p.time, p.src_node, p.dst_node, Hex(buf, n).c_str());
        }
        CHECK(n == want.size() && std::memcmp(buf, want.data(), n) == 0);
    }
    CHECK(kinds[static_cast<u8>(dlp_real::Kind::Auth)] >= g.clients);
    CHECK_EQ(kinds[static_cast<u8>(dlp_real::Kind::AuthAck)], kinds[static_cast<u8>(dlp_real::Kind::Auth)]);
    for (dlp_real::Kind k : {dlp_real::Kind::StartDist, dlp_real::Kind::DistAck, dlp_real::Kind::Finish, dlp_real::Kind::FinishAck, dlp_real::Kind::Final}) {
        CHECK_EQ(kinds[static_cast<u8>(k)], g.clients);  // one of each for each client
    }
    CHECK(kinds[static_cast<u8>(dlp_real::Kind::NoticeAck)] <= kinds[static_cast<u8>(dlp_real::Kind::Notice)]);  // (a client gone at the final answers no more)
    std::printf("   %zu packets: Auth %u, notices %u; the clients' goodbye %d ms after the final\n", g.dialog_n, kinds[static_cast<u8>(dlp_real::Kind::Auth)],
                kinds[static_cast<u8>(dlp_real::Kind::Notice)], g.final_to_bye_ms);
}

// ---------------------------------------------------------------------------------------------------------------------
// A. A console hosts the game with the capture's Azahar clients.

static void A_Host(const DlpRealLobbyGolden& g) {
    std::printf("A: a console hosts %s, %u Azahar client(s) as in the capture\n", g.name, g.clients);
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Host", g.low);
    Tap wire(h.con);
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines = {&h.e()};
    for (u8 i = 0; i < g.clients; ++i) {
        const std::string name = "Az" + std::to_string(i + 2);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        cl.back()->leave_at_final = g.final_to_bye_ms >= 0 && g.final_to_bye_ms < 100;
        engines.push_back(&cl.back()->e());
    }
    Ticker ticker(engines);
    const Boxes bs = {&h};
    CHECK_EQ(h.lobby.ServerInitialize(3, 0, h.name16), 0u);  // (four players: three clients)
    CHECK_EQ(h.lobby.ServerStartAccepting(!g.auto_accept, 0), 0u);
    CHECK(h.net.Holds());
    CHECK_EQ(h.net.players(), 4u);
    CHECK_EQ(h.e().GetConnectionStatus().max_nodes, 4u);  // as the capture's frame 0 and beacon
    std::string ids;
    for (u8 i = 0; i < g.clients; ++i) {
        AzaharDlpClient& m = *cl[i];
        const u16 node = static_cast<u16>(i + 2);
        AzaharDlpClient::Probe pr;
        CHECK(ModelJoin(bs, m, h.mac(), &pr));
        CHECK(pr.all);
        CHECK(pr.ms_to_all <= 1000);
        CHECK_EQ(pr.child_tid, g.child);
        CHECK_EQ(pr.max_nodes, 4u);
        CHECK_EQ(Str(pr.name), std::string("Host"));
        CHECK_EQ(NodeOf(m.e()), node);
        Accept(bs, h, g, node);
        ids += (ids.empty() ? "" : ",") + std::to_string(node);
        const std::string want = std::to_string(i + 1) + ":" + ids;
        CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == want; }));
        CHECK_EQ(h.Clients(), want);
    }
    const std::size_t mark_start = wire.Mark();
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(bs, 1500, [&] { return h.lobby.ServerState() == 8; }));
    for (u8 i = 0; i < g.clients; ++i) {
        CHECK_EQ(h.net.PeerState(static_cast<u8>(i + 2)), RealDlpNet::kReady);
    }
    CHECK_EQ(h.lobby.ServerSendPassphrase(g.pass), 0u);
    CHECK(PumpUntil(bs, 2500, [&] { return h.lobby.ServerState() == 9; }));
    CHECK_EQ(h.lobby.ServerState(), 9u);
    for (auto& m : cl) {
        CHECK(WaitFor(1000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
        const std::vector<u8> pass = m->passphrase();
        CHECK(pass.size() == 9 && std::memcmp(pass.data(), g.pass, 9) == 0);
        CHECK_EQ(m->received(3, 1), 1u);  // ONE StartDistribution each
        CHECK_EQ(m->received(6, 1), 1u);  // ONE final each
        CheckModel(*m);
    }
    {
        // from StartDistribution on the host sends no Logoff and no node map (the door is shut)
        u32 late = 0;
        const std::vector<Tap::Out> out = wire.All();
        for (std::size_t k = mark_start; k < out.size(); ++k) {
            EapolLogoff lo;
            late += Tap::AsLogoff(out[k], &lo) || out[k].type == WifiPacket::Type::NodeMap ? 1u : 0u;
        }
        CHECK_EQ(late, 0u);
    }
    // EVERY frame the host sent, against the capture's form
    u32 n1[5] = {}, a1 = 0, a3 = 0, a5 = 0, a7 = 0, a8 = 0;
    u8 buf[dlp_real::kBufferBytes];
    for (const Tap::Out& o : wire.All()) {
        Tap::Dlp d;
        if (!Tap::AsDlp(o, &d)) {
            continue;
        }
        if (d.channel == 1) {
            CHECK_EQ(d.dest, 0xFFFFu);
            const dlp_real::Packet p = dlp_real::ParseAnnounce(d.p.data(), d.p.size());
            CHECK(p.kind == dlp_real::Kind::Announce);
            if (p.kind != dlp_real::Kind::Announce) {
                continue;
            }
            ++n1[p.index];
            const DlpRealGolden& a = g.announce[p.index];
            CHECK_EQ(d.p.size(), a.size);
            const std::size_t n = dlp_real::AnnounceFrame(buf, p.index, g.child, 4, dlp_real::kTxSeed);
            CHECK(n == d.p.size() && std::memcmp(buf, d.p.data(), n) == 0);
            if (p.index == 4) {
                CHECK(Same(d.p, a, g.seed));
            } else if (p.index == 0) {
                CHECK(std::memcmp(d.p.data() + 0x10, a.bytes + 0x10, 0x28) == 0);
            }
            continue;
        }
        CHECK_EQ(d.channel, 2u);
        CHECK(d.dest >= 2 && d.dest <= 4);
        const dlp_real::Packet p = dlp_real::ParseFromHost(d.p.data(), d.p.size());
        if (p.kind == dlp_real::Kind::Auth) {
            ++a1;
            CHECK(Same(d.p, Need(g, "A1", d.dest), g.seed));
        } else if (p.kind == dlp_real::Kind::StartDist) {
            ++a3;
            CHECK(Same(d.p, Need(g, "A3", d.dest), g.seed));
        } else if (p.kind == dlp_real::Kind::Finish) {
            ++a5;
            CHECK(Same(d.p, Need(g, "A5", d.dest), g.seed));
        } else if (p.kind == dlp_real::Kind::Final) {
            ++a7;
            CHECK(Same(d.p, Need(g, "A7", d.dest), g.seed));
        } else if (p.kind == dlp_real::Kind::Notice) {
            ++a8;
            const u8* seed = nullptr;
            const DlpRealGolden& want = NoticeOf(g, "A8", &seed);
            std::vector<u8> mine = d.p;
            if (mine.size() == want.size) {
                mine[0x0D] = want.bytes[0x0D];  // (the notice's sequence counts on with every answer)
            }
            CHECK(Same(mine, want, seed));
        } else {
            std::printf("   a packet of the host that the capture does not have: %s\n", Hex(d.p.data(), d.p.size()).c_str());
            CHECK(false);
        }
    }
    std::printf("   the host sent: announcement %u/%u/%u/%u/%u frames, Auth %u, StartDistribution %u, Finish %u, final %u, notice %u\n", n1[0], n1[1], n1[2],
                n1[3], n1[4], a1, a3, a5, a7, a8);
    for (u32 i = 0; i < 5; ++i) {
        CHECK(n1[i] >= g.clients);
        CHECK_EQ(n1[i], n1[0]);
    }
    CHECK(a1 >= g.clients);
    CHECK_EQ(a3, g.clients);
    CHECK(a5 >= g.clients);
    CHECK_EQ(a7, g.clients);
    const RealDlpNet::Counters& c = h.net.counters();
    CHECK_EQ(c.bad, 0u);
    CHECK_EQ(c.content, 0u);
    CHECK_EQ(h.lobby.ServerFinalize(), 0u);
    CHECK(!h.net.Holds());
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// B. A console searches: an Azahar host of the game, an Azahar client already in.

static void B_Search(const DlpRealLobbyGolden& g) {
    std::printf("B: a console searches and joins an Azahar host of %s next to an Azahar client\n", g.name);
    g_rules.Reset();
    MockRoom room;
    const u8* nseed = nullptr;
    const DlpRealGolden& notice = NoticeOf(g, "A8", &nseed);
    const AzaharDlpWire wire_az = {g.announce, &Need(g, "A1"), &Need(g, "A3"), &Need(g, "A5"), &Need(g, "A7"), &notice, g.seed};
    AzaharDlpHost hz(room, 1, "Player", g.child, 3, 7, wire_az);
    hz.game_wlan_comm_id = g.game_net;
    hz.game_beacon_after_ms = 170;
    Box c(room, 2, "Guest", g.low);
    Tap wire(c.con);
    AzaharDlpClient m(room, 0x12, "Az2");
    m.leave_at_final = g.final_to_bye_ms >= 0 && g.final_to_bye_ms < 100;
    Ticker ticker({&hz.e(), &c.e(), &m.e()});
    const Boxes bs = {&c};
    hz.StartHosting(!g.auto_accept, 11);
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(bs, m, hz.mac(), &pr));  // node 2
    if (!g.auto_accept) {
        CHECK_EQ(hz.AcceptClient(2), 0);
    }
    CHECK_EQ(pr.child_tid, g.child);
    CHECK_EQ(pr.max_nodes, 4u);
    CHECK(FindHost(bs, c, hz.mac(), g.child));
    CHECK_EQ(c.Status(), 2u);
    CHECK_EQ(c.TitleInOrder(true), 0u);
    CHECK_EQ(LoadLE32(c.Title() + kTitleInfoUniqueId), g.low >> 8);  // the unique id the game wants, from the child of the announcement
    CHECK(std::memcmp(c.Title() + kTitleInfoMac, hz.mac().data(), 6) == 0);
    CHECK_EQ(c.TitleInOrder(false), kNoData);
    CHECK_EQ(c.ServerInfo(hz.mac()), 0u);
    CHECK_EQ(c.Server()[kServerInfoMaxClients], 4u);
    CHECK_EQ(c.Server()[kServerInfoJoined], 2u);  // the host and its client
    CHECK(std::memcmp(c.Server() + kServerInfoMac, hz.mac().data(), 6) == 0);
    CHECK_EQ(JoinHost(bs, c, hz.mac()), 0u);
    CHECK_EQ(c.Status(), 5u);
    CHECK_EQ(NodeOf(c.e()), 3u);
    if (!g.auto_accept) {
        CHECK_EQ(hz.AcceptClient(3), 0);
    }
    CHECK(PumpUntil(bs, 2000, [&] { return c.Nodes() == "3:1,2,3"; }));
    CHECK_EQ(c.Nodes(), std::string("3:1,2,3"));
    CHECK(PumpUntil(bs, 2000, [&] { return hz.client_state(3) == AzaharDlpHost::Accepted; }));
    std::atomic<bool> stop{false};
    std::thread game([&] {
        hz.StartDistribution();
        while (!stop && hz.state() != AzaharDlpHost::kNeedToSendPassphrase) {
            Sleep(2);
        }
        while (!stop && hz.state() == AzaharDlpHost::kNeedToSendPassphrase && !hz.ended()) {
            hz.BeginGame(g.pass);
            Sleep(33);
        }
    });
    CHECK(PumpUntil(bs, 5000, [&] { return c.Status() == 9; }));
    stop = true;
    game.join();
    CHECK_EQ(c.Status(), 9u);
    u8 pass[9] = {};
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, g.pass, 9) == 0);
    CHECK_EQ(c.Nodes(), std::string("3:1,2,3"));
    CHECK_EQ(hz.sent("A3"), 2u);
    CHECK_EQ(hz.sent("A7"), 2u);
    // every answer the console sent is the capture's answer of its kind, byte for byte once sealed with the capture's seed
    u32 a2 = 0, a4 = 0, a6 = 0, a9 = 0;
    for (const Tap::Out& o : wire.All()) {
        Tap::Dlp d;
        if (!Tap::AsDlp(o, &d)) {
            continue;
        }
        CHECK_EQ(d.channel, 2u);
        CHECK_EQ(d.dest, 1u);
        const dlp_real::Packet p = dlp_real::ParseFromClient(d.p.data(), d.p.size());
        if (p.kind == dlp_real::Kind::AuthAck) {
            ++a2;
            CHECK(Same(d.p, Need(g, "A2"), g.seed));
        } else if (p.kind == dlp_real::Kind::DistAck) {
            ++a4;
            CHECK(Same(d.p, Need(g, "A4"), g.seed));
        } else if (p.kind == dlp_real::Kind::FinishAck) {
            ++a6;
            std::vector<u8> want = Bytes(Need(g, "A6"));
            std::memset(want.data() + 0x16, 0, 4);  // (ours zero: the member Azahar's fake client never initialises)
            dlp_real::Seal(g.seed, want.data(), want.size());
            std::vector<u8> mine = d.p;
            dlp_real::Seal(g.seed, mine.data(), mine.size());
            CHECK(mine == want);
        } else if (p.kind == dlp_real::Kind::NoticeAck) {
            ++a9;
            const u8* seed = nullptr;
            const DlpRealGolden& want = NoticeOf(g, "A9", &seed);
            std::vector<u8> mine = d.p;
            if (mine.size() == want.size) {
                mine[0x0D] = want.bytes[0x0D];  // (an Azahar host counts the notice's sequence on at every answer)
            }
            CHECK(Same(mine, want, seed));
        } else {
            CHECK(false);
        }
    }
    std::printf("   the console answered: AuthAck %u, the ack of StartDistribution %u, the Finish ack %u, the notice's %u\n", a2, a4, a6, a9);
    CHECK(a2 >= 1);
    CHECK_EQ(a4, 1u);
    CHECK(a6 >= 1);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK(!c.net.Holds());
    CHECK(!c.e().initialized());
    for (const std::string& v : hz.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(hz.violations().empty());
    CHECK_EQ(hz.size_skipped.load(), 0u);
    CheckModel(m);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// C. A console hosts four; the last client leaves before the start.

static void C_OneLeaves(const DlpRealLobbyGolden& g) {
    std::printf("C: %s, four nodes, a client leaves before the start: the host and the others know\n", g.name);
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Host", g.low);
    Box c(room, 2, "Guest", g.low);
    Tap wire(h.con);
    AzaharDlpClient m2(room, 0x12, "Az2"), m4(room, 0x14, "Az4"), m5(room, 0x15, "Az5");
    Ticker ticker({&h.e(), &c.e(), &m2.e(), &m4.e(), &m5.e()});
    const Boxes bs = {&h, &c};
    CHECK_EQ(h.lobby.ServerInitialize(3, 0, h.name16), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(!g.auto_accept, 0), 0u);
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(bs, m2, h.mac(), &pr));
    Accept(bs, h, g, 2);
    CHECK(FindHost(bs, c, h.mac(), g.child));
    CHECK_EQ(c.ServerInfo(h.mac()), 0u);
    CHECK_EQ(c.Server()[kServerInfoMaxClients], 4u);
    CHECK_EQ(c.Server()[kServerInfoJoined], 2u);
    CHECK_EQ(JoinHost(bs, c, h.mac()), 0u);
    Accept(bs, h, g, 3);
    CHECK(ModelJoin(bs, m4, h.mac(), &pr));
    Accept(bs, h, g, 4);
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "3:2,3,4" && c.Nodes() == "4:1,2,3,4" && MaskOf(m2.e()) == 0x0F; }));
    CHECK_EQ(h.Clients(), std::string("3:2,3,4"));
    CHECK_EQ(c.Nodes(), std::string("4:1,2,3,4"));
    CHECK_EQ(MaskOf(m2.e()), 0x0Fu);
    CHECK_EQ(MaskOf(m4.e()), 0x0Fu);
    // node 4 leaves: ONE Deauthentication (an Azahar client's goodbye)
    const std::size_t mark = wire.Mark();
    m4.StopSession();
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "2:2,3" && c.Nodes() == "3:1,2,3" && MaskOf(m2.e()) == 0x07; }));
    CHECK_EQ(h.Clients(), std::string("2:2,3"));
    CHECK_EQ(c.Nodes(), std::string("3:1,2,3"));
    CHECK_EQ(MaskOf(m2.e()), 0x07u);
    CHECK_EQ(h.net.PeerState(4), RealDlpNet::kFree);
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kAuthed);
    CHECK_EQ(h.net.PeerState(3), RealDlpNet::kAuthed);
    {
        // how they were told: ONE Logoff to each of the two that remain, each naming its own node, the list without node 4 (design D17)
        PumpFor(bs, 200);
        u32 logoffs = 0, maps = 0, seen = 0;
        const std::vector<Tap::Out> out = wire.All();
        for (std::size_t k = mark; k < out.size(); ++k) {
            EapolLogoff lo;
            maps += out[k].type == WifiPacket::Type::NodeMap;
            if (!Tap::AsLogoff(out[k], &lo)) {
                continue;
            }
            ++logoffs;
            CHECK(out[k].dest != kBroadcastMac);
            seen |= 1u << lo.assigned_node_id;
            CHECK_EQ(lo.connected_nodes, 3u);
            CHECK_EQ(lo.max_nodes, 4u);
        }
        CHECK_EQ(logoffs, 2u);
        CHECK_EQ(seen, 0x0Cu);
        CHECK_EQ(maps, 0u);
    }
    // the next one takes the place: node 4, a dialogue from the Auth
    CHECK(ModelJoin(bs, m5, h.mac(), &pr));
    CHECK_EQ(NodeOf(m5.e()), 4u);
    Accept(bs, h, g, 4);
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "3:2,3,4" && c.Nodes() == "4:1,2,3,4"; }));
    {
        NodeEntry en;
        CHECK_EQ(c.lobby.ClientNodeInfo(4, &en), 0u);
        CHECK_EQ(Str(en.name), std::string("Az5"));
    }
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(bs, 1500, [&] { return h.lobby.ServerState() == 8; }));
    CHECK(PumpUntil(bs, 1000, [&] { return c.Status() == 6; }));
    CHECK_EQ(h.lobby.ServerSendPassphrase(g.pass), 0u);
    CHECK(PumpUntil(bs, 2500, [&] { return h.lobby.ServerState() == 9 && c.Status() == 9; }));
    u8 pass[9] = {};
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, g.pass, 9) == 0);
    for (AzaharDlpClient* m : {&m2, &m5}) {
        CHECK(WaitFor(1000, [&] { return m->state() == AzaharDlpClient::kComplete; }));
        CheckModel(*m);
    }
    CHECK_EQ(m4.state(), AzaharDlpClient::kJoined);  // the one that left never heard of the start
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK_EQ(h.lobby.ServerFinalize(), 0u);
    CHECK(!c.net.Holds() && !h.net.Holds());
    CHECK_EQ(h.net.counters().bad, 0u);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// F. Kirby: the hub on the adapter, the network thread gone - the commands that end a role answer success.

static u32 HubCall(DlpHub& hub, DlpKind kind, u32 header, std::initializer_list<u32> args) {
    u32 buf[64] = {};
    buf[0] = header;
    u32 i = 1;
    for (u32 a : args) {
        buf[i++] = a;
    }
    const DlpOutcome out = hub.Serve(kind, buf, nullptr);
    CHECK(out != DlpOutcome::JoinPending);
    return buf[1];
}

static void F_NetworkGone(const DlpRealLobbyGolden& g) {
    std::printf("F: %s, the hub on the real Download Play with the network thread gone: Finalize and StopSession answer 0\n", g.name);
    for (int role = 0; role < 2; ++role) {
        g_rules.Reset();
        MockRoom room;
        Console con(room, 1);
        RealDlpNet real(con.engine.get());
        real.SetIdentity(Me(0xF001, "Host"));
        real.SetPlayers(4);
        GameMem mem;
        LobbyConfig cfg = Player::Cfg(g.low, 1, "Host", 4);
        DlpHub hub(con.engine.get(), &con.fake_kernel, cfg, mem.Resolver(), &real);
        std::atomic<bool> stop{false};
        std::thread net([&] {
            while (!stop) {
                con.engine->Tick(platform::NowMs());
                hub.Tick();
                Sleep(3);
            }
        });
        Sleep(20);
        u16 name[kNameUnits];
        Put(name, "Host");
        const u32 w0 = name[0] | (name[1] << 16), w1 = name[2] | (name[3] << 16);
        if (role == 0) {
            // a creator that accepts: InitializeWithName for three clients, StartAccepting
            CHECK_EQ(HubCall(hub, DlpKind::Srvr, 0x000F0303, {0x85000, 3, 0xE, 0, 0x40000, 2, w0, w1, 0, 0, 0, 0, 0x04000000, 0x7777, 0x8888}), 0u);
            CHECK_EQ(HubCall(hub, DlpKind::Srvr, 0x00050080, {0, 6}), 0u);
        } else {
            // a searcher that scans
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00110203, {0x30000, 2, w0, w1, 0, 0, 0, 0, 0x04000000, 0x3333, 0x4444}), 0u);
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00050180, {0x0421, static_cast<u32>(g.child), static_cast<u32>(g.child >> 32), 0, 0, 0}), 0u);
        }
        CHECK(WaitFor(2000, [&] { return hub.EngineHeld() || role == 1; }));
        stop = true;
        net.join();
        Sleep(DlpHub::kAliveMs + 150);
        // a command that needs the network is refused now (the gate is live) ...
        CHECK(HubCall(hub, DlpKind::Fkcl, 0x00050180, {0x0421, static_cast<u32>(g.child), static_cast<u32>(g.child >> 32), 0, 0, 0}) != 0u);
        // ... and the ones that end a role succeed, twice
        if (role == 0) {
            CHECK_EQ(HubCall(hub, DlpKind::Srvr, 0x00060000, {}), 0u);  // EndAccepting
            CHECK_EQ(HubCall(hub, DlpKind::Srvr, 0x00020000, {}), 0u);  // Finalize
            CHECK_EQ(HubCall(hub, DlpKind::Srvr, 0x00020000, {}), 0u);
        } else {
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00060000, {}), 0u);  // StopScan
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00100000, {}), 0u);  // StopSession
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00020000, {}), 0u);  // Finalize
            CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00020000, {}), 0u);
        }
        CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00100000, {}), 0u);
        CHECK_EQ(HubCall(hub, DlpKind::Fkcl, 0x00020000, {}), 0u);
        // the game's own nwm::UDS takes the engine back on its own thread
        hub.OnGameUdsStarting();
        CHECK(!con.engine->initialized());
        CHECK(!hub.EngineHeld());
        CHECK(!real.Holds());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// S. The console's seed (the rows that have it): the one set on the lobby is the one every node list gives.

static void S_ConsoleSeed(const DlpRealLobbyGolden& g) {
    std::printf("S: %s, the console's own friend code seed in the real Download Play's network\n", g.name);
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Host", g.low);
    Box c(room, 2, "Guest", g.low);
    static const u64 kHostSeed = 0x0123456789ABCDEFull, kGuestSeed = 0x1122334455667788ull;
    h.lobby.SetFriendSeed(kHostSeed);  // (what the plugin does with the config block 0x00090000 for a row with console_seed)
    c.lobby.SetFriendSeed(kGuestSeed);
    Ticker ticker({&h.e(), &c.e()});
    const Boxes bs = {&h, &c};
    CHECK_EQ(h.lobby.ServerInitialize(3, 0, h.name16), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(!g.auto_accept, 0), 0u);
    CHECK(FindHost(bs, c, h.mac(), g.child));
    CHECK_EQ(JoinHost(bs, c, h.mac()), 0u);
    Accept(bs, h, g, 2);
    NodeEntry en;
    CHECK(PumpUntil(bs, 2000, [&] { return h.lobby.ServerClientInfo(2, &en) == 0; }));
    CHECK_EQ(en.seed, kGuestSeed);  // the creator's GetClientInfo
    CHECK_EQ(c.lobby.ClientNodeInfo(1, &en), 0u);
    CHECK_EQ(en.seed, kHostSeed);  // the searcher's GetNodeInfo of the host
    CHECK_EQ(c.lobby.ClientNodeInfo(2, &en), 0u);
    CHECK_EQ(en.seed, kGuestSeed);  // ... and of itself
    std::printf("   known limit: an Azahar's seed in the Download Play's network is zero (every capture so far): a game that compares it with the one of "
                "its own network does not match an Azahar player\n");
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK_EQ(h.lobby.ServerFinalize(), 0u);
    CHECK_RULES();
}

int main() {
    std::printf("dlp_real_lobby_games_test: D6, the games whose players all have the game, on the real Download Play against their own captures\n");
    u32 ran = 0, skipped = 0;
    for (const LobbyGame& game : kGames) {
        const title_rules::Dlp dlp = title_rules::DlpOf(kApp | game.low);
        // a row of these games is Lobby or RealDlp, four players; RealDlp ONLY with its golden (its capture)
        CHECK(dlp.mode == DlpGameMode::Lobby || dlp.mode == DlpGameMode::RealDlp);
        CHECK_EQ(dlp.max_players, 4u);
        if (game.golden == nullptr) {
            CHECK(dlp.mode != DlpGameMode::RealDlp);
            std::printf("SKIPPED %s (row %s): %s\n", game.name, dlp.mode == DlpGameMode::RealDlp ? "RealDlp" : "Lobby", game.without);
            ++skipped;
            continue;
        }
        const DlpRealLobbyGolden& g = *game.golden;
        CHECK_EQ(g.low, game.low);
        CHECK(std::strcmp(g.name, game.name) == 0);
        std::printf("== %s: golden of its capture, row %s\n", game.name, dlp.mode == DlpGameMode::RealDlp ? "RealDlp" : "Lobby (the owner's decision)");
        G_Capture(g);
        A_Host(g);
        B_Search(g);
        C_OneLeaves(g);
        if (game.kirby) {
            F_NetworkGone(g);
        }
        if (dlp.console_seed) {
            S_ConsoleSeed(g);
        }
        ++ran;
    }
    std::printf("%u game(s) held to their capture, %u SKIPPED (no capture that played)\n", ran, skipped);
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
    (void)&MakeBeacon;
    (void)&Ann;
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
