// Latency of the other players of a local match, from the game's own UDS packets. See azl/latency.hpp.
#include "azl/latency.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "azl/ticks.h"

namespace azl {
namespace latency {

namespace {

// ---- the ring the game's threads write and the network thread reads ----
constexpr int kRing = 256; // packets waiting between two rounds of the network thread: a few dozen at most
constexpr int kBytes = 96; // bytes of each kept: the longest thing read is the 91-byte reply of the PRUDP ping (all the captures: Pia 28, PRUDP 75)

struct Rec {
    volatile u32 seq; // (number of the record + 1) when it is complete
    u64 t_us;
    u8 dir;           // 'S' | 'R'
    u8 node;
    u8 chan;
    u8 len;           // bytes copied into data
    u32 size;
    u8 data[kBytes];
};

Rec* volatile g_ring = nullptr; // created by Tick() when the console joins a lobby; the game's threads do nothing until it exists
volatile u32 g_head = 0;
u32 g_tail = 0;
u32 g_dropped = 0;
// the CPU this module used (ARM11 ticks), counted only with the log on: the hook on the game's threads, and Tick() on the network thread
u32 g_cpu_hook_ticks = 0;
u32 g_cpu_hook_calls = 0;
u32 g_cpu_tick_ticks = 0;

inline u64 NowUs() {
    return azl_ticks_to_us(platform::NowTicks());
}

// >>> game-thread code (Tests/policy_check.sh)
void Push(u8 dir, u8 node, u8 chan, u32 size, const u8* data) {
    Rec* ring = g_ring;
    if (!ring) {
        return;
    }
    const u32 i = __sync_fetch_and_add(&g_head, 1u);
    Rec& r = ring[i % kRing];
    r.seq = 0;
    r.t_us = NowUs();
    r.dir = dir;
    r.node = node;
    r.chan = chan;
    r.size = size;
    const u32 n = size < static_cast<u32>(kBytes) ? size : static_cast<u32>(kBytes);
    r.len = static_cast<u8>(n);
    std::memcpy(r.data, data, n);
    __sync_synchronize();
    r.seq = i + 1;
}
// <<< game-thread code

// ---- the estimator (everything below runs on the network thread only: no locks) ----
// ---- per-node latency estimator (Pia: magic 32 AB 98 64, as in Kirby Battle Royale and Monster Hunter XX) ----
constexpr u8 kMagic[4] = {0x32, 0xAB, 0x98, 0x64};
constexpr int kSentRing = 1024;
constexpr int kSamples = 24;
constexpr u64 kWindowUs = 30ULL * 1000000ULL; // median over 30 s: stability is preferred to reactivity
constexpr u64 kShowUs = 60ULL * 1000000ULL;   // a node stays listed while there is a sample in the last 60 s
constexpr u32 kMaxEchoSize = 128; // control packets with an echo are small (64 bytes); the others are game state

struct Sent {
    u16 ts;
    u64 t_us; // 0 = empty
};
struct Sample {
    u64 t_us;
    u32 rtt_us;
};
struct NodeStat {
    Sample s[kSamples];
    int head = 0;
    int count = 0;
    u64 last_rx_us = 0; // last game packet received from this node (any, even without an echo)
    bool slow = false;  // rare samples (PRUDP: few reliable data to acknowledge): the median uses a longer window
};
constexpr u64 kSlowWindowUs = 120ULL * 1000000ULL;
// A node that sends nothing for this long has left the match: its line goes at once and its old samples are dropped (state
// packets arrive ~30 times a second; the 30 s median stays for stability).
constexpr u64 kAliveUs = 3ULL * 1000000ULL;
Sent g_sent[kSentRing];
Sent g_last_sent = {0, 0};
NodeStat g_nodes[9];

inline u16 Be16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}

// Where the magic sits in the packet: 0 in Kirby Battle Royale and Monster Hunter XX, 12 in Mario Party Star Rush (which puts a
// 12-byte wrapper in front); searched in the first 16 bytes. -1 = not this protocol.
int g_magic_off = -1; // where the magic was found last time: always the same within a game, tried first
int FindMagic(const Rec& r) {
    if (g_magic_off >= 0 && g_magic_off + 12 <= r.len && std::memcmp(r.data + g_magic_off, kMagic, 4) == 0) {
        return g_magic_off;
    }
    for (int o = 0; o <= 16 && o + 12 <= r.len; ++o) {
        if (std::memcmp(r.data + o, kMagic, 4) == 0) {
            g_magic_off = o;
            return o;
        }
    }
    return -1;
}

// ---- PRUDP v1 (NEX), as in Final Fantasy Explorers and Pokemon X: after the wrapper comes EA D0 01 ... The state traffic has
// no echoed timestamp: the round trip comes from matching the RELIABLE data sent (DATA with NEED_ACK) to the aggregated
// acknowledgements (MULTI_ACK, payload = acknowledged sequence numbers). The acknowledgements do not leave at once: the game
// aggregates them and sends them ~60 ms later (even on loopback). That delay is MEASURED on this console's own game (from the
// arrival of a reliable data to the sending of my acknowledgement) and subtracted; until there is a measure, 60 ms ----
struct PrudpPend {
    u64 t_us;
    u16 seq;
    u8 node;
    u8 used;
};
PrudpPend g_p_rx[64];
PrudpPend g_p_tx[64];
int g_p_rx_head = 0, g_p_tx_head = 0;
u32 g_hold_us[16];
int g_hold_count = 0, g_hold_head = 0;
constexpr u32 kDefaultHoldUs = 60000;

inline u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

int g_prudp_off = -1; // like g_magic_off
int FindPrudp(const Rec& r) {
    if (g_prudp_off >= 0 && g_prudp_off + 14 <= r.len && r.data[g_prudp_off] == 0xEA && r.data[g_prudp_off + 1] == 0xD0 &&
        r.data[g_prudp_off + 2] == 0x01) {
        return g_prudp_off;
    }
    for (int o = 0; o <= 16 && o + 14 <= r.len; ++o) {
        if (r.data[o] == 0xEA && r.data[o + 1] == 0xD0 && r.data[o + 2] == 0x01) {
            g_prudp_off = o;
            return o;
        }
    }
    return -1;
}

void PendPut(PrudpPend* a, int* head, u8 node, u16 seq, u64 t) {
    a[*head] = {t, seq, node, 1};
    *head = (*head + 1) % 64;
}

u64 PendTakeT(PrudpPend* a, u8 node, u16 seq) {
    for (int i = 0; i < 64; ++i) {
        if (a[i].used && a[i].node == node && a[i].seq == seq) {
            a[i].used = 0;
            return a[i].t_us;
        }
    }
    return 0;
}

// Non-DATA packets (or DATA without aggregated acknowledgement) sent with NEED_ACK, waiting for the individual ACK.
struct TypedPend {
    u64 t_us;
    u16 seq;
    u8 node;
    u8 typ;
    u8 used;
};
TypedPend g_typed[32];
int g_typed_head = 0;
void PendPutT(u8 node, u8 typ, u16 seq, u64 t) {
    g_typed[g_typed_head] = {t, seq, node, typ, 1};
    g_typed_head = (g_typed_head + 1) % 32;
}
u64 PendTakeTyped(u8 node, u8 typ, u16 seq) {
    for (int i = 0; i < 32; ++i) {
        if (g_typed[i].used && g_typed[i].node == node && g_typed[i].typ == typ && g_typed[i].seq == seq) {
            g_typed[i].used = 0;
            return g_typed[i].t_us;
        }
    }
    return 0;
}
// Requests with a (u16) timestamp sent in unreliable DATA, waiting for the reply that sends it back.
struct EchoPend {
    u64 t_us;
    u16 stamp;
    u8 node;
    u8 used;
};
EchoPend g_echo[16];
int g_echo_head = 0;
void EchoPut(u8 node, u16 stamp, u64 t) {
    g_echo[g_echo_head] = {t, stamp, node, 1};
    g_echo_head = (g_echo_head + 1) % 16;
}
u64 EchoTake(u8 node, u16 stamp) {
    for (int i = 0; i < 16; ++i) {
        if (g_echo[i].used && g_echo[i].node == node && g_echo[i].stamp == stamp) {
            g_echo[i].used = 0;
            return g_echo[i].t_us;
        }
    }
    return 0;
}

// Game clock in the state packets (Final Fantasy Explorers): every "type 5" packet (payload 00 40 94 00, byte 12 = 5) carries
// at payload+20 the sender's game clock in ms (u32 LE). The game keeps the consoles' clocks aligned (measured: a few ms apart),
// so "my clock when it arrives" minus "the clock the sender wrote" is the ONE-WAY delay from that node to me, also for another
// client (with which the game does not ping). My clock comes from my own sends (the first send of each new value) plus the time
// elapsed on the platform clock.
struct OwdSample {
    u64 t_us;
    s32 d_ms;
};
constexpr int kOwdSamples = 160;                    // one sample per new value of the sender's clock: ~15 a second
constexpr u64 kOwdWindowUs = 8ULL * 1000000ULL;
constexpr int kOwdMinSamples = 40;
constexpr int kMeasuredMinSamples = 8;              // below this many real samples in the window a node prefers the estimate
OwdSample g_owd[9][kOwdSamples];
int g_owd_head[9];
int g_owd_count[9];
u32 g_owd_last_clk[9];
u32 g_my_clk = 0;
u64 g_my_clk_t = 0;

inline u32 Le32(const u8* p) {
    return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

u32 CurrentHoldUs() {
    if (g_hold_count < 3) {
        return kDefaultHoldUs;
    }
    u32 v[16];
    for (int i = 0; i < g_hold_count; ++i) v[i] = g_hold_us[i];
    for (int i = 1; i < g_hold_count; ++i) { // insertion sort
        const u32 x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
    return v[g_hold_count / 2];
}

void TouchNode(int node, u64 t_us) {
    NodeStat& n = g_nodes[node];
    if (n.last_rx_us != 0 && t_us > n.last_rx_us && t_us - n.last_rx_us > kAliveUs) {
        n.count = 0;
        n.head = 0;
    }
    n.last_rx_us = t_us;
}

void AddSample(int node, u64 t_us, u32 rtt_us) {
    NodeStat& n = g_nodes[node];
    n.slow = true; // (only called from the PRUDP path)
    n.s[n.head] = {t_us, rtt_us};
    n.head = (n.head + 1) % kSamples;
    if (n.count < kSamples) {
        ++n.count;
    }
}

void FeedPrudp(const Rec& r) {
    const int po = FindPrudp(r);
    if (po < 0 || r.node < 1 || r.node > 8) {
        return;
    }
    const u8* d = r.data + po;
    const int psz = d[3];
    const int pay = Le16(d + 4);
    const u16 tf = Le16(d + 8);
    const int typ = tf & 0xF;
    const int fl = tf >> 4;
    const int sub = d[11];
    const u16 seq = r.len >= po + 14 ? Le16(d + 12) : 0;
    const int pl_off = po + 14 + psz + 16;
    int pl_len = r.len > pl_off ? r.len - pl_off : 0;
    if (pl_len > pay) pl_len = pay;
    if (r.dir == 'R') {
        TouchNode(r.node, r.t_us);
    }
    // General rule of the protocol (NintendoClients / Pretendo wiki): every packet with NEED_ACK (0x4) is acknowledged by a
    // packet of the SAME type, flag ACK (0x1) and the same sequence number. It holds for PING (type 4), SYN, CONNECT,
    // DISCONNECT and, in the versions without aggregated acknowledgement, for DATA. The individual acknowledgement leaves at
    // once: no delay to remove.
    if (!(fl & 0x200) && (typ != 2 || sub != 0 || !(fl & 0x2))) {
        if (r.dir == 'S' && (fl & 0x4) && !(fl & 0x1)) {
            PendPutT(r.node, static_cast<u8>(typ), seq, r.t_us);
        } else if (r.dir == 'R' && (fl & 0x1)) {
            const u64 t_sent = PendTakeTyped(r.node, static_cast<u8>(typ), seq);
            if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                AddSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent));
            }
        }
    }
    if (typ != 2) {
        return;
    }
    // Request/reply with a timestamp inside an unreliable DATA (Final Fantasy Explorers and Pokemon X, every ~1-2 s toward the
    // host): the content is "u32 LE length, ..., byte 20 = 2 (request) or 3 (reply), byte 21 = 9, bytes 22-23 = u16 LE
    // timestamp": the reply sends the request's timestamp back. Structure check: the declared length must match the payload.
    if (!(fl & 0x4) && !(fl & 0x200) && pl_len >= 26 && pl_len == pay) {
        const u8* q = r.data + pl_off;
        const u32 declared = static_cast<u32>(q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24));
        if (declared + 4 == static_cast<u32>(pl_len) && q[21] == 9 && (q[20] == 2 || q[20] == 3)) {
            const u16 stamp = Le16(q + 22);
            if (q[20] == 2 && r.dir == 'S') {
                EchoPut(r.node, stamp, r.t_us);
            } else if (q[20] == 3 && r.dir == 'R') {
                const u64 t_sent = EchoTake(r.node, stamp);
                if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                    AddSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent));
                }
            }
            return;
        }
    }
    if (fl == 0x88 && pl_len >= 24 && pl_off + 24 <= r.len) {
        const u8* q = r.data + pl_off;
        if (q[0] == 0x00 && q[1] == 0x40 && q[2] == 0x94 && q[3] == 0x00 && q[12] == 5) {
            const u32 clk = Le32(q + 20);
            if (r.dir == 'S') {
                if (g_my_clk_t == 0 || clk != g_my_clk) { // the first send of a new value is the moment closest to its creation
                    g_my_clk = clk;
                    g_my_clk_t = r.t_us;
                }
            } else if (r.node >= 1 && r.node <= 8 && clk != g_owd_last_clk[r.node]) { // first arrival of a new value: less waiting inside the game
                g_owd_last_clk[r.node] = clk;
                if (g_my_clk_t != 0 && r.t_us >= g_my_clk_t && r.t_us - g_my_clk_t < 500000ULL) {
                    const s32 my_now = static_cast<s32>(g_my_clk + static_cast<u32>((r.t_us - g_my_clk_t) / 1000ULL));
                    const s32 d = my_now - static_cast<s32>(clk);
                    if (d >= -100 && d <= 3000) { // outside: a clock jump (resynchronisation), not a delay
                        g_owd[r.node][g_owd_head[r.node]] = {r.t_us, d};
                        g_owd_head[r.node] = (g_owd_head[r.node] + 1) % kOwdSamples;
                        if (g_owd_count[r.node] < kOwdSamples) ++g_owd_count[r.node];
                    }
                }
            }
        }
    }
    if (r.dir == 'R') {
        if ((fl & 0x4) && sub == 0) {
            PendPut(g_p_rx, &g_p_rx_head, r.node, seq, r.t_us); // reliable data I will have to acknowledge
        } else if (fl & 0x200) { // the node's aggregated acknowledgement: closes my reliable data
            for (int k = 2; k + 1 < pl_len; k += 2) {
                const u64 t_sent = PendTakeT(g_p_tx, r.node, Le16(r.data + pl_off + k));
                if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                    const u32 raw = static_cast<u32>(r.t_us - t_sent);
                    const u32 hold = CurrentHoldUs();
                    AddSample(r.node, r.t_us, raw > hold ? raw - hold : 0);
                }
            }
        }
    } else { // 'S'
        if (fl & 0x200) { // my aggregated acknowledgement: measures how long my game held the data received
            for (int k = 2; k + 1 < pl_len; k += 2) {
                const u64 t_rx = PendTakeT(g_p_rx, r.node, Le16(r.data + pl_off + k));
                if (t_rx != 0 && r.t_us > t_rx && r.t_us - t_rx < 500000ULL) {
                    g_hold_us[g_hold_head] = static_cast<u32>(r.t_us - t_rx);
                    g_hold_head = (g_hold_head + 1) % 16;
                    if (g_hold_count < 16) ++g_hold_count;
                }
            }
        } else if ((fl & 0x4) && sub == 0 && r.node >= 1 && r.node <= 8) {
            PendPut(g_p_tx, &g_p_tx_head, r.node, seq, r.t_us);
        }
    }
}

// Protocol recognition, for the log only (ProtoSummary): counts the packets of each kind and keeps the first bytes of an
// unrecognised packet, sent and received, so that a new game can be looked at from a log sent by someone else.
struct ProtoStat {
    u32 pia, prudp, other;
    u8 smp[2][24];       // first bytes of the first unknown packet: 0 = sent, 1 = received
    u8 smp_len[2];
    u32 smp_size[2];
    u32 logged_total;    // total at the time of the last log line
};
ProtoStat g_proto;

void ClassifyProto(const Rec& r) {
    if (FindMagic(r) >= 0) {
        ++g_proto.pia;
    } else if (FindPrudp(r) >= 0) {
        ++g_proto.prudp;
    } else {
        ++g_proto.other;
        const int k = r.dir == 'S' ? 0 : 1;
        if (g_proto.smp_len[k] == 0 && r.len > 0) {
            g_proto.smp_len[k] = r.len < 24 ? r.len : 24;
            g_proto.smp_size[k] = r.size;
            std::memcpy(g_proto.smp[k], r.data, g_proto.smp_len[k]);
        }
    }
}

void FeedPia(const Rec& r, int mo);

// Lock on the protocol: as soon as the game is known to speak Pia (or PRUDP v1) ONLY that is checked, without trying the other
// one at every packet. Every 64 packets one is classified again to notice a change (8 in a row of the other kind unlock).
enum { kProtoNone = 0, kProtoPia = 1, kProtoPrudp = 2 };
u8 g_lock_proto = kProtoNone;
u32 g_lock_check = 0;
u32 g_lock_mismatch = 0;
constexpr u32 kLockMin = 32; // packets of one kind (and fewer than a quarter of the other) to decide

void Feed(const Rec& r) {
    if (g_lock_proto != kProtoNone) {
        if ((++g_lock_check & 63u) == 0) {
            ClassifyProto(r);
            const bool is_pia = FindMagic(r) >= 0;
            const bool is_prudp = !is_pia && FindPrudp(r) >= 0;
            if ((g_lock_proto == kProtoPia && is_prudp) || (g_lock_proto == kProtoPrudp && is_pia)) {
                if (++g_lock_mismatch >= 8) {
                    g_lock_proto = kProtoNone;
                    g_lock_mismatch = 0;
                }
            } else if (is_pia || is_prudp) {
                g_lock_mismatch = 0;
            }
        }
        if (g_lock_proto == kProtoPia) {
            const int mo = FindMagic(r);
            if (mo >= 0) {
                FeedPia(r, mo);
            }
        } else if (g_lock_proto == kProtoPrudp) {
            FeedPrudp(r);
        }
        return;
    }
    ClassifyProto(r);
    const int mo = FindMagic(r);
    if (mo < 0) {
        FeedPrudp(r);
    } else {
        FeedPia(r, mo);
    }
    if (g_proto.pia >= kLockMin && g_proto.prudp * 4 < g_proto.pia) {
        g_lock_proto = kProtoPia;
    } else if (g_proto.prudp >= kLockMin && g_proto.pia * 4 < g_proto.prudp) {
        g_lock_proto = kProtoPrudp;
    }
}

void FeedPia(const Rec& r, int mo) {
    const u16 ts = Be16(r.data + mo + 8);
    if (r.dir == 'S') {
        g_sent[ts & (kSentRing - 1)] = {ts, r.t_us};
        g_last_sent = {ts, r.t_us};
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    {
        NodeStat& n = g_nodes[r.node];
        if (n.last_rx_us != 0 && r.t_us > n.last_rx_us && r.t_us - n.last_rx_us > kAliveUs) {
            n.count = 0; // the node is back after a pause (new match): start without the old samples
            n.head = 0;
        }
        n.last_rx_us = r.t_us;
    }
    const u16 echo = Be16(r.data + mo + 10);
    // Pia header (NintendoClients wiki "Pia Protocol", versions up to 5.10): 0x8 = the sender's session timer (ms), 0xA = the
    // receiver's timer since the last exchange. 0 = has not received anything yet; 0xFFFF = sentinel the protocol uses when the
    // echo would be 0. Neither is an echo.
    if (echo == 0 || echo == 0xFFFF) {
        return;
    }
    // Rule (checked on Kirby Battle Royale and Monster Hunter XX, which share the header): bytes 8-9 are the sender's timestamp
    // (ms), bytes 10-11 the last timestamp RECEIVED by it from me, sent back: exact (Kirby, control packets) or already
    // increased by the time the packet sat still there (MH XX, almost every packet). Either way the round trip with that node
    // is "my clock now" minus the echo. If the echo is one of my sends the difference is taken on the platform clock
    // (microseconds); otherwise on my game clock, derived from the last send.
    u32 rtt_us;
    const Sent& s = g_sent[echo & (kSentRing - 1)];
    if (s.t_us != 0 && s.ts == echo && r.t_us > s.t_us && r.t_us - s.t_us <= 3000000ULL) {
        rtt_us = static_cast<u32>(r.t_us - s.t_us);
    } else {
        if (g_last_sent.t_us == 0 || r.t_us < g_last_sent.t_us || r.t_us - g_last_sent.t_us > 2000000ULL) {
            return; // no recent send of mine to derive my clock from
        }
        const u32 now_ts = (static_cast<u32>(g_last_sent.ts) + static_cast<u32>((r.t_us - g_last_sent.t_us) / 1000ULL)) & 0xFFFFu;
        const u32 diff_ms = (now_ts - echo) & 0xFFFFu;
        if (diff_ms > 2000) {
            return; // not an echo of a timestamp of mine
        }
        rtt_us = diff_ms * 1000u;
    }
    NodeStat& n = g_nodes[r.node];
    n.s[n.head] = {r.t_us, rtt_us};
    n.head = (n.head + 1) % kSamples;
    if (n.count < kSamples) {
        ++n.count;
    }
}

} // namespace

// >>> game-thread code (Tests/policy_check.sh)
void OnSend(u16 dest_node, u8 channel, const u8* data, u32 size) {
    if (!g_ring || !data || !size || dest_node > 255) {
        return;
    }
    if (g_debug_level > 0) { // the log is on: count the ticks this takes (two clock reads), for the "lat: cpu" line
        const u64 t0 = platform::NowTicks();
        Push('S', static_cast<u8>(dest_node), channel, size, data);
        __sync_fetch_and_add(&g_cpu_hook_ticks, static_cast<u32>(platform::NowTicks() - t0));
        __sync_fetch_and_add(&g_cpu_hook_calls, 1u);
        return;
    }
    Push('S', static_cast<u8>(dest_node), channel, size, data);
}

void OnRecv(u16 src_node, u8 channel, const u8* data, u32 size) {
    if (!g_ring || !data || !size || src_node > 255) {
        return;
    }
    if (g_debug_level > 0) {
        const u64 t0 = platform::NowTicks();
        Push('R', static_cast<u8>(src_node), channel, size, data);
        __sync_fetch_and_add(&g_cpu_hook_ticks, static_cast<u32>(platform::NowTicks() - t0));
        __sync_fetch_and_add(&g_cpu_hook_calls, 1u);
        return;
    }
    Push('R', static_cast<u8>(src_node), channel, size, data);
}
// <<< game-thread code

namespace {

int Pump() {
    int fed = 0;
    const u32 head = g_head;
    if (head - g_tail > static_cast<u32>(kRing)) { // the ring was filled while it was not read: skip to the newest
        g_dropped += head - g_tail - kRing;
        g_tail = head - kRing;
    }
    while (g_tail != head) {
        const Rec& r = g_ring[g_tail % kRing];
        if (r.seq != g_tail + 1) { // still being written: next round
            break;
        }
        Feed(r);
        ++fed;
        ++g_tail;
    }
    return fed;
}

// what Tick() last wrote, to write only changes
struct LogState {
    u64 last_ms = 0;
    u64 proto_ms = 0;
    u64 cpu_ms = 0;
    u32 mask = 0xFFFFFFFFu;
    int shown[9];
    u64 shown_ms[9];
};
LogState g_log;

// "lat: cpu ...": what this module cost in the last window, in ms of CPU per second of wall time (the ticks of the two clock reads that
// measure it are in it too). hook = the game's threads (OnSend / OnRecv), tick = this thread (Pump, estimates, log lines).
void LogCpu(u64 now_ms) {
    const u64 window_ms = now_ms - g_log.cpu_ms;
    if (window_ms < 9000) {
        return;
    }
    g_log.cpu_ms = now_ms;
    const u32 hk = g_cpu_hook_ticks, calls = g_cpu_hook_calls, tk = g_cpu_tick_ticks;
    __sync_fetch_and_sub(&g_cpu_hook_ticks, hk);
    __sync_fetch_and_sub(&g_cpu_hook_calls, calls);
    __sync_fetch_and_sub(&g_cpu_tick_ticks, tk);
    // hundredths of a millisecond of CPU per second: ticks / AZL_TICKS_PER_MS ms in window_ms ms
    const u64 den = static_cast<u64>(AZL_TICKS_PER_MS) * window_ms;
    const u64 hook100 = static_cast<u64>(hk) * 100000ULL / den;
    const u64 tick100 = static_cast<u64>(tk) * 100000ULL / den;
    const u64 total100 = hook100 + tick100;
    const u64 pct100 = total100 / 10; // 1 ms/s = 0.1 % of a core: hundredths of a per cent
    Log("lat: cpu hook %lu.%02lu ms/s (%lu packets), tick %lu.%02lu ms/s, total %lu.%02lu ms/s = %lu.%02lu%% of a core | ring %lu KB, %lu lost",
        static_cast<unsigned long>(hook100 / 100), static_cast<unsigned long>(hook100 % 100), static_cast<unsigned long>(calls),
        static_cast<unsigned long>(tick100 / 100), static_cast<unsigned long>(tick100 % 100), static_cast<unsigned long>(total100 / 100),
        static_cast<unsigned long>(total100 % 100), static_cast<unsigned long>(pct100 / 100), static_cast<unsigned long>(pct100 % 100),
        static_cast<unsigned long>(sizeof(Rec) * kRing / 1024), static_cast<unsigned long>(g_dropped));
}

} // namespace

void Tick(u64 now_ms, bool in_session) {
    if (!g_ring) {
        if (!in_session) {
            return;
        }
        Rec* ring = new (std::nothrow) Rec[kRing];
        if (!ring) {
            return;
        }
        std::memset(ring, 0, sizeof(Rec) * kRing);
        for (int i = 0; i < 9; ++i) {
            g_log.shown[i] = -1000;
            g_log.shown_ms[i] = 0;
        }
        g_log.last_ms = now_ms;
        g_log.proto_ms = now_ms;
        g_log.cpu_ms = now_ms;
        g_ring = ring;
    }
    const bool timing = g_debug_level > 0;
    const u64 t0 = timing ? platform::NowTicks() : 0;
    Pump();
    if (g_debug_level <= 0 || now_ms - g_log.last_ms < 1000) {
        if (timing) {
            g_cpu_tick_ticks += static_cast<u32>(platform::NowTicks() - t0);
        }
        return;
    }
    g_log.last_ms = now_ms;
    NodeRtt nr[8];
    Snapshot(nr);
    u32 mask = 0;
    for (int i = 0; i < 8; ++i) {
        if (nr[i].age_ms >= 0) {
            mask |= 1u << nr[i].node;
        }
    }
    if (mask != g_log.mask) {
        g_log.mask = mask;
        Log("lat: nodes sending %X", static_cast<unsigned>(mask));
    }
    for (int i = 0; i < 8; ++i) {
        const NodeRtt& o = nr[i];
        if (o.age_ms < 0 || o.rtt_ms < 0) {
            continue;
        }
        // a line when the value moved by 2 ms or more, and at least every 5 s while the node plays
        const int moved = o.rtt_ms > g_log.shown[o.node] ? o.rtt_ms - g_log.shown[o.node] : g_log.shown[o.node] - o.rtt_ms;
        if (moved >= 2 || now_ms - g_log.shown_ms[o.node] >= 5000) {
            g_log.shown[o.node] = o.rtt_ms;
            g_log.shown_ms[o.node] = now_ms;
            Log("lat: n%d %d ms (%d samples, %c)", o.node, o.rtt_ms, o.samples, o.stale ? 'S' : (o.estimated ? 'E' : 'M'));
        }
    }
    if (now_ms - g_log.proto_ms >= 30000) {
        g_log.proto_ms = now_ms;
        char pr[300];
        if (ProtoSummary(pr, sizeof(pr)) > 0) {
            Log("lat: protocol %s", pr);
        }
    }
    g_cpu_tick_ticks += static_cast<u32>(platform::NowTicks() - t0); // (the lines above are part of the cost; the cost line itself is not)
    LogCpu(now_ms);
}

int Snapshot(NodeRtt out[8]) {
    const u64 now = NowUs();
    int shown = 0;
    for (int node = 1; node <= 8; ++node) {
        NodeRtt& o = out[node - 1];
        o = NodeRtt();
        o.node = node;
        const NodeStat& n = g_nodes[node];
        u32 v[kSamples];
        int c = 0;
        u64 newest = 0;
        u32 last = 0;
        u32 lowest = 0xFFFFFFFFu;
        for (int i = 0; i < n.count; ++i) {
            const Sample& s = n.s[i];
            if (now - s.t_us <= (n.slow ? kSlowWindowUs : kWindowUs)) {
                v[c++] = s.rtt_us;
                if (s.rtt_us < lowest) lowest = s.rtt_us;
            }
            if (s.t_us > newest) {
                newest = s.t_us;
                last = s.rtt_us;
            }
        }
        // A line is shown while game packets arrive from the node (even if there are no samples yet: then "name --", as for the
        // clients of FF Explorers, which send little reliable data).
        if (n.last_rx_us != 0 && now - n.last_rx_us <= kAliveUs) {
            ++shown;
            o.age_ms = newest ? static_cast<int>((now - newest) / 1000) : 0;
            o.last_ms = newest ? static_cast<int>(last / 1000) : -1;
        }
        if (c > 0) {
            for (int i = 1; i < c; ++i) { // insertion sort (c <= 24)
                const u32 x = v[i];
                int j = i - 1;
                while (j >= 0 && v[j] > x) {
                    v[j + 1] = v[j];
                    --j;
                }
                v[j + 1] = x;
            }
            o.rtt_ms = static_cast<int>(v[c / 2] / 1000);
            o.min_ms = static_cast<int>(lowest / 1000);
            o.samples = c;
        } else if (o.age_ms >= 0 && n.count > 0) {
            // No recent sample but the node is still connected (FF Explorers: a client sends reliable data only when it joins,
            // afterwards there is nothing to match). The last known value is shown, marked as old.
            u32 w[kSamples];
            int m = 0;
            for (int i = 0; i < n.count; ++i) w[m++] = n.s[i].rtt_us;
            for (int i = 1; i < m; ++i) {
                const u32 x = w[i];
                int j = i - 1;
                while (j >= 0 && w[j] > x) {
                    w[j + 1] = w[j];
                    --j;
                }
                w[j + 1] = x;
            }
            o.rtt_ms = static_cast<int>(w[m / 2] / 1000);
            o.stale = true;
        }
        // One-way delay (game clock in the state packets): median of the samples of the last 8 s, one sample per new value of
        // the sender's clock (so the packets that repeat a value add no waiting). Checked with three instances on one PC
        // against the real delivery delay (the same packets seen by sender and receiver): within ~5 ms on all six directions.
        // The 5th percentile was off by 3-16 ms, differently from node to node.
        int d[kOwdSamples];
        int dc = 0;
        for (int i = 0; i < g_owd_count[node]; ++i) {
            if (now - g_owd[node][i].t_us <= kOwdWindowUs) d[dc++] = g_owd[node][i].d_ms;
        }
        if (dc >= kOwdMinSamples && o.age_ms >= 0) {
            for (int i = 1; i < dc; ++i) {
                const int x = d[i];
                int j = i - 1;
                while (j >= 0 && d[j] > x) {
                    d[j + 1] = d[j];
                    --j;
                }
                d[j + 1] = x;
            }
            o.owd_ms = d[dc / 2] < 0 ? 0 : d[dc / 2];
        }
    }
    // Where there is a one-way delay (FF Explorers, every node, also client against client) the round trip is 2 x one-way
    // delay, for ALL nodes with the same rule: the direct measures (the ping echo toward the host: +~33 ms because the host
    // replies at its next frame; the acknowledgements with their correction: +~5 ms) stay for when the clock is not available
    // yet. Note: toward the host (node 1) the clients' clocks are aligned with its (as NTP does) and the one-way delay is
    // exactly half the round trip even if the path is asymmetric; toward another client there remains the alignment error
    // between the two, a few ms.
    for (int i = 0; i < 8; ++i) {
        NodeRtt& o = out[i];
        if (o.owd_ms >= 0) {
            o.rtt_ms = 2 * o.owd_ms;
            if (o.rtt_ms < 1) o.rtt_ms = 1;
            o.stale = false;
            o.estimated = true;
        }
    }
    return shown;
}

int ProtoSummary(char* buf, int cap) {
    const u32 total = g_proto.pia + g_proto.prudp + g_proto.other;
    if (total == 0 || total == g_proto.logged_total) {
        return 0;
    }
    g_proto.logged_total = total;
    const char* name = "unknown";
    if (g_proto.pia > 0 && g_proto.pia >= g_proto.prudp && g_proto.pia > g_proto.other) {
        name = "Pia";
    } else if (g_proto.prudp > 0 && g_proto.prudp > g_proto.pia && g_proto.prudp > g_proto.other) {
        name = "PRUDP v1";
    }
    // compact: "Pia,<pia>,<prudp>,<other>" and, if there are unknown packets, ",U<S|R>,<size>,<first bytes in hex>"
    // after the lock the counts grow by 1 every 64 packets (a check sample), not by every packet
    int len = std::snprintf(buf, cap, "%s,%u,%u,%u,lock=%s", name, static_cast<unsigned>(g_proto.pia),
                            static_cast<unsigned>(g_proto.prudp), static_cast<unsigned>(g_proto.other),
                            g_lock_proto == kProtoPia ? "Pia" : (g_lock_proto == kProtoPrudp ? "PRUDP" : "-"));
    for (int k = 0; k < 2 && g_proto.other > 0; ++k) {
        if (g_proto.smp_len[k] == 0 || len >= cap - 80) {
            continue;
        }
        len += std::snprintf(buf + len, cap - len, ",U%c,%u,", k == 0 ? 'S' : 'R', static_cast<unsigned>(g_proto.smp_size[k]));
        for (int i = 0; i < g_proto.smp_len[k] && len < cap - 4; ++i) {
            len += std::snprintf(buf + len, cap - len, "%02X", g_proto.smp[k][i]);
        }
    }
    return len;
}


} // namespace latency
} // namespace azl
