// MARIO KART 7 on the REAL Download Play with EIGHT players (2026-11-13): our DlpLobby on the adapter RealDlpNet (azl/dlp_real_net.hpp) with
// the ceiling of the game's Download Play column in azl/title_rules.hpp (eight: host + 7 clients), held to the capture of eight Azahar emulators that played the game the
// same day (analysis/mk7-azahar-2026-11-13; its bytes are Tests/dlp_real_mk7_golden.inc, cut out by Tests/make_dlp_real_mk7_goldens.py). The rig
// is the one of Tests/dlp_real_net_test.cpp and dlp_real_client_test.cpp: engines joined by the in-memory room of Tests/mock_room.hpp, real time,
// the Azahar-peer rule checker on every frame our engines send, and the models of Azahar's Download Play client and host
// (Tests/azahar_dlp_peer.hpp, Tests/azahar_dlp_host.hpp: Azahar's code transcribed) with the game's behaviour of the capture: the acceptance
// is automatic, the host's game asks for seven clients, and a client leaves the session as soon as it has the final with the passphrase.
//
//   G  the capture against the codec and the engine, no network: the seventy packets of the capture are what our builders make with the
//      capture's seed (the announcement's sizes, title and icon aside: ours are zero, design 2.2 option c; the sequence acknowledged in a
//      Finish ack, which Azahar never initialises); the recorded beacon is filed as a lobby of the real Download Play of 8 nodes (and is
//      nothing for the dialect); the recorded node map and Logoff of the eight nodes are read whole
//   A  a console HOSTS with SEVEN Azahar clients: the announcement to each as a spectator, the seven in one at a time, GetConnectingClients /
//      GetClientInfo after each, everybody's node list, StartDistribution, the passphrase, the seven gone at the final, the lobby 9; EVERY
//      frame the host sent is compared with the capture's form; from StartDistribution on the host sends NO Logoff (the seven that leave at
//      the final are told of nobody: the refresh of a departure is for a lobby that still admits clients)
//   B a console SEARCHES and finds an Azahar host of 8 nodes: GetTitleInfoInOrder (unique id 0x307), GetServerInfo (max 8, the tick at +0x290
//      never older than 5 s, also after 5 s of scan), joins as a fake client among Azahar clients up to eight nodes, a client leaves (Azahar's
//      host says nothing: the list changes at the next admission), START, the passphrase of the capture, the host gone without a goodbye
//   C  a console hosts, Azahar clients AND a console as clients, one (node 5) leaves before the start: the host and every client that remains
//      know at once (the engine's refresh for the real Download Play's network, design D17: one Logoff to each, none to the spectator that is
//      in at that moment), the next client takes the place and its dialogue starts from the Auth, START and GO for those who are there
//   D  Luigi's Mansion as before: an adapter nobody told has the ceiling of four (refuses a network of 5..8, three dialogues), its host with ONE
//      client sends the bytes of ITS capture, and a client that leaves it puts no frame on the wire, not even to the spectator that is in
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_real_mk7_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o <the core sources of Tests/run.sh>
#include "dlp_net_rig.hpp"

#include "azahar_dlp_host.hpp"
#include "azahar_dlp_peer.hpp"
#include "azl/dlp_ipc.hpp"
#include "azl/dlp_real_net.hpp"
#include "dlp_real_mk7_golden.inc"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static constexpr u32 kMk7Low = 0x00030700;                 // Mario Kart 7, Europe
static constexpr u64 kMk7Child = 0x0004000100030700ULL;    // what its searcher filters by (console log, dlp:FKCL 0x05) and its host announces
static constexpr u32 kMk7GameNet = 0x00030710;             // the game's own network after the Download Play (the capture, t=1035.3)
static constexpr u32 kLuigiLow = 0x001D1A00;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;
static constexpr u64 kFiveSecondsOfTicks = 5000ull * AZL_TICKS_PER_MS;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static const DlpRealGolden& Mk7(const char* id, unsigned node = 0) {
    for (const DlpRealGolden& g : kDlpRealMk7Dialog) {
        if (std::strcmp(g.id, id) == 0 && (node == 0 || g.src_node == node || g.dst_node == node)) {
            return g;
        }
    }
    std::fprintf(stderr, "no golden %s of node %u\n", id, node);
    std::abort();
}
static const DlpRealGolden& Luigi(const char* id) {
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        if (std::strcmp(g.id, id) == 0) {
            return g;
        }
    }
    std::fprintf(stderr, "no golden %s\n", id);
    std::abort();
}
static const DlpRealMk7Frame& Raw(const char* id) {
    for (const DlpRealMk7Frame& f : kDlpRealMk7Raw) {
        if (std::strcmp(f.id, id) == 0) {
            return f;
        }
    }
    std::fprintf(stderr, "no frame %s\n", id);
    std::abort();
}
/// The bytes Azahar's host of Mario Kart 7 sends: the capture's announcement and packets. (No notice in that capture: Luigi's, sealed again.)
static const AzaharDlpWire kMk7Wire = {kDlpRealMk7Announce, &Mk7("A1"), &Mk7("A3"), &Mk7("A5"), &Mk7("A7"), &Luigi("A8"), kDlpRealMk7Seed};

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

// ---------------------------------------------------------------------------------------------------------------------
// A console of Mario Kart 7 (or of another title): the adapter told the ceiling of the row, the lobby on it, and the game's reads through the
// IPC words (the records a game gets, not the lobby's tables).

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
    std::vector<Region> regions_;
    u32 next_ = 0x08000000;
};

struct Box {
    Box(MockRoom& room, u8 last, const char* name, u32 tid_low = kMk7Low, u8 players = 8)
        : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(tid_low, last, name, players)) {
        net.SetIdentity(Me(0xF000u + last, name));
        net.SetPlayers(players);
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
    /// The network thread's turn: the adapter first, then the lobby (as the hub does it).
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
    /// dlp:SRVR GetConnectingClients as Mario Kart 7 asks it (a vector of 15, console log): "total:ids".
    std::string Clients() {
        u32 cmd[16] = {0x000B0042, 15, (30u << 4) | 0xC, a_ids};
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
    /// dlp:FKCL GetTitleInfoInOrder(restart): the result; the record is at Title().
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
    /// dlp:FKCL GetServerInfo(mac): the result; the record is at Server().
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
/// Runs `f` (a call of a model that blocks) while the consoles are pumped.
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

/// The network tag of a beacon (dd 34 00 1f 32 15 ...: OUI, type 21, then wlancommid BE32 at +4 of the OUI, id8 +8, total +0x10, max +0x11,
/// the application data's size +0x33): a pointer to the OUI, or null. `len` = the tag's length byte.
static const u8* NetTag(const std::vector<u8>& beacon, u8* len) {
    for (std::size_t p = 12; p + 2 <= beacon.size(); p += 2u + beacon[p + 1]) {
        if (beacon[p] == 0xDD && beacon[p + 1] >= 0x34 && p + 2u + beacon[p + 1] <= beacon.size() && beacon[p + 2] == 0x00 && beacon[p + 3] == 0x1F &&
            beacon[p + 4] == 0x32 && beacon[p + 5] == 21) {
            *len = beacon[p + 1];
            return &beacon[p + 2];
        }
    }
    return nullptr;
}

static u16 NodeOf(UdsEngine& e, u16* mask = nullptr) {
    u16 node = 0, m = 0;
    if (!e.NodeSnapshot(&node, &m)) {
        node = 0;
        m = 0;
    }
    if (mask != nullptr) {
        *mask = m;
    }
    return node;
}
static u16 MaskOf(UdsEngine& e) {
    u16 m = 0;
    NodeOf(e, &m);
    return m;
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

/// The console's searcher as Mario Kart 7 drives it (console log): InitializeWithName for 4 lobbies, StartScan with the child's title id, then
/// GetTitleInfoInOrder until the host is there.
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
// G. The capture against the codec and the engine.

static void G_Capture() {
    std::printf("G: the capture of eight Azahar against the codec and the engine\n");
    CHECK_EQ(kDlpRealMk7Child, kMk7Child);
    CHECK_EQ(kDlpRealMk7MaxNodes, 8u);
    // the announcement: ours with the capture's seed is the capture's, the sizes (+0x38..0x3F) and the title and icon (redacted) aside
    u8 buf[dlp_real::kBufferBytes];
    static const std::size_t kLen[5] = {768, 1448, 1448, 1448, 1464};
    for (unsigned i = 0; i < 5; ++i) {
        const DlpRealGolden& g = kDlpRealMk7Announce[i];
        CHECK_EQ(g.size, kLen[i]);
        CHECK_EQ(dlp_real::AnnounceFrame(buf, i, kMk7Child, 8, kDlpRealMk7Seed), g.size);
        const dlp_real::Packet p = dlp_real::ParseAnnounce(g.bytes, g.size);
        CHECK(p.kind == dlp_real::Kind::Announce);
        CHECK_EQ(p.index, i);
        CHECK(std::memcmp(buf, g.bytes, 8) == 0);            // type, length, 02 00
        CHECK(std::memcmp(buf + 12, g.bytes + 12, 4) == 0);  // the index, 05 00 00
        if (i == 0) {
            CHECK(std::memcmp(buf + 0x10, g.bytes + 0x10, 0x28) == 0);    // the child's title id, 00 10, 01 01, 00 03 ff c0, max_nodes 8, zeros
            CHECK(std::memcmp(buf + 0x2F8, g.bytes + 0x2F8, 8) == 0);     // the tail
            CHECK_EQ(dlp_real::AnnounceChildTid(g.bytes), kMk7Child);
            CHECK_EQ(dlp_real::AnnounceMaxNodes(g.bytes), 8u);
            bool zero = true;
            for (std::size_t k = 0x38; k < 0x2F8; ++k) {
                zero &= buf[k] == 0;
            }
            CHECK(zero);  // (declared: the sizes, the title and the icon are zero in ours - design 2.2, option c)
        } else if (i == 4) {
            CHECK(std::memcmp(buf, g.bytes, g.size) == 0);  // frame 4 is zeros in Azahar's too: the same bytes, checksum included
        }
    }
    // the dialogue: every packet of the capture is the one our builder makes for that node's sequence and id, with the capture's seed
    u32 kinds[11] = {};
    for (const DlpRealGolden& g : kDlpRealMk7Dialog) {
        const bool from_host = g.src_node == 1;
        const dlp_real::Packet p = from_host ? dlp_real::ParseFromHost(g.bytes, g.size) : dlp_real::ParseFromClient(g.bytes, g.size);
        CHECK(p.kind != dlp_real::Kind::None);
        ++kinds[static_cast<u8>(p.kind)];
        std::size_t n = 0;
        const u8 rid[2] = {g.bytes[0x0E], g.bytes[0x0F]};
        const u8 seq = g.bytes[0x0D];
        std::vector<u8> want = Bytes(g);
        if (std::strcmp(g.id, "A1") == 0) {
            n = dlp_real::Auth(buf, kDlpRealMk7Seed);
        } else if (std::strcmp(g.id, "A3") == 0) {
            CHECK_EQ(seq, 1u);  // (what our host sends: 1, 2, 3)
            n = dlp_real::StartDist(buf, kDlpRealMk7Seed, seq, rid);
        } else if (std::strcmp(g.id, "A5") == 0) {
            CHECK_EQ(seq, 2u);
            n = dlp_real::Finish(buf, kDlpRealMk7Seed, seq, rid);
        } else if (std::strcmp(g.id, "A7") == 0) {
            CHECK_EQ(seq, 3u);
            CHECK(std::memcmp(g.bytes + dlp_real::kPassphraseAt, kDlpRealMk7Pass, 9) == 0);
            n = dlp_real::Final(buf, kDlpRealMk7Seed, seq, rid, kDlpRealMk7Pass);
        } else {  // a client's answer: ours to the host's packet it answers
            const char* to = std::strcmp(g.id, "A2") == 0 ? "A1" : std::strcmp(g.id, "A4") == 0 ? "A3" : "A5";
            const DlpRealGolden& asked = Mk7(to, g.src_node);
            const dlp_real::Packet q = dlp_real::ParseFromHost(asked.bytes, asked.size);
            n = dlp_real::Reply(buf, kDlpRealMk7Seed, q);
            if (std::strcmp(g.id, "A6") == 0) {
                // (declared: +0x16..0x19 is the block Azahar acknowledges, a member its fake client never initialises - 00 00 36 8a from node 2
                //  in this capture, zero from the others; the host does not read it. Ours is zero.)
                std::memset(want.data() + 0x16, 0, 4);
                dlp_real::Seal(kDlpRealMk7Seed, want.data(), want.size());
            }
            // (the answer to the Auth copies its 00 00 00 - the client's id 01 02 is in its body - and the others the host's sequence and that id)
            CHECK_EQ(rid[0], seq == 0 ? 0x00u : 0x01u);
            CHECK_EQ(rid[1], seq == 0 ? 0x00u : 0x02u);
        }
        if (n != want.size() || std::memcmp(buf, want.data(), n) != 0) {
            std::printf("   %s t=%s node %u -> %u differs: ours %s\n", g.id, g.time, g.src_node, g.dst_node, Hex(buf, n).c_str());
        }
        CHECK_EQ(n, want.size());
        CHECK(n == want.size() && std::memcmp(buf, want.data(), n) == 0);
    }
    CHECK_EQ(kinds[static_cast<u8>(dlp_real::Kind::Auth)], 15u);
    CHECK_EQ(kinds[static_cast<u8>(dlp_real::Kind::AuthAck)], 15u);
    for (dlp_real::Kind k : {dlp_real::Kind::StartDist, dlp_real::Kind::DistAck, dlp_real::Kind::Finish, dlp_real::Kind::FinishAck, dlp_real::Kind::Final}) {
        CHECK_EQ(kinds[static_cast<u8>(k)], 7u);  // one of each for each of the seven clients
    }
    CHECK_EQ(kinds[static_cast<u8>(dlp_real::Kind::Notice)], 0u);  // (the clients left 16 ms after the final: no notice in this capture)
    // the AuthAck's body carries the id every later packet of the host has: 01 02
    CHECK_EQ(Mk7("A2").bytes[0x16], 0x01u);
    CHECK_EQ(Mk7("A2").bytes[0x17], 0x02u);

    // the recorded beacon: a lobby of the real Download Play of 8 nodes for an engine of that kind, nothing for the dialect's
    {
        MockRoom room;
        Console real(room, 0x61), dialect(room, 0x62);
        CHECK(real.engine->PrepareLobby());
        CHECK(dialect.engine->PrepareLobby());
        real.engine->SetLobbyBeaconKind(UdsEngine::LobbyBeaconKind::RealDlp);
        const Mac az = {0x34, 0xAF, 0x2C, 0x00, 0x00, 0x11};
        for (const char* id : {"Beacon1", "Beacon8"}) {
            const DlpRealMk7Frame& f = Raw(id);
            WifiPacket b;
            b.type = WifiPacket::Type::Beacon;
            b.channel = static_cast<u8>(f.channel);
            b.transmitter_address = az;
            b.destination_address = kBroadcastMac;
            b.data.assign(f.bytes, f.bytes + f.size);
            real.engine->OnWifiPacket(b);
            dialect.engine->OnWifiPacket(b);
            UdsEngine::HeardLobby h;
            CHECK(real.engine->HeardLobbyAt(0, 1000, &h));
            CHECK(h.host == az);
            CHECK_EQ(h.max_nodes, 8u);
            CHECK_EQ(h.total_nodes, std::strcmp(id, "Beacon1") == 0 ? 1u : 8u);
            CHECK_EQ(h.size, 0u);
            CHECK_EQ(h.channel, 11u);
            CHECK(!real.engine->HeardLobbyAt(1, 1000, &h));
            CHECK(!dialect.engine->HeardLobbyAt(0, 1000, &h));
            u8 len = 0;
            const u8* tag = NetTag(b.data, &len);
            CHECK(tag != nullptr);
            if (tag != nullptr) {
                CHECK_EQ(len, 0x34u);  // (52 bytes: no application data, what Azahar's Download Play client needs)
                CHECK_EQ(LoadBE32(tag + 4), 0x00002810u);
                CHECK_EQ(tag[0x33], 0u);
            }
        }
    }
    // the node map and the Logoff of the seventh admission: the eight nodes, node 8 assigned
    {
        const DlpRealMk7Frame& nm = Raw("NodeMap");
        std::vector<NodeMapEntry> entries;
        CHECK(ParseNodeMapPacket(std::vector<u8>(nm.bytes, nm.bytes + nm.size), &entries));
        CHECK_EQ(entries.size(), 7u);
        u32 ids = 0;
        for (const NodeMapEntry& en : entries) {
            ids |= 1u << en.node_id;
        }
        CHECK_EQ(ids, 0x1FCu);  // nodes 2..8
        const DlpRealMk7Frame& lf = Raw("Logoff8");
        CHECK(lf.broadcast);
        EapolLogoff lo;
        CHECK(ParseEapolLogoffFrame(std::vector<u8>(lf.bytes, lf.bytes + lf.size), &lo));
        CHECK_EQ(lo.assigned_node_id, 8u);
        CHECK_EQ(lo.connected_nodes, 8u);
        CHECK_EQ(lo.max_nodes, 8u);
        for (u32 i = 0; i < 8; ++i) {
            CHECK_EQ(lo.nodes[i].network_node_id, i + 1);
            CHECK_EQ(lo.nodes[i].friend_code_seed, 0u);  // (Azahar's Download Play gives its local friend code seed: zero on these eight)
            CHECK_EQ(Str(lo.nodes[i].username), std::string("Player"));  // (the name the game gave InitializeWithName)
        }
        const DlpRealMk7Frame& st = Raw("Start");
        EapolStart start;
        CHECK(ParseEapolStartFrame(std::vector<u8>(st.bytes, st.bytes + st.size), &start));
        CHECK_EQ(start.connection_type, static_cast<u8>(ConnectionType::Client));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// A. A console hosts Mario Kart 7 with seven Azahar clients.

static void A_HostSeven() {
    std::printf("A: a console hosts Mario Kart 7, seven Azahar clients come in one at a time\n");
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Mario");
    Tap wire(h.con);
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines = {&h.e()};
    for (u8 i = 0; i < 7; ++i) {
        const std::string name = "Az" + std::to_string(i + 2);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        cl.back()->leave_at_final = true;  // (Mario Kart 7 on Azahar: gone 16 ms after the final)
        engines.push_back(&cl.back()->e());
    }
    Ticker ticker(engines);
    const Boxes bs = {&h};
    // the game (console log of 2026-11-13): dlp:SRVR InitializeWithName for SEVEN clients, StartHosting with automatic acceptance, channel word 0
    CHECK_EQ(h.lobby.ServerInitialize(7, 0, h.name16), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 0), 0u);
    CHECK(h.net.Holds());
    CHECK_EQ(h.net.players(), 8u);
    CHECK_EQ(h.e().GetConnectionStatus().max_nodes, 8u);  // "hosting network (max 8 nodes ...)", as the capture's beacon says
    CHECK_EQ(h.lobby.ServerState(), 2u);
    CHECK_EQ(h.Clients(), std::string("0:"));
    PumpFor(bs, 300);
    {
        // nobody is looking: not one frame of the announcement (design 4.3), and the beacon of the capture's form
        u32 beacons = 0;
        for (const Tap::Out& o : wire.All()) {
            Tap::Dlp d;
            CHECK(!Tap::AsDlp(o, &d));
            if (o.type != WifiPacket::Type::Beacon) {
                continue;
            }
            ++beacons;
            u8 len = 0, rlen = 0;
            const DlpRealMk7Frame& rf = Raw("Beacon1");
            const std::vector<u8> recorded(rf.bytes, rf.bytes + rf.size);
            const u8* tag = NetTag(o.data, &len);
            const u8* rec = NetTag(recorded, &rlen);
            CHECK(tag != nullptr && rec != nullptr);
            if (tag == nullptr || rec == nullptr) {
                continue;
            }
            // the capture's: the Download Play's wlancommid, 1 node of 8, no application data, a tag of 52 bytes. Declared, not compared: the id8
            // (ours 0x55; Azahar's is whatever was on its stack), the network id (ours the hash of the MAC; Azahar's the same garbage), the
            // bytes Azahar never initialises, the hash and the encrypted node data of the other tags.
            CHECK_EQ(len, rlen);
            CHECK_EQ(LoadBE32(tag + 4), LoadBE32(rec + 4));
            CHECK_EQ(tag[0x10], rec[0x10]);
            CHECK_EQ(tag[0x11], rec[0x11]);
            CHECK_EQ(tag[0x33], rec[0x33]);
        }
        CHECK(beacons >= 2);
    }
    u16 everybody = 1;
    std::string ids;
    for (u8 i = 0; i < 7; ++i) {
        AzaharDlpClient& m = *cl[i];
        const u16 node = static_cast<u16>(i + 2);
        const std::size_t mark = wire.Mark();
        AzaharDlpClient::Probe pr;
        CHECK(ModelJoin(bs, m, h.mac(), &pr));
        // what the spectator read: the five frames within Azahar's second, the game, eight nodes, the host's name (the game's)
        CHECK(pr.all);
        CHECK(pr.ms_to_all <= 1000);
        CHECK_EQ(pr.child_tid, kMk7Child);
        CHECK_EQ(pr.max_nodes, 8u);
        CHECK_EQ(Str(pr.name), std::string("Mario"));
        CHECK_EQ(NodeOf(m.e()), node);  // one at a time: the lowest free id, as the capture's 2, 3, ... 8
        everybody = static_cast<u16>(everybody | (1u << (node - 1)));
        ids += (ids.empty() ? "" : ",") + std::to_string(node);
        const std::string want = std::to_string(i + 1) + ":" + ids;
        CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == want && h.net.PeerState(static_cast<u8>(node)) == RealDlpNet::kAuthed; }));
        CHECK_EQ(h.Clients(), want);
        CHECK_EQ(h.net.PeerState(static_cast<u8>(node)), RealDlpNet::kAuthed);
        u32 word = 0;
        CHECK_EQ(h.lobby.ServerClientState(node, &word), 0u);
        CHECK_EQ(word >> 24, 5u);  // accepted at once: the acceptance is automatic
        for (u16 k = 2; k <= 9; ++k) {
            NodeEntry en;
            const u32 r = h.lobby.ServerClientInfo(k, &en);
            CHECK_EQ(r, k <= node ? 0u : kNoData);
            if (r == 0) {
                CHECK_EQ(en.node, k);
                CHECK_EQ(Str(en.name), "Az" + std::to_string(k));
                CHECK_EQ(en.seed, 0xA0A0u + 0x12u + (k - 2u));
            }
        }
        // everybody knows everybody: the host's Logoff of this admission went to every member
        CHECK(PumpUntil(bs, 1000, [&] {
            for (u8 k = 0; k <= i; ++k) {
                if (MaskOf(cl[k]->e()) != everybody) {
                    return false;
                }
            }
            return true;
        }));
        CHECK_EQ(MaskOf(h.e()), everybody);
        // the frames of this admission: the node map (to everybody) and the Logoff of the node (to each member) in the capture's form
        u32 maps = 0, logoffs = 0, to_spectator = 0;
        const std::vector<Tap::Out> out = wire.All();
        for (std::size_t k = mark; k < out.size(); ++k) {
            const Tap::Out& o = out[k];
            EapolLogoff lo;
            if (o.type == WifiPacket::Type::NodeMap) {
                std::vector<NodeMapEntry> entries;
                CHECK(ParseNodeMapPacket(o.data, &entries));
                CHECK(o.dest == kBroadcastMac);
                CHECK_EQ(entries.size(), i + 1u);
                ++maps;
            } else if (Tap::AsLogoff(o, &lo) && lo.assigned_node_id == node) {
                CHECK_EQ(lo.connected_nodes, i + 2u);
                CHECK_EQ(lo.max_nodes, 8u);
                // (declared: Azahar BROADCASTS it - the capture's Logoff8 - and ours goes to each member by itself: design D16, an Azahar that
                //  is joining adopts any Logoff it hears)
                CHECK(o.dest != kBroadcastMac);
                ++logoffs;
            } else if (Tap::AsLogoff(o, &lo)) {
                // the only other Logoff of an admission: the spectator's own (node 0), to the spectator alone. Its goodbye tells nobody: a
                // Logoff that named a member here would be the refresh of a departure, which is for a CLIENT that left (design D17)
                CHECK_EQ(lo.assigned_node_id, 0u);
                CHECK(o.dest == m.mac());
                CHECK_EQ(lo.connected_nodes, i + 1u);
                ++to_spectator;
            }
        }
        CHECK_EQ(maps, 1u);
        CHECK_EQ(logoffs, i + 1u);
        CHECK_EQ(to_spectator, 1u);
        if (i == 6) {
            // the eighth node: ours next to the capture's Logoff8
            EapolLogoff rec;
            const DlpRealMk7Frame& lf = Raw("Logoff8");
            CHECK(ParseEapolLogoffFrame(std::vector<u8>(lf.bytes, lf.bytes + lf.size), &rec));
            for (std::size_t k = mark; k < out.size(); ++k) {
                EapolLogoff lo;
                if (!Tap::AsLogoff(out[k], &lo) || lo.assigned_node_id != 8) {
                    continue;
                }
                CHECK_EQ(out[k].data.size(), lf.size);
                CHECK_EQ(lo.connected_nodes, rec.connected_nodes);
                CHECK_EQ(lo.max_nodes, rec.max_nodes);
                for (u32 n = 0; n < kUdsMaxNodes; ++n) {
                    CHECK_EQ(lo.nodes[n].network_node_id, rec.nodes[n].network_node_id);  // 1..8, then nothing (names and seeds: identity)
                }
            }
        }
    }
    CHECK_EQ(h.Clients(), std::string("7:2,3,4,5,6,7,8"));
    CHECK_EQ(h.e().GetConnectionStatus().total_nodes, 8u);
    // a ninth: the network is full, nobody answers its spectator (and Azahar gives up after 3 s); the lobby is not disturbed
    {
        AzaharDlpClient ninth(room, 0x29, "Az9");
        CHECK(PumpUntil(bs, 3000, [&] { return ninth.Heard(h.mac()); }));
        AzaharDlpClient::Probe pr;
        While(bs, [&] { pr = ninth.ProbeHost(h.mac()); });
        CHECK(!pr.connected);
        CHECK_EQ(ninth.timeouts.load(), 1u);
        CHECK_EQ(h.Clients(), std::string("7:2,3,4,5,6,7,8"));
    }
    // StartDistribution: the announcement stops, the seven are told, each answers "no content"; the host is at 8
    const u64 t_start = platform::NowMs();
    const std::size_t mark_start = wire.Mark();
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(bs, 1500, [&] { return h.lobby.ServerState() == 8; }));
    const u32 start_ms = static_cast<u32>(platform::NowMs() - t_start);
    std::printf("   StartDistribution -> the seven ready (state 8): %u ms\n", start_ms);
    CHECK_EQ(h.Clients(), std::string("7:2,3,4,5,6,7,8"));  // nobody thrown out at the START grace
    for (u8 i = 0; i < 7; ++i) {
        CHECK_EQ(cl[i]->state(), AzaharDlpClient::kWaitingForServerReady);
        CHECK_EQ(h.net.PeerState(static_cast<u8>(i + 2)), RealDlpNet::kReady);
    }
    // the passphrase (the capture's), again and again as the game does; the FIRST is the one sent; every client has it and leaves at once
    const u64 t_go = platform::NowMs();
    CHECK_EQ(h.lobby.ServerSendPassphrase(kDlpRealMk7Pass), 0u);
    u8 other[9] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 0};
    h.lobby.ServerSendPassphrase(other);
    CHECK(PumpUntil(bs, 2500, [&] { return h.lobby.ServerState() == 9; }));
    const u32 go_ms = static_cast<u32>(platform::NowMs() - t_go);
    std::printf("   the passphrase -> the lobby complete (state 9): %u ms\n", go_ms);
    CHECK(go_ms < DlpLobby::kGoGraceMs);  // (the seven left at the final: nobody to wait for)
    CHECK(!h.net.Holds());
    CHECK(!h.e().initialized());
    CHECK_EQ(h.Clients(), std::string("0:"));
    for (u8 i = 0; i < 7; ++i) {
        AzaharDlpClient& m = *cl[i];
        CHECK(WaitFor(1000, [&] { return m.left.load(); }));
        CHECK_EQ(m.state(), AzaharDlpClient::kComplete);
        const std::vector<u8> pass = m.passphrase();
        CHECK(pass.size() == 9 && std::memcmp(pass.data(), kDlpRealMk7Pass, 9) == 0);
        CHECK_EQ(m.received(3, 1), 1u);  // ONE StartDistribution each (the answer came before the resend)
        CHECK_EQ(m.received(6, 1), 1u);  // ONE final each
        CheckModel(m);
    }
    // from StartDistribution on the host sends NO Logoff and no node map: the door is shut, and the seven that leave at the final are told of
    // nobody (the capture has no frame of the host there). The refresh of a departure (design D17) is for a lobby that is forming: at the
    // final it made 21 Logoffs (6+5+4+3+2+1) to consoles on their way to the game's own network, where an Azahar that is Connecting takes
    // any Logoff it hears as its admission
    {
        u32 late_logoffs = 0, late_maps = 0;
        const std::vector<Tap::Out> out = wire.All();
        for (std::size_t k = mark_start; k < out.size(); ++k) {
            EapolLogoff lo;
            late_logoffs += Tap::AsLogoff(out[k], &lo) ? 1u : 0u;
            late_maps += out[k].type == WifiPacket::Type::NodeMap ? 1u : 0u;
        }
        CHECK(out.size() > mark_start);
        CHECK_EQ(late_logoffs, 0u);
        CHECK_EQ(late_maps, 0u);
    }
    // EVERY frame the host sent, against the capture's form
    {
        u32 n1[5] = {}, a1 = 0, a3 = 0, a5 = 0, a7 = 0, a8 = 0, other_frames = 0;
        u8 buf[dlp_real::kBufferBytes];
        for (const Tap::Out& o : wire.All()) {
            Tap::Dlp d;
            if (!Tap::AsDlp(o, &d)) {
                // the engine's own frames: beacons, the answers to the Authentication, the node maps and Logoffs looked at above, its goodbyes
                other_frames += o.type == WifiPacket::Type::Data ? 0 : 1;
                continue;
            }
            if (d.channel == 1) {
                // the announcement: to everybody, the five frames of the capture's lengths; with the capture's seed the header, the title id,
                // the constants and max_nodes 8 are the capture's bytes (G), and frame 4 is the capture's whole
                CHECK_EQ(d.dest, 0xFFFFu);
                const dlp_real::Packet p = dlp_real::ParseAnnounce(d.p.data(), d.p.size());
                CHECK(p.kind == dlp_real::Kind::Announce);
                if (p.kind != dlp_real::Kind::Announce) {
                    continue;
                }
                ++n1[p.index];
                const DlpRealGolden& g = kDlpRealMk7Announce[p.index];
                CHECK_EQ(d.p.size(), g.size);
                const std::size_t n = dlp_real::AnnounceFrame(buf, p.index, kMk7Child, 8, dlp_real::kTxSeed);
                CHECK(n == d.p.size() && std::memcmp(buf, d.p.data(), n) == 0);
                if (p.index == 4) {
                    CHECK(Same(d.p, g, kDlpRealMk7Seed));
                } else if (p.index == 0) {
                    CHECK(std::memcmp(d.p.data() + 0x10, g.bytes + 0x10, 0x28) == 0);
                }
                continue;
            }
            // the dialogue: to one client 2..8, each packet the capture's packet of its kind to that node, byte for byte once sealed with the
            // capture's seed (declared: the checksum's seed is ours, 00 00 00 00 - nobody verifies it, design 2.5)
            CHECK_EQ(d.channel, 2u);
            CHECK(d.dest >= 2 && d.dest <= 8);
            const dlp_real::Packet p = dlp_real::ParseFromHost(d.p.data(), d.p.size());
            if (p.kind == dlp_real::Kind::Auth) {
                ++a1;
                CHECK(Same(d.p, Mk7("A1", d.dest), kDlpRealMk7Seed));
            } else if (p.kind == dlp_real::Kind::StartDist) {
                ++a3;
                CHECK(Same(d.p, Mk7("A3", d.dest), kDlpRealMk7Seed));
            } else if (p.kind == dlp_real::Kind::Finish) {
                ++a5;
                CHECK(Same(d.p, Mk7("A5", d.dest), kDlpRealMk7Seed));
            } else if (p.kind == dlp_real::Kind::Final) {
                ++a7;
                CHECK(Same(d.p, Mk7("A7", d.dest), kDlpRealMk7Seed));  // (the passphrase is the capture's: the test's game gave it)
            } else if (p.kind == dlp_real::Kind::Notice) {
                // (declared: not in this capture - Azahar's host sends it every 100 ms from the Finish ack on, and here the final came 32 ms
                //  after the last one - but in Luigi's, whose bytes these are; a client that is gone does not hear it)
                ++a8;
                CHECK(Same(d.p, Luigi("A8"), kDlpRealLuigiSeed));
            } else {
                std::printf("   a packet of the host that the capture does not have: %s\n", Hex(d.p.data(), d.p.size()).c_str());
                CHECK(false);
            }
        }
        std::printf("   the host sent: announcement %u/%u/%u/%u/%u frames, Auth %u, StartDistribution %u, Finish %u, final %u, notice %u\n", n1[0], n1[1],
                    n1[2], n1[3], n1[4], a1, a3, a5, a7, a8);
        for (u32 i = 0; i < 5; ++i) {
            CHECK(n1[i] >= 7);  // at least a group for each of the seven spectators
            CHECK_EQ(n1[i], n1[0]);
        }
        CHECK(a1 >= 7);
        CHECK_EQ(a3, 7u);
        CHECK(a5 >= 7);
        CHECK_EQ(a7, 7u);
        CHECK(other_frames > 0);
        const RealDlpNet::Counters& c = h.net.counters();
        CHECK_EQ(c.bad, 0u);
        CHECK_EQ(c.content, 0u);
        CHECK_EQ(c.dist, a3);
        CHECK_EQ(c.final, a7);
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// B. A console searches: an Azahar host of Mario Kart 7 with a network of 8 nodes, other Azahar clients around it.

static void B_SearchEight() {
    std::printf("B: a console searches and joins an Azahar host of Mario Kart 7 (8 nodes) among Azahar clients\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost hz(room, 1, "Player", kMk7Child, 7, 7, kMk7Wire);
    hz.game_wlan_comm_id = kMk7GameNet;
    hz.game_beacon_after_ms = 170;
    Box c(room, 2, "Luigi");
    Tap wire(c.con);
    std::vector<std::unique_ptr<AzaharDlpClient>> cl;
    std::vector<UdsEngine*> engines = {&hz.e(), &c.e()};
    for (u8 i = 0; i < 6; ++i) {
        const std::string name = "Az" + std::to_string(i);
        cl.emplace_back(new AzaharDlpClient(room, static_cast<u8>(0x12 + i), name.c_str()));
        cl.back()->leave_at_final = true;
        engines.push_back(&cl.back()->e());
    }
    Ticker ticker(engines);
    const Boxes bs = {&c};
    hz.StartHosting(false, 11);  // (Mario Kart 7: automatic acceptance - the Auth leaves at the next poll, as in the capture)
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(bs, *cl[0], hz.mac(), &pr));  // node 2
    CHECK(ModelJoin(bs, *cl[1], hz.mac(), &pr));  // node 3
    CHECK_EQ(pr.child_tid, kMk7Child);
    CHECK_EQ(pr.max_nodes, 8u);

    // the scan as the game does it: GetTitleInfoInOrder until a title is there, then GetServerInfo of its host
    const u64 t_scan = platform::NowMs();
    CHECK(FindHost(bs, c, hz.mac(), kMk7Child));
    std::printf("   StartScan -> the host listed: %u ms\n", static_cast<u32>(platform::NowMs() - t_scan));
    CHECK_EQ(c.Status(), 2u);
    CHECK_EQ(c.TitleInOrder(true), 0u);
    CHECK_EQ(LoadLE32(c.Title() + kTitleInfoUniqueId), 0x307u);  // the unique id the game wants, from the child's title id of the announcement
    CHECK_EQ(LoadLE32(c.Title() + kTitleInfoVariation), 0u);
    CHECK(std::memcmp(c.Title() + kTitleInfoMac, hz.mac().data(), 6) == 0);
    CHECK_EQ(c.TitleInOrder(false), kNoData);  // one host, one title
    auto server_age_ok = [&](u32* joined) {
        const u64 before = platform::NowTicks();
        if (c.ServerInfo(hz.mac()) != 0) {
            return false;
        }
        const u8* s = c.Server();
        const u64 tick = LoadLE64(s + kServerInfoTick);
        *joined = s[kServerInfoJoined];
        // +0x290: the game drops a host whose tick is more than 5 s behind svcGetSystemTick (Mario Kart 7, 0x295940); Azahar leaves it 0
        return tick != 0 && tick <= platform::NowTicks() && before - (tick < before ? tick : before) < kFiveSecondsOfTicks / 5 &&
               std::memcmp(s + kServerInfoMac, hz.mac().data(), 6) == 0 && s[kServerInfoMaxClients] == 8;
    };
    u32 joined = 0;
    CHECK(server_age_ok(&joined));
    CHECK_EQ(joined, 3u);  // the host and its two clients
    {
        u16 name[kNameUnits];
        for (u32 i = 0; i < kNameUnits; ++i) {
            name[i] = LoadLE16(c.Server() + kServerInfoHostName + 2 * i);
        }
        CHECK_EQ(Str(name), std::string("Player"));
    }
    // a third Azahar client comes in while the console still looks: the count follows, and after more than five seconds of scan the tick is
    // still fresh (the entry is heard again at every round: a tick taken once would have aged out and the game would drop the host)
    CHECK(ModelJoin(bs, *cl[2], hz.mac(), &pr));  // node 4
    CHECK(PumpUntil(bs, 2000, [&] { return server_age_ok(&joined) && joined == 4; }));
    while (platform::NowMs() - t_scan < 5600) {
        PumpFor(bs, 100);
        CHECK(server_age_ok(&joined));
    }
    CHECK_EQ(joined, 4u);
    CHECK_EQ(c.TitleInOrder(true), 0u);
    CHECK_EQ(c.net.counters().ignored_hosts, 0u);

    // the join: a fake client, node 5
    CHECK_EQ(JoinHost(bs, c, hz.mac()), 0u);
    CHECK_EQ(c.Status(), 5u);
    CHECK(PumpUntil(bs, 2000, [&] { return c.Nodes() == "5:1,2,3,4,5"; }));
    CHECK_EQ(c.Nodes(), std::string("5:1,2,3,4,5"));
    CHECK_EQ(NodeOf(c.e()), 5u);
    CHECK(PumpUntil(bs, 2000, [&] { return hz.client_state(5) == AzaharDlpHost::Accepted; }));  // the host has our AuthAck
    // three more: the network is full, eight nodes, and the console sees every one come in
    for (u8 i = 3; i < 6; ++i) {
        CHECK(ModelJoin(bs, *cl[i], hz.mac(), &pr));
    }
    CHECK(PumpUntil(bs, 2000, [&] { return c.Nodes() == "8:1,2,3,4,5,6,7,8"; }));
    CHECK_EQ(c.Nodes(), std::string("8:1,2,3,4,5,6,7,8"));
    {
        NodeEntry en;
        CHECK_EQ(c.lobby.ClientNodeInfo(8, &en), 0u);
        CHECK_EQ(Str(en.name), std::string("Az5"));
        CHECK_EQ(c.lobby.ClientNodeInfo(1, &en), 0u);
        CHECK_EQ(Str(en.name), std::string("Player"));
        CHECK_EQ(c.lobby.ClientNodeInfo(9, &en), kNoData);
    }
    // c) on an Azahar host: a client (node 3) leaves. Azahar's host tells nobody (nwm_uds.cpp HandleDeauthenticationFrame, "TODO broadcast new
    // connection_status"; the capture, t=1096.759: a Deauthentication and not one frame from the host): the console - as an Azahar client
    // there - still lists it, until the host's next admission, whose Logoff carries the list as it is
    cl[1]->StopSession();
    CHECK(WaitFor(2000, [&] { return hz.client_state(3) == AzaharDlpHost::NotJoined; }));
    PumpFor(bs, 400);
    CHECK_EQ(c.Nodes(), std::string("8:1,2,3,4,5,6,7,8"));
    {
        // the next admission: a searcher's spectator is enough (Azahar broadcasts that Logoff too)
        AzaharDlpClient seeker(room, 0x31, "Seeker");
        CHECK(PumpUntil(bs, 3000, [&] { return seeker.Heard(hz.mac()); }));
        While(bs, [&] { pr = seeker.ProbeHost(hz.mac()); });
        CHECK(pr.all);
        CHECK(PumpUntil(bs, 2000, [&] { return c.Nodes() == "7:1,2,4,5,6,7,8"; }));
        CHECK_EQ(c.Nodes(), std::string("7:1,2,4,5,6,7,8"));
        CHECK_EQ(NodeOf(c.e()), 5u);  // (a spectator's Logoff names node 0: the console keeps its own - design D16)
    }
    // ... and the one who left comes back into its place
    CHECK(ModelJoin(bs, *cl[1], hz.mac(), &pr));
    CHECK_EQ(NodeOf(cl[1]->e()), 3u);
    CHECK(PumpUntil(bs, 2000, [&] { return c.Nodes() == "8:1,2,3,4,5,6,7,8"; }));
    CHECK(PumpUntil(bs, 2000, [&] {
        for (u16 n = 2; n <= 8; ++n) {
            if (hz.client_state(n) != AzaharDlpHost::Accepted) {
                return false;
            }
        }
        return true;
    }));
    // the host starts: StartDistribution once to each, then the final with the capture's passphrase once to each; then it is gone, without a
    // Deauthentication, and 170 ms later the same MAC beacons the game's own network
    std::atomic<bool> stop{false};
    std::thread game([&] {
        hz.StartDistribution();
        while (!stop && hz.state() != AzaharDlpHost::kNeedToSendPassphrase) {
            Sleep(2);
        }
        while (!stop && hz.state() == AzaharDlpHost::kNeedToSendPassphrase && !hz.ended()) {
            hz.BeginGame(kDlpRealMk7Pass);
            Sleep(33);
        }
    });
    CHECK(PumpUntil(bs, 3000, [&] { return c.Status() == 6 || c.Status() == 9; }));  // 6 at the host's StartDistribution ("downloading") ...
    CHECK(PumpUntil(bs, 5000, [&] { return c.Status() == 9; }));                     // ... 9 at its final
    stop = true;
    game.join();
    CHECK_EQ(c.Status(), 9u);
    u8 pass[9] = {};
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, kDlpRealMk7Pass, 9) == 0);
    CHECK_EQ(c.Nodes(), std::string("8:1,2,3,4,5,6,7,8"));  // the roster of the final, kept while the others leave
    CHECK(WaitFor(2000, [&] { return hz.ended(); }));
    PumpFor(bs, 500);  // (the host's silence, its game beacon: the session is the game's to close)
    CHECK_EQ(c.Status(), 9u);
    CHECK_EQ(hz.sent("A3"), 7u);  // StartDistribution once to each of the seven that are in ...
    CHECK_EQ(hz.sent("A7"), 7u);  // ... and the final once to each
    // every answer the console sent is the capture's answer of its kind, byte for byte once sealed with the capture's seed
    {
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
                CHECK(Same(d.p, Mk7("A2", 5), kDlpRealMk7Seed));
            } else if (p.kind == dlp_real::Kind::DistAck) {
                ++a4;
                CHECK(Same(d.p, Mk7("A4", 5), kDlpRealMk7Seed));
            } else if (p.kind == dlp_real::Kind::FinishAck) {
                ++a6;
                CHECK(Same(d.p, Mk7("A6", 5), kDlpRealMk7Seed));  // (node 5's: +0x16..0x19 zero, as ours)
            } else if (p.kind == dlp_real::Kind::NoticeAck) {
                // (declared: the notice is not in this capture; the answer is Luigi's capture's, with the sequence of the notice it answers:
                //  an Azahar host counts on at every answer, 3, 4, ...)
                ++a9;
                CHECK(d.p.size() == 0x18 && d.p[0x0D] >= 3);
                if (d.p.size() == 0x18) {
                    d.p[0x0D] = 3;
                }
                CHECK(Same(d.p, Luigi("A9"), kDlpRealLuigiSeed));
            } else {
                CHECK(false);
            }
        }
        std::printf("   the console answered: AuthAck %u, the ack of StartDistribution %u, the Finish ack %u, the notice's %u\n", a2, a4, a6, a9);
        CHECK(a2 >= 1);
        CHECK_EQ(a4, 1u);
        CHECK(a6 >= 1);
    }
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK(!c.net.Holds());
    CHECK(!c.e().initialized());
    CHECK(!c.e().client_foreign_filter_enforced());
    for (const std::string& v : hz.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(hz.violations().empty());
    CHECK_EQ(hz.size_skipped.load(), 0u);
    for (auto& m : cl) {
        CheckModel(*m);
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// C. A console hosts; Azahar clients and a console are in; one leaves before the start.

static void C_OneLeaves() {
    std::printf("C: a client leaves the lobby of a console before the start: the host and the others know\n");
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Mario");
    Box c(room, 2, "Luigi");
    Tap wire(h.con);
    AzaharDlpClient m2(room, 0x12, "Az2"), m3(room, 0x13, "Az3"), m5(room, 0x15, "Az5"), m6(room, 0x16, "Az6");
    for (AzaharDlpClient* m : {&m2, &m3, &m5, &m6}) {
        m->leave_at_final = true;
    }
    Console watcher(room, 7);  // a searcher in the middle of its probe: a spectator that is in while the client leaves
    Ticker ticker({&h.e(), &c.e(), &m2.e(), &m3.e(), &m5.e(), &m6.e(), watcher.engine.get()});
    const Boxes bs = {&h, &c};
    CHECK_EQ(h.lobby.ServerInitialize(7, 0, h.name16), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(false, 0), 0u);
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(bs, m2, h.mac(), &pr));
    CHECK(ModelJoin(bs, m3, h.mac(), &pr));
    // the console: it finds the group of the other console (the tick fresh, eight places), joins: node 4
    CHECK(FindHost(bs, c, h.mac(), kMk7Child));
    CHECK_EQ(c.TitleInOrder(true), 0u);
    CHECK_EQ(LoadLE32(c.Title() + kTitleInfoUniqueId), 0x307u);
    CHECK_EQ(c.ServerInfo(h.mac()), 0u);
    CHECK_EQ(c.Server()[kServerInfoMaxClients], 8u);
    CHECK_EQ(c.Server()[kServerInfoJoined], 3u);
    CHECK(platform::NowTicks() - LoadLE64(c.Server() + kServerInfoTick) < kFiveSecondsOfTicks / 5);
    CHECK_EQ(JoinHost(bs, c, h.mac()), 0u);
    CHECK(ModelJoin(bs, m5, h.mac(), &pr));
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "4:2,3,4,5" && c.Nodes() == "5:1,2,3,4,5"; }));
    CHECK_EQ(h.Clients(), std::string("4:2,3,4,5"));
    CHECK_EQ(c.Nodes(), std::string("5:1,2,3,4,5"));
    CHECK(PumpUntil(bs, 2000, [&] { return h.net.PeerState(4) == RealDlpNet::kAuthed && h.net.PeerState(5) == RealDlpNet::kAuthed; }));
    CHECK_EQ(MaskOf(m2.e()), 0x1Fu);
    CHECK_EQ(MaskOf(m3.e()), 0x1Fu);
    CHECK_EQ(MaskOf(m5.e()), 0x1Fu);
    // a spectator comes and stays (every searcher is one while it reads the announcement): it is in nobody's list, and no departure is its business
    {
        u16 name[kNameUnits] = {'S', 'p', 0};
        CHECK_EQ(watcher.engine->InitializeForLobby(0x5757, name), 0u);
        CHECK_EQ(watcher.engine->ConnectBeginDlp(h.mac(), 11, ConnectionType::Spectator), 0u);
        CHECK(PumpUntil(bs, 3000, [&] { return watcher.engine->ConnectPoll() == 1 && watcher.engine->status() == NetworkStatus::ConnectedAsSpectator; }));
        CHECK_EQ(h.e().SpectatorCount(), 1u);
        CHECK_EQ(h.Clients(), std::string("4:2,3,4,5"));
    }

    // node 5 leaves (a dialogue past the three a title of four has): ONE Deauthentication (an Azahar client's goodbye)
    const u32 auths = h.net.counters().auth;
    const std::size_t mark = wire.Mark();
    const u64 t_left = platform::NowMs();
    m5.StopSession();
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "3:2,3,4" && c.Nodes() == "4:1,2,3,4" && MaskOf(m2.e()) == 0x0F && MaskOf(m3.e()) == 0x0F; }));
    std::printf("   a client left -> the host's list, the console's and the two Azahar's: %u ms\n", static_cast<u32>(platform::NowMs() - t_left));
    CHECK_EQ(h.Clients(), std::string("3:2,3,4"));           // the host knows
    CHECK_EQ(c.Nodes(), std::string("4:1,2,3,4"));           // the console that is a client knows
    CHECK_EQ(MaskOf(m2.e()), 0x0Fu);                         // the Azahar clients know
    CHECK_EQ(MaskOf(m3.e()), 0x0Fu);
    CHECK_EQ(h.net.PeerState(5), RealDlpNet::kFree);         // its dialogue is forgotten
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kAuthed);       // ... and nobody else's
    CHECK_EQ(h.net.PeerState(3), RealDlpNet::kAuthed);
    CHECK_EQ(h.net.PeerState(4), RealDlpNet::kAuthed);
    {
        NodeEntry en;
        CHECK_EQ(h.lobby.ServerClientInfo(5, &en), kNoData);
        CHECK_EQ(c.lobby.ClientNodeInfo(5, &en), kNoData);
        CHECK_EQ(c.lobby.ClientNodeInfo(3, &en), 0u);
    }
    {
        // how they were told: ONE Logoff to each of the three that remain, each naming its own node, the list without node 5; no node map,
        // nothing to the room (design D17)
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
            CHECK(out[k].dest != watcher.mac);  // never to the spectator (a Logoff makes an Azahar that is Connecting elsewhere a node of this network)
            CHECK(lo.assigned_node_id == 2 || lo.assigned_node_id == 3 || lo.assigned_node_id == 4);
            seen |= 1u << lo.assigned_node_id;
            CHECK_EQ(lo.connected_nodes, 4u);
            CHECK_EQ(lo.max_nodes, 8u);
            u32 listed = 0;
            for (const NodeInfo& n : lo.nodes) {
                listed |= n.network_node_id != 0 ? 1u << n.network_node_id : 0u;
            }
            CHECK_EQ(listed, 0x1Eu);  // nodes 1, 2, 3, 4
        }
        CHECK_EQ(logoffs, 3u);
        CHECK_EQ(seen, 0x1Cu);
        CHECK_EQ(maps, 0u);
        CHECK_EQ(h.e().SpectatorCount(), 1u);  // (the spectator was there the whole time)
        watcher.engine->Shutdown();            // ... and goes, as a searcher that read the announcement: its goodbye tells nobody (A)
        CHECK(PumpUntil(bs, 2000, [&] { return h.e().SpectatorCount() == 0; }));
    }
    // the next one takes the place: node 5, a dialogue from the Auth
    CHECK(ModelJoin(bs, m6, h.mac(), &pr));
    CHECK_EQ(NodeOf(m6.e()), 5u);
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "4:2,3,4,5" && c.Nodes() == "5:1,2,3,4,5" && h.net.PeerState(5) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.Clients(), std::string("4:2,3,4,5"));
    CHECK_EQ(c.Nodes(), std::string("5:1,2,3,4,5"));
    CHECK(h.net.counters().auth > auths);
    {
        NodeEntry en;
        CHECK_EQ(h.lobby.ServerClientInfo(5, &en), 0u);
        CHECK_EQ(Str(en.name), std::string("Az6"));
        CHECK_EQ(c.lobby.ClientNodeInfo(5, &en), 0u);
        CHECK_EQ(Str(en.name), std::string("Az6"));
    }
    // START and GO for the four that are there: the console client and the three Azahar
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(bs, 1500, [&] { return h.lobby.ServerState() == 8; }));
    CHECK_EQ(h.Clients(), std::string("4:2,3,4,5"));
    CHECK(PumpUntil(bs, 1000, [&] { return c.Status() == 6; }));  // the console client: "downloading" since the START
    CHECK_EQ(h.lobby.ServerSendPassphrase(kDlpRealMk7Pass), 0u);
    CHECK(PumpUntil(bs, 2500, [&] { return h.lobby.ServerState() == 9 && c.Status() == 9; }));
    CHECK_EQ(h.lobby.ServerState(), 9u);
    CHECK_EQ(c.Status(), 9u);
    u8 pass[9] = {};
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, kDlpRealMk7Pass, 9) == 0);
    for (AzaharDlpClient* m : {&m2, &m3, &m6}) {
        CHECK(WaitFor(1000, [&] { return m->left.load(); }));
        const std::vector<u8> p = m->passphrase();
        CHECK(p.size() == 9 && std::memcmp(p.data(), kDlpRealMk7Pass, 9) == 0);
        CheckModel(*m);
    }
    CHECK_EQ(m5.state(), AzaharDlpClient::kJoined);  // the one that left never heard of the start
    CheckModel(m5);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK(!c.net.Holds() && !h.net.Holds());
    CHECK_EQ(h.net.counters().bad, 0u);
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------
// D. Luigi's Mansion: everything as before.

static void D_Luigi() {
    std::printf("D: Luigi's Mansion as before: the ceiling of four, one client, its capture's bytes, no frame when its client leaves\n");
    {
        // an adapter nobody told is a title of four: it refuses a network of more, and has three dialogues
        MockRoom room;
        Console con(room, 9);
        RealDlpNet real(con.engine.get());
        CHECK_EQ(real.players(), 4u);
        CHECK_EQ(RealDlpNet::kRealPlayers, 8u);
        CHECK_EQ(RealDlpNet::kRealClients, 7u);
        const Identity me = Me(0xF009, "Luigi");
        for (const u32 said : {0u, 4u}) {
            real.SetPlayers(static_cast<u8>(said));
            CHECK_EQ(real.players(), 4u);
            for (const u32 nodes : {5u, 6u, 7u, 8u, 9u, 16u, 1u, 0u}) {
                CHECK(!real.HostOpen(Ann(nodes), 6, me));
                CHECK(!real.Holds());
                CHECK(!con.engine->initialized());
            }
            for (const u32 nodes : {2u, 3u, 4u}) {
                CHECK(real.HostOpen(Ann(nodes), 6, me));
                CHECK_EQ(con.engine->GetConnectionStatus().max_nodes, nodes);
                for (u8 node = 0; node < 12; ++node) {
                    CHECK_EQ(real.PeerState(node), 0u);
                    CHECK_EQ(real.Send(node, kMsg, 2), node >= 2 && node <= 4);
                }
                real.HostClose();
                CHECK(!con.engine->initialized());
            }
        }
        // told eight (Mario Kart 7's row): up to eight nodes, seven dialogues, never more
        real.SetPlayers(8);
        CHECK_EQ(real.players(), 8u);
        for (const u32 nodes : {9u, 16u, 1u}) {
            CHECK(!real.HostOpen(Ann(nodes), 6, me));
        }
        for (const u32 nodes : {2u, 4u, 5u, 8u}) {
            CHECK(real.HostOpen(Ann(nodes), 6, me));
            CHECK_EQ(con.engine->GetConnectionStatus().max_nodes, nodes);
            for (u8 node = 0; node < 12; ++node) {
                CHECK_EQ(real.Send(node, kMsg, 2), node >= 2 && node <= 8);
            }
            real.HostClose();
        }
        for (const u32 said : {1u, 2u, 3u, 5u, 9u, 255u}) {
            real.SetPlayers(static_cast<u8>(said));
            CHECK_EQ(real.players(), TitlePlayers(said));
        }
    }
    g_rules.Reset();
    MockRoom room;
    Box h(room, 1, "Host", kLuigiLow, 0);  // (a configuration that says nothing: four)
    Tap wire(h.con);
    AzaharDlpClient m(room, 2, "Azahar"), again(room, 3, "Again");
    Console watcher(room, 7);  // a searcher in the middle of its probe: a spectator that is in while the client leaves
    Ticker ticker({&h.e(), &m.e(), &again.e(), watcher.engine.get()});
    const Boxes bs = {&h};
    CHECK_EQ(h.net.players(), 4u);
    CHECK_EQ(h.lobby.max_players(), 4u);
    // the game (console log of 2026-11-08): ONE client, manual acceptance, channel 6
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK_EQ(h.e().GetConnectionStatus().max_nodes, 2u);
    {
        // the spectator first (a full network of two answers no newcomer), and it stays
        u16 name[kNameUnits] = {'S', 'p', 0};
        CHECK_EQ(watcher.engine->InitializeForLobby(0x5757, name), 0u);
        CHECK_EQ(watcher.engine->ConnectBeginDlp(h.mac(), 6, ConnectionType::Spectator), 0u);
        CHECK(PumpUntil(bs, 3000, [&] { return watcher.engine->ConnectPoll() == 1 && watcher.engine->status() == NetworkStatus::ConnectedAsSpectator; }));
    }
    AzaharDlpClient::Probe pr;
    CHECK(ModelJoin(bs, m, h.mac(), &pr));
    CHECK_EQ(pr.child_tid, kLuigiChild);
    CHECK_EQ(pr.max_nodes, 2u);
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "1:2" && h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.e().SpectatorCount(), 1u);
    // its client leaves before the start: the host knows, and puts NOTHING on the wire for it (nobody remains to tell: the refresh of
    // Mario Kart 7's lobby does not reach a game with one client, and never a spectator) - only its beacons and, for the spectator that is
    // looking, the announcement go on
    PumpFor(bs, 150);
    const std::size_t mark = wire.Mark();
    m.StopSession();
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "0:"; }));
    PumpFor(bs, 700);  // (longer than the announcement's interval)
    {
        const std::vector<Tap::Out> out = wire.All();
        u32 beacons = 0, announced = 0;
        for (std::size_t k = mark; k < out.size(); ++k) {
            Tap::Dlp d;
            if (out[k].type == WifiPacket::Type::Beacon) {
                ++beacons;
                continue;
            }
            if (Tap::AsDlp(out[k], &d) && d.channel == 1 && d.dest == 0xFFFF) {
                ++announced;
                continue;
            }
            std::printf("   a frame after the client left: type %u, %s\n", static_cast<unsigned>(out[k].type), Hex(out[k].data.data(), out[k].data.size() < 32 ? out[k].data.size() : 32).c_str());
            CHECK(false);
        }
        CHECK(beacons >= 2);
        CHECK(announced > 0);  // (the spectator was there, looking, the whole time)
        CHECK_EQ(h.e().SpectatorCount(), 1u);
    }
    watcher.engine->Shutdown();  // the searcher goes: the rest is the session of one client, as before
    CHECK(PumpUntil(bs, 2000, [&] { return h.e().SpectatorCount() == 0; }));
    CHECK_EQ(h.net.PeerState(2), RealDlpNet::kFree);
    // another comes: the whole sequence, with the bytes of Luigi's capture
    CHECK(ModelJoin(bs, again, h.mac(), &pr));
    CHECK(PumpUntil(bs, 2000, [&] { return h.Clients() == "1:2"; }));
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK(PumpUntil(bs, 2000, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(bs, 1500, [&] { return h.lobby.ServerState() == 8; }));
    static const u8 kLuigiPass[9] = {'5', '0', 'e', '4', 'd', '8', '8', 'c', 0};
    CHECK_EQ(h.lobby.ServerSendPassphrase(kLuigiPass), 0u);
    CHECK(PumpUntil(bs, 2500, [&] { return h.lobby.ServerState() == 9; }));
    CHECK_EQ(again.state(), AzaharDlpClient::kComplete);
    {
        u32 kinds = 0;
        for (const Tap::Out& o : wire.All()) {
            Tap::Dlp d;
            if (!Tap::AsDlp(o, &d) || d.channel != 2) {
                continue;
            }
            CHECK_EQ(d.dest, 2u);
            const dlp_real::Packet p = dlp_real::ParseFromHost(d.p.data(), d.p.size());
            const char* id = p.kind == dlp_real::Kind::Auth        ? "A1"
                             : p.kind == dlp_real::Kind::StartDist ? "A3"
                             : p.kind == dlp_real::Kind::Finish    ? "A5"
                             : p.kind == dlp_real::Kind::Final     ? "A7"
                             : p.kind == dlp_real::Kind::Notice    ? "A8"
                                                                   : nullptr;
            CHECK(id != nullptr);
            if (id != nullptr) {
                kinds |= 1u << static_cast<u8>(p.kind);
                CHECK(Same(d.p, Luigi(id), kDlpRealLuigiSeed));
            }
        }
        CHECK(kinds & (1u << static_cast<u8>(dlp_real::Kind::Auth)));
        CHECK(kinds & (1u << static_cast<u8>(dlp_real::Kind::StartDist)));
        CHECK(kinds & (1u << static_cast<u8>(dlp_real::Kind::Finish)));
        CHECK(kinds & (1u << static_cast<u8>(dlp_real::Kind::Final)));
    }
    // a game of a title of four that asks for seven clients gets three: the lobby's ceiling, as before
    {
        Box four(room, 4, "Four", kLuigiLow, 0);
        CHECK_EQ(four.lobby.ServerInitialize(7, 0, nullptr), 0u);
        CHECK_EQ(four.lobby.ServerStartAccepting(false, 6), 0u);
        CHECK_EQ(four.e().GetConnectionStatus().max_nodes, 4u);
        CHECK_EQ(four.lobby.ServerFinalize(), 0u);
        CHECK(!four.net.Holds());
    }
    CheckModel(m);
    CheckModel(again);
    CHECK_RULES();
}

int main() {
    std::printf("dlp_real_mk7_test: Mario Kart 7 on the real Download Play with eight players, against the capture of eight Azahar\n");
    G_Capture();
    A_HostSeven();
    B_SearchEight();
    C_OneLeaves();
    D_Luigi();
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
