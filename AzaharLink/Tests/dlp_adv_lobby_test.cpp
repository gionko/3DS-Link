// Independent tests of the logic of the fake Download Play lobby (azl/dlp_types.hpp + azl/dlp_lobby.hpp).
//
// Written from analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md + SPEC-DLP.md and from the public declarations of the two
// headers ONLY: the bodies of the functions and the tests of the author were not read. The goal is to make the code fail:
// every state transition and every error, the caps of 4, the times at their exact boundaries, the codec, the loss of
// messages, the events from outside, the clock (wrap, jumps), and a randomised invariant fuzz.
//
// The air (LobbyNet double) is written here, from scratch, faithful to the ENGINE as described by the design (sez. 9.0):
// a node shows up in Members() of the host before any message; a join to a full or closed host never ends (ConnectPoll
// stays 0 until Disconnect); nodes are assigned lowest-free 2..4; HostClose/Kick empty the client's view; messages carry
// the engine's node as from_node. It also CHECKS the contract of LobbyNet on every call (sends only to nodes 1..4 of
// 2..16 bytes, no Kick of node 1, no second HostOpen, ScanRead index < kMaxGroups, no overlapping calls, ...).
//
//   dlp_adv_lobby_test      (needs nothing; built like the other pure tests)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_lobby.hpp"

using namespace azl;

// A report of the undefined-behaviour sanitizer stops the test (the default only prints it): an out-of-range index or an
// unaligned access is a failure, not a line nobody reads. (Ignored when the test is built without -fsanitize=undefined.)
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static int g_checks = 0;
static int g_failures = 0;
static int g_sec_fail = 0;
static const char* g_sec = "";
#define SECTION(n)                                                                                                       \
    do {                                                                                                                 \
        g_sec = (n);                                                                                                     \
        g_sec_fail = 0;                                                                                                  \
    } while (0)
#define CHECK(cond)                                                                                                      \
    do {                                                                                                                 \
        ++g_checks;                                                                                                      \
        if (!(cond)) {                                                                                                   \
            ++g_failures;                                                                                                \
            if (g_sec_fail++ < 8) std::printf("  FAIL [%s] line %d: %s\n", g_sec, __LINE__, #cond);                      \
        }                                                                                                                \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                   \
    do {                                                                                                                 \
        ++g_checks;                                                                                                      \
        const unsigned long long a_ = static_cast<unsigned long long>(a);                                                \
        const unsigned long long b_ = static_cast<unsigned long long>(b);                                                \
        if (a_ != b_) {                                                                                                  \
            ++g_failures;                                                                                                \
            if (g_sec_fail++ < 8)                                                                                        \
                std::printf("  FAIL [%s] line %d: %s == %s (got 0x%llx, want 0x%llx)\n", g_sec, __LINE__, #a, #b, a_, b_); \
        }                                                                                                                \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------------
// Small helpers.

namespace {

constexpr u64 kTpm = AZL_TICKS_PER_MS;
constexpr u32 kTdTidLow = 0x0010C000;
constexpr u64 kTdTid = 0x000400000010C000ull;
constexpr u64 kTdFilter = 0x000400010010C000ull;

struct Nm {
    u16 u[kNameUnits];
};
Nm N(const char* s) {
    Nm n;
    std::memset(&n, 0, sizeof n);
    for (u32 i = 0; i < kNameUnits && s[i]; ++i) n.u[i] = static_cast<u16>(static_cast<unsigned char>(s[i]));
    return n;
}
bool SameName(const u16* a, const Nm& b) {
    return std::memcmp(a, b.u, sizeof b.u) == 0;
}
bool NameZero(const u16* a) {
    for (u32 i = 0; i < kNameUnits; ++i)
        if (a[i]) return false;
    return true;
}
Mac M(u8 x) {
    return Mac{{0x02, 0x00, 0x00, 0x00, 0xA0, x}};
}
HostAnnounce Ann(u32 tid_low, const char* name, u8 max_nodes, u8 accepting) {
    HostAnnounce a;
    std::memset(&a, 0, sizeof a);
    a.child_tid_low = tid_low;
    a.max_nodes = max_nodes;
    a.accepting = accepting;
    const Nm n = N(name);
    std::memcpy(a.name, n.u, sizeof n.u);
    return a;
}
Identity Id(u64 seed, const char* name) {
    Identity i;
    std::memset(&i, 0, sizeof i);
    i.seed = seed;
    const Nm n = N(name);
    std::memcpy(i.name, n.u, sizeof n.u);
    return i;
}

// ---------------------------------------------------------------------------------------------------------------------
// The air.

enum Meth {
    kNowMs, kNowTicks, kHostOpen, kHostUpdate, kHostClose, kKick, kScanOpen, kScanClose, kScanRead, kConnectBegin,
    kConnectPoll, kDisconnect, kMyNode, kMembers, kReadNode, kSend, kReceive, kMethCount
};

struct Msg {
    u8 from;
    u8 len;
    u8 data[16];
};
struct SentRec {
    u64 t;
    u8 from;
    u8 to;
    u8 type;
    u8 len;
    u8 data[16];
    bool lost;
};
struct Ghx {
    u64 t;
    u8 from;
    u8 to;
    u8 len;
    u8 data[16];
};

struct Port;
struct Slot {
    bool present = false;
    bool gone = false;  // kicked, but the engine still lists it (linger)
    bool ghost = false;
    Port* port = nullptr;
    Identity id{};
    u64 hidden_until = 0;  // flap: not listed in Members() until then (still reachable)
    u64 linger_until = 0;
};
struct Net {
    bool open = false;
    Mac mac{};
    HostAnnounce ann{};
    u8 channel = 0;
    bool blocked = false;  // the engine blocks new admissions
    Port* host = nullptr;  // nullptr = a ghost host driven by the test
    Slot slot[5];
    u8 forced_total = 0;
    std::vector<Ghx> ghost_rx;
    bool ghost_kicked[5] = {};
    u32 Total() const {
        u32 n = 0;
        for (int i = 1; i <= 4; ++i)
            if (slot[i].present && !slot[i].gone) ++n;
        return n;
    }
};

struct Rule {
    u8 type;  // 0 = any
    u8 from;  // 0 = any
    u8 to;    // 0 = any
    int count;  // -1 = forever
};
struct Part {
    u8 a, b;
    u64 until;
};
struct Delayed {
    u64 at;
    Port* port;
    Msg m;
};

struct World {
    u64 now = 100000;
    u64 tick_off = 1ull << 41;
    bool fixed_ticks = false;
    u64 ticks_val = 0;
    Net nets[8];
    Port* ports[8] = {};
    int nports = 0;
    std::vector<Rule> drops;
    std::vector<Part> parts;
    u8 dup_extra[5] = {};
    u32 delay_ms[5] = {};
    std::vector<Delayed> delayed;
    u32 kick_linger_ms = 0;
    bool scan_dup = false;
    bool use_raw = false;
    std::vector<HeardGroup> raw;
    std::vector<std::pair<Mac, u64>> hidden;  // MAC hidden from scans until
    std::atomic<int> inside{0};
    int overlap = 0;
    u64 trace = 1469598103934665603ull;
    std::vector<int> closelog;  // 1000+port = HostClose, 2000+port = Disconnect, 3000+port = ScanClose

    void Mix(int m, int a, int b) {
        trace = (trace ^ static_cast<u64>(m * 131 + a * 7 + b + 1)) * 1099511628211ull;
    }
    u64 Ticks() const {
        return fixed_ticks ? ticks_val : now * kTpm + tick_off;
    }
    Net* FreeNet() {
        for (auto& n : nets)
            if (!n.open) return &n;
        return nullptr;
    }
    Net* ByMac(const Mac& m) {
        for (auto& n : nets)
            if (n.open && n.mac == m) return &n;
        return nullptr;
    }
    // A host run by the test (no lobby behind it): node 1 present.
    Net* GhostHost(u8 id, const char* name = "Host", u32 tid = kTdTidLow, u8 max_nodes = 4, u8 acc = 1) {
        Net* n = FreeNet();
        *n = Net();
        n->open = true;
        n->mac = M(id);
        n->ann = Ann(tid, name, max_nodes, acc);
        n->channel = 6;
        n->slot[1].present = true;
        n->slot[1].ghost = true;
        n->slot[1].id = Id(0x1000 + id, name);
        return n;
    }
    void GhostClient(Net* n, int node, u64 seed, const char* name) {
        n->slot[node].present = true;
        n->slot[node].ghost = true;
        n->slot[node].id = Id(seed, name);
    }
    void Remove(Net* n, int node) {
        n->slot[node] = Slot();
    }
    void Flap(Net* n, int node, u32 ms) {
        n->slot[node].hidden_until = now + ms;
    }
    bool ShouldDrop(u8 type, u8 from, u8 to) {
        for (auto& p : parts)
            if (p.until > now && ((p.a == from && p.b == to) || (p.a == to && p.b == from))) return true;
        for (auto& r : drops) {
            if (r.count == 0) continue;
            if ((r.type == 0 || r.type == type) && (r.from == 0 || r.from == from) && (r.to == 0 || r.to == to)) {
                if (r.count > 0) --r.count;
                return true;
            }
        }
        return false;
    }
    void Drop(u8 type, u8 from, u8 to, int count) {
        drops.push_back(Rule{type, from, to, count});
    }
    void Hide(const Mac& m, u64 until) {
        hidden.push_back({m, until});
    }
    bool IsHidden(const Mac& m) const {
        for (auto& h : hidden)
            if (h.first == m && h.second > now) return true;
        return false;
    }
    void Settle();
    void Deliver(Net* n, u8 from, u8 to, const u8* d, u32 size);
    void InjectTo(Net* n, int from, int to, const void* bytes, u32 len);
    void Listing(Port* asker, std::vector<HeardGroup>& out);
};

struct CallGuard {
    World& w;
    explicit CallGuard(World& wr) : w(wr) {
        if (w.inside.fetch_add(1) != 0) ++w.overlap;
    }
    ~CallGuard() {
        w.inside.fetch_sub(1);
    }
};

struct Port : LobbyNet {
    World* w;
    int idx;
    Mac mac;
    Net* net = nullptr;
    u8 node = 0;
    bool hosting = false;
    bool connected = false;
    bool scan_open = false;
    bool connecting = false;
    bool conn_failed = false;
    Net* conn_net = nullptr;
    u64 conn_start = 0;
    Identity conn_id{};
    std::deque<Msg> inbox;
    u32 calls[kMethCount] = {};
    // knobs
    u64 connect_delay = 0;
    int connect_force = 0;  // -1 = ConnectPoll answers -1
    int fail_send = 0;
    bool fail_host_open = false, fail_scan_open = false, fail_connect_begin = false;
    bool fail_read[5] = {};
    std::function<void()> on_hostclose;
    std::function<void()> on_connect_poll;  // runs at the start of every ConnectPoll (a slow call: it may move the clock)
    // logs and contract violations
    std::vector<HostAnnounce> open_ann;
    std::vector<u8> open_ch;
    std::vector<Identity> open_id;
    std::vector<u8> update_acc;
    std::vector<u8> kicks;
    std::vector<SentRec> sent;
    Mac last_connect{};
    Identity last_connect_id{};
    u32 bad_send = 0, bad_kick = 0, double_open = 0, bad_scanread = 0, bad_read = 0, small_cap = 0;
    u32 close_when_closed = 0, scanread_closed = 0, connect_while_busy = 0, disconnect_idle = 0;
    u32 max_scan_index = 0;
    u8 inject_members[4] = {};  // garbage ids the engine adds to Members() (a faulty adapter)
    u32 inject_n = 0;
    u32 lie_extra = 0;  // Members() claims this many nodes more than it wrote

    Port(World* wr, int i) : w(wr), idx(i), mac(M(static_cast<u8>(0x10 + i))) {
        w->ports[w->nports++] = this;
    }
    u32 Calls(Meth m) const {
        return calls[m];
    }
    u32 Total() const {  // every call into the net except the clock
        u32 t = 0;
        for (int i = 2; i < kMethCount; ++i) t += calls[i];
        return t;
    }
    u32 Writes() const {  // the calls that change something
        return calls[kHostOpen] + calls[kHostUpdate] + calls[kHostClose] + calls[kKick] + calls[kScanOpen] +
               calls[kScanClose] + calls[kConnectBegin] + calls[kDisconnect] + calls[kSend];
    }
    u32 Sent(u8 type, u8 to = 0) const {
        u32 n = 0;
        for (auto& s : sent)
            if (s.type == type && (to == 0 || s.to == to)) ++n;
        return n;
    }
    void Begin(Meth m, int a = 0, int b = 0) {
        ++calls[m];
        w->Mix(m, a, b);
        if (m != kNowMs && m != kNowTicks) w->Settle();
    }

    u64 NowMs() override {
        CallGuard g(*w);
        ++calls[kNowMs];
        return w->now;
    }
    u64 NowTicks() override {
        CallGuard g(*w);
        ++calls[kNowTicks];
        return w->Ticks();
    }
    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override {
        CallGuard g(*w);
        Begin(kHostOpen, channel, 0);
        open_ann.push_back(a);
        open_ch.push_back(channel);
        open_id.push_back(me);
        if (fail_host_open) return false;
        if (hosting || connected || connecting) {
            ++double_open;
            return false;
        }
        Net* n = w->FreeNet();
        if (!n) return false;
        *n = Net();
        n->open = true;
        n->mac = mac;
        n->ann = a;
        n->channel = channel;
        n->host = this;
        n->slot[1].present = true;
        n->slot[1].port = this;
        n->slot[1].id = me;
        net = n;
        node = 1;
        hosting = true;
        return true;
    }
    void HostUpdate(const HostAnnounce& a) override {
        CallGuard g(*w);
        Begin(kHostUpdate, a.accepting, 0);
        update_acc.push_back(a.accepting);
        if (net && hosting) {
            net->ann = a;
            net->blocked = !a.accepting;
        }
    }
    void HostClose() override {
        CallGuard g(*w);
        Begin(kHostClose, 0, 0);
        w->closelog.push_back(1000 + idx);
        if (on_hostclose) on_hostclose();
        if (!hosting) {
            ++close_when_closed;
            return;
        }
        for (int i = 2; i <= 4; ++i) {
            Slot& s = net->slot[i];
            if (s.port) {
                s.port->connected = false;
                s.port->net = nullptr;
                s.port->node = 0;
            }
            s = Slot();
        }
        net->slot[1] = Slot();
        net->open = false;
        net = nullptr;
        node = 0;
        hosting = false;
    }
    void Kick(u8 n) override {
        CallGuard g(*w);
        Begin(kKick, n, 0);
        kicks.push_back(n);
        if (n < 2 || !net || !hosting) {  // the host (1) and "no node" (0) are never kicked
            ++bad_kick;
            return;
        }
        if (n > 4) return;  // DESIGN sez. 16.3: an id above 4 (a faulty engine listed it) is passed on; the engine has no such slot
        Slot& s = net->slot[n];
        if (!s.present || s.gone) return;  // already ejected: nothing to extend
        if (s.port) {
            s.port->connected = false;
            s.port->net = nullptr;
            s.port->node = 0;
            s.port = nullptr;
        }
        if (s.ghost) net->ghost_kicked[n] = true;
        if (w->kick_linger_ms) {
            s.gone = true;
            s.linger_until = w->now + w->kick_linger_ms;
        } else {
            s = Slot();
        }
    }
    bool ScanOpen() override {
        CallGuard g(*w);
        Begin(kScanOpen);
        if (fail_scan_open) return false;
        scan_open = true;
        return true;
    }
    void ScanClose() override {
        CallGuard g(*w);
        Begin(kScanClose);
        w->closelog.push_back(3000 + idx);
        scan_open = false;
    }
    bool ScanRead(u32 index, HeardGroup* out) override {
        CallGuard g(*w);
        Begin(kScanRead, static_cast<int>(index));
        if (index >= kMaxGroups) ++bad_scanread;
        if (index > max_scan_index) max_scan_index = index;
        if (!scan_open) ++scanread_closed;
        std::vector<HeardGroup> l;
        w->Listing(this, l);
        if (index >= l.size()) return false;
        *out = l[index];
        return true;
    }
    bool ConnectBegin(const Mac& host, const Identity& me) override {
        CallGuard g(*w);
        Begin(kConnectBegin);
        last_connect = host;
        last_connect_id = me;
        if (fail_connect_begin) return false;
        if (hosting || connected || connecting) {
            ++connect_while_busy;
            return false;
        }
        connecting = true;
        conn_failed = false;
        conn_net = w->ByMac(host);
        conn_start = w->now;
        conn_id = me;
        return true;
    }
    int ConnectPoll() override {
        CallGuard g(*w);
        Begin(kConnectPoll);
        if (on_connect_poll) on_connect_poll();
        w->Settle();
        if (conn_failed) return -1;
        if (connected) return 1;
        if (connecting) return 0;
        return -1;
    }
    void Disconnect() override {
        CallGuard g(*w);
        Begin(kDisconnect);
        w->closelog.push_back(2000 + idx);
        if (!connected && !connecting && !conn_failed) ++disconnect_idle;
        if (connected && net) {
            net->slot[node] = Slot();
        }
        connected = false;
        connecting = false;
        conn_failed = false;
        conn_net = nullptr;
        if (!hosting) {
            net = nullptr;
            node = 0;
        }
    }
    u8 MyNode() override {
        CallGuard g(*w);
        Begin(kMyNode);
        return (net && (hosting || connected)) ? node : 0;
    }
    u32 Members(u8 (&nodes)[kMaxPlayers]) override {
        CallGuard g(*w);
        Begin(kMembers);
        if (!net || !(hosting || connected)) return 0;
        u32 n = 0;
        for (int i = 1; i <= 4; ++i) {
            const Slot& s = net->slot[i];
            if (s.present && s.hidden_until <= w->now) nodes[n++] = static_cast<u8>(i);
        }
        for (u32 i = 0; i < inject_n && n < kMaxPlayers; ++i) nodes[n++] = inject_members[i];
        if (lie_extra) for (u32 i = n; i < kMaxPlayers; ++i) nodes[i] = 0;
        return n + lie_extra;
    }
    bool ReadNode(u8 n, NodeEntry* out) override {
        CallGuard g(*w);
        Begin(kReadNode, n);
        if (n < 1 || n > 4) {
            ++bad_read;
            return false;
        }
        if (!net || !(hosting || connected)) return false;
        const Slot& s = net->slot[n];
        if (!s.present || fail_read[n]) return false;
        std::memset(out, 0, sizeof *out);
        out->seed = s.id.seed;
        std::memcpy(out->name, s.id.name, sizeof out->name);
        out->node = n;
        return true;
    }
    bool Send(u8 to, const u8* d, u32 size) override {
        CallGuard g(*w);
        Begin(kSend, to, d ? d[0] : 0);
        if (!d || size < 2 || size > kMaxMessage || to < 1 || to > 4) {
            ++bad_send;
            return false;
        }
        if (!net || !(hosting || connected)) return false;
        if (fail_send > 0) {
            --fail_send;
            return false;
        }
        Slot& dst = net->slot[to];
        if (!dst.present || dst.gone) return false;
        SentRec r;
        std::memset(&r, 0, sizeof r);
        r.t = w->now;
        r.from = node;
        r.to = to;
        r.type = d[0];
        r.len = static_cast<u8>(size);
        std::memcpy(r.data, d, size);
        r.lost = w->ShouldDrop(d[0], node, to);
        sent.push_back(r);
        if (r.lost) return true;
        const int copies = 1 + (d[0] <= 4 ? w->dup_extra[d[0]] : 0);
        for (int i = 0; i < copies; ++i) w->Deliver(net, node, to, d, size);
        return true;
    }
    bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) override {
        CallGuard g(*w);
        Begin(kReceive);
        if (capacity < kMaxMessage) ++small_cap;
        if (inbox.empty()) return false;
        const Msg m = inbox.front();
        inbox.pop_front();
        const u32 n = m.len > capacity ? capacity : m.len;
        std::memcpy(data, m.data, n);
        *size = n;
        *from_node = m.from;
        return true;
    }
};

void World::Settle() {
    for (size_t i = 0; i < delayed.size();) {
        if (delayed[i].at <= now) {
            delayed[i].port->inbox.push_back(delayed[i].m);
            delayed.erase(delayed.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    for (auto& n : nets)
        for (int i = 1; i <= 4; ++i)
            if (n.slot[i].gone && n.slot[i].linger_until <= now) n.slot[i] = Slot();
    for (int pi = 0; pi < nports; ++pi) {
        Port* p = ports[pi];
        if (!p->connecting || p->conn_failed || now < p->conn_start + p->connect_delay) continue;
        if (p->connect_force < 0) {
            p->conn_failed = true;
            continue;
        }
        Net* n = p->conn_net;
        if (!n || !n->open || n->blocked || !n->ann.accepting || !n->slot[1].present) continue;
        for (int i = 2; i <= 4 && i <= n->ann.max_nodes; ++i) {
            if (n->slot[i].present) continue;
            n->slot[i] = Slot();
            n->slot[i].present = true;
            n->slot[i].port = p;
            n->slot[i].id = p->conn_id;
            p->net = n;
            p->node = static_cast<u8>(i);
            p->connected = true;
            p->connecting = false;
            break;
        }
    }
}

void World::Deliver(Net* n, u8 from, u8 to, const u8* d, u32 size) {
    Slot& dst = n->slot[to];
    if (dst.ghost || !dst.port) {
        Ghx g;
        std::memset(&g, 0, sizeof g);
        g.t = now;
        g.from = from;
        g.to = to;
        g.len = static_cast<u8>(size);
        std::memcpy(g.data, d, size);
        n->ghost_rx.push_back(g);
        return;
    }
    Msg m;
    std::memset(&m, 0, sizeof m);
    m.from = from;
    m.len = static_cast<u8>(size);
    std::memcpy(m.data, d, size);
    const u32 dl = d[0] <= 4 ? delay_ms[d[0]] : 0;
    if (dl) delayed.push_back(Delayed{now + dl, dst.port, m});
    else dst.port->inbox.push_back(m);
}

// The test plays a node (ghost or not): puts a message in the inbox of the node `to` as if `from` had sent it.
void World::InjectTo(Net* n, int from, int to, const void* bytes, u32 len) {
    if (!n || to < 0 || to > 4) return;
    Slot& dst = n->slot[to];
    if (!dst.port) return;
    Msg m;
    std::memset(&m, 0, sizeof m);
    m.from = static_cast<u8>(from);
    m.len = static_cast<u8>(len);
    std::memcpy(m.data, bytes, len);
    dst.port->inbox.push_back(m);
}

void World::Listing(Port* asker, std::vector<HeardGroup>& out) {
    if (use_raw) {
        out = raw;
        return;
    }
    for (auto& n : nets) {
        if (!n.open || n.host == asker || IsHidden(n.mac)) continue;
        u8 enc[kAnnounceSize];
        EncodeAnnounce(n.ann, enc);
        HeardGroup g;
        std::memset(&g, 0, sizeof g);
        if (!DecodeAnnounce(enc, kAnnounceSize, &g.announce)) continue;  // the adapter drops the undecodable ones
        g.host = n.mac;
        g.total_nodes = n.forced_total ? n.forced_total : static_cast<u8>(n.Total());
        g.signal = 3;
        out.push_back(g);
        if (scan_dup) {
            HeardGroup g2 = g;
            g2.announce.name[0] = 'Z';
            g2.total_nodes = static_cast<u8>(g.total_nodes > 1 ? g.total_nodes - 1 : g.total_nodes);
            out.push_back(g2);
        }
    }
}

// One console: its port on the air and its lobby.
struct Con {
    Port port;
    DlpLobby lobby;
    Con(World& w, int idx, const LobbyConfig& cfg) : port(&w, idx), lobby(&port, cfg) {}
};

LobbyConfig Cfg(int i = 0, u64 tid = kTdTid) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof c);
    c.game_tid = tid;
    c.friend_seed = 0xF000u + static_cast<u64>(i);
    const std::string nm = std::string("P") + static_cast<char>('0' + i);
    const Nm n = N(nm.c_str());
    std::memcpy(c.name, n.u, sizeof n.u);
    return c;
}

struct Rig {
    World w;
    std::vector<std::unique_ptr<Con>> c;
    explicit Rig(u64 start = 100000) {
        w.now = start;
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Con& Add(u64 tid = kTdTid) {
        const int i = static_cast<int>(c.size());
        c.push_back(std::make_unique<Con>(w, i, Cfg(i, tid)));
        return *c.back();
    }
    void PumpAll() {
        for (auto& x : c) x->lobby.Pump();
    }
    void PollAll() {
        for (auto& x : c) x->lobby.Poll(true);
    }
    void Run(u32 ms, u32 step = 20) {
        u32 t = 0;
        while (t < ms) {
            const u32 s = std::min(step, ms - t);
            w.now += s;
            t += s;
            PumpAll();
        }
    }
};

}  // namespace

// =====================================================================================================================
// 1. Constants, sizes, helpers.

static_assert(kMaxPlayers == 4 && kMaxClients == 3 && kMaxGroups == 4, "the caps of the product");
static_assert(kAnnounceSize == 29 && kMaxMessage == 16 && kPassphraseSize == 9, "wire sizes");
static_assert(sizeof(Group) == 40, "Group is 40 bytes");
static_assert(sizeof(NodeEntry) == 32, "NodeEntry is 32 bytes");
static_assert(sizeof(HeardGroup) <= 40, "HeardGroup is a local of a game thread");
static_assert(sizeof(DlpLobby) <= 320, "DlpLobby memory budget");
static_assert(sizeof(DlpLobby::ScanTable::g) / sizeof(Group) == kMaxGroups, "scan table = kMaxGroups");
static_assert(sizeof(DlpLobby::SessionTable::roster) / sizeof(NodeEntry) == kMaxPlayers, "roster = kMaxPlayers");
static_assert(sizeof(DlpLobby::HostTable::t) / sizeof(DlpLobby::Track) == kMaxClients, "host table = kMaxClients");
static_assert(sizeof(DlpLobby::Track) <= 8, "Track is 8 bytes");
static_assert(sizeof(DlpLobby::Tables) <= 168, "tables are one of three shapes");

void T_constants() {
    SECTION("constants");
    CHECK_EQ(kMaxPlayers, 4);
    CHECK_EQ(kMaxClients, 3);
    CHECK_EQ(kMaxGroups, 4);
    CHECK_EQ(kNameUnits, 10);
    CHECK_EQ(kAnnounceSize, 29);
    CHECK_EQ(kMaxMessage, 16);
    CHECK_EQ(kPassphraseSize, 9);
    CHECK_EQ(DlpLobby::kPollMs, 20);
    CHECK_EQ(DlpLobby::kMaxRx, 8);
    CHECK_EQ(DlpLobby::kGroupTtlMs, 1500);
    CHECK_EQ(DlpLobby::kJoinMs, 3000);
    CHECK_EQ(DlpLobby::kHostGoneMs, 2000);
    CHECK_EQ(DlpLobby::kResendMs, 200);
    CHECK_EQ(DlpLobby::kStartGraceMs, 1500);
    CHECK_EQ(DlpLobby::kGoGraceMs, 2000);
    CHECK_EQ(DlpLobby::kRosterMs, 250);
    // result codes of the DLP module (SPEC 4.4)
    CHECK_EQ(kInvalidState, 0xE0A0AC01u);
    CHECK_EQ(kBusy, 0xE0A0AC03u);
    CHECK_EQ(kNoData, 0xC880AFEFu);
    CHECK_EQ(kNotFound, 0xC880AC05u);
    CHECK_EQ(kTimeout, 0xC920AFFEu);
    CHECK_EQ(kOutOfRange, 0xD8E0AFFDu);
    CHECK_EQ(kInvalidPointer, 0xD8E0AFF6u);
    CHECK_EQ(kBadHeader, 0xD9001830u);
    // states
    CHECK_EQ(kCliOff, 0);
    CHECK_EQ(kCliIdle, 1);
    CHECK_EQ(kCliScanning, 2);
    CHECK_EQ(kCliInSession, 5);
    CHECK_EQ(kCliStarted, 6);
    CHECK_EQ(kCliComplete, 9);
    CHECK_EQ(kCliLost, 0x80);
    CHECK_EQ(kSrvOff, 0);
    CHECK_EQ(kSrvIdle, 1);
    CHECK_EQ(kSrvAccepting, 2);
    CHECK_EQ(kSrvDistributing, 6);
    CHECK_EQ(kSrvPassphrase, 8);
    CHECK_EQ(kSrvComplete, 9);
    CHECK_EQ(kPeerWaiting, 0x40);
    CHECK_EQ(kPeerAccepted, 5);
    CHECK_EQ(kPeerDistributed, 7);
    CHECK_EQ(kPeerComplete, 9);
    // record layout (sizes and the offsets TD reads)
    CHECK_EQ(kTitleInfoSize, 0x13A8);
    CHECK_EQ(kServerInfoSize, 0x298);
    CHECK_EQ(kDlpNodeInfoSize, 0x28);
    CHECK_EQ(kNodeInfoName, 0x0C);
    CHECK_EQ(kNodeInfoNodeId, 0x24);
    CHECK_EQ(kServerInfoHostName, 0x1C);
    CHECK_EQ(kServerInfoTick, 0x290);
    CHECK_EQ(kServerInfoMaxClients, 0x08);
    CHECK_EQ(kServerInfoJoined, 0x09);
    CHECK_EQ(kTitleInfoUniqueId, 0);
    CHECK_EQ(kTitleInfoVariation, 4);
    CHECK_EQ(kTitleInfoMac, 8);
    // IPC helpers with the literal values of the game
    CHECK_EQ(StaticBufferDesc(0x298, 0), 0x00A60002u);
    CHECK_EQ(StaticBufferDesc(0x13A8, 0), 0x04EA0002u);
    CHECK_EQ(StaticBufferDesc(0x10, 3), (0x10u << 14) | (3u << 10) | 2u);
    CHECK_EQ(StaticBufferDesc(0x10, 0x13), (0x10u << 14) | (3u << 10) | 2u);  // the id is 4 bits
    CHECK_EQ(Header(0x0B, 2, 2), 0x000B0082u);
    CHECK_EQ(Header(0x0C, 6, 0), 0x000C0180u);
    CHECK_EQ(Header(1, 1, 0), 0x00010040u);
    CHECK_EQ(MappedWriteDesc(8), 0x8Cu);
    CHECK_EQ(MappedWriteDesc(0), 0xCu);
    // the enums of the wire
    CHECK_EQ(static_cast<u8>(MsgType::Start), 1);
    CHECK_EQ(static_cast<u8>(MsgType::StartAck), 2);
    CHECK_EQ(static_cast<u8>(MsgType::Go), 3);
    CHECK_EQ(static_cast<u8>(MsgType::GoAck), 4);
}

void T_sizes() {
    SECTION("sizes");
    std::printf("  sizeof: DlpLobby=%zu Group=%zu NodeEntry=%zu Track=%zu Tables=%zu HeardGroup=%zu Identity=%zu "
                "HostAnnounce=%zu (ptr=%zu)\n",
                sizeof(DlpLobby), sizeof(Group), sizeof(NodeEntry), sizeof(DlpLobby::Track), sizeof(DlpLobby::Tables),
                sizeof(HeardGroup), sizeof(Identity), sizeof(HostAnnounce), sizeof(void*));
    CHECK(sizeof(DlpLobby) <= 320);
    if (sizeof(DlpLobby) > 288) std::printf("  NOTE: sizeof(DlpLobby)=%zu is above the 288 target of the design\n", sizeof(DlpLobby));
    CHECK(sizeof(DlpLobby::Tables) <= 168);
    CHECK_EQ(sizeof(Group), 40);
    CHECK_EQ(sizeof(NodeEntry), 32);
    CHECK(sizeof(DlpLobby::Track) <= 8);
    // The memory of an object does not grow with use: 200 scans of different lobbies cannot change sizeof (trivially) and,
    // more to the point, the object has no pointer to anything but the net (no heap): a bitwise copy of a used lobby on
    // top of a fresh one must be harmless to ASan (nothing to free).
    Rig r;
    Con& c = r.Add();
    c.lobby.ClientInitialize(nullptr, 16);
    c.lobby.ClientStartScan(0);
    for (int i = 0; i < 8; ++i) r.w.GhostHost(static_cast<u8>(0x40 + i));
    r.Run(200);
    CHECK(c.lobby.active());
}

// =====================================================================================================================
// 2. Codec.

u8* Exact(const u8* src, u32 n) {  // a heap block of EXACTLY n bytes (ASan sees any byte read past it)
    u8* p = new u8[n ? n : 1];
    if (n) std::memcpy(p, src, n);
    return p;
}

void T_codec_announce() {
    SECTION("codec/announce");
    HostAnnounce a = Ann(0x0010C002, "Ana", 4, 1);
    a.name[9] = 0x1234;
    u8 out[kAnnounceSize];
    std::memset(out, 0xEE, sizeof out);
    EncodeAnnounce(a, out);
    const u8 want[29] = {0x44, 0x4C, 0x01, 0x01, 0x02, 0xC0, 0x10, 0x00, 0x04, 0x41, 0x00, 0x6E, 0x00, 0x61, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x12};
    CHECK(std::memcmp(out, want, 29) == 0);
    if (std::memcmp(out, want, 29) != 0)
        for (int i = 0; i < 29; ++i)
            if (out[i] != want[i]) std::printf("    byte %d: 0x%02x want 0x%02x\n", i, out[i], want[i]);
    // accepting = 0 -> flags 0
    a.accepting = 0;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[2], 1);
    // max_nodes 2 and 3 written as is
    a.max_nodes = 2;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[8], 2);
    a.max_nodes = 3;
    EncodeAnnounce(a, out);
    CHECK_EQ(out[8], 3);

    // decode of the hand-made bytes (in an exact heap block)
    u8* b = Exact(want, 29);
    HostAnnounce d;
    std::memset(&d, 0x5A, sizeof d);
    CHECK(DecodeAnnounce(b, 29, &d));
    CHECK_EQ(d.child_tid_low, 0x0010C002u);
    CHECK_EQ(d.max_nodes, 4);
    CHECK_EQ(d.accepting, 1);
    CHECK_EQ(d.name[0], 0x41);
    CHECK_EQ(d.name[1], 0x6E);
    CHECK_EQ(d.name[2], 0x61);
    CHECK_EQ(d.name[3], 0);
    CHECK_EQ(d.name[9], 0x1234);
    delete[] b;

    // every size below 29 is refused and the output is left untouched; no read past the block
    for (u32 n = 0; n < 29; ++n) {
        u8* s = Exact(want, n);
        HostAnnounce z;
        std::memset(&z, 0x77, sizeof z);
        HostAnnounce z0 = z;
        CHECK(!DecodeAnnounce(s, n, &z));
        CHECK(std::memcmp(&z, &z0, sizeof z) == 0);
        delete[] s;
    }
    // a longer announce with version 1 is accepted
    {
        u8 big[40];
        std::memset(big, 0x99, sizeof big);
        std::memcpy(big, want, 29);
        u8* s = Exact(big, 30);
        HostAnnounce z;
        CHECK(DecodeAnnounce(s, 30, &z));
        CHECK_EQ(z.child_tid_low, 0x0010C002u);
        delete[] s;
        s = Exact(big, 40);
        CHECK(DecodeAnnounce(s, 40, &z));
        delete[] s;
    }
    // magic and version
    for (int k = 0; k < 4; ++k) {
        u8 m[29];
        std::memcpy(m, want, 29);
        if (k == 0) m[0] = 'd';
        if (k == 1) m[1] = 'M';
        if (k == 2) m[2] = 0;
        if (k == 3) m[2] = 2;
        u8* s = Exact(m, 29);
        HostAnnounce z;
        std::memset(&z, 0x77, sizeof z);
        HostAnnounce z0 = z;
        CHECK(!DecodeAnnounce(s, 29, &z));
        CHECK(std::memcmp(&z, &z0, sizeof z) == 0);
        delete[] s;
    }
    {
        u8 m[29];
        std::memcpy(m, want, 29);
        m[2] = 255;
        HostAnnounce z;
        CHECK(!DecodeAnnounce(m, 29, &z));
    }
    // max_nodes: only 2..4
    for (int mn = 0; mn < 256; ++mn) {
        u8 m[29];
        std::memcpy(m, want, 29);
        m[8] = static_cast<u8>(mn);
        u8* s = Exact(m, 29);
        HostAnnounce z;
        std::memset(&z, 0x77, sizeof z);
        HostAnnounce z0 = z;
        const bool ok = DecodeAnnounce(s, 29, &z);
        CHECK(ok == (mn >= 2 && mn <= 4));
        if (!ok) CHECK(std::memcmp(&z, &z0, sizeof z) == 0);
        else CHECK_EQ(z.max_nodes, mn);
        delete[] s;
    }
    // flags: bit 0 is "accepting"
    for (int f = 0; f < 256; ++f) {
        u8 m[29];
        std::memcpy(m, want, 29);
        m[3] = static_cast<u8>(f);
        HostAnnounce z;
        CHECK(DecodeAnnounce(m, 29, &z));
        CHECK_EQ(z.accepting, f & 1);
    }
    // round trip, a few shapes
    for (u32 tid : {0u, 0xFFFFFFFFu, 0x00183600u, 0x0010C0FFu}) {
        for (u8 mn : {u8(2), u8(3), u8(4)}) {
            for (u8 acc : {u8(0), u8(1)}) {
                HostAnnounce x = Ann(tid, "Zed", mn, acc);
                for (u32 i = 0; i < kNameUnits; ++i) x.name[i] = static_cast<u16>(0xD800 + i * 0x111);
                u8 e[kAnnounceSize];
                EncodeAnnounce(x, e);
                HostAnnounce y;
                std::memset(&y, 0, sizeof y);
                CHECK(DecodeAnnounce(e, kAnnounceSize, &y));
                CHECK_EQ(y.child_tid_low, tid);
                CHECK_EQ(y.max_nodes, mn);
                CHECK_EQ(y.accepting, acc);
                CHECK(std::memcmp(y.name, x.name, sizeof x.name) == 0);
            }
        }
    }
}

void T_codec_message() {
    SECTION("codec/message");
    u8* out = new u8[kMaxMessage];  // exact
    std::memset(out, 0xEE, kMaxMessage);
    const u8 nine[9] = {'0', '1', '2', '3', '4', '5', '6', '7', 0};
    // sizes and bytes, one by one
    CHECK_EQ(EncodeMessage(MsgType::Start, nullptr, 0, out), 2);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(EncodeMessage(MsgType::StartAck, nullptr, 0, out), 2);
    CHECK_EQ(out[0], 2);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(EncodeMessage(MsgType::GoAck, nullptr, 0, out), 2);
    CHECK_EQ(out[0], 4);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(EncodeMessage(MsgType::Go, nine, 9, out), 11);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[1], 9);
    CHECK(std::memcmp(out + 2, nine, 9) == 0);
    // errors return 0: the length must be the one of the type
    for (u32 len = 0; len <= 20; ++len) {
        u8 body[32];
        std::memset(body, 0x42, sizeof body);
        CHECK_EQ(EncodeMessage(MsgType::Go, body, len, out) != 0, len == 9);
        CHECK_EQ(EncodeMessage(MsgType::Start, body, len, out) != 0, len == 0);
        CHECK_EQ(EncodeMessage(MsgType::StartAck, body, len, out) != 0, len == 0);
        CHECK_EQ(EncodeMessage(MsgType::GoAck, body, len, out) != 0, len == 0);
    }
    CHECK_EQ(EncodeMessage(MsgType::Go, nine, 0xFFFFFFFFu, out), 0);
    for (int t : {0, 5, 6, 127, 128, 255}) {
        CHECK_EQ(EncodeMessage(static_cast<MsgType>(t), nullptr, 0, out), 0);
        CHECK_EQ(EncodeMessage(static_cast<MsgType>(t), nine, 9, out), 0);
    }
    delete[] out;

    // decode: valid ones, then every kind of damage; always on exact heap blocks
    const u8 m_start[2] = {1, 0}, m_sack[2] = {2, 0}, m_gack[2] = {4, 0};
    const u8 m_go[11] = {3, 9, '9', '8', '7', '6', '5', '4', '3', '2', 0};
    MsgType t = MsgType::GoAck;
    const u8* body = nullptr;
    {
        u8* d = Exact(m_start, 2);
        CHECK(DecodeMessage(d, 2, &t, &body));
        CHECK(t == MsgType::Start);
        delete[] d;
        d = Exact(m_sack, 2);
        CHECK(DecodeMessage(d, 2, &t, &body));
        CHECK(t == MsgType::StartAck);
        delete[] d;
        d = Exact(m_gack, 2);
        CHECK(DecodeMessage(d, 2, &t, &body));
        CHECK(t == MsgType::GoAck);
        delete[] d;
        d = Exact(m_go, 11);
        CHECK(DecodeMessage(d, 11, &t, &body));
        CHECK(t == MsgType::Go);
        CHECK(body == d + 2);
        CHECK(body && std::memcmp(body, m_go + 2, 9) == 0);
        delete[] d;
    }
    // truncated and oversized: size must be exactly 2 + length
    for (u32 n = 0; n <= 14; ++n) {
        u8 big[16];
        std::memset(big, 0, sizeof big);
        std::memcpy(big, m_go, 11);
        u8* d = Exact(big, n);
        CHECK_EQ(DecodeMessage(d, n, &t, &body) ? 1 : 0, n == 11 ? 1 : 0);
        delete[] d;
        std::memset(big, 0, sizeof big);
        std::memcpy(big, m_start, 2);
        d = Exact(big, n);
        CHECK_EQ(DecodeMessage(d, n, &t, &body) ? 1 : 0, n == 2 ? 1 : 0);
        delete[] d;
    }
    // length byte lying about the body
    for (int len = 0; len < 256; ++len) {
        for (int ty = 0; ty < 8; ++ty) {
            u8 m[16];
            std::memset(m, 0xAB, sizeof m);
            m[0] = static_cast<u8>(ty);
            m[1] = static_cast<u8>(len);
            for (u32 sz : {0u, 1u, 2u, 3u, 10u, 11u, 12u, 16u}) {
                u8* d = Exact(m, sz);
                const bool want = (ty == 1 || ty == 2 || ty == 4) ? (len == 0 && sz == 2) : (ty == 3 ? (len == 9 && sz == 11) : false);
                CHECK_EQ(DecodeMessage(d, sz, &t, &body) ? 1 : 0, want ? 1 : 0);
                delete[] d;
            }
        }
    }
    // round trip through both
    for (int ty = 1; ty <= 4; ++ty) {
        u8 e[kMaxMessage];
        const u32 blen = ty == 3 ? 9 : 0;
        const u32 n = EncodeMessage(static_cast<MsgType>(ty), nine, blen, e);
        CHECK_EQ(n, 2 + blen);
        u8* d = Exact(e, n);
        CHECK(DecodeMessage(d, n, &t, &body));
        CHECK_EQ(static_cast<u8>(t), ty);
        if (blen) CHECK(std::memcmp(body, nine, 9) == 0);
        delete[] d;
    }
}

// =====================================================================================================================
// 3. The searcher (dlp:FKCL side). The host is a ghost driven by the test.

namespace {

char g_secbuf[200];
#define SECTIONF(...)                                                                                                    \
    do {                                                                                                                 \
        std::snprintf(g_secbuf, sizeof g_secbuf, __VA_ARGS__);                                                           \
        g_sec = g_secbuf;                                                                                                \
        g_sec_fail = 0;                                                                                                  \
    } while (0)

const u8 kPass1[9] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0};
const u8 kPass2[9] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0};
const u8 kZero9[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

struct SF {  // searcher fixture
    Rig r;
    Con* s;
    Net* h;
    explicit SF(u64 start = 100000, bool init = true, u32 beacons = 16) : r(start) {
        s = &r.Add();
        h = r.w.GhostHost(1, "Host");
        if (init) s->lobby.ClientInitialize(nullptr, beacons);
    }
    DlpLobby& L() {
        return s->lobby;
    }
    Port& P() {
        return s->port;
    }
    World& W() {
        return r.w;
    }
    void Adv(u32 ms) {
        r.w.now += ms;
    }
    void Poll(u32 ms = 0) {
        r.w.now += ms;
        s->lobby.Poll(true);
    }
};

void Tx(World& w, Net* n, int from, int to, MsgType t, const u8* body = nullptr) {
    u8 m[kMaxMessage];
    const u32 len = EncodeMessage(t, body, t == MsgType::Go ? 9 : 0, m);
    w.InjectTo(n, from, to, m, len);
}
u32 GhostGot(const Net* n, u8 type, u8 from = 0) {
    u32 c = 0;
    for (auto& g : n->ghost_rx)
        if (g.data[0] == type && (from == 0 || g.from == from)) ++c;
    return c;
}

void DoScan(SF& f, u64 filter = kTdFilter) {
    CHECK_EQ(f.L().ClientStartScan(filter), 0);
    f.Poll(100);
}
void DoJoinTo5(SF& f) {
    f.L().ClientStopScan();
    u32 res = 0xDEAD;
    const JoinStep js = f.L().ClientJoinBegin(M(1), &res);
    CHECK(js == JoinStep::Pending);
    f.Poll(40);
    u32 out = 0xDEAD;
    const bool done = f.L().ClientJoinPoll(&out);
    CHECK(done);
    CHECK_EQ(out, 0);
}

enum { S0 = 0, S1 = 1, S1T = 0x101, S2 = 2, S3 = 3, S5 = 5, S6 = 6, S9 = 9, S80 = 0x80 };

void Reach(SF& f, int st) {
    switch (st) {
        case S0:
        case S1:
            break;
        case S1T:
            DoScan(f);
            f.L().ClientStopScan();
            break;
        case S2:
            DoScan(f);
            break;
        case S3: {
            DoScan(f);
            f.L().ClientStopScan();
            f.P().connect_delay = 100000000ull;
            u32 r = 0;
            f.L().ClientJoinBegin(M(1), &r);
            f.Poll(20);
            break;
        }
        case S5:
            DoScan(f);
            DoJoinTo5(f);
            break;
        case S6:
            DoScan(f);
            DoJoinTo5(f);
            Tx(f.W(), f.h, 1, f.P().node, MsgType::Start);
            f.Poll(20);
            break;
        case S9:
            DoScan(f);
            DoJoinTo5(f);
            Tx(f.W(), f.h, 1, f.P().node, MsgType::Start);
            f.Poll(20);
            Tx(f.W(), f.h, 1, f.P().node, MsgType::Go, kPass1);
            f.Poll(20);
            break;
        case S80:
            DoScan(f);
            DoJoinTo5(f);
            f.W().Flap(f.h, 1, 100000000u);
            f.Poll(0);
            f.Poll(2000);
            break;
    }
    CHECK_EQ(f.L().client_state(), st == S1T ? 1 : st);
}

std::vector<Mac> Order(DlpLobby& L) {
    std::vector<Mac> v;
    GroupView g{};
    bool restart = true;
    for (int i = 0; i < 12; ++i) {
        const u32 r = L.ClientTitleInfoInOrder(restart, &g);
        restart = false;
        if (r == kNoData) break;
        CHECK_EQ(r, 0);
        if (r != 0 || !g.group) break;
        v.push_back(g.group->host);
    }
    return v;
}

// ---- the matrix: every command of the searcher in every state -----------------------------------------------------

void T_searcher_matrix() {
    const int sts[] = {S0, S1, S1T, S2, S3, S5, S6, S9, S80};
    for (const int st : sts) {
        const bool in_table = st == S1T || st == S2 || st == S3;
        const bool in_session = st == S5 || st == S6 || st == S9;
        auto mk = [&]() {
            auto f = std::make_unique<SF>(100000, st != S0);
            Reach(*f, st);
            return f;
        };
        const int raw = st == S1T ? 1 : st;

        SECTIONF("searcher/T-list (success or kNoData, never kInvalidState) in state 0x%x", st);
        {
            auto f = mk();
            GroupView v{};
            const u32 r1 = f->L().ClientTitleInfo(M(1), &v);
            CHECK_EQ(r1, (in_table || in_session) ? 0 : kNoData);
            if (r1 == 0) CHECK(v.group && v.group->host == M(1));
            GroupView v2{};
            const u32 r2 = f->L().ClientServerInfo(M(1), &v2);
            CHECK_EQ(r2, (in_table || in_session) ? 0 : kNoData);
            if (r2 == 0) CHECK(v2.group && v2.group->host == M(1));
            GroupView v3{};
            CHECK_EQ(f->L().ClientTitleInfo(M(9), &v3), kNoData);
            CHECK_EQ(f->L().ClientServerInfo(M(9), &v3), kNoData);
            CHECK_EQ(f->L().ClientTitleInfoInOrder(true, &v3), in_table ? 0 : kNoData);
            CHECK_EQ(f->L().ClientTitleInfoInOrder(false, &v3), kNoData);
            NodeEntry ne;
            std::memset(&ne, 0, sizeof ne);
            CHECK_EQ(f->L().ClientNodeInfo(2, &ne), in_session ? 0 : kNoData);
            CHECK_EQ(f->L().ClientNodeInfo(1, &ne), in_session ? 0 : kNoData);
            CHECK_EQ(f->L().ClientNodeInfo(7, &ne), kNoData);
            CHECK_EQ(f->L().ClientNodeInfo(0, &ne), kNoData);
        }
        {
            auto f = mk();
            const bool del = st == S1T || st == S2;
            CHECK_EQ(f->L().ClientDeleteScanInfo(M(1)), del ? 0 : kNoData);
            CHECK_EQ(f->L().ClientDeleteScanInfo(M(9)), kNoData);
            GroupView v{};
            // the entry is gone only if it was deleted; a session / joining table is untouched
            const u32 r = f->L().ClientServerInfo(M(1), &v);
            CHECK_EQ(r, (in_table || in_session) && !del ? 0 : kNoData);
        }

        SECTIONF("searcher/status word, StartScan, JoinBegin, JoinPoll in state 0x%x", st);
        {
            auto f = mk();
            u32 want = 0;
            switch (st) {
                case S0: want = 0; break;
                case S1: case S1T: case S3: want = 1u << 24; break;
                case S2: want = 2u << 24; break;
                case S5: want = (5u << 24) | (1u << 16) | 2; break;
                case S6: want = (6u << 24) | (1u << 16) | 2; break;
                case S9: want = (9u << 24) | (1u << 16) | 2; break;
                case S80: want = 0x80u << 24; break;
            }
            CHECK_EQ(f->L().ClientStatusWord(), want);
            // StartScan only from 1
            const u32 ss = f->L().ClientStartScan(0);
            CHECK_EQ(ss, (st == S1 || st == S1T) ? 0 : kInvalidState);
            if (ss != 0) CHECK_EQ(f->L().client_state(), raw);
            else CHECK_EQ(f->L().client_state(), 2);
        }
        {
            auto f = mk();
            u32 res = 0xCAFE;
            const JoinStep js = f->L().ClientJoinBegin(M(1), &res);
            if (st == S1T) {
                CHECK(js == JoinStep::Pending);
                CHECK_EQ(res, 0xCAFE);  // written only when Failed
                CHECK_EQ(f->L().client_state(), 3);
            } else {
                CHECK(js == JoinStep::Failed);
                CHECK_EQ(res, st == S1 ? kNoData : kInvalidState);
                CHECK_EQ(f->L().client_state(), raw);
            }
            // a second begin is refused while the first one runs
            if (st == S1T) {
                u32 r2 = 0;
                CHECK(f->L().ClientJoinBegin(M(1), &r2) == JoinStep::Failed);
                CHECK_EQ(r2, kInvalidState);
            }
        }
        {
            auto f = mk();
            u32 out = 0xBEEF;
            const bool done = f->L().ClientJoinPoll(&out);
            if (st == S3) {
                CHECK(!done);
            } else {
                CHECK(done);
                CHECK_EQ(out, kInvalidState);
            }
        }

        SECTIONF("searcher/connecting nodes, passphrase in state 0x%x", st);
        {
            auto f = mk();
            u16 ids[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            u32 total = 77;
            CHECK_EQ(f->L().ClientConnectingNodes(ids, 4, &total), 0);
            if (in_session) {
                CHECK_EQ(total, 2);
                CHECK_EQ(ids[0], 1);
                CHECK_EQ(ids[1], 2);
                CHECK_EQ(ids[2], 0xFFFF);
            } else {
                CHECK_EQ(total, 0);
                CHECK_EQ(ids[0], 0xFFFF);
            }
            u8 pp[9];
            std::memset(pp, 0x55, sizeof pp);
            CHECK_EQ(f->L().ClientPassphrase(pp), 0);
            CHECK(std::memcmp(pp, st == S9 ? kPass1 : kZero9, 9) == 0);
        }

        SECTIONF("searcher/StopScan, StopSession, Finalize, Initialize in state 0x%x", st);
        {
            auto f = mk();
            CHECK_EQ(f->L().ClientStopScan(), 0);
            CHECK_EQ(f->L().client_state(), st == S2 ? 1 : raw);
        }
        {
            auto f = mk();
            CHECK_EQ(f->L().ClientStopSession(), 0);
            int want = raw;
            if (st == S3 || st == S5 || st == S6 || st == S9 || st == S80) want = 1;
            CHECK_EQ(f->L().client_state(), want);
        }
        {
            auto f = mk();
            CHECK_EQ(f->L().ClientFinalize(), 0);
            CHECK_EQ(f->L().client_state(), st == S0 ? 0 : 1);
            CHECK_EQ(f->L().ClientFinalize(), 0);  // twice
            CHECK_EQ(f->L().client_state(), st == S0 ? 0 : 1);
            u8 pp[9];
            f->L().ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kZero9, 9) == 0);
        }
        {
            auto f = mk();
            CHECK_EQ(f->L().ClientInitialize(nullptr, 16), 0);
            CHECK_EQ(f->L().client_state(), 1);
            CHECK_EQ(f->L().ClientInitialize(nullptr, 16), 0);  // twice
            CHECK_EQ(f->L().client_state(), 1);
            GroupView v{};
            CHECK_EQ(f->L().ClientTitleInfo(M(1), &v), kNoData);  // tables empty
            u8 pp[9];
            f->L().ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kZero9, 9) == 0);
            CHECK_EQ(f->L().ClientStatusWord(), 1u << 24);
        }
        // and what a teardown does to the net: nothing it does not hold
        SECTIONF("searcher/net calls of a teardown in state 0x%x", st);
        {
            auto f = mk();
            Port& p = f->P();
            const u32 d0 = p.Calls(kDisconnect), sc0 = p.Calls(kScanClose);
            f->L().ClientFinalize();
            const u32 dd = p.Calls(kDisconnect) - d0, ds = p.Calls(kScanClose) - sc0;
            // holds the radio: 2 (scan), 3/5/6/9 (connection)
            if (st == S2) CHECK_EQ(ds, 1);
            else CHECK_EQ(ds, 0);
            if (st == S3 || st == S5 || st == S6 || st == S9) CHECK_EQ(dd, 1);
            else if (st == S0 || st == S1 || st == S1T || st == S2) CHECK_EQ(dd, 0);
        }
    }
}

// ---- the table: filter, expiry, capacity, cursor --------------------------------------------------------------------

void T_scan_content_and_filter() {
    SECTION("scan/content");
    {
        SF f;
        f.h->ann = Ann(kTdTidLow | 0x02, "Hostess", 3, 1);
        f.W().GhostClient(f.h, 2, 0x2222, "Bob");
        DoScan(f);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK(v.group != nullptr);
        if (v.group) {
            CHECK(v.group->host == M(1));
            CHECK_EQ(v.group->child_tid_low, kTdTidLow | 0x02);
            CHECK_EQ(v.group->max_nodes, 3);
            CHECK_EQ(v.group->total_nodes, 2);
            CHECK_EQ(v.group->signal, 3);
            CHECK(SameName(v.group->name, N("Hostess")));
        }
        GroupView v2{};
        CHECK_EQ(f.L().ClientTitleInfo(M(1), &v2), 0);
        CHECK(v2.group && v.group && v2.group->host == v.group->host);
        CHECK(f.L().active());
        CHECK_EQ(f.P().Calls(kScanOpen), 1);
    }
    SECTION("scan/filter");
    // which hosts does a given StartScan filter let through? (the low byte of the filter = the child index is ignored)
    struct Case {
        u64 filter;
        bool a_visible;  // host A: TD child index 0
        bool b_visible;  // host B: TD child index 3
    };
    const Case cases[] = {
        {0, true, true},
        {kTdFilter, true, true},
        {kTdFilter | 0x05, true, true},
        {kTdFilter | 0xFF, true, true},
        {0x000400010010C100ull, false, false},  // another unique id
        {0x000400010010BF00ull, false, false},
        {0x000400020010C000ull, false, false},  // another high part
        {0x000500010010C000ull, false, false},
        {0x000400010110C000ull, false, false},
        {0x000400010010C001ull, true, true},
    };
    for (const Case& c : cases) {
        SF f;
        Net* b = f.W().GhostHost(2, "B", kTdTidLow | 3);
        (void)b;
        Net* pr = f.W().GhostHost(3, "PR", 0x00183600);  // another game: never visible, filter or not
        (void)pr;
        DoScan(f, c.filter);
        const std::vector<Mac> o = Order(f.L());
        bool a = false, bb = false, p = false;
        for (auto& m : o) {
            if (m == M(1)) a = true;
            if (m == M(2)) bb = true;
            if (m == M(3)) p = true;
        }
        CHECK_EQ(a, c.a_visible);
        CHECK_EQ(bb, c.b_visible);
        CHECK(!p);
    }
    // another game with the same low byte but a different unique id; and the same unique id with another low byte
    {
        SF f;
        f.W().GhostHost(2, "Other", 0x0010C100);
        f.W().GhostHost(3, "Same", 0x0010C0FF);
        DoScan(f, 0);
        const std::vector<Mac> o = Order(f.L());
        CHECK_EQ(o.size(), 2);
        if (o.size() == 2) {
            CHECK(o[0] == M(1));
            CHECK(o[1] == M(3));
        }
    }
    // a console playing another game sees ITS lobbies and not ours
    {
        Rig r;
        Con& pr = r.Add(0x0004000000183600ull);
        r.w.GhostHost(1, "TD", kTdTidLow);
        r.w.GhostHost(2, "PR", 0x00183600);
        pr.lobby.ClientInitialize(nullptr, 16);
        pr.lobby.ClientStartScan(0);
        r.w.now += 100;
        pr.lobby.Poll(true);
        const std::vector<Mac> o = Order(pr.lobby);
        CHECK_EQ(o.size(), 1);
        if (o.size() == 1) CHECK(o[0] == M(2));
    }
    SECTION("scan/total_nodes validation");
    {
        // total_nodes must be 1..max_nodes: 0 and max+1 are refused, max is accepted (the game hides a full group)
        SF f;
        f.W().use_raw = true;
        for (int t : {0, 1, 2, 3, 4, 5, 255}) {
            f.W().raw.clear();
            HeardGroup g;
            std::memset(&g, 0, sizeof g);
            g.host = M(0x70);
            g.total_nodes = static_cast<u8>(t);
            g.signal = 3;
            g.announce = Ann(kTdTidLow, "Raw", 4, 1);
            f.W().raw.push_back(g);
            f.L().ClientStartScan(0);
            f.Poll(50);
            GroupView v{};
            const u32 r = f.L().ClientServerInfo(M(0x70), &v);
            CHECK_EQ(r == 0, t >= 1 && t <= 4);
            f.L().ClientStopScan();
            f.L().ClientInitialize(nullptr, 16);
        }
        // max_nodes 3 with total 4 is refused; with total 3 accepted
        for (int t : {3, 4}) {
            f.W().raw.clear();
            HeardGroup g;
            std::memset(&g, 0, sizeof g);
            g.host = M(0x71);
            g.total_nodes = static_cast<u8>(t);
            g.signal = 3;
            g.announce = Ann(kTdTidLow, "Raw", 3, 1);
            f.W().raw.push_back(g);
            f.L().ClientStartScan(0);
            f.Poll(50);
            GroupView v{};
            CHECK_EQ(f.L().ClientServerInfo(M(0x71), &v) == 0, t == 3);
            f.L().ClientStopScan();
            f.L().ClientInitialize(nullptr, 16);
        }
    }
    SECTION("scan/garbage heard groups never break the tables");
    {
        SF f;
        f.W().use_raw = true;
        u32 seed = 12345;
        auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        f.L().ClientStartScan(0);
        for (int round = 0; round < 300; ++round) {
            f.W().raw.clear();
            const int n = static_cast<int>(rnd() % 7);
            for (int i = 0; i < n; ++i) {
                HeardGroup g;
                std::memset(&g, 0, sizeof g);
                g.host = M(static_cast<u8>(rnd() % 9));
                g.total_nodes = static_cast<u8>(rnd());
                g.signal = static_cast<u8>(rnd());
                g.announce.child_tid_low = (rnd() & 1) ? kTdTidLow : static_cast<u32>(rnd());
                g.announce.max_nodes = static_cast<u8>(rnd() % 8);
                g.announce.accepting = static_cast<u8>(rnd() % 3);
                for (u32 k = 0; k < kNameUnits; ++k) g.announce.name[k] = static_cast<u16>(rnd());
                f.W().raw.push_back(g);
            }
            f.Poll(static_cast<u32>(rnd() % 700));
            const std::vector<Mac> o = Order(f.L());
            CHECK(o.size() <= kMaxGroups);
            for (size_t i = 0; i < o.size(); ++i)
                for (size_t j = i + 1; j < o.size(); ++j) CHECK(!(o[i] == o[j]));  // one entry per MAC
        }
        CHECK(f.P().max_scan_index < kMaxGroups);
        CHECK_EQ(f.P().bad_scanread, 0);
    }
}

void T_scan_capacity() {
    SECTION("scan/capacity (max_beacons, 4 slots)");
    const u32 beacons[] = {0, 1, 2, 3, 4, 5, 16, 0xFFFFFFFFu};
    for (const u32 mb : beacons) {
        SF f(100000, true, mb);
        for (int i = 2; i <= 6; ++i) f.W().GhostHost(static_cast<u8>(i), "H");
        DoScan(f, 0);
        const u32 want = mb == 0 ? 1 : (mb > 4 ? 4 : mb);
        CHECK_EQ(Order(f.L()).size(), want);
    }
    SECTION("scan/6 hosts in 4 slots are stable");
    {
        SF f;
        // 6 hosts: M(1) (the fixture's) and M(2..6)
        for (int i = 2; i <= 6; ++i) f.W().GhostHost(static_cast<u8>(i), "H");
        DoScan(f, 0);
        const std::vector<Mac> first = Order(f.L());
        CHECK_EQ(first.size(), 4);
        for (int i = 0; i < 400; ++i) {
            f.Poll(20);
            const std::vector<Mac> o = Order(f.L());
            CHECK(o == first);
            if (o != first) break;
        }
        CHECK(f.P().max_scan_index < kMaxGroups);
        // one of the four goes away: the freed slot is taken by a newcomer (the first one not in the table), the three
        // others stay where they are
        f.W().Hide(first[1], ~0ull);
        f.Poll(1600);
        f.Poll(20);
        const std::vector<Mac> after = Order(f.L());
        CHECK_EQ(after.size(), 4);
        for (const Mac& m : after) CHECK(!(m == first[1]));
        for (size_t i : {size_t(0), size_t(2), size_t(3)}) {
            bool found = false;
            for (const Mac& m : after)
                if (m == first[i]) found = true;
            CHECK(found);
        }
    }
    SECTION("scan/same MAC twice = one entry, the last one wins");
    {
        SF f;
        f.W().scan_dup = true;
        f.W().GhostClient(f.h, 2, 5, "c");
        f.W().GhostClient(f.h, 3, 6, "d");
        DoScan(f, 0);
        const std::vector<Mac> o = Order(f.L());
        CHECK_EQ(o.size(), 1);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        if (v.group) {
            CHECK_EQ(v.group->name[0], 'Z');  // the second report of the pair
            CHECK_EQ(v.group->total_nodes, 2);
        }
    }
}

// Seen at `base`, then not reported any more: the entry lives while its age is < 1500 ms.
void ScenarioTtl(u64 base) {
    SECTIONF("scan/ttl 1499/1500 base=0x%llx", static_cast<unsigned long long>(base));
    SF f(base);
    DoScan(f, 0);  // StartScan then +100 ms then Poll: last_seen = base+100
    const u64 seen = f.W().now;
    f.W().Hide(M(1), ~0ull);
    GroupView v{};
    f.W().now = seen + 1499;
    f.L().Poll(true);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    CHECK_EQ(Order(f.L()).size(), 1);
    f.W().now = seen + 1500;
    f.L().Poll(true);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
    CHECK_EQ(Order(f.L()).size(), 0);
    // the host comes back: a new entry
    f.W().hidden.clear();
    f.W().now += 20;
    f.L().Poll(true);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    // refreshed by a read in time: lives another 1500 from the LAST read
    const u64 t1 = f.W().now;
    f.W().now = t1 + 1000;
    f.L().Poll(true);  // heard again at t1+1000
    f.W().Hide(M(1), ~0ull);
    f.W().now = t1 + 1000 + 1499;
    f.L().Poll(true);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    f.W().now = t1 + 1000 + 1500;
    f.L().Poll(true);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
}

void T_scan_ttl() {
    const u64 bases[] = {1000, 100000, 0xFFFFFF00ull, 0xFFFFFC00ull, 0xFFFFF000ull, 0x100000000ull - 1550,
                         0x100000000ull - 1500, 0x1FFFFFF00ull, 0x7FFFFFF0ull, 0x7FFFFFFFFFFFFF00ull};
    for (u64 b : bases) ScenarioTtl(b);
    SECTION("scan/ttl only while scanning; table frozen after StopScan");
    SF f;
    DoScan(f, 0);
    f.W().Hide(M(1), ~0ull);
    f.L().ClientStopScan();
    const u32 reads = f.P().Calls(kScanRead);
    f.Adv(60000);
    for (int i = 0; i < 10; ++i) {
        f.Adv(7000);
        f.L().Pump();
        f.L().Poll(true);
    }
    GroupView v{};
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);  // still there after 130 s
    CHECK_EQ(Order(f.L()).size(), 1);
    CHECK_EQ(f.P().Calls(kScanRead), reads);  // nothing read from the air
    CHECK_EQ(f.P().scanread_closed, 0);
    CHECK(!f.L().active());
    // the frozen entry can be joined
    u32 res = 0;
    CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
    CHECK_EQ(f.P().Calls(kConnectBegin), 1);
    CHECK(f.P().last_connect == M(1));
    // an entry heard NOW while the host announces something else does not change a frozen table
    SF g;
    DoScan(g, 0);
    g.L().ClientStopScan();
    g.h->ann.max_nodes = 2;
    g.h->ann.accepting = 0;
    g.Adv(500);
    g.L().Poll(true);
    CHECK_EQ(g.L().ClientServerInfo(M(1), &v), 0);
    CHECK(v.group && v.group->max_nodes == 4);
}

void T_scan_accepting() {
    SECTION("scan/accepting=0 is absent");
    SF f;
    DoScan(f, 0);
    GroupView v{};
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    f.h->ann.accepting = 0;
    f.Poll(20);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);  // removed at once, not after the TTL
    CHECK_EQ(Order(f.L()).size(), 0);
    f.Poll(20);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
    f.h->ann.accepting = 1;
    f.Poll(20);
    CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    // a lobby that is never accepting never enters
    SF g;
    g.h->ann.accepting = 0;
    DoScan(g, 0);
    CHECK_EQ(g.L().ClientServerInfo(M(1), &v), kNoData);
    CHECK_EQ(Order(g.L()).size(), 0);
}

void T_scan_cursor_and_delete() {
    SECTION("scan/GetTitleInfoInOrder cursor");
    {
        SF f;
        f.W().GhostHost(2, "B");
        f.W().GhostHost(3, "C");
        DoScan(f, 0);  // table [A=M1, B=M2, C=M3]
        GroupView v{};
        CHECK_EQ(f.L().ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group && v.group->host == M(1));
        // A expires (no longer reported), the others are refreshed
        f.W().Hide(M(1), ~0ull);
        f.Poll(1000);
        f.Poll(600);
        GroupView x{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &x), kNoData);
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group && v.group->host == M(2));
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group && v.group->host == M(3));
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), kNoData);
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), kNoData);  // stays at the end
        // restart gives B, C (A is gone)
        const std::vector<Mac> o = Order(f.L());
        CHECK_EQ(o.size(), 2);
    }
    {
        SF f;
        f.W().GhostHost(2, "B");
        f.W().GhostHost(3, "C");
        DoScan(f, 0);
        GroupView v{};
        CHECK_EQ(f.L().ClientTitleInfoInOrder(true, &v), 0);
        CHECK(v.group && v.group->host == M(1));
        // DeleteScanInfo of B in the middle of the round: the rest is C only, once
        CHECK_EQ(f.L().ClientDeleteScanInfo(M(2)), 0);
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), 0);
        CHECK(v.group && v.group->host == M(3));
        CHECK_EQ(f.L().ClientTitleInfoInOrder(false, &v), kNoData);
        // a new round: A and C
        const std::vector<Mac> o = Order(f.L());
        CHECK_EQ(o.size(), 2);
    }
    SECTION("scan/DeleteScanInfo: no ignore list");
    {
        SF f;
        DoScan(f, 0);
        CHECK_EQ(f.L().ClientDeleteScanInfo(M(1)), 0);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
        CHECK_EQ(f.L().ClientDeleteScanInfo(M(1)), kNoData);  // already gone
        f.Poll(20);                                           // the lobby is still alive: it comes back
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        // in state 1 (frozen) a delete is final
        f.L().ClientStopScan();
        CHECK_EQ(f.L().ClientDeleteScanInfo(M(1)), 0);
        f.Poll(100);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
    }
    SECTION("scan/StartScan clears the table and the cursor");
    {
        SF f;
        f.W().GhostHost(2, "B");
        DoScan(f, 0);
        GroupView v{};
        CHECK_EQ(f.L().ClientTitleInfoInOrder(true, &v), 0);
        f.L().ClientStopScan();
        f.W().Hide(M(1), ~0ull);
        f.W().Hide(M(2), ~0ull);
        CHECK_EQ(f.L().ClientStartScan(0), 0);
        CHECK_EQ(Order(f.L()).size(), 0);  // the old table is gone
        f.W().hidden.clear();
        f.Poll(20);
        CHECK_EQ(Order(f.L()).size(), 2);
    }
    SECTION("scan/ScanOpen false -> kBusy, no state change");
    {
        SF f;
        f.P().fail_scan_open = true;
        CHECK_EQ(f.L().ClientStartScan(0), kBusy);
        CHECK_EQ(f.L().client_state(), 1);
        CHECK(!f.L().active());
        f.P().fail_scan_open = false;
        CHECK_EQ(f.L().ClientStartScan(0), 0);
        CHECK_EQ(f.L().client_state(), 2);
        CHECK_EQ(f.L().ClientStartScan(0), kInvalidState);
        CHECK_EQ(f.P().Calls(kScanOpen), 2);  // the second StartScan did not reach the net
    }
}

void T_scan_ticks() {
    SECTION("scan/tick of ServerInfo+0x290");
    for (const u64 base : {10000ull, 0x100000000ull + 9000ull, 0x2FFFFFF00ull}) {
        SF f(base);
        f.W().now = base;
        f.L().ClientStartScan(0);
        f.L().Poll(true);  // heard at `base`
        f.W().Hide(M(1), ~0ull);
        GroupView v{};
        f.W().now = base + 1000;
        f.L().Poll(true);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks() - 1000 * kTpm);
        GroupView t{};
        CHECK_EQ(f.L().ClientTitleInfo(M(1), &t), 0);
        CHECK_EQ(t.tick, f.W().Ticks() - 1000 * kTpm);
        f.W().now = base + 1400;
        f.L().Poll(true);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks() - 1400 * kTpm);
        // frozen by StopScan: the age keeps growing (3.9 s fine for the game, 5.1 s dropped by it)
        f.W().now = base + 1450;
        f.L().Poll(true);
        f.L().ClientStopScan();
        f.W().now = base + 3900;
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks() - 3900 * kTpm);
        CHECK((f.W().Ticks() - v.tick) / kTpm < 5000);
        f.W().now = base + 5100;
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks() - 5100 * kTpm);
        CHECK((f.W().Ticks() - v.tick) / kTpm >= 5000);
    }
    // in the table while heard: the age is 0 .. one poll
    {
        SF f;
        DoScan(f, 0);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks());
    }
    // the system tick is small (just booted): saturated at 0, no wrap
    {
        SF f;
        DoScan(f, 0);
        f.W().Hide(M(1), ~0ull);
        f.Adv(1400);
        f.L().Poll(true);
        f.W().fixed_ticks = true;
        f.W().ticks_val = 100;
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, 0);
        f.W().ticks_val = 1400 * kTpm + 12345;
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, 12345);
    }
    // in a session the tick is the current one
    {
        SF f;
        Reach(f, S5);
        f.Adv(30000);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks());
        CHECK_EQ(f.L().ClientTitleInfo(M(1), &v), 0);
        CHECK_EQ(v.tick, f.W().Ticks());
    }
}

// ---- the join -------------------------------------------------------------------------------------------------------

void FillHost(SF& f) {  // all of nodes 2..4 taken by ghosts: a full lobby the engine never admits into
    for (int n = 2; n <= 4; ++n) f.W().GhostClient(f.h, n, 0x300 + static_cast<u64>(n), "full");
}

void ScenarioJoinTimeout(u64 base) {
    SECTIONF("join/timeout 2999/3000 base=0x%llx", static_cast<unsigned long long>(base));
    SF f(base);
    DoScan(f, 0);
    f.L().ClientStopScan();
    FillHost(f);
    u32 res = 0;
    CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
    const u64 t0 = f.W().now;
    u32 out = 0xBEEF;
    f.W().now = t0 + 1500;
    CHECK(!f.L().ClientJoinPoll(&out));
    CHECK_EQ(f.L().client_state(), 3);
    CHECK_EQ(f.L().ClientStatusWord(), 1u << 24);  // the game still sees 1
    f.W().now = t0 + 2999;
    CHECK(!f.L().ClientJoinPoll(&out));
    CHECK_EQ(f.L().client_state(), 3);
    CHECK_EQ(f.P().Calls(kDisconnect), 0);
    f.W().now = t0 + 3000;
    CHECK(f.L().ClientJoinPoll(&out));
    CHECK_EQ(out, kTimeout);
    CHECK_EQ(f.L().client_state(), 1);
    CHECK_EQ(f.P().Calls(kDisconnect), 1);
    CHECK_EQ(f.L().ClientStatusWord(), 1u << 24);
    // the outcome is collected once
    CHECK(f.L().ClientJoinPoll(&out));
    CHECK_EQ(out, kInvalidState);
    // nothing more happens to the net, and the table is intact
    f.Adv(10000);
    f.L().Pump();
    f.L().Poll(true);
    CHECK_EQ(f.P().Calls(kDisconnect), 1);
    f.L().ClientFinalize();
    CHECK_EQ(f.P().Calls(kDisconnect), 1);
    f.L().ClientInitialize(nullptr, 16);
    // (Initialize cleared the table; retry the whole way)
    DoScan(f, 0);
    f.L().ClientStopScan();
    f.h->slot[3] = Slot();  // room for one
    f.h->slot[4] = Slot();
    f.h->slot[2] = Slot();
    CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
    f.Poll(60);
    CHECK(f.L().ClientJoinPoll(&out));
    CHECK_EQ(out, 0);
    CHECK_EQ(f.L().client_state(), 5);
}

void T_join() {
    const u64 bases[] = {5000, 100000, 0xFFFFFF00ull, 0xFFFFF000ull, 0x100000000ull - 3000, 0x100000000ull - 2999,
                         0x1FFFFF000ull};
    for (u64 b : bases) ScenarioJoinTimeout(b);

    SECTION("join/ConnectBegin false -> kNotFound");
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.P().fail_connect_begin = true;
        u32 res = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Failed);
        CHECK_EQ(res, kNotFound);
        CHECK_EQ(f.L().client_state(), 1);
        f.P().fail_connect_begin = false;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);  // table intact
        f.Poll(50);
        u32 out = 0;
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, 0);
    }
    SECTION("join/ConnectPoll -1 on the first round");
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.P().connect_force = -1;
        u32 res = 0;
        const u64 t0 = f.W().now;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.Poll(20);
        u32 out = 0;
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kNotFound);
        CHECK(f.W().now - t0 < DlpLobby::kJoinMs);
        CHECK_EQ(f.L().client_state(), 1);
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);  // the table is intact: retry without StartScan
        f.P().connect_force = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.Poll(50);
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, 0);
        CHECK_EQ(f.L().client_state(), 5);
        CHECK_EQ(f.P().Calls(kConnectBegin), 2);
    }
    SECTION("join/late host and slow ConnectPoll");
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.P().connect_delay = 3500;
        u32 res = 0, out = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.Poll(3000);
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kTimeout);
        f.Adv(2000);
        f.L().Pump();
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 1);
        CHECK_EQ(f.h->Total(), 1);  // never admitted: the lobby abandoned the join
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
    }
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.P().connect_delay = 2999;
        u32 res = 0, out = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.Poll(2999);
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, 0);
        CHECK_EQ(f.L().client_state(), 5);
    }
    {
        // a ConnectPoll that takes 2900 ms of the game's time: the join ends by 3000 ms + one round
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        FillHost(f);
        World* w = &f.W();
        bool once = true;
        f.P().on_connect_poll = [w, &once]() {
            if (once) {
                once = false;
                w->now += 2900;
            }
        };
        u32 res = 0;
        const u64 t0 = f.W().now;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        u64 end = 0;
        for (int i = 0; i < 400 && !end; ++i) {
            f.W().now += 20;
            f.L().Pump();
            if (f.L().client_state() == 1) end = f.W().now - t0;
        }
        CHECK(end >= 3000);
        CHECK(end <= 3000 + 60);
    }
    SECTION("join/nobody asks: it times out by itself");
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        FillHost(f);
        u32 res = 0, out = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        for (int i = 0; i < 200; ++i) {
            f.W().now += 50;
            f.L().Pump();
        }
        CHECK_EQ(f.L().client_state(), 1);
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
        CHECK(!f.L().join_active());
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kTimeout);
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kInvalidState);
        // a new begin discards an old unread outcome
        SF g;
        DoScan(g, 0);
        g.L().ClientStopScan();
        FillHost(g);
        CHECK(g.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        g.Adv(4000);
        g.L().Poll(true);  // the join ended unread
        CHECK_EQ(g.L().client_state(), 1);
        g.h->slot[2] = Slot();
        CHECK(g.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        g.Poll(40);
        CHECK(g.L().ClientJoinPoll(&out));
        CHECK_EQ(out, 0);  // not the old timeout
        CHECK_EQ(g.L().client_state(), 5);
    }
    SECTION("join/cancelled in the middle");
    for (int how = 0; how < 4; ++how) {
        SECTIONF("join/cancelled by %s", how == 0 ? "StopSession" : how == 1 ? "Finalize" : how == 2 ? "Initialize" : "OnGameUdsStarting");
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        FillHost(f);
        u32 res = 0, out = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.Poll(500);
        CHECK(f.L().join_active());
        bool ret = false;
        if (how == 0) CHECK_EQ(f.L().ClientStopSession(), 0);
        if (how == 1) CHECK_EQ(f.L().ClientFinalize(), 0);
        if (how == 2) CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        if (how == 3) {
            ret = f.L().OnGameUdsStarting();
            CHECK(ret);
            CHECK(!f.L().OnGameUdsStarting());
        }
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
        CHECK_EQ(f.L().client_state(), 1);
        CHECK(!f.L().join_active());
        CHECK(!f.L().active());
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kInvalidState);
        f.Adv(5000);
        f.L().Pump();
        f.L().Poll(true);
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
        CHECK_EQ(f.P().Calls(kConnectBegin), 1);
        CHECK_EQ(f.L().client_state(), 1);
        if (how == 0 || how == 2) {
            GroupView v{};
            CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);  // the tables were cleared
        }
    }
    SECTION("join/identity given to the engine");
    {
        SF f(100000, false);
        const Nm zoe = N("Zoe");
        f.L().ClientInitialize(zoe.u, 16);
        DoScan(f, 0);
        DoJoinTo5(f);
        CHECK(SameName(f.P().last_connect_id.name, zoe));
        CHECK_EQ(f.P().last_connect_id.seed, 0xF000);
        NodeEntry me, host;
        std::memset(&me, 0, sizeof me);
        std::memset(&host, 0, sizeof host);
        CHECK_EQ(f.L().ClientNodeInfo(2, &me), 0);
        CHECK_EQ(me.seed, 0xF000);
        CHECK(SameName(me.name, zoe));
        CHECK_EQ(me.node, 2);
        CHECK_EQ(f.L().ClientNodeInfo(1, &host), 0);
        CHECK_EQ(host.seed, 0x1001);
        CHECK(SameName(host.name, N("Host")));
        CHECK_EQ(host.node, 1);
        // a name given once stays when a later Initialize gives none
        f.L().ClientInitialize(nullptr, 16);
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.h->slot[2] = Slot();
        u32 res = 0;
        f.L().ClientJoinBegin(M(1), &res);
        CHECK(SameName(f.P().last_connect_id.name, zoe));
        // default name when never given
        SF g;
        DoScan(g, 0);
        DoJoinTo5(g);
        CHECK(SameName(g.P().last_connect_id.name, N("P0")));
    }
}

// ---- the session ----------------------------------------------------------------------------------------------------

void T_session() {
    SECTION("session/5 status, roster");
    {
        SF f;
        Reach(f, S5);
        CHECK_EQ(f.L().ClientStatusWord(), (5u << 24) | (1u << 16) | 2);
        // already filled right after the join
        u16 ids[4];
        u32 total = 0;
        CHECK_EQ(f.L().ClientConnectingNodes(ids, 4, &total), 0);
        CHECK_EQ(total, 2);
        // capacity smaller than the total: min(total, capacity) ids written, the total stays whole
        u16 one[1] = {0xFFFF};
        total = 0;
        CHECK_EQ(f.L().ClientConnectingNodes(one, 1, &total), 0);
        CHECK_EQ(total, 2);
        CHECK_EQ(one[0], 1);
        u16 canary[2] = {0xABCD, 0xABCD};
        total = 0;
        CHECK_EQ(f.L().ClientConnectingNodes(canary, 0, &total), 0);
        CHECK_EQ(total, 2);
        CHECK_EQ(canary[0], 0xABCD);
        // ids above 0xFF are never the node 2 / 1
        NodeEntry ne;
        std::memset(&ne, 0, sizeof ne);
        CHECK_EQ(f.L().ClientNodeInfo(0x0102, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(0x0101, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(0x0100, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(0xFF02, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(0xFFFF, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(4, &ne), kNoData);
        CHECK_EQ(f.L().ClientNodeInfo(2, &ne), 0);
        // a client that comes in later shows up within kRosterMs + a poll
        f.W().GhostClient(f.h, 3, 0x777, "Late");
        f.W().GhostClient(f.h, 4, 0x888, "Later");
        u64 seen_at = 0;
        const u64 t0 = f.W().now;
        for (int i = 0; i < 100 && !seen_at; ++i) {
            f.Poll(10);
            total = 0;
            f.L().ClientConnectingNodes(ids, 4, &total);
            if (total == 4) seen_at = f.W().now - t0;
        }
        CHECK(seen_at > 0);
        CHECK(seen_at <= DlpLobby::kRosterMs + 20 + 10);
        total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 4);
        CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 4);
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), 0);
        CHECK_EQ(ne.seed, 0x777);
        CHECK(SameName(ne.name, N("Late")));
        // one leaves
        f.W().Remove(f.h, 3);
        f.Poll(400);
        total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 3);
        CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 4);
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), kNoData);
        // the engine's raw seed: whatever it says
        f.W().GhostClient(f.h, 3, 0, "NoSeed");
        f.Poll(400);
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), 0);
        CHECK_EQ(ne.seed, 0);
    }
    SECTION("session/ReadNode failing keeps the id with a zero name");
    {
        SF f;
        Reach(f, S5);
        f.W().GhostClient(f.h, 3, 0x777, "Late");
        f.P().fail_read[3] = true;
        f.Poll(400);
        u16 ids[4];
        u32 total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 3);
        NodeEntry ne;
        std::memset(&ne, 0x5A, sizeof ne);
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), 0);
        CHECK_EQ(ne.seed, 0);
        CHECK(NameZero(ne.name));
    }
    SECTION("session/roster frozen at 9, with a last re-read at GO");
    {
        SF f;
        Reach(f, S6);
        f.W().GhostClient(f.h, 3, 0x777, "Late");  // arrives just before GO, the periodic re-read has not seen it
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass1);
        f.Poll(10);
        CHECK_EQ(f.L().client_state(), 9);
        u16 ids[4];
        u32 total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 3);  // the final re-read at GO
        // then frozen: the host closes, others leave, 60 s pass
        f.W().Remove(f.h, 3);
        f.W().Remove(f.h, 1);
        for (int i = 0; i < 600; ++i) f.Poll(100);
        total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 3);
        NodeEntry ne;
        CHECK_EQ(f.L().ClientNodeInfo(3, &ne), 0);
        CHECK_EQ(f.L().ClientNodeInfo(1, &ne), 0);
        CHECK(SameName(ne.name, N("Host")));
    }
}

void T_session_messages() {
    SECTION("messages/START -> 6, GO -> 9");
    {
        SF f;
        Reach(f, S5);
        Tx(f.W(), f.h, 1, 2, MsgType::Start);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 6);
        CHECK_EQ(f.L().ClientStatusWord(), (6u << 24) | (1u << 16) | 2);
        CHECK_EQ(GhostGot(f.h, 2, 2), 1);  // one START_ACK, to node 1
        CHECK_EQ(f.P().Sent(2, 1), 1);
        for (int i = 0; i < 10; ++i) Tx(f.W(), f.h, 1, 2, MsgType::Start);
        for (int i = 0; i < 4; ++i) f.Poll(20);
        CHECK_EQ(f.L().client_state(), 6);
        CHECK_EQ(GhostGot(f.h, 2), 11);  // every duplicate is answered
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass1);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 9);
        CHECK_EQ(f.L().ClientStatusWord(), (9u << 24) | (1u << 16) | 2);
        CHECK_EQ(GhostGot(f.h, 4, 2), 1);  // GO_ACK
        u8 pp[9];
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass1, 9) == 0);
        // GO again with other bytes: answered, the first passphrase stays
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass2);
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass2);
        f.Poll(20);
        CHECK_EQ(GhostGot(f.h, 4), 3);
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass1, 9) == 0);
        CHECK_EQ(f.L().client_state(), 9);
        // a late START in 9: no change, but answered
        Tx(f.W(), f.h, 1, 2, MsgType::Start);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 9);
        CHECK_EQ(GhostGot(f.h, 2), 12);
        CHECK_EQ(f.L().bad_messages(), 0);
    }
    SECTION("messages/GO without START: 5 -> 9 directly");
    {
        SF f;
        Reach(f, S5);
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass2);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 9);
        u8 pp[9];
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass2, 9) == 0);
        CHECK_EQ(GhostGot(f.h, 4, 2), 1);
        // START after the GO (reordered): state stays 9, START_ACK sent
        Tx(f.W(), f.h, 1, 2, MsgType::Start);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 9);
        CHECK_EQ(GhostGot(f.h, 2, 2), 1);
    }
    SECTION("messages/from the wrong node or the wrong role are discarded and counted");
    {
        // searcher in 5: START/GO from node 3 or 4, acks of any node, and the same in 6 and 2/joining
        const int sts[] = {S2, S3, S5, S6, S9};
        for (const int st : sts) {
            SECTIONF("messages/discards in searcher state %d", st);
            SF f;
            Reach(f, st);
            const u32 bad0 = f.L().bad_messages();
            const u32 acks0 = f.P().Sent(2) + f.P().Sent(4);
            const int raw = f.L().client_state();
            u32 expect_bad = 0;
            const MsgType types[] = {MsgType::Start, MsgType::StartAck, MsgType::Go, MsgType::GoAck};
            for (MsgType t : types) {
                for (int from : {3, 4}) {  // never the host
                    u8 m[kMaxMessage];
                    const u32 len = EncodeMessage(t, kPass2, t == MsgType::Go ? 9 : 0, m);
                    f.P().inbox.push_back(Msg{static_cast<u8>(from), static_cast<u8>(len), {}});
                    std::memcpy(f.P().inbox.back().data, m, len);
                    ++expect_bad;
                }
            }
            f.Poll(20);
            f.Poll(20);
            f.Poll(20);
            CHECK_EQ(f.L().bad_messages(), bad0 + expect_bad);
            CHECK_EQ(f.L().client_state(), raw);
            CHECK_EQ(f.P().Sent(2) + f.P().Sent(4), acks0);  // no reply to a wrong sender
            u8 pp[9];
            f.L().ClientPassphrase(pp);
            CHECK(std::memcmp(pp, st == S9 ? kPass1 : kZero9, 9) == 0);
            // the creator's messages from the host, to a searcher that is not a creator: acks are the wrong role
            const u32 bad1 = f.L().bad_messages();
            for (MsgType t : {MsgType::StartAck, MsgType::GoAck}) {
                u8 m[kMaxMessage];
                const u32 len = EncodeMessage(t, nullptr, 0, m);
                f.P().inbox.push_back(Msg{1, static_cast<u8>(len), {}});
                std::memcpy(f.P().inbox.back().data, m, len);
            }
            f.Poll(20);
            CHECK_EQ(f.L().bad_messages(), bad1 + 2);
            CHECK_EQ(f.L().client_state(), raw);
            // START / GO from the host are valid only in 5, 6, 9
            const u32 bad2 = f.L().bad_messages();
            Tx(f.W(), f.h, 1, f.P().node ? f.P().node : 2, MsgType::Start);
            if (!f.P().node) f.P().inbox.push_back(Msg{1, 2, {1, 0}});
            f.Poll(20);
            const bool valid = st == S5 || st == S6 || st == S9;
            CHECK_EQ(f.L().bad_messages(), bad2 + (valid ? 0 : 1));
        }
    }
    SECTION("messages/garbage never changes anything");
    {
        SF f;
        Reach(f, S5);
        u32 seed = 99;
        auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        u32 bad = f.L().bad_messages();
        for (int i = 0; i < 300; ++i) {
            Msg m;
            std::memset(&m, 0, sizeof m);
            m.from = 1;
            m.len = static_cast<u8>(rnd() % 17);
            for (u32 k = 0; k < m.len; ++k) m.data[k] = static_cast<u8>(rnd());
            // make it a valid-looking frame only if it is really valid: skip those (type 1-4 with the right length)
            if (m.len >= 2 && m.data[0] >= 1 && m.data[0] <= 4 && m.data[1] == (m.data[0] == 3 ? 9 : 0) &&
                m.len == 2u + m.data[1])
                continue;
            f.P().inbox.push_back(m);
            ++bad;
            f.L().Poll(true);
            if (i % 8 == 7) CHECK_EQ(f.L().bad_messages(), bad);
        }
        f.L().Poll(true);
        f.L().Poll(true);
        CHECK_EQ(f.L().bad_messages(), bad);
        CHECK_EQ(f.L().client_state(), 5);
        CHECK_EQ(f.P().Sent(2) + f.P().Sent(4), 0);
        // Receive always got a capacity of kMaxMessage or more
        CHECK_EQ(f.P().small_cap, 0);
    }
    SECTION("messages/kMaxRx = 8 per poll");
    {
        SF f;
        Reach(f, S6);
        for (int i = 0; i < 20; ++i) Tx(f.W(), f.h, 1, 2, MsgType::Start);
        const u32 s0 = f.P().Sent(2);
        f.L().Poll(true);
        const u32 s1 = f.P().Sent(2) - s0;
        CHECK(s1 <= DlpLobby::kMaxRx);
        CHECK(s1 >= 1);
        f.L().Poll(true);
        f.L().Poll(true);
        f.L().Poll(true);
        CHECK_EQ(f.P().Sent(2) - s0, 20);  // none lost
        CHECK_EQ(f.P().inbox.size(), 0);
    }
}

// ---- the host goes away ---------------------------------------------------------------------------------------------

void ScenarioHostGone(u64 base, int st) {
    SECTIONF("host-gone/%s base=0x%llx", st == S5 ? "5" : st == S6 ? "6" : "9", static_cast<unsigned long long>(base));
    SF f(base);
    Reach(f, st);
    // the host vanishes from the engine's list but we are still in its network
    f.W().Flap(f.h, 1, 3000);
    f.L().Poll(true);  // first sees it missing
    const u64 t0 = f.W().now;
    f.W().now = t0 + 1999;
    f.L().Poll(true);
    CHECK_EQ(f.L().client_state(), st);
    f.W().now = t0 + 2000;
    f.L().Poll(true);
    if (st == S9) {
        CHECK_EQ(f.L().client_state(), 9);
        CHECK_EQ(f.L().ClientStatusWord() >> 24, 9);
    } else {
        CHECK_EQ(f.L().client_state(), 0x80);
        CHECK_EQ(f.L().ClientStatusWord(), 0x80u << 24);
        CHECK(f.P().Calls(kDisconnect) >= 1);
        // stays there for a minute
        for (int i = 0; i < 600; ++i) {
            f.Adv(100);
            f.L().Pump();
        }
        CHECK_EQ(f.L().ClientStatusWord(), 0x80u << 24);
        CHECK_EQ(f.L().ClientFinalize(), 0);
        CHECK_EQ(f.L().client_state(), 1);
    }
}

void T_host_gone() {
    const u64 bases[] = {50000, 0xFFFFFF00ull, 0xFFFFF000ull, 0x100000000ull - 2000, 0x100000000ull - 1999,
                         0x1FFFFF000ull};
    for (u64 b : bases)
        for (int st : {S5, S6, S9}) ScenarioHostGone(b, st);

    SECTION("host-gone/a flap shorter than 2000 ms resets the timer");
    {
        SF f;
        Reach(f, S6);
        for (int k = 0; k < 5; ++k) {
            f.W().Flap(f.h, 1, 1500);
            f.Poll(0);
            f.Poll(1400);
            CHECK_EQ(f.L().client_state(), 6);
            f.Poll(200);  // the host is back
            f.Poll(100);
            CHECK_EQ(f.L().client_state(), 6);
        }
    }
    SECTION("host-gone/MyNode() == 0 (deauthenticated) in 5/6");
    for (int st : {S5, S6}) {
        SF f;
        Reach(f, st);
        // the engine drops us (as a HostClose of a real host would)
        f.P().connected = false;
        f.P().net = nullptr;
        f.P().node = 0;
        f.Poll(0);
        u64 t = 0;
        for (int i = 0; i < 200 && f.L().client_state() == st; ++i) {
            f.Poll(20);
            t += 20;
        }
        CHECK_EQ(f.L().client_state(), 0x80);
        CHECK(t <= DlpLobby::kHostGoneMs + 60);
    }
    SECTION("host-gone/9 is final: nothing worsens it");
    {
        SF f;
        Reach(f, S9);
        // host closes, we are deauthenticated, a kick, a late START, a minute goes by
        f.P().connected = false;
        f.P().net = nullptr;
        f.P().node = 0;
        f.W().Remove(f.h, 1);
        Tx(f.W(), f.h, 1, 2, MsgType::Start);
        for (int i = 0; i < 600; ++i) {
            f.Poll(100);
            CHECK_EQ(f.L().client_state(), 9);
            if (f.L().client_state() != 9) break;
        }
        CHECK_EQ(f.L().ClientStatusWord(), (9u << 24) | (1u << 16) | 2);
        u8 pp[9];
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass1, 9) == 0);
        u16 ids[4];
        u32 total = 0;
        f.L().ClientConnectingNodes(ids, 4, &total);
        CHECK_EQ(total, 2);
        // only these three leave 9
        for (int how = 0; how < 3; ++how) {
            SF g;
            Reach(g, S9);
            if (how == 0) g.L().ClientStopSession();
            if (how == 1) g.L().ClientFinalize();
            if (how == 2) g.L().ClientInitialize(nullptr, 16);
            CHECK_EQ(g.L().client_state(), 1);
            u8 q[9];
            g.L().ClientPassphrase(q);
            CHECK(std::memcmp(q, kZero9, 9) == 0);
        }
        // these do not
        SF h;
        Reach(h, S9);
        CHECK_EQ(h.L().ClientStopScan(), 0);
        CHECK_EQ(h.L().client_state(), 9);
        CHECK_EQ(h.L().ClientStartScan(0), kInvalidState);
        CHECK_EQ(h.L().client_state(), 9);
    }
    SECTION("host-gone/0x80 leaves only on StopSession / Finalize / Initialize");
    for (int how = 0; how < 4; ++how) {
        SF f;
        Reach(f, S80);
        if (how == 0) f.L().ClientStopSession();
        if (how == 1) f.L().ClientFinalize();
        if (how == 2) f.L().ClientInitialize(nullptr, 16);
        if (how == 3) {
            f.L().ClientStopScan();
            CHECK_EQ(f.L().ClientStartScan(0), kInvalidState);
        }
        CHECK_EQ(f.L().client_state(), how == 3 ? 0x80 : 1);
    }
    SECTION("host-gone/a jump of the clock forward with the host present loses nothing");
    {
        SF f;
        Reach(f, S6);
        f.Adv(60000);
        f.L().Pump();
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 6);
        // the first look after a jump with the host missing starts the 2000 ms: no 0x80 yet
        f.W().Flap(f.h, 1, 1000000);
        f.Adv(60000);
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 6);
    }
}

// ---- OnGameUdsStarting on the searcher ------------------------------------------------------------------------------

void T_events_searcher() {
    const int sts[] = {S0, S1, S1T, S2, S3, S5, S6, S9, S80};
    for (int st : sts) {
        SECTIONF("event/searcher state 0x%x", st);
        SF f(100000, st != S0);
        Reach(f, st);
        Port& p = f.P();
        const u32 d0 = p.Calls(kDisconnect), s0 = p.Calls(kScanClose), w0 = p.Writes();
        const bool first = f.L().OnGameUdsStarting();
        const bool holds = st == S2 || st == S3 || st == S5 || st == S6 || st == S9;
        if (st == S80) {
            // nothing is held any more (the connection was dropped when the host was lost)
            CHECK(!f.L().active());
        } else {
            CHECK_EQ(first, holds);
        }
        CHECK(!f.L().OnGameUdsStarting());  // idempotent
        CHECK(!f.L().active());
        if (st == S2) {
            CHECK_EQ(p.Calls(kScanClose) - s0, 1);
            CHECK_EQ(f.L().client_state(), 1);
            GroupView v{};
            CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);  // frozen, not cleared
        }
        if (st == S3 || st == S5 || st == S6) {
            CHECK_EQ(p.Calls(kDisconnect) - d0, 1);
            CHECK_EQ(f.L().client_state(), 1);
        }
        if (st == S5 || st == S6) {
            u16 ids[4];
            u32 total = 5;
            f.L().ClientConnectingNodes(ids, 4, &total);
            CHECK_EQ(total, 0);
        }
        if (st == S9) {
            CHECK_EQ(p.Calls(kDisconnect) - d0, 1);
            CHECK_EQ(f.L().client_state(), 9);
            u8 pp[9];
            f.L().ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kPass1, 9) == 0);
            u16 ids[4];
            u32 total = 0;
            f.L().ClientConnectingNodes(ids, 4, &total);
            CHECK_EQ(total, 2);
            NodeEntry ne;
            CHECK_EQ(f.L().ClientNodeInfo(1, &ne), 0);
            f.Adv(10000);
            f.L().Pump();
            f.L().Poll(true);
            CHECK_EQ(f.L().client_state(), 9);
            CHECK_EQ(p.Calls(kDisconnect) - d0, 1);
        }
        if (st == S1 || st == S1T || st == S0) CHECK_EQ(p.Writes(), w0);
    }
}

}  // namespace

// =====================================================================================================================
// 4. The creator (dlp:SRVR side). The clients are ghosts driven by the test.

namespace {

struct CF {  // creator fixture
    Rig r;
    Con* h;
    explicit CF(u64 start = 100000, u64 tid = kTdTid) : r(start) {
        h = &r.Add(tid);
    }
    DlpLobby& L() {
        return h->lobby;
    }
    Port& P() {
        return h->port;
    }
    World& W() {
        return r.w;
    }
    Net* N() {
        return r.w.ByMac(h->port.mac);
    }
    void Adv(u32 ms) {
        r.w.now += ms;
    }
    void Poll(u32 ms = 0) {
        r.w.now += ms;
        h->lobby.Poll(true);
    }
    void Cli(int node, const char* name = "Bob") {  // a client comes in
        r.w.GhostClient(N(), node, 0xB00u + static_cast<u64>(node), name);
    }
    void Ack(int node, MsgType t) {  // a client answers
        u8 m[kMaxMessage];
        const u32 len = EncodeMessage(t, nullptr, 0, m);
        r.w.InjectTo(N(), node, 1, m, len);
    }
    u32 Sent(u8 type, u8 to = 0) const {
        return h->port.Sent(type, to);
    }
    // the bytes of the n-th message of `type` sent to `to` (index from 0); false if there is none
    bool NthSent(u8 type, u8 to, u32 n, SentRec* out) const {
        u32 i = 0;
        for (auto& s : h->port.sent)
            if (s.type == type && s.to == to) {
                if (i++ == n) {
                    *out = s;
                    return true;
                }
            }
        return false;
    }
};

enum { C0 = 0, C1 = 1, C2 = 2, C6 = 6, C8 = 8, C9 = 9 };

void CReach(CF& f, int st, bool manual = false) {
    if (st == C0) return;
    CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
    if (st == C1) return;
    CHECK_EQ(f.L().ServerStartAccepting(manual, 6), 0);
    f.Cli(2);
    f.Poll(20);
    if (manual && st > C2) {
        CHECK_EQ(f.L().ServerAcceptClient(2), 0);
    }
    if (st == C2) return;
    CHECK_EQ(f.L().ServerStartDistribution(), 0);
    f.Poll(20);
    if (st == C6) return;
    f.Ack(2, MsgType::StartAck);
    f.Poll(20);
    if (st == C8) return;
    CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
    f.Ack(2, MsgType::GoAck);
    f.Poll(20);
}

u32 Word(u32 ext, u32 joined, u32 node) {
    return (ext << 24) | (joined << 16) | node;
}

void T_creator_matrix() {
    const int sts[] = {C0, C1, C2, C6, C8, C9};
    for (const int st : sts) {
        auto mk = [&]() {
            auto f = std::make_unique<CF>();
            CReach(*f, st);
            CHECK_EQ(f->L().server_state(), st);
            return f;
        };
        const bool has_client = st == C2 || st == C6 || st == C8;

        SECTIONF("creator/matrix state %d: state byte, Initialize, Finalize, EndAccepting", st);
        {
            auto f = mk();
            CHECK_EQ(f->L().ServerState(), st);
            CHECK_EQ(f->L().ServerState(), st);  // reading is free
        }
        {
            auto f = mk();
            const u32 hc = f->P().Calls(kHostClose);
            CHECK_EQ(f->L().ServerInitialize(3, 0, nullptr), 0);
            CHECK_EQ(f->L().ServerState(), 1);
            CHECK_EQ(f->P().Calls(kHostClose) - hc, has_client ? 1 : 0);
            u16 ids[4];
            u32 total = 9;
            CHECK_EQ(f->L().ServerConnectingClients(ids, 4, &total), 0);
            CHECK_EQ(total, 0);
            // and a fresh lobby can be opened again
            CHECK_EQ(f->L().ServerStartAccepting(false, 6), 0);
            CHECK_EQ(f->L().ServerState(), 2);
            CHECK_EQ(f->P().Calls(kHostOpen), (st >= C2 ? 2 : 1));
        }
        {
            auto f = mk();
            const u32 hc = f->P().Calls(kHostClose), k0 = f->P().Calls(kKick);
            CHECK_EQ(f->L().ServerFinalize(), 0);
            CHECK_EQ(f->L().ServerState(), 0);
            CHECK_EQ(f->P().Calls(kHostClose) - hc, has_client ? 1 : 0);
            CHECK_EQ(f->P().Calls(kKick), k0);
            CHECK(!f->L().active());
            CHECK_EQ(f->L().ServerFinalize(), 0);  // twice
            CHECK_EQ(f->P().Calls(kHostClose) - hc, has_client ? 1 : 0);
            // state 0 again: the commands that need a state refuse
            CHECK_EQ(f->L().ServerStartAccepting(false, 6), kInvalidState);
        }
        {
            auto f = mk();
            const u32 hu = f->P().Calls(kHostUpdate);
            CHECK_EQ(f->L().ServerEndAccepting(), 0);
            CHECK_EQ(f->L().ServerState(), st == C2 ? 1 : st);
            CHECK_EQ(f->P().Calls(kHostUpdate) - hu, st == C2 ? 1 : 0);
            if (st == C2) {
                CHECK_EQ(f->P().update_acc.back(), 0);
                u16 ids[4];
                u32 total = 0;
                CHECK_EQ(f->L().ServerConnectingClients(ids, 4, &total), 0);
                CHECK_EQ(total, 1);  // the client stays
                CHECK_EQ(f->P().Calls(kHostClose), 0);
                CHECK(f->L().active());
            }
        }

        SECTIONF("creator/matrix state %d: StartAccepting", st);
        {
            auto f = mk();
            const u32 r = f->L().ServerStartAccepting(false, 6);
            CHECK_EQ(r, st == C1 ? 0 : kInvalidState);
            CHECK_EQ(f->L().ServerState(), st == C1 ? 2 : st);
        }
        {
            auto f = mk();
            const u32 tot0 = f->P().Total();
            CHECK_EQ(f->L().ServerStartAccepting(false, 7), kOutOfRange);  // the channel is checked FIRST
            CHECK_EQ(f->L().ServerStartAccepting(false, 5), kOutOfRange);
            CHECK_EQ(f->L().ServerStartAccepting(true, 0x106), kOutOfRange);
            CHECK_EQ(f->L().ServerStartAccepting(true, 0xFFFFFFFFu), kOutOfRange);
            CHECK_EQ(f->L().ServerState(), st);
            CHECK_EQ(f->P().Total(), tot0);
        }

        SECTIONF("creator/matrix state %d: clients lists", st);
        {
            auto f = mk();
            u16 ids[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            u32 total = 77;
            const u32 r = f->L().ServerConnectingClients(ids, 4, &total);
            CHECK_EQ(r, st == C0 ? kInvalidState : 0);
            if (st != C0) {
                CHECK_EQ(total, has_client ? 1 : 0);  // in 9 it is 0
                CHECK_EQ(ids[0], has_client ? 2 : 0xFFFF);
                CHECK_EQ(ids[1], 0xFFFF);
            }
        }
        {
            auto f = mk();
            NodeEntry e;
            std::memset(&e, 0, sizeof e);
            const u32 r = f->L().ServerClientInfo(2, &e);
            CHECK_EQ(r, st == C0 ? kInvalidState : (has_client ? 0 : kNoData));
            if (r == 0) {
                CHECK_EQ(e.seed, 0xB02);
                CHECK(SameName(e.name, N("Bob")));
                CHECK_EQ(e.node, 2);
            }
        }
        {
            auto f = mk();
            NodeEntry e;
            std::memset(&e, 0, sizeof e);
            const u32 r = f->L().ServerClientInfo(1, &e);  // the host: kNoData, but FILLED
            CHECK_EQ(r, st == C0 ? kInvalidState : kNoData);
            if (st != C0) {
                CHECK_EQ(e.seed, 0xF000);
                CHECK(SameName(e.name, N("P0")));
                CHECK_EQ(e.node, 1);
            }
            std::memset(&e, 0, sizeof e);
            const u32 r3 = f->L().ServerClientInfo(3, &e);
            CHECK_EQ(r3, st == C0 ? kInvalidState : kNoData);
            CHECK_EQ(e.node, 0);  // not filled
            CHECK_EQ(f->L().ServerClientInfo(0x0102, &e) == (st == C0 ? kInvalidState : kNoData), 1);
            CHECK_EQ(e.node, 0);
            CHECK_EQ(f->L().ServerClientInfo(0x0002 + 0x100, &e) == (st == C0 ? kInvalidState : kNoData), 1);
            CHECK_EQ(f->L().ServerClientInfo(0, &e) == (st == C0 ? kInvalidState : kNoData), 1);
            CHECK_EQ(f->L().ServerClientInfo(0xFFFF, &e) == (st == C0 ? kInvalidState : kNoData), 1);
            CHECK_EQ(e.node, 0);
        }
        {
            auto f = mk();
            u32 w = 0xFEEDF00D;
            const u32 r = f->L().ServerClientState(2, &w);
            u32 want_word = 0;
            if (st == C2) want_word = Word(5, 1, 2);
            if (st == C6) want_word = Word(6, 1, 2);
            if (st == C8) want_word = Word(7, 1, 2);
            CHECK_EQ(r, st == C0 ? kInvalidState : (has_client ? 0 : kNoData));
            if (r == 0) CHECK_EQ(w, want_word);
            for (u16 n : {u16(0), u16(1), u16(3), u16(4), u16(5), u16(0xFF), u16(0x100), u16(0x0102), u16(0xFFFF)}) {
                CHECK_EQ(f->L().ServerClientState(n, &w), st == C0 ? kInvalidState : kNoData);
            }
        }

        SECTIONF("creator/matrix state %d: Accept / Disconnect / Distribution / Passphrase", st);
        {
            auto f = mk();
            for (u16 n : {u16(2), u16(1), u16(3), u16(0), u16(0x0102)})
                CHECK_EQ(f->L().ServerAcceptClient(n), st == C0 ? kInvalidState : kNoData);  // the client is already accepted
        }
        {
            auto f = mk();
            const u32 k0 = f->P().Calls(kKick);
            const u32 r = f->L().ServerDisconnectClient(2);
            CHECK_EQ(r, st == C0 ? kInvalidState : (has_client ? 0 : kNoData));
            CHECK_EQ(f->P().Calls(kKick) - k0, r == 0 ? 1 : 0);
            if (r == 0) CHECK_EQ(f->P().kicks.back(), 2);
            CHECK_EQ(f->L().ServerDisconnectClient(1), st == C0 ? kInvalidState : kNoData);
            CHECK_EQ(f->L().ServerDisconnectClient(3), st == C0 ? kInvalidState : kNoData);
            CHECK_EQ(f->L().ServerDisconnectClient(0x0102), st == C0 ? kInvalidState : kNoData);
            CHECK_EQ(f->P().Calls(kKick) - k0, r == 0 ? 1 : 0);
        }
        {
            auto f = mk();
            const u32 r = f->L().ServerStartDistribution();
            CHECK_EQ(r, st == C2 ? 0 : kInvalidState);
            if (st == C2) CHECK_EQ(f->L().ServerState(), 6);
            else CHECK_EQ(f->L().ServerState(), st);
        }
        {
            auto f = mk();
            u8* nine = Exact(kPass2, 9);
            const u32 r = f->L().ServerSendPassphrase(nine);
            delete[] nine;
            CHECK_EQ(r, st == C8 ? 0 : kInvalidState);
            if (st == C8) CHECK_EQ(f->L().ServerState(), 8);  // waits for the GO_ACKs
        }
    }
}

void T_creator_open() {
    SECTION("creator/StartAccepting arguments");
    {
        const u32 maxc[] = {0, 1, 2, 3, 4, 5, 15, 16, 255, 0x100, 0xFFFFFFFFu};
        const u8 want[] = {2, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4};
        for (size_t i = 0; i < sizeof maxc / sizeof maxc[0]; ++i) {
            CF f;
            CHECK_EQ(f.L().ServerInitialize(maxc[i], 0, nullptr), 0);
            CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
            CHECK_EQ(f.P().open_ann.size(), 1);
            if (!f.P().open_ann.empty()) CHECK_EQ(f.P().open_ann[0].max_nodes, want[i]);
        }
    }
    {
        const u32 idx[] = {0, 2, 0xFF, 0x1FF, 0x12345678u, 0xFFFFFFFFu, 0x100};
        for (const u32 ci : idx) {
            CF f;  // TD
            f.L().ServerInitialize(3, ci, nullptr);
            f.L().ServerStartAccepting(false, 6);
            CHECK_EQ(f.P().open_ann.size(), 1);
            if (!f.P().open_ann.empty()) CHECK_EQ(f.P().open_ann[0].child_tid_low, kTdTidLow | (ci & 0xFF));
            // a game whose tid low has a byte of its own: that byte is replaced by the child index
            CF g(100000, 0x00040000001C2155ull);
            g.L().ServerInitialize(3, ci, nullptr);
            g.L().ServerStartAccepting(false, 6);
            if (!g.P().open_ann.empty()) CHECK_EQ(g.P().open_ann[0].child_tid_low, 0x001C2100u | (ci & 0xFF));
        }
    }
    {
        // channel, accepting, identity, name
        const u8 ok[] = {0, 1, 6, 11};
        for (u8 ch : ok) {
            CF f;
            f.L().ServerInitialize(3, 0, nullptr);
            CHECK_EQ(f.L().ServerStartAccepting(ch % 2 == 0, ch), 0);
            CHECK_EQ(f.P().open_ch.size(), 1);
            if (!f.P().open_ch.empty()) CHECK_EQ(f.P().open_ch[0], ch);
            CHECK_EQ(f.P().open_ann[0].accepting, 1);
            CHECK_EQ(f.P().open_id[0].seed, 0xF000);
            CHECK(SameName(f.P().open_id[0].name, N("P0")));
            CHECK(SameName(f.P().open_ann[0].name, N("P0")));
            CHECK_EQ(f.L().ServerStartAccepting(false, ch), kInvalidState);  // already in 2
            CHECK_EQ(f.P().Calls(kHostOpen), 1);
            CHECK(f.L().active());
        }
        for (u32 ch = 0; ch < 300; ++ch) {
            if (ch == 0 || ch == 1 || ch == 6 || ch == 11) continue;
            CF f;
            f.L().ServerInitialize(3, 0, nullptr);
            CHECK_EQ(f.L().ServerStartAccepting(false, ch), kOutOfRange);
            CHECK_EQ(f.P().Calls(kHostOpen), 0);
            CHECK_EQ(f.L().ServerState(), 1);
        }
        for (u32 ch : {0x100u, 0x101u, 0x106u, 0x10Bu, 0x80000006u, 0xFFFFFF06u}) {
            CF f;
            f.L().ServerInitialize(3, 0, nullptr);
            CHECK_EQ(f.L().ServerStartAccepting(false, ch), kOutOfRange);
        }
        // a name given to Initialize replaces the default one, in the announce and in the identity
        CF f;
        const Nm kim = N("Kim");
        f.L().ServerInitialize(3, 0, kim.u);
        f.L().ServerStartAccepting(false, 6);
        CHECK(SameName(f.P().open_ann[0].name, kim));
        CHECK(SameName(f.P().open_id[0].name, kim));
        // ten non-zero units: no terminator is needed, none is added
        CF g;
        Nm full;
        for (u32 i = 0; i < kNameUnits; ++i) full.u[i] = static_cast<u16>(0x4E00 + i);
        g.L().ServerInitialize(3, 0, full.u);
        g.L().ServerStartAccepting(false, 6);
        CHECK(SameName(g.P().open_ann[0].name, full));
        CHECK(SameName(g.P().open_id[0].name, full));
        // the name stays when a later Initialize gives none
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(false, 6);
        CHECK_EQ(g.P().open_ann.size(), 2);
        if (g.P().open_ann.size() == 2) CHECK(SameName(g.P().open_ann[1].name, full));
    }
    SECTION("creator/HostOpen false -> kBusy");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.P().fail_host_open = true;
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), kBusy);
        CHECK_EQ(f.L().ServerState(), 1);
        CHECK(!f.L().active());
        CHECK_EQ(f.P().Calls(kHostOpen), 1);
        f.P().fail_host_open = false;
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        CHECK_EQ(f.L().ServerState(), 2);
        CHECK_EQ(f.P().Calls(kHostOpen), 2);
    }
    SECTION("creator/EndAccepting then StartAccepting: HostUpdate, not a second HostOpen");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Poll(20);
        CHECK_EQ(f.L().ServerEndAccepting(), 0);
        CHECK_EQ(f.L().ServerState(), 1);
        CHECK(f.L().active());
        CHECK_EQ(f.L().ServerEndAccepting(), 0);  // again: nothing
        CHECK_EQ(f.P().Calls(kHostUpdate), 1);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        CHECK_EQ(f.L().ServerState(), 2);
        CHECK_EQ(f.P().Calls(kHostOpen), 1);
        CHECK_EQ(f.P().Calls(kHostUpdate), 2);
        CHECK_EQ(f.P().update_acc.size(), 2);
        if (f.P().update_acc.size() == 2) {
            CHECK_EQ(f.P().update_acc[0], 0);
            CHECK_EQ(f.P().update_acc[1], 1);
        }
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 1);
        // a client that arrived a moment ago (not yet seen by any poll) is not lost by EndAccepting
        CF g;
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(false, 6);
        g.Cli(2);
        g.Cli(3);
        CHECK_EQ(g.L().ServerEndAccepting(), 0);
        g.Poll(20);
        g.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 2);
        CHECK_EQ(g.P().Calls(kKick), 0);
        // after EndAccepting a node that shows up is kicked and never stored
        g.Cli(4);
        g.Poll(20);
        g.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 2);
        CHECK(g.N()->ghost_kicked[4]);
        CHECK_EQ(g.P().Calls(kKick), 1);
    }
}

void T_creator_tracking() {
    SECTION("creator/tracking, ids, caps");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(4, "Dan");
        f.Cli(2, "Bob");
        f.Poll(20);
        u16 ids[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        u32 total = 0;
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 2);
        CHECK(ids[0] == 2 && ids[1] == 4);  // ascending, host excluded
        f.Cli(3, "Cid");
        f.Poll(20);
        ids[0] = ids[1] = ids[2] = ids[3] = 0xFFFF;
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 3);
        CHECK(ids[0] == 2 && ids[1] == 3 && ids[2] == 4 && ids[3] == 0xFFFF);
        // capacity limits what is written, not the total
        u16 two[2] = {0xFFFF, 0xFFFF};
        CHECK_EQ(f.L().ServerConnectingClients(two, 2, &total), 0);
        CHECK_EQ(total, 3);
        CHECK(two[0] == 2 && two[1] == 3);
        u16 none[1] = {0xBEEF};
        CHECK_EQ(f.L().ServerConnectingClients(none, 0, &total), 0);
        CHECK_EQ(total, 3);
        CHECK_EQ(none[0], 0xBEEF);
        // exact-size heap buffers (ASan)
        u16* heap3 = new u16[3];
        CHECK_EQ(f.L().ServerConnectingClients(heap3, 3, &total), 0);
        CHECK_EQ(total, 3);
        delete[] heap3;
        // names come from the engine
        NodeEntry e;
        std::memset(&e, 0, sizeof e);
        CHECK_EQ(f.L().ServerClientInfo(3, &e), 0);
        CHECK(SameName(e.name, N("Cid")));
        CHECK_EQ(e.seed, 0xB03);
        // a failing ReadNode: success with zeros
        f.P().fail_read[3] = true;
        std::memset(&e, 0x5A, sizeof e);
        CHECK_EQ(f.L().ServerClientInfo(3, &e), 0);
        CHECK_EQ(e.seed, 0);
        CHECK(NameZero(e.name));
        f.P().fail_read[3] = false;
        // a client leaves: the slot frees at once
        f.W().Remove(f.N(), 3);
        f.Poll(20);
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 2);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(3, &w), kNoData);
        // a new one takes the same node: a new slot
        f.Cli(3, "Eve");
        f.Poll(20);
        CHECK_EQ(f.L().ServerClientState(3, &w), 0);
        CHECK_EQ(w, Word(5, 1, 3));
        CHECK_EQ(f.P().Calls(kKick), 0);
    }
    SECTION("creator/the cap of clients");
    for (u32 maxc = 1; maxc <= 3; ++maxc) {
        CF f;
        f.L().ServerInitialize(maxc, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        for (int n = 2; n <= 4; ++n) f.Cli(n);  // the engine would not admit more than maxc: the test forces it
        f.Poll(20);
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, maxc);
        for (int n = 2; n <= 4; ++n) {
            const bool allowed = static_cast<u32>(n - 1) <= maxc;
            CHECK_EQ(f.N()->ghost_kicked[n] ? 1 : 0, allowed ? 0 : 1);
            u32 w;
            CHECK_EQ(f.L().ServerClientState(static_cast<u16>(n), &w), allowed ? 0 : kNoData);
        }
        CHECK_EQ(f.P().Calls(kKick), 3 - maxc);
        for (u8 k : f.P().kicks) CHECK(k >= 2 && k <= 4);
    }
    SECTION("creator/a node that appears in 6, 8 or after EndAccepting is kicked");
    for (int st : {C6, C8}) {
        CF f;
        CReach(f, st);
        f.Cli(3, "Late");
        f.Poll(20);
        CHECK(f.N()->ghost_kicked[3]);
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 1);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(3, &w), kNoData);
        // and it does not count to complete the phase: the first client alone decides
        if (st == C6) {
            f.Ack(2, MsgType::StartAck);
            f.Poll(20);
            CHECK_EQ(f.L().ServerState(), 8);
        }
    }
    SECTION("creator/manual acceptance");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(true, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(0x40, 0, 2));
        CHECK_EQ(f.L().ServerClientState(3, &w), 0);
        CHECK_EQ(w, Word(0x40, 0, 3));
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 2);  // waiting clients are listed too
        NodeEntry e;
        std::memset(&e, 0, sizeof e);
        CHECK_EQ(f.L().ServerClientInfo(2, &e), 0);
        CHECK_EQ(f.L().ServerAcceptClient(1), kNoData);
        CHECK_EQ(f.L().ServerAcceptClient(4), kNoData);
        CHECK_EQ(f.L().ServerAcceptClient(0x0102), kNoData);
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(0x40, 0, 2));  // 0x0102 did not hit node 2
        CHECK_EQ(f.L().ServerAcceptClient(2), 0);
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(5, 1, 2));
        CHECK_EQ(f.L().ServerAcceptClient(2), kNoData);  // twice
        CHECK_EQ(f.L().ServerClientState(3, &w), 0);
        CHECK_EQ(w, Word(0x40, 0, 3));
        // StartDistribution kicks the one still waiting, and sends START only to the accepted one
        CHECK_EQ(f.L().ServerStartDistribution(), 0);
        CHECK_EQ(f.L().ServerState(), 6);
        CHECK_EQ(f.P().Calls(kKick), 1);
        CHECK(f.N()->ghost_kicked[3]);
        CHECK_EQ(f.Sent(1, 2), 1);
        CHECK_EQ(f.Sent(1, 3), 0);
        CHECK_EQ(f.P().update_acc.back(), 0);
        f.Poll(20);
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 1);
        // all waiting: all kicked, state 8 at once
        CF g;
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(true, 6);
        g.Cli(2);
        g.Cli(3);
        g.Poll(20);
        CHECK_EQ(g.L().ServerStartDistribution(), 0);
        CHECK_EQ(g.P().Calls(kKick), 2);
        CHECK_EQ(g.Sent(1), 0);
        g.Poll(20);
        CHECK_EQ(g.L().ServerState(), 8);
    }
    SECTION("creator/DisconnectClient with a node that lingers in the engine");
    {
        CF f;
        f.W().kick_linger_ms = 500;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Poll(20);
        CHECK_EQ(f.L().ServerDisconnectClient(2), 0);
        const u64 t0 = f.W().now;
        for (int i = 0; i < 24; ++i) {
            f.Poll(20);
            u32 w = 0;
            const u32 r = f.L().ServerClientState(2, &w);
            if (r == 0) CHECK((w >> 24) != 5);  // not reborn as a new accepted client
        }
        CHECK(f.W().now - t0 < 500);
        const u32 k = f.P().Calls(kKick);
        CHECK(k >= 1 && k <= 4);  // repeated at kResendMs at most while it lingers
        f.Poll(300);
        f.Poll(20);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(2, &w), kNoData);
        u16 ids[4];
        u32 total = 9;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 0);
        // the node can come back later as a new client
        f.Cli(2, "Again");
        f.Poll(20);
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(5, 1, 2));
    }
}

// ---- the phases and their clocks ------------------------------------------------------------------------------------

void ScenarioStartGrace(u64 base) {
    SECTIONF("creator/START grace 1499/1500 base=0x%llx", static_cast<unsigned long long>(base));
    CF f(base);
    f.L().ServerInitialize(3, 0, nullptr);
    f.L().ServerStartAccepting(false, 6);
    f.Cli(2);
    f.Cli(3);
    f.Poll(20);
    const u64 t0 = f.W().now;
    CHECK_EQ(f.L().ServerStartDistribution(), 0);
    CHECK_EQ(f.L().ServerState(), 6);
    CHECK_EQ(f.Sent(1, 2), 1);
    CHECK_EQ(f.Sent(1, 3), 1);
    // the body of START is empty, 2 bytes
    SentRec rec;
    CHECK(f.NthSent(1, 3, 0, &rec));
    CHECK_EQ(rec.len, 2);
    CHECK_EQ(rec.data[0], 1);
    CHECK_EQ(rec.data[1], 0);
    CHECK_EQ(rec.from, 1);
    // resends: 199 no, 200 yes, 399 no, 400 yes
    f.W().now = t0 + 199;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(1, 3), 1);
    f.W().now = t0 + 200;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(1, 3), 2);
    CHECK_EQ(f.Sent(1, 2), 2);
    f.Ack(2, MsgType::StartAck);
    f.W().now = t0 + 399;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(1, 3), 2);
    f.W().now = t0 + 400;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(1, 3), 3);
    CHECK_EQ(f.Sent(1, 2), 2);  // acked: no more
    CHECK_EQ(f.L().ServerState(), 6);
    u32 w;
    CHECK_EQ(f.L().ServerClientState(2, &w), 0);
    CHECK_EQ(w, Word(7, 1, 2));
    CHECK_EQ(f.L().ServerClientState(3, &w), 0);
    CHECK_EQ(w, Word(6, 1, 3));
    f.W().now = t0 + 1499;
    f.L().Poll(true);
    CHECK_EQ(f.L().ServerState(), 6);
    CHECK_EQ(f.P().Calls(kKick), 0);
    f.W().now = t0 + 1500;
    f.L().Poll(true);
    CHECK_EQ(f.P().Calls(kKick), 1);
    CHECK(f.N()->ghost_kicked[3]);
    CHECK_EQ(f.L().ServerState(), 8);
    u16 ids[4];
    u32 total = 0;
    f.W().now += 20;
    f.L().Poll(true);
    f.L().ServerConnectingClients(ids, 4, &total);
    CHECK_EQ(total, 1);
    CHECK_EQ(ids[0], 2);
    CHECK_EQ(f.P().Calls(kKick), 1);  // kicked once (the engine removed it)
}

void ScenarioGoGrace(u64 base) {
    SECTIONF("creator/GO grace 1999/2000 base=0x%llx", static_cast<unsigned long long>(base));
    CF f(base);
    f.L().ServerInitialize(3, 0, nullptr);
    f.L().ServerStartAccepting(false, 6);
    f.Cli(2);
    f.Cli(3);
    f.Poll(20);
    f.L().ServerStartDistribution();
    f.Ack(2, MsgType::StartAck);
    f.Ack(3, MsgType::StartAck);
    f.Poll(20);
    CHECK_EQ(f.L().ServerState(), 8);
    f.Poll(300);  // the game takes its time
    CHECK_EQ(f.L().ServerState(), 8);
    CHECK_EQ(f.Sent(3), 0);
    const u64 g0 = f.W().now;
    CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
    CHECK_EQ(f.Sent(3, 2), 1);
    CHECK_EQ(f.Sent(3, 3), 1);
    SentRec rec;
    CHECK(f.NthSent(3, 2, 0, &rec));
    CHECK_EQ(rec.len, 11);
    CHECK_EQ(rec.data[0], 3);
    CHECK_EQ(rec.data[1], 9);
    CHECK(std::memcmp(rec.data + 2, kPass1, 9) == 0);
    u32 w;
    CHECK_EQ(f.L().ServerClientState(2, &w), 0);
    CHECK_EQ(w, Word(7, 1, 2));  // GO sent
    // the game keeps calling every frame: refused, nothing touched
    for (int i = 0; i < 20; ++i) CHECK_EQ(f.L().ServerSendPassphrase(kPass2), kInvalidState);
    CHECK_EQ(f.Sent(3, 2), 1);
    f.W().now = g0 + 199;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(3, 2), 1);
    f.W().now = g0 + 200;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(3, 2), 2);
    CHECK_EQ(f.Sent(3, 3), 2);
    CHECK(f.NthSent(3, 3, 1, &rec));
    CHECK(std::memcmp(rec.data + 2, kPass1, 9) == 0);  // the resend carries the first passphrase
    f.Ack(2, MsgType::GoAck);
    f.W().now = g0 + 400;
    f.L().Poll(true);
    CHECK_EQ(f.Sent(3, 2), 2);  // acked: no more
    CHECK_EQ(f.Sent(3, 3), 3);
    CHECK_EQ(f.L().ServerClientState(2, &w), 0);
    CHECK_EQ(w, Word(9, 1, 2));
    CHECK_EQ(f.L().ServerState(), 8);
    f.W().now = g0 + 1999;
    f.L().Poll(true);
    CHECK_EQ(f.L().ServerState(), 8);
    CHECK_EQ(f.P().Calls(kHostClose), 0);
    f.W().now = g0 + 2000;
    f.L().Poll(true);
    CHECK_EQ(f.L().ServerState(), 9);
    CHECK_EQ(f.P().Calls(kHostClose), 1);
    u16 ids[4];
    u32 total = 9;
    CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
    CHECK_EQ(total, 0);
    CHECK_EQ(f.L().ServerClientState(2, &w), kNoData);
    CHECK(!f.L().active());
    // finishing after 9 does not close again
    CHECK_EQ(f.L().ServerFinalize(), 0);
    CHECK_EQ(f.P().Calls(kHostClose), 1);
    CHECK_EQ(f.L().ServerState(), 0);
}

void T_creator_phases() {
    const u64 bases[] = {5000, 100000, 0xFFFFFF00ull, 0xFFFFF000ull, 0x100000000ull - 1500, 0x100000000ull - 1499,
                         0x100000000ull - 2000, 0x1FFFFF000ull};
    for (u64 b : bases) {
        ScenarioStartGrace(b);
        ScenarioGoGrace(b);
    }
    SECTION("creator/zero clients: 6 -> 8 and 8 -> 9 in the same call");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        CHECK_EQ(f.L().ServerStartDistribution(), 0);
        CHECK_EQ(f.L().ServerState(), 8);
        CHECK_EQ(f.Sent(1), 0);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(f.L().ServerState(), 9);
        CHECK_EQ(f.P().Calls(kHostClose), 1);
        CHECK_EQ(f.Sent(3), 0);
        u16 ids[4];
        u32 total = 5;
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 0);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass2), kInvalidState);
    }
    SECTION("creator/HostClose comes BEFORE the state 9 is visible");
    {
        CF f;
        int state_at_close = -1;
        int closes = 0;
        f.P().on_hostclose = [&]() {
            state_at_close = f.L().server_state();
            ++closes;
        };
        CReach(f, C9);
        CHECK_EQ(closes, 1);
        CHECK_EQ(state_at_close, 8);  // DESIGN sez. 16.2: the network is closed BEFORE the byte becomes 9
        CHECK_EQ(f.L().ServerState(), 9);
    }
    SECTION("creator/one client leaves in phase 6 after the other confirmed: 8 in the same poll");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        f.L().ServerStartDistribution();
        f.Ack(2, MsgType::StartAck);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 6);
        f.W().Remove(f.N(), 3);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 8);
        CHECK_EQ(f.P().Calls(kKick), 0);
    }
    SECTION("creator/one client leaves in the GO phase after the other confirmed: 9 in the same poll");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        f.L().ServerStartDistribution();
        f.Ack(2, MsgType::StartAck);
        f.Ack(3, MsgType::StartAck);
        f.Poll(20);
        f.L().ServerSendPassphrase(kPass1);
        f.Ack(2, MsgType::GoAck);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 8);
        f.W().Remove(f.N(), 3);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 9);
        CHECK_EQ(f.P().Calls(kHostClose), 1);
    }
    SECTION("creator/the only client leaves in phase 6 -> 8, in the GO phase -> 9");
    {
        CF f;
        CReach(f, C6);
        f.W().Remove(f.N(), 2);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 8);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(f.L().ServerState(), 9);
        CF g;
        CReach(g, C8);
        CHECK_EQ(g.L().ServerSendPassphrase(kPass1), 0);
        g.W().Remove(g.N(), 2);
        g.Poll(20);
        CHECK_EQ(g.L().ServerState(), 9);
    }
    SECTION("creator/60 calls of SendPassphrase with different bytes: the first one wins");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Cli(4);
        f.Poll(20);
        f.L().ServerStartDistribution();
        for (int n = 2; n <= 4; ++n) f.Ack(n, MsgType::StartAck);
        f.Poll(20);
        CHECK_EQ(f.L().ServerState(), 8);
        u32 ok = 0;
        for (int i = 0; i < 60; ++i) {
            u8 nine[9];
            for (int k = 0; k < 9; ++k) nine[k] = static_cast<u8>(0x30 + i + k);
            const u32 r = f.L().ServerSendPassphrase(nine);
            if (r == 0) ++ok;
            else CHECK_EQ(r, kInvalidState);
            f.Poll(16);  // GO_ACKs in flight, the clock runs
            if (i == 5) {
                f.Ack(2, MsgType::GoAck);
                f.Ack(3, MsgType::GoAck);
            }
        }
        CHECK_EQ(ok, 1);
        // every GO sent carries the first passphrase: 0x30..0x38
        u8 first[9];
        for (int k = 0; k < 9; ++k) first[k] = static_cast<u8>(0x30 + k);
        for (auto& s : f.P().sent)
            if (s.type == 3) {
                CHECK_EQ(s.len, 11);
                CHECK(std::memcmp(s.data + 2, first, 9) == 0);
            }
        CHECK(f.Sent(3, 2) >= 1);
        CHECK(f.Sent(3, 4) >= 1);
        // one GO per client plus the 200 ms resends (60 * 16 = 960 ms -> at most 5 resends)
        CHECK(f.Sent(3, 4) <= 1 + 5);
    }
    SECTION("creator/the passphrase is copied: the caller's buffer may change");
    {
        CF f;
        CReach(f, C8);
        u8 buf[9];
        std::memcpy(buf, kPass1, 9);
        CHECK_EQ(f.L().ServerSendPassphrase(buf), 0);
        std::memset(buf, 0xEE, 9);
        f.Poll(200);
        SentRec rec;
        CHECK(f.NthSent(3, 2, 1, &rec));
        CHECK(std::memcmp(rec.data + 2, kPass1, 9) == 0);
    }
    SECTION("creator/the host loses its own network: 2000 ms, then state 1");
    for (int st : {C2, C6, C8}) {
        for (int how = 0; how < 2; ++how) {
            SECTIONF("creator/network lost in state %d (%s)", st, how ? "MyNode()==0" : "node 1 missing");
            CF f;
            CReach(f, st);
            if (how == 0) {
                f.W().Flap(f.N(), 1, 100000000u);
            } else {
                f.P().node = 0;  // MyNode() answers 0
            }
            f.Poll(0);
            const u64 t0 = f.W().now;
            u64 end = 0;
            if (how == 0) {
                f.W().now = t0 + 1999;
                f.L().Poll(true);
                if (st != C6) CHECK_EQ(f.L().server_state(), st);  // (in 6 the START grace has already moved it to 8)
                CHECK_EQ(f.P().Calls(kHostClose), 0);
                f.W().now = t0 + 2000;
                f.L().Poll(true);
                end = 2000;
            } else {
                for (int i = 0; i < 200 && f.L().server_state() != 1; ++i) {
                    f.Poll(20);
                    end = f.W().now - t0;
                }
            }
            CHECK_EQ(f.L().server_state(), 1);
            CHECK(end <= 2060);
            CHECK_EQ(f.P().Calls(kHostClose), 1);
            CHECK(!f.L().active());
            u16 ids[4];
            u32 total = 9;
            CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
            CHECK_EQ(total, 0);
            // the game can open a new lobby
            CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
            CHECK_EQ(f.P().Calls(kHostOpen), 2);
        }
    }
    SECTION("creator/the clock jumps");
    {
        // back by 5 s in phase 6: nothing expires by mistake; forward by 60 s: one resend per client per poll
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Cli(4);
        f.Poll(20);
        f.L().ServerStartDistribution();
        f.Poll(300);
        f.W().now -= 5000;
        for (int i = 0; i < 20; ++i) f.Poll(20);
        CHECK_EQ(f.P().Calls(kKick), 0);
        CHECK_EQ(f.L().ServerState(), 6);
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 3);
        const u32 s2 = f.Sent(1, 2), s3 = f.Sent(1, 3), s4 = f.Sent(1, 4);
        f.W().now += 65000;
        f.L().Poll(true);
        CHECK(f.Sent(1, 2) - s2 <= 1);
        CHECK(f.Sent(1, 3) - s3 <= 1);
        CHECK(f.Sent(1, 4) - s4 <= 1);
        // a jump forward in state 2 (the machine sleeps): nobody is lost, nothing is sent
        CF g;
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(false, 6);
        g.Cli(2);
        g.Cli(3);
        g.Cli(4);
        g.Poll(20);
        g.W().now += 65000;
        g.L().Poll(true);
        g.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 3);
        CHECK_EQ(g.P().Calls(kKick), 0);
        CHECK_EQ(g.L().ServerState(), 2);
        CHECK_EQ(g.Sent(1) + g.Sent(3), 0);
    }
    SECTION("creator/long idle: no sends, no timers in 2 and 8");
    {
        CF f;
        CReach(f, C2);
        const u32 s0 = f.P().Calls(kSend);
        for (int i = 0; i < 1800; ++i) {  // 3 minutes at 60 Hz
            f.W().now += 16;
            f.L().Pump();
        }
        CHECK_EQ(f.P().Calls(kSend), s0);
        CHECK_EQ(f.L().ServerState(), 2);
        CF g;
        CReach(g, C8);
        const u32 g0 = g.P().Calls(kSend);
        for (int i = 0; i < 1800; ++i) {
            g.W().now += 16;
            g.L().Pump();
        }
        CHECK_EQ(g.P().Calls(kSend), g0);
        CHECK_EQ(g.L().ServerState(), 8);
        CHECK_EQ(g.P().Calls(kKick), 0);
        // a client that never acks START is resent about every 200 ms and kicked at 1500: at most 8 sends
        CF h;
        CReach(h, C6);
        for (int i = 0; i < 1800; ++i) {
            h.W().now += 16;
            h.L().Pump();
        }
        CHECK(h.Sent(1, 2) <= 9);
        CHECK(h.Sent(1, 2) >= 6);
        CHECK_EQ(h.L().ServerState(), 8);
    }
}

// ---- admission table of messages on the creator ---------------------------------------------------------------------

void T_creator_messages() {
    SECTION("creator/messages out of role or order are discarded and counted");
    {
        // START and GO arriving at a creator (wrong role), in 2 / 6 / 8
        for (int st : {C2, C6, C8}) {
            SECTIONF("creator/wrong role in state %d", st);
            CF f;
            CReach(f, st);
            const u32 bad0 = f.L().bad_messages();
            u32 w0 = 0, w1 = 0;
            f.L().ServerClientState(2, &w0);
            f.Ack(2, MsgType::Start);
            u8 go[kMaxMessage];
            const u32 len = EncodeMessage(MsgType::Go, kPass2, 9, go);
            f.W().InjectTo(f.N(), 2, 1, go, len);
            f.Poll(20);
            CHECK_EQ(f.L().bad_messages(), bad0 + 2);
            f.L().ServerClientState(2, &w1);
            CHECK_EQ(w0, w1);
            CHECK_EQ(f.L().server_state(), st);
        }
        // START_ACK in state 2 (nothing started), GO_ACK before the passphrase, from a waiting client
        {
            CF f;
            CReach(f, C2);
            const u32 bad0 = f.L().bad_messages();
            f.Ack(2, MsgType::StartAck);
            f.Ack(2, MsgType::GoAck);
            f.Poll(20);
            CHECK_EQ(f.L().bad_messages(), bad0 + 2);
            u32 w;
            f.L().ServerClientState(2, &w);
            CHECK_EQ(w, Word(5, 1, 2));
            CF g;
            CReach(g, C6);
            const u32 bad1 = g.L().bad_messages();
            g.Ack(2, MsgType::GoAck);  // no passphrase sent yet
            g.Poll(20);
            CHECK_EQ(g.L().bad_messages(), bad1 + 1);
            g.L().ServerClientState(2, &w);
            CHECK_EQ(w, Word(6, 1, 2));
            CHECK_EQ(g.L().server_state(), 6);
            // a waiting (not accepted) client's START_ACK is ignored
            CF h;
            h.L().ServerInitialize(3, 0, nullptr);
            h.L().ServerStartAccepting(true, 6);
            h.Cli(2);
            h.Poll(20);
            const u32 bad2 = h.L().bad_messages();
            h.Ack(2, MsgType::StartAck);
            h.Poll(20);
            CHECK_EQ(h.L().bad_messages(), bad2 + 1);
            h.L().ServerClientState(2, &w);
            CHECK_EQ(w, Word(0x40, 0, 2));
        }
        // acks from nodes that are not tracked: 3 and 4 when only node 2 is a client
        {
            CF f;
            CReach(f, C6);
            const u32 bad0 = f.L().bad_messages();
            f.Ack(3, MsgType::StartAck);
            f.Ack(4, MsgType::StartAck);
            f.Poll(20);
            CHECK_EQ(f.L().bad_messages(), bad0 + 2);
            CHECK_EQ(f.L().server_state(), 6);
            CF g;
            CReach(g, C8);
            g.L().ServerSendPassphrase(kPass1);
            const u32 bad1 = g.L().bad_messages();
            g.Ack(3, MsgType::GoAck);
            g.Ack(4, MsgType::GoAck);
            g.Poll(20);
            CHECK_EQ(g.L().bad_messages(), bad1 + 2);
            CHECK_EQ(g.L().server_state(), 8);
        }
        // a message "from node 1" (the host itself) or from node 0 never counts as an ack
        {
            CF f;
            CReach(f, C6);
            f.W().InjectTo(f.N(), 1, 1, "\x02\x00", 2);
            f.W().InjectTo(f.N(), 0, 1, "\x02\x00", 2);
            f.Poll(20);
            CHECK_EQ(f.L().server_state(), 6);
        }
    }
    SECTION("creator/acks are idempotent and monotonic");
    {
        CF f;
        CReach(f, C8);
        u32 w;
        for (int i = 0; i < 5; ++i) f.Ack(2, MsgType::StartAck);  // duplicates after 8
        f.Poll(20);
        f.L().ServerClientState(2, &w);
        CHECK_EQ(w, Word(7, 1, 2));
        CHECK_EQ(f.L().server_state(), 8);
        f.L().ServerSendPassphrase(kPass1);
        f.Ack(2, MsgType::GoAck);
        f.Poll(20);
        CHECK_EQ(f.L().server_state(), 9);
        // GO_ACK twice, a stale START_ACK after: no effect on a closed lobby, and nothing breaks
        f.Ack(2, MsgType::GoAck);
        f.Ack(2, MsgType::StartAck);
        f.Poll(20);
        CHECK_EQ(f.L().server_state(), 9);
        CF g;
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(false, 6);
        g.Cli(2);
        g.Cli(3);
        g.Poll(20);
        g.L().ServerStartDistribution();
        g.Ack(2, MsgType::StartAck);
        g.Ack(3, MsgType::StartAck);
        g.Poll(20);
        g.L().ServerSendPassphrase(kPass1);
        g.Ack(2, MsgType::GoAck);
        g.Ack(2, MsgType::StartAck);  // a late START_ACK after GO_ACK must not lower the client
        g.Poll(20);
        g.L().ServerClientState(2, &w);
        CHECK_EQ(w, Word(9, 1, 2));
        CHECK_EQ(g.L().server_state(), 8);
    }
}

}  // namespace

// =====================================================================================================================
// 5. Ownership of the network, events on the creator, and whole sessions between real lobbies.

namespace {

void CheckClean(Rig& r, const char* what) {
    SECTIONF("contract of LobbyNet respected (%s)", what);
    for (auto& c : r.c) {
        const Port& p = c->port;
        CHECK_EQ(p.bad_send, 0);
        CHECK_EQ(p.bad_kick, 0);
        CHECK_EQ(p.double_open, 0);
        CHECK_EQ(p.bad_scanread, 0);
        CHECK_EQ(p.bad_read, 0);
        CHECK_EQ(p.small_cap, 0);
        CHECK_EQ(p.connect_while_busy, 0);
    }
    CHECK_EQ(r.w.overlap, 0);
}

void T_ownership() {
    SECTION("ownership/the creator in 2 is untouched by the searcher's commands");
    {
        CF f;
        CReach(f, C2);
        Port& p = f.P();
        const u32 w0 = p.Writes();
        CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(f.L().ClientStopScan(), 0);
        CHECK_EQ(f.L().ClientStopSession(), 0);
        CHECK_EQ(f.L().ClientFinalize(), 0);
        CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(p.Writes(), w0);
        CHECK_EQ(p.Calls(kHostClose), 0);
        CHECK_EQ(p.Calls(kKick), 0);
        CHECK_EQ(p.Calls(kHostOpen), 1);
        CHECK_EQ(f.L().ServerState(), 2);
        CHECK(f.L().active());
        CHECK(f.N() != nullptr);
        u16 ids[4];
        u32 total = 0;
        f.L().ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 1);
        // the searcher cannot take the network
        const u32 t0 = p.Total();
        CHECK_EQ(f.L().ClientStartScan(0), kBusy);
        CHECK_EQ(p.Total(), t0);  // zero calls into the net
        CHECK_EQ(f.L().client_state(), 1);
        u32 res = 0;
        const JoinStep js = f.L().ClientJoinBegin(M(0x55), &res);
        CHECK(js == JoinStep::Failed);
        CHECK_EQ(res, kNoData);  // DESIGN sez. 16.1: the MAC is checked before the owner, and the creator's table is empty
        CHECK_EQ(p.Writes(), w0);
        // the creator carries on
        f.Ack(2, MsgType::StartAck);
        CHECK_EQ(f.L().ServerStartDistribution(), 0);
    }
    SECTION("ownership/the searcher holds the radio: the creator gets kBusy and leaves it alone");
    for (int st : {S2, S3, S5, S6, S9}) {
        SECTIONF("ownership/searcher state %d vs the creator", st);
        SF f;
        Reach(f, st);
        Port& p = f.P();
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);  // never fails for that reason
        CHECK_EQ(f.L().client_state(), st);
        const u32 w0 = p.Writes(), t0 = p.Total();
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), kBusy);
        CHECK_EQ(p.Total(), t0);  // zero calls
        CHECK_EQ(f.L().ServerState(), 1);
        CHECK_EQ(f.L().ServerEndAccepting(), 0);
        CHECK_EQ(f.L().ServerFinalize(), 0);
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(p.Writes(), w0);  // it never touched the searcher's radio
        CHECK_EQ(f.L().client_state(), st);
        if (st == S9) {
            u8 pp[9];
            f.L().ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kPass1, 9) == 0);  // a creator teardown does not erase the searcher's passphrase
        }
    }
    SECTION("ownership/the searcher's passphrase survives the creator's Initialize/Finalize, not its own Finalize");
    {
        SF f;
        Reach(f, S9);
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerFinalize();
        f.L().ServerInitialize(1, 0, nullptr);
        u8 pp[9];
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass1, 9) == 0);
        CHECK_EQ(f.L().ClientFinalize(), 0);
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kZero9, 9) == 0);
        // the TD order: passphrase read at 9, FKCL Finalize, then the creator opens
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        CHECK_EQ(f.L().ServerState(), 2);
    }
    SECTION("ownership/the creator's passphrase survives the searcher's teardowns");
    {
        CF f;
        CReach(f, C8);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
        f.Adv(10);
        CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(f.L().ClientFinalize(), 0);
        CHECK_EQ(f.L().ClientStopSession(), 0);
        CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        f.Poll(200);
        SentRec rec;
        CHECK(f.NthSent(3, 2, 1, &rec));  // the resend
        CHECK(std::memcmp(rec.data + 2, kPass1, 9) == 0);
        CHECK_EQ(f.L().ServerState(), 8);
    }
    SECTION("ownership/TD order: FKCL scan + Finalize, then SRVR Initialize + StartAccepting");
    {
        SF f;
        DoScan(f, kTdFilter);
        CHECK_EQ(f.L().ClientStopScan(), 0);
        CHECK_EQ(f.L().ClientFinalize(), 0);
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        CHECK_EQ(f.L().ServerState(), 2);
        CHECK(f.L().active());
        CHECK_EQ(f.P().Calls(kHostOpen), 1);
        // and the other way round, after the creator is done
        CHECK_EQ(f.L().ServerFinalize(), 0);
        CHECK(!f.L().active());
        CHECK_EQ(f.L().ClientInitialize(nullptr, 16), 0);
        CHECK_EQ(f.L().ClientStartScan(0), 0);
        CHECK_EQ(f.L().client_state(), 2);
    }
    SECTION("ownership/a frozen table of the searcher is dropped when the creator takes the radio");
    {
        SF f;
        DoScan(f, kTdFilter);
        f.L().ClientStopScan();
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        f.L().ServerInitialize(3, 0, nullptr);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
        CHECK_EQ(Order(f.L()).size(), 0);
        u32 res = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Failed);
        // the creator is closed, the searcher starts afresh
        f.L().ServerFinalize();
        CHECK_EQ(f.L().ClientStartScan(0), 0);
        f.Poll(50);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
    }
    SECTION("ownership/the creator in state 1 without a network does not block the searcher");
    {
        SF f;
        f.L().ServerInitialize(3, 0, nullptr);
        CHECK_EQ(f.L().ClientStartScan(0), 0);
        CHECK_EQ(f.L().client_state(), 2);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), kBusy);  // the searcher scans now
    }
}

// ---- OnGameUdsStarting on the creator ---------------------------------------------------------------------------------

void T_events_creator() {
    const int sts[] = {C0, C1, C2, C6, C8, C9};
    for (int st : sts) {
        SECTIONF("event/creator state %d", st);
        CF f;
        CReach(f, st);
        Port& p = f.P();
        const u32 hc = p.Calls(kHostClose), k0 = p.Calls(kKick);
        const bool held = st == C2 || st == C6 || st == C8;
        const bool first = f.L().OnGameUdsStarting();
        CHECK_EQ(first, held);
        CHECK(!f.L().OnGameUdsStarting());
        CHECK_EQ(p.Calls(kHostClose) - hc, held ? 1 : 0);
        CHECK_EQ(p.Calls(kKick), k0);
        CHECK(!f.L().active());
        if (held) {
            CHECK_EQ(f.L().ServerState(), 1);
            u16 ids[4];
            u32 total = 9;
            CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
            CHECK_EQ(total, 0);
            CHECK(!f.L().OnGameUdsStarting());
        } else {
            CHECK_EQ(f.L().ServerState(), st);
        }
        if (st != C0) {
            // the game finishes: a lobby can be opened again afterwards (state 1) or not (9, 0)
            const u32 r = f.L().ServerStartAccepting(false, 6);
            CHECK_EQ(r, (st == C1 || held) ? 0 : kInvalidState);
        }
        // the clock goes on: nothing wakes up
        const u32 w0 = p.Writes();
        if (!held || true) {
            for (int i = 0; i < 100; ++i) {
                f.W().now += 50;
                f.L().Pump();
            }
        }
        if (!(st == C1 || held)) CHECK_EQ(p.Writes(), w0);
    }
    SECTION("event/creator in 1 with the network open (after EndAccepting)");
    {
        CF f;
        CReach(f, C2);
        f.L().ServerEndAccepting();
        CHECK_EQ(f.L().ServerState(), 1);
        CHECK(f.L().active());
        CHECK(f.L().OnGameUdsStarting());
        CHECK_EQ(f.P().Calls(kHostClose), 1);
        CHECK_EQ(f.L().ServerState(), 1);
        CHECK(!f.L().OnGameUdsStarting());
    }
}

// ---- whole sessions between real lobbies ------------------------------------------------------------------------------

struct E2E {
    Rig r;
    Con* h = nullptr;
    std::vector<Con*> c;
    Net* hn = nullptr;
    int n;
    E2E(int nclients, u64 start = 100000, bool manual = false, bool br = false) : r(start), n(nclients) {
        h = &r.Add();
        for (int i = 0; i < n; ++i) c.push_back(&r.Add());
        CHECK_EQ(h->lobby.ServerInitialize(br ? 4 : 3, 0, nullptr), 0);
        CHECK_EQ(h->lobby.ServerStartAccepting(manual, 6), 0);
        hn = r.w.ByMac(h->port.mac);
        for (Con* x : c) {
            CHECK_EQ(x->lobby.ClientInitialize(nullptr, 16), 0);
            CHECK_EQ(x->lobby.ClientStartScan(kTdFilter), 0);
        }
        r.Run(200);
        for (Con* x : c) {
            const std::vector<Mac> o = Order(x->lobby);
            CHECK_EQ(o.size(), 1);
            x->lobby.ClientStopScan();
            u32 res = 0;
            CHECK(x->lobby.ClientJoinBegin(h->port.mac, &res) == JoinStep::Pending);
        }
        r.Run(120);
        for (Con* x : c) {
            u32 out = 0xBEEF;
            CHECK(x->lobby.ClientJoinPoll(&out));
            CHECK_EQ(out, 0);
            CHECK_EQ(x->lobby.client_state(), 5);
        }
        if (manual) AcceptAll();
        r.Run(100);
    }
    void AcceptAll() {
        u16 ids[4];
        u32 total = 0;
        h->lobby.ServerConnectingClients(ids, 4, &total);
        for (u32 i = 0; i < total && i < 4; ++i) {
            u32 w = 0;
            h->lobby.ServerClientState(ids[i], &w);
            if ((w >> 24) == 0x40) CHECK_EQ(h->lobby.ServerAcceptClient(ids[i]), 0);
        }
    }
    u32 HostClients() {
        u16 ids[4];
        u32 total = 99;
        CHECK_EQ(h->lobby.ServerConnectingClients(ids, 4, &total), 0);
        return total;
    }
};

struct HostGame {  // the game on the creator: from state 8 it calls SendPassphrase EVERY frame with new random bytes
    u8 first[9] = {};
    bool have_first = false;
    u32 ok_calls = 0;
    u32 calls = 0;
    u32 seed = 7;
    u32 first_9_ms = 0;
};

// One frame of the game of the creator. Returns true when it has "state 9 and zero clients".
bool HostFrame(E2E& e, HostGame& g) {
    const u32 st = e.h->lobby.ServerState();
    if (st == 8) {
        u8 nine[9];
        for (int k = 0; k < 8; ++k) {
            g.seed = g.seed * 1664525u + 1013904223u;
            nine[k] = static_cast<u8>("0123456789abcdef"[(g.seed >> 24) & 15]);
        }
        nine[8] = 0;
        const u32 r = e.h->lobby.ServerSendPassphrase(nine);
        ++g.calls;
        if (r == 0) {
            ++g.ok_calls;
            std::memcpy(g.first, nine, 9);
            g.have_first = true;
        } else {
            CHECK_EQ(r, kInvalidState);
        }
    }
    if (st == 9) {
        u16 ids[4];
        u32 total = 99;
        CHECK_EQ(e.h->lobby.ServerConnectingClients(ids, 4, &total), 0);
        return total == 0;
    }
    return false;
}

void Frame(E2E& e, u32 ms = 16) {
    e.r.w.now += ms;
    e.r.PumpAll();
}

void RunNominal(int n, bool br, bool manual) {
    SECTIONF("e2e/nominal n=%d %s%s", n, br ? "BR" : "TD", manual ? " manual" : "");
    E2E e(n, 100000, manual, br);
    HostGame g;
    CHECK_EQ(e.HostClients(), n);
    // the host sees the right clients with the right names
    u16 ids[4] = {};
    u32 total = 0;
    e.h->lobby.ServerConnectingClients(ids, 4, &total);
    for (int i = 0; i < n; ++i) {
        CHECK_EQ(ids[i], 2 + i);
        NodeEntry ne;
        std::memset(&ne, 0, sizeof ne);
        CHECK_EQ(e.h->lobby.ServerClientInfo(ids[i], &ne), 0);
        CHECK_EQ(ne.seed, 0xF001u + static_cast<u64>(i));
        const std::string nm = std::string("P") + static_cast<char>('1' + i);
        CHECK(SameName(ne.name, N(nm.c_str())));
        CHECK_EQ(ne.node, 2 + i);
    }
    for (Con* x : e.c) {
        CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 5);
        u16 cn[4];
        u32 t = 0;
        CHECK_EQ(x->lobby.ClientConnectingNodes(cn, 4, &t), 0);
        CHECK_EQ(t, n + 1);
    }
    const u64 t0 = e.r.w.now;
    CHECK_EQ(e.h->lobby.ServerStartDistribution(), 0);
    bool done = false;
    u64 t9 = 0;
    for (int i = 0; i < 600 && !done; ++i) {
        Frame(e);
        for (Con* x : e.c) x->lobby.ClientStatusWord();
        done = HostFrame(e, g);
        if (done) t9 = e.r.w.now - t0;
    }
    CHECK(done);
    CHECK(t9 < 600);
    CHECK_EQ(e.h->lobby.ServerState(), 9);
    CHECK_EQ(e.h->port.Calls(kHostClose), 1);
    CHECK_EQ(e.h->port.Calls(kKick), 0);
    CHECK_EQ(e.h->lobby.bad_messages(), 0);
    CHECK(!e.h->lobby.active());
    CHECK_EQ(g.ok_calls, 1);
    if (n == 0) CHECK_EQ(g.calls, 1);
    Frame(e, 100);
    for (int i = 0; i < n; ++i) {
        Con* x = e.c[static_cast<size_t>(i)];
        CHECK_EQ(x->lobby.ClientStatusWord(), (9u << 24) | (1u << 16) | static_cast<u32>(2 + i));
        u8 pp[9];
        CHECK_EQ(x->lobby.ClientPassphrase(pp), 0);
        CHECK(std::memcmp(pp, g.first, 9) == 0);  // the FIRST passphrase the game generated
        CHECK_EQ(x->lobby.bad_messages(), 0);
        u16 cn[4];
        u32 t = 0;
        x->lobby.ClientConnectingNodes(cn, 4, &t);
        CHECK_EQ(t, n + 1);
        for (u32 k = 0; k < t; ++k) CHECK_EQ(cn[k], k + 1);
        NodeEntry ne;
        std::memset(&ne, 0, sizeof ne);
        CHECK_EQ(x->lobby.ClientNodeInfo(1, &ne), 0);
        CHECK_EQ(ne.seed, 0xF000);
        CHECK(SameName(ne.name, N("P0")));
        CHECK_EQ(x->port.Calls(kKick), 0);
        // the TD end: read the passphrase, Finalize, the lobby gives the radio back
        CHECK_EQ(x->lobby.ClientFinalize(), 0);
        CHECK(!x->lobby.active());
    }
    // later the radio is quiet
    for (int i = 0; i < 200; ++i) Frame(e, 50);
    CheckClean(e.r, "nominal");
    CHECK_EQ(e.h->port.Calls(kHostClose), 1);
    CHECK_EQ(e.h->lobby.ServerFinalize(), 0);
}

void T_e2e_nominal() {
    for (int n = 0; n <= 3; ++n) {
        RunNominal(n, false, false);
        RunNominal(n, true, false);
        RunNominal(n, false, true);
    }
    SECTION("e2e/the room is full: a fifth console cannot get in");
    {
        E2E e(3);
        Con& x = e.r.Add();
        x.lobby.ClientInitialize(nullptr, 16);
        x.lobby.ClientStartScan(kTdFilter);
        e.r.Run(200);
        std::vector<Mac> o = Order(x.lobby);
        // the beacon says 4 of 4: it is listed (the game hides it); a join gets no answer: a timeout, no seat
        x.lobby.ClientStopScan();
        u32 res = 0;
        if (!o.empty()) {
            CHECK(x.lobby.ClientJoinBegin(e.h->port.mac, &res) == JoinStep::Pending);
            e.r.Run(2990);
            u32 out = 0;
            CHECK(!x.lobby.ClientJoinPoll(&out));
            e.r.Run(20);
            CHECK(x.lobby.ClientJoinPoll(&out));
            CHECK_EQ(out, kTimeout);
        }
        CHECK_EQ(e.HostClients(), 3);
        CHECK_EQ(e.h->port.Calls(kKick), 0);
        CheckClean(e.r, "full room");
    }
}

// Losing messages. n=1 client; the rule drops `count` messages of `type` (-1 = for ever).
struct LossResult {
    int host_state;
    int cli_state;
    bool cli_has_pass;
    u32 host9_ms;  // from StartDistribution
    u32 cli9_ms;
    u32 cli80_ms;
    bool pass_ok;
};
LossResult RunLoss(u8 type, int count, int n = 1, u32 run_ms = 8000) {
    E2E e(n);
    HostGame g;
    e.r.w.Drop(type, 0, 0, count);
    LossResult lr{};
    lr.host9_ms = lr.cli9_ms = lr.cli80_ms = 0xFFFFFFFF;
    const u64 t0 = e.r.w.now;
    CHECK_EQ(e.h->lobby.ServerStartDistribution(), 0);
    for (u32 t = 0; t < run_ms; t += 16) {
        Frame(e);
        HostFrame(e, g);
        if (e.h->lobby.ServerState() == 9 && lr.host9_ms == 0xFFFFFFFF) lr.host9_ms = static_cast<u32>(e.r.w.now - t0);
        for (Con* x : e.c) {
            const u32 s = x->lobby.ClientStatusWord() >> 24;
            if (s == 9 && lr.cli9_ms == 0xFFFFFFFF) lr.cli9_ms = static_cast<u32>(e.r.w.now - t0);
            if (s == 0x80 && lr.cli80_ms == 0xFFFFFFFF) lr.cli80_ms = static_cast<u32>(e.r.w.now - t0);
        }
    }
    lr.host_state = e.h->lobby.ServerState();
    lr.cli_state = static_cast<int>(e.c[0]->lobby.ClientStatusWord() >> 24);
    u8 pp[9];
    e.c[0]->lobby.ClientPassphrase(pp);
    lr.cli_has_pass = std::memcmp(pp, kZero9, 9) != 0;
    lr.pass_ok = !lr.cli_has_pass || (g.have_first && std::memcmp(pp, g.first, 9) == 0);
    CheckClean(e.r, "loss run");
    return lr;
}

void T_e2e_loss() {
    const u8 START = 1, SACK = 2, GO = 3, GACK = 4;
    SECTION("loss/START lost 1 and 3 times: resent every 200 ms");
    for (int cnt : {1, 3}) {
        SECTIONF("loss/START lost %d times", cnt);
        const LossResult r = RunLoss(START, cnt);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);
        CHECK(r.cli_has_pass && r.pass_ok);
        CHECK(r.host9_ms < 200u * static_cast<u32>(cnt) + 900);
        CHECK(r.host9_ms >= 200u * static_cast<u32>(cnt));  // it cannot be faster than the resends
    }
    SECTION("loss/START lost for ever: the client is dropped at 1500 ms, the creator goes on");
    {
        const LossResult r = RunLoss(START, -1);
        CHECK_EQ(r.host_state, 9);
        CHECK(r.host9_ms >= 1500 && r.host9_ms <= 1500 + 200);
        CHECK(!r.cli_has_pass);
        CHECK_EQ(r.cli_state, 0x80);  // kicked: the engine took it out of the network
    }
    for (int cnt : {1, 3}) {
        SECTIONF("loss/START_ACK lost %d times: the creator waits, the searcher stays in 6", cnt);
        const LossResult r = RunLoss(SACK, cnt);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);
        CHECK(r.cli_has_pass && r.pass_ok);
        CHECK(r.host9_ms >= 200u * static_cast<u32>(cnt));
        CHECK(r.host9_ms < 200u * static_cast<u32>(cnt) + 900);
    }
    SECTION("loss/START_ACK lost for ever");
    {
        const LossResult r = RunLoss(SACK, -1);
        CHECK_EQ(r.host_state, 9);
        CHECK(r.host9_ms >= 1500 && r.host9_ms <= 1500 + 200);
        CHECK_EQ(r.cli_state, 0x80);
        CHECK(!r.cli_has_pass);
    }
    for (int cnt : {1, 3}) {
        SECTIONF("loss/GO lost %d times", cnt);
        const LossResult r = RunLoss(GO, cnt);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);
        CHECK(r.cli_has_pass && r.pass_ok);
        CHECK(r.host9_ms < 200u * static_cast<u32>(cnt) + 900);
    }
    SECTION("loss/GO lost for ever: the creator reaches 9 at 2000 ms, the searcher sees the host vanish");
    {
        const LossResult r = RunLoss(GO, -1);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 0x80);
        CHECK(!r.cli_has_pass);
        CHECK(r.host9_ms >= 2000);
        CHECK(r.host9_ms <= 2000 + 400);
        CHECK(r.cli80_ms != 0xFFFFFFFF);
        CHECK(r.cli80_ms <= r.host9_ms + 2000 + 60);
    }
    for (int cnt : {1, 3}) {
        SECTIONF("loss/GO_ACK lost %d times", cnt);
        const LossResult r = RunLoss(GACK, cnt);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);
        CHECK(r.cli_has_pass && r.pass_ok);
        CHECK(r.host9_ms < 200u * static_cast<u32>(cnt) + 900);
    }
    SECTION("loss/GO_ACK lost for ever: the creator reaches 9 at kGoGraceMs and the searcher HAS the passphrase");
    {
        const LossResult r = RunLoss(GACK, -1);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);  // the host closed the network: 9 is final
        CHECK(r.cli_has_pass && r.pass_ok);
        CHECK(r.host9_ms >= 2000 && r.host9_ms <= 2000 + 400);
    }
    SECTION("loss/2 and 3 clients with a different client losing its answers");
    for (int n : {2, 3}) {
        const LossResult r = RunLoss(SACK, 2, n);
        CHECK_EQ(r.host_state, 9);
        CHECK_EQ(r.cli_state, 9);
        const LossResult q = RunLoss(GACK, -1, n);
        CHECK_EQ(q.host_state, 9);
        CHECK_EQ(q.cli_state, 9);
    }
    SECTION("loss/every message duplicated 2 more times");
    for (int n : {1, 3}) {
        E2E e(n);
        for (int t = 1; t <= 4; ++t) e.r.w.dup_extra[t] = 2;
        HostGame g;
        e.h->lobby.ServerStartDistribution();
        bool done = false;
        for (int i = 0; i < 300 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        for (Con* x : e.c) {
            CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 9);
            u8 pp[9];
            x->lobby.ClientPassphrase(pp);
            CHECK(std::memcmp(pp, g.first, 9) == 0);
            // every duplicate gets its answer: START_ACKs == 3 * STARTs received (1 START sent, 3 copies)
            CHECK_EQ(x->port.Sent(2), 3);
            CHECK_EQ(x->port.Sent(4), 3);
        }
        CheckClean(e.r, "duplicates");
    }
    SECTION("loss/messages delayed 300 ms");
    {
        E2E e(2);
        for (int t = 1; t <= 4; ++t) e.r.w.delay_ms[t] = 300;
        HostGame g;
        e.h->lobby.ServerStartDistribution();
        bool done = false;
        for (int i = 0; i < 400 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        for (Con* x : e.c) CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 9);
    }
    SECTION("loss/Send fails (the frame is not even queued) a few times");
    {
        E2E e(2);
        HostGame g;
        e.h->port.fail_send = 5;
        e.c[0]->port.fail_send = 3;
        e.h->lobby.ServerStartDistribution();
        bool done = false;
        for (int i = 0; i < 400 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        for (Con* x : e.c) CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 9);
        CheckClean(e.r, "send fails");
    }
    SECTION("loss/a partition between the host and one client for 800 ms");
    {
        E2E e(2);
        e.r.w.parts.push_back(Part{1, 3, e.r.w.now + 800});
        HostGame g;
        e.h->lobby.ServerStartDistribution();
        bool done = false;
        for (int i = 0; i < 400 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        for (Con* x : e.c) CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 9);
    }
    SECTION("loss/a client leaves in phase 6 after the others confirmed");
    {
        E2E e(3);
        HostGame g;
        e.r.w.Drop(2, 4, 1, -1);  // the client of node 4 never acks
        e.h->lobby.ServerStartDistribution();
        for (int i = 0; i < 10; ++i) Frame(e);  // 160 ms: nodes 2 and 3 have acked, node 4 is being resent
        CHECK_EQ(e.h->lobby.ServerState(), 6);
        // the third console quits (the game calls StopSession)
        CHECK_EQ(e.c[2]->lobby.ClientStopSession(), 0);
        Frame(e, 20);
        CHECK_EQ(e.h->lobby.ServerState(), 8);  // at the next poll: the completion is recomputed
        CHECK_EQ(e.h->port.Calls(kKick), 0);
        CHECK_EQ(e.HostClients(), 2);
        bool done = false;
        for (int i = 0; i < 300 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        CHECK_EQ(e.c[0]->lobby.ClientStatusWord() >> 24, 9);
        CHECK_EQ(e.c[1]->lobby.ClientStatusWord() >> 24, 9);
        CHECK_EQ(e.c[2]->lobby.ClientStatusWord() >> 24, 1);
    }
    SECTION("loss/forged messages between clients are ignored, the session still ends");
    {
        E2E e(3);
        HostGame g;
        // node 2 "sends" GO and START to node 3, node 3 "sends" them to node 2, node 4 sends acks to node 3
        u8 go[kMaxMessage], st[kMaxMessage];
        const u32 lg = EncodeMessage(MsgType::Go, kPass2, 9, go);
        const u32 ls = EncodeMessage(MsgType::Start, nullptr, 0, st);
        e.r.w.InjectTo(e.hn, 2, 3, go, lg);
        e.r.w.InjectTo(e.hn, 2, 3, st, ls);
        e.r.w.InjectTo(e.hn, 3, 2, go, lg);
        e.r.w.InjectTo(e.hn, 4, 2, st, ls);
        Frame(e);
        Frame(e);
        for (Con* x : e.c) {
            CHECK_EQ(x->lobby.client_state(), 5);
            u8 pp[9];
            x->lobby.ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kZero9, 9) == 0);
        }
        CHECK_EQ(e.c[0]->lobby.bad_messages(), 2);
        CHECK_EQ(e.c[1]->lobby.bad_messages(), 2);
        e.h->lobby.ServerStartDistribution();
        bool done = false;
        for (int i = 0; i < 300 && !done; ++i) {
            Frame(e);
            done = HostFrame(e, g);
        }
        CHECK(done);
        for (Con* x : e.c) {
            CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 9);
            u8 pp[9];
            x->lobby.ClientPassphrase(pp);
            CHECK(std::memcmp(pp, g.first, 9) == 0);  // not the forged kPass2
        }
    }
}

void T_e2e_misc() {
    SECTION("e2e/10 s with no IPC: only Pump every 50 ms does the work");
    {
        E2E e(3);
        // the game's only commands: StartDistribution once, SendPassphrase once when it sees 8
        e.h->lobby.ServerStartDistribution();
        bool sent = false;
        for (int i = 0; i < 200; ++i) {  // 10 s
            e.r.w.now += 50;
            e.r.PumpAll();
            if (!sent && e.h->lobby.server_state() == 8) {
                CHECK_EQ(e.h->lobby.ServerSendPassphrase(kPass1), 0);
                sent = true;
            }
        }
        CHECK(sent);
        CHECK_EQ(e.h->lobby.server_state(), 9);
        for (Con* x : e.c) {
            CHECK_EQ(x->lobby.client_state(), 9);
            u8 pp[9];
            x->lobby.ClientPassphrase(pp);
            CHECK(std::memcmp(pp, kPass1, 9) == 0);
        }
    }
    SECTION("e2e/a client enters with no IPC at all but Pump (the join ends by itself)");
    {
        Rig r;
        Con& h = r.Add();
        Con& c = r.Add();
        h.lobby.ServerInitialize(3, 0, nullptr);
        h.lobby.ServerStartAccepting(false, 6);
        c.lobby.ClientInitialize(nullptr, 16);
        c.lobby.ClientStartScan(kTdFilter);
        r.Run(200);
        c.lobby.ClientStopScan();
        u32 res = 0;
        c.lobby.ClientJoinBegin(h.port.mac, &res);
        r.Run(10000, 50);
        CHECK_EQ(c.lobby.client_state(), 5);
        u16 ids[4];
        u32 total = 0;
        h.lobby.ServerConnectingClients(ids, 4, &total);
        CHECK_EQ(total, 1);
        u32 out = 0xBEEF;
        CHECK(c.lobby.ClientJoinPoll(&out));
        CHECK_EQ(out, 0);
    }
    SECTION("e2e/the host closes: searchers see 0x80 in 5 and keep 9");
    {
        E2E e(2);
        e.h->lobby.ServerFinalize();  // HostClose: both clients are dropped by the engine
        for (int i = 0; i < 200; ++i) Frame(e, 20);
        for (Con* x : e.c) CHECK_EQ(x->lobby.ClientStatusWord() >> 24, 0x80);
        CHECK_EQ(e.h->port.Calls(kHostClose), 1);
        for (Con* x : e.c) CHECK_EQ(x->port.Calls(kDisconnect), 1);
    }
    SECTION("e2e/two rooms on the same air do not interfere");
    {
        Rig r;
        Con& h1 = r.Add();
        Con& h2 = r.Add();
        Con& c1 = r.Add();
        Con& c2 = r.Add();
        h1.lobby.ServerInitialize(3, 0, nullptr);
        h2.lobby.ServerInitialize(3, 1, nullptr);
        h1.lobby.ServerStartAccepting(false, 6);
        h2.lobby.ServerStartAccepting(false, 1);
        c1.lobby.ClientInitialize(nullptr, 16);
        c2.lobby.ClientInitialize(nullptr, 16);
        c1.lobby.ClientStartScan(kTdFilter);
        c2.lobby.ClientStartScan(kTdFilter);
        r.Run(200);
        CHECK_EQ(Order(c1.lobby).size(), 2);
        CHECK_EQ(Order(c2.lobby).size(), 2);
        c1.lobby.ClientStopScan();
        c2.lobby.ClientStopScan();
        u32 res = 0;
        CHECK(c1.lobby.ClientJoinBegin(h1.port.mac, &res) == JoinStep::Pending);
        CHECK(c2.lobby.ClientJoinBegin(h2.port.mac, &res) == JoinStep::Pending);
        r.Run(200);
        u32 out = 0;
        CHECK(c1.lobby.ClientJoinPoll(&out));
        CHECK(c2.lobby.ClientJoinPoll(&out));
        // room 1 goes all the way
        CHECK_EQ(h1.lobby.ServerStartDistribution(), 0);
        r.Run(300);
        CHECK_EQ(h1.lobby.server_state(), 8);
        CHECK_EQ(h1.lobby.ServerSendPassphrase(kPass1), 0);
        r.Run(300);
        CHECK_EQ(h1.lobby.server_state(), 9);
        CHECK_EQ(c1.lobby.client_state(), 9);
        // room 2 has not moved
        CHECK_EQ(h2.lobby.server_state(), 2);
        CHECK_EQ(c2.lobby.client_state(), 5);
        CHECK_EQ(c2.lobby.bad_messages(), 0);
        CHECK_EQ(h2.lobby.bad_messages(), 0);
        u8 pp[9];
        c2.lobby.ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kZero9, 9) == 0);
        CheckClean(r, "two rooms");
    }
    SECTION("e2e/an announce with max_nodes above 4 never enters (the adapter's decode drops it)");
    {
        SF f;
        f.W().GhostHost(2, "Big", kTdTidLow, 5);
        f.W().GhostHost(3, "Huge", kTdTidLow, 17);
        DoScan(f, 0);
        const std::vector<Mac> o = Order(f.L());
        CHECK_EQ(o.size(), 1);
    }
}

}  // namespace

// =====================================================================================================================
// 5b. More ways to break it: exact-size buffers, a faulty engine, malformed frames through the lobby, a clock that goes
// back, the game's own aging of the entries, a poll storm.

namespace {

void T_extra_buffers() {
    SECTION("extra/exact-size buffers handed to the lobby");
    SF f(100000, false);
    u16* nm = new u16[kNameUnits];
    for (u32 i = 0; i < kNameUnits; ++i) nm[i] = static_cast<u16>(0x41 + i);
    CHECK_EQ(f.L().ClientInitialize(nm, 16), 0);
    CHECK_EQ(f.L().ServerInitialize(3, 0, nm), 0);
    delete[] nm;
    f.L().ServerFinalize();
    DoScan(f, 0);
    DoJoinTo5(f);
    CHECK_EQ(f.P().last_connect_id.name[9], 0x41 + 9);
    CHECK_EQ(f.P().last_connect_id.name[0], 0x41);
    for (u32 cap = 0; cap <= 4; ++cap) {
        u16* ids = new u16[cap];
        u32 total = 99;
        CHECK_EQ(f.L().ClientConnectingNodes(ids, cap, &total), 0);
        CHECK_EQ(total, 2);
        if (cap >= 1) CHECK_EQ(ids[0], 1);
        if (cap >= 2) CHECK_EQ(ids[1], 2);
        delete[] ids;
    }
    u8* pp = new u8[9];
    CHECK_EQ(f.L().ClientPassphrase(pp), 0);
    delete[] pp;
    NodeEntry* ne = new NodeEntry;
    CHECK_EQ(f.L().ClientNodeInfo(2, ne), 0);
    CHECK_EQ(f.L().ClientNodeInfo(5, ne), kNoData);
    delete ne;
    GroupView* gv = new GroupView;
    CHECK_EQ(f.L().ClientServerInfo(M(1), gv), 0);
    delete gv;
    u32* res = new u32;
    CHECK(f.L().ClientJoinBegin(M(1), res) == JoinStep::Failed);  // state 5: kInvalidState, written
    CHECK_EQ(*res, kInvalidState);
    delete res;
    // the creator side
    CF c;
    c.L().ServerInitialize(3, 0, nullptr);
    c.L().ServerStartAccepting(false, 6);
    for (int n = 2; n <= 4; ++n) c.Cli(n);
    c.Poll(20);
    for (u32 cap = 0; cap <= 4; ++cap) {
        u16* ids = new u16[cap];
        u32 total = 99;
        CHECK_EQ(c.L().ServerConnectingClients(ids, cap, &total), 0);
        CHECK_EQ(total, 3);
        for (u32 i = 0; i < cap && i < 3; ++i) CHECK_EQ(ids[i], 2 + i);
        delete[] ids;
    }
    u32* w = new u32;
    CHECK_EQ(c.L().ServerClientState(3, w), 0);
    delete w;
    NodeEntry* e = new NodeEntry;
    CHECK_EQ(c.L().ServerClientInfo(1, e), kNoData);
    CHECK_EQ(c.L().ServerClientInfo(3, e), 0);
    delete e;
}

void T_extra_faulty_engine() {
    SECTION("extra/an engine that lists ids outside 1..4 in Members()");
    {
        CF f;
        CReach(f, C2);
        Port& p = f.P();
        p.inject_members[0] = 7;
        p.inject_members[1] = 0;
        p.inject_n = 2;
        const u32 k0 = p.Calls(kKick);
        for (int i = 0; i < 50; ++i) f.Poll(20);  // 1 s
        const u32 kicks = p.Calls(kKick) - k0;
        CHECK(kicks >= 1 && kicks <= 6);  // DESIGN sez. 16.3: the id 7 is kicked, every 200 ms at most; the id 0 never
        for (size_t i = static_cast<size_t>(k0); i < p.kicks.size(); ++i) CHECK_EQ(p.kicks[i], 7);
        u16 ids[4];
        u32 total = 0;
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK_EQ(total, 1);
        CHECK_EQ(ids[0], 2);
        CHECK_EQ(f.L().ServerState(), 2);
        CHECK_EQ(p.bad_kick, 0);
        p.inject_n = 0;
    }
    SECTION("extra/Members() claims more nodes than the array can hold");
    {
        CF f;
        CReach(f, C2);
        f.P().lie_extra = 9;
        for (int i = 0; i < 20; ++i) f.Poll(20);
        u16 ids[4];
        u32 total = 0;
        CHECK_EQ(f.L().ServerConnectingClients(ids, 4, &total), 0);
        CHECK(total <= kMaxClients);
        SF s;
        Reach(s, S5);
        s.P().lie_extra = 9;
        for (int i = 0; i < 40; ++i) s.Poll(300);
        total = 0;
        s.L().ClientConnectingNodes(ids, 4, &total);
        CHECK(total <= kMaxPlayers);
        f.P().bad_kick = 0;
        s.P().bad_kick = 0;
    }
    SECTION("extra/heard group with a signal above 3");
    {
        SF f;
        f.W().use_raw = true;
        HeardGroup g;
        std::memset(&g, 0, sizeof g);
        g.host = M(0x70);
        g.total_nodes = 2;
        g.signal = 200;
        g.announce = Ann(kTdTidLow, "Raw", 4, 1);
        f.W().raw.push_back(g);
        f.L().ClientStartScan(0);
        f.Poll(50);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(0x70), &v), 0);
        CHECK(v.group && v.group->signal <= 3);
    }
}

// Every (type, length byte, size) near the valid frames, sent by the right node, through the lobby.
void T_extra_frames() {
    SECTION("extra/malformed frames to a searcher in 9");
    {
        SF f;
        Reach(f, S9);
        u32 sent_replies0 = f.P().Sent(2) + f.P().Sent(4);
        u32 valid = 0, total = 0;
        for (int ty = 0; ty < 8; ++ty) {
            for (int len = 0; len < 256; len += (len < 14 ? 1 : 41)) {
                for (u32 sz : {0u, 1u, 2u, 3u, 10u, 11u, 12u, 16u}) {
                    Msg m;
                    std::memset(&m, 0x5A, sizeof m);
                    m.from = 1;
                    m.len = static_cast<u8>(sz);
                    m.data[0] = static_cast<u8>(ty);
                    m.data[1] = static_cast<u8>(len);
                    const bool ok = (ty == 1 && len == 0 && sz == 2) || (ty == 3 && len == 9 && sz == 11);
                    const u32 bad0 = f.L().bad_messages();
                    f.P().inbox.push_back(m);
                    f.L().Poll(true);
                    const u32 bad1 = f.L().bad_messages();
                    ++total;
                    if (ok) ++valid;
                    if (bad1 - bad0 != (ok ? 0u : 1u)) {
                        CHECK_EQ(bad1 - bad0, ok ? 0u : 1u);
                        std::printf("    frame type=%d len=%d size=%u\n", ty, len, sz);
                    }
                }
            }
        }
        CHECK_EQ(f.L().client_state(), 9);
        u8 pp[9];
        f.L().ClientPassphrase(pp);
        CHECK(std::memcmp(pp, kPass1, 9) == 0);
        CHECK_EQ(f.P().Sent(2) + f.P().Sent(4) - sent_replies0, valid);  // one answer per valid frame, none for the rest
        CHECK_EQ(f.P().inbox.size(), 0);
    }
    SECTION("extra/malformed frames to a creator with the passphrase sent");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        f.L().ServerStartDistribution();
        f.Ack(2, MsgType::StartAck);
        f.Ack(3, MsgType::StartAck);
        f.Poll(20);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
        for (int ty = 0; ty < 8; ++ty) {
            for (int len = 0; len < 256; len += (len < 14 ? 1 : 41)) {
                for (u32 sz : {0u, 1u, 2u, 3u, 10u, 11u, 12u, 16u}) {
                    Msg m;
                    std::memset(&m, 0x5A, sizeof m);
                    m.from = 2;  // a tracked client; node 3 never answers GO, so the phase stays open
                    m.len = static_cast<u8>(sz);
                    m.data[0] = static_cast<u8>(ty);
                    m.data[1] = static_cast<u8>(len);
                    const bool ok = (ty == 2 || ty == 4) && len == 0 && sz == 2;
                    const u32 bad0 = f.L().bad_messages();
                    f.P().inbox.push_back(m);
                    f.L().Poll(true);
                    if (f.L().bad_messages() - bad0 != (ok ? 0u : 1u)) {
                        CHECK_EQ(f.L().bad_messages() - bad0, ok ? 0u : 1u);
                        std::printf("    frame type=%d len=%d size=%u\n", ty, len, sz);
                    }
                }
            }
        }
        CHECK_EQ(f.L().ServerState(), 8);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(9, 1, 2));
        CHECK_EQ(f.L().ServerClientState(3, &w), 0);
        CHECK_EQ(w, Word(7, 1, 3));
    }
    SECTION("extra/the bytes of the answers");
    {
        SF f;
        Reach(f, S6);
        Tx(f.W(), f.h, 1, 2, MsgType::Go, kPass1);
        f.Poll(20);
        u32 sa = 0, ga = 0;
        for (auto& g : f.h->ghost_rx) {
            CHECK_EQ(g.len, 2);
            CHECK_EQ(g.data[1], 0);
            CHECK_EQ(g.from, 2);
            CHECK_EQ(g.to, 1);
            if (g.data[0] == 2) ++sa;
            if (g.data[0] == 4) ++ga;
        }
        CHECK_EQ(sa, 1);
        CHECK_EQ(ga, 1);
    }
}

// ---- the clock goes back ----------------------------------------------------------------------------------------------

void T_extra_clock_back() {
    SECTION("extra/clock back: scan entry");
    {
        SF f;
        DoScan(f, 0);
        const u64 seen = f.W().now;
        f.W().Hide(M(1), ~0ull);
        f.W().now = seen - 5000;
        f.L().Poll(true);
        GroupView v{};
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);  // a negative age counts as 0
        f.W().now = seen + 1499;
        f.L().Poll(true);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), 0);
        f.W().now = seen + 1500;
        f.L().Poll(true);
        CHECK_EQ(f.L().ClientServerInfo(M(1), &v), kNoData);
    }
    SECTION("extra/clock back: join");
    {
        SF f;
        DoScan(f, 0);
        f.L().ClientStopScan();
        FillHost(f);
        u32 res = 0, out = 0;
        const u64 t0 = f.W().now;
        CHECK(f.L().ClientJoinBegin(M(1), &res) == JoinStep::Pending);
        f.W().now = t0 + 1000;
        f.L().Poll(true);
        f.W().now = t0 - 4000;
        for (int i = 0; i < 40; ++i) {
            f.Poll(100);  // up to t0
            CHECK(!f.L().ClientJoinPoll(&out));
            CHECK_EQ(f.L().client_state(), 3);
        }
        f.W().now = t0 + 2999;
        CHECK(!f.L().ClientJoinPoll(&out));
        f.W().now = t0 + 3000;
        CHECK(f.L().ClientJoinPoll(&out));
        CHECK_EQ(out, kTimeout);
        CHECK_EQ(f.P().Calls(kDisconnect), 1);
    }
    SECTION("extra/clock back: host gone");
    {
        SF f;
        Reach(f, S5);
        f.W().Flap(f.h, 1, 100000000u);
        f.Poll(0);
        const u64 t0 = f.W().now;
        f.W().now = t0 - 5000;
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 5);
        f.W().now = t0 + 1999;
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 5);
        f.W().now = t0 + 2000;
        f.L().Poll(true);
        CHECK_EQ(f.L().client_state(), 0x80);
    }
    SECTION("extra/clock back: START grace and the rate limit");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Poll(20);
        const u64 t0 = f.W().now;
        f.L().ServerStartDistribution();
        f.W().now = t0 - 5000;
        f.L().Poll(true);
        CHECK_EQ(f.P().Calls(kKick), 0);
        f.W().now = t0 + 1499;
        f.L().Poll(true);
        CHECK_EQ(f.P().Calls(kKick), 0);
        CHECK_EQ(f.L().ServerState(), 6);
        f.W().now = t0 + 1500;
        f.L().Poll(true);
        CHECK_EQ(f.P().Calls(kKick), 1);
        // Pump after a clock that went back runs at once
        CF g;
        CReach(g, C2);
        g.L().Poll(true);
        const u32 m0 = g.P().Calls(kMembers);
        g.W().now -= 5000;
        g.L().Pump();
        CHECK(g.P().Calls(kMembers) > m0);
    }
}

// ---- the game ages the entries itself ------------------------------------------------------------------------------------

void T_extra_aging() {
    SECTION("extra/the game's own aging (5000 ms) never finds an old entry while scanning");
    SF f;
    f.W().GhostHost(2, "B");
    DoScan(f, 0);
    u32 deleted = 0, seen_max_age = 0;
    for (int i = 0; i < 1500; ++i) {  // 24 s at 16 ms
        f.W().now += 16;
        f.L().Pump();
        if (i == 150) f.W().Hide(M(2), ~0ull);  // B vanishes at ~2.4 s
        if (i == 700) f.W().hidden.clear();     // and comes back at ~11 s
        bool restart = true;
        for (int k = 0; k < 8; ++k) {
            GroupView v{};
            if (f.L().ClientTitleInfoInOrder(restart, &v) == kNoData) break;
            restart = false;
            GroupView sv{};
            if (f.L().ClientServerInfo(v.group->host, &sv) == 0) {
                const u64 age_ms = (f.W().Ticks() - sv.tick) / kTpm;
                if (age_ms > seen_max_age) seen_max_age = static_cast<u32>(age_ms);
                if (age_ms >= 5000) {
                    ++deleted;
                    f.L().ClientDeleteScanInfo(v.group->host);
                }
            }
        }
    }
    CHECK_EQ(deleted, 0);
    CHECK(seen_max_age < DlpLobby::kGroupTtlMs + 100);
    std::printf("  NOTE: oldest entry the game ever saw while scanning: %u ms\n", seen_max_age);
}

void T_extra_storm() {
    SECTION("extra/a storm of Poll(true) at the same instant sends nothing new");
    {
        CF f;
        CReach(f, C6);
        f.Ack(2, MsgType::GoAck);  // (a stray, wrong-phase message just to have something to chew on)
        const u32 s0 = f.Sent(1, 2);
        for (int i = 0; i < 5000; ++i) f.L().Poll(true);
        CHECK_EQ(f.Sent(1, 2), s0);
        CHECK_EQ(f.P().Calls(kKick), 0);
    }
    {
        SF f;
        Reach(f, S5);
        const u32 w0 = f.P().Writes();
        for (int i = 0; i < 5000; ++i) f.L().Poll(true);
        CHECK_EQ(f.P().Writes(), w0);
        CHECK_EQ(f.L().client_state(), 5);
    }
    SECTION("extra/CReach also with manual acceptance");
    for (int st : {C2, C6, C8, C9}) {
        CF f;
        CReach(f, st, true);
        CHECK_EQ(f.L().server_state(), st);
    }
}

void T_extra_disconnect_completion() {
    SECTION("extra/DisconnectClient of the last awaited client completes the phase");
    {
        CF f;
        CReach(f, C6);
        CHECK_EQ(f.L().ServerDisconnectClient(2), 0);
        CHECK_EQ(f.L().ServerState(), 8);
        CHECK_EQ(f.P().Calls(kKick), 1);
    }
    {
        CF f;
        CReach(f, C8);
        CHECK_EQ(f.L().ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(f.L().ServerState(), 8);
        CHECK_EQ(f.L().ServerDisconnectClient(2), 0);
        CHECK_EQ(f.L().ServerState(), 9);
        CHECK_EQ(f.P().Calls(kHostClose), 1);
    }
    {
        // manual: a waiting client disconnected by the game; the accepted one carries on
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(true, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        CHECK_EQ(f.L().ServerAcceptClient(2), 0);
        CHECK_EQ(f.L().ServerDisconnectClient(3), 0);
        u32 w;
        CHECK_EQ(f.L().ServerClientState(2, &w), 0);
        CHECK_EQ(w, Word(5, 1, 2));
        CHECK_EQ(f.L().ServerStartDistribution(), 0);
        CHECK_EQ(f.Sent(1, 2), 1);
        CHECK_EQ(f.Sent(1, 3), 0);
    }
}

void T_extra_creator_timers() {
    SECTION("extra/creator: a flap of its own node 1 shorter than 2000 ms resets the timer");
    {
        CF f;
        CReach(f, C2);
        for (int k = 0; k < 5; ++k) {
            f.W().Flap(f.N(), 1, 1500);
            f.Poll(0);
            f.Poll(1400);
            CHECK_EQ(f.L().ServerState(), 2);
            f.Poll(200);  // back
            f.Poll(100);
            CHECK_EQ(f.L().ServerState(), 2);
        }
        CHECK_EQ(f.P().Calls(kHostClose), 0);
    }
    SECTION("extra/creator: Kick is repeated every kResendMs while the node lingers in the engine");
    {
        CF f;
        f.W().kick_linger_ms = 1000;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Poll(20);
        CHECK_EQ(f.L().ServerDisconnectClient(2), 0);
        for (int i = 0; i < 45; ++i) f.Poll(20);  // 900 ms
        const u32 k = f.P().Calls(kKick);
        CHECK(k >= 4 && k <= 6);
    }
    SECTION("extra/StartDistribution and SendPassphrase poll first: a client that just left gets nothing");
    {
        CF f;
        f.L().ServerInitialize(3, 0, nullptr);
        f.L().ServerStartAccepting(false, 6);
        f.Cli(2);
        f.Cli(3);
        f.Poll(20);
        f.W().Remove(f.N(), 3);  // gone, and nobody has polled since
        const u32 s0 = f.P().Calls(kSend);
        CHECK_EQ(f.L().ServerStartDistribution(), 0);
        CHECK_EQ(f.P().Calls(kSend) - s0, 1);
        CF g;
        g.L().ServerInitialize(3, 0, nullptr);
        g.L().ServerStartAccepting(false, 6);
        g.Cli(2);
        g.Cli(3);
        g.Poll(20);
        g.L().ServerStartDistribution();
        g.Ack(2, MsgType::StartAck);
        g.Ack(3, MsgType::StartAck);
        g.Poll(20);
        CHECK_EQ(g.L().ServerState(), 8);
        g.W().Remove(g.N(), 3);
        const u32 s1 = g.P().Calls(kSend);
        CHECK_EQ(g.L().ServerSendPassphrase(kPass1), 0);
        CHECK_EQ(g.P().Calls(kSend) - s1, 1);
    }
}

}  // namespace

// =====================================================================================================================
// 6. Rate of the work, threads, determinism, and the randomised invariant fuzz.

namespace {

void T_rate() {
    SECTION("rate/Pump runs at most once per kPollMs");
    {
        CF f;
        CReach(f, C2);
        f.L().Poll(true);
        Port& p = f.P();
        const u32 m0 = p.Calls(kMembers), r0 = p.Calls(kReceive);
        f.W().now += 19;
        f.L().Pump();
        CHECK_EQ(p.Calls(kMembers), m0);
        CHECK_EQ(p.Calls(kReceive), r0);
        f.W().now += 1;
        f.L().Pump();
        CHECK(p.Calls(kMembers) > m0);
        const u32 m1 = p.Calls(kMembers);
        for (int i = 0; i < 10; ++i) f.L().Pump();  // same instant
        CHECK_EQ(p.Calls(kMembers), m1);
        // Poll(true) always runs
        f.L().Poll(true);
        CHECK(p.Calls(kMembers) > m1);
    }
    SECTION("rate/an idle lobby never touches the net");
    {
        Rig r;
        Con& c = r.Add();
        for (int i = 0; i < 100; ++i) {
            r.w.now += 50;
            c.lobby.Pump();
        }
        CHECK_EQ(c.port.Total(), 0);
        c.lobby.ClientInitialize(nullptr, 16);
        c.lobby.ServerInitialize(3, 0, nullptr);
        for (int i = 0; i < 100; ++i) {
            r.w.now += 50;
            c.lobby.Pump();
            c.lobby.Poll(true);
        }
        CHECK_EQ(c.port.Total(), 0);
        CHECK(!c.lobby.active());
    }
    SECTION("rate/60 Hz for 10 s: at most 50 runs per second, quiet states send nothing");
    {
        const int sts[] = {S2, S5, S6, S9};
        for (int st : sts) {
            SECTIONF("rate/searcher %d at 60 Hz", st);
            SF f;
            Reach(f, st);
            Port& p = f.P();
            const u32 m0 = p.Calls(kMembers), r0 = p.Calls(kReceive), s0 = p.Calls(kScanRead), w0 = p.Writes();
            for (int i = 0; i < 625; ++i) {  // 10 s at 16 ms
                f.W().now += 16;
                f.L().Pump();
                f.L().ClientStatusWord();
            }
            CHECK(p.Calls(kMembers) - m0 <= 505);
            CHECK(p.Calls(kReceive) - r0 <= 505);
            CHECK(p.Calls(kScanRead) - s0 <= kMaxGroups * 505u);
            CHECK_EQ(p.Writes(), w0);
            CHECK_EQ(f.L().client_state(), st == S6 ? 6 : st);
        }
        const int cst[] = {C2, C8};
        for (int st : cst) {
            SECTIONF("rate/creator %d at 60 Hz", st);
            CF f;
            CReach(f, st);
            Port& p = f.P();
            const u32 m0 = p.Calls(kMembers), r0 = p.Calls(kReceive), w0 = p.Writes();
            for (int i = 0; i < 625; ++i) {
                f.W().now += 16;
                f.L().Pump();
                f.L().ServerState();
            }
            CHECK(p.Calls(kMembers) - m0 <= 505);
            CHECK(p.Calls(kReceive) - r0 <= 505);
            CHECK_EQ(p.Writes(), w0);
            CHECK_EQ(f.L().server_state(), st);
        }
    }
    SECTION("rate/ReadNode at most 4 per poll, ScanRead at most 4 per poll");
    {
        E2E e(3);
        for (int i = 0; i < 100; ++i) {
            e.r.w.now += 100;
            for (auto& x : e.r.c) {
                const u32 rn0 = x->port.Calls(kReadNode), sr0 = x->port.Calls(kScanRead);
                x->lobby.Poll(true);
                CHECK(x->port.Calls(kReadNode) - rn0 <= 4);
                CHECK(x->port.Calls(kScanRead) - sr0 <= 4);
            }
        }
        SF f;
        for (int i = 2; i <= 7; ++i) f.W().GhostHost(static_cast<u8>(i), "H");
        f.L().ClientStartScan(0);
        for (int i = 0; i < 50; ++i) {
            const u32 s0 = f.P().Calls(kScanRead);
            f.Poll(30);
            CHECK(f.P().Calls(kScanRead) - s0 <= 4);
        }
    }
}

// ---- the external-poll mode (phase 3: the hub of azl/dlp_hub.hpp puts the lobby in it) ------------------------------------

// SetExternalPoll(true): the network work is the network thread's. Poll / Pump return at once - no call into LobbyNet, not even the
// clock - unless the caller said it is that thread (SetOnNet(true)); everything else (the tables, the states, the commands) is as in
// phase 1. With the mode off (the default) the lobby polls where it is called and SetOnNet means nothing.
void T_external_poll() {
    SECTION("external/off (the default): Poll(true) and Pump reach the net where they are called, SetOnNet means nothing");
    {
        CF f;
        CReach(f, C2);
        Port& p = f.P();
        for (int on = 0; on < 2; ++on) {
            f.L().SetOnNet(on != 0);
            const u32 m0 = p.Calls(kMembers), r0 = p.Calls(kReceive);
            f.Adv(30);
            f.L().Pump();
            CHECK(p.Calls(kMembers) > m0);
            CHECK(p.Calls(kReceive) > r0);
            const u32 m1 = p.Calls(kMembers);
            f.L().Poll(true);
            CHECK(p.Calls(kMembers) > m1);
        }
        SF s;
        Reach(s, S5);
        const u32 m0 = s.P().Calls(kMembers);
        s.L().SetOnNet(true);
        s.L().SetOnNet(false);
        s.L().Poll(true);
        CHECK(s.P().Calls(kMembers) > m0);
    }
    SECTION("external/on, not the network thread: the creator at 2, 6, 8, 9 reaches nothing of the net and moves by nothing");
    {
        const int cst[] = {C2, C6, C8, C9};
        for (const int st : cst) {
            SECTIONF("external/creator %d off the network thread", st);
            CF f;
            CReach(f, st);
            f.L().SetExternalPoll(true);
            Port& p = f.P();
            const u32 total0 = p.Total();
            const u8 state0 = f.L().server_state();
            CHECK_EQ(state0, st);
            for (int i = 0; i < 40; ++i) {  // 2.4 s of the game's calls: more than the grace of START (1500 ms) and of GO (2000 ms)
                f.Adv(60);
                f.L().Pump();
                f.L().Poll(true);
                u16 ids[4];
                u32 total = 0, word = 0;
                f.L().ServerConnectingClients(ids, 4, &total);
                f.L().ServerClientState(2, &word);
                f.L().ServerAcceptClient(2);  // (a no-op in these states: the client is accepted, or gone)
                f.L().ServerState();
                NodeEntry ne;
                f.L().ServerClientInfo(1, &ne);  // the host's own record: no ReadNode
            }
            CHECK_EQ(p.Total(), total0);
            CHECK_EQ(f.L().server_state(), state0);  // (nothing polled: the grace of the distribution did not run out)
            // the network thread does the work: the same pump, OnNet on
            f.L().SetOnNet(true);
            f.L().Pump();
            f.L().SetOnNet(false);
            if (st == C2 || st == C6 || st == C8) {
                CHECK(p.Total() > total0);  // Members / Receive at least
            } else {
                CHECK_EQ(p.Total(), total0);  // (9: the network is closed and the table dropped, nothing to read)
            }
            if (st == C6) {
                CHECK_EQ(f.L().server_state(), 8);  // the grace ran out in that one pump: the client without START_ACK was thrown out
            }
        }
    }
    SECTION("external/on, not the network thread: the searcher at 2, 5, 6, 9 and a join in progress");
    {
        const int sts[] = {S2, S5, S6, S9};
        for (const int st : sts) {
            SECTIONF("external/searcher %d off the network thread", st);
            SF f;
            Reach(f, st);
            f.L().SetExternalPoll(true);
            Port& p = f.P();
            const u32 total0 = p.Total();
            const u8 state0 = f.L().client_state();
            for (int i = 0; i < 40; ++i) {
                f.Adv(60);
                f.L().Pump();
                f.L().Poll(true);
                GroupView g;
                Mac any = M(1);
                u16 ids[4];
                u32 total = 0;
                NodeEntry ne;
                u8 pass[kPassphraseSize];
                f.L().ClientTitleInfo(any, &g);
                f.L().ClientServerInfo(any, &g);
                f.L().ClientTitleInfoInOrder(i == 0, &g);
                f.L().ClientConnectingNodes(ids, 4, &total);
                f.L().ClientNodeInfo(1, &ne);
                f.L().ClientPassphrase(pass);
                f.L().ClientStatusWord();
            }
            CHECK_EQ(p.Total(), total0);
            CHECK_EQ(f.L().client_state(), state0);
            f.L().SetOnNet(true);
            f.L().Pump();
            f.L().SetOnNet(false);
            CHECK(p.Total() > total0);  // the network thread's pump: ScanRead / Members / Receive
        }
        SECTION("external/a join in progress does not time out off the network thread, and does on it");
        SF f;
        Reach(f, S3);
        f.L().SetExternalPoll(true);
        Port& p = f.P();
        const u32 total0 = p.Total();
        u32 out = 0xDEAD;
        for (int i = 0; i < 100; ++i) {  // 10 s: three times the join's time
            f.Adv(100);
            f.L().Pump();
            CHECK(!f.L().ClientJoinPoll(&out));  // (its Poll(true) is the network thread's too)
        }
        CHECK(f.L().join_active());
        CHECK_EQ(f.L().client_state(), 3);
        CHECK_EQ(p.Total(), total0);
        f.L().SetOnNet(true);
        CHECK(f.L().ClientJoinPoll(&out));  // the network thread asks: the deadline passed long ago
        f.L().SetOnNet(false);
        CHECK_EQ(out, kTimeout);
        CHECK(!f.L().join_active());
        CHECK_EQ(f.L().client_state(), 1);
        CHECK(p.Calls(kDisconnect) >= 1);
    }
    SECTION("external/the mode is not a state of a role: Initialize, Finalize, StopSession, OnGameUdsStarting leave it as it was");
    {
        SF f(100000, false);  // (not initialised yet: the mode is set first, as the hub does it)
        f.L().SetExternalPoll(true);
        Port& p = f.P();
        // the lobby holds the net; a pump and a Poll(true) off the network thread reach nothing of it
        auto quiet = [&](const char* where) {
            SECTIONF("external/quiet %s", where);
            const u32 total0 = p.Total();
            f.Adv(60);
            f.L().Pump();
            f.L().Poll(true);
            CHECK_EQ(p.Total(), total0);
        };
        f.L().SetOnNet(true);
        f.L().ClientInitialize(nullptr, 16);
        Reach(f, S5);  // (its polls are the network thread's: OnNet is up)
        f.L().SetOnNet(false);
        quiet("in a session");
        f.L().ClientStopSession();  // the full teardown of the searcher
        CHECK_EQ(f.L().client_state(), 1);
        f.L().SetOnNet(true);
        DoScan(f);
        f.L().SetOnNet(false);
        quiet("scanning after StopSession");
        f.L().ClientFinalize();
        f.L().ClientInitialize(nullptr, 16);
        CHECK_EQ(f.L().ClientStartScan(kTdFilter), 0);
        quiet("scanning after Finalize and Initialize");
        f.L().SetOnNet(true);
        f.L().Poll(true);
        f.L().SetOnNet(false);
        CHECK(f.L().OnGameUdsStarting());  // scanning: released, the state is 1
        CHECK_EQ(f.L().ClientStartScan(kTdFilter), 0);
        quiet("scanning after OnGameUdsStarting");
        // the creator's side: its teardowns do not clear it either
        f.L().ClientFinalize();
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
        f.L().SetOnNet(true);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        f.L().SetOnNet(false);
        quiet("creator accepting");
        f.L().ServerFinalize();
        CHECK_EQ(f.L().ServerInitialize(3, 0, nullptr), 0);
        f.L().SetOnNet(true);
        CHECK_EQ(f.L().ServerStartAccepting(false, 6), 0);
        f.L().SetOnNet(false);
        quiet("creator accepting again");
    }
    SECTION("external/the commands that act keep acting with OnNet on, and Poll(true) first");
    {
        CF f;
        CReach(f, C2, true);  // manual acceptance: the client waits to be accepted
        f.L().SetExternalPoll(true);
        f.L().SetOnNet(true);
        f.Cli(3, "Cleo");  // a second client comes in: only a poll tracks it
        f.Adv(30);
        u32 word = 0;
        CHECK_EQ(f.L().ServerClientState(3, &word), kNoData);  // not tracked yet
        CHECK_EQ(f.L().ServerAcceptClient(3), 0);              // Poll(true) first (OnNet): tracked, then accepted
        CHECK_EQ(f.L().ServerClientState(3, &word), 0);
        CHECK_EQ(word, Word(5, 1, 3));
        f.L().SetOnNet(false);
        f.Cli(4, "Dan");
        f.Adv(30);
        CHECK_EQ(f.L().ServerAcceptClient(4), kNoData);  // off the network thread: no poll, so not tracked, so nobody to accept
        CHECK_EQ(f.L().ServerClientState(4, &word), kNoData);
        f.L().SetOnNet(true);
        f.L().Pump();
        f.L().SetOnNet(false);
        CHECK_EQ(f.L().ServerClientState(4, &word), 0);  // the network thread's pump tracked it (still waiting: manual)
        CHECK_EQ(word, Word(0x40, 0, 4));
        CHECK_EQ(f.L().ServerAcceptClient(4), 0);        // and now it needs no poll to be accepted
    }
}

// ---- the external-poll mode over a GO (mutation round of phase 3, label "mutants") ------------------------------------------
// The searcher's GO sets kF2FinalRoster (the roster is read once more at the end of that poll, then it freezes) in the byte that also holds the
// hub's two bits. A GO that assigned the byte, or a final-roster step that cleared it, would drop kF2External: the NEXT role of the lobby would
// poll on a game thread again. (ClientReset's line has its own check above; these two lines had none.)
void T_external_go() {
    SECTION("external/a GO and the frozen roster leave the mode as it was: the next scan is still quiet off the network thread");
    {
        SF f(100000, false);
        f.L().SetExternalPoll(true);
        Port& p = f.P();
        f.L().SetOnNet(true);
        f.L().ClientInitialize(nullptr, 16);
        Reach(f, S9);  // (the GO arrived with the network thread's pump: state 9, the roster frozen, the network closed)
        f.L().SetOnNet(false);
        CHECK_EQ(f.L().client_state(), 9);
        f.L().SetOnNet(true);
        f.L().ClientStopSession();
        DoScan(f);
        f.L().SetOnNet(false);
        CHECK_EQ(f.L().client_state(), 2);
        const u32 total0 = p.Total();
        for (int i = 0; i < 40; ++i) {
            f.Adv(60);
            f.L().Pump();
            f.L().Poll(true);
            GroupView g;
            f.L().ClientTitleInfoInOrder(i == 0, &g);
        }
        CHECK_EQ(p.Total(), total0);  // quiet: the mode is still on
        f.L().SetOnNet(true);
        f.L().Pump();
        f.L().SetOnNet(false);
        CHECK(p.Total() > total0);  // and the network thread's pump still works
    }
    SECTION("external/a GO at 6 (START seen) and the same");
    {
        SF f(100000, false);
        f.L().SetExternalPoll(true);
        Port& p = f.P();
        f.L().SetOnNet(true);
        f.L().ClientInitialize(nullptr, 16);
        DoScan(f);
        DoJoinTo5(f);
        Tx(f.W(), f.h, 1, f.P().node, MsgType::Start);
        f.Poll(20);
        Tx(f.W(), f.h, 1, f.P().node, MsgType::Go, kPass1);
        f.Poll(20);
        CHECK_EQ(f.L().client_state(), 9);
        f.L().ClientFinalize();
        f.L().ClientInitialize(nullptr, 16);
        CHECK_EQ(f.L().ClientStartScan(kTdFilter), 0);
        f.L().SetOnNet(false);
        const u32 total0 = p.Total();
        for (int i = 0; i < 20; ++i) {
            f.Adv(60);
            f.L().Pump();
            f.L().Poll(true);
        }
        CHECK_EQ(p.Total(), total0);
    }
}

// ---- real threads ---------------------------------------------------------------------------------------------------

void T_threads() {
    SECTION("threads/join poll in one thread, OnGameUdsStarting and Pump in another (one mutex)");
    for (int iter = 0; iter < 60; ++iter) {
        SF f(100000 + static_cast<u64>(iter));
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.P().connect_delay = 60;
        u32 r0 = 0;
        CHECK(f.L().ClientJoinBegin(M(1), &r0) == JoinStep::Pending);
        std::mutex mu;
        bool done = false;
        u32 res_val = 0xBEEF;
        std::thread a([&] {
            for (int i = 0; i < 4000; ++i) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    f.W().now += 3;
                    u32 out = 0xBEEF;
                    if (f.L().ClientJoinPoll(&out)) {
                        done = true;
                        res_val = out;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(40));
            }
        });
        std::thread b([&] {
            for (int i = 0; i < 150; ++i) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    f.W().now += 5;
                    if (i == (iter * 7) % 120) f.L().OnGameUdsStarting();
                    f.L().Pump();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(70));
            }
        });
        a.join();
        b.join();
        CHECK(done);
        CHECK(res_val == 0 || res_val == kInvalidState);
        CHECK_EQ(f.W().overlap, 0);
        const int s = f.L().client_state();
        CHECK(s == 1 || s == 5);
        CHECK_EQ(f.P().bad_send + f.P().connect_while_busy, 0);
        if (s == 1) CHECK(!f.L().active());
    }
    SECTION("threads/two lobbies of one room driven by two threads under one mutex");
    for (int iter = 0; iter < 10; ++iter) {
        E2E e(1);
        std::mutex mu;
        std::atomic<bool> stop{false};
        HostGame g;
        std::thread host([&] {
            std::lock_guard<std::mutex> l0(mu);
            e.h->lobby.ServerStartDistribution();
        });
        host.join();
        std::thread t1([&] {
            for (int i = 0; i < 400 && !stop; ++i) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    e.r.w.now += 16;
                    e.r.PumpAll();
                    if (HostFrame(e, g)) stop = true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(30));
            }
        });
        std::thread t2([&] {
            for (int i = 0; i < 400 && !stop; ++i) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    e.c[0]->lobby.ClientStatusWord();
                    e.c[0]->lobby.Pump();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(30));
            }
        });
        t1.join();
        t2.join();
        CHECK(stop);
        CHECK_EQ(e.c[0]->lobby.client_state(), 9);
        CHECK_EQ(e.r.w.overlap, 0);
    }
}

// ---- determinism ----------------------------------------------------------------------------------------------------

struct DetOut {
    u64 trace;
    u32 states;
    u32 counts;
    u32 bad;
};
DetOut DetRun() {
    E2E e(3);
    e.r.w.Drop(2, 3, 1, 2);
    e.r.w.Drop(3, 0, 0, 1);
    e.r.w.dup_extra[4] = 1;
    HostGame g;
    e.h->lobby.ServerStartDistribution();
    for (int i = 0; i < 400; ++i) {
        Frame(e, 16 + (i % 3));
        HostFrame(e, g);
    }
    DetOut o;
    o.trace = e.r.w.trace;
    o.states = e.h->lobby.server_state();
    for (Con* x : e.c) o.states = o.states * 31 + (x->lobby.ClientStatusWord() >> 24);
    o.counts = 0;
    o.bad = 0;
    for (auto& x : e.r.c) {
        for (int m = 0; m < kMethCount; ++m) o.counts = o.counts * 131 + x->port.calls[m];
        o.bad += x->lobby.bad_messages();
    }
    return o;
}

void T_determinism() {
    SECTION("determinism");
    const DetOut a = DetRun();
    const DetOut b = DetRun();
    CHECK_EQ(a.trace, b.trace);
    CHECK_EQ(a.states, b.states);
    CHECK_EQ(a.counts, b.counts);
    CHECK_EQ(a.bad, b.bad);
}

// ---- the invariant fuzz -----------------------------------------------------------------------------------------------

bool Known(u32 r) {
    return r == 0 || r == kInvalidState || r == kBusy || r == kNoData || r == kNotFound || r == kTimeout || r == kOutOfRange;
}

struct Fuzz {
    Rig r;
    Con* c[4];
    u32 s;
    int step = 0;
    u32 notes_open_close = 0;
    explicit Fuzz(u32 seed, u64 start) : r(start), s(seed * 2654435761u + 12345u) {
        for (int i = 0; i < 4; ++i) c[i] = &r.Add(i == 3 ? 0x0004000000183600ull : kTdTid);
        r.w.GhostHost(0x60, "GhostA", kTdTidLow, 4, 1);
        r.w.GhostHost(0x61, "GhostB", kTdTidLow | 1, 3, 1);
        r.w.GhostClient(r.w.ByMac(M(0x60)), 2, 0x6002, "gc");
    }
    u32 R() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    Net* AnyNet() {
        std::vector<Net*> v;
        for (auto& n : r.w.nets)
            if (n.open) v.push_back(&n);
        if (v.empty()) return nullptr;
        return v[R() % v.size()];
    }
    Mac AnyMac() {
        static const u8 ids[] = {0x60, 0x61, 0x10, 0x11, 0x12, 0x13, 0x99};
        return M(ids[R() % sizeof ids]);
    }
    void Inv(Con& k, int idx) {
        DlpLobby& L = k.lobby;
        Port& p = k.port;
        const u32 sw = L.ClientStatusWord();
        const u32 cs = sw >> 24;
        const u8 raw = L.client_state();
        CHECK(cs == 0 || cs == 1 || cs == 2 || cs == 5 || cs == 6 || cs == 9 || cs == 0x80);
        CHECK(raw == 0 || raw == 1 || raw == 2 || raw == 3 || raw == 5 || raw == 6 || raw == 9 || raw == 0x80);
        CHECK_EQ(cs, raw == 3 ? 1 : raw);
        const bool sess = raw == 5 || raw == 6 || raw == 9;
        CHECK_EQ((sw >> 16) & 0xFF, sess ? 1 : 0);
        if (sess) {
            CHECK((sw & 0xFFFF) >= 2 && (sw & 0xFFFF) <= 4);
        } else {
            CHECK_EQ(sw & 0xFFFF, 0);
        }
        const u32 ss = L.ServerState();
        CHECK(ss == 0 || ss == 1 || ss == 2 || ss == 6 || ss == 8 || ss == 9);
        CHECK_EQ(ss, L.server_state());
        u16 ids[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        u32 total = 99;
        CHECK_EQ(L.ClientConnectingNodes(ids, 4, &total), 0);
        if (sess) {
            CHECK(total <= kMaxPlayers);
        } else {
            CHECK_EQ(total, 0);
        }
        for (u32 i = 0; i < total && i < 4; ++i) {
            CHECK(ids[i] >= 1 && ids[i] <= 4);
            if (i) CHECK(ids[i] > ids[i - 1]);
        }
        u16 cid[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        total = 99;
        const u32 rc = L.ServerConnectingClients(cid, 4, &total);
        if (ss == 0) {
            CHECK_EQ(rc, kInvalidState);
        } else {
            CHECK_EQ(rc, 0);
            CHECK(total <= kMaxClients);
            if (ss == 9) CHECK_EQ(total, 0);
            for (u32 i = 0; i < total && i < 3; ++i) {
                CHECK(cid[i] >= 2 && cid[i] <= 4);
                if (i) CHECK(cid[i] > cid[i - 1]);
            }
        }
        CHECK(!(p.hosting && (p.scan_open || p.connected || p.connecting)));
        if (ss == 2 || ss == 6 || ss == 8) CHECK(p.hosting);
        if (raw == 2) CHECK(p.scan_open);
        if (raw == 3) CHECK(p.connecting || p.connected);
        if (!L.active()) CHECK(!p.hosting && !p.scan_open && !p.connecting && !p.connected);
        CHECK_EQ(p.bad_send, 0);
        CHECK_EQ(p.bad_kick, 0);
        CHECK_EQ(p.double_open, 0);
        CHECK_EQ(p.bad_scanread, 0);
        CHECK_EQ(p.bad_read, 0);
        CHECK_EQ(p.small_cap, 0);
        CHECK_EQ(p.connect_while_busy, 0);
        CHECK_EQ(p.scanread_closed, 0);
        (void)idx;
    }
    void Op() {
        Con& k = *c[R() % 4];
        DlpLobby& L = k.lobby;
        static const Nm names[3] = {N("Ann"), N("Bo"), N("Cy")};
        static const u64 filters[5] = {0, kTdFilter, 0x000400010010C105ull, 0x000400010018360ull, 0xFFFFFFFFFFFFFFFFull};
        static const u16 nodes[10] = {0, 1, 2, 3, 4, 5, 0xFF, 0x100, 0x102, 0xFFFF};
        static const u32 chans[8] = {0, 1, 6, 11, 2, 7, 255, 0x106};
        u32 res = 0;
        GroupView gv{};
        NodeEntry ne;
        std::memset(&ne, 0, sizeof ne);
        u16 ids[4];
        u32 total = 0;
        u8 pp[9];
        switch (R() % 28) {
            case 0: L.ClientInitialize((R() % 3) ? nullptr : names[R() % 3].u, R() % 20); break;
            case 1: L.ClientFinalize(); break;
            case 2: CHECK(Known(L.ClientStartScan(filters[R() % 5]))); break;
            case 3: L.ClientStopScan(); break;
            case 4:
                if (L.ClientJoinBegin(AnyMac(), &res) == JoinStep::Failed) CHECK(Known(res));
                break;
            case 5:
                if (L.ClientJoinPoll(&res)) CHECK(Known(res));
                break;
            case 6: L.ClientStopSession(); break;
            case 7: CHECK(Known(L.ClientTitleInfo(AnyMac(), &gv))); break;
            case 8: CHECK(Known(L.ClientServerInfo(AnyMac(), &gv))); break;
            case 9: {
                const u32 rr = L.ClientTitleInfoInOrder((R() & 1) != 0, &gv);
                CHECK(rr == 0 || rr == kNoData);
                if (rr == 0) CHECK(gv.group != nullptr);
                break;
            }
            case 10: CHECK(L.ClientDeleteScanInfo(AnyMac()) == 0 || true); break;
            case 11: L.ClientConnectingNodes(ids, R() % 5, &total); break;
            case 12: CHECK(Known(L.ClientNodeInfo(nodes[R() % 10], &ne))); break;
            case 13: L.ClientPassphrase(pp); break;
            case 14: L.ServerInitialize(R() % 20, R(), (R() % 3) ? nullptr : names[R() % 3].u); break;
            case 15: L.ServerFinalize(); break;
            case 16: CHECK(Known(L.ServerStartAccepting((R() & 1) != 0, chans[R() % 8]))); break;
            case 17: L.ServerEndAccepting(); break;
            case 18: L.ServerConnectingClients(ids, R() % 5, &total); break;
            case 19: CHECK(Known(L.ServerClientInfo(nodes[R() % 10], &ne))); break;
            case 20: CHECK(Known(L.ServerClientState(nodes[R() % 10], &total))); break;
            case 21: CHECK(Known(L.ServerAcceptClient(nodes[R() % 10]))); break;
            case 22: CHECK(Known(L.ServerDisconnectClient(nodes[R() % 10]))); break;
            case 23: CHECK(Known(L.ServerStartDistribution())); break;
            case 24: {
                u8* nine = new u8[9];
                for (int i = 0; i < 9; ++i) nine[i] = static_cast<u8>(R());
                CHECK(Known(L.ServerSendPassphrase(nine)));
                delete[] nine;
                break;
            }
            case 25: L.OnGameUdsStarting(); break;
            case 26: L.Pump(); break;
            case 27: L.Poll(true); break;
        }
    }
    void World_() {
        World& w = r.w;
        switch (R() % 16) {
            case 0:
                if (Net* n = AnyNet()) w.GhostClient(n, 2 + static_cast<int>(R() % 3), R(), "fz");
                break;
            case 1:
                if (Net* n = AnyNet()) w.Remove(n, 2 + static_cast<int>(R() % 3));
                break;
            case 2:
                if (Net* n = AnyNet()) w.Flap(n, 1 + static_cast<int>(R() % 4), R() % 3000);
                break;
            case 3: w.Drop(static_cast<u8>(R() % 5), 0, 0, (R() & 1) ? -1 : static_cast<int>(R() % 4)); break;
            case 4: w.drops.clear(); w.parts.clear(); w.hidden.clear(); break;
            case 5: c[R() % 4]->port.fail_send = static_cast<int>(R() % 5); break;
            case 6: c[R() % 4]->port.fail_host_open = (R() % 4) == 0; break;
            case 7: c[R() % 4]->port.fail_scan_open = (R() % 4) == 0; break;
            case 8: c[R() % 4]->port.fail_connect_begin = (R() % 4) == 0; break;
            case 9: {
                static const u64 d[5] = {0, 0, 50, 2900, 4000};
                c[R() % 4]->port.connect_delay = d[R() % 5];
                c[R() % 4]->port.connect_force = (R() % 6) == 0 ? -1 : 0;
                break;
            }
            case 10: w.kick_linger_ms = (R() & 1) ? 0 : 300; break;
            case 11:
                if (Net* n = AnyNet()) {
                    n->ann.accepting = static_cast<u8>(R() & 1);
                    n->blocked = (R() % 4) == 0;
                    n->forced_total = static_cast<u8>(R() % 7);
                }
                break;
            case 12: {
                Port& p = c[R() % 4]->port;
                Msg m;
                std::memset(&m, 0, sizeof m);
                m.from = static_cast<u8>(R() % 6);
                m.len = static_cast<u8>(R() % 17);
                for (u32 i = 0; i < m.len; ++i) m.data[i] = static_cast<u8>(R());
                p.inbox.push_back(m);
                break;
            }
            case 13: {
                Port& p = c[R() % 4]->port;
                u8 buf[kMaxMessage];
                const MsgType t = static_cast<MsgType>(1 + R() % 4);
                const u32 len = EncodeMessage(t, kPass2, t == MsgType::Go ? 9 : 0, buf);
                Msg m;
                std::memset(&m, 0, sizeof m);
                m.from = static_cast<u8>(1 + R() % 4);
                m.len = static_cast<u8>(len);
                std::memcpy(m.data, buf, len);
                p.inbox.push_back(m);
                break;
            }
            case 14: c[R() % 4]->port.fail_read[1 + R() % 4] = (R() % 3) == 0; break;
            case 15: w.dup_extra[1 + R() % 4] = static_cast<u8>(R() % 3); break;
        }
    }
    void Time() {
        const u32 x = R() % 100;
        World& w = r.w;
        if (x < 60) w.now += R() % 40;
        else if (x < 88) w.now += R() % 400;
        else if (x < 96) w.now += R() % 5000;
        else if (x < 99) w.now += R() % 70000;
        else w.now -= R() % 5000;
    }
    void Run(int steps) {
        for (step = 0; step < steps; ++step) {
            Op();
            if (R() % 3) Op();
            if (R() % 6 == 0) World_();
            Time();
            if (R() % 5 < 3) r.PumpAll();
            for (int i = 0; i < 4; ++i) Inv(*c[i], i);
            if (g_sec_fail >= 8) break;
        }
        // the end: everything given back
        for (auto& x : r.c) {
            x->lobby.ClientFinalize();
            x->lobby.ServerFinalize();
            x->lobby.OnGameUdsStarting();
            CHECK(!x->lobby.active());
            CHECK(!x->port.hosting && !x->port.scan_open && !x->port.connecting && !x->port.connected);
        }
    }
};

// The name every list shows for a console whose games send Initialize with no name (Kirby Triple Deluxe, Planet Robobot): the plugin gives the
// lobby the console's profile name with SetDefaultName (DlpHub::SetPlayerName), long before a game opens the wireless menu.
void T_default_name() {
    SECTION("name/searcher: SetDefaultName is the name of an Initialize that gives none; a given name replaces it and stays");
    {
        SF f(100000, false);
        f.L().SetDefaultName(N("Zoe").u);  // (before any Initialize, as the plugin does it)
        f.L().ClientInitialize(nullptr, 16);
        DoScan(f, 0);
        DoJoinTo5(f);
        CHECK(SameName(f.P().last_connect_id.name, N("Zoe")));  // (what the engine is given for the join)
        CHECK_EQ(f.P().last_connect_id.seed, 0xF000);           // (the seed is the configuration's: only the name is replaced)
        NodeEntry me;
        std::memset(&me, 0, sizeof me);
        CHECK_EQ(f.L().ClientNodeInfo(2, &me), 0);
        CHECK(SameName(me.name, N("Zoe")));
        CHECK_EQ(me.seed, 0xF000);
        // a name given by InitializeWithName replaces the default, and a later Initialize with none keeps it
        f.L().ClientInitialize(N("Zed").u, 16);
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.h->slot[2] = Slot();
        u32 res = 0;
        f.L().ClientJoinBegin(M(1), &res);
        CHECK(SameName(f.P().last_connect_id.name, N("Zed")));
        f.L().ClientInitialize(nullptr, 16);
        DoScan(f, 0);
        f.L().ClientStopScan();
        f.h->slot[2] = Slot();
        f.L().ClientJoinBegin(M(1), &res);
        CHECK(SameName(f.P().last_connect_id.name, N("Zed")));
        // the default is read when the lobby is built, too: no SetDefaultName, the configuration's name
        SF g;
        DoScan(g, 0);
        DoJoinTo5(g);
        CHECK(SameName(g.P().last_connect_id.name, N("P0")));
    }
    SECTION("name/creator: SetDefaultName is the host's name in its own record; InitializeWithName replaces it");
    {
        Rig r;
        Con& a = r.Add();  // (the configuration's name: "P0", seed 0xF000)
        NodeEntry e;
        auto host = [&]() {
            std::memset(&e, 0, sizeof e);
            return a.lobby.ServerClientInfo(1, &e);  // the host: kNoData, but FILLED
        };
        CHECK_EQ(a.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(host(), kNoData);
        CHECK(SameName(e.name, N("P0")));
        a.lobby.SetDefaultName(N("Zoe").u);
        CHECK_EQ(host(), kNoData);
        CHECK(SameName(e.name, N("Zoe")));
        CHECK_EQ(e.seed, 0xF000);
        CHECK_EQ(e.node, 1);
        a.lobby.ServerFinalize();
        CHECK_EQ(a.lobby.ServerInitialize(3, 0, nullptr), 0);  // (an Initialize with no name keeps the default)
        CHECK_EQ(host(), kNoData);
        CHECK(SameName(e.name, N("Zoe")));
        CHECK_EQ(a.lobby.ServerInitialize(3, 0, N("Zed").u), 0);  // (a given name replaces it)
        CHECK_EQ(host(), kNoData);
        CHECK(SameName(e.name, N("Zed")));
        CHECK_EQ(a.lobby.ServerInitialize(3, 0, nullptr), 0);  // (and the default does not come back by itself)
        CHECK_EQ(host(), kNoData);
        CHECK(SameName(e.name, N("Zed")));
        // the name goes out in the announcement of the group (the searcher's list) and in the identity handed to the engine
        CHECK_EQ(a.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(a.port.open_ann.size(), 1);
        CHECK(SameName(a.port.open_ann[0].name, N("Zed")));
        CHECK(SameName(a.port.open_id[0].name, N("Zed")));
        CHECK_EQ(a.port.open_id[0].seed, 0xF000);
        // the default alone (an Initialize with no name, as the games send it) is what goes out
        Rig r2;
        Con& b = r2.Add();
        b.lobby.SetDefaultName(N("Yan").u);
        CHECK_EQ(b.lobby.ServerInitialize(3, 0, nullptr), 0);
        CHECK_EQ(b.lobby.ServerStartAccepting(false, 6), 0);
        CHECK_EQ(b.port.open_ann.size(), 1);
        CHECK(SameName(b.port.open_ann[0].name, N("Yan")));
        CHECK(SameName(b.port.open_id[0].name, N("Yan")));
    }
}

void T_fuzz() {
    const u64 starts[] = {1000, 0xFFFFFE00ull, 0x1FFFFFE00ull, 5000000000ull, 0xFFFFFFFF00ull};
    for (u32 seed = 1; seed <= 24; ++seed) {
        SECTIONF("fuzz seed %u", seed);
        Fuzz f(seed, starts[seed % 5]);
        f.Run(2500);
        if (g_sec_fail >= 8) std::printf("  (fuzz seed %u stopped at step %d)\n", seed, f.step);
    }
}

}  // namespace

int main() {
    T_constants();
    T_sizes();
    T_codec_announce();
    T_codec_message();
    T_searcher_matrix();
    T_scan_content_and_filter();
    T_scan_capacity();
    T_scan_ttl();
    T_scan_accepting();
    T_scan_cursor_and_delete();
    T_scan_ticks();
    T_join();
    T_session();
    T_session_messages();
    T_host_gone();
    T_events_searcher();
    T_creator_matrix();
    T_creator_open();
    T_creator_tracking();
    T_creator_phases();
    T_creator_messages();
    T_ownership();
    T_events_creator();
    T_e2e_nominal();
    T_e2e_loss();
    T_e2e_misc();
    T_extra_buffers();
    T_extra_faulty_engine();
    T_extra_frames();
    T_extra_clock_back();
    T_extra_aging();
    T_extra_storm();
    T_extra_disconnect_completion();
    T_extra_creator_timers();
    T_rate();
    T_external_poll();
    T_external_go();
    T_default_name();
    T_threads();
    T_determinism();
    T_fuzz();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
