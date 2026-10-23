#include "azl/log.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "azl/platform.hpp"
#include "azl/printf_lite.hpp"
#include "azl/stats.hpp"

namespace azl {

namespace {

platform::Mutex g_log_mutex;
LogSink g_sink;
u32 g_log_busy_drops = 0; // lines given up because g_log_mutex was busy (atomic adds; LogTakeBusyDrops clears)

} // namespace

Stats g_stats;
u32 g_cmd_counts[kStatsCommands];

// ---- debug capture ring --------------------------------------------------------------------
// The master switch of the log (see stats.hpp): 0 no log of any kind, 1 everything.
volatile int g_debug_level = 0;

namespace {
constexpr u32 kDebugRingBytes = 512 * 1024;
// The last 32 KB are for the control lane only (stats.hpp): a flood of frames fills the ring up to here and no further.
constexpr u32 kControlReserveBytes = 32 * 1024;
constexpr u32 kHeavyLimit = kDebugRingBytes - kControlReserveBytes;
constexpr u32 kDebugMaxPayload = 1600; // a UDS data frame is at most 0x5C6 bytes
constexpr u32 kLostRecordBytes = 32;   // the header and four words
// A producer that finds the ring's lock taken tries again this many times (about 30 us on the console: more than the copy
// of a whole 1.6 KB frame by another producer, which is the longest the lock is held, and than the few instructions the
// writer holds it for) and then gives the record up: the writer can be preempted with the lock in its hand (by a thread
// above it; until 2026-10-20 it was the lowest priority and the game's threads could), and a game thread must not wait for it.
constexpr u32 kLockSpins = 256;
// The diet of the IPC lines (stats.hpp, kDbgIpcAgg).
constexpr u32 kDietSlots = 32;       // (a power of two)
constexpr u32 kDietSlotBits = 5;
constexpr u32 kDietProbe = 4;
constexpr u32 kDietWindowMs = 1000;  // one count line per second and kind ...
constexpr u32 kDietFullCalls = 3;    // ... after the first three calls of that second

// The layout DebugIpc writes by hand (little-endian words: stored, kind, dir | ms | src, dest | size, channel, pad)
static_assert(offsetof(DebugRecord, stored) == 0 && offsetof(DebugRecord, kind) == 2 && offsetof(DebugRecord, dir) == 3 &&
                  offsetof(DebugRecord, ms) == 4 && offsetof(DebugRecord, src) == 8 && offsetof(DebugRecord, dest) == 10 &&
                  offsetof(DebugRecord, size) == 12 && offsetof(DebugRecord, channel) == 14 && offsetof(DebugRecord, pad) == 15,
              "DebugRecord: the byte layout DebugIpc writes");

u8* g_dbg_ring = nullptr;
u32 g_dbg_head = 0; // next byte to write
u32 g_dbg_tail = 0; // next byte to read
u32 g_dbg_used = 0;
u32 g_dbg_dropped = 0;
platform::Mutex g_dbg_mutex;

// The records a lane has lost since the last marker, and what the marker will say.
struct Lost {
    u32 full;     // dropped: the lane had no room
    u32 busy;     // dropped: the lock was busy (folded in from g_dbg_miss)
    u32 first_ms;
    u32 last_ms;
};
Lost g_dbg_lost[2]; // by lane
// Producers that could not get the lock: counted without it, folded into g_dbg_lost by whoever holds the lock next.
u32 g_dbg_miss[2];
u32 g_dbg_miss_first[2];
u32 g_dbg_miss_last[2];
u32 g_dbg_miss_any;
CaptureTotals g_dbg_totals;

struct DietSlot {
    u32 handle;
    u32 header;
    u32 window_ms; // start of the second this kind is in
    u32 calls;     // calls in it (the first kDietFullCalls were written whole)
    u32 pending;   // calls counted in this second and not yet written
    u32 first_ms;  // the first and the last of those
    u32 last_ms;
    u32 last_arg;
    u32 done;          // calls counted in seconds that are over, handed to the writer thread and not yet written
    u32 done_first_ms; // the first and the last of those
    u32 done_last_ms;
    u32 done_arg;
};
DietSlot g_dbg_diet[kDietSlots];

void RingWrite(const void* data, u32 n) {
    const u8* p = static_cast<const u8*>(data);
    const u32 first = std::min<u32>(n, kDebugRingBytes - g_dbg_head);
    std::memcpy(g_dbg_ring + g_dbg_head, p, first);
    if (n > first) {
        std::memcpy(g_dbg_ring, p + first, n - first);
    }
    g_dbg_head = (g_dbg_head + n) % kDebugRingBytes;
    g_dbg_used += n;
}

// Copies n bytes out of the ring, starting at `from`. The caller holds the lock, or knows nobody writes there.
void RingCopyOut(void* data, u32 from, u32 n) {
    u8* p = static_cast<u8*>(data);
    const u32 first = std::min<u32>(n, kDebugRingBytes - from);
    std::memcpy(p, g_dbg_ring + from, first);
    if (n > first) {
        std::memcpy(p + first, g_dbg_ring, n - first);
    }
}

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
// Everything from here to the end marker is what a producer of the capture runs, and the producers include the svc hook's
// pass-through (every IPC call of every thread of the game): on whatever stack the thread has, so no printf family, no
// buffer, no heap, and in the lines of the hook no call at all (not even a memcpy frame): byte by byte, in line.

// The ring's lock for a producer: a few microseconds of trying, then never.
inline bool TryLockBriefly(platform::Mutex& mutex) {
    for (u32 i = 0; i < kLockSpins; ++i) {
        if (mutex.try_lock()) {
            return true;
        }
    }
    return false;
}

// The log's lock for a line (Log(), LogLine): tried briefly like the ring's. A thread that does not get it gives the line up,
// counted (the writer says so in the log), unless it IS the writer, which waits. A LightLock has no priority inheritance: with
// the writer at the lowest priority (until 2026-10-20) a thread of the game that waited for a lock the preempted writer held
// waited for every thread above the writer to leave the core, and a game that spins at its splash screen never does (Tekken,
// 2026-10-20: stuck at the publisher's screen with the log on, nothing stuck with it off). The writer runs above the game's
// threads since the same day (Plugin::StartLogWriter), which takes that preemption away; this is the belt to those braces.
inline bool LockLogBriefly() {
    if (TryLockBriefly(g_log_mutex)) {
        return true;
    }
    if (platform::IsLogWriterThread()) {
        g_log_mutex.lock();
        return true;
    }
    __atomic_fetch_add(&g_log_busy_drops, 1u, __ATOMIC_RELAXED);
    return false;
}

// A record given up for the lock (any thread, no lock held): counted in the atomics, folded in by the next producer or the
// writer that holds the lock.
inline void NoteMiss(u32 lane, u32 now) {
    if (__atomic_fetch_add(&g_dbg_miss[lane], 1, __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&g_dbg_miss_first[lane], now, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_dbg_miss_last[lane], now, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dbg_miss_any, 1, __ATOMIC_RELAXED);
}

// g_dbg_mutex held.
__attribute__((noinline)) void FoldMissesLocked() {
    __atomic_store_n(&g_dbg_miss_any, 0, __ATOMIC_RELAXED);
    for (u32 lane = 0; lane < 2; ++lane) {
        const u32 n = __atomic_exchange_n(&g_dbg_miss[lane], 0, __ATOMIC_RELAXED);
        if (n == 0) {
            continue;
        }
        Lost& lost = g_dbg_lost[lane];
        if (lost.full == 0 && lost.busy == 0) {
            lost.first_ms = __atomic_load_n(&g_dbg_miss_first[lane], __ATOMIC_RELAXED);
        }
        lost.busy += n;
        lost.last_ms = __atomic_load_n(&g_dbg_miss_last[lane], __ATOMIC_RELAXED);
        g_dbg_dropped += n;
        g_dbg_totals.lost_busy[lane] += n;
    }
}

// Byte by byte, in line, with no call at all (not even a memcpy frame): the header and `n` (at most 32) bytes at the head
// of the ring. g_dbg_mutex held and the room checked.
__attribute__((always_inline)) inline void PutSmall(const DebugRecord& rec, const void* payload, u32 n) {
    u32 head = g_dbg_head;
    const u8* parts[2] = {reinterpret_cast<const u8*>(&rec), static_cast<const u8*>(payload)};
    const u32 sizes[2] = {static_cast<u32>(sizeof(DebugRecord)), n};
    for (int part = 0; part < 2; ++part) {
        for (u32 i = 0; i < sizes[part]; ++i) {
            g_dbg_ring[head] = parts[part][i];
            if (++head == kDebugRingBytes) {
                head = 0;
            }
        }
    }
    g_dbg_head = head;
    g_dbg_used += static_cast<u32>(sizeof(DebugRecord)) + n;
    if (g_dbg_used > g_dbg_totals.ring_peak) {
        g_dbg_totals.ring_peak = g_dbg_used;
    }
}

// The same for the one record that every IPC call of every thread of the game makes (DebugIpc): the 16 bytes of the header
// and the four words straight from registers into the ring, with no record and no array on the stack - the thread of Tri
// Force Heroes that receives the exit notification has 464 bytes and the frame of this function is part of what it pays.
__attribute__((always_inline)) inline void PutByte(u32& head, u32 value) {
    g_dbg_ring[head] = static_cast<u8>(value);
    if (++head == kDebugRingBytes) {
        head = 0;
    }
}
__attribute__((always_inline)) inline void PutWord(u32& head, u32 value) {
    PutByte(head, value);
    PutByte(head, value >> 8);
    PutByte(head, value >> 16);
    PutByte(head, value >> 24);
}

// The marker of a hole in a lane, written ahead of the first record that goes in after it (it is a control record: the
// room for it was checked by the caller).
// (The words go straight into the ring, like DebugIpc's: this runs on the thread of whichever producer finds the hole, and
// the one of Tri Force Heroes' notification thread - 464 bytes - may be it. No record and no array on the stack.)
__attribute__((noinline)) void EmitLostLocked(u32 lane, u32 now) {
    Lost& lost = g_dbg_lost[lane];
    u32 head = g_dbg_head;
    PutWord(head, 16u | (static_cast<u32>(kDbgLost) << 16) | (lane << 24)); // stored, kind, dir (the lane that lost them)
    PutWord(head, now);                                                      // ms
    PutWord(head, 0);                                                        // src, dest
    PutWord(head, static_cast<u32>(kLaneControl) << 24);                     // size, channel, pad (the marker is a control record)
    PutWord(head, lost.full);
    PutWord(head, lost.busy);
    PutWord(head, lost.first_ms);
    PutWord(head, lost.last_ms);
    g_dbg_head = head;
    g_dbg_used += static_cast<u32>(sizeof(DebugRecord)) + 4 * sizeof(u32);
    if (g_dbg_used > g_dbg_totals.ring_peak) {
        g_dbg_totals.ring_peak = g_dbg_used;
    }
    ++g_dbg_totals.records[kLaneControl];
    ++g_dbg_totals.markers;
    lost.full = lost.busy = lost.first_ms = lost.last_ms = 0;
}

// Does a record of `need` bytes go into its lane? g_dbg_mutex held. If not, it is counted as lost; if it does and the lane
// has lost records before it, their marker is written first.
__attribute__((always_inline)) inline bool AdmitLocked(u32 lane, u32 need, u32 now) {
    if (__atomic_load_n(&g_dbg_miss_any, __ATOMIC_RELAXED) != 0) {
        FoldMissesLocked();
    }
    Lost& lost = g_dbg_lost[lane];
    const bool marker = (lost.full | lost.busy) != 0;
    const u32 limit = lane == kLaneHeavy ? kHeavyLimit : kDebugRingBytes;
    if (g_dbg_used + (marker ? kLostRecordBytes : 0) + need > limit) {
        if (!marker) {
            lost.first_ms = now;
        }
        ++lost.full;
        lost.last_ms = now;
        ++g_dbg_dropped;
        ++g_dbg_totals.lost_full[lane];
        return false;
    }
    if (marker) {
        EmitLostLocked(lane, now);
    }
    return true;
}

// g_dbg_mutex held. The payload is `first` followed by `second` (`rec.stored` bytes in all); for the records of a few words
// that the hook stores (PushSmallLocked) there is a version that calls nothing.
void PushLocked(const DebugRecord& rec, const void* first, u32 first_size, const void* second = nullptr,
                u32 second_size = 0) {
    if (!AdmitLocked(rec.pad == kLaneHeavy ? kLaneHeavy : kLaneControl, sizeof(DebugRecord) + rec.stored, rec.ms)) {
        return;
    }
    RingWrite(&rec, sizeof(rec));
    if (first_size) {
        RingWrite(first, first_size);
    }
    if (second_size) {
        RingWrite(second, second_size);
    }
    ++g_dbg_totals.records[rec.pad == kLaneHeavy ? kLaneHeavy : kLaneControl];
    if (g_dbg_used > g_dbg_totals.ring_peak) {
        g_dbg_totals.ring_peak = g_dbg_used;
    }
}

// The hook's IPC lines are a record of a few words stored by whatever thread of the game made the call, on a stack that
// may be a few hundred bytes. g_dbg_mutex held, `n` is at most 32.
__attribute__((always_inline)) inline bool PushSmallLocked(const DebugRecord& rec, const void* payload, u32 n) {
    const u32 lane = rec.pad == kLaneHeavy ? kLaneHeavy : kLaneControl;
    if (!AdmitLocked(lane, static_cast<u32>(sizeof(DebugRecord)) + n, rec.ms)) {
        return false;
    }
    PutSmall(rec, payload, n);
    ++g_dbg_totals.records[lane];
    return true;
}

// The count line of a kind of IPC call whose seconds are over (the diet): the calls handed over by the game threads
// (`done`) and, when `with_pending`, those of the second that is still open. Only the writer thread calls this - a game
// thread that finds its second over hands the count over in the slot and goes on - so the stack it needs is the writer's.
// g_dbg_mutex held.
__attribute__((noinline)) void PushAggregateLocked(DietSlot& slot, u32 now, bool with_pending) {
    DebugRecord rec;
    rec.stored = 7 * sizeof(u32);
    rec.kind = kDbgIpcAgg;
    rec.dir = 0;
    rec.ms = now;
    rec.src = rec.dest = rec.size = 0;
    rec.channel = 0;
    rec.pad = kLaneControl; // (a count stands for many calls, at most a line per kind and second: it never goes missing for want of room)
    const u32 count = slot.done + (with_pending ? slot.pending : 0);
    const u32 first = slot.done ? slot.done_first_ms : slot.first_ms;
    const u32 last = with_pending ? slot.last_ms : slot.done_last_ms;
    const u32 arg = with_pending ? slot.last_arg : slot.done_arg;
    const u32 words[7] = {slot.handle, slot.header, arg, 0, count, first, last};
    if (PushSmallLocked(rec, words, sizeof(words))) {
        ++g_dbg_totals.agg_lines;
        g_dbg_totals.agg_calls += count;
    } else {
        // The control lane is full and the line is lost (AdmitLocked counted it as one lost record): the calls it stood for are
        // lost with it, and the LOST marker has to say so - `count` calls, not one record.
        g_dbg_lost[kLaneControl].full += count - 1;
        g_dbg_dropped += count - 1;
        g_dbg_totals.lost_full[kLaneControl] += count - 1;
    }
    slot.done = 0;
    if (with_pending) {
        slot.pending = 0;
    }
}

// Has the second that began at `since` ended at `now`? (the two clocks are read before the lock, by different threads: a
// `since` a little in the future is not a second that ended)
inline bool SecondOver(u32 now, u32 since) {
    return static_cast<s32>(now - since) >= static_cast<s32>(kDietWindowMs);
}

// The diet: true when the call (result 0) is only counted. g_dbg_mutex held. A kind (session handle and command) written
// whole the first three times of every second, then one line for the rest of that second; a table with no free slot writes
// everything whole.
__attribute__((always_inline)) inline bool DietLocked(u32 handle, u32 header, u32 arg, u32 now) {
    if (handle == 0) {
        return false;
    }
    const u32 index = ((handle * 0x9E3779B1u) ^ (header * 0x85EBCA77u)) >> (32 - kDietSlotBits);
    DietSlot* slot = nullptr;
    DietSlot* spare = nullptr;
    for (u32 probe = 0; probe < kDietProbe; ++probe) {
        DietSlot& s = g_dbg_diet[(index + probe) & (kDietSlots - 1)];
        if (s.handle == handle && s.header == header) {
            slot = &s;
            break;
        }
        if (!spare && (s.handle == 0 || (s.pending == 0 && s.done == 0 && SecondOver(now, s.window_ms)))) {
            spare = &s;
        }
    }
    if (!slot) {
        if (!spare) {
            return false;
        }
        slot = spare;
        slot->handle = handle;
        slot->header = header;
        slot->window_ms = now;
        slot->calls = 0;
        slot->pending = 0;
        slot->done = 0;
    }
    if (SecondOver(now, slot->window_ms)) {
        if (slot->pending) {
            // The count of the second that ended is handed over, not written: a game thread stores a few words here and
            // pushes no record for it (the writer thread turns it into the line within a quarter of a second).
            if (slot->done == 0) {
                slot->done_first_ms = slot->first_ms;
            }
            slot->done += slot->pending;
            slot->done_last_ms = slot->last_ms;
            slot->done_arg = slot->last_arg;
        }
        slot->window_ms = now;
        slot->calls = 0;
        slot->pending = 0;
    }
    if (++slot->calls <= kDietFullCalls) {
        return false;
    }
    if (slot->pending++ == 0) {
        slot->first_ms = now;
    }
    slot->last_ms = now;
    slot->last_arg = arg;
    return true;
}

void DebugPush(const DebugRecord& rec, const void* payload) {
    if (!g_dbg_ring) {
        return;
    }
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(rec.pad == kLaneHeavy ? kLaneHeavy : kLaneControl, rec.ms);
        return;
    }
    PushLocked(rec, payload, rec.stored);
    g_dbg_mutex.unlock();
}

inline DebugRecord MakeRecord(u8 kind, u8 lane) {
    DebugRecord rec;
    rec.stored = 0;
    rec.kind = kind;
    rec.dir = 0;
    rec.ms = static_cast<u32>(platform::NowMs());
    rec.src = rec.dest = rec.size = 0;
    rec.channel = 0;
    rec.pad = lane;
    return rec;
}
// <<< game-thread code
} // namespace

bool DebugInit() {
    if (g_dbg_ring) {
        return true;
    }
    g_dbg_ring = new (std::nothrow) u8[kDebugRingBytes];
    return g_dbg_ring != nullptr;
}

bool DebugRingReady() {
    return g_dbg_ring != nullptr;
}

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
void DebugFrame(u8 dir, u16 src, u16 dest, u8 channel, const u8* payload, u32 size) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    DebugRecord rec = MakeRecord(kDbgFrame, kLaneHeavy);
    rec.dir = dir;
    rec.src = src;
    rec.dest = dest;
    rec.size = static_cast<u16>(size > 0xFFFF ? 0xFFFF : size);
    rec.channel = channel;
    rec.stored = static_cast<u16>(payload ? std::min<u32>(size, kDebugMaxPayload) : 0);
    DebugPush(rec, payload);
}
// <<< game-thread code

// A line of the capture made with the formats of Log() / DebugEvent(): newlib's vsnprintf chain needs ~1.4 KB of stack (the
// HLE proper calls these on the game's UDS thread), so the conversions the plugin uses are written by azl/printf_lite.hpp
// and only a format with something else in it goes to newlib. The line is cut at size - 1 characters, as snprintf cuts it.
static u32 FormatLine(char* out, u32 size, const char* fmt, va_list args) {
    u32 n;
    if (printf_lite::Supported(fmt)) {
        n = printf_lite::Format(out, size - 1, fmt, args);
    } else {
        const int r = std::vsnprintf(out, size, fmt, args);
        n = r < 0 ? 0 : std::min<u32>(static_cast<u32>(r), size - 1);
    }
    out[n] = 0;
    return n;
}

namespace {
char g_dbg_line[200]; // (the capture's text lines were cut at 199 characters)
} // namespace

// The line lives in the static buffer, under the ring's lock, in a frame of its own: a caller that logs with the capture off
// pays the few words of the variadic entry (the HLE proper calls DebugEvent on the game's threads at debug=0).
__attribute__((noinline)) static void DebugEventV(const char* fmt, va_list args) {
    DebugRecord rec = MakeRecord(kDbgText, kLaneControl);
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(kLaneControl, rec.ms);
        return;
    }
    rec.stored = static_cast<u16>(FormatLine(g_dbg_line, sizeof(g_dbg_line), fmt, args));
    PushLocked(rec, g_dbg_line, rec.stored);
    g_dbg_mutex.unlock();
}

void DebugEvent(const char* fmt, ...) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    DebugEventV(fmt, args);
    va_end(args);
}

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
// The hook's two pass-through lines are stored as raw words and formatted by the writer (DebugFormat): the game thread
// that makes the IPC call does no formatting at all.
void DebugIpc(u32 handle, u32 header, u32 arg, u32 result) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    const u32 now = static_cast<u32>(platform::NowMs());
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(kLaneHeavy, now);
        return;
    }
    // The diet: a call that succeeds and that this session has already made three times this second is only counted.
    if ((result != 0 || !DietLocked(handle, header, arg, now)) &&
        AdmitLocked(kLaneHeavy, static_cast<u32>(sizeof(DebugRecord)) + 4 * sizeof(u32), now)) {
        u32 head = g_dbg_head;
        PutWord(head, (4 * sizeof(u32)) | (static_cast<u32>(kDbgIpc) << 16)); // stored, kind, dir
        PutWord(head, now);                                                      // ms
        PutWord(head, 0);                                                        // src, dest
        PutWord(head, static_cast<u32>(kLaneHeavy) << 24);                       // size, channel, pad
        PutWord(head, handle);
        PutWord(head, header);
        PutWord(head, arg);
        PutWord(head, result);
        g_dbg_head = head;
        g_dbg_used += static_cast<u32>(sizeof(DebugRecord)) + 4 * sizeof(u32);
        ++g_dbg_totals.records[kLaneHeavy];
        if (g_dbg_used > g_dbg_totals.ring_peak) {
            g_dbg_totals.ring_peak = g_dbg_used;
        }
    }
    g_dbg_mutex.unlock();
}

namespace {
struct SvcRealPayload {
    u32 command;
    u32 arg;
    u32 result;
    char service[12];
};
} // namespace

void DebugSvcReal(const char* service, u32 command, u32 arg, u32 result) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    DebugRecord rec = MakeRecord(kDbgSvcReal, kLaneHeavy);
    rec.stored = sizeof(SvcRealPayload);
    SvcRealPayload payload;
    payload.command = command;
    payload.arg = arg;
    payload.result = result;
    bool more = service != nullptr; // (strncpy by hand: the zero padding must not become a memset call)
    for (u32 n = 0; n < sizeof(payload.service); ++n) {
        const char c = more && n < sizeof(payload.service) - 1 ? service[n] : 0;
        more = more && c != 0;
        payload.service[n] = c;
    }
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(kLaneHeavy, rec.ms);
        return;
    }
    PushSmallLocked(rec, &payload, sizeof(payload));
    g_dbg_mutex.unlock();
}

DebugLine::DebugLine() : LineBuf(g_dbg_line, sizeof(g_dbg_line) - 1), locked_(false) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        Deactivate();
        return;
    }
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(kLaneControl, static_cast<u32>(platform::NowMs()));
        Deactivate(); // (the line is lost and counted: a game thread does not wait for the writer)
        return;
    }
    locked_ = true;
}

DebugLine::~DebugLine() {
    if (!locked_) {
        return;
    }
    DebugRecord rec = MakeRecord(kDbgText, kLaneControl);
    rec.stored = static_cast<u16>(size());
    PushLocked(rec, data(), rec.stored);
    g_dbg_mutex.unlock();
}

namespace {
constexpr u32 kMaxCommandWords = 63; // the IPC command buffer has 64 words and the header is one
constexpr u32 kMaxCommandHead = 90;  // (a head of 91 characters or more used to wreck the line: cut at 90)

// The 16 bytes in front of a kDbgCommand's words; what follows is the head's text (kCmdText only), the request words, the
// reply words.
enum CommandTag : u8 { kCmdUds = 0, kCmdNdm = 1, kCmdPull = 2, kCmdText = 3 };
struct CommandHead {
    u8 tag;
    u8 request_words;
    u8 reply_words;
    u8 text_len;
    u32 a; // kCmdUds / kCmdNdm: the command id; kCmdPull: the bind
    u32 b; // kCmdPull: the bytes of the frame
    u32 c; // kCmdPull: the node it came from
};
static_assert(sizeof(CommandHead) == 16, "CommandHead layout");

// The commands a game may call at frame rate or in a spin (PullPacket, SendTo, the status and node reads, the scans):
// their lines are the heavy lane's. The rest of the UDS commands, and all of ndm:u, change state or happen a few times a
// launch: the control lane's.
bool HotUdsCommand(u32 id) {
    switch (id) {
    case 0x0B: // GetConnectionStatus
    case 0x0C:
    case 0x0D: // GetNodeInformation
    case 0x0F: // RecvBeaconBroadcastData
    case 0x14: // PullPacket
    case 0x17: // SendTo
    case 0x22: // ScanOnConnection
        return true;
    }
    return false;
}

// One command of the HLE proper as raw words: the caller formats nothing and holds the lock for a copy.
void PushCommand(u8 lane, CommandHead head, const char* text, const u32* request, const u32* reply) {
    head.request_words = static_cast<u8>(std::min(static_cast<u32>(head.request_words), kMaxCommandWords));
    head.reply_words = static_cast<u8>(std::min(static_cast<u32>(head.reply_words), kMaxCommandWords));
    DebugRecord rec = MakeRecord(kDbgCommand, lane);
    rec.stored = static_cast<u16>(sizeof(head) + head.text_len + 4 * (head.request_words + head.reply_words));
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(lane, rec.ms);
        return;
    }
    if (AdmitLocked(lane, sizeof(DebugRecord) + rec.stored, rec.ms)) {
        RingWrite(&rec, sizeof(rec));
        RingWrite(&head, sizeof(head));
        if (head.text_len) {
            RingWrite(text, head.text_len);
        }
        if (head.request_words) {
            RingWrite(request, 4u * head.request_words);
        }
        if (head.reply_words) {
            RingWrite(reply, 4u * head.reply_words);
        }
        ++g_dbg_totals.records[lane];
        if (g_dbg_used > g_dbg_totals.ring_peak) {
            g_dbg_totals.ring_peak = g_dbg_used;
        }
    }
    g_dbg_mutex.unlock();
}
} // namespace

void DebugCommand(const char* head, const u32* request, u32 request_words, const u32* reply,
                  u32 reply_words) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    CommandHead h = {};
    h.tag = kCmdText;
    h.request_words = static_cast<u8>(std::min(request_words, kMaxCommandWords));
    h.reply_words = static_cast<u8>(std::min(reply_words, kMaxCommandWords));
    u32 length = 0;
    while (head && head[length] && length < kMaxCommandHead) {
        ++length;
    }
    h.text_len = static_cast<u8>(length);
    PushCommand(kLaneControl, h, head, request, reply);
}

void DebugServiceCommand(bool uds, u32 id, const u32* request, u32 request_words, const u32* reply, u32 reply_words) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    CommandHead h = {};
    h.tag = uds ? kCmdUds : kCmdNdm;
    h.request_words = static_cast<u8>(std::min(request_words, kMaxCommandWords));
    h.reply_words = static_cast<u8>(std::min(reply_words, kMaxCommandWords));
    h.a = id;
    PushCommand(uds && HotUdsCommand(id) ? kLaneHeavy : kLaneControl, h, nullptr, request, reply);
}

void DebugPullPacket(u32 bind, u32 bytes, u32 node, const u32* request, u32 request_words, const u32* reply,
                     u32 reply_words) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    CommandHead h = {};
    h.tag = kCmdPull;
    h.request_words = static_cast<u8>(std::min(request_words, kMaxCommandWords));
    h.reply_words = static_cast<u8>(std::min(reply_words, kMaxCommandWords));
    h.a = bind;
    h.b = bytes;
    h.c = node;
    PushCommand(kLaneHeavy, h, nullptr, request, reply);
}

// The capture line of an 802.11 frame that is not game data. The engine sends its Deauthentications from the thread that
// ends a session - the game's, when it closes its fake nwm::UDS session or the console goes to sleep (UdsHle::NoteHandleClosed,
// Plugin::OnSleepEnter) - so the line is built by hand, with the MACs in capitals as the printf format wrote them.
void DebugWifiLine(const char* dir, u32 type, u32 bytes, u32 channel, const u8* transmitter, const u8* destination) {
    DebugLine line;
    line.Str("wifi ").Str(dir).Str(" type ").Dec(type).Ch(' ').Dec(bytes).Str("B ch").Dec(channel).Str(" from ");
    for (u32 i = 0; i < 6; ++i) {
        line.Hex(transmitter[i], 2, true).Ch(i < 5 ? ':' : ' ');
    }
    line.Str("to ");
    for (u32 i = 0; i < 6; ++i) {
        line.Hex(destination[i], 2, true);
        if (i < 5) {
            line.Ch(':');
        }
    }
}

void DebugBeacon(u8 dir, const u8* transmitter_mac, u8 channel, const u8* body, u32 size) {
    if (g_debug_level <= 0 || !g_dbg_ring) {
        return;
    }
    DebugRecord rec = MakeRecord(kDbgBeacon, kLaneHeavy);
    rec.dir = dir;
    rec.size = static_cast<u16>(size > 0xFFFF ? 0xFFFF : size);
    rec.channel = channel;
    const u32 keep = body ? std::min<u32>(size, kDebugMaxPayload - 6) : 0;
    static const u8 kNoMac[6] = {};
    rec.stored = static_cast<u16>(6 + keep);
    if (!TryLockBriefly(g_dbg_mutex)) {
        NoteMiss(kLaneHeavy, rec.ms);
        return;
    }
    PushLocked(rec, transmitter_mac ? transmitter_mac : kNoMac, 6, body, keep);
    g_dbg_mutex.unlock();
}
// <<< game-thread code

// The writer's side, one thread at a time (LogFiles::DrainDebug). It holds the lock for the header of a record and again
// for moving the tail, not for the copy of the bytes: nobody writes between the tail and the head (a producer writes at the
// head and drops what does not fit), so the payload is copied without it and a producer that arrives meanwhile does not
// find the lock taken for the 15 microseconds a 1.6 KB copy takes on the console. It waits for the lock: it has all the time.
bool DebugPop(DebugRecord* rec, u8* buf, u32 cap) {
    if (!g_dbg_ring) {
        return false;
    }
    u32 tail;
    {
        platform::LockGuard lock(g_dbg_mutex);
        if (g_dbg_used < sizeof(DebugRecord)) {
            return false;
        }
        tail = g_dbg_tail;
        RingCopyOut(rec, tail, sizeof(*rec)); // (a record is whole in the ring when the lock is free: producers write it under it)
    }
    const u32 total = static_cast<u32>(sizeof(DebugRecord)) + rec->stored;
    const u32 take = std::min<u32>(rec->stored, cap);
    if (take) {
        RingCopyOut(buf, (tail + static_cast<u32>(sizeof(DebugRecord))) % kDebugRingBytes, take);
    }
    {
        platform::LockGuard lock(g_dbg_mutex);
        g_dbg_tail = (tail + total) % kDebugRingBytes;
        g_dbg_used -= total;
    }
    rec->stored = static_cast<u16>(take);
    return true;
}

bool DebugOldestMs(u32* ms) {
    if (!g_dbg_ring) {
        return false;
    }
    platform::LockGuard lock(g_dbg_mutex);
    if (g_dbg_used < sizeof(DebugRecord)) {
        return false;
    }
    DebugRecord rec;
    RingCopyOut(&rec, g_dbg_tail, sizeof(rec));
    *ms = rec.ms;
    return true;
}

void DebugFlushAggregates(bool final) {
    if (!g_dbg_ring) {
        return;
    }
    const u32 now = static_cast<u32>(platform::NowMs());
    platform::LockGuard lock(g_dbg_mutex);
    for (DietSlot& slot : g_dbg_diet) {
        const bool with_pending = slot.pending != 0 && (final || SecondOver(now, slot.window_ms));
        if (slot.done != 0 || with_pending) {
            PushAggregateLocked(slot, now, with_pending);
        }
    }
}

u32 DebugBytesQueued() {
    return __atomic_load_n(&g_dbg_used, __ATOMIC_RELAXED);
}

CaptureTotals DebugCaptureTotals() {
    platform::LockGuard lock(g_dbg_mutex);
    if (__atomic_load_n(&g_dbg_miss_any, __ATOMIC_RELAXED) != 0) {
        FoldMissesLocked();
    }
    return g_dbg_totals;
}

void DebugReset() {
    platform::LockGuard lock(g_dbg_mutex);
    g_dbg_head = g_dbg_tail = g_dbg_used = g_dbg_dropped = 0;
    std::memset(g_dbg_lost, 0, sizeof(g_dbg_lost));
    std::memset(g_dbg_miss, 0, sizeof(g_dbg_miss));
    std::memset(g_dbg_miss_first, 0, sizeof(g_dbg_miss_first));
    std::memset(g_dbg_miss_last, 0, sizeof(g_dbg_miss_last));
    g_dbg_miss_any = 0;
    std::memset(&g_dbg_totals, 0, sizeof(g_dbg_totals));
    std::memset(g_dbg_diet, 0, sizeof(g_dbg_diet));
}

namespace {

// " xxxxxxxx": a word of a command line, as the hook-side formatter wrote it.
void AppendWordText(std::string* out, u32 word) {
    static const char kDigits[] = "0123456789abcdef";
    out->push_back(' ');
    for (int shift = 28; shift >= 0; shift -= 4) {
        out->push_back(kDigits[(word >> shift) & 0xF]);
    }
}

// "12.345": a time of the capture, relative to the start of the plugin (the time column of the lines is "%6lu.%03lu").
void AppendRelative(std::string* out, u32 ms, u32 start_ms) {
    char text[24];
    const u32 rel = ms - start_ms;
    std::snprintf(text, sizeof(text), "%lu.%03lu", static_cast<unsigned long>(rel / 1000),
                  static_cast<unsigned long>(rel % 1000));
    *out += text;
}

} // namespace

void DebugFormat(const DebugRecord& rec, const u8* payload, u32 start_ms, std::string* out) {
    static const char* const kDirs[] = {"tx", "rx", "other", "dup", "echo"};
    static const char* const kDigits = "0123456789abcdef";
    const u32 rel = rec.ms - start_ms;
    char line[160];
    auto hex = [&](u32 from) {
        // In one go: the hex of a 1.5 KB frame was 3,000 appends of one char (a capacity check each). The writer's CPU is
        // the lowest priority there is, but what it does not format in time is what the ring drops (2026-10-20).
        const u32 n = rec.stored > from ? rec.stored - from : 0;
        const std::size_t at = out->size();
        out->resize(at + 2 * static_cast<std::size_t>(n) + 1);
        char* p = &(*out)[at];
        for (u32 i = 0; i < n; ++i) {
            *p++ = kDigits[payload[from + i] >> 4];
            *p++ = kDigits[payload[from + i] & 15];
        }
        *p = '\n';
    };
    auto stamp = [&]() {
        std::snprintf(line, sizeof(line), "%6lu.%03lu ", static_cast<unsigned long>(rel / 1000),
                      static_cast<unsigned long>(rel % 1000));
        *out += line;
    };
    if (rec.kind == kDbgText) {
        stamp();
        out->append(reinterpret_cast<const char*>(payload), rec.stored);
        *out += '\n';
    } else if (rec.kind == kDbgIpc && rec.stored >= 4 * sizeof(u32)) {
        // The raw words of DebugIpc(): the text the hook used to format itself, byte for byte.
        u32 w[4];
        std::memcpy(w, payload, sizeof(w));
        std::snprintf(line, sizeof(line), "%6lu.%03lu ipc h=0x%lx cmd=0x%08lx arg=0x%lx -> 0x%08lx",
                      static_cast<unsigned long>(rel / 1000), static_cast<unsigned long>(rel % 1000),
                      static_cast<unsigned long>(w[0]), static_cast<unsigned long>(w[1]),
                      static_cast<unsigned long>(w[2]), static_cast<unsigned long>(w[3]));
        *out += line;
        *out += '\n';
    } else if (rec.kind == kDbgIpcAgg && rec.stored >= 7 * sizeof(u32)) {
        // The count of a kind of IPC call (the diet): the line of the last call of the second, then how many calls there were
        // in all after the first three and when the first and the last of them came.
        u32 w[7];
        std::memcpy(w, payload, sizeof(w));
        std::snprintf(line, sizeof(line), "%6lu.%03lu ipc h=0x%lx cmd=0x%08lx arg=0x%lx -> 0x%08lx x%lu from ",
                      static_cast<unsigned long>(rel / 1000), static_cast<unsigned long>(rel % 1000),
                      static_cast<unsigned long>(w[0]), static_cast<unsigned long>(w[1]),
                      static_cast<unsigned long>(w[2]), static_cast<unsigned long>(w[3]),
                      static_cast<unsigned long>(w[4]));
        *out += line;
        AppendRelative(out, w[5], start_ms);
        *out += " to ";
        AppendRelative(out, w[6], start_ms);
        *out += '\n';
    } else if (rec.kind == kDbgSvcReal && rec.stored >= sizeof(SvcRealPayload)) {
        SvcRealPayload p;
        std::memcpy(&p, payload, sizeof(p));
        p.service[sizeof(p.service) - 1] = 0;
        std::snprintf(line, sizeof(line), "%6lu.%03lu svc REAL %s cmd 0x%04lx arg 0x%lx -> 0x%08lx",
                      static_cast<unsigned long>(rel / 1000), static_cast<unsigned long>(rel % 1000), p.service,
                      static_cast<unsigned long>(p.command), static_cast<unsigned long>(p.arg),
                      static_cast<unsigned long>(p.result));
        *out += line;
        *out += '\n';
    } else if (rec.kind == kDbgCommand && rec.stored >= sizeof(CommandHead)) {
        // The raw words of a UDS / ndm command: the head, " req <words> -> rep <words>", all as the hook used to write them.
        CommandHead h;
        std::memcpy(&h, payload, sizeof(h));
        char head[kMaxCommandHead + 8];
        LineBuf text(head, kMaxCommandHead);
        const u8* at = payload + sizeof(h);
        const u8* end = payload + rec.stored;
        switch (h.tag) {
        case kCmdUds:
        case kCmdNdm:
            text.Str(h.tag == kCmdUds ? "uds" : "ndm").Str(" cmd 0x").Hex(h.a, 2);
            break;
        case kCmdPull:
            text.Str("uds PullPacket bind ").Dec(h.a).Str(": ").Dec(h.b).Str(" bytes from node ").Dec(h.c);
            break;
        default: {
            const u32 n = std::min<u32>(h.text_len, static_cast<u32>(end - at));
            for (u32 i = 0; i < n; ++i) {
                text.Ch(static_cast<char>(at[i]));
            }
            at += n;
            break;
        }
        }
        if (h.tag != kCmdText) {
            at += h.text_len;
        }
        stamp();
        out->append(head, text.size());
        *out += " req";
        for (u32 i = 0; i < h.request_words && at + 4 <= end; ++i, at += 4) {
            AppendWordText(out, LoadLE32(at));
        }
        *out += " -> rep";
        for (u32 i = 0; i < h.reply_words && at + 4 <= end; ++i, at += 4) {
            AppendWordText(out, LoadLE32(at));
        }
        *out += '\n';
    } else if (rec.kind == kDbgLost && rec.stored >= 4 * sizeof(u32)) {
        // A hole in a lane: how many records, when, and why.
        u32 w[4];
        std::memcpy(w, payload, sizeof(w));
        stamp();
        std::snprintf(line, sizeof(line), "LOST %lu %s records from ", static_cast<unsigned long>(w[0] + w[1]),
                      rec.dir == kLaneHeavy ? "heavy" : "control");
        *out += line;
        AppendRelative(out, w[2], start_ms);
        *out += " to ";
        AppendRelative(out, w[3], start_ms);
        std::snprintf(line, sizeof(line), " (ring full %lu, lock busy %lu)\n", static_cast<unsigned long>(w[0]),
                      static_cast<unsigned long>(w[1]));
        *out += line;
    } else if (rec.kind == kDbgBeacon && rec.stored >= 6) {
        // The transmitter MAC leads the stored bytes. A keyword of its own, so scripts that read
        // FRAME lines never meet these.
        const u8* mac = payload;
        std::snprintf(line, sizeof(line), "%6lu.%03lu BEACON %-5s %02x:%02x:%02x:%02x:%02x:%02x ch%u %uB%s: ",
                      static_cast<unsigned long>(rel / 1000), static_cast<unsigned long>(rel % 1000),
                      kDirs[rec.dir <= 4 ? rec.dir : 2], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      rec.channel, rec.size, rec.size + 6u > rec.stored ? " (cut)" : "");
        *out += line;
        hex(6);
    } else {
        auto node = [](u16 n) { return n == 0xFFFF ? std::string("all") : std::to_string(n); };
        // (a header-only record - other, dup, echo - holds no bytes of the frame on purpose: not "cut")
        std::snprintf(line, sizeof(line), "%6lu.%03lu FRAME %-5s %s>%s ch%u %uB%s: ",
                      static_cast<unsigned long>(rel / 1000), static_cast<unsigned long>(rel % 1000),
                      kDirs[rec.dir <= 4 ? rec.dir : 2], node(rec.src).c_str(), node(rec.dest).c_str(),
                      rec.channel, rec.size, rec.stored != 0 && rec.size > rec.stored ? " (cut)" : "");
        *out += line;
        hex(0);
    }
}

u32 DebugTakeDropped() {
    platform::LockGuard lock(g_dbg_mutex);
    if (__atomic_load_n(&g_dbg_miss_any, __ATOMIC_RELAXED) != 0) {
        FoldMissesLocked();
    }
    const u32 n = g_dbg_dropped;
    g_dbg_dropped = 0;
    return n;
}

void StatsSocCall(int kind, u64 ticks) {
    if (!platform::IsNetThread()) {
        return; // (the statistics are those of the network thread; the PC tests run a room server in the same process, with this ENet layer)
    }
    ++g_stats.soc_calls[kind];
    StatsAdd64(g_stats.soc_ticks[kind], ticks);
    const u32 shown = ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(ticks);
    if (shown > g_stats.soc_max_ticks[kind]) {
        g_stats.soc_max_ticks[kind] = shown;
    }
    if (ticks > 20ull * AZL_TICKS_PER_MS) {
        ++g_stats.soc_over20[kind];
        if (ticks > 100ull * AZL_TICKS_PER_MS) {
            ++g_stats.soc_over100[kind];
        }
    }
    // The network thread was blocked on the soc process for that long: not its CPU (the loop takes it off its sections).
    platform::NoteNetWait(ticks);
}

Stats TakeStats() {
    Stats out;
    // The counters: a copy and a clear (a count lost now and then does not matter) ...
    constexpr std::size_t kCounterBytes = offsetof(Stats, t_net_service);
    std::memcpy(&out, &g_stats, kCounterBytes);
    std::memset(&g_stats, 0, kCounterBytes);
    // ... the time sums, each read and cleared in one atomic step: an add that lands meanwhile is in this window or in the
    // next, and the halves of a sum are never read apart.
    const auto take = [](Ticks64& sum) { return __atomic_exchange_n(&sum, u64(0), __ATOMIC_RELAXED); };
    out.t_net_service = take(g_stats.t_net_service);
    out.t_net_events = take(g_stats.t_net_events);
    out.t_net_send = take(g_stats.t_net_send);
    out.t_cb = take(g_stats.t_cb);
    out.t_writer = take(g_stats.t_writer);
    out.ipc_sum_ticks = take(g_stats.ipc_sum_ticks);
    out.t_net_wall = take(g_stats.t_net_wall);
    out.t_net_pause = take(g_stats.t_net_pause);
    out.t_writer_wait = take(g_stats.t_writer_wait);
    out.lock_wait_ticks = take(g_stats.lock_wait_ticks);
    out.lock_wait_net_ticks = take(g_stats.lock_wait_net_ticks);
    out.lock_hold_ticks = take(g_stats.lock_hold_ticks);
    for (int k = 0; k < kSocKinds; ++k) {
        out.soc_ticks[k] = take(g_stats.soc_ticks[k]);
    }
    out.frame_gap_sum = take(g_stats.frame_gap_sum);
    return out;
}

void StatsRoomDatagram(u32 bytes, u64 gap_ticks) {
    ++g_stats.rx_datagrams;
    ++g_stats.drain_dgrams_cur;
    g_stats.drain_bytes_cur += bytes;
    // The room's relay holds datagrams back now and then (130-275 ms seen); a hold shows as a gap here,
    // followed by a burst. The histogram is the long gaps only, and 2 s or more is a quiet room, not a hold.
    if (gap_ticks >= 2000ull * AZL_TICKS_PER_MS) {
        return;
    }
    static const u32 kBounds[] = {50, 100, 150, 250, 500};
    for (int i = 4; i >= 0; --i) {
        if (gap_ticks >= static_cast<u64>(kBounds[i]) * AZL_TICKS_PER_MS) {
            ++g_stats.gap_hist[i];
            break;
        }
    }
}

u32 StatsServiceCall(u32 ticks) {
    if (ticks > g_stats.service_max_ticks) {
        g_stats.service_max_ticks = ticks;
    }
    const u32 datagrams = g_stats.drain_dgrams_cur;
    if (datagrams > g_stats.drain_dgrams_max) {
        g_stats.drain_dgrams_max = datagrams;
    }
    if (g_stats.drain_bytes_cur > g_stats.drain_bytes_max) {
        g_stats.drain_bytes_max = g_stats.drain_bytes_cur;
    }
    g_stats.drain_dgrams_cur = 0;
    g_stats.drain_bytes_cur = 0;
    return datagrams;
}

unsigned long PerSecond10(u64 ticks, u64 window_ms) {
    return static_cast<unsigned long>(ticks * 10000 / AZL_TICKS_PER_MS / (window_ms ? window_ms : 1));
}

CpuNumbers MakeCpuNumbers(const Stats& s, u64 window_ms) {
    CpuNumbers c;
    // The network thread: its sums are already exclusive (the loop cuts the soc calls, the engine's lock and its pauses
    // out of every section as it goes).
    c.enet10 = static_cast<u32>(PerSecond10(s.t_net_service, window_ms));
    c.frames10 = static_cast<u32>(PerSecond10(s.t_net_events, window_ms));
    c.queue10 = static_cast<u32>(PerSecond10(s.t_net_send, window_ms));
    c.net10 = c.enet10 + c.frames10 + c.queue10;
    // The game's threads inside the commands: they wait for the engine's lock when the network thread holds it. The lock
    // counts the waits of the network thread apart, so what is left is the game threads' (taken off, never below zero).
    const u64 game_lock = s.lock_wait_ticks > s.lock_wait_net_ticks ? s.lock_wait_ticks - s.lock_wait_net_ticks : 0;
    c.uds10 = static_cast<u32>(PerSecond10(s.ipc_sum_ticks > game_lock ? s.ipc_sum_ticks - game_lock : 0, window_ms));
    c.frame10 = static_cast<u32>(PerSecond10(s.t_cb, window_ms));
    c.writer10 = static_cast<u32>(PerSecond10(s.t_writer, window_ms));
    // (the writer's figure is wall time of a thread that mostly waits for the core: shown apart, not added)
    c.plugin10 = c.net10 + c.uds10 + c.frame10;

    c.soc10 = static_cast<u32>(PerSecond10(s.soc_ticks[kSocRecvData] + s.soc_ticks[kSocRecvEmpty] + s.soc_ticks[kSocSend], window_ms));
    c.poll10 = static_cast<u32>(PerSecond10(s.soc_ticks[kSocPoll], window_ms));
    c.lock10 = static_cast<u32>(PerSecond10(s.lock_wait_ticks, window_ms));
    c.pause10 = static_cast<u32>(PerSecond10(s.t_net_pause, window_ms));
    c.sd10 = static_cast<u32>(PerSecond10(s.t_writer_wait, window_ms));
    c.wall10 = static_cast<u32>(PerSecond10(s.t_net_wall, window_ms));
    // What the loop's wall time holds beyond its CPU and the waits the loop noted: a thread that was preempted inside a section
    // is charged for it as CPU, so what is left is rounding and time outside the sections.
    c.netlock10 = static_cast<u32>(PerSecond10(s.lock_wait_net_ticks, window_ms));
    const u32 accounted = c.net10 + c.soc10 + c.poll10 + c.pause10 + c.netlock10;
    c.other10 = c.wall10 > accounted ? c.wall10 - accounted : 0;
    return c;
}

StatusNumbers MakeStatusNumbers(const Stats& s, u64 dt_ms, u32 rtt_ms, u32 loss_permille, int soc_ring_bytes) {
    StatusNumbers n;
    if (dt_ms == 0) {
        return n;
    }
    n.valid = true;
    n.window_ms = static_cast<u32>(dt_ms);
    n.fps10 = static_cast<u32>(static_cast<u64>(s.frames) * 10000 / dt_ms);
    n.fps_min10 = s.frame_gap_max ? static_cast<u32>(static_cast<u64>(AZL_TICKS_PER_MS) * 10000 / s.frame_gap_max) : 0;

    // The same numbers as the "ms per second" log line: CPU only (the soc calls and the poll are the network thread
    // waiting, not working).
    const CpuNumbers cpu = MakeCpuNumbers(s, dt_ms);
    n.net10 = cpu.net10;
    n.uds10 = cpu.uds10;
    n.frame10 = cpu.frame10;
    n.writer10 = cpu.writer10;
    n.plugin10 = cpu.plugin10;
    n.loop_pause_ms = s.loop_gap_max;

    // The soc calls the network thread waited for (recv and send; the poll is its idle time): mean and longest.
    u64 calls = 0;
    u64 sum = 0;
    u32 longest = 0;
    for (int k = 0; k < kSocKinds; ++k) {
        if (k == kSocPoll) {
            continue;
        }
        calls += s.soc_calls[k];
        sum += s.soc_ticks[k];
        longest = longest > s.soc_max_ticks[k] ? longest : s.soc_max_ticks[k];
        n.soc_over20 += s.soc_over20[k];
        n.soc_over100 += s.soc_over100[k];
    }
    n.soc_mean_us = calls ? static_cast<u32>(sum / calls / 268) : 0;
    n.soc_max_us = longest / 268;

    n.ring_peak_bytes = s.ring_peak_units * 32;
    n.ring_lost = s.ring_overwritten;
    n.tx_stale = s.tx_stale_drops;
    n.tx_cap = s.tx_cap_drops;
    n.twin_candidates = s.fwd_candidates;
    n.twin_dropped = s.drop_fwd_dup;
    n.rtt_ms = rtt_ms;
    n.loss_permille = loss_permille;
    n.soc_ring_kb = soc_ring_bytes > 0 ? soc_ring_bytes / 1024 : -1;
    return n;
}

namespace {

/// snprintf onto the end of a text that is built piece by piece (stops at the end of the buffer).
void AppendText(char* text, std::size_t size, int* n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void AppendText(char* text, std::size_t size, int* n, const char* fmt, ...) {
    if (*n < 0 || static_cast<std::size_t>(*n) >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int added = std::vsnprintf(text + *n, size - *n, fmt, args);
    va_end(args);
    if (added > 0) {
        *n = std::min<int>(*n + added, static_cast<int>(size) - 1);
    }
}

unsigned long UL(u32 v) {
    return static_cast<unsigned long>(v);
}

} // namespace

std::string FormatStatusNumbers(const StatusNumbers& n) {
    if (!n.valid) {
        return "Numbers: the first window closes a few seconds after the start";
    }
    char text[480];
    int len = 0;
    // (lines of at most 44 characters: that is the width of the framework's message box)
    AppendText(text, sizeof(text), &len, "Last %lu s: %lu.%lu fps (slowest %lu.%lu)\n", UL((n.window_ms + 500) / 1000),
               UL(n.fps10 / 10), UL(n.fps10 % 10), UL(n.fps_min10 / 10), UL(n.fps_min10 % 10));
    AppendText(text, sizeof(text), &len, "Plugin CPU ms/s %lu.%lu: net %lu.%lu, uds %lu.%lu,\n", UL(n.plugin10 / 10),
               UL(n.plugin10 % 10), UL(n.net10 / 10), UL(n.net10 % 10), UL(n.uds10 / 10), UL(n.uds10 % 10));
    AppendText(text, sizeof(text), &len, "  frame %lu.%lu, log %lu.%lu wall\n", UL(n.frame10 / 10), UL(n.frame10 % 10),
               UL(n.writer10 / 10), UL(n.writer10 % 10));
    AppendText(text, sizeof(text), &len, "Net loop: longest pause %lu ms\n", UL(n.loop_pause_ms));
    AppendText(text, sizeof(text), &len, "soc calls: mean %lu.%lu, max %lu.%lu ms\n", UL(n.soc_mean_us / 1000),
               UL(n.soc_mean_us % 1000 / 100), UL(n.soc_max_us / 1000), UL(n.soc_max_us % 1000 / 100));
    AppendText(text, sizeof(text), &len, "  over 20 ms: %lu, over 100 ms: %lu\n", UL(n.soc_over20), UL(n.soc_over100));
    char ring[24] = "?";
    if (n.soc_ring_kb >= 0) {
        std::snprintf(ring, sizeof(ring), "%d KB", n.soc_ring_kb);
    }
    AppendText(text, sizeof(text), &len, "Ring: peak %lu B, lost %lu, soc %s\n", UL(n.ring_peak_bytes), UL(n.ring_lost),
               ring);
    AppendText(text, sizeof(text), &len, "TX not sent: %lu old, %lu queue full\n", UL(n.tx_stale), UL(n.tx_cap));
    AppendText(text, sizeof(text), &len, "Twins: %lu seen, %lu dropped\n", UL(n.twin_candidates), UL(n.twin_dropped));
    AppendText(text, sizeof(text), &len, "Room: rtt %lu ms, loss %lu/1000", UL(n.rtt_ms), UL(n.loss_permille));
    return std::string(text, static_cast<std::size_t>(len));
}

u32 LogTakeBusyDrops() {
    return __atomic_exchange_n(&g_log_busy_drops, 0u, __ATOMIC_RELAXED);
}

void SetLogSink(LogSink sink) {
    platform::LockGuard lock(g_log_mutex);
    g_sink = std::move(sink);
}

namespace {
/// Where a line goes when nobody has set a sink: stderr on the host (the tests), nowhere on the console.
void NoSink(const char* line) {
#ifndef __3DS__
    std::fprintf(stderr, "%s\n", line);
#else
    (void)line;
#endif
}
} // namespace

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
namespace {
char g_log_line[400]; // (the same 400 characters Log() cuts a line at)
} // namespace

LogLine::LogLine() : LineBuf(g_log_line, sizeof(g_log_line) - 1), locked_(false) {
    if (g_debug_level <= 0) {
        Deactivate(); // debug=0: no log at all, so nothing is built, locked or queued
        return;
    }
    if (!LockLogBriefly()) {
        Deactivate(); // (the lock is busy: the line is given up and counted, the thread does not wait)
        return;
    }
    locked_ = true;
}

LogLine::~LogLine() {
    if (!locked_) {
        return;
    }
    g_log_line[size()] = 0;
    if (g_sink) {
        g_sink(g_log_line);
    } else {
        NoSink(g_log_line);
    }
    g_log_mutex.unlock();
}
// <<< game-thread code

// The 400-byte line lives in the static buffer, under the log's lock, in a frame of its own (see DebugEventV): with the log
// off, Log() is a few words of stack, and with it on the HLE proper, which logs on the game's threads, no longer pays for
// newlib's vsnprintf (azl/printf_lite.hpp writes the conversions of the plugin's formats).
__attribute__((noinline)) static void LogV(const char* fmt, va_list args) {
    if (!LockLogBriefly()) {
        return; // (busy: given up and counted)
    }
    FormatLine(g_log_line, sizeof(g_log_line), fmt, args);
    if (g_sink) {
        g_sink(g_log_line);
    } else {
        NoSink(g_log_line);
    }
    g_log_mutex.unlock();
}

void Log(const char* fmt, ...) {
    if (g_debug_level <= 0) {
        return; // debug=0: no log at all, so nothing is formatted, locked or queued
    }
    va_list args;
    va_start(args, fmt);
    LogV(fmt, args);
    va_end(args);
}

} // namespace azl

// For the C sources (the ENet platform layer): statistics of the soc calls and of the room datagrams.
extern "C" void azl_c_soc_call(int kind, unsigned long long ticks) {
    azl::StatsSocCall(kind, ticks);
}

extern "C" void azl_c_room_datagram(unsigned bytes, unsigned long long gap_ticks) {
    azl::StatsRoomDatagram(bytes, gap_ticks);
}

// For the C sources (the ENet platform layer): what SO_RCVBUF the soc service really gave the room
// socket (asked: after the clamp; got: the readback, -1 if it could not be read). The plugin shows a
// shortfall on screen.
namespace azl {
int g_rcvbuf_asked = 0;
int g_rcvbuf_got = -1;
} // namespace azl
extern "C" void azl_c_rcvbuf_result(int asked, int got) {
    azl::g_rcvbuf_asked = asked;
    azl::g_rcvbuf_got = got;
}

// For the C sources (the ENet platform layer): same sink, printf style.
extern "C" void azl_c_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void azl_c_log(const char* fmt, ...) {
    if (azl::g_debug_level <= 0) {
        return;
    }
    char line[200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    azl::Log("%s", line);
}
