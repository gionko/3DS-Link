#include "ctr/uds_hle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "azl/log.hpp"
#include "azl/stats.hpp"

namespace azl {
namespace ctr {

namespace {

// srv:GetServiceHandle request header (command 5, 4 normal words).
constexpr u32 kSrvGetServiceHandleHeader = 0x00050100;

constexpr std::size_t kNetworkInfoSize = sizeof(NetworkInfo);
constexpr std::size_t kDeprecatedNetworkInfoSize = 0x3C;
constexpr u16 kDeprecatedVersion = 0x100; // what the deprecated Initialize (0x01) stands for
constexpr u16 kChannelMaskAll = 0x07FF;   // channels 1 to 11, valid in every region
constexpr u32 kMaxNdmAnomalyLogs = 8;     // ndm:u requests with a header the firmware refuses: lines in the log

inline u32 Hdr(u32 id, unsigned normal, unsigned translate) {
    return IPC_MakeHeader(static_cast<u16>(id), normal, translate);
}

/// Writes a reply made only of the result code.
inline void Reply(u32* cmd, u32 id, u32 result) {
    cmd[0] = Hdr(id, 1, 0);
    cmd[1] = result;
}

/// The "output static buffer 0" the calling thread prepared at TLS+0x180 (libctru sets it before
/// calls like PullPacket). A real service would write into it through the kernel.
struct OutStaticBuffer {
    u8* ptr;
    u32 capacity;
};
inline OutStaticBuffer GetOutStaticBuffer() {
    const u32* sb = getThreadStaticBuffers();
    return {reinterpret_cast<u8*>(sb[1]), sb[0] >> 14};
}

// Words after the header that an IPC header declares (normal + translate), capped at what the
// 64-word command buffer can hold after the header.
inline u32 DeclaredWords(u32 header) {
    return std::min<u32>(((header >> 6) & 0x3F) + (header & 0x3F), 63);
}

// Scans, binds, connects and status reads are rare (a few per minute) and are what a post-mortem
// needs when the debug capture was off: every call gets a line. A game that polls one of them gets
// a burst of 8 and then two lines a second, each saying how many calls were left out before it.
int EveryCallSlot(u32 id) {
    switch (id) {
    case 0x0B:
        return 0;
    case 0x0F:
        return 1;
    case 0x12:
        return 2;
    case 0x13:
        return 3;
    case 0x1E:
        return 4;
    }
    return -1;
}

bool EveryCallWanted(int slot, u32* skipped) {
    struct Bucket {
        bool primed;
        u32 tokens;
        u32 last_ms;
        u32 skipped;
    };
    static Bucket buckets[5]; // lost updates between game threads only blur the counts
    Bucket& b = buckets[slot];
    const u32 now = static_cast<u32>(platform::NowMs());
    if (!b.primed) {
        b.primed = true;
        b.tokens = 8;
        b.last_ms = now;
    }
    const u32 earned = (now - b.last_ms) / 500;
    if (earned) {
        b.tokens = std::min<u32>(8, b.tokens + earned);
        b.last_ms += earned * 500;
    }
    if (b.tokens == 0) {
        ++b.skipped;
        return false;
    }
    --b.tokens;
    *skipped = b.skipped;
    b.skipped = 0;
    return true;
}

// Per-command bookkeeping for the log: the first time each (command, result) pair shows up, and
// the first few times a failing one does. Games poll some commands thousands of times a minute,
// so anything more would flood the log.
void NoteCommand(const char* service, u32 id, u32 result) {
    if (g_debug_level <= 0) {
        return; // the counts and the first-call lines are the log's: with it off a command costs nothing here
    }
    const bool is_uds = service[0] == 'u';
    if (is_uds && id < kStatsCommands) {
        ++g_cmd_counts[id]; // (the window's count: LogStats zeroes it)
        static bool said[kStatsCommands]; // (never cleared: the line below is said once per command in the life of the plugin)
        if (!said[id]) {
            said[id] = true;
            // (once per command: the thread of the game that calls it and its priority, against the network thread's 0x2F:
            // a game thread of higher priority preempts the network thread whenever it is ready)
            s32 priority = 0;
            u32 thread_id = 0;
            svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
            svcGetThreadId(&thread_id, CUR_THREAD_HANDLE);
            Log("uds: command 0x%02lx is first called by thread %lu at priority 0x%02lx", static_cast<unsigned long>(id),
                static_cast<unsigned long>(thread_id), static_cast<unsigned long>(priority));
        }
    }
    const int slot = is_uds ? EveryCallSlot(id) : -1;
    if (slot >= 0) {
        u32 skipped = 0;
        if (EveryCallWanted(slot, &skipped)) {
            if (skipped) {
                Log("%s: command 0x%02lx -> 0x%08lx (call %lu, %lu left out before)", service,
                    static_cast<unsigned long>(id), static_cast<unsigned long>(result),
                    static_cast<unsigned long>(g_cmd_counts[id]), static_cast<unsigned long>(skipped));
            } else {
                Log("%s: command 0x%02lx -> 0x%08lx (call %lu)", service, static_cast<unsigned long>(id),
                    static_cast<unsigned long>(result), static_cast<unsigned long>(g_cmd_counts[id]));
            }
        }
        return;
    }
    struct Seen {
        u32 key;
        u32 count;
    };
    static Seen seen[48];
    static int used = 0;
    // The two services share command numbers: keep their "first call" lines apart.
    const u32 key = ((id << 24) ^ result) ^ (is_uds ? 0u : 0x80000000u);
    Seen* entry = nullptr;
    for (int i = 0; i < used; ++i) {
        if (seen[i].key == key) {
            entry = &seen[i];
            break;
        }
    }
    if (!entry && used < 48) {
        entry = &seen[used++];
        entry->key = key;
        entry->count = 0;
    }
    if (!entry) {
        return;
    }
    ++entry->count;
    const u32 limit = result == 0 ? 1 : 4;
    if (entry->count <= limit) {
        Log("%s: command 0x%02lx -> 0x%08lx%s", service, static_cast<unsigned long>(id),
            static_cast<unsigned long>(result), result == 0 ? " (first call)" : "");
    }
}

} // namespace

UdsHle::UdsHle(UdsEngine* engine) : engine_(engine) {
    for (int i = 0; i < kMaxFakeHandles; ++i) {
        fake_[i] = 0;
        fake_kind_[i] = 0;
        fake_initialized_[i] = 0;
        fake_ndm_[i] = -1;
    }
    // An ndm:u request names its sender by a PID word the kernel fills in, and the hook serves the
    // request before the kernel does: whatever sits in that word is not a PID. Every request is this
    // process' own.
    u32 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    ndm_.SetSelfPid(pid, false);
    Log("ndm: the fake ndm:u serves the requests as process id %lu", static_cast<unsigned long>(pid));
}

u32 UdsHle::ndm_header_anomalies() {
    platform::LockGuard lock(mutex_);
    return ndm_.header_anomalies();
}

// The front of the IPC front-end - the lookup, the close, the GetServiceHandle and the capture of a command - runs on
// the GAME's threads before (or instead of) any command is served, on whatever stack the thread that made the call
// has (see hook.cpp): no printf family, no Log()/DebugEvent() with a format and no buffer over 64 bytes between the
// markers (Tests/policy_check.sh). What Run() reaches - Dispatch, the engine, NdmHle - is the HLE proper, which
// legitimately needs more stack (Tests/chain_sim documents how much).
// >>> game-thread code (Tests/policy_check.sh)
u8 UdsHle::FakeKind(Handle handle, int* ndm_session) {
    if (handle == 0) {
        return 0;
    }
    for (int i = 0; i < kMaxFakeHandles; ++i) {
        if (fake_[i] == handle) {
            if (ndm_session) {
                *ndm_session = fake_ndm_[i];
            }
            return fake_kind_[i];
        }
    }
    return 0;
}

void UdsHle::MarkInitialized(Handle handle) {
    platform::LockGuard lock(mutex_);
    for (int i = 0; i < kMaxFakeHandles; ++i) {
        if (fake_[i] == handle) {
            fake_initialized_[i] = 1;
        }
    }
}

bool UdsHle::ForgetFakeHandle(Handle handle) {
    bool initialized = false;
    platform::LockGuard lock(mutex_);
    for (int i = 0; i < kMaxFakeHandles; ++i) {
        if (fake_[i] == handle) {
            initialized |= fake_initialized_[i] != 0;
            const int ndm_session = fake_ndm_[i];
            if (ndm_session >= 0 && --ndm_handles_[ndm_session] == 0) {
                ndm_.CloseSession(ndm_session); // releases what the session held
            }
            fake_[i] = 0;
            fake_kind_[i] = 0;
            fake_initialized_[i] = 0;
            fake_ndm_[i] = -1;
        }
    }
    return initialized;
}

// The " (ndm session N)" / " (all 6 ndm sessions are in use: shares session N)" tail of the fake-session lines
// (nothing for a nwm::UDS session).
enum class SessionNote { None, Own, Shared };
static void AppendSessionNote(LineBuf& line, SessionNote note, int ndm_session) {
    if (note == SessionNote::Shared) {
        line.Str(" (all ").SDec(NdmHle::kMaxSessions).Str(" ndm sessions are in use: shares session ").SDec(ndm_session)
            .Str(")");
    } else if (note == SessionNote::Own) {
        line.Str(" (ndm session ").SDec(ndm_session).Str(")");
    }
}

void UdsHle::NoteHandleClosed(Handle handle) {
    int ndm_session = -1;
    const u8 kind = FakeKind(handle, &ndm_session);
    if (kind != 0) {
        const char* const label = kind == kKindUds ? "nwm::UDS" : "ndm:u";
        const SessionNote note = kind == kKindNdm ? SessionNote::Own : SessionNote::None;
        {
            LogLine line;
            line.Str("uds: game closed its fake ").Str(label).Str(" session 0x").Hex(handle);
            AppendSessionNote(line, note, ndm_session);
        }
        {
            DebugLine line;
            line.Str("uds game closed its fake ").Str(label).Str(" session 0x").Hex(handle);
            AppendSessionNote(line, note, ndm_session);
        }
        if (ForgetFakeHandle(handle)) {
            // (mutex_ is released: the engine takes its own lock)
            {
                LogLine line;
                line.Str("uds: that session had called Initialize: the service shuts down with it");
            }
            engine_->Shutdown();
        }
    }
}

bool UdsHle::InterceptGetServiceHandle(Result* svc_result) {
    u32* cmd = getThreadCommandBuffer();
    // The service name sits in words 1-2 (not terminated) and its length in word 3.
    u8 kind = 0;
    const char* label = "";
    if (cmd[3] == 8 && std::memcmp(&cmd[1], "nwm::UDS", 8) == 0) {
        kind = kKindUds;
        label = "nwm::UDS";
    } else if (cmd[3] == 5 && std::memcmp(&cmd[1], "ndm:u", 5) == 0) {
        // Answered locally so that "enter local communications mode" does not drop the
        // infrastructure WiFi connection that carries our room traffic.
        kind = kKindNdm;
        label = "ndm:u";
    } else {
        return false;
    }

    // The fake session is just an event we own: any kernel object with a handle will do, and the
    // game closes it like it would a real session.
    Handle fake = 0;
    if (R_FAILED(svcCreateEvent(&fake, RESET_ONESHOT))) {
        ++g_stats.hle_declined; // (shown on screen with the next statistics)
        LogLine line;
        line.Str("uds: WARNING no kernel event for a fake ").Str(label).Str(" session: the REAL service will answer");
        return false;
    }
    SessionNote note = SessionNote::None;
    int ndm_session = -1;
    {
        platform::LockGuard lock(mutex_);
        int slot = -1;
        for (int i = 0; i < kMaxFakeHandles; ++i) {
            if (fake_[i] == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            svcCloseHandle(fake);
            ++g_stats.hle_declined;
            // (the lock of mutex_ is still held: LogLine takes the log's own, never the other way round)
            LogLine line;
            line.Str("uds: WARNING no free fake session slot for ").Str(label).Str(": the REAL service will answer");
            return false; // too many sessions: let the real service answer
        }
        if (kind == kKindNdm) {
            ndm_session = ndm_.OpenSession();
            if (ndm_session >= 0) {
                last_ndm_session_ = ndm_session;
                note = SessionNote::Own;
            } else {
                // All 6 sessions of the real service are in use. A real console would refuse the 7th, but
                // the REAL ndm:u must never answer a game (its Enter drops the WiFi): share the newest one.
                // (All 6 are open, so it is.)
                ndm_session = last_ndm_session_;
                note = SessionNote::Shared;
            }
            ++ndm_handles_[ndm_session];
        }
        fake_kind_[slot] = kind;
        fake_initialized_[slot] = 0;
        fake_ndm_[slot] = static_cast<s8>(ndm_session);
        fake_[slot] = fake;
    }
    if (kind == kKindUds) {
        ++sessions_;
    }
    {
        LogLine line;
        line.Str("uds: game opened ").Str(label).Str(" -> fake session 0x").Hex(fake);
        AppendSessionNote(line, note, ndm_session);
    }
    {
        DebugLine line;
        line.Str("uds game opened ").Str(label).Str(" -> fake session 0x").Hex(fake);
        AppendSessionNote(line, note, ndm_session);
    }

    cmd[0] = Hdr(0x5, 1, 2);
    cmd[1] = 0;
    cmd[2] = IPC_Desc_MoveHandles(1);
    cmd[3] = fake;
    *svc_result = 0;
    return true;
}

bool UdsHle::HandleSendSyncRequest(Handle handle, Result* svc_result) {
    u32* cmd = getThreadCommandBuffer();

    int ndm_session = -1;
    const u8 kind = FakeKind(handle, &ndm_session);
    if (kind == 0) {
        if (cmd[0] == kSrvGetServiceHandleHeader) {
            return InterceptGetServiceHandle(svc_result);
        }
        return false;
    }

    // A handle value we remembered may have been closed through a code path we do not hook and
    // reused for another object. A live event answers to ClearEvent, anything else does not.
    if (R_FAILED(svcClearEvent(handle))) {
        if (ForgetFakeHandle(handle)) {
            engine_->Shutdown(); // the session is gone, we just did not see it close
        }
        // This value now names something else: it may even be the service session that carries a
        // GetServiceHandle request for a new nwm::UDS / ndm:u.
        if (cmd[0] == kSrvGetServiceHandleHeader) {
            return InterceptGetServiceHandle(svc_result);
        }
        return false;
    }

    const u32 id = cmd[0] >> 16;
    if (kind == kKindUds && (id == 0x01 || id == 0x1B)) {
        MarkInitialized(handle); // (before the call: a refused Initialize counts too)
    }
    // Raw ticks: converting to microseconds cost two 64-bit divisions per call (see azl/ticks.h). Only the low 32 bits: the
    // difference is all that is used, and a 64-bit argument of the call below is one that goes through this frame's stack
    // (the hot frame of every IPC call of every thread of the game: the canary of Tri Force Heroes' thread sees it).
    const u32 started = static_cast<u32>(platform::NowTicks());
    if (g_debug_level > 0) {
        RunTraced(kind, ndm_session, id, cmd);
    } else {
        Run(kind, ndm_session, id, cmd);
    }
    AccountCall(id, cmd, started);
    *svc_result = 0;
    return true;
}

// What a served request costs the counters, and the throttle of a thread that keeps asking for what fails. Out of line, with
// a frame of its own: HandleSendSyncRequest runs for every IPC call of every thread of the game (the pure pass-through
// path of Tri Force Heroes' 464-byte thread goes through it), and its frame has not grown with the throttle.
__attribute__((noinline)) void UdsHle::AccountCall(u32 id, const u32* cmd, u32 started_ticks) {
    const u64 ended = platform::NowTicks();
    const u32 ticks = static_cast<u32>(ended) - started_ticks;
    // Scans and connects block on purpose (like the real service); they would hide real slowness.
    const bool blocking = id == 0x0F || id == 0x22 || id == 0x1E || id == 0x09;
    if (!blocking) {
        ++g_stats.ipc_requests;
        StatsAdd64(g_stats.ipc_sum_ticks, ticks);
        if (ticks > g_stats.ipc_max_ticks) {
            g_stats.ipc_max_ticks = ticks;
        }
        if (cmd[1] != 0) {
            ThrottleFailing(ended); // (the reply's result: after the stats, which count the call as it was)
        }
    }
}

// Past kFailFreeCalls failing replies within kFailWindowMs, every further one waits kFailPauseNs before the game gets it.
// Normal traffic never gets near this (a handful of failing replies a second at the most); the spin it is for ran at 8,000
// a second, answered in a few microseconds each, on a core that the network thread and the log writer share with the
// game (PROVA 1: the game's acknowledgement of a sleep took 0.5 to 3.7 s instead of 20 ms, one network round 2.2 s).
// The result codes are never touched.
__attribute__((noinline)) void UdsHle::ThrottleFailing(u64 now_ticks) {
    constexpr u64 kFailWindowTicks = 100ull * AZL_TICKS_PER_MS;
    constexpr u32 kFailFreeCalls = 50;
    constexpr s64 kFailPauseNs = 500000;
    if (now_ticks - fail_window_start_ > kFailWindowTicks) {
        fail_window_start_ = now_ticks;
        fail_window_count_ = 0;
    }
    if (++fail_window_count_ <= kFailFreeCalls) {
        return;
    }
    svcSleepThread(kFailPauseNs);
    StatsAdd(g_stats.hle_throttled);
    if (++throttled_ == 1) {
        s32 priority = 0;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        LogLine line;
        line.Str("uds: a game thread keeps calling commands that fail (more than ").Dec(kFailFreeCalls)
            .Str(" in 100 ms): each further failing reply waits 0.5 ms so that the other threads get the core (thread"
                 " priority 0x")
            .Hex(static_cast<u32>(priority)).Str(")");
    }
}
// <<< game-thread code

void UdsHle::Run(u8 kind, int ndm_session, u32 id, u32* cmd) {
    if (kind == kKindNdm) {
        RunNdm(ndm_session, id, cmd);
    } else {
        ++requests_;
        Dispatch(cmd);
        NoteCommand("uds", id, cmd[1]);
    }
}

// One ndm:u request, served by the session of the handle it came through. The lock is held for the
// call only: nothing in NdmHle waits, logs or calls out.
void UdsHle::RunNdm(int ndm_session, u32 id, u32* cmd) {
    const u32 header = cmd[0];
    u32 anomalies;
    bool changed;
    {
        platform::LockGuard lock(mutex_);
        const u32 before = ndm_.header_anomalies();
        ndm_.Dispatch(ndm_session, cmd);
        anomalies = ndm_.header_anomalies();
        changed = anomalies != before;
    }
    // A request the real ndm would refuse is served all the same; the first few say so, with the header.
    if (changed && anomalies <= kMaxNdmAnomalyLogs) {
        Log("ndm: request 0x%08lx has a header or PID descriptor the firmware's ndm refuses: served as usual (%lu so far)",
            static_cast<unsigned long>(header), static_cast<unsigned long>(anomalies));
    }
    NoteCommand("ndm", id, cmd[1]);
}

// Debug capture of one command: the reply overwrites the request words, so they are kept first
// (in a frame of its own, and only as many words as the header declares - a PullPacket 3, a SendTo 8 - since the frame
// is only paid when the capture is on). Every word the two headers declare is written. Empty PullPackets, ~100 a
// second, would only add noise; every other call of every command is there, SendTo included (its flags word is not in
// the frame trace).
// (The capture takes the raw words and the writer thread makes the lines: DebugPullPacket / DebugServiceCommand format
// nothing, neither here nor under the capture's lock.)
// >>> game-thread code (Tests/policy_check.sh)
__attribute__((noinline)) void UdsHle::RunTraced(u8 kind, int ndm_session, u32 id, u32* cmd) {
    const u32 request_words = DeclaredWords(cmd[0]);
    u32 request[request_words != 0 ? request_words : 1]; // (up to 63 words; PullPacket 3, SendTo 8)
    std::memcpy(request, cmd + 1, request_words * sizeof(u32));
    Run(kind, ndm_session, id, cmd);

    const bool uds = kind == kKindUds;
    const u32 reply_words = DeclaredWords(cmd[0]);
    if (uds && id == 0x14 && cmd[1] == 0) {
        if (cmd[2] == 0) {
            return; // a PullPacket that found nothing
        }
        // The line the analysis scripts read, with the words after it.
        DebugPullPacket(request[0], cmd[2], cmd[3], request, request_words, cmd + 1, reply_words);
        return;
    }
    DebugServiceCommand(uds, id, request, request_words, cmd + 1, reply_words);
}
// <<< game-thread code

// RecvBeaconBroadcastData (0x0F) and ScanOnConnection (0x22): the scan input (52 bytes) is in words
// 2-14, the wlancommid and id8 in words 15-16, the game's output buffer (descriptor and pointer) in
// `desc` / `desc + 1`; the reply hands the descriptor back. Word 1 (a size) is never read by the
// service, the buffer descriptor says how large the buffer is. The 0x0F also has the game's cancel
// event in word 18, a handle the game owns.
void UdsHle::Scan(u32* cmd, u32 id, unsigned desc, bool on_connection) {
    ScanParams scan;
    scan.type = static_cast<u16>(cmd[2]);
    scan.mode = static_cast<u16>(cmd[2] >> 16);
    scan.channel_mask = static_cast<u16>(cmd[3]);
    scan.dwell_ms = static_cast<u16>(cmd[3] >> 16);
    std::memcpy(scan.bssid.data(), &cmd[4], 6);
    scan.ssid_len = static_cast<u8>(cmd[14]);
    scan.wlan_comm_id = cmd[15];
    scan.id8 = static_cast<u8>(cmd[16]);
    scan.cancel = on_connection ? 0 : cmd[18];
    const u32 buffer_desc = cmd[desc];
    u8* buffer = reinterpret_cast<u8*>(cmd[desc + 1]);
    u32 r = res::kTooLarge;
    if (buffer) {
        const u32 usable = buffer_desc >> 4;
        r = on_connection ? engine_->ScanOnConnection(buffer, usable, scan)
                          : engine_->RecvBeaconBroadcastData(buffer, usable, scan);
    }
    if (r != res::kSuccess) {
        Reply(cmd, id, r);
        return;
    }
    cmd[0] = Hdr(id, 1, 2);
    cmd[1] = res::kSuccess;
    cmd[2] = buffer_desc;
    cmd[3] = reinterpret_cast<u32>(buffer);
}

void UdsHle::Dispatch(u32* cmd) {
    const u32 id = cmd[0] >> 16;

    switch (id) {
    case 0x01:   // Initialize (deprecated)
    case 0x1B: { // InitializeWithVersion
        NodeInfo node;
        node.Load(reinterpret_cast<const u8*>(&cmd[2]));
        const u16 version = id == 0x1B ? static_cast<u16>(cmd[12]) : kDeprecatedVersion;
        u32 event = 0;
        const u32 r = engine_->Initialize(node, cmd[1], version, &event);
        Log("uds: Initialize (cmd 0x%02lx, version 0x%x) -> 0x%08lx", static_cast<unsigned long>(id),
            version, static_cast<unsigned long>(r));
        // The 40 bytes the game announces as itself (nwm copies them verbatim): the friend code seed the
        // game read from cfg:u, the user name and bytes 0x1C-0x1F, which this plugin does not carry. The
        // console's own seed must reach the other players unchanged; zero or a copy of a peer's would be
        // a fault (the engine says so when a peer's arrives).
        char name[11] = {};
        for (int i = 0; i < 10 && node.username[i]; ++i) {
            name[i] = node.username[i] < 0x80 ? static_cast<char>(node.username[i]) : '?';
        }
        Log("uds: own node info: friend code seed %08lx%08lx, name '%s', bytes 0x1C-0x1F %08lx%s",
            static_cast<unsigned long>(node.friend_code_seed >> 32), static_cast<unsigned long>(node.friend_code_seed),
            name, static_cast<unsigned long>(cmd[9]), node.friend_code_seed == 0 ? " (WARNING: the seed is zero)" : "");
        cmd[0] = Hdr(id, 1, 2);
        cmd[1] = r;
        cmd[2] = IPC_Desc_SharedHandles(1);
        cmd[3] = event;
        break;
    }
    case 0x02: // the unnamed "stop the service" (no libctru wrapper, no known game): left alone
        Reply(cmd, id, res::kSuccess);
        break;
    case 0x03: { // Shutdown
        const u32 r = engine_->Shutdown();
        Log("uds: Shutdown -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x04: { // BeginHostingNetwork (deprecated): 0x3C bytes of network info inline
        const u32 r = engine_->BeginHostingNetwork(reinterpret_cast<const u8*>(&cmd[1]),
                                                   kDeprecatedNetworkInfoSize);
        Log("uds: BeginHostingNetwork(old) -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x1D: { // BeginHostingNetwork
        const u8* network = reinterpret_cast<const u8*>(cmd[3]);
        const std::size_t size = std::min<std::size_t>(cmd[2] >> 14, kNetworkInfoSize);
        const u32 r = network ? engine_->BeginHostingNetwork(network, size) : res::kTooLarge;
        Log("uds: BeginHostingNetwork -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x05: { // EjectClient
        Reply(cmd, id, engine_->EjectClient(static_cast<u16>(cmd[1])));
        break;
    }
    case 0x06: // EjectSpectators
        Reply(cmd, id, engine_->EjectSpectators());
        break;
    case 0x07: // UpdateNetworkAttribute
        Reply(cmd, id, engine_->UpdateNetworkAttribute(static_cast<u16>(cmd[1]), static_cast<u8>(cmd[2])));
        break;
    case 0x08: { // DestroyNetwork
        const u32 r = engine_->DestroyNetwork();
        Log("uds: DestroyNetwork -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x09: { // ConnectToNetwork (deprecated)
        const u8 type = static_cast<u8>(cmd[16]);
        const u32 r = engine_->ConnectToNetwork(reinterpret_cast<const u8*>(&cmd[1]),
                                                kDeprecatedNetworkInfoSize, type);
        Log("uds: ConnectToNetwork(old) -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x1E: { // ConnectToNetwork
        const u8 type = static_cast<u8>(cmd[1]);
        const u8* network = reinterpret_cast<const u8*>(cmd[4]);
        const std::size_t size = std::min<std::size_t>(cmd[3] >> 14, kNetworkInfoSize);
        const u32 r = network ? engine_->ConnectToNetwork(network, size, type) : res::kTooLarge;
        Log("uds: ConnectToNetwork -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x0A: { // DisconnectNetwork
        const u32 r = engine_->DisconnectNetwork();
        Log("uds: DisconnectNetwork -> 0x%08lx", static_cast<unsigned long>(r));
        Reply(cmd, id, r);
        break;
    }
    case 0x0B: { // GetConnectionStatus
        const ConnectionStatus status = engine_->GetConnectionStatus();
        cmd[0] = Hdr(id, 13, 0);
        cmd[1] = res::kSuccess;
        std::memcpy(&cmd[2], &status, sizeof(status));
        break;
    }
    case 0x0C: { // the unnamed signal strength of the peers
        u16 table[UdsEngine::kPeerSignalEntries];
        const u32 r = engine_->GetPeerSignal(table);
        if (r != res::kSuccess) {
            Reply(cmd, id, r);
            break;
        }
        cmd[0] = Hdr(id, 10, 0);
        cmd[1] = res::kSuccess;
        cmd[2] = table[0];
        for (unsigned i = 0; i < 8; ++i) {
            cmd[3 + i] = static_cast<u32>(table[1 + 2 * i]) | static_cast<u32>(table[2 + 2 * i]) << 16;
        }
        break;
    }
    case 0x0D: { // GetNodeInformation
        NodeInfo node;
        const u32 r = engine_->GetNodeInformation(static_cast<u16>(cmd[1]), &node);
        if (r != res::kSuccess) {
            Reply(cmd, id, r);
            break;
        }
        cmd[0] = Hdr(id, 11, 0);
        cmd[1] = res::kSuccess;
        node.Store(reinterpret_cast<u8*>(&cmd[2]));
        break;
    }
    case 0x0E:   // DecryptBeaconData (deprecated)
    case 0x1F: { // DecryptBeaconData
        const NetworkInfo* net = reinterpret_cast<const NetworkInfo*>(cmd[2]);
        const u8* tag0 = reinterpret_cast<const u8*>(cmd[4]);
        const std::size_t tag0_size = cmd[3] >> 14;
        const u8* tag1 = reinterpret_cast<const u8*>(cmd[6]);
        const std::size_t tag1_size = cmd[5] >> 14;
        const OutStaticBuffer out = GetOutStaticBuffer();
        std::vector<u8> nodes;
        u32 r = res::kTooLarge;
        if (net && tag0 && tag1 && (cmd[1] >> 14) >= kNetworkInfoSize) {
            r = engine_->DecryptBeaconData(*net, tag0, tag0_size, tag1, tag1_size, &nodes);
        }
        if (r != res::kSuccess || !out.ptr) {
            Reply(cmd, id, r != res::kSuccess ? r : res::kTooLarge);
            break;
        }
        const u32 n = static_cast<u32>(std::min<std::size_t>(nodes.size(), out.capacity));
        std::memcpy(out.ptr, nodes.data(), n);
        cmd[0] = Hdr(id, 1, 2);
        cmd[1] = res::kSuccess;
        cmd[2] = IPC_Desc_StaticBuffer(n, 0);
        cmd[3] = reinterpret_cast<u32>(out.ptr);
        break;
    }
    case 0x0F: // RecvBeaconBroadcastData: buffer in words 19-20
        Scan(cmd, id, 19, false);
        break;
    case 0x10: { // SetApplicationData: the size is the descriptor's (word 1 is not read)
        const u32 size = cmd[2] >> 14;
        const u8* data = reinterpret_cast<const u8*>(cmd[3]);
        // (the firmware never sees the pointer: its only rule is size <= 0xC8, and size 0 clears the data a game advertises)
        static const u8 kNone = 0;
        Reply(cmd, id, data ? engine_->SetApplicationData(data, size) : size == 0 ? engine_->SetApplicationData(&kNone, 0) : res::kTooLarge);
        break;
    }
    case 0x11: { // GetApplicationData
        const OutStaticBuffer out = GetOutStaticBuffer();
        const u32 capacity = out.ptr ? std::min<u32>(cmd[1], out.capacity) : 0;
        u32 actual = 0;
        const u32 r = engine_->GetApplicationData(out.ptr, capacity, &actual);
        cmd[0] = Hdr(id, 2, 2);
        cmd[1] = r;
        cmd[2] = actual;
        cmd[3] = IPC_Desc_StaticBuffer(actual, 0);
        cmd[4] = reinterpret_cast<u32>(out.ptr);
        break;
    }
    case 0x12: { // Bind
        u32 event = 0;
        const u32 r = engine_->Bind(cmd[1], cmd[2], static_cast<u8>(cmd[3]),
                                    static_cast<u16>(cmd[4]), &event);
        if (r != res::kSuccess) {
            Reply(cmd, id, r);
            break;
        }
        cmd[0] = Hdr(id, 1, 2);
        cmd[1] = res::kSuccess;
        cmd[2] = IPC_Desc_SharedHandles(1);
        cmd[3] = event;
        break;
    }
    case 0x13: { // Unbind
        const u32 bind_node_id = cmd[1];
        UdsEngine::UnbindInfo info;
        const u32 r = engine_->Unbind(bind_node_id, &info);
        if (r != res::kSuccess) {
            Reply(cmd, id, r);
            break;
        }
        cmd[0] = Hdr(id, 5, 0);
        cmd[1] = res::kSuccess;
        cmd[2] = bind_node_id;
        cmd[3] = (static_cast<u32>(info.channel) << 16) | info.src;
        cmd[4] = info.enqueued;
        cmd[5] = info.overwritten;
        break;
    }
    case 0x14: { // PullPacket
        const u32 bind_node_id = cmd[1];
        // The reply's buffer descriptor has this length whether a frame was copied or not: the caller's buffer
        // is only written where a frame goes, an empty pull leaves it alone (the real service's too).
        const u32 buffer_size = std::min<u32>(cmd[2], 0x172) << 2;
        const u32 max_size = cmd[3];
        const OutStaticBuffer out = GetOutStaticBuffer();
        const u32 capacity = out.ptr ? std::min(buffer_size, out.capacity) : 0;
        u32 data_size = 0;
        u16 src_node = 0;
        const u32 r = engine_->PullPacket(bind_node_id, max_size, out.ptr, capacity, &data_size, &src_node);
        if (r != res::kSuccess) {
            Reply(cmd, id, r);
            break;
        }
        cmd[0] = Hdr(id, 3, 2);
        cmd[1] = res::kSuccess;
        cmd[2] = data_size;
        cmd[3] = src_node;
        cmd[4] = IPC_Desc_StaticBuffer(capacity, 0);
        cmd[5] = reinterpret_cast<u32>(out.ptr);
        break;
    }
    case 0x15: // SetMaxSendDelay: nanoseconds in words 1 (low) and 2 (high)
        Reply(cmd, id, engine_->SetMaxSendDelay(static_cast<u64>(cmd[1]) | (static_cast<u64>(cmd[2]) << 32)));
        break;
    case 0x16: // the unnamed radio profile
        Reply(cmd, id, engine_->ApplyRadioProfile(static_cast<u8>(cmd[1])));
        break;
    case 0x17: { // SendTo
        const u16 dest = static_cast<u16>(cmd[2]);
        const u8 channel = static_cast<u8>(cmd[3]);
        const u32 size = cmd[5];
        const u8 flags = static_cast<u8>(cmd[6]);
        const std::size_t available = cmd[7] >> 14;
        const u8* data = reinterpret_cast<const u8*>(cmd[8]);
        const u32 r = data ? engine_->SendTo(dest, channel, size, flags, data, available) : res::kTooLarge;
        Reply(cmd, id, r);
        break;
    }
    case 0x18: // the unnamed beacon-miss time, in time units
        Reply(cmd, id, engine_->SetBeaconMissTime(static_cast<u16>(cmd[1])));
        break;
    case 0x19: // the unnamed access point parameter
        Reply(cmd, id, engine_->SetApParam(cmd[1]));
        break;
    case 0x1A: { // GetChannel
        u8 channel = 0;
        const u32 r = engine_->GetChannel(&channel);
        cmd[0] = Hdr(id, 2, 0);
        cmd[1] = r;
        cmd[2] = channel;
        break;
    }
    case 0x1C: // the unnamed "Deauthenticate everybody when the network ends"
        Reply(cmd, id, engine_->SetDeauthOnDestroy((cmd[1] & 0xFF) != 0));
        break;
    case 0x20: // Flush
        Reply(cmd, id, engine_->Flush(static_cast<u8>(cmd[1])));
        break;
    case 0x21: // SetProbeResponseParam (libctru's udsCreateNetwork calls it and fails on errors)
        Reply(cmd, id, engine_->SetProbeResponseParam()); // (word 1 is a constant of the caller's: not checked)
        break;
    case 0x22: // ScanOnConnection: buffer in words 17-18
        Scan(cmd, id, 17, true);
        break;
    case 0x23: // the unnamed allowed-channel mask
        cmd[0] = Hdr(id, 2, 0);
        cmd[1] = res::kSuccess;
        cmd[2] = kChannelMaskAll;
        break;
    default: // 0 and everything past the last command: the dispatcher itself answers
        cmd[0] = Hdr(0, 1, 0);
        cmd[1] = res::kUnknownCommand;
        break;
    }
}

} // namespace ctr
} // namespace azl
