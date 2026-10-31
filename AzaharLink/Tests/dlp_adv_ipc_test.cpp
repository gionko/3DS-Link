// Independent ("adversarial") test of the IPC front-end of the fake Download Play lobby (Includes/azl/dlp_ipc.hpp).
//
// Written from the SPECIFICATION and the DESIGN only (analysis/dlp-lobby-fake-2026-10-28/SPEC-DLP.md and DESIGN-FASE1.md), not
// from the code under test: every request is built with the EXACT words of the specification (sections 2-3, with the real
// variants seen in the log of Luigi's Mansion 1 (az#983), Battle Royale and Triple Deluxe), every reply is checked word by word
// (header, number of words, result, payload), the three records of the game (DLPTitleInfo 0x13A8, DLPServerInfo 0x298,
// DLPNodeInfo 0x28) are compared byte by byte with records built by hand from the offsets of the specification (literal
// numbers, NOT the constants of dlp_types.hpp), the buffers of the game are guarded by canaries and by exact-size heap blocks
// (ASan), an absent buffer must give 0xD8E0AFF6, and headers with a wrong word count, unknown ids and truncated requests are
// thrown at both services in both modes.
//
// The lobby underneath is driven through a scripted LobbyNet of this file (StubNet): no code of the real fake network is used.
//
// POSIX only (fork() for the resolver-without-map probe, std::thread for the join/event test). Sections: constexpr helpers; FKCL
// flow word by word (Triple Deluxe, Luigi's Mansion 1, Battle Royale); the three records byte by byte; the tick at +0x290; max
// beacons and filters; SRVR flows (automatic and manual accept); widths of the fields (garbage in the high bytes); buffers
// (absent / wrong descriptor / too small / unmapped, canaries, unaligned blocks); unknown ids, wrong header word counts,
// truncated requests; the command x state matrix of both services; ProbeOnly (differential against DlpFkclHle); StartFakeSession
// in two steps; network ownership; counters; threads; fuzz with invariants.
//
//   dlp_adv_ipc_test      (needs nothing; built with ASan + UBSan; see the compile line in DESIGN-FASE1.md sez. 10)
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// Self-contained checks (not Tests/support.hpp). The clash that kept this test from including it, `kNodeInfoSize` in both
// uds_types.hpp and dlp_types.hpp, is gone: the DLP constant is now kDlpNodeInfoSize (DESIGN sez. 16.5). This test and
// dlp_ipc_test.cpp include the UDS engine's header beside the DLP ones, as the plugin's uds_hle.cpp will.
#include "azl/uds_engine.hpp"
#include "azl/dlp_fkcl_hle.hpp"
#include "azl/dlp_ipc.hpp"
#include "azl/dlp_lobby.hpp"
#include "azl/dlp_types.hpp"

using namespace azl;

// A report of the undefined-behaviour sanitizer stops the test (the default only prints it): an out-of-range index or an
// unaligned access is a failure, not a line nobody reads. (Ignored when the test is built without -fsanitize=undefined.)
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static int g_failures = 0;
static int g_checks = 0;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Check macros (the failure line says the section, the file line and both values).

const char* g_ctx = "";
char g_ctxbuf[96];
#define SECTION(name)                                                                                                    \
    do {                                                                                                                 \
        g_ctx = name;                                                                                                    \
        std::printf("[%s]\n", name);                                                                                     \
    } while (0)

#define EQ(a, b)                                                                                                         \
    do {                                                                                                                 \
        ++g_checks;                                                                                                      \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                                               \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                                               \
        if (va_ != vb_) {                                                                                                \
            ++g_failures;                                                                                                \
            std::fprintf(stderr, "  FAIL %s:%d [%s]: %s == %s (got 0x%llx, want 0x%llx)\n", __FILE__, __LINE__, g_ctx,   \
                         #a, #b, va_, vb_);                                                                              \
        }                                                                                                                \
    } while (0)

#define OK(c)                                                                                                            \
    do {                                                                                                                 \
        ++g_checks;                                                                                                      \
        if (!(c)) {                                                                                                      \
            ++g_failures;                                                                                                \
            std::fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_ctx, #c);                              \
        }                                                                                                                \
    } while (0)

// The numbers the design writes by hand (DESIGN 5.2, 9.2): kept literal here on purpose.
constexpr u32 kTicksPerMs = 268112;
constexpr u32 kCmdWords = 64;

constexpr u64 kTd = 0x000400000010C000ull;   // Kirby Triple Deluxe: child unique id 0x10C0
constexpr u64 kLm1 = 0x00040000001D1900ull;  // Luigi's Mansion 1: filter 0x00040001'001D1900 (az#983)
constexpr u64 kBr = 0x00040000001C2100ull;   // Kirby Battle Royale: unique id 0x1C21
constexpr u64 kSeed = 0x00C0FFEE12345678ull;

// ---------------------------------------------------------------------------------------------------------------------
// Names (UTF-16, 10 units) and hand-written little-endian stores.

struct Nm {
    u16 u[10];
};
Nm MkName(const char* s) {
    Nm n;
    std::memset(&n, 0, sizeof(n));
    for (int i = 0; i < 10 && s[i]; ++i) {
        n.u[i] = static_cast<u16>(static_cast<u8>(s[i]));
    }
    return n;
}
Nm MkNameU(std::initializer_list<u16> l) {
    Nm n;
    std::memset(&n, 0, sizeof(n));
    int i = 0;
    for (u16 c : l) {
        if (i < 10) {
            n.u[i++] = c;
        }
    }
    return n;
}
// Five request words of a name: unit 2i in the low half of word i, unit 2i+1 in the high half.
void NameWords(const Nm& n, u32* w) {
    for (int i = 0; i < 5; ++i) {
        w[i] = static_cast<u32>(n.u[2 * i]) | (static_cast<u32>(n.u[2 * i + 1]) << 16);
    }
}
bool SameName(const u16* a, const Nm& b) {
    return std::memcmp(a, b.u, 20) == 0;
}

void P16(std::vector<u8>& v, size_t o, u32 x) {
    v[o] = static_cast<u8>(x);
    v[o + 1] = static_cast<u8>(x >> 8);
}
void P32(std::vector<u8>& v, size_t o, u32 x) {
    P16(v, o, x & 0xFFFF);
    P16(v, o + 2, x >> 16);
}
void P64(std::vector<u8>& v, size_t o, u64 x) {
    P32(v, o, static_cast<u32>(x));
    P32(v, o + 4, static_cast<u32>(x >> 32));
}
u32 G32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// The records, built by hand from SPEC-DLP.md 5.1-5.3 and DESIGN 5.2 (literal offsets).
std::vector<u8> ExpTitle(u32 unique_id, u32 variation, const Mac& mac) {
    std::vector<u8> v(0x13A8, 0);
    P32(v, 0x00, unique_id);
    P32(v, 0x04, variation);
    for (int i = 0; i < 6; ++i) {
        v[0x08 + i] = mac[i];
    }
    return v;
}
std::vector<u8> ExpServer(const Mac& mac, u8 signal, u8 max_nodes, u8 joined, const Nm& host, u64 tick) {
    std::vector<u8> v(0x298, 0);
    for (int i = 0; i < 6; ++i) {
        v[i] = mac[i];
    }
    v[0x07] = signal;
    v[0x08] = max_nodes;
    v[0x09] = joined;
    for (int i = 0; i < 10; ++i) {
        P16(v, 0x1C + 2 * i, host.u[i]);  // node_info[0].username = 0x10 + 0x0C
    }
    P64(v, 0x290, tick);
    return v;
}
std::vector<u8> ExpNode(u64 seed, const Nm& nm, u32 node) {
    std::vector<u8> v(0x28, 0);
    P64(v, 0x00, seed);
    for (int i = 0; i < 10; ++i) {
        P16(v, 0x0C + 2 * i, nm.u[i]);
    }
    P32(v, 0x24, node);
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// The scripted virtual network (a LobbyNet written for this test).

class StubNet final : public LobbyNet {
public:
    struct Sent {
        u8 to;
        std::vector<u8> data;
    };
    struct In {
        u8 from;
        std::vector<u8> data;
    };
    u64 now = 5000000;
    u64 tick_offset = 0x20000000000ull;  // >= 2^40 on purpose
    bool fail_host_open = false;
    bool fail_scan_open = false;
    bool fail_connect_begin = false;
    int connect_result = 0;  // what ConnectPoll answers
    int host_open_calls = 0, host_update_calls = 0, host_close_calls = 0;
    int scan_open_calls = 0, scan_close_calls = 0, connect_begin_calls = 0, disconnect_calls = 0;
    HostAnnounce last_announce;
    u8 last_channel = 0xEE;
    Identity host_id;
    Identity connect_id;
    Mac connect_mac{};
    std::vector<u8> kicked;
    std::vector<HeardGroup> heard;
    std::vector<u8> members;
    NodeEntry entries[8];
    bool have[8];
    u8 my_node = 0;
    std::vector<Sent> sent;
    std::deque<In> inbox;
    int rx_pops = 0;

    StubNet() {
        std::memset(&last_announce, 0, sizeof(last_announce));
        std::memset(&host_id, 0, sizeof(host_id));
        std::memset(&connect_id, 0, sizeof(connect_id));
        std::memset(entries, 0, sizeof(entries));
        std::memset(have, 0, sizeof(have));
    }

    u64 NowMs() override {
        return now;
    }
    u64 NowTicks() override {
        return now * kTicksPerMs + tick_offset;
    }
    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override {
        ++host_open_calls;
        if (fail_host_open) {
            return false;
        }
        last_announce = a;
        last_channel = channel;
        host_id = me;
        my_node = 1;
        members.assign(1, 1);
        SetEntry(1, me.seed, me.name);
        return true;
    }
    void HostUpdate(const HostAnnounce& a) override {
        ++host_update_calls;
        last_announce = a;
    }
    void HostClose() override {
        ++host_close_calls;
        my_node = 0;
        members.clear();
    }
    void Kick(u8 node) override {
        kicked.push_back(node);
        members.erase(std::remove(members.begin(), members.end(), node), members.end());
    }
    bool ScanOpen() override {
        ++scan_open_calls;
        return !fail_scan_open;
    }
    void ScanClose() override {
        ++scan_close_calls;
    }
    bool ScanRead(u32 index, HeardGroup* out) override {
        if (index < heard.size()) {
            *out = heard[index];
            return true;
        }
        return false;
    }
    bool ConnectBegin(const Mac& host, const Identity& me) override {
        ++connect_begin_calls;
        if (fail_connect_begin) {
            return false;
        }
        connect_mac = host;
        connect_id = me;
        return true;
    }
    int ConnectPoll() override {
        return connect_result;
    }
    void Disconnect() override {
        ++disconnect_calls;
        my_node = 0;
        members.clear();
    }
    u8 MyNode() override {
        return my_node;
    }
    u32 Members(u8 (&nodes)[kMaxPlayers]) override {
        std::vector<u8> s = members;
        std::sort(s.begin(), s.end());
        u32 n = 0;
        for (u8 m : s) {
            if (n < kMaxPlayers) {
                nodes[n++] = m;
            }
        }
        return n;
    }
    bool ReadNode(u8 node, NodeEntry* out) override {
        if (node < 8 && have[node]) {
            *out = entries[node];
            return true;
        }
        return false;
    }
    bool Send(u8 to_node, const u8* data, u32 size) override {
        sent.push_back({to_node, std::vector<u8>(data, data + size)});
        return true;
    }
    bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) override {
        if (inbox.empty()) {
            return false;
        }
        In m = inbox.front();
        inbox.pop_front();
        ++rx_pops;
        *from_node = m.from;
        const u32 n = std::min<u32>(capacity, static_cast<u32>(m.data.size()));
        std::memcpy(data, m.data.data(), n);
        *size = n;
        return true;
    }

    // --- scripting helpers ---
    void SetEntry(u8 node, u64 seed, const u16* name) {
        entries[node].seed = seed;
        std::memcpy(entries[node].name, name, 20);
        entries[node].node = node;
        have[node] = true;
    }
    void AddNode(u8 node, u64 seed, const char* name) {
        const Nm n = MkName(name);
        SetEntry(node, seed, n.u);
        if (std::find(members.begin(), members.end(), node) == members.end()) {
            members.push_back(node);
        }
    }
    void BecomeClient(u8 mine, u64 host_seed, const char* host_name, u64 my_seed, const char* my_name) {
        my_node = mine;
        members.clear();
        AddNode(1, host_seed, host_name);
        AddNode(mine, my_seed, my_name);
    }
    void Inbound(u8 from, std::initializer_list<u8> bytes) {
        inbox.push_back({from, std::vector<u8>(bytes)});
    }
    int CountSent(u8 to, std::initializer_list<u8> bytes) const {
        const std::vector<u8> want(bytes);
        int n = 0;
        for (const Sent& s : sent) {
            if (s.to == to && s.data == want) {
                ++n;
            }
        }
        return n;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// The memory of the game: blocks with canaries, resolved by address.

struct Arena {
    u32 base, size, front, back;
    std::unique_ptr<u8[]> mem;
    u8* p;
    Arena(u32 b, u32 s, u32 f = 0, u32 bk = 0, u8 fill = 0xEE) : base(b), size(s), front(f), back(bk) {
        mem.reset(new u8[f + s + bk + 1]);
        std::memset(mem.get(), 0xA5, f + s + bk);
        p = mem.get() + f;
        std::memset(p, fill, s);
    }
    bool CanariesOk() const {
        for (u32 i = 0; i < front; ++i) {
            if (mem[i] != 0xA5) {
                return false;
            }
        }
        for (u32 i = 0; i < back; ++i) {
            if (p[size + i] != 0xA5) {
                return false;
            }
        }
        return true;
    }
    bool All(u8 v) const {
        for (u32 i = 0; i < size; ++i) {
            if (p[i] != v) {
                return false;
            }
        }
        return true;
    }
};

struct Rep {
    u32 w[kCmdWords];
    DlpOutcome out;
};

constexpr u32 kSbAddr = 0x08100000;
constexpr u32 kMwAddr = 0x08200000;

struct Rig {
    StubNet net;
    LobbyConfig cfg;
    DlpLobby lobby;
    DlpService svc;
    std::vector<std::unique_ptr<Arena>> arenas;
    std::vector<std::pair<u32, u32>> map_calls;
    IpcResolver res;

    static LobbyConfig MkCfg(u64 tid) {
        LobbyConfig c;
        std::memset(&c, 0, sizeof(c));
        c.game_tid = tid;
        c.friend_seed = kSeed;
        const Nm n = MkName("Kiri");
        std::memcpy(c.name, n.u, 20);
        return c;
    }
    explicit Rig(u64 tid = kTd) : cfg(MkCfg(tid)), lobby(&net, cfg) {
        svc.SetLobby(&lobby);
        res.ctx = this;
        res.map = &Rig::Thunk;
    }
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        return static_cast<Rig*>(ctx)->Map(addr, size);
    }
    u8* Map(u32 addr, u32 size) {
        map_calls.push_back({addr, size});
        if (addr == 0) {
            return nullptr;
        }
        for (auto& a : arenas) {
            if (addr >= a->base && static_cast<u64>(addr) + size <= static_cast<u64>(a->base) + a->size) {
                return a->p + (addr - a->base);
            }
        }
        return nullptr;
    }
    Arena* Add(u32 base, u32 size, u32 front = 0, u32 back = 0, u8 fill = 0xEE) {
        arenas.emplace_back(new Arena(base, size, front, back, fill));
        return arenas.back().get();
    }
    void DropArenas() {
        arenas.clear();
    }

    // One IPC request: the words are placed in a 64-word command buffer whose remaining words carry a canary; nothing
    // beyond the larger of request and reply may change.
    Rep Call(DlpKind k, const u32* words, u32 n, const u32* sb = nullptr, u32 step = 25) {
        Rep r;
        for (u32 i = 0; i < kCmdWords; ++i) {
            r.w[i] = 0xD0D00000u | i;
        }
        for (u32 i = 0; i < n; ++i) {
            r.w[i] = words[i];
        }
        u32 req[kCmdWords];
        std::memcpy(req, r.w, sizeof(req));
        net.now += step;
        map_calls.clear();
        r.out = svc.Dispatch(k, r.w, sb, res);
        if (r.out == DlpOutcome::Done) {
            const u32 rw = 1 + ((r.w[0] >> 6) & 0x3F) + (r.w[0] & 0x3F);
            const u32 lim = std::max(rw, n);
            for (u32 i = lim; i < kCmdWords; ++i) {
                if (r.w[i] != req[i]) {
                    ++g_failures;
                    std::fprintf(stderr, "  FAIL [%s]: word %u beyond the reply/request was written (0x%x)\n", g_ctx, i,
                                 r.w[i]);
                    break;
                }
            }
            ++g_checks;
        } else {
            ++g_checks;
            if (std::memcmp(req, r.w, sizeof(req)) != 0) {
                ++g_failures;
                std::fprintf(stderr, "  FAIL [%s]: JoinPending but the command words were written\n", g_ctx);
            }
        }
        return r;
    }
    Rep Fk(std::initializer_list<u32> w, const u32* sb = nullptr, u32 step = 25) {
        std::vector<u32> v(w);
        return Call(DlpKind::Fkcl, v.data(), static_cast<u32>(v.size()), sb, step);
    }
    Rep Sv(std::initializer_list<u32> w, const u32* sb = nullptr, u32 step = 25) {
        std::vector<u32> v(w);
        return Call(DlpKind::Srvr, v.data(), static_cast<u32>(v.size()), sb, step);
    }
    // Status word of the searcher / state byte of the creator through the IPC.
    u32 Status() {
        return Fk({0x000C0000}).w[2];
    }
    u32 SrvState() {
        return Sv({0x00030000}).w[2];
    }
};

void CheckWords(const Rep& r, std::initializer_list<u32> want, int line) {
    ++g_checks;
    if (r.out != DlpOutcome::Done) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL %s:%d [%s]: outcome is JoinPending, a reply was expected\n", __FILE__, line, g_ctx);
        return;
    }
    u32 i = 0;
    for (u32 v : want) {
        if (r.w[i] != v) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL %s:%d [%s]: reply word [%u] is 0x%08x, expected 0x%08x (reply [0]=0x%08x [1]=0x%08x)\n",
                         __FILE__, line, g_ctx, i, r.w[i], v, r.w[0], r.w[1]);
            return;
        }
        ++i;
    }
}
#define WORDS(r, ...) CheckWords((r), std::initializer_list<u32>{__VA_ARGS__}, __LINE__)
// An error reply: (id << 16) | 0x40 and the result, two words.
#define ERRW(r, id, res) WORDS((r), (static_cast<u32>(id) << 16) | 0x40u, static_cast<u32>(res))

void CheckBytes(const char* what, const u8* got, const std::vector<u8>& want, int line) {
    ++g_checks;
    for (size_t i = 0; i < want.size(); ++i) {
        if (got[i] != want[i]) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL %s:%d [%s]: %s differs at +0x%zx (got 0x%02x, expected 0x%02x)\n", __FILE__, line,
                         g_ctx, what, i, got[i], want[i]);
            return;
        }
    }
}
#define BYTES(what, got, want) CheckBytes((what), (got), (want), __LINE__)

// The words [2..11] of a node-info reply as bytes.
std::vector<u8> NodeBytes(const Rep& r) {
    std::vector<u8> v(40);
    for (int i = 0; i < 10; ++i) {
        P32(v, 4 * i, r.w[2 + i]);
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scenario pieces.

const Mac kMacA = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x01};
const Mac kMacB = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x02};
const Mac kMacC = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x03};
const Mac kMacD = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x04};
const Mac kMacE = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x05};
const Mac kMacF = {0x40, 0xF4, 0x07, 0xAA, 0xBB, 0x06};

u32 MW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) |
           (static_cast<u32>(m[3]) << 24);
}
u32 MW2(const Mac& m, u32 high_garbage = 0) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8) | high_garbage;
}

HeardGroup MkGroup(const Mac& mac, u32 child_tid_low, const char* name, u8 max_nodes, u8 total, u8 signal = 3,
                   u8 accepting = 1) {
    HeardGroup g;
    std::memset(&g, 0, sizeof(g));
    g.host = mac;
    g.total_nodes = total;
    g.signal = signal;
    g.announce.child_tid_low = child_tid_low;
    const Nm n = MkName(name);
    std::memcpy(g.announce.name, n.u, 20);
    g.announce.max_nodes = max_nodes;
    g.announce.accepting = accepting;
    return g;
}

const u32 kStaticTitle = StaticBufferDesc(0x13A8, 0);
const u32 kStaticServer = StaticBufferDesc(0x298, 0);

// The static-buffer pair of the thread.
struct Sb {
    u32 w[2];
    Sb(u32 desc, u32 addr) {
        w[0] = desc;
        w[1] = addr;
    }
};

// FKCL enters a state by the commands a game sends. States: the numbers of the design, 3 = joining, 0x80 = lost.
enum { kS0 = 0, kS1, kS2, kS3, kS5, kS6, kS9, kS80, kNumSt };
const char* const kStName[kNumSt] = {"0", "1", "2", "joining", "5", "6", "9", "0x80"};
const u8 kPass[9] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};

void EnterFkcl(Rig& g, int st) {
    if (st == kS0) {
        return;
    }
    g.Fk({0x00010083, 0x30000, 16, 0x04000000, 0x00D00001, 0x00D00002});
    if (st == kS1) {
        return;
    }
    g.net.heard.assign(1, MkGroup(kMacA, 0x0010C002, "HostName", 4, 2));
    g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
    if (st == kS2) {
        return;
    }
    // load the table (the pump of the next command reads the group), then leave the scan, then join
    g.Status();
    g.Fk({0x00060000});
    g.net.connect_result = 0;
    const Rep r = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001});
    OK(r.out == DlpOutcome::JoinPending);
    if (st == kS3) {
        return;
    }
    g.net.BecomeClient(2, 0x1111, "HostName", 0x2222, "Me");
    g.net.connect_result = 1;
    u32 cmd[kCmdWords] = {};
    OK(g.svc.PollPending(cmd));
    if (st == kS5) {
        return;
    }
    if (st == kS80) {
        g.net.members.assign(1, 2);  // the host vanishes
        for (int i = 0; i < 40 && g.Status() != 0x80000000u; ++i) {
            g.net.now += 100;
        }
        return;
    }
    g.net.Inbound(1, {1, 0});
    g.Status();
    if (st == kS6) {
        return;
    }
    g.net.Inbound(1, {3, 9, kPass[0], kPass[1], kPass[2], kPass[3], kPass[4], kPass[5], kPass[6], kPass[7], kPass[8]});
    g.Status();
}

enum { kV0 = 0, kV1, kV2, kV6, kV8, kV9, kNumSv };
const char* const kSvName[kNumSv] = {"0", "1", "2", "6", "8", "9"};

void EnterSrvr(Rig& g, int st, bool manual = false) {
    if (st == kV0) {
        return;
    }
    g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x11, 0x12});
    if (st == kV1) {
        return;
    }
    g.Sv({0x00050080, manual ? 1u : 0u, 6});
    if (st == kV2) {
        return;
    }
    g.net.AddNode(2, 0x2222, "Client2");
    g.Sv({0x00030000});  // the pump tracks the client
    if (manual) {
        g.Sv({0x00090040, 2});  // AcceptClient
    }
    g.Sv({0x00070000});  // StartDistribution
    if (st == kV6) {
        return;
    }
    g.net.Inbound(2, {2, 0});
    g.Sv({0x00030000});
    if (st == kV8) {
        return;
    }
    g.Sv({0x000800C0, 0x14131211, 0x18171615, 0x00000019});
    g.net.Inbound(2, {4, 0});
    g.Sv({0x00030000});
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. The helpers and the values the design writes by hand.

void TestHelpers() {
    SECTION("constexpr helpers: literal values of the games");
    EQ(Header(0x0B, 2, 2), 0x000B0082u);
    EQ(Header(0x01, 1, 0), 0x00010040u);
    EQ(Header(0x05, 6, 0), 0x00050180u);
    EQ(StaticBufferDesc(0x298, 0), 0x00A60002u);
    EQ(StaticBufferDesc(0x13A8, 0), 0x04EA0002u);
    EQ(StaticBufferDesc(0x10, 1), (0x10u << 14) | (1u << 10) | 2u);
    EQ(MappedWriteDesc(8), 0x8Cu);
    EQ(MappedWriteDesc(0), 0xCu);
    EQ(kTitleInfoSize, 5032u);
    EQ(kServerInfoSize, 664u);
    EQ(kDlpNodeInfoSize, 40u);
    EQ(kNodeInfoSize, 40u);  // the UDS engine's own constant (uds_types.hpp), now able to live beside it
    EQ(kMaxPlayers, 4u);
    EQ(kMaxClients, 3u);
    EQ(kMaxGroups, 4u);
    EQ(kInvalidState, 0xE0A0AC01u);
    EQ(kNoData, 0xC880AFEFu);
    EQ(kNotFound, 0xC880AC05u);
    EQ(kTimeout, 0xC920AFFEu);
    EQ(kOutOfRange, 0xD8E0AFFDu);
    EQ(kInvalidPointer, 0xD8E0AFF6u);
    EQ(kBusy, 0xE0A0AC03u);
    EQ(kBadHeader, 0xD9001830u);
    // the object sizes the user's rule fixes (x86-64 here; the design says <= 320 / <= 64 on every ABI)
    OK(sizeof(DlpLobby) <= 320);
    OK(sizeof(DlpService) <= 64);
    std::printf("  sizeof(DlpLobby)=%zu sizeof(DlpService)=%zu\n", sizeof(DlpLobby), sizeof(DlpService));
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. FKCL, the command words of Triple Deluxe, one by one.

void TestFkclFlowTd() {
    SECTION("FKCL: Triple Deluxe flow, exact words");
    Rig g(kTd);
    Rep r;
    // state 0: the commands that always succeed already answer
    r = g.Fk({0x00040000});
    WORDS(r, 0x00040080, 0, 0x0421);
    r = g.Fk({0x000C0000});
    WORDS(r, 0x000C0180, 0, 0, 0, 0, 0, 0);
    r = g.Fk({0x00030000});
    WORDS(r, 0x000301C0, 0, 0, 0, 0, 0, 0, 0);
    r = g.Fk({0x000F0000});
    WORDS(r, 0x000F0100, 0, 0, 0, 0);
    r = g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
    WORDS(r, 0x00050040, kInvalidState);

    // Initialize (TD: 0x00010083, not InitializeWithName), random handles must not matter
    r = g.Fk({0x00010083, 0x30000, 16, 0x04000000, 0x00D00001, 0x00D00002});
    WORDS(r, 0x00010040, 0);
    EQ(g.Status(), 0x01000000u);
    r = g.Fk({0x00010083, 0x1000, 16, 0x04000000, 0xFFFFFFFF, 0});
    WORDS(r, 0x00010040, 0);

    // StartScan: [1] is the channel mask of GetChannels (ignored), [2][3] the title id, [4][5] the MAC filter, [6] unknown
    g.net.heard = {MkGroup(kMacA, 0x0010C002, "HostName", 4, 2), MkGroup(kMacB, 0x0010C000, "Other", 4, 1)};
    r = g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
    WORDS(r, 0x00050040, 0);
    EQ(g.Status(), 0x02000000u);
    OK(g.net.scan_open_calls >= 1);
    r = g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});  // already scanning
    WORDS(r, 0x00050040, kInvalidState);

    // the loop of the game: GetTitleInfoInOrder(1) then (0) until exactly 0xC880AFEF
    Arena* a = g.Add(kSbAddr, 0x13A8);
    Sb sb(kStaticTitle, kSbAddr);
    r = g.Fk({0x00090040, 1}, sb.w);
    WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
    BYTES("TitleInfo A", a->p, ExpTitle(0x10C0, 2, kMacA));
    OK(g.map_calls.size() == 1 && g.map_calls[0].first == kSbAddr && g.map_calls[0].second == 0x13A8);
    std::memset(a->p, 0xCC, 0x13A8);
    r = g.Fk({0x00090040, 0}, sb.w);
    WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
    BYTES("TitleInfo B", a->p, ExpTitle(0x10C0, 0, kMacB));
    std::memset(a->p, 0xCC, 0x13A8);
    r = g.Fk({0x00090040, 0}, sb.w);
    ERRW(r, 9, kNoData);
    OK(a->All(0xCC));  // an error writes nothing
    r = g.Fk({0x00090040, 0}, sb.w);
    ERRW(r, 9, kNoData);
    r = g.Fk({0x00090040, 1}, sb.w);  // restart
    WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
    BYTES("TitleInfo A again", a->p, ExpTitle(0x10C0, 2, kMacA));

    // GetServerInfo(mac)
    g.DropArenas();
    Arena* s = g.Add(kSbAddr, 0x298);
    Sb sbs(kStaticServer, kSbAddr);
    r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, sbs.w);
    WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
    BYTES("ServerInfo A", s->p, ExpServer(kMacA, 3, 4, 2, MkName("HostName"), g.net.NowTicks()));
    OK(g.map_calls.size() == 1 && g.map_calls[0].second == 0x298);
    // a MAC whose upper half-word of [2] carries garbage still finds the group
    std::memset(s->p, 0xCC, 0x298);
    r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA, 0xFFFF0000u)}, sbs.w);
    WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
    BYTES("ServerInfo A, garbage in [2]", s->p, ExpServer(kMacA, 3, 4, 2, MkName("HostName"), g.net.NowTicks()));
    // an unknown MAC: error reply, nothing written
    std::memset(s->p, 0xCC, 0x298);
    r = g.Fk({0x00070080, MW1(kMacC), MW2(kMacC)}, sbs.w);
    ERRW(r, 7, kNoData);
    OK(s->All(0xCC));
    // GetTitleInfo(mac, title id): the title id is ignored
    g.DropArenas();
    a = g.Add(kSbAddr, 0x13A8);
    r = g.Fk({0x00080100, MW1(kMacB), MW2(kMacB), 0xDEADBEEF, 0xCAFEBABE}, sb.w);
    WORDS(r, 0x00080042, 0, kStaticTitle, kSbAddr);
    BYTES("GetTitleInfo B", a->p, ExpTitle(0x10C0, 0, kMacB));
    r = g.Fk({0x00080100, MW1(kMacC), MW2(kMacC), 0, 0}, sb.w);
    ERRW(r, 8, kNoData);

    // DeleteScanInfo (no step: the pump must not bring the entry back before the check)
    r = g.Fk({0x000A0080, MW1(kMacB), MW2(kMacB)}, nullptr, 0);
    WORDS(r, 0x000A0040, 0);
    r = g.Fk({0x00080100, MW1(kMacB), MW2(kMacB), 0, 0}, sb.w, 0);
    ERRW(r, 8, kNoData);
    r = g.Fk({0x000A0080, MW1(kMacB), MW2(kMacB)}, nullptr, 0);
    ERRW(r, 0xA, kNoData);

    // StopScan
    r = g.Fk({0x00060000});
    WORDS(r, 0x00060040, 0);
    EQ(g.Status(), 0x01000000u);
    OK(g.net.scan_close_calls >= 1);

    // StartFakeSession: [1][2] MAC, [3][4] title id (ignored): two-step reply
    g.net.connect_result = 0;
    const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001});
    OK(jp.out == DlpOutcome::JoinPending);
    OK(g.svc.join_pending());
    OK(g.net.connect_begin_calls == 1);
    OK(g.net.connect_mac == kMacA);
    OK(SameName(g.net.connect_id.name, MkName("Kiri")));  // Initialize without name: the default name
    EQ(g.net.connect_id.seed, kSeed);
    EQ(g.Status(), 0x01000000u);  // joining is shown as 1
    u32 cmd[kCmdWords];
    for (u32 i = 0; i < kCmdWords; ++i) {
        cmd[i] = 0xABAB0000u | i;
    }
    OK(!g.svc.PollPending(cmd));  // still joining: cmd untouched
    for (u32 i = 0; i < kCmdWords; ++i) {
        EQ(cmd[i], 0xABAB0000u | i);
    }
    g.net.BecomeClient(2, 0x1111, "HostName", 0x2222, "Me");
    g.net.connect_result = 1;
    OK(g.svc.PollPending(cmd));
    EQ(cmd[0], 0x000B0040u);
    EQ(cmd[1], 0u);
    OK(!g.svc.join_pending());
    EQ(g.svc.last_result(), 0u);
    EQ(g.svc.last_id(), 0x0Bu);
    EQ(g.Status(), 0x05010002u);

    // GetConnectingNodes(4): ids 1 and 2, total 2
    g.DropArenas();
    Arena* n = g.Add(kSbAddr, 8);
    Sb sbn(StaticBufferDesc(8, 0), kSbAddr);
    r = g.Fk({0x000D0040, 4}, sbn.w);
    WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(4, 0), kSbAddr);
    OK(n->p[0] == 1 && n->p[1] == 0 && n->p[2] == 2 && n->p[3] == 0);
    for (u32 i = 4; i < 8; ++i) {
        EQ(n->p[i], 0xEE);  // only 2 ids written
    }
    // GetNodeInfo(1), (2), unknown
    r = g.Fk({0x000E0040, 1});
    WORDS(r, 0x000E02C0, 0);
    BYTES("NodeInfo 1", NodeBytes(r).data(), ExpNode(0x1111, MkName("HostName"), 1));
    r = g.Fk({0x000E0040, 2});
    WORDS(r, 0x000E02C0, 0);
    BYTES("NodeInfo 2", NodeBytes(r).data(), ExpNode(0x2222, MkName("Me"), 2));
    r = g.Fk({0x000E0040, 3});
    ERRW(r, 0xE, kNoData);
    r = g.Fk({0x000E0040, 0});
    ERRW(r, 0xE, kNoData);
    // the title and server records of the group we joined stay readable, with a fresh tick
    g.DropArenas();
    s = g.Add(kSbAddr, 0x298);
    r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, sbs.w);
    WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
    OK(G32(s->p + 0x290 + 4) == static_cast<u32>(g.net.NowTicks() >> 32));
    EQ(G32(s->p + 0x290), static_cast<u32>(g.net.NowTicks()));  // in session the age is 0

    // START: 5 -> 6 and an answer; GO: 9 with the passphrase; the host then goes away: 9 stays
    r = g.Fk({0x000F0000});
    WORDS(r, 0x000F0100, 0, 0, 0, 0);
    g.net.Inbound(1, {1, 0});
    EQ(g.Status(), 0x06010002u);
    OK(g.net.CountSent(1, {2, 0}) >= 1);  // START_ACK to the host
    g.net.Inbound(1, {3, 9, kPass[0], kPass[1], kPass[2], kPass[3], kPass[4], kPass[5], kPass[6], kPass[7], kPass[8]});
    EQ(g.Status(), 0x09010002u);
    OK(g.net.CountSent(1, {4, 0}) >= 1);  // GO_ACK
    r = g.Fk({0x000F0000});
    WORDS(r, 0x000F0100, 0, 0x14131211, 0x18171615, 0x00000019);
    g.net.members.assign(1, 2);  // the host closes its virtual network
    for (int i = 0; i < 100; ++i) {
        g.net.now += 100;
    }
    EQ(g.Status(), 0x09010002u);  // 9 is final
    r = g.Fk({0x000F0000});
    WORDS(r, 0x000F0100, 0, 0x14131211, 0x18171615, 0x00000019);

    // StopSession, Finalize
    r = g.Fk({0x00100000});
    WORDS(r, 0x00100040, 0);
    EQ(g.Status(), 0x01000000u);
    r = g.Fk({0x00020000});
    WORDS(r, 0x00020040, 0);
    EQ(g.Status(), 0x01000000u);  // Finalize leaves 1 (as Azahar), not 0
    r = g.Fk({0x00020000});
    WORDS(r, 0x00020040, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. The real variants seen: Luigi's Mansion 1 (az#983), Battle Royale.

void TestFkclLm1() {
    SECTION("FKCL: Luigi's Mansion 1 buffers (az#983)");
    Rig g(kLm1);
    Rep r;
    // InitializeWithName as the log has it: [1]=0xa000 [2]=2 [3..8]="Guest-LM1" [9]=0x4000000 [10]=handle [11]=1
    const Nm nm = MkName("Guest-LM1");
    u32 w[12] = {0x00110203, 0xA000, 2, 0, 0, 0, 0, 0, 0, 0x04000000, 0x00340003, 1};
    NameWords(nm, w + 3);
    r = g.Call(DlpKind::Fkcl, w, 12);
    WORDS(r, 0x00110040, 0);
    EQ(g.svc.initializes(), 1u);
    // GetMyStatus right after Initialize: state 1
    r = g.Fk({0x000C0000});
    WORDS(r, 0x000C0180, 0, 0x01000000, 0, 0, 0, 0);
    // GetChannels, then StartScan with [1] = 0x2 (LM1 passes 0x2, not 0x421)
    r = g.Fk({0x00040000});
    WORDS(r, 0x00040080, 0, 0x0421);
    g.net.heard.assign(1, MkGroup(kMacA, 0x001D1900, "Luigi", 2, 1));
    r = g.Fk({0x00050180, 0x2, 0x1d1900, 0x40001, 0, 0, 0});
    WORDS(r, 0x00050040, 0);
    Arena* a = g.Add(kSbAddr, 0x13A8);
    Sb sb(kStaticTitle, kSbAddr);
    r = g.Fk({0x00090040, 1}, sb.w);
    WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
    BYTES("LM1 TitleInfo", a->p, ExpTitle(0x1D19, 0, kMacA));
    r = g.Fk({0x00060000});
    WORDS(r, 0x00060040, 0);
    g.net.connect_result = 0;
    const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x1d1900, 0x40001});
    OK(jp.out == DlpOutcome::JoinPending);
    OK(SameName(g.net.connect_id.name, nm));  // the name of InitializeWithName goes to the network
    g.net.BecomeClient(2, 0x1111, "Luigi", 0x2222, "Guest-LM1");
    g.net.connect_result = 1;
    u32 cmd[kCmdWords] = {};
    OK(g.svc.PollPending(cmd));
    EQ(cmd[0], 0x000B0040u);
    EQ(cmd[1], 0u);
    EQ(g.Status(), 0x05010002u);

    // the creator side: InitializeWithName [1]=0x85000 [2]=1 [3]=0xe [4]=0 [5]=0x40000 [6]=2 [7..11]=name [13]=0x4000000
    Rig h(kLm1);
    u32 sw[16] = {0x000F0303, 0x85000, 1, 0xE, 0, 0x40000, 2, 0, 0, 0, 0, 0, 0xFFFF0000u, 0x04000000, 0x00340004, 1};
    NameWords(nm, sw + 7);
    r = h.Call(DlpKind::Srvr, sw, 16);
    WORDS(r, 0x000F0040, 0);
    r = h.Sv({0x00100000});  // GetDupNoticeNeed
    WORDS(r, 0x00100080, 0, 0);
    EQ(h.SrvState(), 1u);
    // StartAccepting (1, 0x03a12006): manual, and only the low byte (6) is the channel
    r = h.Sv({0x00050080, 1, 0x03a12006});
    WORDS(r, 0x00050040, 0);
    EQ(h.net.last_channel, 6u);
    EQ(h.net.last_announce.max_nodes, 2u);  // max 1 client + the host
    EQ(h.net.last_announce.child_tid_low, 0x001D1900u);
    EQ(h.net.last_announce.accepting, 1u);
    OK(SameName(h.net.last_announce.name, nm));
    OK(SameName(h.net.host_id.name, nm));
    EQ(h.SrvState(), 2u);
    // a client arrives; manual accept: waiting 0x40, GetClientState and AcceptClient
    h.net.AddNode(2, 0x2222, "Client");
    Arena* mw = h.Add(kMwAddr, 8);
    r = h.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    WORDS(r, 0x000B0082, 0, 1, MappedWriteDesc(8), kMwAddr);
    OK(mw->p[0] == 2 && mw->p[1] == 0);
    for (u32 i = 2; i < 8; ++i) {
        EQ(mw->p[i], 0xEE);
    }
    r = h.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x40000002, 0, 0, 0, 0);
    r = h.Sv({0x00090040, 2});
    WORDS(r, 0x00090040, 0);
    r = h.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x05010002, 0, 0, 0, 0);
    r = h.Sv({0x00090040, 2});  // twice: not waiting any more
    ERRW(r, 9, kNoData);
    r = h.Sv({0x000C0040, 2});
    WORDS(r, 0x000C02C0, 0);
    BYTES("LM1 client info", NodeBytes(r).data(), ExpNode(0x2222, MkName("Client"), 2));
}

void TestFkclBr() {
    SECTION("FKCL: Battle Royale words (InitializeWithName 0x30000 / 16 beacons)");
    Rig g(kBr);
    const Nm nm = MkName("Brawler");
    u32 w[12] = {0x00110203, 0x30000, 16, 0, 0, 0, 0, 0, 0, 0x04000000, 0x00330001, 0x00330002};
    NameWords(nm, w + 3);
    Rep r = g.Call(DlpKind::Fkcl, w, 12);
    WORDS(r, 0x00110040, 0);
    r = g.Fk({0x00040000});
    WORDS(r, 0x00040080, 0, 0x0421);
    // StartScan with the filter 0x00040001'001C2100 | child index (BR: variation = index), MAC filter NULL
    g.net.heard.assign(1, MkGroup(kMacA, 0x001C2103, "Host", 4, 3, 2));
    r = g.Fk({0x00050180, 0x0421, 0x001C2101, 0x00040001, 0, 0, 0});
    WORDS(r, 0x00050040, 0);
    Arena* a = g.Add(kSbAddr, 0x13A8);
    Sb sb(kStaticTitle, kSbAddr);
    r = g.Fk({0x00090040, 1}, sb.w);
    WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
    BYTES("BR TitleInfo", a->p, ExpTitle(0x1C21, 3, kMacA));
    r = g.Fk({0x00090040, 0}, sb.w);
    ERRW(r, 9, kNoData);
    g.DropArenas();
    Arena* s = g.Add(kSbAddr, 0x298);
    Sb sbs2(kStaticServer, kSbAddr);
    r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, sbs2.w);
    WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
    BYTES("BR ServerInfo", s->p, ExpServer(kMacA, 2, 4, 3, MkName("Host"), g.net.NowTicks()));
    // BR: StopScan, StartFakeSession, then DeleteScanInfo of the old entries
    r = g.Fk({0x00060000});
    WORDS(r, 0x00060040, 0);
    g.net.connect_result = 1;
    g.net.BecomeClient(3, 0x1111, "Host", 0x2222, "Brawler");
    const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x001C2101, 0x00040001});
    // the stub answers "joined" at the first poll: the reply is either immediate or pending
    if (jp.out == DlpOutcome::Done) {
        WORDS(jp, 0x000B0040, 0);
    } else {
        u32 cmd[kCmdWords] = {};
        OK(g.svc.PollPending(cmd));
        EQ(cmd[0], 0x000B0040u);
        EQ(cmd[1], 0u);
    }
    EQ(g.Status(), 0x05010003u);
    // GetConnectingNodes + GetNodeInfo(id) in the session: nodes 1 and 3
    g.DropArenas();
    Arena* n = g.Add(kSbAddr, 8);
    Sb sbn(StaticBufferDesc(8, 0), kSbAddr);
    r = g.Fk({0x000D0040, 4}, sbn.w);
    WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(4, 0), kSbAddr);
    OK(n->p[0] == 1 && n->p[1] == 0 && n->p[2] == 3 && n->p[3] == 0);
    r = g.Fk({0x000E0040, 3});
    WORDS(r, 0x000E02C0, 0);
    BYTES("BR NodeInfo 3", NodeBytes(r).data(), ExpNode(0x2222, MkName("Brawler"), 3));
    r = g.Fk({0x00100000});
    WORDS(r, 0x00100040, 0);
    r = g.Fk({0x00020000});
    WORDS(r, 0x00020040, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. The three records, byte by byte.

void TestRecords() {
    SECTION("records: DLPTitleInfo / DLPServerInfo / DLPNodeInfo byte by byte");
    {
        // a full group: every field non-zero, the buffer garbage-filled to prove the record is zeroed first
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        const Mac mac = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
        HeardGroup hg = MkGroup(mac, 0x0010C0FF, "", 4, 4, 1);
        const Nm host = MkNameU({0x30A2, 0x0041, 0xD83D, 0xDE00, 0x00E9, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046});
        std::memcpy(hg.announce.name, host.u, 20);
        g.net.heard.assign(1, hg);
        g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
        Arena* t = g.Add(kSbAddr, 0x13A8, 3, 5, 0xCC);  // an unaligned buffer with canaries
        Sb sbt(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sbt.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        BYTES("full TitleInfo", t->p, ExpTitle(0x10C0, 0xFF, mac));
        OK(t->CanariesOk());
        Rig& gg = g;
        gg.DropArenas();
        Arena* s = gg.Add(kSbAddr, 0x298, 1, 7, 0xCC);
        Sb sbs(kStaticServer, kSbAddr);
        r = gg.Fk({0x00070080, MW1(mac), MW2(mac)}, sbs.w);
        WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
        BYTES("full ServerInfo", s->p, ExpServer(mac, 1, 4, 4, host, gg.net.NowTicks()));
        OK(s->CanariesOk());
        // spot checks of the hand offsets (not through the helper)
        EQ(s->p[0x07], 1u);
        EQ(s->p[0x08], 4u);
        EQ(s->p[0x09], 4u);
        EQ(s->p[0x1C], 0xA2u);  // 0x30A2 low byte, name of the host at +0x1C
        EQ(s->p[0x1D], 0x30u);
        for (u32 i = 0x10; i < 0x1C; ++i) {
            EQ(s->p[i], 0u);  // node_info[0] seed + pad
        }
        for (u32 i = 0x30; i < 0x290; ++i) {
            EQ(s->p[i], 0u);  // the other 15 nodes, and node 0's tail, are zero
        }
    }
    {
        // a "minimal" group: 2 nodes max, 1 present, empty name, signal 0
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        const Mac mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x07};
        g.net.heard.assign(1, MkGroup(mac, 0x0010C000, "", 2, 1, 0));
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});  // filter 0: no filter
        Arena* t = g.Add(kSbAddr, 0x13A8, 0, 0, 0xCC);
        Sb sbt(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sbt.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        BYTES("empty TitleInfo", t->p, ExpTitle(0x10C0, 0, mac));
        g.DropArenas();
        Arena* s = g.Add(kSbAddr, 0x298, 0, 0, 0xCC);
        Sb sbs(kStaticServer, kSbAddr);
        r = g.Fk({0x00070080, MW1(mac), MW2(mac)}, sbs.w);
        WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
        BYTES("empty ServerInfo", s->p, ExpServer(mac, 0, 2, 1, MkName(""), g.net.NowTicks()));
    }
    {
        // the node record through both services: seed, name at +0x0C, node id u32 at +0x24, all else zero
        Rig g(kTd);
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x11, 0x12});
        g.Sv({0x00050080, 0, 6});
        const Nm nm = MkNameU({0x0048, 0x30A2, 0x0000, 0x0049, 0xFFFF, 0, 0, 0, 0, 0x0001});
        g.net.AddNode(3, 0x8877665544332211ull, "x");
        std::memcpy(g.net.entries[3].name, nm.u, 20);
        Rep r = g.Sv({0x00030000});
        EQ(r.w[2], 2u);
        r = g.Sv({0x000C0040, 3});
        WORDS(r, 0x000C02C0, 0);
        BYTES("SRVR NodeInfo", NodeBytes(r).data(), ExpNode(0x8877665544332211ull, nm, 3));
        // the host (node 1): kNoData with the FULL header and the host's own record
        r = g.Sv({0x000C0040, 1});
        WORDS(r, 0x000C02C0, kNoData);
        BYTES("host NodeInfo", NodeBytes(r).data(), ExpNode(kSeed, MkName("Kiri"), 1));
        // an unknown node: a plain error reply
        r = g.Sv({0x000C0040, 4});
        ERRW(r, 0xC, kNoData);
        r = g.Sv({0x000C0040, 0});
        ERRW(r, 0xC, kNoData);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. The tick at ServerInfo+0x290 (DESIGN 5.2).

void TestTick() {
    SECTION("ServerInfo+0x290: the tick of the last beacon");
    auto run = [](u32 age_ms, bool freeze, u64 base_now, bool expect_present) {
        Rig g(kTd);
        g.net.now = base_now;
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard.assign(1, MkGroup(kMacA, 0x0010C002, "HostName", 4, 2));
        g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
        g.Status();          // the pump reads the group: last_seen = now
        g.net.heard.clear();  // it is not heard any more
        if (freeze) {
            g.Fk({0x00060000}, nullptr, 0);  // StopScan
        }
        Arena* s = g.Add(kSbAddr, 0x298);
        Sb sbs(kStaticServer, kSbAddr);
        const Rep r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, sbs.w, age_ms);
        if (expect_present) {
            WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
            const u64 want = g.net.NowTicks() - static_cast<u64>(age_ms) * kTicksPerMs;
            const u64 got = static_cast<u64>(G32(s->p + 0x290)) | (static_cast<u64>(G32(s->p + 0x294)) << 32);
            EQ(got, want);
        } else {
            ERRW(r, 7, kNoData);
        }
    };
    run(0, false, 10000, true);
    run(1000, false, 10000, true);
    run(1400, false, 10000, true);
    run(1499, false, 10000, true);
    run(1500, false, 10000, false);  // in scan state a voice not heard for kGroupTtlMs is gone
    run(3900, true, 10000, true);    // frozen by StopScan: the entry stays, the tick says how old
    run(5100, true, 10000, true);
    run(60000, true, 10000, true);
    run(1000, false, 0x100000000ull + 500000, true);  // NowMs() above 2^32
    run(3900, true, 0x100000000ull + 500000, true);
    run(1000, false, 0xFFFFFF00ull - 100, true);      // the 32-bit clock wraps in the middle
    run(3000, true, 0xFFFFFF00ull - 100, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. max_beacons, filters.

void TestMaxBeaconsAndFilters() {
    SECTION("FKCL Initialize [2] (max beacons, never above 4) and the filter of StartScan");
    const Mac macs[6] = {kMacA, kMacB, kMacC, kMacD, kMacE, kMacF};
    auto count = [&](u32 beacons, int heard_groups) {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, beacons, 0x04000000, 1, 2});
        g.net.heard.clear();
        for (int i = 0; i < heard_groups; ++i) {
            g.net.heard.push_back(MkGroup(macs[i], 0x0010C000, "H", 4, 1));
        }
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        int n = 0;
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        while (r.w[1] == 0 && n < 20) {
            ++n;
            r = g.Fk({0x00090040, 0}, sb.w);
        }
        EQ(r.w[1], kNoData);
        (void)a;
        return n;
    };
    EQ(count(16, 6), 4);
    EQ(count(4, 6), 4);
    EQ(count(3, 6), 3);
    EQ(count(2, 6), 2);
    EQ(count(1, 6), 1);
    EQ(count(0, 6), 1);
    EQ(count(0xFFFFFFFFu, 6), 4);
    EQ(count(16, 2), 2);

    auto visible = [&](u32 f_lo, u32 f_hi, u32 child_low, u32 mac_lo = 0, u32 mac_hi = 0) {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard.assign(1, MkGroup(kMacA, child_low, "H", 4, 1));
        g.Fk({0x00050180, 0x0421, f_lo, f_hi, mac_lo, mac_hi, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        (void)a;
        Sb sb(kStaticTitle, kSbAddr);
        const Rep r = g.Fk({0x00090040, 1}, sb.w);
        return r.w[1] == 0;
    };
    OK(visible(0x0010C000, 0x00040001, 0x0010C002));    // the low byte (child index) of the filter is ignored
    OK(visible(0x0010C0FF, 0x00040001, 0x0010C002));
    OK(visible(0x0010C000, 0x00040001, 0x0010C000));
    OK(visible(0, 0, 0x0010C002));                      // no filter
    OK(!visible(0x0010C100, 0x00040001, 0x0010C002));   // another unique id
    OK(!visible(0x0010C000, 0x00040000, 0x0010C002));   // another high word
    OK(!visible(0x0010C000, 0x00040001, 0x001D1900));   // a group of another game: never visible, not even without filter
    OK(!visible(0, 0, 0x001D1900));
    OK(visible(0x0010C000, 0x00040001, 0x0010C002, 0x11223344, 0x5566));  // the MAC filter is read and never used
}

// ---------------------------------------------------------------------------------------------------------------------
// 7. SRVR flows.

void TestSrvrFlowAuto() {
    SECTION("SRVR: Triple Deluxe / Battle Royale creator flow, exact words");
    Rig g(kTd);
    Rep r;
    Arena* mw = g.Add(kMwAddr, 8);  // the mapped block of GetConnectingClients (valid for the whole test)
    // before Initialize
    r = g.Sv({0x00030000});
    WORDS(r, 0x00030080, 0, 0);
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    ERRW(r, 0xB, kInvalidState);
    OK(mw->All(0xEE));
    r = g.Sv({0x00050080, 0, 6});
    ERRW(r, 5, kInvalidState);
    r = g.Sv({0x00050080, 0, 5});
    ERRW(r, 5, kOutOfRange);  // the channel is checked first
    r = g.Sv({0x00070000});
    ERRW(r, 7, kInvalidState);
    r = g.Sv({0x000800C0, 1, 2, 3});
    ERRW(r, 8, kInvalidState);

    // Initialize (TD): [2] = 3 clients, [3] = process id, [4] = child index (not written by TD: 0)
    r = g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x00D00001, 0x00D00002});
    WORDS(r, 0x00010040, 0);
    r = g.Sv({0x00100000});
    WORDS(r, 0x00100080, 0, 0);
    EQ(g.SrvState(), 1u);
    r = g.Sv({0x00030000});
    WORDS(r, 0x00030080, 0, 1);
    r = g.Sv({0x00040000});
    WORDS(r, 0x00040200, 0, 0, 0, 0, 0, 0, 0, 0);
    r = g.Sv({0x000E0040, 0xE});  // IsChild
    WORDS(r, 0x000E0080, 0, 0);
    // StartAccepting(0, 6)
    r = g.Sv({0x00050080, 0, 6});
    WORDS(r, 0x00050040, 0);
    EQ(g.net.host_open_calls, 1);
    EQ(g.net.last_channel, 6u);
    EQ(g.net.last_announce.max_nodes, 4u);
    EQ(g.net.last_announce.child_tid_low, 0x0010C000u);
    EQ(g.net.last_announce.accepting, 1u);
    OK(SameName(g.net.last_announce.name, MkName("Kiri")));
    EQ(g.SrvState(), 2u);
    r = g.Sv({0x00050080, 0, 6});  // twice
    ERRW(r, 5, kInvalidState);

    // two clients arrive: the pump of the next command tracks them
    g.net.AddNode(2, 0x2222, "Alice");
    g.net.AddNode(3, 0x3333, "Bob");
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    WORDS(r, 0x000B0082, 0, 2, MappedWriteDesc(8), kMwAddr);
    OK(mw->p[0] == 2 && mw->p[1] == 0 && mw->p[2] == 3 && mw->p[3] == 0);
    for (u32 i = 4; i < 8; ++i) {
        EQ(mw->p[i], 0xEE);
    }
    r = g.Sv({0x000C0040, 2});
    WORDS(r, 0x000C02C0, 0);
    BYTES("Alice", NodeBytes(r).data(), ExpNode(0x2222, MkName("Alice"), 2));
    r = g.Sv({0x000C0040, 3});
    BYTES("Bob", NodeBytes(r).data(), ExpNode(0x3333, MkName("Bob"), 3));
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x05010002, 0, 0, 0, 0);

    // StartDistribution: state 6, START {1,0} to the accepted clients
    r = g.Sv({0x00070000});
    WORDS(r, 0x00070040, 0);
    EQ(g.SrvState(), 6u);
    EQ(g.net.CountSent(2, {1, 0}) >= 1, 1);
    EQ(g.net.CountSent(3, {1, 0}) >= 1, 1);
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x06010002, 0, 0, 0, 0);
    r = g.Sv({0x000800C0, 1, 2, 3});  // state 6: too early
    ERRW(r, 8, kInvalidState);
    g.net.Inbound(2, {2, 0});
    EQ(g.SrvState(), 6u);  // one ack is not enough
    g.net.Inbound(3, {2, 0});
    EQ(g.SrvState(), 8u);
    r = g.Sv({0x000D0040, 3});
    WORDS(r, 0x000D0180, 0, 0x07010003, 0, 0, 0, 0);

    // SendWirelessRebootPassphrase at every frame: the FIRST passphrase wins, the others are invalid state (never -1)
    r = g.Sv({0x000800C0, 0x14131211, 0x18171615, 0xFFFFFF19});  // garbage in the padding bytes
    WORDS(r, 0x00080040, 0);
    const std::vector<u8> go = {3, 9, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
    int go2 = 0, go3 = 0;
    for (const auto& s : g.net.sent) {
        if (s.data == go && s.to == 2) {
            ++go2;
        }
        if (s.data == go && s.to == 3) {
            ++go3;
        }
    }
    OK(go2 >= 1 && go3 >= 1);
    for (int i = 0; i < 20; ++i) {
        r = g.Sv({0x000800C0, 0xAAAAAAAAu + i, 0xBBBBBBBBu + i, 0xCCCCCCCCu + i});
        ERRW(r, 8, kInvalidState);
    }
    for (const auto& s : g.net.sent) {
        if (!s.data.empty() && s.data[0] == 3) {
            OK(s.data == go);  // every GO ever sent carries the first passphrase
        }
    }
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x07010002, 0, 0, 0, 0);
    g.net.Inbound(2, {4, 0});
    g.net.Inbound(3, {4, 0});
    // state 9 and zero clients; the virtual network is closed before the game sees 9
    EQ(g.SrvState(), 9u);
    OK(g.net.host_close_calls >= 1);
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    WORDS(r, 0x000B0082, 0, 0, MappedWriteDesc(8), kMwAddr);
    r = g.Sv({0x000800C0, 1, 2, 3});
    ERRW(r, 8, kInvalidState);
    r = g.Sv({0x00070000});
    ERRW(r, 7, kInvalidState);
    r = g.Sv({0x00020000});  // Finalize
    WORDS(r, 0x00020040, 0);
    r = g.Sv({0x00030000});
    WORDS(r, 0x00030080, 0, 0);  // defect 4 of Azahar: state 0 after Finalize
    r = g.Sv({0x00020000});
    WORDS(r, 0x00020040, 0);
}

void TestSrvrManualAndEdges() {
    SECTION("SRVR: manual accept, kick, EndAccepting");
    Rig g(kTd);
    EnterSrvr(g, kV2, true);
    g.net.AddNode(2, 0x2222, "Alice");
    g.net.AddNode(3, 0x3333, "Bob");
    Arena* mw = g.Add(kMwAddr, 8);
    Rep r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    WORDS(r, 0x000B0082, 0, 2, MappedWriteDesc(8), kMwAddr);  // waiting clients are listed too
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x40000002, 0, 0, 0, 0);
    r = g.Sv({0x00090040, 1});  // the host cannot be accepted
    ERRW(r, 9, kNoData);
    r = g.Sv({0x00090040, 7});
    ERRW(r, 9, kNoData);
    r = g.Sv({0x00090040, 0x0102});  // 0x0102 is NOT node 2
    ERRW(r, 9, kNoData);
    r = g.Sv({0x00090040, 2});
    WORDS(r, 0x00090040, 0);
    r = g.Sv({0x00090040, 2});
    ERRW(r, 9, kNoData);
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x05010002, 0, 0, 0, 0);
    // DisconnectClient: the node 3 is kicked; unknown nodes are NoData (never -1)
    r = g.Sv({0x000A0040, 9});
    ERRW(r, 0xA, kNoData);
    r = g.Sv({0x000A0040, 1});
    ERRW(r, 0xA, kNoData);
    r = g.Sv({0x000A0040, 3});
    WORDS(r, 0x000A0040, 0);
    OK(std::find(g.net.kicked.begin(), g.net.kicked.end(), 3) != g.net.kicked.end());
    // EndAccepting: success in any state; the client inside stays, the network stays
    const int closes = g.net.host_close_calls;
    r = g.Sv({0x00060000});
    WORDS(r, 0x00060040, 0);
    EQ(g.SrvState(), 1u);
    EQ(g.net.host_close_calls, closes);
    EQ(g.net.host_update_calls, 1);
    EQ(g.net.last_announce.accepting, 0u);
    r = g.Sv({0x000D0040, 2});
    WORDS(r, 0x000D0180, 0, 0x05010002, 0, 0, 0, 0);
    // and StartAccepting again re-opens the announcement (HostOpen once, HostUpdate twice)
    r = g.Sv({0x00050080, 0, 6});
    WORDS(r, 0x00050040, 0);
    EQ(g.net.host_open_calls, 1);
    EQ(g.net.host_update_calls, 2);
    EQ(g.net.last_announce.accepting, 1u);
    (void)mw;
    // EndAccepting in a state where there is nothing to end
    Rig h(kTd);
    r = h.Sv({0x00060000});
    WORDS(r, 0x00060040, 0);
    // StartDistribution with a client still waiting: it is thrown out, START goes to the accepted only
    Rig k(kTd);
    EnterSrvr(k, kV2, true);
    k.net.AddNode(2, 0x2222, "A");
    k.net.AddNode(3, 0x3333, "B");
    k.Sv({0x00030000});
    k.Sv({0x00090040, 2});
    r = k.Sv({0x00070000});
    WORDS(r, 0x00070040, 0);
    OK(std::find(k.net.kicked.begin(), k.net.kicked.end(), 3) != k.net.kicked.end());
    OK(k.net.CountSent(2, {1, 0}) >= 1);
    OK(k.net.CountSent(3, {1, 0}) == 0);
}

void TestSrvrWidths() {
    SECTION("SRVR: Initialize / StartAccepting widths (garbage in the high bytes)");
    // max clients: the low byte of [2], limited to 1..3, announced as +1
    struct M {
        u32 req;
        u32 nodes;
    };
    const M cases[] = {{0, 2},          {1, 2},    {2, 3},          {3, 4},     {4, 4},        {15, 4},
                       {0xFF, 4},       {0x100, 2}, {0x0403, 4},     {0xFFFFFF01u, 2}, {0x12340002u, 3}};
    for (const M& c : cases) {
        Rig g(kTd);
        Rep r = g.Sv({0x00010183, 0x85000, c.req, 0xE, 0, 0x40000, 2, 0x04000000, 0x11, 0x12});
        WORDS(r, 0x00010040, 0);
        r = g.Sv({0x00050080, 0, 6});
        WORDS(r, 0x00050040, 0);
        EQ(g.net.last_announce.max_nodes, c.nodes);
    }
    // child index: the low byte of [4] enters the child title id (defect 6 of Azahar)
    struct Ix {
        u32 req;
        u32 tid;
    };
    const Ix idx[] = {{0, 0x0010C000}, {2, 0x0010C002}, {0xFF, 0x0010C0FF}, {0x12345678, 0x0010C078}, {0x100, 0x0010C000}};
    for (const Ix& c : idx) {
        Rig g(kTd);
        g.Sv({0x00010183, 0x85000, 3, 0xE, c.req, 0x40000, 2, 0x04000000, 0x11, 0x12});
        g.Sv({0x00050080, 0, 6});
        EQ(g.net.last_announce.child_tid_low, c.tid);
        // and a searcher scanning with child index 0 sees it
    }
    // channels
    struct Ch {
        u32 w2;
        bool ok;
        u32 ch;
    };
    const Ch chans[] = {{0, true, 0},          {1, true, 1},           {6, true, 6},         {11, true, 11},
                        {0x03a12006u, true, 6}, {0x00000106u, true, 6}, {0x03a12005u, false, 0}, {2, false, 0},
                        {12, false, 0},        {0x100, true, 0},       {0xFF, false, 0},     {0x0B0B0B0Bu, true, 11}};
    for (const Ch& c : chans) {
        Rig g(kTd);
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x11, 0x12});
        const Rep r = g.Sv({0x00050080, 0, c.w2});
        if (c.ok) {
            WORDS(r, 0x00050040, 0);
            EQ(g.net.last_channel, c.ch);
        } else {
            ERRW(r, 5, kOutOfRange);
            EQ(g.net.host_open_calls, 0);
            EQ(g.SrvState(), 1u);
        }
    }
    // manual: a byte != 0 (garbage above the low byte is not read)
    {
        Rig g(kTd);
        EnterSrvr(g, kV1);
        g.Sv({0x00050080, 0x01020300u, 6});  // low byte 0: automatic
        g.net.AddNode(2, 0x2222, "A");
        EQ(g.Sv({0x000D0040, 2}).w[2], 0x05010002u);
    }
    {
        Rig g(kTd);
        EnterSrvr(g, kV1);
        g.Sv({0x00050080, 0x00000101u, 6});  // low byte 1: manual
        g.net.AddNode(2, 0x2222, "A");
        EQ(g.Sv({0x000D0040, 2}).w[2], 0x40000002u);
    }
    // node-id widths with node 2 present: 0x0102 is not node 2; 0x10002 is node 2 (the design's rule & 0xFFFF)
    {
        Rig g(kTd);
        EnterSrvr(g, kV2);
        g.net.AddNode(2, 0x2222, "A");
        g.Sv({0x00030000});
        Rep r = g.Sv({0x000C0040, 0x0102});
        ERRW(r, 0xC, kNoData);
        r = g.Sv({0x000D0040, 0x0102});
        ERRW(r, 0xD, kNoData);
        r = g.Sv({0x000A0040, 0x0102});
        ERRW(r, 0xA, kNoData);
        r = g.Sv({0x000C0040, 0x10002});  // & 0xFFFF = 2
        WORDS(r, 0x000C02C0, 0);
        r = g.Sv({0x000D0040, 0xABCD0002u});
        WORDS(r, 0x000D0180, 0, 0x05010002, 0, 0, 0, 0);
        r = g.Sv({0x000D0040, 0});
        ERRW(r, 0xD, kNoData);
        r = g.Sv({0x000D0040, 1});
        ERRW(r, 0xD, kNoData);  // defect 3 of Azahar: node < 2 must not index out of the vector
        r = g.Sv({0x000D0040, 0xFFFF});
        ERRW(r, 0xD, kNoData);
        r = g.Sv({0x000D0040, 5});
        ERRW(r, 0xD, kNoData);
    }
    // GetConnectingClients: n and the declared size bound what is written; the total is the real one
    {
        Rig g(kTd);
        EnterSrvr(g, kV2);
        g.net.AddNode(2, 0x2222, "A");
        g.net.AddNode(3, 0x3333, "B");
        g.net.AddNode(4, 0x4444, "C");
        g.Sv({0x00030000});
        struct Gc {
            u32 n;
            u32 bytes;
            u32 written;
        };
        const Gc gcs[] = {{0, 8, 0}, {1, 8, 1}, {2, 8, 2}, {3, 8, 3}, {4, 8, 3}, {0xFFFF, 8, 3}, {0x10003, 8, 3}, {4, 2, 1}, {4, 3, 1},
                          {4, 4, 2}, {4, 6, 3}, {4, 0, 0}, {0xFFFF, 0x20, 3}};
        for (const Gc& c : gcs) {
            g.DropArenas();
            const u32 sz = std::max<u32>(c.bytes, 1);
            Arena* a = g.Add(kMwAddr, sz, 0, 8, 0xEE);
            const Rep r = g.Sv({0x000B0042, c.n, MappedWriteDesc(c.bytes), kMwAddr});
            WORDS(r, 0x000B0082, 0, 3, MappedWriteDesc(c.bytes), kMwAddr);
            OK(a->CanariesOk());
            for (u32 i = 0; i < sz; ++i) {
                if (i < c.written * 2) {
                    EQ(a->p[i], (i % 2 == 0) ? (i / 2 + 2) : 0);
                } else {
                    EQ(a->p[i], 0xEE);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 8. Buffers of the game: absent, wrong, too small.

void TestPointersFkcl() {
    SECTION("FKCL buffers: absent / wrong descriptor / too small / unmapped -> 0xD8E0AFF6, nothing written");
    struct Cmd {
        u32 id;
        u32 size;
        u32 words[5];
        u32 n;
    };
    // the three record commands, each with a state that would otherwise succeed
    const Cmd cmds[] = {{7, 0x298, {0x00070080, MW1(kMacA), MW2(kMacA)}, 3},
                        {8, 0x13A8, {0x00080100, MW1(kMacA), MW2(kMacA), 0, 0}, 5},
                        {9, 0x13A8, {0x00090040, 1}, 2}};
    for (const Cmd& c : cmds) {
        auto fresh = [&](Rig& g) {
            g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
            g.net.heard.assign(1, MkGroup(kMacA, 0x0010C002, "HostName", 4, 2));
            g.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
            g.Status();
        };
        auto call = [&](Rig& g, const u32* sb) {
            std::vector<u32> w(c.words, c.words + c.n);
            return g.Call(DlpKind::Fkcl, w.data(), c.n, sb);
        };
        const u32 good = StaticBufferDesc(c.size, 0);
        {
            Rig g(kTd);
            fresh(g);
            const u32 bad0 = g.svc.bad_pointer();
            Rep r = call(g, nullptr);  // no static buffer at all
            ERRW(r, c.id, kInvalidPointer);
            EQ(g.svc.bad_pointer(), bad0 + 1);
            EQ(g.svc.last_result(), kInvalidPointer);
            // wrong descriptor type, wrong buffer id, zero address, address in nothing, capacity one byte short
            Arena* a = g.Add(kSbAddr, c.size, 0, 16, 0xEE);
            const Sb bad[] = {Sb((c.size << 14) | 0, kSbAddr),           Sb((c.size << 14) | 0xC, kSbAddr),
                              Sb(StaticBufferDesc(c.size, 1), kSbAddr), Sb(good, 0),
                              Sb(good, kSbAddr + 0x10000),               Sb(StaticBufferDesc(c.size - 1, 0), kSbAddr),
                              Sb(StaticBufferDesc(0, 0), kSbAddr),       Sb(StaticBufferDesc(c.size / 2, 0), kSbAddr),
                              Sb(0, 0),                                  Sb(0xFFFFFFFFu, kSbAddr)};
            for (const Sb& b : bad) {
                r = call(g, b.w);
                ERRW(r, c.id, kInvalidPointer);
                OK(a->All(0xEE));
                OK(a->CanariesOk());
            }
            // the same, once the block is smaller than the declared capacity: map() fails, nothing written
            g.DropArenas();
            Arena* small = g.Add(kSbAddr, c.size - 1, 0, 16, 0xEE);
            Sb declared(good, kSbAddr);
            r = call(g, declared.w);
            ERRW(r, c.id, kInvalidPointer);
            OK(small->All(0xEE));
            OK(small->CanariesOk());
            // and the very first valid call afterwards still sees the first entry: no side effect of the failed ones
            g.DropArenas();
            Arena* ok = g.Add(kSbAddr, c.size, 0, 0, 0xEE);
            r = call(g, declared.w);
            WORDS(r, (c.id << 16) | 0x42, 0, good, kSbAddr);
            OK(!ok->All(0xEE));
            BYTES("after the failed ones", ok->p,
                  c.id == 7 ? ExpServer(kMacA, 3, 4, 2, MkName("HostName"), g.net.NowTicks())
                            : ExpTitle(0x10C0, 2, kMacA));
        }
        {
            // capacity larger than the record is fine, the reply tells the size WRITTEN
            Rig g(kTd);
            fresh(g);
            Arena* a = g.Add(kSbAddr, c.size + 64, 0, 16, 0xEE);
            Sb big(StaticBufferDesc(c.size + 64, 0), kSbAddr);
            const Rep r = call(g, big.w);
            WORDS(r, (c.id << 16) | 0x42, 0, good, kSbAddr);
            for (u32 i = c.size; i < c.size + 64; ++i) {
                EQ(a->p[i], 0xEE);
            }
            OK(a->CanariesOk());
        }
    }
    // GetTitleInfoInOrder: a failed call does not advance the cursor (two entries; the bad call is between the good ones)
    {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard = {MkGroup(kMacA, 0x0010C002, "A", 4, 2), MkGroup(kMacB, 0x0010C000, "B", 4, 1)};
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        BYTES("first", a->p, ExpTitle(0x10C0, 2, kMacA));
        r = g.Fk({0x00090040, 0}, nullptr);
        ERRW(r, 9, kInvalidPointer);
        r = g.Fk({0x00090040, 0}, Sb(kStaticTitle, 0).w);
        ERRW(r, 9, kInvalidPointer);
        r = g.Fk({0x00090040, 0}, sb.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        BYTES("second", a->p, ExpTitle(0x10C0, 0, kMacB));
        r = g.Fk({0x00090040, 0}, sb.w);
        ERRW(r, 9, kNoData);
        // and even when there is nothing to return (end of list), a bad buffer is reported first
        r = g.Fk({0x00090040, 0}, nullptr);
        ERRW(r, 9, kInvalidPointer);
    }
    // GetConnectingNodes in a session: absent / wrong buffer -> invalid pointer; a short capacity writes fewer ids
    {
        Rig g(kTd);
        EnterFkcl(g, kS5);
        Arena* a = g.Add(kSbAddr, 8, 0, 8, 0xEE);
        Rep r = g.Fk({0x000D0040, 4}, nullptr);
        ERRW(r, 0xD, kInvalidPointer);
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(8, 1), kSbAddr).w);
        ERRW(r, 0xD, kInvalidPointer);
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(8, 0), 0).w);
        ERRW(r, 0xD, kInvalidPointer);
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(8, 0), kSbAddr + 0x5000).w);
        ERRW(r, 0xD, kInvalidPointer);
        OK(a->All(0xEE));
        // capacity 3: one id (cap/2); capacity 1: none
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(3, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(2, 0), kSbAddr);
        OK(a->p[0] == 1 && a->p[1] == 0 && a->p[2] == 0xEE);
        std::memset(a->p, 0xEE, 8);
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(1, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(0, 0), kSbAddr);
        OK(a->All(0xEE));
        // n bounds it too
        r = g.Fk({0x000D0040, 1}, Sb(StaticBufferDesc(8, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(2, 0), kSbAddr);
        OK(a->p[0] == 1 && a->p[1] == 0 && a->p[2] == 0xEE);
        std::memset(a->p, 0xEE, 8);
        r = g.Fk({0x000D0040, 0}, Sb(StaticBufferDesc(8, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(0, 0), kSbAddr);
        OK(a->All(0xEE));
        for (u32 n : {0xFFFFu, 0x1000Cu, 0xFFFFFFFFu, 5u, 100u}) {
            std::memset(a->p, 0xEE, 8);
            r = g.Fk({0x000D0040, n}, Sb(StaticBufferDesc(8, 0), kSbAddr).w);
            WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(4, 0), kSbAddr);  // never more than the 2 present (and never > 4)
            OK(a->p[0] == 1 && a->p[2] == 2 && a->p[4] == 0xEE);
            OK(a->CanariesOk());
        }
        // a game-sized static buffer of 0x1000 bytes with the biggest n: still at most 4 ids (8 bytes)
        g.DropArenas();
        Arena* big = g.Add(kSbAddr, 0x1000, 0, 16, 0xEE);
        r = g.Fk({0x000D0040, 0xFFFF}, Sb(StaticBufferDesc(0x1000, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 2, StaticBufferDesc(4, 0), kSbAddr);
        for (u32 i = 4; i < 0x1000; ++i) {
            EQ(big->p[i], 0xEE);
        }
    }
    // GetConnectingNodes outside a session: success with total 0 and no write
    {
        Rig g(kTd);
        EnterFkcl(g, kS1);
        Arena* a = g.Add(kSbAddr, 8, 0, 8, 0xEE);
        const Rep r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(8, 0), kSbAddr).w);
        WORDS(r, 0x000D0082, 0, 0, StaticBufferDesc(0, 0), kSbAddr);
        OK(a->All(0xEE));
    }
}

void TestPointersSrvr() {
    SECTION("SRVR GetConnectingClients: the mapped-write buffer of the request");
    Rig g(kTd);
    EnterSrvr(g, kV2);
    g.net.AddNode(2, 0x2222, "A");
    g.net.AddNode(3, 0x3333, "B");
    g.Sv({0x00030000});
    const u32 bad0 = g.svc.bad_pointer();
    // descriptor not 0xC: invalid pointer
    Rep r = g.Sv({0x000B0042, 4, 0x80, kMwAddr});
    ERRW(r, 0xB, kInvalidPointer);
    r = g.Sv({0x000B0042, 4, 0, 0});  // a truncated request: zeros after the header
    ERRW(r, 0xB, kInvalidPointer);
    r = g.Sv({0x000B0042, 4, 0x8D, kMwAddr});
    ERRW(r, 0xB, kInvalidPointer);
    // right descriptor, nothing mapped there
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    ERRW(r, 0xB, kInvalidPointer);
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), 0});
    ERRW(r, 0xB, kInvalidPointer);
    EQ(g.svc.bad_pointer(), bad0 + 5);
    // a block smaller than the declared size
    Arena* a = g.Add(kMwAddr, 4, 0, 8, 0xEE);
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
    ERRW(r, 0xB, kInvalidPointer);
    OK(a->All(0xEE));
    OK(a->CanariesOk());
    // exact-size block of 2 bytes with 2 clients: ONE id
    g.DropArenas();
    a = g.Add(kMwAddr, 2, 0, 8, 0xEE);
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(2), kMwAddr});
    WORDS(r, 0x000B0082, 0, 2, MappedWriteDesc(2), kMwAddr);
    OK(a->p[0] == 2 && a->p[1] == 0);
    OK(a->CanariesOk());
    // a declared size of 0 needs no pointer (nothing to write): success with the real total
    r = g.Sv({0x000B0042, 4, MappedWriteDesc(0), 0});
    WORDS(r, 0x000B0082, 0, 2, MappedWriteDesc(0), 0);
    // an unaligned block (UBSan alignment)
    for (u32 skew = 1; skew <= 3; ++skew) {
        g.DropArenas();
        Arena* u = g.Add(kMwAddr, 8, skew, 8, 0xEE);
        r = g.Sv({0x000B0042, 3, MappedWriteDesc(8), kMwAddr});
        WORDS(r, 0x000B0082, 0, 2, MappedWriteDesc(8), kMwAddr);
        OK(u->p[0] == 2 && u->p[2] == 3 && u->p[4] == 0xEE);
        OK(u->CanariesOk());
    }
    // ... and the record commands on unaligned static buffers
    Rig f(kTd);
    EnterFkcl(f, kS2);
    for (u32 skew = 1; skew <= 3; ++skew) {
        f.DropArenas();
        Arena* t = f.Add(kSbAddr, 0x298, skew, 8, 0xCC);
        const Rep rr = f.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, Sb(kStaticServer, kSbAddr).w);
        WORDS(rr, 0x00070042, 0, kStaticServer, kSbAddr);
        BYTES("unaligned ServerInfo", t->p, ExpServer(kMacA, 3, 4, 2, MkName("HostName"), f.net.NowTicks()));
        OK(t->CanariesOk());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 9. Headers, unknown ids, truncated requests.

const u32 kFkOkHdr[0x12] = {0,          0x00010040, 0x00020040, 0x000301C0, 0x00040080, 0x00050040, 0x00060040, 0x00070042, 0x00080042,
                            0x00090042, 0x000A0040, 0x000B0040, 0x000C0180, 0x000D0082, 0x000E02C0, 0x000F0100, 0x00100040, 0x00110040};
const u32 kSvOkHdr[0x11] = {0,          0x00010040, 0x00020040, 0x00030080, 0x00040200, 0x00050040, 0x00060040, 0x00070040, 0x00080040,
                            0x00090040, 0x000A0040, 0x000B0082, 0x000C02C0, 0x000D0180, 0x000E0080, 0x000F0040, 0x00100080};

// A spec-exact request for each FKCL id (the MAC is kMacA, the node 2).
std::vector<u32> FkReq(u32 id) {
    const Nm nm = MkName("Name");
    u32 nw[5];
    NameWords(nm, nw);
    switch (id) {
    case 1: return {0x00010083, 0xA000, 2, 0x04000000, 0x11, 0x12};
    case 2: return {0x00020000};
    case 3: return {0x00030000};
    case 4: return {0x00040000};
    case 5: return {0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0};
    case 6: return {0x00060000};
    case 7: return {0x00070080, MW1(kMacA), MW2(kMacA)};
    case 8: return {0x00080100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001};
    case 9: return {0x00090040, 1};
    case 10: return {0x000A0080, MW1(kMacA), MW2(kMacA)};
    case 11: return {0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001};
    case 12: return {0x000C0000};
    case 13: return {0x000D0040, 4};
    case 14: return {0x000E0040, 2};
    case 15: return {0x000F0000};
    case 16: return {0x00100000};
    default: return {0x00110203, 0xA000, 2, nw[0], nw[1], nw[2], nw[3], nw[4], 0, 0x04000000, 0x11, 0x12};
    }
}
std::vector<u32> SvReq(u32 id) {
    const Nm nm = MkName("Name");
    u32 nw[5];
    NameWords(nm, nw);
    switch (id) {
    case 1: return {0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 0x11, 0x12};
    case 2: return {0x00020000};
    case 3: return {0x00030000};
    case 4: return {0x00040000};
    case 5: return {0x00050080, 0, 6};
    case 6: return {0x00060000};
    case 7: return {0x00070000};
    case 8: return {0x000800C0, 0x14131211, 0x18171615, 0x00000019};
    case 9: return {0x00090040, 2};
    case 10: return {0x000A0040, 2};
    case 11: return {0x000B0042, 4, MappedWriteDesc(8), kMwAddr};
    case 12: return {0x000C0040, 2};
    case 13: return {0x000D0040, 2};
    case 14: return {0x000E0040, 0xE};
    case 15: return {0x000F0303, 0x85000, 3, 0xE, 0, 0x40000, 2, nw[0], nw[1], nw[2], nw[3], nw[4], 0, 0x04000000, 0x11, 0x12};
    default: return {0x00100000};
    }
}

void TestUnknownAndHeaders() {
    SECTION("unknown ids, wrong word counts in the header, truncated requests");
    // unknown ids: [0x40, 0xD9001830] in both modes and both services; counted as refused AND unknown
    const u32 fk_unknown[] = {0x00, 0x12, 0x13, 0x1F, 0x20, 0x7F, 0xFF, 0x100, 0x1234, 0x7FFF, 0xFFFF};
    const u32 sv_unknown[] = {0x00, 0x11, 0x12, 0x20, 0xFF, 0x1234, 0xFFFF};
    for (int mode = 0; mode < 2; ++mode) {
        Rig g(kTd);
        DlpService probe;
        DlpService* s = mode ? &g.svc : &probe;
        u32 cmd[kCmdWords];
        u32 n_unknown = 0;
        for (DlpKind k : {DlpKind::Fkcl, DlpKind::Srvr}) {
            const auto& list = k == DlpKind::Fkcl ? std::vector<u32>(std::begin(fk_unknown), std::end(fk_unknown))
                                                   : std::vector<u32>(std::begin(sv_unknown), std::end(sv_unknown));
            for (u32 id : list) {
                for (u32 low : {0x00u, 0x40u, 0x83u, 0xFFFu}) {
                    for (u32 i = 0; i < kCmdWords; ++i) {
                        cmd[i] = 0x55550000u | i;
                    }
                    cmd[0] = (id << 16) | low;
                    const DlpOutcome o = s->Dispatch(k, cmd, nullptr, g.res);
                    ++n_unknown;
                    OK(o == DlpOutcome::Done);
                    EQ(cmd[0], 0x00000040u);
                    EQ(cmd[1], kBadHeader);
                    EQ(s->last_id(), id);
                    EQ(s->last_result(), kBadHeader);
                }
            }
        }
        EQ(s->unknown_commands(), n_unknown);
        EQ(s->refused(), n_unknown);
        EQ(s->commands(), n_unknown);
    }

    // the reply follows the id even when the header's word counts are wrong or missing (real games vary the header)
    const u32 lows[] = {0x0000, 0x0001, 0x0040, 0x0083, 0x0FFF, 0xFFC0, 0xFFFF, 0x0203, 0x0180};
    for (int kind = 0; kind < 2; ++kind) {
        const u32 last = kind == 0 ? 0x11 : 0x10;
        for (u32 id = 1; id <= last; ++id) {
            for (u32 low : lows) {
                // two identical rigs in an identical state; the first gets the exact request, the second a wrong header
                Rig a(kTd), b(kTd);
                if (kind == 0) {
                    EnterFkcl(a, kS5);
                    EnterFkcl(b, kS5);
                } else {
                    EnterSrvr(a, kV2);
                    EnterSrvr(b, kV2);
                    a.net.AddNode(2, 0x2222, "A");
                    b.net.AddNode(2, 0x2222, "A");
                    a.Sv({0x00030000});
                    b.Sv({0x00030000});
                }
                std::vector<u32> exact = kind == 0 ? FkReq(id) : SvReq(id);
                std::vector<u32> wrong = exact;
                wrong[0] = (id << 16) | low;
                Arena* aa = a.Add(kSbAddr, 0x13A8);
                Arena* bb = b.Add(kSbAddr, 0x13A8);
                Arena* am = a.Add(kMwAddr, 8);
                Arena* bm = b.Add(kMwAddr, 8);
                Sb sba(StaticBufferDesc(0x13A8, 0), kSbAddr);
                Sb sbb(StaticBufferDesc(0x13A8, 0), kSbAddr);
                // GetServerInfo wants a 0x298 descriptor in the TD sense; declaring 0x13A8 (bigger) is fine for all
                const DlpKind kd = kind == 0 ? DlpKind::Fkcl : DlpKind::Srvr;
                const Rep ra = a.Call(kd, exact.data(), static_cast<u32>(exact.size()), sba.w);
                const Rep rb = b.Call(kd, wrong.data(), static_cast<u32>(wrong.size()), sbb.w);
                OK(ra.out == rb.out);
                if (ra.out == DlpOutcome::Done && rb.out == DlpOutcome::Done) {
                    const u32 n = 1 + ((ra.w[0] >> 6) & 0x3F) + (ra.w[0] & 0x3F);
                    if (ra.w[1] == 0) {
                        EQ(ra.w[0], kind == 0 ? kFkOkHdr[id] : kSvOkHdr[id]);
                    } else {
                        EQ(ra.w[0], (id << 16) | 0x40u);
                    }
                    EQ(rb.w[0], ra.w[0]);
                    for (u32 i = 1; i < n && i < kCmdWords; ++i) {
                        EQ(rb.w[i], ra.w[i]);
                    }
                    OK(std::memcmp(aa->p, bb->p, 0x13A8) == 0);
                    OK(std::memcmp(am->p, bm->p, 8) == 0);
                }
            }
        }
    }

    // truncated requests: the header of the command and the rest of its words ZERO (what a game that leaves the tail of
    // its command buffer empty sends); the words are written out to their full length, zero-filled
    auto Z = [](u32 hdr, u32 n) {
        std::vector<u32> v(n, 0);
        v[0] = hdr;
        return v;
    };
    {
        Rig g(kTd);
        // FKCL
        Rep r = g.Call(DlpKind::Fkcl, Z(0x00010083, 6).data(), 6);
        WORDS(r, 0x00010040, 0);
        r = g.Call(DlpKind::Fkcl, Z(0x00050180, 7).data(), 7);
        WORDS(r, 0x00050040, 0);  // filter 0, MAC 0, channel mask 0: ignored
        r = g.Call(DlpKind::Fkcl, Z(0x00060000, 1).data(), 1);
        WORDS(r, 0x00060040, 0);
        r = g.Call(DlpKind::Fkcl, Z(0x00070080, 3).data(), 3, Sb(kStaticServer, kSbAddr).w);
        ERRW(r, 7, kInvalidPointer);  // nothing mapped
        g.Add(kSbAddr, 0x298);
        r = g.Call(DlpKind::Fkcl, Z(0x00070080, 3).data(), 3, Sb(kStaticServer, kSbAddr).w);
        ERRW(r, 7, kNoData);  // MAC 00:00:00:00:00:00
        r = g.Call(DlpKind::Fkcl, Z(0x000A0080, 3).data(), 3);
        ERRW(r, 0xA, kNoData);
        r = g.Call(DlpKind::Fkcl, Z(0x000E0040, 2).data(), 2);
        ERRW(r, 0xE, kNoData);
        r = g.Call(DlpKind::Fkcl, Z(0x0011, 1).data(), 1);  // an id with an empty header
        WORDS(r, 0x00000040, kBadHeader);
        r = g.Call(DlpKind::Fkcl, Z(0x00110203, 12).data(), 12);
        WORDS(r, 0x00110040, 0);
        // SRVR
        r = g.Call(DlpKind::Srvr, Z(0x00010183, 10).data(), 10);
        WORDS(r, 0x00010040, 0);  // max clients 0 -> 1
        r = g.Call(DlpKind::Srvr, Z(0x00050080, 3).data(), 3);
        WORDS(r, 0x00050040, 0);  // manual 0, channel 0
        EQ(g.net.last_channel, 0u);
        EQ(g.net.last_announce.max_nodes, 2u);
        r = g.Call(DlpKind::Srvr, Z(0x000B0042, 4).data(), 4);
        ERRW(r, 0xB, kInvalidPointer);
        r = g.Call(DlpKind::Srvr, Z(0x000C0040, 2).data(), 2);
        ERRW(r, 0xC, kNoData);
        r = g.Call(DlpKind::Srvr, Z(0x000D0040, 2).data(), 2);
        ERRW(r, 0xD, kNoData);
        r = g.Call(DlpKind::Srvr, Z(0x00090040, 2).data(), 2);
        ERRW(r, 9, kNoData);
        r = g.Call(DlpKind::Srvr, Z(0x000A0040, 2).data(), 2);
        ERRW(r, 0xA, kNoData);
        r = g.Call(DlpKind::Srvr, Z(0x000F0303, 16).data(), 16);
        WORDS(r, 0x000F0040, 0);
        r = g.Call(DlpKind::Srvr, Z(0x000800C0, 4).data(), 4);  // state 2: not 8
        ERRW(r, 8, kInvalidState);
        r = g.Call(DlpKind::Srvr, Z(0x000E0040, 2).data(), 2);  // IsChild(pid 0)
        WORDS(r, 0x000E0080, 0, 0);
    }
    {
        // a header ONLY, the tail left as the canary the test puts there (stale words of a former call): every command must
        // still answer in shape, without a crash, and never write beyond its reply
        for (int kind = 0; kind < 2; ++kind) {
            for (u32 id = 1; id <= (kind == 0 ? 0x11u : 0x10u); ++id) {
                Rig g(kTd);
                g.Add(kSbAddr, 0x13A8, 0, 8);
                g.Add(kMwAddr, 8, 0, 8);
                const u32 hdr = kind == 0 ? kFkOkHdr[id] & 0xFFFFu : kSvOkHdr[id] & 0xFFFFu;
                const u32 w[1] = {(id << 16) | hdr};
                const Rep r = g.Call(kind == 0 ? DlpKind::Fkcl : DlpKind::Srvr, w, 1, Sb(StaticBufferDesc(0x13A8, 0), kSbAddr).w);
                OK(r.out == DlpOutcome::Done || (kind == 0 && id == 0xB));
                if (r.out == DlpOutcome::Done) {
                    OK(r.w[1] == 0 || r.w[0] == ((id << 16) | 0x40u) || r.w[0] == 0x000C02C0u);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 10. The matrix command x state (DESIGN 4.5, the three lists).

void TestMatrixFkcl() {
    SECTION("FKCL matrix: every command in every state (lists S, T and the rest)");
    const bool in_s[0x12] = {false, true, true, true, true, false, true, false, false, false, false, false, true, true, false, true, true, true};
    const bool in_t[0x12] = {false, false, false, false, false, false, false, true, true, true, true, false, false, false, true, false, false, false};
    const int sts[] = {kS0, kS1, kS2, kS3, kS5, kS6, kS9, kS80};
    for (int st : sts) {
        for (u32 id = 1; id <= 0x11; ++id) {
            Rig g(kTd);
            EnterFkcl(g, st);
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), "FKCL matrix: state %s, id 0x%02x", kStName[st], id);
            std::snprintf(g_ctxbuf, sizeof(g_ctxbuf), "%s", ctx);
            g_ctx = g_ctxbuf;
            g.Add(kSbAddr, 0x13A8, 0, 8, 0xEE);
            Sb sb(StaticBufferDesc(0x13A8, 0), kSbAddr);
            const std::vector<u32> w = FkReq(id);
            const Rep r = g.Call(DlpKind::Fkcl, w.data(), static_cast<u32>(w.size()), sb.w);
            OK(g.arenas[0]->CanariesOk());
            if (id == 11) {
                if (st == kS1) {
                    // no table in a bare state 1 (nothing scanned): the MAC is unknown
                    OK(r.out == DlpOutcome::Done);
                    if (r.out == DlpOutcome::Done) {
                        ERRW(r, 0xB, kNoData);
                    }
                } else {
                    OK(r.out == DlpOutcome::Done);
                    if (r.out == DlpOutcome::Done) {
                        ERRW(r, 0xB, kInvalidState);
                    }
                }
                continue;
            }
            OK(r.out == DlpOutcome::Done);
            if (r.out != DlpOutcome::Done) {
                continue;
            }
            if (in_s[id]) {
                EQ(r.w[0], kFkOkHdr[id]);
                EQ(r.w[1], 0u);
            } else if (in_t[id]) {
                OK(r.w[1] == 0 || r.w[1] == kNoData);
                EQ(r.w[0], r.w[1] == 0 ? kFkOkHdr[id] : ((id << 16) | 0x40u));
                OK(r.w[1] != kInvalidState);
            } else if (id == 5) {
                EQ(r.w[1], st == kS1 ? 0u : kInvalidState);
                EQ(r.w[0], st == kS1 ? kFkOkHdr[5] : ((5u << 16) | 0x40u));
            }
            // GetMyStatus and GetConnectingNodes content
            if (id == 12) {
                const u32 want[kNumSt] = {0x00000000, 0x01000000, 0x02000000, 0x01000000, 0x05010002, 0x06010002, 0x09010002, 0x80000000};
                EQ(r.w[2], want[st]);
                EQ(r.w[3] | r.w[4] | r.w[5] | r.w[6], 0u);
            }
            if (id == 13) {
                const bool sess = st == kS5 || st == kS6 || st == kS9;
                EQ(r.w[2], sess ? 2u : 0u);
                EQ(r.w[3], StaticBufferDesc(sess ? 4 : 0, 0));
                EQ(r.w[4], kSbAddr);
            }
            if (id == 15) {
                const bool pass = st == kS9;
                EQ(r.w[2], pass ? 0x14131211u : 0u);
                EQ(r.w[3], pass ? 0x18171615u : 0u);
                EQ(r.w[4], pass ? 0x19u : 0u);
            }
        }
    }
    g_ctx = "";
}

void TestMatrixSrvr() {
    SECTION("SRVR matrix: every command in every state");
    const bool in_s[0x11] = {false, true, true, true, true, false, true, false, false, false, false, false, false, false, true, true, true};
    const int sts[] = {kV0, kV1, kV2, kV6, kV8, kV9};
    const u32 sv_state[kNumSv] = {0, 1, 2, 6, 8, 9};
    for (int st : sts) {
        for (u32 id = 1; id <= 0x10; ++id) {
            Rig g(kTd);
            EnterSrvr(g, st);
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), "SRVR matrix: state %s, id 0x%02x", kSvName[st], id);
            std::snprintf(g_ctxbuf, sizeof(g_ctxbuf), "%s", ctx);
            g_ctx = g_ctxbuf;
            Arena* mw = g.Add(kMwAddr, 8, 0, 8, 0xEE);
            const std::vector<u32> w = SvReq(id);
            const Rep r = g.Call(DlpKind::Srvr, w.data(), static_cast<u32>(w.size()), nullptr);
            OK(mw->CanariesOk());
            OK(r.out == DlpOutcome::Done);
            if (in_s[id]) {
                EQ(r.w[1], 0u);
                EQ(r.w[0], kSvOkHdr[id]);
                if (id == 3) {
                    EQ(r.w[2], sv_state[st]);
                }
                if (id == 14 || id == 16) {
                    EQ(r.w[2], 0u);
                }
                continue;
            }
            switch (id) {
            case 5:  // StartAccepting: only from 1
                EQ(r.w[1], st == kV1 ? 0u : kInvalidState);
                break;
            case 7:  // StartDistribution: only from 2
                EQ(r.w[1], st == kV2 ? 0u : kInvalidState);
                break;
            case 8:  // SendPassphrase: only from 8
                EQ(r.w[1], st == kV8 ? 0u : kInvalidState);
                break;
            default:  // 9, 10, 11, 12, 13: from 1 on, success or NoData; from 0 invalid state
                if (st == kV0) {
                    EQ(r.w[1], kInvalidState);
                } else {
                    OK(r.w[1] == 0 || r.w[1] == kNoData);
                    if (id == 11) {
                        EQ(r.w[1], 0u);
                        EQ(r.w[2], (st == kV6 || st == kV8) ? 1u : 0u);  // the one client is tracked in 6 and 8; none in 0/1/2/9
                    }
                }
                break;
            }
            if (r.w[1] != 0) {
                OK(r.w[0] == ((id << 16) | 0x40u));
            }
        }
    }
    g_ctx = "";
}

// ---------------------------------------------------------------------------------------------------------------------
// 11. ProbeOnly.

void TestProbeOnly() {
    SECTION("ProbeOnly: FKCL identical to DlpFkclHle, SRVR answers only IsChild");
    // differential, word for word and counter for counter
    {
        DlpService svc;
        DlpFkclHle hle;
        OK(svc.mode() == DlpMode::ProbeOnly);
        Rig dummy;
        u32 s = 12345;
        auto rnd = [&]() {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        };
        for (int i = 0; i < 6000; ++i) {
            u32 a[kCmdWords], b[kCmdWords];
            for (u32 j = 0; j < kCmdWords; ++j) {
                a[j] = b[j] = (j == 0) ? 0 : rnd();
            }
            u32 id;
            switch (i % 4) {
            case 0: id = i / 4 % 0x30; break;
            case 1: id = rnd() & 0xFFFF; break;
            case 2: id = 1 + (rnd() % 0x11); break;
            default: id = (i % 7 == 0) ? 0xFFFF : (rnd() % 0x12); break;
            }
            const u32 low = (i % 3 == 0) ? 0 : (i % 3 == 1 ? (rnd() & 0xFFFF) : 0x83);
            a[0] = b[0] = (id << 16) | low;
            svc.Dispatch(DlpKind::Fkcl, a, nullptr, dummy.res);
            hle.Dispatch(b);
            for (u32 j = 0; j < kCmdWords; ++j) {
                if (a[j] != b[j]) {
                    EQ(a[j], b[j]);
                    break;
                }
            }
            EQ(svc.initializes(), hle.initializes());
            EQ(svc.finalizes(), hle.finalizes());
            EQ(svc.refused(), hle.refused());
        }
        EQ(svc.commands(), 6000u);
    }
    // the exact words of the three commands the probe accepts
    {
        DlpService svc;
        Rig dummy;
        u32 c1[12] = {0x00110203, 0x30000, 16, 0x004A0042, 0x006D0069, 0, 0, 0, 0, 0x04000000, 1, 2};
        svc.Dispatch(DlpKind::Fkcl, c1, nullptr, dummy.res);
        EQ(c1[0], 0x00110040u);
        EQ(c1[1], 0u);
        u32 c2[2] = {0x00020000, 0};
        svc.Dispatch(DlpKind::Fkcl, c2, nullptr, dummy.res);
        EQ(c2[0], 0x00020040u);
        EQ(c2[1], 0u);
        u32 c3[6] = {0x00050180, 0x0421, 0, 0, 0, 0};
        svc.Dispatch(DlpKind::Fkcl, c3, nullptr, dummy.res);
        EQ(c3[0], 0x00050040u);
        EQ(c3[1], kInvalidState);
        EQ(svc.initializes(), 1u);
        EQ(svc.finalizes(), 1u);
        EQ(svc.refused(), 1u);
    }
    // SRVR in ProbeOnly
    {
        DlpService svc;
        Rig dummy;
        u32 c[kCmdWords] = {0x000E0040, 0xE, 0xAAAAAAAA, 0xBBBBBBBB};
        svc.Dispatch(DlpKind::Srvr, c, nullptr, dummy.res);
        EQ(c[0], 0x000E0080u);
        EQ(c[1], 0u);
        EQ(c[2], 0u);
        EQ(svc.refused(), 0u);
        for (u32 id = 1; id <= 0x10; ++id) {
            if (id == 0xE) {
                continue;
            }
            const u32 before = svc.refused();
            u32 cc[kCmdWords] = {(id << 16) | 0x83, 1, 2, 3, 4, 5, 6, 7, 8, 9};
            svc.Dispatch(DlpKind::Srvr, cc, nullptr, dummy.res);
            EQ(cc[0], (id << 16) | 0x40u);
            EQ(cc[1], kInvalidState);
            EQ(svc.refused(), before + 1);
        }
        EQ(svc.initializes(), 0u);  // the probe refuses the SRVR Initialize: it is not counted
        // a join cannot be pending
        u32 cmd[kCmdWords] = {};
        OK(svc.PollPending(cmd));
        EQ(cmd[0], 0x000B0040u);
        EQ(cmd[1], kInvalidState);
        svc.OnSessionClosed(DlpKind::Fkcl);  // nothing to do, must not crash
        svc.OnSessionClosed(DlpKind::Srvr);
    }
    // SetLobby switches the mode both ways
    {
        Rig g;
        OK(g.svc.mode() == DlpMode::Lobby);
        g.svc.SetLobby(nullptr);
        OK(g.svc.mode() == DlpMode::ProbeOnly);
        u32 c[kCmdWords] = {0x00040000};
        g.svc.Dispatch(DlpKind::Fkcl, c, nullptr, g.res);
        EQ(c[0], 0x00040040u);
        EQ(c[1], kInvalidState);  // GetChannels is refused by the probe
        g.svc.SetLobby(&g.lobby);
        OK(g.svc.mode() == DlpMode::Lobby);
        Rep r = g.Fk({0x00040000});
        WORDS(r, 0x00040080, 0, 0x0421);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 12. StartFakeSession in two steps; the defects of Azahar; ownership; counters.

void TestJoin() {
    SECTION("StartFakeSession: JoinPending / PollPending, timeout, failures (defect 2 of Azahar)");
    auto setup = [](Rig& g) {
        EnterFkcl(g, kS2);
        g.Status();
        g.Fk({0x00060000});
        g.net.connect_result = 0;
    };
    {
        Rig g(kTd);
        setup(g);
        const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001});
        OK(jp.out == DlpOutcome::JoinPending);
        const u64 t0 = g.net.now;
        u32 cmd[kCmdWords] = {};
        g.net.now = t0 + 1000;
        OK(!g.svc.PollPending(cmd));
        g.net.now = t0 + 2999;
        OK(!g.svc.PollPending(cmd));
        OK(g.svc.join_pending());
        g.net.now = t0 + 3000;
        OK(g.svc.PollPending(cmd));
        EQ(cmd[0], 0x000B0040u);
        EQ(cmd[1], kTimeout);
        EQ(g.svc.last_result(), kTimeout);
        EQ(g.Status(), 0x01000000u);  // 1, never a residual 5
        EQ(g.net.disconnect_calls, 1);
        // the outcome is collected once
        OK(g.svc.PollPending(cmd));
        EQ(cmd[1], kInvalidState);
        // the same MAC can be tried again without a new scan
        g.net.connect_result = 1;
        g.net.BecomeClient(2, 0x1111, "H", 0x2222, "Me");
        const Rep again = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
        if (again.out == DlpOutcome::JoinPending) {
            OK(g.svc.PollPending(cmd));
            EQ(cmd[1], 0u);
        } else {
            WORDS(again, 0x000B0040, 0);
        }
        EQ(g.Status(), 0x05010002u);
    }
    {
        // ConnectPoll -1: "server not found", state 1
        Rig g(kTd);
        setup(g);
        g.net.connect_result = -1;
        const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001});
        u32 cmd[kCmdWords] = {};
        if (jp.out == DlpOutcome::JoinPending) {
            OK(g.svc.PollPending(cmd));
            EQ(cmd[0], 0x000B0040u);
            EQ(cmd[1], kNotFound);
        } else {
            WORDS(jp, 0x000B0040, kNotFound);
        }
        EQ(g.Status(), 0x01000000u);
    }
    {
        // ConnectBegin refuses: an immediate Done reply with "server not found"
        Rig g(kTd);
        setup(g);
        g.net.fail_connect_begin = true;
        const Rep r = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0x0010C000, 0x00040001});
        OK(r.out == DlpOutcome::Done);
        ERRW(r, 0xB, kNotFound);
        OK(!g.svc.join_pending());
        EQ(g.Status(), 0x01000000u);
    }
    {
        // unknown MAC: NoData at once, the network untouched
        Rig g(kTd);
        setup(g);
        const Rep r = g.Fk({0x000B0100, MW1(kMacC), MW2(kMacC), 0x0010C000, 0x00040001});
        OK(r.out == DlpOutcome::Done);
        ERRW(r, 0xB, kNoData);
        EQ(g.net.connect_begin_calls, 0);
    }
    {
        // a second StartFakeSession during a join: invalid state, Done
        Rig g(kTd);
        setup(g);
        const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
        OK(jp.out == DlpOutcome::JoinPending);
        const Rep r = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
        OK(r.out == DlpOutcome::Done);
        ERRW(r, 0xB, kInvalidState);
        EQ(g.net.connect_begin_calls, 1);
    }
    {
        // the join is cancelled by StopSession / Finalize / OnSessionClosed: PollPending says invalid state
        for (int how = 0; how < 3; ++how) {
            Rig g(kTd);
            setup(g);
            const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
            OK(jp.out == DlpOutcome::JoinPending);
            if (how == 0) {
                g.Fk({0x00100000});
            } else if (how == 1) {
                g.Fk({0x00020000});
            } else {
                g.svc.OnSessionClosed(DlpKind::Fkcl);
            }
            u32 cmd[kCmdWords] = {};
            OK(g.svc.PollPending(cmd));
            EQ(cmd[0], 0x000B0040u);
            EQ(cmd[1], kInvalidState);
            EQ(g.Status(), 0x01000000u);
            EQ(g.net.disconnect_calls, 1);
        }
    }
    {
        // a StartFakeSession from a state that is not 1 (scanning): invalid state (the table is there)
        Rig g(kTd);
        EnterFkcl(g, kS2);
        g.Status();
        const Rep r = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
        OK(r.out == DlpOutcome::Done);
        ERRW(r, 0xB, kInvalidState);
    }
    {
        // PollPending without any join
        Rig g(kTd);
        u32 cmd[kCmdWords] = {1, 2, 3};
        OK(g.svc.PollPending(cmd));
        EQ(cmd[0], 0x000B0040u);
        EQ(cmd[1], kInvalidState);
    }
}

void TestOwnershipAndBusy() {
    SECTION("network ownership (kBusy) and engine refusals");
    {
        // TD's sequence: FKCL Finalize, then SRVR Initialize / StartAccepting
        Rig g(kTd);
        EnterFkcl(g, kS1);
        g.Fk({0x00020000});
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 1, 2});
        Rep r = g.Sv({0x00050080, 0, 6});
        WORDS(r, 0x00050040, 0);
        // now a searcher cannot take the network, but its Initialize works
        r = g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        WORDS(r, 0x00010040, 0);
        r = g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        ERRW(r, 5, kBusy);
        EQ(g.net.scan_open_calls, 0);
        // ... its Finalize / StopScan / StopSession do not close the creator's network
        const int closes = g.net.host_close_calls;
        g.Fk({0x00060000});
        g.Fk({0x00100000});
        g.Fk({0x00020000});
        EQ(g.net.host_close_calls, closes);
        EQ(g.SrvState(), 2u);
    }
    {
        // the other way: a searcher in session, a creator that wants the network
        Rig g(kTd);
        EnterFkcl(g, kS5);
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 1, 2});
        const Rep r = g.Sv({0x00050080, 0, 6});
        ERRW(r, 5, kBusy);
        EQ(g.net.host_open_calls, 0);
        EQ(g.SrvState(), 1u);
        EQ(g.Status(), 0x05010002u);  // the session is intact
    }
    {
        // the engine cannot open the radio
        Rig g(kTd);
        g.net.fail_host_open = true;
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 1, 2});
        Rep r = g.Sv({0x00050080, 0, 6});
        ERRW(r, 5, kBusy);
        EQ(g.SrvState(), 1u);
        Rig f(kTd);
        f.net.fail_scan_open = true;
        f.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        r = f.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        ERRW(r, 5, kBusy);
        EQ(f.Status(), 0x01000000u);
    }
}

void TestDefectsAndCounters() {
    SECTION("Azahar defects 1-7 and the counters");
    // 1. StartScan never asserts on the channel mask
    for (u32 mask : {0x0421u, 0x2u, 0u, 0xFFFFu, 0xFFFFFFFFu, 0x0400u}) {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        const Rep r = g.Fk({0x00050180, mask, 0x0010C000, 0x00040001, 0, 0, 0});
        WORDS(r, 0x00050040, 0);
    }
    // 4. SRVR Finalize -> state 0 (done in the flow), here also from 2 and 6
    for (int st : {kV1, kV2, kV6, kV8, kV9}) {
        Rig g(kTd);
        EnterSrvr(g, st);
        const Rep r = g.Sv({0x00020000});
        WORDS(r, 0x00020040, 0);
        EQ(g.SrvState(), 0u);
    }
    // 5. no 0xFFFFFFFF anywhere: walk every command in every state and look at all results
    {
        const u32 allowed[] = {0, kInvalidState, kBusy, kNoData, kNotFound, kTimeout, kOutOfRange, kInvalidPointer, kBadHeader};
        auto okres = [&](u32 res) {
            for (u32 a : allowed) {
                if (a == res) {
                    return true;
                }
            }
            return false;
        };
        for (int st = 0; st < kNumSt; ++st) {
            for (u32 id = 1; id <= 0x11; ++id) {
                Rig g(kTd);
                EnterFkcl(g, st);
                g.Add(kSbAddr, 0x13A8);
                const std::vector<u32> w = FkReq(id);
                const Rep r = g.Call(DlpKind::Fkcl, w.data(), static_cast<u32>(w.size()), Sb(StaticBufferDesc(0x13A8, 0), kSbAddr).w);
                if (r.out == DlpOutcome::Done) {
                    OK(okres(r.w[1]));
                }
            }
        }
        for (int st = 0; st < kNumSv; ++st) {
            for (u32 id = 1; id <= 0x10; ++id) {
                for (u32 node : {0u, 1u, 2u, 3u, 9u}) {
                    Rig g(kTd);
                    EnterSrvr(g, st);
                    g.Add(kMwAddr, 8);
                    std::vector<u32> w = SvReq(id);
                    if (id == 9 || id == 10 || id == 12 || id == 13) {
                        w[1] = node;
                    }
                    const Rep r = g.Call(DlpKind::Srvr, w.data(), static_cast<u32>(w.size()), nullptr);
                    OK(okres(r.w[1]));
                }
            }
        }
    }
    // counters on a hand-made sequence (Lobby mode)
    {
        Rig g(kTd);
        EQ(g.svc.commands(), 0u);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});  // initialize
        EQ(g.svc.initializes(), 1u);
        EQ(g.svc.last_id(), 1u);
        EQ(g.svc.last_result(), 0u);
        g.Fk({0x00110203, 0x30000, 16, 0, 0, 0, 0, 0, 0, 0x04000000, 1, 2});  // initialize with name
        EQ(g.svc.initializes(), 2u);
        g.Sv({0x00010183, 0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, 1, 2});
        g.Sv({0x000F0303, 0x85000, 3, 0xE, 0, 0x40000, 2, 0, 0, 0, 0, 0, 0, 0x04000000, 1, 2});
        EQ(g.svc.initializes(), 4u);
        EQ(g.svc.refused(), 0u);
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});  // fine
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});  // invalid state
        EQ(g.svc.refused(), 1u);
        EQ(g.svc.last_result(), kInvalidState);
        g.Fk({0x00070080, 1, 2});  // no static buffer: invalid pointer
        EQ(g.svc.refused(), 2u);
        EQ(g.svc.bad_pointer(), 1u);
        g.Add(kSbAddr, 0x298);
        g.Fk({0x00070080, 1, 2}, Sb(kStaticServer, kSbAddr).w);  // NoData: not refused
        EQ(g.svc.refused(), 2u);
        EQ(g.svc.last_result(), kNoData);
        g.Fk({0x00FF0000});  // unknown id
        EQ(g.svc.refused(), 3u);
        EQ(g.svc.unknown_commands(), 1u);
        EQ(g.svc.last_id(), 0xFFu);
        g.Sv({0x00110000});  // unknown for SRVR
        EQ(g.svc.unknown_commands(), 2u);
        EQ(g.svc.last_id(), 0x11u);
        g.Fk({0x00020000});
        g.Sv({0x00020000});
        EQ(g.svc.finalizes(), 2u);
        EQ(g.svc.commands(), 12u);
    }
    // OnSessionClosed: the role's Finalize
    {
        Rig g(kTd);
        EnterSrvr(g, kV2);
        EQ(g.net.host_close_calls, 0);
        g.svc.OnSessionClosed(DlpKind::Srvr);
        EQ(g.net.host_close_calls, 1);
        EQ(g.SrvState(), 0u);
        Rig f(kTd);
        EnterFkcl(f, kS5);
        f.svc.OnSessionClosed(DlpKind::Fkcl);
        EQ(f.net.disconnect_calls, 1);
        EQ(f.Status(), 0x01000000u);
        // closing a role that was never initialized does not touch the network
        Rig h(kTd);
        h.svc.OnSessionClosed(DlpKind::Fkcl);
        h.svc.OnSessionClosed(DlpKind::Srvr);
        EQ(h.net.disconnect_calls + h.net.host_close_calls + h.net.scan_close_calls, 0);
    }
    // the name of SRVR InitializeWithName: 10 units from [7..11], [12] is the extra word
    {
        Rig g(kTd);
        u32 w[16] = {0x000F0303, 0x85000, 3, 0xE, 0, 0x40000, 2, 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0x04000000, 0x11, 0x12};
        const Nm nm = MkNameU({0x0041, 0x30A2, 0x0042, 0xD83D, 0xDE00, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047});
        NameWords(nm, w + 7);
        Rep r = g.Call(DlpKind::Srvr, w, 16);
        WORDS(r, 0x000F0040, 0);
        g.Sv({0x00050080, 0, 6});
        OK(SameName(g.net.host_id.name, nm));
        OK(SameName(g.net.last_announce.name, nm));
        EQ(g.net.host_id.seed, kSeed);
    }
    // the name of FKCL InitializeWithName: 10 units from [3..7], [8] is the extra word
    {
        Rig g(kTd);
        u32 w[12] = {0x00110203, 0xA000, 2, 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0x04000000, 0x11, 0x12};
        const Nm nm = MkNameU({0x0041, 0x30A2, 0x0042, 0xD83D, 0xDE00, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047});
        NameWords(nm, w + 3);
        Rep r = g.Call(DlpKind::Fkcl, w, 12);
        WORDS(r, 0x00110040, 0);
        g.net.heard.assign(1, MkGroup(kMacA, 0x0010C000, "H", 4, 1));
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        g.Fk({0x00060000});
        g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
        OK(SameName(g.net.connect_id.name, nm));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 12b. More adversarial pieces: widths of the FKCL commands, the table seen through the IPC, counters around a join,
//      the events from outside (OnGameUdsStarting), two threads, a resolver without a map function.

Mac MacAt(const u8* p) {
    Mac m;
    for (int i = 0; i < 6; ++i) {
        m[i] = p[8 + i];
    }
    return m;
}

void TestFkclWidthsAndTable() {
    SECTION("FKCL widths (restart flag, node id) and the table rules seen through the IPC");
    {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard = {MkGroup(kMacA, 0x0010C002, "A", 4, 2), MkGroup(kMacB, 0x0010C000, "B", 4, 1)};
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        OK(MacAt(a->p) == kMacA);
        r = g.Fk({0x00090040, 0x100}, sb.w);  // the low byte is 0: "next", the garbage above is not read
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0xFFFFFF01u}, sb.w);  // low byte 1: restart
        OK(MacAt(a->p) == kMacA);
        r = g.Fk({0x00090040, 0}, sb.w);
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0x80}, sb.w);  // any byte != 0 restarts
        OK(MacAt(a->p) == kMacA);
        r = g.Fk({0x00090040, 0xABCD0000u}, sb.w);  // low byte 0: next
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0xABCD0000u}, sb.w);
        ERRW(r, 9, kNoData);
        // a group that stops accepting is treated as absent, and is back when it accepts again
        g.net.heard[0].announce.accepting = 0;
        r = g.Fk({0x00090040, 1}, sb.w);
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0}, sb.w);
        ERRW(r, 9, kNoData);
        g.net.heard[0].announce.accepting = 1;
        r = g.Fk({0x00090040, 1}, sb.w);
        OK(MacAt(a->p) == kMacA);
    }
    {
        // the same MAC twice is ONE entry, the last one heard counts
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard = {MkGroup(kMacA, 0x0010C002, "First", 4, 1), MkGroup(kMacA, 0x0010C002, "Second", 4, 2)};
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        r = g.Fk({0x00090040, 0}, sb.w);
        ERRW(r, 9, kNoData);
        g.DropArenas();
        Arena* s = g.Add(kSbAddr, 0x298);
        r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, Sb(kStaticServer, kSbAddr).w);
        BYTES("last heard wins", s->p, ExpServer(kMacA, 3, 4, 2, MkName("Second"), g.net.NowTicks()));
        (void)a;
    }
    {
        // node ids of FKCL GetNodeInfo: u16, 0x0102 is not node 2, the upper half of the word is not read
        Rig g(kTd);
        EnterFkcl(g, kS5);
        Rep r = g.Fk({0x000E0040, 0x0102});
        ERRW(r, 0xE, kNoData);
        r = g.Fk({0x000E0040, 0x10002});
        WORDS(r, 0x000E02C0, 0);
        BYTES("node 2 through 0x10002", NodeBytes(r).data(), ExpNode(0x2222, MkName("Me"), 2));
        r = g.Fk({0x000E0040, 0xABCD0001u});
        WORDS(r, 0x000E02C0, 0);
        BYTES("node 1 through 0xABCD0001", NodeBytes(r).data(), ExpNode(0x1111, MkName("HostName"), 1));
        r = g.Fk({0x000E0040, 0xFFFF});
        ERRW(r, 0xE, kNoData);
        r = g.Fk({0x000E0040, 5});
        ERRW(r, 0xE, kNoData);
    }
}

void TestCountersAroundJoin() {
    SECTION("counters and last_result around a two-step StartFakeSession");
    Rig g(kTd);
    EnterFkcl(g, kS2);
    g.Status();
    g.Fk({0x00060000});
    g.Fk({0x000E0040, 7});  // NoData: the last result is now kNoData
    EQ(g.svc.last_result(), kNoData);
    const u32 refused0 = g.svc.refused();
    const u32 commands0 = g.svc.commands();
    g.net.connect_result = 0;
    const Rep jp = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
    OK(jp.out == DlpOutcome::JoinPending);
    EQ(g.svc.commands(), commands0 + 1);
    EQ(g.svc.last_result(), kNoData);  // a pending join has no result yet
    EQ(g.svc.last_id(), 0x0Bu);
    EQ(g.svc.refused(), refused0);
    u32 cmd[kCmdWords] = {};
    g.net.now += 3000;
    OK(g.svc.PollPending(cmd));
    EQ(cmd[1], kTimeout);
    EQ(g.svc.commands(), commands0 + 1);  // PollPending is not a Dispatch
    EQ(g.svc.last_result(), kTimeout);
    EQ(g.svc.refused(), refused0 + 1);  // a timeout is a refusal
    // a successful join is not
    g.net.BecomeClient(2, 0x1111, "H", 0x2222, "Me");
    g.net.connect_result = 1;
    const Rep j2 = g.Fk({0x000B0100, MW1(kMacA), MW2(kMacA), 0, 0});
    if (j2.out == DlpOutcome::JoinPending) {
        OK(g.svc.PollPending(cmd));
    }
    EQ(g.svc.last_result(), 0u);
    EQ(g.svc.refused(), refused0 + 1);
}

void TestEventsFromOutside() {
    SECTION("OnGameUdsStarting through the lobby, seen from the IPC");
    {
        Rig g(kTd);
        EnterSrvr(g, kV2);
        g.net.AddNode(2, 0x2222, "A");
        EQ(g.SrvState(), 2u);
        OK(g.lobby.OnGameUdsStarting());
        EQ(g.net.host_close_calls, 1);
        EQ(g.SrvState(), 1u);
        OK(!g.lobby.OnGameUdsStarting());  // idempotent
        EQ(g.net.host_close_calls, 1);
        Arena* mw = g.Add(kMwAddr, 8);
        const Rep r = g.Sv({0x000B0042, 4, MappedWriteDesc(8), kMwAddr});
        WORDS(r, 0x000B0082, 0, 0, MappedWriteDesc(8), kMwAddr);
        OK(mw->All(0xEE));
    }
    {
        Rig g(kTd);
        EnterFkcl(g, kS5);
        OK(g.lobby.OnGameUdsStarting());
        EQ(g.net.disconnect_calls, 1);
        EQ(g.Status(), 0x01000000u);
        OK(!g.lobby.OnGameUdsStarting());
    }
    {
        // in 9 the network is released but the 9 and the passphrase stay readable (TD reads them after Finalize)
        Rig g(kTd);
        EnterFkcl(g, kS9);
        g.lobby.OnGameUdsStarting();
        EQ(g.Status(), 0x09010002u);
        const Rep r = g.Fk({0x000F0000});
        WORDS(r, 0x000F0100, 0, 0x14131211, 0x18171615, 0x00000019);
        OK(!g.lobby.OnGameUdsStarting());
    }
    {
        // a scan is released
        Rig g(kTd);
        EnterFkcl(g, kS2);
        const int closes = g.net.scan_close_calls;
        OK(g.lobby.OnGameUdsStarting());
        EQ(g.net.scan_close_calls, closes + 1);
    }
    {
        // a join in progress is cancelled
        Rig g(kTd);
        EnterFkcl(g, kS3);
        OK(g.svc.join_pending());
        OK(g.lobby.OnGameUdsStarting());
        u32 cmd[kCmdWords] = {};
        OK(g.svc.PollPending(cmd));
        EQ(cmd[0], 0x000B0040u);
        EQ(cmd[1], kInvalidState);
        EQ(g.Status(), 0x01000000u);
    }
}

void TestThreads() {
    SECTION("two threads around the join (one mutex around every call)");
    for (int variant = 0; variant < 2; ++variant) {
        Rig g(kTd);
        EnterFkcl(g, kS3);
        OK(g.svc.join_pending());
        std::mutex mu;
        u32 cmd[kCmdWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            cmd[i] = 0xABAB0000u | i;
        }
        bool released = false;
        bool finished = false;
        std::thread waiter([&] {
            for (int i = 0; i < 3000; ++i) {
                bool done;
                {
                    std::lock_guard<std::mutex> l(mu);
                    done = g.svc.PollPending(cmd);
                }
                if (done) {
                    finished = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::thread busy([&] {
            for (int i = 0; i < 300; ++i) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    g.Fk({0x000C0000}, nullptr, 0);  // no time passes: the join must not time out
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::thread other([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::lock_guard<std::mutex> l(mu);
            if (variant == 0) {
                released = g.lobby.OnGameUdsStarting();
            } else {
                g.net.BecomeClient(2, 0x1111, "HostName", 0x2222, "Me");
                g.net.connect_result = 1;
            }
        });
        waiter.join();
        busy.join();
        other.join();
        OK(finished);
        EQ(cmd[0], 0x000B0040u);
        if (variant == 0) {
            OK(released);
            EQ(cmd[1], kInvalidState);
            EQ(g.Status(), 0x01000000u);
        } else {
            EQ(cmd[1], 0u);
            EQ(g.Status(), 0x05010002u);
        }
    }
}

void TestNullMapFunction() {
    SECTION("a resolver without a map function (the design lists \"map nullo\")");
    // run in a child process: a crash would take the whole test with it
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        Rig g(kTd);
        EnterFkcl(g, kS2);
        g.Status();
        IpcResolver none;
        none.ctx = nullptr;
        none.map = nullptr;
        u32 cmd[kCmdWords] = {0x00070080, MW1(kMacA), MW2(kMacA)};
        Sb sb(kStaticServer, kSbAddr);
        g.svc.Dispatch(DlpKind::Fkcl, cmd, sb.w, none);
        _exit(cmd[1] == kInvalidPointer ? 0 : 3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ++g_checks;
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL [%s]: Dispatch with IpcResolver::map == nullptr and a static buffer: %s (status 0x%x)\n", g_ctx,
                     WIFSIGNALED(status) ? "the process crashed" : "the reply is not 0xD8E0AFF6", status);
    }
}

void TestExtras() {
    SECTION("slots and the cursor through the IPC; the child index end to end; places where design and code might part");
    // fixed slots: A expires while the cursor is past it: B and C come out once each, in order
    {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard = {MkGroup(kMacA, 0x0010C000, "A", 4, 1), MkGroup(kMacB, 0x0010C000, "B", 4, 1), MkGroup(kMacC, 0x0010C000, "C", 4, 1)};
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        OK(MacAt(a->p) == kMacA);
        g.net.heard.erase(g.net.heard.begin());  // A is no longer heard
        g.Status();
        g.net.now += 800;
        g.Status();
        g.net.now += 800;                        // A is now 1600+ ms old, B and C were refreshed 800 ms ago
        r = g.Fk({0x00090040, 0}, sb.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0}, sb.w);
        OK(MacAt(a->p) == kMacC);
        r = g.Fk({0x00090040, 0}, sb.w);
        ERRW(r, 9, kNoData);
        r = g.Fk({0x00090040, 1}, sb.w);  // a new round: B then C only
        OK(MacAt(a->p) == kMacB);
        r = g.Fk({0x00090040, 0}, sb.w);
        OK(MacAt(a->p) == kMacC);
    }
    // DeleteScanInfo in the middle of a round: no repeat, no skip
    {
        Rig g(kTd);
        g.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        g.net.heard = {MkGroup(kMacA, 0x0010C000, "A", 4, 1), MkGroup(kMacB, 0x0010C000, "B", 4, 1), MkGroup(kMacC, 0x0010C000, "C", 4, 1)};
        g.Fk({0x00050180, 0x0421, 0, 0, 0, 0, 0});
        g.Status();
        Arena* a = g.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        Rep r = g.Fk({0x00090040, 1}, sb.w);
        OK(MacAt(a->p) == kMacA);
        r = g.Fk({0x000A0080, MW1(kMacB), MW2(kMacB)}, nullptr, 0);
        WORDS(r, 0x000A0040, 0);
        r = g.Fk({0x00090040, 0}, sb.w, 0);
        OK(MacAt(a->p) == kMacC);
        r = g.Fk({0x00090040, 0}, sb.w, 0);
        ERRW(r, 9, kNoData);
    }
    // the child index of SRVR Initialize [4] end to end: the creator's announcement is visible to a searcher that scans with
    // child index 0, whatever the index
    for (u32 idx : {0u, 2u, 0xFFu, 0x12345678u, 0x100u}) {
        Rig host(kTd);
        host.Sv({0x00010183, 0x85000, 3, 0xE, idx, 0x40000, 2, 0x04000000, 0x11, 0x12});
        host.Sv({0x00050080, 0, 6});
        Rig seeker(kTd);
        seeker.Fk({0x00010083, 0x30000, 16, 0x04000000, 1, 2});
        HeardGroup hg = MkGroup(kMacA, host.net.last_announce.child_tid_low, "H", host.net.last_announce.max_nodes, 1);
        seeker.net.heard.assign(1, hg);
        seeker.Fk({0x00050180, 0x0421, 0x0010C000, 0x00040001, 0, 0, 0});
        seeker.Status();
        Arena* a = seeker.Add(kSbAddr, 0x13A8);
        Sb sb(kStaticTitle, kSbAddr);
        const Rep r = seeker.Fk({0x00090040, 1}, sb.w);
        WORDS(r, 0x00090042, 0, kStaticTitle, kSbAddr);
        EQ(G32(a->p), 0x10C0u);
        EQ(G32(a->p + 4), idx & 0xFFu);
    }
    // ServerInfo read again IN the session (Battle Royale does): the tick is current; the count of players is the one of the last
    // beacon heard before the join (printed, not asserted: the design does not say whether it follows the live roster)
    {
        Rig g(kTd);
        EnterFkcl(g, kS5);
        g.net.AddNode(3, 0x3333, "Third");
        g.Status();
        g.net.now += 300;
        g.Status();
        Arena* s = g.Add(kSbAddr, 0x298);
        Rep r = g.Fk({0x00070080, MW1(kMacA), MW2(kMacA)}, Sb(kStaticServer, kSbAddr).w);
        WORDS(r, 0x00070042, 0, kStaticServer, kSbAddr);
        r = g.Fk({0x000D0040, 4}, Sb(StaticBufferDesc(8, 0), kSbAddr).w);
        std::printf("  NOTE: in session with 3 nodes present: GetConnectingNodes total=%u, ServerInfo +9 (clients_joined)=%u, +8=%u\n", r.w[2],
                    s->p[9], s->p[8]);
        EQ(r.w[2], 3u);
    }
    // GetConnectingNodes with NO static buffer when there is nothing to write (printed: design 6.2 says "sbuf null -> invalid
    // pointer", and "if 0, no map")
    {
        Rig g(kTd);
        EnterFkcl(g, kS1);
        const Rep r = g.Fk({0x000D0040, 4}, nullptr);
        WORDS(r, 0x000D0082, 0, 0, StaticBufferDesc(0, 0), 0);  // DESIGN sez. 16.4: nothing to write, no buffer needed: success
        Rig h(kTd);
        EnterFkcl(h, kS5);
        const Rep r2 = h.Fk({0x000D0040, 4}, nullptr);
        ERRW(r2, 0xD, kInvalidPointer);  // with ids to write, an absent buffer is an error
    }
}

// The design's invariants of everything a game can read from a successful reply (mode Lobby).
void FuzzInvariants(DlpKind kind, u32 id, const u32* w, const u32* req, const u32* sbw, bool have_sb, Arena* sb, Arena* mw) {
    if (w[1] != 0) {
        return;
    }
    auto min3 = [](u32 a, u32 b, u32 c) { return std::min(a, std::min(b, c)); };
    if (kind == DlpKind::Fkcl) {
        if (id == 0xC) {
            const u32 st = w[2] >> 24, con = (w[2] >> 16) & 0xFF, node = w[2] & 0xFFFF;
            OK(st == 0 || st == 1 || st == 2 || st == 5 || st == 6 || st == 9 || st == 0x80);  // never 3 (joining), 7, 8
            const bool sess = st == 5 || st == 6 || st == 9;
            OK(sess ? (con == 1 && node >= 1 && node <= 4) : (con == 0 && node == 0));
            OK((w[3] | w[4] | w[5] | w[6]) == 0);
        } else if (id == 0xD && have_sb) {
            const u32 cap = sbw[0] >> 14;
            const u32 n = req[1] & 0xFFFF;
            const u32 k = min3(w[2], n, std::min(4u, cap / 2));
            OK(w[2] <= 4);
            OK(w[3] == StaticBufferDesc(2 * k, 0));
            OK(w[4] == sbw[1]);
            if (k > 0 && sbw[1] >= kSbAddr && sbw[1] - kSbAddr + 2 * k <= sb->size) {
                const u8* p = sb->p + (sbw[1] - kSbAddr);
                u32 prev = 0;
                for (u32 i = 0; i < k; ++i) {
                    const u32 v = p[2 * i] | (static_cast<u32>(p[2 * i + 1]) << 8);
                    OK(v > prev && v <= 4);
                    prev = v;
                }
            }
        } else if (id == 0xE) {
            OK(w[11] >= 1 && w[11] <= 4);  // network node id (u32 at +0x24)
            OK(w[4] == 0 && w[10] == 0);   // the pad and unk1
        } else if ((id == 7 || id == 8 || id == 9) && have_sb && sbw[1] >= kSbAddr) {
            const u32 size = id == 7 ? 0x298 : 0x13A8;
            const u8* p = sb->p + (sbw[1] - kSbAddr);
            OK(sbw[1] - kSbAddr + size <= sb->size);
            if (id == 7) {
                OK(p[6] == 0 && p[7] <= 3 && p[8] >= 2 && p[8] <= 4 && p[9] >= 1 && p[9] <= p[8]);
                for (u32 i = 0x0A; i < 0x1C; ++i) {
                    OK(p[i] == 0);
                }
                for (u32 i = 0x30; i < 0x290; ++i) {
                    OK(p[i] == 0);
                }
            } else {
                for (u32 i = 0x0E; i < size; ++i) {
                    OK(p[i] == 0);
                }
            }
        }
    } else {
        if (id == 3) {
            const u32 v = w[2];
            OK(v == 0 || v == 1 || v == 2 || v == 6 || v == 8 || v == 9);  // never 10, never 3-5, 7
        } else if (id == 0xB) {
            const u32 bytes = req[2] >> 4;
            const u32 n = req[1] & 0xFFFF;
            OK(w[2] <= 3);
            OK(w[3] == req[2] && w[4] == req[3]);
            const u32 k = min3(w[2], n, std::min(3u, bytes / 2));
            if (k > 0 && req[3] >= kMwAddr && req[3] - kMwAddr + 2 * k <= mw->size) {
                const u8* p = mw->p + (req[3] - kMwAddr);
                u32 prev = 1;
                for (u32 i = 0; i < k; ++i) {
                    const u32 v = p[2 * i] | (static_cast<u32>(p[2 * i + 1]) << 8);
                    OK(v > prev && v <= 4);
                    prev = v;
                }
                // and nothing written past the k-th id
                if (2 * k < bytes && req[3] - kMwAddr + 2 * k < mw->size) {
                    OK(p[2 * k] == 0xEE);
                }
            }
        } else if (id == 0xC) {
            OK(w[11] >= 2 && w[11] <= 4);
            OK(w[4] == 0 && w[10] == 0);
        } else if (id == 0xD) {
            const u32 ext = w[2] >> 24, joined = (w[2] >> 16) & 0xFF, node = w[2] & 0xFFFF;
            OK(ext == 0x40 || ext == 5 || ext == 6 || ext == 7 || ext == 9);
            OK(joined == (ext == 0x40 ? 0u : 1u));
            OK(node >= 2 && node <= 4);
            OK((w[3] | w[4] | w[5] | w[6]) == 0);
        } else if (id == 0xE || id == 0x10) {
            OK(w[2] == 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 13. Fuzz: random words against both services in both modes. Nothing may write outside the buffers of the game;
//     every reply has a coherent shape.

void TestFuzz() {
    SECTION("fuzz: random requests, canaries, reply shapes");
    u32 s = 0xC0FFEE11;
    auto rnd = [&]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    for (int mode = 0; mode < 2; ++mode) {
        Rig g(rnd() % 2 ? kTd : kLm1);
        if (mode == 0) {
            g.svc.SetLobby(nullptr);
        }
        Arena* sb = g.Add(kSbAddr, 0x13A8, 0, 16, 0xEE);
        Arena* mw = g.Add(kMwAddr, 8, 0, 16, 0xEE);
        for (int i = 0; i < 25000; ++i) {
            // random scenery now and then
            if (mode == 1 && i % 7 == 0) {
                const u32 act = rnd() % 8;
                if (act == 0 && g.net.members.size() < 4) {
                    g.net.AddNode(static_cast<u8>(2 + rnd() % 3), rnd(), "Fz");
                } else if (act == 1 && !g.net.members.empty()) {
                    g.net.members.pop_back();
                } else if (act == 2) {
                    g.net.heard.assign(1 + rnd() % 3, MkGroup(kMacA, 0x0010C000 | (rnd() & 3), "Fz", 4, 1 + rnd() % 3));
                    for (size_t k = 0; k < g.net.heard.size(); ++k) {
                        g.net.heard[k].host[5] = static_cast<u8>(k + 1);
                    }
                } else if (act == 3) {
                    g.net.Inbound(static_cast<u8>(1 + rnd() % 4), {static_cast<u8>(1 + rnd() % 4), 0});
                } else if (act == 4) {
                    g.net.Inbound(static_cast<u8>(1 + rnd() % 4), {3, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9});
                } else if (act == 5) {
                    g.net.connect_result = static_cast<int>(rnd() % 3) - 1;
                    g.net.BecomeClient(static_cast<u8>(2 + rnd() % 3), 7, "H", 9, "M");
                }
            }
            const DlpKind kind = (rnd() & 1) ? DlpKind::Fkcl : DlpKind::Srvr;
            const u32 last = kind == DlpKind::Fkcl ? 0x11 : 0x10;
            u32 id = (rnd() % 10 == 0) ? (rnd() & 0xFFFF) : (1 + rnd() % last);
            u32 w[kCmdWords];
            for (u32 j = 0; j < kCmdWords; ++j) {
                w[j] = (rnd() % 3 == 0) ? 0 : rnd();
            }
            // sometimes a plausible request for the id
            if (id >= 1 && id <= last && (rnd() & 1)) {
                const std::vector<u32> good = kind == DlpKind::Fkcl ? FkReq(id) : SvReq(id);
                for (size_t j = 1; j < good.size(); ++j) {
                    if (rnd() % 4 != 0) {
                        w[j] = good[j];
                    }
                }
            }
            w[0] = (id << 16) | (rnd() % 4 == 0 ? (rnd() & 0xFFFF) : (kind == DlpKind::Fkcl ? (id <= 0x11 ? (kFkOkHdr[id] & 0xFFFF) : 0) : (id <= 0x10 ? (kSvOkHdr[id] & 0xFFFF) : 0)));
            u32 sbw[2];
            const u32 pick = rnd() % 8;
            const u32 sizes[] = {0x298, 0x13A8, 8, 0x1000};
            sbw[0] = pick < 5 ? StaticBufferDesc(sizes[rnd() % 4], 0) : rnd();
            sbw[1] = (rnd() % 3 == 0) ? kSbAddr + (rnd() % 0x20) : (rnd() % 2 ? kSbAddr : rnd());
            const u32* sbp = (rnd() % 6 == 0) ? nullptr : sbw;
            std::memset(sb->p, 0xEE, sb->size);
            std::memset(mw->p, 0xEE, mw->size);
            u32 req[kCmdWords];
            std::memcpy(req, w, sizeof(req));
            g.net.now += rnd() % 120;
            g.map_calls.clear();
            const DlpOutcome o = g.svc.Dispatch(kind, w, sbp, g.res);
            // the buffers of the game: canaries
            if (!sb->CanariesOk() || !mw->CanariesOk()) {
                OK(false);
                std::fprintf(stderr, "  canary broken at iteration %d (mode %d, kind %d, id 0x%x, req[0]=0x%08x)\n", i, mode, int(kind), id, req[0]);
                return;
            }
            // every address the module asked to map, and the size, is inside what the game declared
            for (const auto& mc : g.map_calls) {
                OK(static_cast<u64>(mc.first) + mc.second <= 0xFFFFFFFFull + 1);
            }
            if (o == DlpOutcome::JoinPending) {
                OK(kind == DlpKind::Fkcl && id == 0xB);
                OK(std::memcmp(w, req, sizeof(w)) == 0);
                g.net.now += 3100;
                u32 cmd[kCmdWords] = {};
                bool done = g.svc.PollPending(cmd);
                for (int t = 0; t < 3 && !done; ++t) {
                    g.net.now += 1000;
                    done = g.svc.PollPending(cmd);
                }
                OK(done);
                OK(cmd[0] == 0x000B0040);
                continue;
            }
            const u32 rw = 1 + ((w[0] >> 6) & 0x3F) + (w[0] & 0x3F);
            bool beyond_ok = true;
            for (u32 j = std::max(rw, 12u); j < kCmdWords; ++j) {
                if (w[j] != req[j]) {
                    beyond_ok = false;
                }
            }
            OK(beyond_ok);
            // shape
            const bool known = id >= 1 && id <= last;
            if (!known) {
                OK(w[0] == 0x40 && w[1] == kBadHeader);
                continue;
            }
            const u32 okh = kind == DlpKind::Fkcl ? kFkOkHdr[id] : kSvOkHdr[id];
            const bool special = kind == DlpKind::Srvr && id == 0xC && w[0] == 0x000C02C0 && w[1] == kNoData;
            if (mode == 1) {
                const bool good = (w[1] == 0 && w[0] == okh) || (w[1] != 0 && w[0] == ((id << 16) | 0x40u)) || special;
                if (!good) {
                    OK(false);
                    std::fprintf(stderr, "  odd reply: kind %d id 0x%x -> [0]=0x%08x [1]=0x%08x\n", int(kind), id, w[0], w[1]);
                }
                const u32 res = w[1];
                OK(res == 0 || res == kInvalidState || res == kBusy || res == kNoData || res == kNotFound || res == kTimeout ||
                   res == kOutOfRange || res == kInvalidPointer);
                FuzzInvariants(kind, id, w, req, sbw, sbp != nullptr, sb, mw);
            }
        }
        EQ(g.svc.commands(), 25000u);
        OK(g.svc.unknown_commands() <= g.svc.refused());
        OK(g.svc.bad_pointer() <= g.svc.refused());
        OK(g.svc.refused() <= g.svc.commands() + 25000);
    }
}

}  // namespace

int main() {
    TestHelpers();
    TestFkclFlowTd();
    TestFkclLm1();
    TestFkclBr();
    TestRecords();
    TestTick();
    TestMaxBeaconsAndFilters();
    TestSrvrFlowAuto();
    TestSrvrManualAndEdges();
    TestSrvrWidths();
    TestPointersFkcl();
    TestPointersSrvr();
    TestUnknownAndHeaders();
    TestMatrixFkcl();
    TestMatrixSrvr();
    TestProbeOnly();
    TestJoin();
    TestOwnershipAndBusy();
    TestDefectsAndCounters();
    TestFkclWidthsAndTable();
    TestCountersAroundJoin();
    TestEventsFromOutside();
    TestThreads();
    TestNullMapFunction();
    TestExtras();
    TestFuzz();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
