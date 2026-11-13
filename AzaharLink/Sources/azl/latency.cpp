// Latency of the other players of a local match, from the game's own UDS packets. See azl/latency.hpp.
#include "azl/latency.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "azl/ticks.h"
#include "azl/wallclock.hpp"

namespace azl {
namespace latency {

// ---- latency.log: the queue and the file (the writer thread's side), see latency.hpp ----
namespace {
constexpr const char* kFileName = "latency.log";
constexpr const char* kOldFileName = "latency.1.log";
constexpr std::size_t kMaxPending = 32 * 1024; // lines waiting for the writer (a second of this module is ~300 bytes)
} // namespace

LatencyFile::LatencyFile(FileLayer* files, std::string dir, u64 rotate_bytes)
    : files_(files), dir_(std::move(dir)), rotate_bytes_(rotate_bytes) {}

LatencyFile::~LatencyFile() {
    Close();
}

void LatencyFile::Append(const char* text, u32 size) {
    if (!platform::TryLockBriefly(queue_mutex_)) {
        if (!platform::IsLogWriterThread()) {
            __atomic_fetch_add(&dropped_, 1u, __ATOMIC_RELAXED);
            return;
        }
        queue_mutex_.lock();
    }
    if (pending_.size() + size <= kMaxPending) {
        pending_.append(text, size);
    } else {
        __atomic_fetch_add(&dropped_, 1u, __ATOMIC_RELAXED);
    }
    queue_mutex_.unlock();
}

void LatencyFile::RotateLocked() {
    files_->Close(file_);
    file_ = nullptr;
    files_->Remove(dir_ + kOldFileName);
    files_->Rename(dir_ + kFileName, dir_ + kOldFileName);
    size_ = 0;
}

void LatencyFile::Flush() {
    std::string text;
    {
        platform::LockGuard lock(queue_mutex_);
        if (pending_.empty()) {
            return;
        }
        text.swap(pending_);
    }
    if (g_debug_level <= 0) {
        return; // the log was switched off: nothing is created or written (what was queued is dropped)
    }
    platform::LockGuard lock(file_mutex_);
    if (!file_) {
        file_ = files_->OpenAppend(dir_ + kFileName);
        if (!file_) {
            return;
        }
        size_ = files_->Size(file_);
    }
    files_->Write(file_, text.data(), text.size());
    files_->Flush(file_);
    size_ += text.size();
    if (size_ >= rotate_bytes_) {
        RotateLocked();
    }
}

void LatencyFile::Close() {
    Flush();
    platform::LockGuard lock(file_mutex_);
    if (file_) {
        files_->Close(file_);
        file_ = nullptr;
    }
}

namespace {

LatencyFile* g_file = nullptr;
u64 (*g_clock_ms)() = nullptr;
bool (*g_name_of)(int node, char* out, u32 cap) = nullptr;
char g_node_name[9][24] = {}; // the last name given for each node (index = node number 1..8) // the name the game gave a node (plain ASCII), for the lines of the log

/// One line of this module: with a file, "hh:mm:ss.mmm text\n" (the console's clock) into latency.log's queue; without, Log().
void Emit(const char* fmt, ...) {
    char text[420];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (!g_file || !g_clock_ms) {
        Log("%s", text);
        return;
    }
    char stamp[24];
    FormatConsoleClock(g_clock_ms(), stamp); // "YYYY-MM-DD hh:mm:ss.mmm": the time of the day is from the 12th character
    char line[460];
    const int n = std::snprintf(line, sizeof(line), "%s %s\n", stamp + 11, text);
    if (n > 0 && n < static_cast<int>(sizeof(line))) {
        g_file->Append(line, static_cast<u32>(n));
    }
}


// ---- the ring the game's threads write and the network thread reads ----
// Packets waiting between two rounds of the network thread. A round is a few ms (measured on a New 3DS: ~330 rounds a second, longest pause 9 ms) and a
// match gives 60-250 packets a second, so a handful wait at most; 128 hold half a second of the busiest match (8 players, a Tetris-like lobby: the
// user's decision, 2026-11-11 - 64 would do for four players, but the log of a full lobby must be whole: the longest pause of the network thread in the
// logs of real consoles is 481 ms). What a longer pause loses gives no sample (never a wrong one) and is counted and written ("lat: ring dropped").
constexpr int kRing = 128;
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
// The window of a node: its last kSlots half-seconds (30 s), each with ONE value - the median of the samples that came in it (the first kSlotSamples).
// The value shown and logged is the median of those values; the lowest and the 90th percentile of them go to the log too. Why not "the last 24
// samples" as before: in a game that gives ~10 samples a second (Monster Hunter) 24 samples are 2.4 s, and on a network that did not change the value
// moved by -9..+21 ms around the real delay (replayed on the captures, NOTE_LATENCY.md); with this window it stays within -6..+11 ms,
// as the exact median of every sample of the last 30 s does (-6..+10), at a tenth of the memory that would take. With 1-3 samples a second (most Pia
// games) a half-second holds one sample, so nothing is lost.
constexpr int kSlots = 60;
constexpr int kSlotSamples = 8;
constexpr u64 kSlotUs = 500000ULL;
constexpr u64 kSlowSlotUs = 500000ULL;  // the games with few samples (PRUDP, Tetris, Tekken, Street Fighter) had slots of 2 s - a window of 120 s - and
                                         // that was too long to follow (the user, 2026-11-13): the same 30 s as the others; a game with samples minutes apart
                                         // (Tetris) shows its last value as old between them
// The window's median is steady by construction: a connection that gets worse for a few seconds would not move it. So the last 2 s are looked at on
// their own (the slots of the last 2 s, the open one included: 4 of a half-second, or the 2 s one of the slow games) and, when they are clearly worse
// than the window - at least one and a half times it AND 30 ms above it - THAT value is shown, until they are back near the window (the user's request,
// 2026-11-13: "if I go from 50 to 500 ms for 2 s I want to see 500, not the median"). A better moment does not move the shown value: the window does.
constexpr u64 kRecentUs = 2000000ULL;
constexpr int kSpikeMinMs = 30;

struct Sent {
    u16 ts;
    u64 t_us; // 0 = empty
};
struct Slot {
    u32 number;   // which one: time / the slot's length
    u16 ms;       // the median of its samples
    u8 n;         // samples that came in it (0 = empty; counted up to 255)
    u8 corrected; // how many of them are aggregated acknowledgements with the hold time removed (PRUDP), not a direct measure
};
struct NodeStat {
    Slot slot[kSlots];
    u16 cur[kSlotSamples]; // the samples (ms) of the slot being filled
    u32 cur_number = 0;
    u8 cur_n = 0;          // samples that came in it (the first kSlotSamples are kept)
    u8 cur_corrected = 0;
    bool slow = false;     // PRUDP: see kSlowSlotUs
    bool has_last = false;
    u16 last_ms = 0;       // the newest sample
    u64 last_sample_us = 0;
    u64 last_rx_us = 0;    // last game packet received from this node (any, even without an echo)
};
// A node that sends nothing for this long has left the match: its line goes at once and its old samples are dropped (state
// packets arrive ~30 times a second; the 30 s median stays for stability).
constexpr u64 kAliveUs = 3ULL * 1000000ULL;
Sent g_last_sent = {0, 0}; // my last send: my game clock at any moment is its timer plus the time since (see FeedPia)
NodeStat g_nodes[8];       // (node - 1)

inline NodeStat& StatOf(int node) {
    return g_nodes[node - 1];
}

inline u64 SlotUs(const NodeStat& n) {
    return n.slow ? kSlowSlotUs : kSlotUs;
}

// The median of `n` (1..kSlotSamples) values, sorted in place. For an even count the mean of the two in the middle: with two samples "the lower one"
// would be a minimum, and the value read ~5 ms low in the games that give 1-3 samples a second (replayed on the captures).
void Sort(u16* v, int n) { // insertion sort (n <= kSlots + 1)
    for (int i = 1; i < n; ++i) {
        const u16 x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
}

u16 MedianOf(u16* v, int n) {
    Sort(v, n);
    return (n & 1) ? v[n / 2] : static_cast<u16>((static_cast<u32>(v[n / 2 - 1]) + v[n / 2]) / 2u);
}

void CloseSlot(NodeStat& n) {
    if (n.cur_n == 0) {
        return;
    }
    Slot& s = n.slot[n.cur_number % kSlots];
    s.number = n.cur_number;
    s.ms = MedianOf(n.cur, n.cur_n < kSlotSamples ? n.cur_n : kSlotSamples);
    s.n = n.cur_n;
    s.corrected = n.cur_corrected;
    n.cur_n = 0;
    n.cur_corrected = 0;
}

// The node is back after a pause (a new match), or changes protocol: start without the old samples.
void ResetNode(NodeStat& n) {
    for (int i = 0; i < kSlots; ++i) {
        n.slot[i].n = 0;
    }
    n.cur_n = 0;
    n.cur_corrected = 0;
    n.has_last = false;
    n.last_sample_us = 0;
}

void AddSample(NodeStat& n, u64 t_us, u32 rtt_us, bool corrected) {
    const u32 number = static_cast<u32>(t_us / SlotUs(n));
    if (n.cur_n != 0 && number != n.cur_number) {
        CloseSlot(n);
    }
    n.cur_number = number;
    const u32 ms = rtt_us / 1000u;
    const u16 v = static_cast<u16>(ms > 0xFFFFu ? 0xFFFFu : ms);
    if (n.cur_n < kSlotSamples) {
        n.cur[n.cur_n] = v;
    }
    if (n.cur_n < 255) {
        ++n.cur_n;
    }
    if (corrected && n.cur_corrected < 255) {
        ++n.cur_corrected;
    }
    n.has_last = true;
    n.last_ms = v;
    n.last_sample_us = t_us;
}

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
    u32 t_ms; // (the platform clock in ms, modulo 2^32: only differences of a few seconds are taken)
    std::int16_t d_ms;
};
constexpr int kOwdSamples = 160;                    // one sample per new value of the sender's clock: ~15 a second
constexpr u32 kOwdWindowMs = 8000;
constexpr int kOwdMinSamples = 40;
// 8 nodes x kOwdSamples (10 KB), made the first time a game sends such a clock: only Final Fantasy Explorers does, no other game pays for it.
OwdSample* g_owd = nullptr;
inline OwdSample* OwdOf(int node) {
    return g_owd + (node - 1) * kOwdSamples;
}
int g_owd_head[9];
int g_owd_count[9];
u32 g_owd_last_clk[9];
u32 g_my_clk = 0;
u64 g_my_clk_t = 0;

inline u32 Le32(const u8* p) {
    return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

u32 CurrentHoldUs(u32 default_us = kDefaultHoldUs) {
    if (g_hold_count < 3) {
        return default_us;
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
    NodeStat& n = StatOf(node);
    if (n.last_rx_us != 0 && t_us > n.last_rx_us && t_us - n.last_rx_us > kAliveUs) {
        ResetNode(n);
    }
    n.last_rx_us = t_us;
}

// A sample of the PRUDP path. `corrected`: it is an aggregated acknowledgement with the hold time removed (an assumption about the other game), not a
// direct measure like the ping echo or the individual ACK: the log marks a value made mostly of those with A instead of M.
void AddPrudpSample(int node, u64 t_us, u32 rtt_us, bool corrected) {
    NodeStat& n = StatOf(node);
    if (!n.slow) {
        ResetNode(n); // (the slots change length)
        n.slow = true;
    }
    AddSample(n, t_us, rtt_us, corrected);
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
                AddPrudpSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent), false);
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
                    AddPrudpSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent), false);
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
                        if (!g_owd) {
                            g_owd = new (std::nothrow) OwdSample[8 * kOwdSamples]; // (the network thread, like the ring; no memory: no estimate from the clock)
                        }
                        if (g_owd) {
                            OwdOf(r.node)[g_owd_head[r.node]] = {static_cast<u32>(r.t_us / 1000ULL), static_cast<std::int16_t>(d)};
                            g_owd_head[r.node] = (g_owd_head[r.node] + 1) % kOwdSamples;
                            if (g_owd_count[r.node] < kOwdSamples) ++g_owd_count[r.node];
                        }
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
                    AddPrudpSample(r.node, r.t_us, raw > hold ? raw - hold : 0, true);
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

// ---- "ack" layer (Tetris, EUR - the game of Hudson Soft that takes up to 8 players): neither Pia nor PRUDP. Found on a capture of three instances (2026-11-12);
// every packet has this header (all words little endian):
//   0  u32 destination node (0xFFFFFFFF = everybody)        4  u32 0        8  u16 0x0080        10 u16 the packet's own size
//   12 u16 X, u8 counter of the packets this console sent (any kind), u8 0   16 u32 1     20 u32 sender node     24 u32 destination again
//   28 u8 TYPE, then the three letters "ack"                32 u32 length of what follows from byte 20
//   36 u16 msg, 38 u16 node   (msg = the number of a reliable message, 0 = unreliable; node = the node that SENT the message)
// TYPE 0 = DATA (every size: 108, 124, 132, 224 ...; broadcast), 1 = the heartbeat every second (52 bytes, broadcast), 6 = ACK (52 bytes, to the sender of the
// data, with the SAME msg and node), 2 = a big transfer, 4 and 5 = rare control. A reliable DATA is answered by an ACK from EVERY other node, which leaves when
// the other game next looks at its network (measured: 17-34 ms after the data arrived, a frame at 30 fps). So the round trip with a node is
// "my DATA left" -> "the ACK of that node arrived", minus the time the other game held it - like the PRUDP acknowledgements, estimated with MY OWN hold (my game
// holds its ACKs the same way: measured here from the arrival of a node's reliable DATA to the departure of my ACK).
// Checked on three instances on one PC (the same bytes seen leaving one and arriving at another give the real delay): see NOTE_LATENCY.md.
constexpr u32 kAckDefaultHoldUs = 25000; // until three holds of my own are measured: between the 18 and the 34 ms seen

inline bool IsAckLayer(const Rec& r) {
    return r.len >= 40 && r.data[29] == 'a' && r.data[30] == 'c' && r.data[31] == 'k' && Le32(r.data + 4) == 0 && Le32(r.data + 16) == 1 &&
           static_cast<u32>(Le16(r.data + 10)) == r.size;
}

struct AckTx {
    u64 t_us;
    u16 seq;
    u8 used;
    u8 dup; // sent again: its acknowledgements say nothing about this send
};
AckTx g_ack_tx[32];
int g_ack_tx_head = 0;
u16 g_ack_me = 0; // my node, from the sender field of what I send

void FeedAck(const Rec& r) {
    const u8 ty = r.data[28];
    const u16 msg = Le16(r.data + 36);
    const u16 node = Le16(r.data + 38);
    if (r.dir == 'S') {
        const u32 me = Le32(r.data + 20);
        if (me >= 1 && me <= 8) {
            g_ack_me = static_cast<u16>(me);
        }
        if (ty == 0 && msg != 0) { // a reliable message of mine: the acknowledgements will name it
            for (int i = 0; i < 32; ++i) {
                if (g_ack_tx[i].used && g_ack_tx[i].seq == msg && r.t_us - g_ack_tx[i].t_us < 5000000ULL) {
                    g_ack_tx[i].dup = 1;
                    return;
                }
            }
            g_ack_tx[g_ack_tx_head] = {r.t_us, msg, 1, 0};
            g_ack_tx_head = (g_ack_tx_head + 1) % 32;
        } else if (ty == 6) { // my acknowledgement of a message of a node: how long my game held it
            const u64 t_rx = PendTakeT(g_p_rx, node <= 255 ? static_cast<u8>(node) : 0, msg);
            if (t_rx != 0 && r.t_us > t_rx && r.t_us - t_rx < 500000ULL) {
                g_hold_us[g_hold_head] = static_cast<u32>(r.t_us - t_rx);
                g_hold_head = (g_hold_head + 1) % 16;
                if (g_hold_count < 16) ++g_hold_count;
            }
        }
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    TouchNode(r.node, r.t_us);
    if (ty == 0 && msg != 0) { // a reliable message of the node: I will acknowledge it
        PendPut(g_p_rx, &g_p_rx_head, r.node, msg, r.t_us);
    } else if (ty == 6 && g_ack_me != 0 && node == g_ack_me) { // the node acknowledges a message of mine
        for (int i = 0; i < 32; ++i) {
            const AckTx& s = g_ack_tx[i];
            if (s.used && s.seq == msg) {
                if (!s.dup && r.t_us > s.t_us && r.t_us - s.t_us < 3000000ULL) {
                    const u32 raw = static_cast<u32>(r.t_us - s.t_us);
                    const u32 hold = CurrentHoldUs(kAckDefaultHoldUs);
                    AddPrudpSample(r.node, r.t_us, raw > hold ? raw - hold : 0, true);
                }
                break;
            }
        }
    }
}

// ---- Tekken 3D Prime Edition (EUR): its own layer, neither Pia nor PRUDP. Found on a capture of an Azahar host against a real console (2026-11-12): every packet is
//   0  u8 0x02, u8 0x01   2  u8 sender node, u8 destination node   4  u16 1   6  u16 length of what follows the 16-byte header (so size = 16 + length)
//   8  8 zero bytes       16 u8 0x11, u8 0x11   18 u8 TYPE   19 u8 0   then the content
// The game state goes in packets of 281 bytes (one a frame, 60 a second, a frame counter at bytes 25-26 - no echo of the other side's counter: nothing to measure there)
// and every 2 s each side sends a SHORT packet (30 bytes in the match, 34 in the lobby) of TYPE 0xC4 / 0xC0 - a request - which the other game answers at once with a
// short packet of TYPE 0x94 / 0x90 (it measured 33 ms against the 281-byte frames' 60 a second, 16-50 ms). Round trip = my request leaving -> the reply of that node arriving.
// It includes the wait for the other game's next frame, like the other protocols' values. One sample every 2 s a node.
constexpr u32 kTekkenShortMax = 40; // the requests and replies are short (30, 34); the frames are 281, the other data 73-389

inline bool IsTekken(const Rec& r) {
    return r.len >= 24 && r.data[0] == 0x02 && r.data[1] == 0x01 && r.data[16] == 0x11 && r.data[17] == 0x11 && Le16(r.data + 6) + 16u == r.size;
}

u64 g_tk_req[9]; // when my last request to each node left (0 = none waiting)

void FeedTekken(const Rec& r) {
    const bool small = r.size <= kTekkenShortMax;
    const u8 type = r.data[18];
    if (r.dir == 'S') {
        const u8 dst = r.data[3];
        if (small && (type & 0xF0) == 0xC0 && dst >= 1 && dst <= 8) {
            g_tk_req[dst] = r.t_us;
        }
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    TouchNode(r.node, r.t_us); // (every packet of the node: it is listed even before a value)
    if (small && (type & 0xF0) == 0x90) {
        const u64 t_req = g_tk_req[r.node];
        if (t_req != 0 && r.t_us > t_req && r.t_us - t_req < 1000000ULL) {
            g_tk_req[r.node] = 0;
            AddPrudpSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_req), false);
        }
    }
}

// ---- Super Street Fighter IV 3D Edition (EUR): its own layer, neither Pia nor PRUDP. Found on two captures (2026-11-12/13): two Azahar instances on one PC (the real
// delay known) and an instance against a real console through a public room. Every message starts with
//   0  u16 size - 2    2  u16 size - 4    4  u8 TYPE    5  u8 flags (0x00 or 0xFF)    6  u32 the sender's packet number    10 u32 a checksum    14 ... the content
// (a packet now and then carries several messages and does not match: it is counted as unknown, which the lock allows). TYPE 0 = the game's data (60 a second:
// inputs and state; the equal values on the two sides come from both simulating the same frame - no echo there), 3 = ACK, 7 = a KEEPALIVE once a second,
// with the number of the keepalive it acknowledges (u16 at 20) and ITS OWN number (u16 at 22), sent again every ~110 ms until acknowledged. The other game
// answers a keepalive AT ONCE with an ACK (28 bytes, flags 0) carrying that number at 20 - when it next looks at its network: 0-30 ms later, a frame at 60 fps.
// Round trip = my keepalive leaving -> the ACK with its number arriving, minus the other game's hold, estimated with MY OWN hold (from the arrival of a keepalive
// of the node to the departure of my ACK of it), like the Tetris layer. A keepalive sent again gives no sample (which send does the ACK answer?). Measured on the
// two instances: 49.9 ms seen, my hold 16.7 ms, the real round trip 33.4 ms. One sample a second a node.
constexpr u32 kSf4DefaultHoldUs = 16000; // until three holds of my own are measured: a frame at 60 fps
constexpr u32 kSf4AckMax = 32;           // the ACK of a keepalive is 28 bytes; the bigger ACKs (flags 0xFF) answer other things

inline bool IsSf4(const Rec& r) {
    return r.len >= 24 && static_cast<u32>(Le16(r.data)) + 2u == r.size && static_cast<u32>(Le16(r.data + 2)) + 4u == r.size && r.data[4] <= 0x0F &&
           (r.data[5] == 0x00 || r.data[5] == 0xFF);
}

struct Sf4Tx {
    u64 t_us;
    u16 seq;
    u8 used;
    u8 dup;
};
struct Sf4Rx {
    u64 t_us;
    u16 seq;
    u8 used;
};
Sf4Tx g_sf_tx[8]; // my keepalives waiting for their ACK (one a second: 8 is plenty)
Sf4Rx g_sf_rx[8]; // keepalives of the other game waiting for my ACK (my hold)
int g_sf_tx_head = 0, g_sf_rx_head = 0;

void FeedSf4(const Rec& r) {
    const u8 type = r.data[4];
    if (r.data[5] != 0x00) {
        if (r.dir == 'R' && r.node >= 1 && r.node <= 8) {
            TouchNode(r.node, r.t_us);
        }
        return;
    }
    if (r.dir == 'S') {
        if (type == 7) { // my keepalive: its own number at 22
            const u16 seq = Le16(r.data + 22);
            for (int i = 0; i < 8; ++i) {
                if (g_sf_tx[i].used && g_sf_tx[i].seq == seq && r.t_us - g_sf_tx[i].t_us < 5000000ULL) {
                    g_sf_tx[i].dup = 1; // sent again
                    return;
                }
            }
            g_sf_tx[g_sf_tx_head] = {r.t_us, seq, 1, 0};
            g_sf_tx_head = (g_sf_tx_head + 1) % 8;
        } else if (type == 3 && r.size <= kSf4AckMax) { // my ACK of a keepalive of the other game: how long my game held it
            const u16 seq = Le16(r.data + 20);
            for (int i = 0; i < 8; ++i) {
                Sf4Rx& x = g_sf_rx[i];
                if (x.used && x.seq == seq) {
                    x.used = 0;
                    if (r.t_us > x.t_us && r.t_us - x.t_us < 500000ULL) {
                        g_hold_us[g_hold_head] = static_cast<u32>(r.t_us - x.t_us);
                        g_hold_head = (g_hold_head + 1) % 16;
                        if (g_hold_count < 16) ++g_hold_count;
                    }
                    break;
                }
            }
        }
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    TouchNode(r.node, r.t_us); // (every packet of the node: it is listed even before a value)
    if (type == 7) { // a keepalive of the node: I will acknowledge it
        g_sf_rx[g_sf_rx_head] = {r.t_us, Le16(r.data + 22), 1};
        g_sf_rx_head = (g_sf_rx_head + 1) % 8;
    } else if (type == 3 && r.size <= kSf4AckMax) { // the node acknowledges a keepalive of mine
        const u16 seq = Le16(r.data + 20);
        for (int i = 0; i < 8; ++i) {
            Sf4Tx& s = g_sf_tx[i];
            if (s.used && s.seq == seq) {
                s.used = 0;
                if (!s.dup && r.t_us > s.t_us && r.t_us - s.t_us < 3000000ULL) {
                    const u32 raw = static_cast<u32>(r.t_us - s.t_us);
                    const u32 hold = CurrentHoldUs(kSf4DefaultHoldUs);
                    AddPrudpSample(r.node, r.t_us, raw > hold ? raw - hold : 0, true);
                }
                break;
            }
        }
    }
}

// Protocol recognition, for the log only (ProtoSummary): counts the packets of each kind and keeps the first bytes of an
// unrecognised packet, sent and received, so that a new game can be looked at from a log sent by someone else.
struct ProtoStat {
    u32 pia, prudp, other;
    u32 ack;             // the "ack" layer (Tetris)
    u32 tk;              // Tekken 3D Prime Edition
    u32 sf;              // Super Street Fighter IV
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
    } else if (IsAckLayer(r)) {
        ++g_proto.ack;
    } else if (IsTekken(r)) {
        ++g_proto.tk;
    } else if (IsSf4(r)) {
        ++g_proto.sf;
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
enum { kProtoNone = 0, kProtoPia = 1, kProtoPrudp = 2, kProtoAck = 3, kProtoTk = 4, kProtoSf = 5 };
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
            const int kind = is_pia ? kProtoPia : (is_prudp ? kProtoPrudp : (IsAckLayer(r) ? kProtoAck : (IsTekken(r) ? kProtoTk : (IsSf4(r) ? kProtoSf : kProtoNone))));
            if (kind != kProtoNone && kind != g_lock_proto) {
                if (++g_lock_mismatch >= 8) {
                    g_lock_proto = kProtoNone;
                    g_lock_mismatch = 0;
                }
            } else if (kind == g_lock_proto) {
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
        } else if (g_lock_proto == kProtoAck && IsAckLayer(r)) {
            FeedAck(r);
        } else if (g_lock_proto == kProtoTk && IsTekken(r)) {
            FeedTekken(r);
        } else if (g_lock_proto == kProtoSf && IsSf4(r)) {
            FeedSf4(r);
        }
        return;
    }
    ClassifyProto(r);
    const int mo = FindMagic(r);
    if (mo >= 0) {
        FeedPia(r, mo);
    } else if (FindPrudp(r) >= 0) {
        FeedPrudp(r);
    } else if (IsAckLayer(r)) {
        FeedAck(r);
    } else if (IsTekken(r)) {
        FeedTekken(r);
    } else if (IsSf4(r)) {
        FeedSf4(r);
    }
    // the lock: 32 packets of one kind, the others fewer than a quarter of it (the unknown ones do not count: a game may mix a few in)
    const u32 c[5] = {g_proto.pia, g_proto.prudp, g_proto.ack, g_proto.tk, g_proto.sf};
    for (int k = 0; k < 5; ++k) {
        if (c[k] < kLockMin) {
            continue;
        }
        bool alone = true;
        for (int j = 0; j < 5 && alone; ++j) {
            alone = j == k || c[j] * 4 < c[k];
        }
        if (alone) {
            g_lock_proto = static_cast<u8>(kProtoPia + k);
            break;
        }
    }
}

void FeedPia(const Rec& r, int mo) {
    const u16 ts = Be16(r.data + mo + 8);
    if (r.dir == 'S') {
        g_last_sent = {ts, r.t_us};
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    TouchNode(r.node, r.t_us); // (back after a pause - a new match: starts without the old samples)
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
    // is "my clock now" minus the echo. My clock now is the timer of my last send plus the time since, on the platform clock.
    // (There was also a table of my last 1024 sends, to take the difference in microseconds when the echo was exactly one of them: 16 KB that changed
    // nothing - replayed on the captures of seven games the median was the same to the millisecond with and without it - so it is gone.)
    if (g_last_sent.t_us == 0 || r.t_us < g_last_sent.t_us || r.t_us - g_last_sent.t_us > 2000000ULL) {
        return; // no recent send of mine to derive my clock from
    }
    const u32 now_ts = (static_cast<u32>(g_last_sent.ts) + static_cast<u32>((r.t_us - g_last_sent.t_us) / 1000ULL)) & 0xFFFFu;
    const u32 diff_ms = (now_ts - echo) & 0xFFFFu;
    if (diff_ms > 2000) {
        return; // not an echo of a timestamp of mine
    }
    AddSample(StatOf(r.node), r.t_us, diff_ms * 1000u, false);
}

} // namespace

// >>> game-thread code (Tests/policy_check.sh)
void OnSend(u16 dest_node, u8 channel, const u8* data, u32 size) {
    if (!g_ring || !data || !size || dest_node > 255) {
        return;
    }
    Push('S', static_cast<u8>(dest_node), channel, size, data);
}

void OnRecv(u16 src_node, u8 channel, const u8* data, u32 size) {
    if (!g_ring || !data || !size || src_node > 255) {
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
    u32 mask = 0xFFFFFFFFu;
    int shown[9];
    u64 shown_ms[9];
    u8 spike[9];          // the node's line last said a spike
    u32 dropped = 0;      // g_dropped at the last "ring dropped" line
    u32 file_dropped = 0; // the file's own count at its last line
    u32 link = 0;         // g_link at the last "room link" line
    u64 link_ms = 0;
};
LogState g_log;

// This console's own link to the room, as 3DS Link measures it (ENet: round trip and its variance, reliable packets lost per thousand), handed over by
// the plugin with every window of its statistics (NoteLink). One word, written by that thread and read by the network thread:
// bit 31 = there is a value, 30..19 the round trip (ms, up to 4095), 18..10 its variance (up to 511), 9..0 the loss (0..1000).
u32 g_link = 0;

} // namespace

void SetSink(LatencyFile* file, u64 (*console_clock_ms)()) {
    g_file = file;
    g_clock_ms = console_clock_ms;
}

void SetNameSource(bool (*name_of)(int node, char* out, u32 cap)) {
    g_name_of = name_of;
}

void NoteLink(u32 rtt_ms, u32 rtt_var_ms, u32 loss_permille) {
    const u32 word = 0x80000000u | ((rtt_ms > 4095u ? 4095u : rtt_ms) << 19) | ((rtt_var_ms > 511u ? 511u : rtt_var_ms) << 10) |
                     (loss_permille > 1000u ? 1000u : loss_permille);
    __atomic_store_n(&g_link, word, __ATOMIC_RELAXED);
}

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
            g_log.spike[i] = 0;
        }
        g_log.last_ms = now_ms;
        g_log.proto_ms = now_ms;
        g_ring = ring;
        if (g_debug_level > 0 && g_file && g_clock_ms) { // the file says which day it is once, here: its lines only carry the time of day
            char day[24];
            FormatConsoleClock(g_clock_ms(), day);
            Emit("lat: ---- the console joined a lobby, console clock %s ----", day);
        }
    }
    Pump();
    if (g_debug_level <= 0 || now_ms - g_log.last_ms < 1000) {
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
        Emit("lat: nodes sending %X", static_cast<unsigned>(mask));
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
            // (the name the game gave the node - its UDS username, plain ASCII, never an address - so that who had which latency can be crossed with who did what)
            // (the last name the engine gave this node is kept: the engine forgets a node that left, and its last lines still say who it was)
            char* name = g_node_name[o.node];
            if (g_name_of != nullptr) {
                char fresh[sizeof(g_node_name[0])];
                fresh[0] = '\0';
                if (g_name_of(o.node, fresh, sizeof(fresh)) && fresh[0] != '\0') {
                    std::memcpy(name, fresh, sizeof(fresh));
                }
            }
            // what the value is made of: M measured (the game's own echo), A acknowledgements with the hold time removed, E estimated from the game
            // clock, S an old value. With M and A: the lowest and the 90th percentile of the window's half-seconds - a p90 far above the value is a
            // connection that jumps, while the value itself is steady by construction
            char detail[96];
            if (o.stale) {
                std::snprintf(detail, sizeof(detail), "old value, S");
            } else if (o.estimated) {
                std::snprintf(detail, sizeof(detail), "2 x %d ms one way, from the game clock, E", o.owd_ms);
            } else {
                std::snprintf(detail, sizeof(detail), "%d s: min %d, p90 %d; %d samples, %c", o.window_s, o.min_ms, o.p90_ms, o.samples, o.corrected ? 'A' : 'M');
            }
            if (name[0] != '\0') {
                Emit("lat: n%d \"%s\" %d ms (%s)", o.node, name, o.rtt_ms, detail);
            } else {
                Emit("lat: n%d %d ms (%s)", o.node, o.rtt_ms, detail);
            }
        }
        // the value shown is the last 2 s, not the window: said when it starts and when it ends (the lines above carry the value itself)
        if ((o.spike ? 1 : 0) != g_log.spike[o.node]) {
            g_log.spike[o.node] = o.spike ? 1 : 0;
            if (o.spike) {
                Emit("lat: n%d spike: the last 2 s are %d ms, the window %d ms (shown: the last 2 s)", o.node, o.recent_ms, o.window_ms);
            } else {
                Emit("lat: n%d spike over: back to the window's %d ms", o.node, o.rtt_ms);
            }
        }
    }
    // This console's own link to the room, next to the players' values: a player's latency that rises together with this line is this console's
    // connection; one that rises alone is the other player's (or the path between the two).
    const u32 link = __atomic_load_n(&g_link, __ATOMIC_RELAXED);
    if (link != 0 && (link != g_log.link || now_ms - g_log.link_ms >= 30000)) {
        g_log.link = link;
        g_log.link_ms = now_ms;
        Emit("lat: room link %u+-%u ms, loss %u/1000", static_cast<unsigned>((link >> 19) & 0xFFFu), static_cast<unsigned>((link >> 10) & 0x1FFu),
             static_cast<unsigned>(link & 0x3FFu));
    }
    // What was lost on the way to this file: packets the ring could not hold (they gave no sample) and lines the queue could not take.
    if (g_dropped != g_log.dropped) {
        g_log.dropped = g_dropped;
        Emit("lat: ring dropped %u packets in all (the network thread paused: they gave no sample)", static_cast<unsigned>(g_dropped));
    }
    if (g_file != nullptr && g_file->dropped() != g_log.file_dropped) {
        g_log.file_dropped = g_file->dropped();
        Emit("lat: %u lines of this file dropped in all", static_cast<unsigned>(g_log.file_dropped));
    }
    if (now_ms - g_log.proto_ms >= 30000) {
        g_log.proto_ms = now_ms;
        char pr[300];
        if (ProtoSummary(pr, sizeof(pr)) > 0) {
            Emit("lat: protocol %s", pr);
        }
    }
}

int Snapshot(NodeRtt out[8]) {
    const u64 now = NowUs();
    const u32 now_ms32 = static_cast<u32>(now / 1000ULL);
    int shown = 0;
    for (int node = 1; node <= 8; ++node) {
        NodeRtt& o = out[node - 1];
        o = NodeRtt();
        o.node = node;
        const NodeStat& n = StatOf(node);
        const u64 slot_us = SlotUs(n);
        const u32 now_number = static_cast<u32>(now / slot_us);
        o.window_s = static_cast<int>(slot_us * kSlots / 1000000ULL);
        // the values of the window: one a slot that has samples and is not older than kSlots slots, and the one being filled;
        // `old`: those and the older ones still kept (what is shown, marked as old, when the window is empty)
        u16 v[kSlots + 1];
        u16 old[kSlots + 1];
        u16 rec[kSlots + 1]; // the slots of the last 2 s (see kRecentUs)
        int c = 0;
        int oc = 0;
        int rc = 0;
        int samples = 0;
        int corrected = 0;
        const u32 recent_slots = static_cast<u32>(kRecentUs / slot_us) > 0 ? static_cast<u32>(kRecentUs / slot_us) : 1u;
        for (int i = 0; i < kSlots; ++i) {
            const Slot& s = n.slot[i];
            if (s.n == 0) {
                continue;
            }
            old[oc++] = s.ms;
            if (now_number - s.number < static_cast<u32>(kSlots)) {
                v[c++] = s.ms;
                samples += s.n;
                corrected += s.corrected;
                if (now_number - s.number < recent_slots) {
                    rec[rc++] = s.ms;
                }
            }
        }
        if (n.cur_n != 0) {
            u16 w[kSlotSamples];
            const int kept = n.cur_n < kSlotSamples ? n.cur_n : kSlotSamples;
            for (int i = 0; i < kept; ++i) w[i] = n.cur[i];
            const u16 m = MedianOf(w, kept);
            old[oc++] = m;
            if (now_number - n.cur_number < static_cast<u32>(kSlots)) {
                v[c++] = m;
                samples += n.cur_n;
                corrected += n.cur_corrected;
                if (now_number - n.cur_number < recent_slots) {
                    rec[rc++] = m;
                }
            }
        }
        // A line is shown while game packets arrive from the node (even if there are no samples yet: then "name --", as for the
        // clients of FF Explorers, which send little reliable data).
        if (n.last_rx_us != 0 && now - n.last_rx_us <= kAliveUs) {
            ++shown;
            o.age_ms = n.last_sample_us ? static_cast<int>((now - n.last_sample_us) / 1000) : 0;
            o.last_ms = n.has_last ? static_cast<int>(n.last_ms) : -1;
        }
        if (c > 0) {
            Sort(v, c);
            o.rtt_ms = static_cast<int>(v[c / 2]);
            o.window_ms = o.rtt_ms;
            o.min_ms = static_cast<int>(v[0]);
            o.p90_ms = static_cast<int>(v[(c * 9) / 10]);
            o.samples = samples;
            o.corrected = corrected * 2 > samples;
            if (rc > 0) { // the last 2 s on their own: shown instead of the window while they are clearly worse (kSpikeMinMs)
                Sort(rec, rc);
                o.recent_ms = static_cast<int>(rec[rc / 2]);
                if (o.recent_ms >= o.window_ms + kSpikeMinMs && o.recent_ms * 2 >= o.window_ms * 3) {
                    o.spike = true;
                    o.rtt_ms = o.recent_ms;
                }
            }
        } else if (o.age_ms >= 0 && oc > 0) {
            // No recent sample but the node is still connected (FF Explorers: a client sends reliable data only when it joins,
            // afterwards there is nothing to match). The last known value - the median of what is still kept - is shown, marked as old.
            Sort(old, oc);
            o.rtt_ms = static_cast<int>(old[oc / 2]);
            o.stale = true;
        }
        // One-way delay (game clock in the state packets): median of the samples of the last 8 s, one sample per new value of
        // the sender's clock (so the packets that repeat a value add no waiting). Checked with three instances on one PC
        // against the real delivery delay (the same packets seen by sender and receiver): within ~5 ms on all six directions.
        // The 5th percentile was off by 3-16 ms, differently from node to node.
        if (g_owd != nullptr && o.age_ms >= 0) {
            int d[kOwdSamples];
            int dc = 0;
            const OwdSample* owd = OwdOf(node);
            for (int i = 0; i < g_owd_count[node]; ++i) {
                if (now_ms32 - owd[i].t_ms <= kOwdWindowMs) d[dc++] = owd[i].d_ms;
            }
            if (dc >= kOwdMinSamples) {
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
            o.spike = false; // (the game clock's 8 s window follows a change by itself)
        }
    }
    return shown;
}

int ProtoSummary(char* buf, int cap) {
    const u32 total = g_proto.pia + g_proto.prudp + g_proto.ack + g_proto.tk + g_proto.sf + g_proto.other;
    if (total == 0 || total == g_proto.logged_total) {
        return 0;
    }
    g_proto.logged_total = total;
    // the name: the kind with most packets, if it has more than the unknown ones
    static const char* const kNames[5] = {"Pia", "PRUDP v1", "AckTag", "Tekken", "SSF4"};
    const u32 c[5] = {g_proto.pia, g_proto.prudp, g_proto.ack, g_proto.tk, g_proto.sf};
    int best = 0;
    for (int k = 1; k < 5; ++k) {
        if (c[k] > c[best]) {
            best = k;
        }
    }
    const char* name = c[best] > 0 && c[best] > g_proto.other ? kNames[best] : "unknown";
    // compact: "Pia,<pia>,<prudp>,<other>" and, if there are unknown packets, ",U<S|R>,<size>,<first bytes in hex>"
    // after the lock the counts grow by 1 every 64 packets (a check sample), not by every packet
    static const char* const kLockNames[6] = {"-", "Pia", "PRUDP", "AckTag", "Tekken", "SSF4"};
    int len = std::snprintf(buf, cap, "%s,%u,%u,%u,lock=%s,ack=%u,tk=%u,sf=%u", name, static_cast<unsigned>(g_proto.pia),
                            static_cast<unsigned>(g_proto.prudp), static_cast<unsigned>(g_proto.other), kLockNames[g_lock_proto <= kProtoSf ? g_lock_proto : 0],
                            static_cast<unsigned>(g_proto.ack), static_cast<unsigned>(g_proto.tk), static_cast<unsigned>(g_proto.sf));
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
