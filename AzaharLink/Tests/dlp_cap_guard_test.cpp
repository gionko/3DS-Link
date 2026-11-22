// The guard of the 4-player titles of the fake Download Play lobby (2026-11-13), written BEFORE the lobby learns to hold eight players
// (host + 7 clients) for the titles that ask for them. It holds what the lobby does TODAY for a title whose ceiling is four - Kirby Triple
// Deluxe, Planet Robobot, Mario Tennis Open, and Luigi's Mansion on the real Download Play - so that a change of the tables' capacity that
// moves one byte on the wire, one message, one ejection or one word of an answer to the game for those titles fails here. Every expected
// value below is a literal taken from the output of the code of that day (the snapshot is analysis/lobby-otto-2026-11-13/prima), never
// a constant of the code under test. A LobbyConfig is always built zeroed with the three fields of that day: a field added later must
// mean "four players" at zero.
//
//   A  the announcement: the 29 bytes of the codec, and the 29 bytes the lobby puts on the air for what a game asks of dlp:SRVR Initialize
//      (3 clients as Kirby, 1 as Luigi's Mansion, and 0, 2, 4, 7, 15, 255 and words with rubbish in the high bytes: cut to 1..3 clients),
//      for each title; what a searcher of such a title lists of the lobbies in the air (never one that says more than 4 nodes)
//   B  whole sessions, every console driven ONLY through the IPC words of dlp:FKCL / dlp:SRVR as the plugin's hub runs them (the
//      commands that read a table on the game's thread, the others and the pump on the network thread), over the in-memory air of
//      Tests/dlp_fakenet.hpp and its simulated clock (no real time: the same run gives the same lines):
//        S1  host + 3 clients: the search, the three joins, a fifth console that tries to come in and is never answered (its join
//            times out at 3000 ms), START, GO, the passphrase, the clients that leave
//        S2  manual acceptance: a client that leaves and its seat taken again, DisconnectClient, the client never accepted ejected at
//            StartDistribution, the client that never answers START ejected at 1500 ms, EndAccepting / StartAccepting again
//        S3  a lobby of two (what Luigi's Mansion asks): the third console never answered, a node above the title's size ejected
//        S4  a faulty engine that lists a node 5: ejected, again every 200 ms while it lingers
//      Each session is recorded as text lines - what the lobby asked of the network that changes it or the air (HostOpen with the
//      announcement, HostUpdate, HostClose, Kick, ScanOpen / ScanClose, ConnectBegin, Disconnect, every Send with its bytes) and every
//      answer to the game (the reply words, the bytes written in the game's buffers) - and held by the SHA-1 of the lines, their count
//      and the count of each kind; how often the lobby READ the network (Members, ReadNode, Receive, ScanRead, ConnectPoll) is held too.
//   C  the answers that depend on the number of players, in S1 with the four in: GetConnectingClients / GetConnectingNodes for
//      every size of the game's array, GetServerState, GetClientInfo / GetClientState / GetNodeInfo of the nodes 0..5, GetServerInfo
//   D  Luigi's Mansion, the REAL Download Play (the lobby on RealDlpNet over the real engine, real time): the codec against the
//      golden bytes of Tests/dlp_real_luigi_golden.inc (the minimum of Tests/dlp_real_wire_test.cpp), our host in front of the model of
//      Azahar's client (the 5 frames of the announcement and the packets A1, A3, A5, A7, A8 it sends are the golden ones) and our
//      searcher in front of the model of Azahar's host (the answers A2, A4, A6, A9 it sends are the golden ones)
//   E  the dialect over the real engine (EngineLobbyNet, real time): the 29 bytes as another console hears them in the beacon, a
//      session of four with a fifth console the engine never admits (the messages of the lobby as they leave each console), and the
//      beacons of lobbies of other sizes in front of a searcher (2, 3, 4 nodes listed; 1, 5, 8, 16 never)
// The parts on the real engine run in real time (about 15 s): nothing in them is held by a count of frames or by a time.
//
// With a folder as its argument the test writes the lines of every session there (S1-wire.txt, S1-ipc.txt, S1-reads.txt, ...): those of
// the day are in analysis/lobby-otto-2026-11-13/guardia/righe (the SHA-1 of a -wire or -ipc file is the one held here), and
// analysis/lobby-otto-2026-11-13/guardia/RAPPORTO.md says what this test holds and what it cannot.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=address,undefined Tests/dlp_cap_guard_test.cpp
//       Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o
//       <the core sources and the ENet objects of Tests/run.sh>
#pragma GCC diagnostic ignored "-Wunused-function"  // (the rig's helpers this test does not use)
#include "dlp_net_rig.hpp"

#include <algorithm>
#include <map>

#include "azahar_dlp_host.hpp"
#include "azahar_dlp_peer.hpp"
#include "azl/crypto.hpp"
#include "azl/dlp_hub.hpp"
#include "azl/dlp_ipc.hpp"
#include "azl/dlp_real_net.hpp"
#include "dlp_fakenet.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static std::string g_dump_dir;  // argv[1]: where the lines of the sessions are written (nowhere when empty)

// ---------------------------------------------------------------------------------------------------------------------
// Numbers written out (not the constants of the headers).

static constexpr u32 G_NO_DATA = 0xC880AFEF;
static constexpr u32 G_TIMEOUT = 0xC920AFFE;
static constexpr u32 G_INVALID_STATE = 0xE0A0AC01;
static constexpr DlpKind kFk = DlpKind::Fkcl;
static constexpr DlpKind kSv = DlpKind::Srvr;

static constexpr u32 kTdLow = 0x0010C000;     // Kirby Triple Deluxe
static constexpr u32 kPrLow = 0x00183600;     // Kirby Planet Robobot
static constexpr u32 kMtoLow = 0x0007C800;    // Mario Tennis Open (Europe)
static constexpr u32 kLuigiLow = 0x001D1A00;  // Luigi's Mansion (2018, Europe)
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;

static std::string Fmt(const char* f, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0, unsigned long long d = 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}
static std::string MacHex(const Mac& m) {
    return Hex(m.data(), 6);
}
static std::string NameOf(const u16* n) {
    std::string s;
    for (u32 i = 0; i < 10 && n[i] != 0; ++i) {
        s += static_cast<char>(n[i]);
    }
    return s;
}
static std::string NameLE(const u8* p) {
    u16 n[10];
    for (u32 i = 0; i < 10; ++i) {
        n[i] = LoadLE16(p + 2 * i);
    }
    return NameOf(n);
}
static std::string AnnHex(const HostAnnounce& a) {
    u8 b[29];
    EncodeAnnounce(a, b);
    return Hex(b, 29);
}
static void SetName(u16* dst, const char* s) {
    for (u32 i = 0; i < 10; ++i) {
        dst[i] = (s != nullptr && *s != 0) ? static_cast<u16>(static_cast<u8>(*s++)) : 0;
    }
}
static u32 MacW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 MacW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}

// ---------------------------------------------------------------------------------------------------------------------
// The record of a session: text lines, held by their SHA-1.

struct Trace {
    std::string text;
    u32 lines = 0;
    void Add(const std::string& line) {
        text += line;
        text += '\n';
        ++lines;
    }
    std::string Sha() const {
        u8 d[20];
        Sha1(reinterpret_cast<const u8*>(text.data()), text.size(), d);
        return Hex(d, 20);
    }
    /// Lines that contain `what`.
    u32 Count(const std::string& what) const {
        u32 n = 0;
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t end = text.find('\n', at);
            n += text.substr(at, end - at).find(what) != std::string::npos;
            at = end + 1;
        }
        return n;
    }
};

static void Hold(const std::string& name, const Trace& t, const char* want_sha, u32 want_lines) {
    const std::string got = t.Sha();
    std::printf("   %-10s %s  %u lines\n", name.c_str(), got.c_str(), t.lines);
    CHECK_EQ(got, std::string(want_sha));
    CHECK_EQ(t.lines, want_lines);
    if (!g_dump_dir.empty()) {
        const std::string path = g_dump_dir + "/" + name + ".txt";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(t.text.data(), 1, t.text.size(), f);
            std::fclose(f);
        } else {
            std::printf("   (cannot write %s)\n", path.c_str());
        }
    }
}

/// How often every console READ its network in a session (not the wire, not an answer: the lobby's own work on the engine).
static void HoldReads(const std::string& name, const std::string& got, const char* want) {
    if (got != want) {
        std::printf("   %s: the lobby reads its network another number of times (the wire and the answers are the two SHA-1 above):\n%s", name.c_str(), got.c_str());
    }
    CHECK(got == want);
    if (!g_dump_dir.empty()) {
        if (FILE* f = std::fopen((g_dump_dir + "/" + name + ".txt").c_str(), "wb")) {
            std::fwrite(got.data(), 1, got.size(), f);
            std::fclose(f);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The network of a console: the in-memory air, with every call of the lobby that changes something written down, and the reads counted.

struct World;

class RecNet : public FakeLobbyNet {
public:
    RecNet(FakeAir& air, int index, World* world) : FakeLobbyNet(air, index), world_(world) {}

    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override {
        const bool ok = FakeLobbyNet::HostOpen(a, channel, me);
        Line(Fmt("HostOpen ch=%llu max=%llu acc=%llu ann=", channel, a.max_nodes, a.accepting) + AnnHex(a) + Fmt(" seed=%llx name=", me.seed) +
             NameOf(me.name) + Fmt(" -> %llu", ok));
        return ok;
    }
    void HostUpdate(const HostAnnounce& a) override {
        FakeLobbyNet::HostUpdate(a);
        Line(Fmt("HostUpdate max=%llu acc=%llu ann=", a.max_nodes, a.accepting) + AnnHex(a));
    }
    void HostClose() override {
        FakeLobbyNet::HostClose();
        Line("HostClose");
    }
    void Kick(u8 node) override {
        FakeLobbyNet::Kick(node);
        Line(Fmt("Kick %llu", node));
    }
    bool ScanOpen() override {
        const bool ok = FakeLobbyNet::ScanOpen();
        Line(Fmt("ScanOpen -> %llu", ok));
        return ok;
    }
    void ScanClose() override {
        FakeLobbyNet::ScanClose();
        Line("ScanClose");
    }
    bool ConnectBegin(const Mac& host, const Identity& me) override {
        const bool ok = FakeLobbyNet::ConnectBegin(host, me);
        Line("ConnectBegin host=" + MacHex(host) + Fmt(" seed=%llx name=", me.seed) + NameOf(me.name) + Fmt(" -> %llu", ok));
        return ok;
    }
    void Disconnect() override {
        FakeLobbyNet::Disconnect();
        Line("Disconnect");
    }
    bool Send(u8 to_node, const u8* data, u32 size) override {
        const bool ok = FakeLobbyNet::Send(to_node, data, size);
        Line(Fmt("Send to=%llu len=%llu ", to_node, size) + Hex(data, size) + Fmt(" -> %llu", ok));
        return ok;
    }
    // the reads: counted, not written down
    bool ScanRead(u32 index, HeardGroup* out) override {
        ++scan_read;
        return FakeLobbyNet::ScanRead(index, out);
    }
    int ConnectPoll() override {
        ++connect_poll;
        return FakeLobbyNet::ConnectPoll();
    }
    u8 MyNode() override {
        ++my_node;
        return FakeLobbyNet::MyNode();
    }
    u32 Members(u8 (&nodes)[kMaxPlayers]) override {
        ++members;
        return FakeLobbyNet::Members(nodes);
    }
    bool ReadNode(u8 node, NodeEntry* out) override {
        ++read_node;
        return FakeLobbyNet::ReadNode(node, out);
    }
    bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) override {
        ++receive;
        return FakeLobbyNet::Receive(from_node, data, capacity, size);
    }
    u32 scan_read = 0, connect_poll = 0, my_node = 0, members = 0, read_node = 0, receive = 0;

private:
    void Line(const std::string& what);
    World* world_;
};

// ---------------------------------------------------------------------------------------------------------------------
// The memory of the game: regions at made-up 32-bit addresses, each a heap block of exactly its size (a byte past it is an
// AddressSanitizer report), filled with 0xCC before every call so that what the service did not write shows.

class GameMem {
public:
    u32 Alloc(u32 size) {
        Region r;
        r.addr = next_;
        r.bytes.assign(size, 0xCC);
        next_ += (size + 0x4F) & ~15u;
        regions_.push_back(std::move(r));
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
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        return static_cast<GameMem*>(ctx)->Find(addr, size);
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = &Thunk;
        return r;
    }

private:
    struct Region {
        u32 addr = 0;
        std::vector<u8> bytes;
    };
    std::vector<Region> regions_;
    u32 next_ = 0x08000000;
};

struct Reply {
    u32 w[16];
    DlpOutcome out;
    u32 hdr() const {
        return w[0];
    }
    u32 res() const {
        return w[1];
    }
};

/// The record GetServerInfo wrote, as a line: every byte is in it or is said to be zero (the tick at +0x290 is left out: it is the clock).
static std::string ServerRecord(const u8* p) {
    bool zero = true;
    for (u32 i = 0; i < 0x290; ++i) {
        const bool known = i < 6 || i == 7 || i == 8 || i == 9 || (i >= 0x1C && i < 0x30);
        zero &= known || p[i] == 0;
    }
    return " | server mac=" + Hex(p, 6) + Fmt(" signal=%llu max=%llu joined=%llu name=", p[7], p[8], p[9]) + NameLE(p + 0x1C) + (zero ? " rest=0" : " rest=NOT ZERO");
}
/// The record GetTitleInfo / GetTitleInfoInOrder wrote.
static std::string TitleRecord(const u8* p) {
    bool zero = true;
    for (u32 i = 14; i < 0x13A8; ++i) {
        zero &= p[i] == 0;
    }
    return " | title uid=" + Fmt("%llx var=%llx mac=", LoadLE32(p), LoadLE32(p + 4)) + Hex(p + 8, 6) + (zero ? " rest=0" : " rest=NOT ZERO");
}

// ---------------------------------------------------------------------------------------------------------------------
// One console: the lobby, the service, the memory of its game, and the game itself (what it asks every frame).

enum class Role { Idle, Host, Scan, Joining, Session };

struct Box {
    Box(World& world, int index, const LobbyConfig& cfg);

    // ---- one request, as the plugin's hub runs it -----------------------------------------------------------------------------------
    Reply Call(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf = nullptr, bool poll = false);
    Reply Pending(bool* done);
    /// The network thread's round (the hub's Tick).
    void Tick() {
        lobby.SetOnNet(true);
        net.Pump();
        lobby.Pump();
        lobby.SetOnNet(false);
    }
    /// The game starts nwm::UDS (the hub's OnGameUdsStarting, served by the network thread).
    void GameUds();

    // ---- dlp:FKCL, the request headers of the games -----------------------------------------------------------------------------------
    Reply FkInit(u32 beacons) {
        return Call(kFk, 0x00010083, {0x30000, beacons, 0x04000000, 0x1111, 0x2222});
    }
    Reply FkInitName(const char* name, u32 beacons) {
        u16 n[10];
        SetName(n, name);
        return Call(kFk, 0x00110203, {0x30000, beacons, W(n, 0), W(n, 1), W(n, 2), W(n, 3), W(n, 4), 0, 0x04000000, 0x3333, 0x4444});
    }
    Reply FkFinalize() {
        return Call(kFk, 0x00020000, {});
    }
    Reply FkChannels() {
        return Call(kFk, 0x00040000, {});
    }
    Reply FkScan(u64 filter) {
        return Call(kFk, 0x00050180, {0x0421, static_cast<u32>(filter), static_cast<u32>(filter >> 32), 0, 0, 0});
    }
    Reply FkStopScan() {
        return Call(kFk, 0x00060000, {});
    }
    Reply FkServerInfo(const Mac& m, bool poll = false) {
        mem.Refill(a_server);
        return Call(kFk, 0x00070080, {MacW1(m), MacW2(m)}, sb_server, poll);
    }
    Reply FkInOrder(u32 restart, bool poll = false) {
        mem.Refill(a_title);
        return Call(kFk, 0x00090040, {restart}, sb_title, poll);
    }
    Reply FkJoin(const Mac& m) {
        return Call(kFk, 0x000B0100, {MacW1(m), MacW2(m), tid_low, 0x00040001});
    }
    Reply FkStatus(bool poll = false) {
        return Call(kFk, 0x000C0000, {}, nullptr, poll);
    }
    Reply FkNodeInfo(u32 node, bool poll = false) {
        return Call(kFk, 0x000E0040, {node}, nullptr, poll);
    }
    Reply FkPass(bool poll = false) {
        return Call(kFk, 0x000F0000, {}, nullptr, poll);
    }
    Reply FkStopSession() {
        return Call(kFk, 0x00100000, {});
    }
    // ---- dlp:SRVR ---------------------------------------------------------------------------------------------------------------------
    /// [1] the shared memory size (Kirby Triple Deluxe's), [2] the clients, [3] the process id, [4] the child index.
    Reply SvInit(u32 max_clients, u32 child_word = 0) {
        return Call(kSv, 0x00010183, {0x87000, max_clients, 0xE, child_word, 0x40000, 2, 0x04000000, 0x5555, 0x6666});
    }
    Reply SvInitName(u32 max_clients, u32 child_word, const char* name) {
        u16 n[10];
        SetName(n, name);
        return Call(kSv, 0x000F0303, {0x87000, max_clients, 0xE, child_word, 0x40000, 2, W(n, 0), W(n, 1), W(n, 2), W(n, 3), W(n, 4), 0, 0x04000000, 0x7777, 0x8888});
    }
    Reply SvFinalize() {
        return Call(kSv, 0x00020000, {});
    }
    Reply SvState(bool poll = false) {
        return Call(kSv, 0x00030000, {}, nullptr, poll);
    }
    Reply SvAccepting(u32 manual, u32 channel) {
        return Call(kSv, 0x00050080, {manual, channel});
    }
    Reply SvEnd() {
        return Call(kSv, 0x00060000, {});
    }
    Reply SvDistribute() {
        return Call(kSv, 0x00070000, {});
    }
    Reply SvPass(const u8* b, bool poll = false) {
        const u32 w0 = static_cast<u32>(b[0]) | (static_cast<u32>(b[1]) << 8) | (static_cast<u32>(b[2]) << 16) | (static_cast<u32>(b[3]) << 24);
        const u32 w1 = static_cast<u32>(b[4]) | (static_cast<u32>(b[5]) << 8) | (static_cast<u32>(b[6]) << 16) | (static_cast<u32>(b[7]) << 24);
        return Call(kSv, 0x000800C0, {w0, w1, b[8]}, nullptr, poll);
    }
    Reply SvAccept(u32 node) {
        return Call(kSv, 0x00090040, {node});
    }
    Reply SvDisconnect(u32 node) {
        return Call(kSv, 0x000A0040, {node});
    }
    Reply SvClientInfo(u32 node, bool poll = false) {
        return Call(kSv, 0x000C0040, {node}, nullptr, poll);
    }
    Reply SvClientState(u32 node, bool poll = false) {
        return Call(kSv, 0x000D0040, {node}, nullptr, poll);
    }
    Reply SvDup() {
        return Call(kSv, 0x00100000, {});
    }

    // ---- what a game does with the answers ----------------------------------------------------------------------------------------------
    /// GetConnectingClients into an array of `n` elements that the game says is `bytes` long: the ids it can read, *total the count answered.
    std::vector<u16> Clients(u32 n, u32 bytes, u32* total, bool poll = false, u32* result = nullptr) {
        mem.Refill(a_ids);
        const Reply r = Call(kSv, 0x000B0042, {n, (bytes << 4) | 0xC, a_ids}, nullptr, poll);
        std::vector<u16> ids;
        if (result != nullptr) {
            *result = r.res();
        }
        *total = r.res() == 0 ? r.w[2] : 0;
        if (r.res() != 0) {
            return ids;
        }
        const u8* p = mem.Find(a_ids, 64);
        for (u32 i = 0; i < 32 && p[2 * i] != 0xCC; ++i) {
            ids.push_back(LoadLE16(p + 2 * i));
        }
        return ids;
    }
    /// GetConnectingNodes(n) into a static buffer of `cap` bytes: the ids, *total, *desc the descriptor of what was written.
    std::vector<u16> Nodes(u32 n, u32 cap, u32* total, bool poll = false, u32* desc = nullptr) {
        mem.Refill(a_ids);
        const u32 sb[2] = {(cap << 14) | 2, a_ids};
        const Reply r = Call(kFk, 0x000D0040, {n}, sb, poll);
        std::vector<u16> ids;
        *total = r.res() == 0 ? r.w[2] : 0;
        if (desc != nullptr) {
            *desc = r.res() == 0 ? r.w[3] : 0xFFFFFFFFu;
        }
        if (r.res() != 0) {
            return ids;
        }
        const u8* p = mem.Find(a_ids, 64);
        for (u32 i = 0; i < 32 && p[2 * i] != 0xCC; ++i) {
            ids.push_back(LoadLE16(p + 2 * i));
        }
        return ids;
    }
    u32 Status() {
        return FkStatus().w[2];
    }
    /// The 9 bytes of GetWirelessRebootPassphrase, as text.
    std::string Pass() {
        const Reply r = FkPass();
        return std::string(reinterpret_cast<const char*>(&r.w[2]), 8);
    }

    // ---- the game's frame -----------------------------------------------------------------------------------------------------------------
    void Frame();
    void BeginJoin(const Mac& host) {
        FkStopScan();
        const Reply r = FkJoin(host);
        if (r.out == DlpOutcome::JoinPending) {
            role = Role::Joining;
        } else {
            join_result = r.res();
            role = Role::Idle;
        }
    }
    const Mac& mac() {
        return net.mac();
    }

    static u32 W(const u16* n, u32 i) {
        return static_cast<u32>(n[2 * i]) | (static_cast<u32>(n[2 * i + 1]) << 16);
    }

    World& w;
    int idx;
    RecNet net;
    DlpLobby lobby;
    DlpService svc;
    GameMem mem;
    u32 tid_low;
    u32 a_title, a_server, a_ids;
    u32 sb_title[2], sb_server[2];
    Role role = Role::Idle;
    bool pass_on = false;  // the host's game sends the passphrase every frame while the state is 8 (a new one each time, as "%08x")
    u32 pass_k = 0;
    u32 join_result = 0xFFFFFFFFu;
    std::vector<Mac> seen;  // the hosts of the last list the game read
};

struct World {
    World() : air(100000), t0(air.now()) {}
    Box& Add(const char* name, u32 tid_low) {
        LobbyConfig c;
        std::memset(&c, 0, sizeof(c));  // (zeroed: whatever a later version adds to it must mean "four players" at zero)
        c.game_tid = 0x0004000000000000ULL | tid_low;
        c.friend_seed = 0xF000u + box.size();
        SetName(c.name, name);
        box.emplace_back(new Box(*this, static_cast<int>(box.size()), c));
        box.back()->tid_low = tid_low;
        return *box.back();
    }
    /// Moves the clock to `ms` after the start in steps of 5 ms: the network thread of every console at each step, its game's frame
    /// every third one (15 ms).
    void Run(u64 ms) {
        while (air.now() - t0 < ms) {
            air.Advance(5);
            ++step;
            for (auto& b : box) {
                b->Tick();
            }
            if (step % 3 == 0) {
                for (auto& b : box) {
                    b->Frame();
                }
            }
        }
    }
    std::string Stamp(int console) const {
        return Fmt("t=+%llu c%llu ", air.now() - t0, static_cast<unsigned long long>(console));
    }
    /// How often every console read its network, one line each.
    std::string Reads() {
        std::string s;
        for (auto& b : box) {
            const RecNet& n = b->net;
            s += Fmt("c%llu members=%llu read_node=%llu receive=%llu ", static_cast<unsigned long long>(b->idx), n.members, n.read_node, n.receive) +
                 Fmt("scan_read=%llu connect_poll=%llu my_node=%llu\n", n.scan_read, n.connect_poll, n.my_node);
        }
        return s;
    }
    FakeAir air;
    u64 t0;
    u32 step = 0;
    Trace wire, ipc;
    std::map<std::string, std::string> last_poll;  // the last answer of every command a game repeats each frame: written again only when it changes
    std::vector<std::unique_ptr<Box>> box;
};

void RecNet::Line(const std::string& what) {
    world_->wire.Add(world_->Stamp(index()) + what);
}

Box::Box(World& world, int index, const LobbyConfig& cfg) : w(world), idx(index), net(world.air, index, &world), lobby(&net, cfg), tid_low(0) {
    lobby.SetExternalPoll(true);  // (as the plugin's hub: the network work is the network thread's)
    svc.SetLobby(&lobby);
    a_title = mem.Alloc(0x13A8);
    a_server = mem.Alloc(0x298);
    a_ids = mem.Alloc(64);
    sb_title[0] = (0x13A8u << 14) | 2;
    sb_title[1] = a_title;
    sb_server[0] = (0x298u << 14) | 2;
    sb_server[1] = a_server;
}

Reply Box::Call(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf, bool poll) {
    u32 buf[64];
    for (u32 i = 0; i < 64; ++i) {
        buf[i] = 0xC0DE0000u + i;  // (rubbish after the arguments: a service that read a word it should not would show)
    }
    buf[0] = header;
    u32 a[16] = {};
    u32 n = 0;
    for (u32 v : args) {
        a[n] = v;
        buf[1 + n] = v;
        ++n;
    }
    const u32 id = header >> 16;
    Reply rp;
    std::memset(&rp, 0, sizeof(rp));
    if (DlpHub::IsInline(kind, id)) {
        rp.out = svc.Dispatch(kind, buf, sbuf, mem.Resolver());  // on the game's thread: it reads a table
    } else {
        lobby.SetOnNet(true);  // posted to the network thread: the request, then its round
        rp.out = svc.Dispatch(kind, buf, sbuf, mem.Resolver());
        net.Pump();
        lobby.Pump();
        lobby.SetOnNet(false);
    }
    std::memcpy(rp.w, buf, sizeof(rp.w));
    std::string key = Fmt(kind == kFk ? "FKCL %02llx(" : "SRVR %02llx(", id);
    for (u32 i = 0; i < n; ++i) {
        key += Fmt(i == 0 ? "%llx" : ",%llx", a[i]);
    }
    key += sbuf != nullptr ? Fmt(")[%llu]", sbuf[0] >> 14) : std::string(")");
    std::string body = " ->";
    if (rp.out == DlpOutcome::JoinPending) {
        body += " pending";
    } else {
        const u32 extent = 1 + ((buf[0] >> 6) & 0x3F) + (buf[0] & 0x3F);
        for (u32 i = 0; i < extent && i < 16; ++i) {
            body += Fmt(" %08llx", buf[i]);
        }
        if (kind == kFk && id == 0x07 && rp.res() == 0) {
            body += ServerRecord(mem.Find(a_server, 0x298));
        } else if (kind == kFk && (id == 0x08 || id == 0x09) && rp.res() == 0) {
            body += TitleRecord(mem.Find(a_title, 0x13A8));
        } else if ((kind == kFk && id == 0x0D) || (kind == kSv && id == 0x0B)) {
            body += " | " + Hex(mem.Find(a_ids, 32), 32);
        }
    }
    const std::string who = Fmt("c%llu ", static_cast<unsigned long long>(idx));
    if (poll) {
        std::string& last = w.last_poll[who + key];
        if (last == body) {
            return rp;
        }
        last = body;
    }
    w.ipc.Add(w.Stamp(idx) + key + body);
    return rp;
}

Reply Box::Pending(bool* done) {
    u32 buf[64];
    for (u32 i = 0; i < 64; ++i) {
        buf[i] = 0xC0DE0000u + i;
    }
    Reply rp;
    std::memset(&rp, 0, sizeof(rp));
    *done = svc.PollPending(buf);  // (on the game's thread, as the hub does it: the network thread's round decides the join)
    std::memcpy(rp.w, buf, sizeof(rp.w));
    rp.out = DlpOutcome::Done;
    if (*done) {
        w.ipc.Add(w.Stamp(idx) + Fmt("FKCL 0b pending -> %08llx %08llx", buf[0], buf[1]));
    }
    return rp;
}

void Box::GameUds() {
    lobby.SetOnNet(true);
    const bool released = lobby.OnGameUdsStarting();
    net.Pump();
    lobby.Pump();
    lobby.SetOnNet(false);
    w.ipc.Add(w.Stamp(idx) + Fmt("game starts nwm::UDS -> released %llu", released));
}

void Box::Frame() {
    if (role == Role::Host) {
        const Reply st = SvState(true);
        u32 total = 0;
        const std::vector<u16> ids = Clients(15, 30, &total, true);  // (15 elements, 30 bytes: what Kirby Triple Deluxe sends)
        for (u16 id : ids) {
            SvClientInfo(id, true);
            SvClientState(id, true);
        }
        if (pass_on && st.res() == 0 && st.w[2] == 8) {
            char text[16];
            std::snprintf(text, sizeof(text), "%08x", 0x50e4d88cu + pass_k++);
            SvPass(reinterpret_cast<const u8*>(text), true);
        }
    } else if (role == Role::Scan) {
        seen.clear();
        Reply r = FkInOrder(1, true);
        for (u32 i = 0; i < 16 && r.res() == 0; ++i) {
            Mac m;
            std::memcpy(m.data(), mem.Find(a_title, 0x13A8) + 8, 6);
            seen.push_back(m);
            FkServerInfo(m, true);
            r = FkInOrder(0, true);
        }
    } else if (role == Role::Joining) {
        bool done = false;
        const Reply r = Pending(&done);
        if (done) {
            join_result = r.res();
            role = r.res() == 0 ? Role::Session : Role::Idle;
        }
    } else if (role == Role::Session) {
        const Reply st = FkStatus(true);
        u32 total = 0;
        const std::vector<u16> ids = Nodes(16, 32, &total, true);
        for (u16 id : ids) {
            FkNodeInfo(id, true);
        }
        if ((st.w[2] >> 24) == 9) {
            FkPass(true);
        }
    }
}

static std::string Ids(const std::vector<u16>& v) {
    std::string s;
    for (u16 id : v) {
        s += Fmt(s.empty() ? "%llu" : ",%llu", id);
    }
    return s;
}

// =====================================================================================================================
// A. The announcement.

struct AnnWant {
    const char* what;
    u32 tid_low;
    u32 asked;       // the word [2] of dlp:SRVR Initialize as the game sends it
    u32 child_word;  // the word [4]
    const char* name;  // InitializeWithName's, or nullptr: Initialize
    u8 nodes;        // max_nodes the lobby announces
    u8 bytes[29];
};
#define ANN_NAME_3DSLINK 0x33, 0x00, 0x44, 0x00, 0x53, 0x00, 0x20, 0x00, 0x4C, 0x00, 0x69, 0x00, 0x6E, 0x00, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00
static const AnnWant kAnnWant[] = {
    // Kirby Triple Deluxe, Initialize with no name (the console log of 2026-11-13: "dlp:SRVR 0x01 req 00087000 00000003")
    {"TD 3 clients", kTdLow, 3, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"TD 1 client", kTdLow, 1, 0, nullptr, 2, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x02, ANN_NAME_3DSLINK}},
    {"TD 2 clients", kTdLow, 2, 0, nullptr, 3, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x03, ANN_NAME_3DSLINK}},
    // out of the lobby's size: cut to 1..3 clients, silently
    {"TD 0 clients", kTdLow, 0, 0, nullptr, 2, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x02, ANN_NAME_3DSLINK}},
    {"TD 4 clients", kTdLow, 4, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"TD 7 clients", kTdLow, 7, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"TD 15 clients", kTdLow, 15, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"TD 255 clients", kTdLow, 255, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    // rubbish in the high bytes: only the low byte counts (0x100 is 0 clients, 0x103 is 3, 0xFFFFFF07 is 7)
    {"TD 0x100", kTdLow, 0x100, 0, nullptr, 2, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x02, ANN_NAME_3DSLINK}},
    {"TD 0x103", kTdLow, 0x103, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"TD 0xFFFFFF07", kTdLow, 0xFFFFFF07u, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    // the child index is the low byte of [4], in the low byte of the title id
    {"TD child 2", kTdLow, 3, 0xABCD0102u, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x02, 0xC0, 0x10, 0x00, 0x04, ANN_NAME_3DSLINK}},
    // the other titles
    {"PR 3 clients", kPrLow, 3, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0x36, 0x18, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"PR 7 clients", kPrLow, 7, 0, nullptr, 4, {0x44, 0x4C, 0x01, 0x01, 0x00, 0x36, 0x18, 0x00, 0x04, ANN_NAME_3DSLINK}},
    {"MTO 3 clients, a name", kMtoLow, 3, 0, "Mario", 4,
     {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC8, 0x07, 0x00, 0x04, 0x4D, 0x00, 0x61, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6F, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"MTO 7 clients, a name", kMtoLow, 7, 0, "Mario", 4,
     {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC8, 0x07, 0x00, 0x04, 0x4D, 0x00, 0x61, 0x00, 0x72, 0x00, 0x69, 0x00, 0x6F, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Luigi's Mansion asks one client (what its lobby hands the adapter of the real Download Play: these bytes never go on its wire)
    {"Luigi 1 client, a name", kLuigiLow, 1, 0, "Luigi", 2,
     {0x44, 0x4C, 0x01, 0x01, 0x00, 0x1A, 0x1D, 0x00, 0x02, 0x4C, 0x00, 0x75, 0x00, 0x69, 0x00, 0x67, 0x00, 0x69, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Luigi 7 clients, a name", kLuigiLow, 7, 0, "Luigi", 4,
     {0x44, 0x4C, 0x01, 0x01, 0x00, 0x1A, 0x1D, 0x00, 0x04, 0x4C, 0x00, 0x75, 0x00, 0x69, 0x00, 0x67, 0x00, 0x69, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
};

static void TestAnnouncement() {
    std::printf("A: the announcement\n");
    // the codec, on fields written here
    {
        HostAnnounce a;
        std::memset(&a, 0, sizeof(a));
        a.child_tid_low = 0x0010C000;
        a.max_nodes = 4;
        a.accepting = 1;
        SetName(a.name, "Kirby");
        static const u8 kWant[29] = {0x44, 0x4C, 0x01, 0x01, 0x00, 0xC0, 0x10, 0x00, 0x04, 0x4B, 0x00, 0x69, 0x00, 0x72, 0x00,
                                     0x62, 0x00, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8 out[29 + 4];
        std::memset(out, 0xEE, sizeof(out));
        EncodeAnnounce(a, out);
        CHECK_EQ(Hex(out, 29), Hex(kWant, 29));
        CHECK(out[29] == 0xEE);  // 29 bytes, not one more
        CHECK_EQ(static_cast<u32>(kAnnounceSize), 29u);
        a.accepting = 0;
        a.max_nodes = 2;
        EncodeAnnounce(a, out);
        CHECK(out[3] == 0 && out[8] == 2);
        // what a console of that day reads back, for the sizes a title of four can announce (2, 3, 4), and the two it never takes (0, 1)
        for (u32 nodes = 0; nodes <= 4; ++nodes) {
            u8 in[29];
            std::memcpy(in, kWant, 29);
            in[8] = static_cast<u8>(nodes);
            HostAnnounce got;
            std::memset(&got, 0xAA, sizeof(got));
            const bool ok = DecodeAnnounce(in, 29, &got);
            CHECK_EQ(ok, nodes >= 2);
            if (ok) {
                CHECK_EQ(got.max_nodes, nodes);
                CHECK_EQ(got.child_tid_low, 0x0010C000u);
                CHECK_EQ(got.accepting, 1u);
                CHECK_EQ(NameOf(got.name), std::string("Kirby"));
            }
        }
    }
    // what the lobby puts on the air for what the game asks
    for (const AnnWant& want : kAnnWant) {
        World w;
        Box& h = w.Add("3DS Link", want.tid_low);  // (the name the plugin's hub gives the lobby until the game says another one)
        const Reply init = want.name != nullptr ? h.SvInitName(want.asked, want.child_word, want.name) : h.SvInit(want.asked, want.child_word);
        CHECK_EQ(init.hdr(), want.name != nullptr ? 0x000F0040u : 0x00010040u);
        CHECK_EQ(init.res(), 0u);
        CHECK_EQ(h.SvAccepting(0, 6).res(), 0u);
        CHECK_EQ(h.net.HostOpenCalls(), 1);
        CHECK_EQ(h.net.LastChannel(), 6);
        const std::string got = AnnHex(h.net.LastAnnounce());
        if (got != Hex(want.bytes, 29)) {
            std::printf("   %s: %s\n", want.what, got.c_str());
        }
        CHECK_EQ(got, Hex(want.bytes, 29));
        CHECK_EQ(h.net.LastAnnounce().max_nodes, want.nodes);
        CHECK_EQ(h.net.LastAnnounce().accepting, 1u);
        // the door shut (EndAccepting): the same bytes with the flag down, and nothing else
        CHECK_EQ(h.SvEnd().res(), 0u);
        u8 shut[29];
        std::memcpy(shut, want.bytes, 29);
        shut[3] = 0;
        CHECK_EQ(AnnHex(h.net.LastAnnounce()), Hex(shut, 29));
        CHECK_EQ(h.net.HostUpdateCalls(), 1);
        // the game's GetServerState and an empty GetConnectingClients do not depend on what was asked
        CHECK_EQ(h.SvState().w[2], 1u);
        u32 total = 99;
        CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string(""));
        CHECK_EQ(total, 0u);
    }
}

/// What a searcher of a title of four lists of the lobbies in the air: `max` nodes announced, `total` in. Returns "max/joined" as
/// GetServerInfo gives them, or "-" when the lobby is not listed.
static std::string Listed(u32 max, u32 total, u32 accepting, u32 tid_low) {
    World w;
    Box& g = w.Add("Seeker", kTdLow);
    HostAnnounce a;
    std::memset(&a, 0, sizeof(a));
    a.child_tid_low = tid_low;
    a.max_nodes = static_cast<u8>(max);
    a.accepting = static_cast<u8>(accepting);
    SetName(a.name, "Air");
    const Mac host = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x01};
    w.air.AddAir(host, a, static_cast<u8>(total));
    CHECK_EQ(g.FkInit(16).res(), 0u);
    CHECK_EQ(g.FkScan(0x0004000100000000ULL | kTdLow).res(), 0u);
    g.role = Role::Scan;
    w.Run(100);
    if (g.seen.empty()) {
        return "-";
    }
    const Reply r = g.FkServerInfo(host);
    CHECK_EQ(r.res(), 0u);
    const u8* p = g.mem.Find(g.a_server, 0x298);
    return Fmt("%llu/%llu", p[8], p[9]);
}

static void TestSearcherLists() {
    std::printf("A: what a searcher of a title of four lists\n");
    CHECK_EQ(Listed(4, 1, 1, kTdLow), std::string("4/1"));
    CHECK_EQ(Listed(4, 4, 1, kTdLow), std::string("4/4"));  // (a full lobby is listed: the game hides it, +9 == +8)
    CHECK_EQ(Listed(3, 2, 1, kTdLow), std::string("3/2"));
    CHECK_EQ(Listed(2, 1, 1, kTdLow), std::string("2/1"));
    CHECK_EQ(Listed(4, 1, 0, kTdLow), std::string("-"));    // a host that no longer accepts
    CHECK_EQ(Listed(4, 0, 1, kTdLow), std::string("-"));    // nobody in it
    CHECK_EQ(Listed(4, 5, 1, kTdLow), std::string("-"));    // more in than it holds
    CHECK_EQ(Listed(1, 1, 1, kTdLow), std::string("-"));
    CHECK_EQ(Listed(0, 1, 1, kTdLow), std::string("-"));
    // a lobby that says more than four nodes is never listed by a title of four, however many are in it
    for (u32 max : {5u, 6u, 7u, 8u, 9u, 16u, 255u}) {
        CHECK_EQ(Listed(max, 1, 1, kTdLow), std::string("-"));
        CHECK_EQ(Listed(max, 4, 1, kTdLow), std::string("-"));
    }
    CHECK_EQ(Listed(4, 1, 1, kPrLow), std::string("-"));    // another game's lobby
    // FKCL Initialize's number of beacons is cut to 1..4 lobbies: six in the air, the game reads four (16 asked) or one (0 or 1 asked)
    for (u32 asked : {0u, 1u, 2u, 4u, 16u, 255u}) {
        World w;
        Box& g = w.Add("Seeker", kTdLow);
        for (u8 i = 0; i < 6; ++i) {
            HostAnnounce a;
            std::memset(&a, 0, sizeof(a));
            a.child_tid_low = kTdLow;
            a.max_nodes = 4;
            a.accepting = 1;
            SetName(a.name, "Air");
            w.air.AddAir(Mac{0x40, 0xF4, 0x07, 0xAA, 0xBB, static_cast<u8>(0x10 + i)}, a, 1);
        }
        CHECK_EQ(g.FkInit(asked).res(), 0u);
        CHECK_EQ(g.FkScan(0x0004000100000000ULL | kTdLow).res(), 0u);
        g.role = Role::Scan;
        w.Run(100);
        CHECK_EQ(g.seen.size(), asked < 1 ? 1u : asked > 4 ? 4u : asked);
    }
}

// =====================================================================================================================
// B, C. The sessions.

static const u64 kTdFilter = 0x0004000100000000ULL | kTdLow;

static void StartGuest(Box& g, const char* name_or_null) {
    CHECK_EQ((name_or_null != nullptr ? g.FkInitName(name_or_null, 16) : g.FkInit(16)).res(), 0u);
    CHECK_EQ(g.FkChannels().w[2], 0x0421u);
    CHECK_EQ(g.FkScan(kTdFilter).res(), 0u);
    g.role = Role::Scan;
}

static void S1_FourAndAFifth() {
    std::printf("B/C S1: host + 3 clients, a fifth console, START, GO\n");
    World w;
    Box& h = w.Add("Host", kTdLow);
    Box& c1 = w.Add("Cli1", kTdLow);
    Box& c2 = w.Add("Cli2", kTdLow);
    Box& c3 = w.Add("Cli3", kTdLow);
    Box& f = w.Add("Fifth", kTdLow);
    Box* const guests[3] = {&c1, &c2, &c3};

    CHECK_EQ(h.SvInit(3).res(), 0u);
    CHECK_EQ(h.SvDup().w[2], 0u);
    CHECK_EQ(h.SvAccepting(0, 6).res(), 0u);
    h.role = Role::Host;
    for (Box* g : {&c1, &c2, &c3, &f}) {
        StartGuest(*g, nullptr);
    }
    w.Run(300);
    for (Box* g : {&c1, &c2, &c3, &f}) {
        CHECK_EQ(g->seen.size(), 1u);
        CHECK(!g->seen.empty() && g->seen[0] == h.mac());
    }
    // the three come in, one every 300 ms
    for (u32 i = 0; i < 3; ++i) {
        guests[i]->BeginJoin(h.mac());
        CHECK(guests[i]->role == Role::Joining);
        w.Run(600 + 300 * i);
        CHECK(guests[i]->role == Role::Session);
        CHECK_EQ(guests[i]->join_result, 0u);
        CHECK_EQ(guests[i]->Status(), 0x05010000u | (2 + i));
        u32 total = 0;
        const std::vector<u16> ids = h.Clients(15, 30, &total);
        CHECK_EQ(total, i + 1);
        CHECK_EQ(Ids(ids), std::string(i == 0 ? "2" : i == 1 ? "2,3" : "2,3,4"));
    }

    // ---- C: the answers that depend on the number of players, with the four in ----------------------------------------------------------
    CHECK_EQ(h.SvState().w[2], 2u);
    struct ClientsCase {
        u32 n, bytes;
        const char* ids;
    };
    static const ClientsCase kClients[] = {{15, 30, "2,3,4"}, {0, 30, ""},     {1, 30, "2"},      {2, 30, "2,3"},      {3, 30, "2,3,4"},  {4, 8, "2,3,4"},
                                           {4, 6, "2,3,4"},   {4, 4, "2,3"},   {15, 2, "2"},      {15, 0, ""},         {16, 32, "2,3,4"}, {7, 14, "2,3,4"},
                                           {8, 16, "2,3,4"},  {0xFFFF, 30, "2,3,4"}, {0x10002, 30, "2,3"}};
    for (const ClientsCase& c : kClients) {
        u32 total = 0, res = 1;
        CHECK_EQ(Ids(h.Clients(c.n, c.bytes, &total, false, &res)), std::string(c.ids));
        CHECK_EQ(res, 0u);
        CHECK_EQ(total, 3u);  // the count is the lobby's, whatever the array holds
    }
    static const u32 kClientState[6] = {G_NO_DATA, G_NO_DATA, 0x05010002, 0x05010003, 0x05010004, G_NO_DATA};
    static const char* const kNames[5] = {"", "Host", "Cli1", "Cli2", "Cli3"};
    for (u32 node = 0; node <= 5; ++node) {
        const Reply st = h.SvClientState(node);
        if (node >= 2 && node <= 4) {
            CHECK_EQ(st.hdr(), 0x000D0180u);
            CHECK_EQ(st.res(), 0u);
            CHECK_EQ(st.w[2], kClientState[node]);
        } else {
            CHECK_EQ(st.hdr(), 0x000D0040u);
            CHECK_EQ(st.res(), kClientState[node]);
        }
        const Reply info = h.SvClientInfo(node);
        if (node >= 1 && node <= 4) {
            CHECK_EQ(info.hdr(), 0x000C02C0u);
            CHECK_EQ(info.res(), node == 1 ? G_NO_DATA : 0u);  // (the host's record comes with "no data": Azahar's convention)
            const u8* rec = reinterpret_cast<const u8*>(&info.w[2]);
            CHECK_EQ(LoadLE64(rec), 0xF000u + node - 1);
            CHECK_EQ(NameLE(rec + 0x0C), std::string(kNames[node]));
            CHECK_EQ(LoadLE32(rec + 0x24), node);
        } else {
            CHECK_EQ(info.hdr(), 0x000C0040u);
            CHECK_EQ(info.res(), G_NO_DATA);
        }
    }
    struct NodesCase {
        u32 n, cap;
        const char* ids;
    };
    static const NodesCase kNodes[] = {{16, 32, "1,2,3,4"}, {0, 32, ""},       {1, 32, "1"},        {2, 32, "1,2"},          {3, 32, "1,2,3"},     {4, 8, "1,2,3,4"},
                                       {4, 6, "1,2,3"},     {15, 32, "1,2,3,4"}, {8, 16, "1,2,3,4"}, {0xFFFF, 32, "1,2,3,4"}, {0x10003, 32, "1,2,3"}, {16, 2, "1"}};
    for (Box* g : guests) {
        for (const NodesCase& c : kNodes) {
            u32 total = 0, desc = 0;
            const std::vector<u16> ids = g->Nodes(c.n, c.cap, &total, false, &desc);
            CHECK_EQ(Ids(ids), std::string(c.ids));
            CHECK_EQ(total, 4u);
            CHECK_EQ(desc, (static_cast<u32>(2 * ids.size()) << 14) | 2);  // the descriptor says the bytes written
        }
        for (u32 node = 0; node <= 5; ++node) {
            const Reply info = g->FkNodeInfo(node);
            if (node >= 1 && node <= 4) {
                CHECK_EQ(info.hdr(), 0x000E02C0u);
                CHECK_EQ(info.res(), 0u);
                const u8* rec = reinterpret_cast<const u8*>(&info.w[2]);
                CHECK_EQ(LoadLE64(rec), 0xF000u + node - 1);
                CHECK_EQ(NameLE(rec + 0x0C), std::string(kNames[node]));
                CHECK_EQ(LoadLE32(rec + 0x24), node);
            } else {
                CHECK_EQ(info.hdr(), 0x000E0040u);
                CHECK_EQ(info.res(), G_NO_DATA);
            }
        }
        // the group as the session shows it: four of four
        CHECK_EQ(g->FkServerInfo(h.mac()).res(), 0u);
        const u8* p = g->mem.Find(g->a_server, 0x298);
        CHECK(p[8] == 4 && p[9] == 4);
        CHECK_EQ(LoadLE64(p + 0x290), w.air.ticks());  // (in a session the entry is "heard now")
    }
    // the fifth console still lists the group, full (the game hides a group whose +9 is its +8)
    CHECK_EQ(f.seen.size(), 1u);
    CHECK_EQ(f.FkServerInfo(h.mac()).res(), 0u);
    {
        const u8* p = f.mem.Find(f.a_server, 0x298);
        CHECK(p[8] == 4 && p[9] == 4);
    }

    // ---- the fifth tries to come in: nobody answers it, its join gives up at 3000 ms; nothing changes for the four -------------------
    f.BeginJoin(h.mac());
    CHECK(f.role == Role::Joining);
    w.Run(4195);
    CHECK(f.role == Role::Joining);
    w.Run(4300);
    CHECK(f.role == Role::Idle);
    CHECK_EQ(f.join_result, G_TIMEOUT);
    CHECK_EQ(f.Status(), 0x01000000u);
    {
        u32 total = 0;
        CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2,3,4"));
        CHECK_EQ(total, 3u);
        CHECK_EQ(Ids(c1.Nodes(16, 32, &total)), std::string("1,2,3,4"));
        CHECK_EQ(h.net.Kicked(2) + h.net.Kicked(3) + h.net.Kicked(4) + h.net.Kicked(5), 0);
    }
    CHECK_EQ(f.FkScan(kTdFilter).res(), 0u);
    f.role = Role::Scan;
    w.Run(4800);
    CHECK_EQ(f.seen.size(), 1u);

    // ---- START, GO ------------------------------------------------------------------------------------------------------------------------
    CHECK_EQ(h.SvDistribute().res(), 0u);
    h.pass_on = true;
    w.Run(5400);
    CHECK_EQ(h.SvState().w[2], 9u);
    {
        u32 total = 99;
        CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string(""));
        CHECK_EQ(total, 0u);  // (in 9 the game wants no client left)
    }
    for (u32 i = 0; i < 3; ++i) {
        CHECK_EQ(guests[i]->Status(), 0x09010000u | (2 + i));
        CHECK_EQ(guests[i]->Pass(), std::string("50e4d88c"));  // the FIRST passphrase the host's game sent
        u32 total = 0;
        CHECK_EQ(Ids(guests[i]->Nodes(16, 32, &total)), std::string("1,2,3,4"));  // the roster the GO froze
    }
    CHECK_EQ(f.seen.size(), 0u);  // (the lobby stopped accepting, then closed)

    // ---- the clients leave: Finalize, StopSession, the game's own nwm::UDS; the host's Finalize ------------------------------------------
    CHECK_EQ(c1.FkFinalize().res(), 0u);
    c1.role = Role::Idle;
    CHECK_EQ(c2.FkStopSession().res(), 0u);
    c2.role = Role::Idle;
    c3.GameUds();
    CHECK_EQ(c3.Status(), 0x09010004u);  // (9 stays 9 with its passphrase)
    CHECK_EQ(h.SvFinalize().res(), 0u);
    h.role = Role::Idle;
    w.Run(5600);
    CHECK_EQ(f.FkStopScan().res(), 0u);
    CHECK_EQ(f.FkFinalize().res(), 0u);
    f.role = Role::Idle;
    w.Run(6000);
    CHECK_EQ(c1.Status(), 0x01000000u);
    CHECK_EQ(c2.Status(), 0x01000000u);
    CHECK_EQ(h.SvState().w[2], 0u);
    for (auto& b : w.box) {
        CHECK(!b->lobby.active());
    }
    CHECK_EQ(w.air.overlaps(), 0);

    // ---- the record ----------------------------------------------------------------------------------------------------------------------
    const Trace& t = w.wire;
    CHECK_EQ(t.Count(" HostOpen "), 1u);
    CHECK_EQ(t.Count(" HostUpdate "), 1u);
    CHECK_EQ(t.Count(" HostClose"), 1u);
    CHECK_EQ(t.Count(" Kick "), 0u);
    CHECK_EQ(t.Count(" ScanOpen "), 5u);
    CHECK_EQ(t.Count(" ScanClose"), 5u);
    CHECK_EQ(t.Count(" ConnectBegin "), 4u);
    CHECK_EQ(t.Count(" Disconnect"), 4u);
    CHECK_EQ(t.Count(" Send "), 12u);
    for (u32 node = 2; node <= 4; ++node) {
        CHECK_EQ(t.Count(Fmt("c0 Send to=%llu len=2 0100 -> 1", node)), 1u);                    // START
        CHECK_EQ(t.Count(Fmt("c0 Send to=%llu len=11 0309353065346438386300 -> 1", node)), 1u);  // GO with "50e4d88c"
        CHECK_EQ(t.Count(Fmt("c%llu Send to=1 len=2 0200 -> 1", node - 1)), 1u);                 // START_ACK
        CHECK_EQ(t.Count(Fmt("c%llu Send to=1 len=2 0400 -> 1", node - 1)), 1u);                 // GO_ACK
    }
    CHECK_EQ(t.Count("c4 Send "), 0u);  // the fifth console never said a word
    Hold("S1-wire", w.wire, "c20b58e3721418134b9f6843ade467c45db733b3", 33);
    Hold("S1-ipc", w.ipc, "2b9943a7ef6c068ecb3428fd1054ff0a6193446f", 217);
    HoldReads("S1-reads", w.Reads(),
              "c0 members=246 read_node=852 receive=252 scan_read=0 connect_poll=0 my_node=246\n"
              "c1 members=229 read_node=71 receive=273 scan_read=32 connect_poll=1 my_node=228\n"
              "c2 members=214 read_node=70 receive=273 scan_read=62 connect_poll=1 my_node=213\n"
              "c3 members=199 read_node=68 receive=273 scan_read=92 connect_poll=1 my_node=198\n"
              "c4 members=0 read_node=0 receive=276 scan_read=217 connect_poll=150 my_node=0\n");
}

static void S2_ManualAcceptance() {
    std::printf("B S2: manual acceptance, a seat taken again, DisconnectClient, the ejections of StartDistribution\n");
    World w;
    Box& h = w.Add("3DS Link", kTdLow);
    Box& c1 = w.Add("3DS Link", kTdLow);
    Box& c2 = w.Add("3DS Link", kTdLow);
    Box& c3 = w.Add("3DS Link", kTdLow);
    Box& f = w.Add("3DS Link", kTdLow);

    CHECK_EQ(h.SvInitName(3, 0, "BrHost").res(), 0u);
    CHECK_EQ(h.SvAccepting(1, 1).res(), 0u);  // manual acceptance, channel 1
    h.role = Role::Host;
    StartGuest(c1, "BrCli1");
    StartGuest(c2, "BrCli2");
    StartGuest(c3, "BrCli3");
    StartGuest(f, "BrCli4");
    w.Run(200);
    for (Box* g : {&c1, &c2, &c3}) {
        g->BeginJoin(h.mac());
    }
    w.Run(600);
    u32 total = 0;
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2,3,4"));  // a client that waits for the acceptance is in the list
    for (u32 node = 2; node <= 4; ++node) {
        CHECK_EQ(h.SvClientState(node).w[2], 0x40000000u | node);  // waiting, not joined
    }
    CHECK_EQ(h.SvAccept(2).res(), 0u);
    CHECK_EQ(h.SvAccept(3).res(), 0u);
    CHECK_EQ(h.SvAccept(2).res(), G_NO_DATA);  // accepted already
    CHECK_EQ(h.SvAccept(1).res(), G_NO_DATA);  // the host
    CHECK_EQ(h.SvAccept(5).res(), G_NO_DATA);  // nobody
    CHECK_EQ(h.SvAccept(9).res(), G_NO_DATA);
    CHECK_EQ(h.SvClientState(2).w[2], 0x05010002u);
    CHECK_EQ(h.SvClientState(4).w[2], 0x40000004u);  // (node 4 is never accepted)
    w.Run(800);
    // node 3 leaves; the rosters of the others follow; the fifth console takes its seat (the lowest free id)
    CHECK_EQ(c2.FkStopSession().res(), 0u);
    c2.role = Role::Idle;
    w.Run(1100);
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2,4"));
    CHECK_EQ(total, 2u);
    CHECK_EQ(Ids(c1.Nodes(16, 32, &total)), std::string("1,2,4"));
    CHECK_EQ(total, 3u);
    f.BeginJoin(h.mac());
    w.Run(1400);
    CHECK(f.role == Role::Session);
    CHECK_EQ(f.Status(), 0x05010003u);
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2,3,4"));
    CHECK_EQ(h.SvClientState(3).w[2], 0x40000003u);
    CHECK_EQ(h.SvAccept(3).res(), 0u);
    // DisconnectClient
    CHECK_EQ(h.SvDisconnect(2).res(), 0u);
    CHECK_EQ(h.SvDisconnect(2).res(), G_NO_DATA);  // ejected: no longer a client
    CHECK_EQ(h.SvDisconnect(1).res(), G_NO_DATA);
    CHECK_EQ(h.SvDisconnect(7).res(), G_NO_DATA);
    w.Run(1600);
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("3,4"));
    // the door shut and opened again
    CHECK_EQ(h.SvEnd().res(), 0u);
    CHECK_EQ(h.SvState().w[2], 1u);
    CHECK_EQ(h.SvAccepting(1, 1).res(), 0u);
    CHECK_EQ(h.SvState().w[2], 2u);
    w.Run(1800);
    CHECK_EQ(Ids(f.Nodes(16, 32, &total)), std::string("1,3,4"));  // (a searcher reads its roster again every 250 ms)
    // StartDistribution: node 4 (never accepted) is ejected at once; node 3 never answers START and is ejected at 1500 ms
    w.air.Drop(2, 3, 1, FakeAir::kForever);
    CHECK_EQ(h.SvDistribute().res(), 0u);
    h.pass_on = true;
    CHECK_EQ(h.SvState().w[2], 6u);
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("3"));
    CHECK_EQ(h.SvClientState(3).w[2], 0x06010003u);
    CHECK_EQ(h.SvClientState(4).res(), G_NO_DATA);
    w.Run(3295);
    CHECK_EQ(h.SvState().w[2], 6u);
    w.Run(3400);
    CHECK_EQ(h.SvState().w[2], 9u);  // (nobody left to send the passphrase to: 8 and 9 in one frame)
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string(""));
    w.Run(6000);
    // the clients the host threw out see the host go (0x80, the error of the games), 2000 ms after
    CHECK_EQ(c1.Status(), 0x80000000u);
    CHECK_EQ(c3.Status(), 0x80000000u);
    CHECK_EQ(f.Status(), 0x80000000u);
    CHECK_EQ(c2.Status(), 0x01000000u);
    for (Box* g : {&c1, &c2, &c3, &f}) {
        CHECK_EQ(g->FkFinalize().res(), 0u);
        g->role = Role::Idle;
    }
    CHECK_EQ(h.SvFinalize().res(), 0u);
    h.role = Role::Idle;
    w.Run(6200);
    for (auto& b : w.box) {
        CHECK(!b->lobby.active());
    }

    const Trace& t = w.wire;
    CHECK_EQ(t.Count(" HostOpen "), 1u);
    CHECK_EQ(t.Count(" HostUpdate "), 3u);  // EndAccepting, StartAccepting again, StartDistribution
    CHECK_EQ(t.Count(" HostClose"), 1u);
    CHECK_EQ(t.Count("c0 Kick 2"), 1u);  // DisconnectClient
    CHECK_EQ(t.Count("c0 Kick 4"), 1u);  // never accepted
    CHECK_EQ(t.Count("c0 Kick 3"), 1u);  // no START_ACK in 1500 ms
    CHECK_EQ(t.Count(" Kick "), 3u);
    CHECK_EQ(t.Count("c0 Send to=3 len=2 0100 -> 1"), 8u);  // START at 0, 200, ..., 1400 ms
    CHECK_EQ(t.Count("c0 Send "), 8u);                      // no START to the others, no GO to anybody
    Hold("S2-wire", w.wire, "bdcf83257d921bd62e8edd680758e28ae7f82cfe", 40);
    Hold("S2-ipc", w.ipc, "ca3f2e2ca4d0ba9ad3be6e74d34443b09eba415d", 151);
    HoldReads("S2-reads", w.Reads(),
              "c0 members=173 read_node=370 receive=173 scan_read=0 connect_poll=0 my_node=173\n"
              "c1 members=162 read_node=19 receive=172 scan_read=22 connect_poll=1 my_node=162\n"
              "c2 members=31 read_node=12 receive=41 scan_read=22 connect_poll=1 my_node=31\n"
              "c3 members=182 read_node=25 receive=192 scan_read=22 connect_poll=1 my_node=182\n"
              "c4 members=211 read_node=23 receive=274 scan_read=112 connect_poll=1 my_node=211\n");
}

static void S3_LobbyOfTwo() {
    std::printf("B S3: a lobby of two, a third console, a node above the size of the title\n");
    World w;
    Box& h = w.Add("Host", kTdLow);
    Box& c1 = w.Add("Cli1", kTdLow);
    Box& f = w.Add("Third", kTdLow);
    CHECK_EQ(h.SvInit(1).res(), 0u);  // one client: what Luigi's Mansion asks
    CHECK_EQ(h.SvAccepting(1, 6).res(), 0u);
    h.role = Role::Host;
    StartGuest(c1, nullptr);
    StartGuest(f, nullptr);
    w.Run(200);
    c1.BeginJoin(h.mac());
    w.Run(500);
    CHECK_EQ(c1.Status(), 0x05010002u);
    CHECK_EQ(h.SvAccept(2).res(), 0u);
    CHECK_EQ(f.FkServerInfo(h.mac()).res(), 0u);
    {
        const u8* p = f.mem.Find(f.a_server, 0x298);
        CHECK(p[8] == 2 && p[9] == 2);
    }
    f.BeginJoin(h.mac());  // never answered
    w.Run(700);
    // a node 3 the engine should never have let in (the lobby of this game holds two): ejected when seen, again every 200 ms while it lingers
    w.air.SetKickLag(500);
    w.air.Phantom(0, 3, 0xBAD3, "Ghost");
    w.Run(1500);
    u32 total = 0;
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2"));
    CHECK_EQ(total, 1u);
    CHECK_EQ(w.wire.Count("c0 Kick 3"), 3u);
    CHECK_EQ(w.wire.Count(" Kick "), 3u);
    w.Run(3600);
    CHECK(f.role == Role::Idle);
    CHECK_EQ(f.join_result, G_TIMEOUT);
    CHECK_EQ(h.SvDistribute().res(), 0u);
    h.pass_on = true;
    w.Run(4200);
    CHECK_EQ(h.SvState().w[2], 9u);
    CHECK_EQ(c1.Status(), 0x09010002u);
    CHECK_EQ(c1.Pass(), std::string("50e4d88c"));
    CHECK_EQ(Ids(c1.Nodes(16, 32, &total)), std::string("1,2"));
    CHECK_EQ(c1.FkFinalize().res(), 0u);
    c1.role = Role::Idle;
    CHECK_EQ(f.FkFinalize().res(), 0u);
    CHECK_EQ(h.SvFinalize().res(), 0u);
    h.role = Role::Idle;
    w.Run(4400);
    CHECK_EQ(w.wire.Count(" HostOpen ch=6 max=2 acc=1 "), 1u);
    CHECK_EQ(w.wire.Count(" Send "), 4u);  // START, START_ACK, GO, GO_ACK
    Hold("S3-wire", w.wire, "b078835547e144e34e49368e13158393fbcab4e1", 18);
    Hold("S3-ipc", w.ipc, "f12f0c101f7a99882e0b3de6edd21e21e6dcc36d", 57);
    HoldReads("S3-reads", w.Reads(),
              "c0 members=186 read_node=230 receive=188 scan_read=0 connect_poll=0 my_node=186\n"
              "c1 members=174 read_node=32 receive=213 scan_read=22 connect_poll=1 my_node=173\n"
              "c2 members=0 read_node=0 receive=175 scan_read=52 connect_poll=150 my_node=0\n");
}

static void S4_NodeFive() {
    std::printf("B S4: a faulty engine lists a node 5\n");
    World w;
    Box& h = w.Add("Host", kTdLow);
    Box& c1 = w.Add("Cli1", kTdLow);
    CHECK_EQ(h.SvInit(3).res(), 0u);
    CHECK_EQ(h.SvAccepting(0, 6).res(), 0u);
    h.role = Role::Host;
    StartGuest(c1, nullptr);
    w.Run(200);
    c1.BeginJoin(h.mac());
    w.Run(500);
    w.air.SetKickLag(500);
    w.air.Phantom(0, 5, 0xBAD5, "Ghost5");
    w.Run(1500);
    u32 total = 0;
    CHECK_EQ(Ids(h.Clients(15, 30, &total)), std::string("2"));  // never a client
    CHECK_EQ(h.SvClientState(5).res(), G_NO_DATA);
    CHECK_EQ(w.wire.Count("c0 Kick 5"), 3u);  // when seen, and every 200 ms until the engine drops it (500 ms)
    CHECK_EQ(w.wire.Count(" Kick "), 3u);
    CHECK_EQ(h.SvDistribute().res(), 0u);
    h.pass_on = true;
    w.Run(2100);
    CHECK_EQ(h.SvState().w[2], 9u);
    CHECK_EQ(c1.Status(), 0x09010002u);
    CHECK_EQ(w.wire.Count(" Send "), 4u);
    Hold("S4-wire", w.wire, "53470490b4692970af20673986400b962a9d8263", 13);
    Hold("S4-ipc", w.ipc, "a0441cdcfd81f2f018cf8a37f842ff4827256330", 39);
    HoldReads("S4-reads", w.Reads(),
              "c0 members=81 read_node=90 receive=83 scan_read=0 connect_poll=0 my_node=81\n"
              "c1 members=69 read_node=16 receive=108 scan_read=22 connect_poll=1 my_node=68\n");
}

// =====================================================================================================================
// D, E. The real engine (real time: nothing below is held by a count of frames or by a time, only by what the bytes are).

static void SleepMs(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
template <typename P, typename F>
static bool PumpUntil(P& p, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        p.Pump();
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        SleepMs(2);
    }
}
/// Runs `f` (a call of a model that blocks) while `p` is pumped.
template <typename P, typename F>
static void Pumping(P& p, F f) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        f();
        done = true;
    });
    while (!done) {
        p.Pump();
        SleepMs(2);
    }
    t.join();
}

/// What a console put on the wire as UDS data: the payload of its SecureData frames, with the channel and the node it went to.
class Sniff {
public:
    struct Sent {
        u8 channel;
        u16 dest;
        std::vector<u8> p;
    };
    explicit Sniff(Console& c) {
        c.sniff = [this](const WifiPacket& w) {
            EtherType t;
            SecureDataHeader h;
            if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData || !ParseSecureDataHeader(w.data, &h) ||
                h.ActualDataSize() > w.data.size()) {
                return;
            }
            Sent s;
            s.channel = h.data_channel;
            s.dest = h.dest_node_id;
            s.p.assign(w.data.end() - h.ActualDataSize(), w.data.end());
            std::lock_guard<std::mutex> l(m_);
            v_.push_back(std::move(s));
        };
    }
    std::vector<Sent> All() {
        std::lock_guard<std::mutex> l(m_);
        return v_;
    }
    /// The different payloads of `channel`, each as "node:hex", sorted, joined by spaces.
    std::string Kinds(u8 channel) {
        std::vector<std::string> kinds;
        for (const Sent& s : All()) {
            if (s.channel == channel) {
                const std::string k = Fmt("%llu:", s.dest) + Hex(s.p.data(), s.p.size());
                if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) {
                    kinds.push_back(k);
                }
            }
        }
        std::sort(kinds.begin(), kinds.end());
        std::string out;
        for (const std::string& k : kinds) {
            out += (out.empty() ? "" : " ") + k;
        }
        return out;
    }

private:
    std::mutex m_;
    std::vector<Sent> v_;
};

/// The answers a game reads of a lobby that is not behind a hub (the consoles of the rig): the IPC words, through a service of its own.
struct Ipc {
    explicit Ipc(DlpLobby* lobby) {
        svc.SetLobby(lobby);
        a = mem.Alloc(64);
    }
    std::string IdsAt() {
        const u8* p = mem.Find(a, 64);
        std::string s;
        for (u32 i = 0; i < 32 && p[2 * i] != 0xCC; ++i) {
            s += Fmt(s.empty() ? "%llu" : ",%llu", LoadLE16(p + 2 * i));
        }
        return s;
    }
    /// GetConnectingClients (15 elements, 30 bytes): "total:ids".
    std::string Clients() {
        u32 cmd[16] = {0x000B0042, 15, (30u << 4) | 0xC, a};
        mem.Refill(a);
        svc.Dispatch(kSv, cmd, nullptr, mem.Resolver());
        return cmd[1] != 0 ? Fmt("error %llx", cmd[1]) : Fmt("%llu:", cmd[2]) + IdsAt();
    }
    /// GetConnectingNodes (16 elements, a static buffer of 32 bytes): "total:ids".
    std::string Nodes() {
        const u32 sb[2] = {(32u << 14) | 2, a};
        u32 cmd[16] = {0x000D0040, 16};
        mem.Refill(a);
        svc.Dispatch(kFk, cmd, sb, mem.Resolver());
        return cmd[1] != 0 ? Fmt("error %llx", cmd[1]) : Fmt("%llu:", cmd[2]) + IdsAt();
    }
    /// The word [2] of a command with one argument (GetServerState, GetClientState, GetMyStatus), or its error.
    u32 Word(DlpKind kind, u32 header, u32 arg = 0) {
        u32 cmd[16] = {header, arg};
        svc.Dispatch(kind, cmd, nullptr, mem.Resolver());
        return cmd[1] != 0 ? cmd[1] : cmd[2];
    }
    DlpService svc;
    GameMem mem;
    u32 a;
};

/// The lobby of `host` as the searcher's table has it ("max N, in N, child X, name"), or "-" when it is not there.
static std::string GroupOf(DlpLobby& lobby, const Mac& host) {
    GroupView v;
    if (lobby.ClientServerInfo(host, &v) != 0) {
        return "-";
    }
    return Fmt("max %llu, in %llu, child %llx, ", v.group->max_nodes, v.group->total_nodes, v.group->child_tid_low) + NameOf(v.group->name);
}

static const u8 kGoldenPass[9] = {'5', '0', 'e', '4', 'd', '8', '8', 'c', 0};  // the passphrase of the capture
static const u8 kGoldenRid[2] = {0x01, 0x02};                                 // the client's id of the capture

static const DlpRealGolden& Golden(const char* id) {
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        if (std::strcmp(g.id, id) == 0) {
            return g;
        }
    }
    std::fprintf(stderr, "no golden %s\n", id);
    std::abort();
}
/// `p` is a packet sealed with our seed: with the capture's seed, is it the golden packet, byte for byte?
static bool IsGolden(std::vector<u8> p, const DlpRealGolden& g) {
    if (p.size() != g.size) {
        return false;
    }
    dlp_real::Seal(kDlpRealLuigiSeed, p.data(), p.size());
    return std::memcmp(p.data(), g.bytes, g.size) == 0;
}
/// `p` is frame `i` of OUR announcement (no title, no icon: zeros there): with the capture's zone put in and the capture's seed, is it
/// the golden frame, byte for byte? (As Tests/dlp_real_wire_test.cpp W2, W3.)
static bool IsGoldenFrame(std::vector<u8> p, unsigned i) {
    const DlpRealGolden& g = kDlpRealLuigiAnnounce[i];
    if (p.size() != g.size) {
        return false;
    }
    if (i == 0) {
        std::memcpy(p.data() + 0x38, g.bytes + 0x38, 0x2F8 - 0x38);
    } else if (i < 4) {
        std::memcpy(p.data() + 0x10, g.bytes + 0x10, g.size - 0x10);
    }
    dlp_real::Seal(kDlpRealLuigiSeed, p.data(), p.size());
    return std::memcmp(p.data(), g.bytes, g.size) == 0;
}

static void D1_Codec() {
    std::printf("D1: the codec of the real Download Play against the golden bytes\n");
    u8 buf[dlp_real::kBufferBytes];
    // the announcement of a lobby of two (Luigi's Mansion): the lengths, the golden bytes, the checksums of what we send (seed 0)
    static const std::size_t kLen[5] = {768, 1448, 1448, 1448, 1464};
    static const u32 kSum[5] = {0x3e3efede, 0xafafaf02, 0xafafaf02, 0xafafaf02, 0xbfbfbf02};
    for (unsigned i = 0; i < 5; ++i) {
        const std::size_t n = dlp_real::AnnounceFrame(buf, i, kLuigiChild, 2, dlp_real::kTxSeed);
        CHECK_EQ(n, kLen[i]);
        CHECK(IsGoldenFrame(std::vector<u8>(buf, buf + n), i));
        if (LoadBE32(buf + 8) != kSum[i]) {
            std::printf("   frame %u: checksum %08x\n", i, LoadBE32(buf + 8));
        }
        CHECK_EQ(LoadBE32(buf + 8), kSum[i]);
    }
    CHECK_EQ(dlp_real::AnnounceFrame(buf, 0, kLuigiChild, 2, dlp_real::kTxSeed), 768u);
    CHECK_EQ(buf[0x20], 2u);
    CHECK_EQ(dlp_real::AnnounceMaxNodes(buf), 2u);
    CHECK_EQ(dlp_real::AnnounceChildTid(buf), kLuigiChild);
    // the dialogue: what the host sends (A1, A3, A5, A7, A8) and what the fake client answers (A2, A4, A6, A9)
    CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::Auth(buf, dlp_real::kTxSeed)), Golden("A1")));
    CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::StartDist(buf, dlp_real::kTxSeed, 1, kGoldenRid)), Golden("A3")));
    CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::Finish(buf, dlp_real::kTxSeed, 2, kGoldenRid)), Golden("A5")));
    CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::Final(buf, dlp_real::kTxSeed, 3, kGoldenRid, kGoldenPass)), Golden("A7")));
    CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::Notice(buf, dlp_real::kTxSeed, 3, kGoldenRid)), Golden("A8")));
    static const char* const kPairs[4][2] = {{"A1", "A2"}, {"A3", "A4"}, {"A5", "A6"}, {"A8", "A9"}};
    for (const auto& pair : kPairs) {
        const DlpRealGolden& from = Golden(pair[0]);
        const dlp_real::Packet got = dlp_real::ParseFromHost(from.bytes, from.size);
        CHECK(got.why == dlp_real::Why::Ok);
        CHECK(IsGolden(std::vector<u8>(buf, buf + dlp_real::Reply(buf, dlp_real::kTxSeed, got)), Golden(pair[1])));
    }
    CHECK_EQ(Golden("A1").size, 0x14u);
    CHECK_EQ(Golden("A7").size, 0x20u);
    CHECK(std::memcmp(Golden("A7").bytes + 0x14, kGoldenPass, 9) == 0);
}

/// A console of Luigi's Mansion: the adapter of the real Download Play, the lobby on it (the plugin's hub pumps the adapter, then the lobby).
struct RealBox {
    RealBox(MockRoom& room, u8 last, const char* name) : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(kLuigiLow, last, name)), ipc(&lobby) {
        net.SetIdentity(Me(0xF000u + last, name));
    }
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    Console con;
    RealDlpNet net;
    DlpLobby lobby;
    Ipc ipc;
};

static void D2_RealHost() {
    std::printf("D2: Luigi's host on the real Download Play, in front of the model of Azahar's client\n");
    g_rules.Reset();
    MockRoom room;
    RealBox h(room, 1, "Host");
    AzaharDlpClient m(room, 2, "Azahar");
    Sniff wire(h.con);
    Ticker ticker({&h.e(), &m.e()});
    // Luigi's Mansion: one client, manual acceptance, channel 6
    CHECK_EQ(h.lobby.ServerInitialize(1, 0, nullptr), 0u);
    CHECK_EQ(h.lobby.ServerStartAccepting(true, 6), 0u);
    CHECK(h.net.Holds());
    // the beacon: a network of two, no application data
    CHECK(PumpUntil(h, 3000, [&] { return m.Heard(h.mac()); }));
    {
        UdsEngine::HeardLobby heard;
        bool found = false;
        for (u32 i = 0; m.e().HeardLobbyAt(i, 1000, &heard); ++i) {
            if (heard.host == h.mac()) {
                found = true;
                CHECK_EQ(heard.max_nodes, 2u);
                CHECK_EQ(heard.total_nodes, 1u);
                CHECK_EQ(heard.size, 0u);
                CHECK_EQ(heard.channel, 6u);
            }
        }
        CHECK(found);
    }
    // the announcement, read by Azahar's client as a spectator: the 5 golden frames
    AzaharDlpClient::Probe pr;
    Pumping(h, [&] { pr = m.ProbeHost(h.mac()); });
    CHECK(pr.connected && pr.all && !pr.ignored);
    CHECK_EQ(pr.child_tid, kLuigiChild);
    CHECK_EQ(pr.max_nodes, 2u);
    CHECK_EQ(NameOf(pr.name), std::string("Host"));
    CHECK_EQ(h.ipc.Clients(), std::string("0:"));  // (a spectator is no client)
    {
        u32 frames = 0, golden = 0, ours = 0;
        u8 buf[dlp_real::kBufferBytes];
        for (const Sniff::Sent& s : wire.All()) {
            if (s.channel != 1) {
                continue;
            }
            ++frames;
            CHECK_EQ(s.dest, 0xFFFFu);
            const dlp_real::Packet p = dlp_real::ParseAnnounce(s.p.data(), s.p.size());
            CHECK(p.kind == dlp_real::Kind::Announce);
            if (p.kind != dlp_real::Kind::Announce) {
                continue;
            }
            golden += IsGoldenFrame(s.p, p.index);
            const std::size_t n = dlp_real::AnnounceFrame(buf, p.index, kLuigiChild, 2, dlp_real::kTxSeed);
            ours += n == s.p.size() && std::memcmp(buf, s.p.data(), n) == 0;
        }
        CHECK(frames >= 5);
        CHECK_EQ(golden, frames);  // every frame the host sent is the golden one (but the game's titles and icon, which ours does not carry)
        CHECK_EQ(ours, frames);    // ... and the codec's frame of a lobby of two, byte for byte
    }
    CHECK(PumpUntil(h, 1000, [&] { return h.e().SpectatorCount() == 0; }));
    // the session: the Auth, the acceptance, StartDistribution, the game's BeginGame every 33 ms with a new passphrase
    bool joined = false;
    Pumping(h, [&] { joined = m.StartSession(h.mac()); });
    CHECK(joined);
    CHECK(PumpUntil(h, 2000, [&] { return h.ipc.Clients() == "1:2"; }));
    CHECK(PumpUntil(h, 4000, [&] { return h.net.PeerState(2) == RealDlpNet::kAuthed; }));
    CHECK_EQ(h.ipc.Word(kSv, 0x000D0040, 2), 0x40000002u);  // waiting for the acceptance
    CHECK_EQ(h.ipc.Word(kSv, 0x00030000), 2u);
    CHECK_EQ(h.lobby.ServerAcceptClient(2), 0u);
    CHECK_EQ(h.lobby.ServerStartDistribution(), 0u);
    CHECK(PumpUntil(h, 3000, [&] { return h.lobby.server_state() == 8; }));
    {
        const u64 t0 = platform::NowMs();
        u64 next = t0;
        for (u32 k = 0; h.lobby.server_state() == 8 && platform::NowMs() - t0 < 4000;) {
            if (platform::NowMs() >= next) {
                u8 nine[9];
                std::memcpy(nine, kGoldenPass, 9);
                nine[7] = static_cast<u8>(nine[7] + k++);  // the first is the capture's, the others are not
                h.lobby.ServerSendPassphrase(nine);
                next += 33;
            }
            h.Pump();
            SleepMs(2);
        }
    }
    CHECK_EQ(h.ipc.Word(kSv, 0x00030000), 9u);
    CHECK_EQ(h.ipc.Clients(), std::string("0:"));
    CHECK(!h.net.Holds());
    CHECK(PumpUntil(h, 1000, [&] { return m.state() == AzaharDlpClient::kComplete; }));
    CHECK(m.passphrase() == std::vector<u8>(kGoldenPass, kGoldenPass + 9));
    {
        // every packet of the dialogue the host sent is the golden one of its kind
        u32 kinds[11] = {};
        for (const Sniff::Sent& s : wire.All()) {
            if (s.channel != 2) {
                continue;
            }
            CHECK_EQ(s.dest, 2u);
            const dlp_real::Packet p = dlp_real::ParseFromHost(s.p.data(), s.p.size());
            const char* id = p.kind == dlp_real::Kind::Auth        ? "A1"
                             : p.kind == dlp_real::Kind::StartDist ? "A3"
                             : p.kind == dlp_real::Kind::Finish    ? "A5"
                             : p.kind == dlp_real::Kind::Final     ? "A7"
                             : p.kind == dlp_real::Kind::Notice    ? "A8"
                                                                   : nullptr;
            CHECK(id != nullptr);
            if (id != nullptr) {
                ++kinds[static_cast<u8>(p.kind)];
                if (!IsGolden(s.p, Golden(id))) {
                    std::printf("   %s differs: %s\n", id, Hex(s.p.data(), s.p.size()).c_str());
                }
                CHECK(IsGolden(s.p, Golden(id)));
            }
        }
        for (dlp_real::Kind k : {dlp_real::Kind::Auth, dlp_real::Kind::StartDist, dlp_real::Kind::Finish, dlp_real::Kind::Final}) {
            CHECK(kinds[static_cast<u8>(k)] >= 1);
        }
    }
    CHECK_EQ(h.net.counters().bad, 0u);
    m.StopSession();
    CHECK(m.violations().empty());
    CHECK(!m.session_broken);
    CHECK_RULES();
}

static void D3_RealSearcher() {
    std::printf("D3: Luigi's searcher on the real Download Play, in front of the model of Azahar's host\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "AzHost", kLuigiChild, 1, 1);
    RealBox c(room, 2, "Client");
    Sniff wire(c.con);
    Ticker ticker({&h.e(), &c.e()});
    h.StartHosting(true, 6);
    CHECK_EQ(c.lobby.ClientInitialize(nullptr, 2), 0u);
    CHECK_EQ(c.lobby.ClientStartScan(kLuigiChild), 0u);
    CHECK(PumpUntil(c, 4000, [&] { return GroupOf(c.lobby, h.mac()) != "-"; }));
    CHECK_EQ(GroupOf(c.lobby, h.mac()), std::string("max 2, in 1, child 1d1a00, AzHost"));
    CHECK(PumpUntil(c, 500, [&] { return !c.net.Probing(); }));
    // the host's game: AcceptClient when the client is in, StartDistribution, BeginGame with the capture's passphrase
    std::atomic<bool> stop{false};
    std::thread game([&] {
        while (!stop && h.client_state(2) == AzaharDlpHost::NotJoined) {
            SleepMs(2);
        }
        SleepMs(300);
        if (stop || h.AcceptClient(2) != 0) {
            return;
        }
        while (!stop && h.client_state(2) != AzaharDlpHost::Accepted) {
            SleepMs(2);
        }
        if (stop) {
            return;
        }
        h.StartDistribution();
        while (!stop && h.state() != AzaharDlpHost::kNeedToSendPassphrase) {
            SleepMs(2);
        }
        const u64 t0 = platform::NowMs();
        for (u32 k = 0; !stop && h.state() == AzaharDlpHost::kNeedToSendPassphrase && !h.ended() && platform::NowMs() - t0 < 4000; ++k) {
            u8 nine[9];
            std::memcpy(nine, kGoldenPass, 9);
            nine[7] = static_cast<u8>(nine[7] + k);
            h.BeginGame(nine);
            SleepMs(33);
        }
    });
    c.lobby.ClientStopScan();
    u32 res = 0xFFFFFFFFu;
    if (c.lobby.ClientJoinBegin(h.mac(), &res) == JoinStep::Pending) {
        PumpUntil(c, 5000, [&] { return c.lobby.ClientJoinPoll(&res); });
    }
    CHECK_EQ(res, 0u);
    CHECK_EQ(c.ipc.Word(kFk, 0x000C0000), 0x05010002u);
    CHECK(PumpUntil(c, 2000, [&] { return c.ipc.Nodes() == "2:1,2"; }));
    CHECK(PumpUntil(c, 8000, [&] { return (c.lobby.ClientStatusWord() >> 24) == 9; }));
    CHECK_EQ(c.ipc.Word(kFk, 0x000C0000), 0x09010002u);
    CHECK_EQ(c.ipc.Nodes(), std::string("2:1,2"));
    u8 pass[9] = {};
    CHECK_EQ(c.lobby.ClientPassphrase(pass), 0u);
    CHECK(std::memcmp(pass, kGoldenPass, 9) == 0);
    PumpUntil(c, 1500, [&] { return h.ended(); });
    stop = true;
    game.join();
    {
        // every answer our searcher sent is the golden one of its kind
        u32 kinds[11] = {};
        for (const Sniff::Sent& s : wire.All()) {
            if (s.channel != 2) {
                continue;
            }
            CHECK_EQ(s.dest, 1u);
            const dlp_real::Packet p = dlp_real::ParseFromClient(s.p.data(), s.p.size());
            const char* id = p.kind == dlp_real::Kind::AuthAck     ? "A2"
                             : p.kind == dlp_real::Kind::DistAck   ? "A4"
                             : p.kind == dlp_real::Kind::FinishAck ? "A6"
                             : p.kind == dlp_real::Kind::NoticeAck ? "A9"
                                                                   : nullptr;
            CHECK(id != nullptr);
            if (id != nullptr) {
                ++kinds[static_cast<u8>(p.kind)];
                if (!IsGolden(s.p, Golden(id))) {
                    std::printf("   %s differs: %s\n", id, Hex(s.p.data(), s.p.size()).c_str());
                }
                CHECK(IsGolden(s.p, Golden(id)));
            }
        }
        for (dlp_real::Kind k : {dlp_real::Kind::AuthAck, dlp_real::Kind::DistAck, dlp_real::Kind::FinishAck}) {
            CHECK(kinds[static_cast<u8>(k)] >= 1);
        }
    }
    CHECK_EQ(c.net.counters().host_bad, 0u);
    CHECK_EQ(c.lobby.ClientFinalize(), 0u);
    CHECK(!c.net.Holds());
    CHECK(h.violations().empty());
    CHECK_RULES();
}

static const AnnWant& AnnOf(const char* what) {
    for (const AnnWant& a : kAnnWant) {
        if (std::strcmp(a.what, what) == 0) {
            return a;
        }
    }
    std::fprintf(stderr, "no announcement %s\n", what);
    std::abort();
}

static void E1_BeaconBytes() {
    std::printf("E1: the announcement as another console hears it in the beacon (the dialect on the real engine)\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "3DS Link", kTdLow);
    Console ear(room, 9);
    CHECK(ear.engine->PrepareLobby());
    Ticker ticker({&a.e(), ear.engine.get()});
    struct Case {
        u32 asked;
        const char* want;
    };
    static const Case kCases[] = {{3, "TD 3 clients"}, {1, "TD 1 client"}, {7, "TD 7 clients"}, {0, "TD 0 clients"}, {255, "TD 255 clients"}, {2, "TD 2 clients"}};
    for (const Case& c : kCases) {
        const AnnWant& want = AnnOf(c.want);
        CHECK_EQ(a.lobby.ServerInitialize(c.asked, 0, nullptr), 0u);
        CHECK_EQ(a.lobby.ServerStartAccepting(false, 6), 0u);
        UdsEngine::HeardLobby heard;
        std::memset(&heard, 0, sizeof(heard));
        const bool got = WaitFor(3000, [&] {
            for (u32 i = 0; ear.engine->HeardLobbyAt(i, 300, &heard); ++i) {
                if (heard.host == a.mac()) {
                    return true;
                }
            }
            return false;
        });
        CHECK(got);
        CHECK_EQ(heard.size, 29u);
        CHECK_EQ(Hex(heard.data, 29), Hex(want.bytes, 29));
        CHECK_EQ(heard.max_nodes, want.nodes);  // the engine's own network holds what the announcement says
        CHECK_EQ(heard.total_nodes, 1u);
        CHECK_EQ(heard.channel, 6u);
        u16 me = 0, mask = 0;
        CHECK(a.e().NodeSnapshot(&me, &mask));
        CHECK(me == 1 && mask == 1);
        CHECK_EQ(a.lobby.ServerFinalize(), 0u);
        CHECK(EngineClean(a));
        CHECK(WaitFor(3000, [&] { return !ear.engine->HeardLobbyAt(0, 300, &heard); }));  // (the next lobby is not read from this one's last beacon)
    }
    CHECK_RULES();
}

static void E3_BeaconsOfOtherSizes() {
    std::printf("E3: a searcher of a title of four and the beacons of lobbies of other sizes (the dialect on the real engine)\n");
    g_rules.Reset();
    MockRoom room;
    Player s(room, 2, "Seeker", kTdLow);
    Ticker ticker({&s.e()});
    CHECK_EQ(s.lobby.ClientInitialize(nullptr, 4), 0u);
    CHECK_EQ(s.lobby.ClientStartScan(0), 0u);
    struct Case {
        u8 nodes;
        bool listed;
    };
    // (one host after the other: the engine keeps the four newest lobbies it heard)
    static const Case kCases[] = {{4, true}, {5, false}, {8, false}, {2, true}, {16, false}, {1, false}, {3, true}};
    u8 last = 0x40;
    for (const Case& c : kCases) {
        const Mac from = {0x00, 0x1F, 0x32, 0x00, 0x77, last++};
        u8 app[29];
        std::memcpy(app, AnnOf("TD 3 clients").bytes, 29);
        app[8] = c.nodes;  // the announcement says so, and so does the network of the beacon
        GroupView v;
        bool listed = false;
        const u64 end = platform::NowMs() + 700;
        u64 next = 0;
        while (platform::NowMs() < end) {
            if (platform::NowMs() >= next) {
                s.e().OnWifiPacket(MakeBeacon(from, UdsEngine::kLobbyWlanCommId, UdsEngine::kLobbyId8, 1, c.nodes, app, 29, 6));
                next = platform::NowMs() + 100;
            }
            s.lobby.Pump();
            listed |= s.lobby.ClientServerInfo(from, &v) == 0;
            SleepMs(2);
        }
        if (listed != c.listed) {
            std::printf("   a lobby of %u nodes: listed %d\n", c.nodes, listed);
        }
        CHECK_EQ(listed, c.listed);
    }
    CHECK_EQ(s.lobby.ClientFinalize(), 0u);
}

static void E2_FourOnTheEngine() {
    std::printf("E2: host + 3 clients and a fifth console over the real engine (the dialect)\n");
    g_rules.Reset();
    MockRoom room;
    Player a(room, 1, "Alice", kTdLow), b(room, 2, "Bob", kTdLow), c(room, 3, "Cleo", kTdLow), d(room, 4, "Dave", kTdLow), e(room, 5, "Eve", kTdLow);
    const Players ps = {&a, &b, &c, &d, &e};
    Sniff from_a(a.con), from_b(b.con), from_c(c.con), from_d(d.con), from_e(e.con);
    Ipc ia(&a.lobby), id(&d.lobby);
    Ticker ticker(Engines(ps));
    CHECK_EQ(a.lobby.ServerInitialize(3, 0, nullptr), 0u);
    CHECK_EQ(a.lobby.ServerStartAccepting(false, 6), 0u);
    Player* const guests[3] = {&b, &c, &d};
    for (Player* g : {&b, &c, &d, &e}) {
        CHECK_EQ(g->lobby.ClientInitialize(nullptr, 4), 0u);
        CHECK_EQ(g->lobby.ClientStartScan(0), 0u);
    }
    for (Player* g : {&b, &c, &d, &e}) {
        CHECK(Until(ps, 4000, [&] { return GroupOf(g->lobby, a.mac()) != "-"; }));
        CHECK_EQ(GroupOf(g->lobby, a.mac()), std::string("max 4, in 1, child 10c000, Alice"));
    }
    for (u32 i = 0; i < 3; ++i) {  // one after the other: the order the nodes are numbered in
        Player& g = *guests[i];
        g.lobby.ClientStopScan();
        u32 res = 0xFFFFFFFFu;
        if (g.lobby.ClientJoinBegin(a.mac(), &res) == JoinStep::Pending) {
            Until(ps, 4500, [&] { return g.lobby.ClientJoinPoll(&res); });
        }
        CHECK_EQ(res, 0u);
        CHECK_EQ(g.lobby.ClientStatusWord(), 0x05010000u | (2 + i));
    }
    CHECK(Until(ps, 3000, [&] { return ia.Clients() == "3:2,3,4"; }));
    CHECK(Until(ps, 3000, [&] { return id.Nodes() == "4:1,2,3,4"; }));
    // the fifth console sees the group full, tries all the same: the engine never answers it, the join gives up (3000 ms)
    CHECK(Until(ps, 3000, [&] { return GroupOf(e.lobby, a.mac()) == "max 4, in 4, child 10c000, Alice"; }));
    e.lobby.ClientStopScan();
    u32 res = 0xFFFFFFFFu;
    if (e.lobby.ClientJoinBegin(a.mac(), &res) == JoinStep::Pending) {
        Until(ps, 6000, [&] { return e.lobby.ClientJoinPoll(&res); });
    }
    CHECK_EQ(res, G_TIMEOUT);
    CHECK(EngineClean(e));
    u16 me = 0, mask = 0;
    CHECK(a.e().NodeSnapshot(&me, &mask));
    CHECK_EQ(mask, 0x000Fu);  // four nodes, never a fifth
    CHECK_EQ(a.e().SpectatorCount(), 0u);
    CHECK_EQ(ia.Clients(), std::string("3:2,3,4"));
    CHECK_EQ(id.Nodes(), std::string("4:1,2,3,4"));
    // START, GO
    CHECK_EQ(a.lobby.ServerStartDistribution(), 0u);
    CHECK(Until(ps, 3000, [&] { return a.lobby.ServerState() == 8; }));
    CHECK_EQ(a.lobby.ServerSendPassphrase(kGoldenPass), 0u);
    CHECK(Until(ps, 5000, [&] {
        return a.lobby.ServerState() == 9 && (b.lobby.ClientStatusWord() >> 24) == 9 && (c.lobby.ClientStatusWord() >> 24) == 9 &&
               (d.lobby.ClientStatusWord() >> 24) == 9;
    }));
    CHECK_EQ(ia.Clients(), std::string("0:"));
    for (Player* g : guests) {
        u8 pass[9] = {};
        g->lobby.ClientPassphrase(pass);
        CHECK(std::memcmp(pass, kGoldenPass, 9) == 0);
    }
    CHECK_EQ(id.Nodes(), std::string("4:1,2,3,4"));
    // the messages of the lobby on the wire (its own data channel): START and GO to each client, the two answers to the host, nothing else
    CHECK_EQ(from_a.Kinds(UdsEngine::kLobbyChannel),
             std::string("2:0100 2:0309353065346438386300 3:0100 3:0309353065346438386300 4:0100 4:0309353065346438386300"));
    for (Sniff* s : {&from_b, &from_c, &from_d}) {
        CHECK_EQ(s->Kinds(UdsEngine::kLobbyChannel), std::string("1:0200 1:0400"));
    }
    CHECK_EQ(from_e.Kinds(UdsEngine::kLobbyChannel), std::string(""));
    CHECK(EngineClean(a));
    for (Player* g : guests) {
        CHECK_EQ(g->lobby.ClientFinalize(), 0u);
        CHECK(EngineClean(*g));
    }
    CHECK_EQ(a.lobby.ServerFinalize(), 0u);
    CHECK_RULES();
}

int main(int argc, char** argv) {
    std::printf("dlp_cap_guard_test: what the titles of four players do today, held before the lobby holds eight\n");
    if (argc > 1) {
        g_dump_dir = argv[1];
    }
    TestAnnouncement();
    TestSearcherLists();
    S1_FourAndAFifth();
    S2_ManualAcceptance();
    S3_LobbyOfTwo();
    S4_NodeFive();
    D1_Codec();
    D2_RealHost();
    D3_RealSearcher();
    E1_BeaconBytes();
    E2_FourOnTheEngine();
    E3_BeaconsOfOtherSizes();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
