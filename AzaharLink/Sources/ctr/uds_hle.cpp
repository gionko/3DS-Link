#include "ctr/uds_hle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "azl/dlp_hub.hpp"
#include "azl/log.hpp"
#include "azl/stats.hpp"

namespace azl {
namespace ctr {

namespace {

// srv:GetServiceHandle request header (command 5, 4 normal words).
constexpr u32 kSrvGetServiceHandleHeader = 0x00050100;

// The Download Play lobby of the hub (DESIGN-FASE3.md sez. 4.1).
// A join the hub began (FKCL StartFakeSession) is waited for in slices of this length, outside every lock ...
constexpr s64 kDlpJoinSleepNs = 10 * 1000 * 1000;
// ... and at most this long: the lobby ends a join by itself in 3 s, so a wait that gets here means the network thread is not running.
constexpr u32 kDlpJoinCapMs = 8000;
// The answer after the cap is StartFakeSession's "server not found" (kNotFound, a result of the DLP module).
// Lines of the hub's trouble (a command it could not run): the first ones, then one in this many.
constexpr u32 kMaxDlpTroubleLogs = 8;
constexpr u32 kDlpTroubleEvery = 64;
// The last command of nwm::UDS (0x23, the allowed-channel mask): an id past it is the dispatcher's own answer in any state, so the
// gate that answers "wrong state" while the lobby holds the engine (Dispatch) does not take it.
constexpr u32 kUdsLastCommand = 0x23;

constexpr std::size_t kNetworkInfoSize = sizeof(NetworkInfo);
constexpr std::size_t kDeprecatedNetworkInfoSize = 0x3C;
constexpr u16 kDeprecatedVersion = 0x100; // what the deprecated Initialize (0x01) stands for
constexpr u16 kChannelMaskAll = 0x07FF;   // channels 1 to 11, valid in every region
constexpr u32 kMaxDlpRefusedLogs = 3;      // dlp:FKCL commands refused beyond the wireless check: lines in the log
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

// The pairs (command, result) the first-call lines of NoteCommand remember: 48 for nwm::UDS, ndm:u and the probe's "dlp" - exactly what
// they were before the hub, so that a game without a hub logs what it always did - and room of their own for the two services of the
// hub (Triple Deluxe's dlp:FKCL and dlp:SRVR add about 30 pairs, and a full table says nothing about the commands that come last: the
// join). The count of a pair is a byte of its own (the table is 400 bytes, 384 before).
constexpr int kPlainSeen = 48;
constexpr int kMaxSeen = kPlainSeen + 32;

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
    static u32 seen_key[kMaxSeen];
    static u8 seen_count[kMaxSeen];
    static int used = 0;     // [0, kPlainSeen): nwm::UDS, ndm:u, the probe
    static int used_hub = 0; // [kPlainSeen, kMaxSeen): dlp:FKCL, dlp:SRVR of the hub
    // The services share command numbers: keep their "first call" lines apart. nwm::UDS has no salt, ndm:u and the probe's "dlp" (as
    // always) bit 31, and the two services of the hub ("dlp:FKCL", "dlp:SRVR": the name has a ':' at [3]) bit 30 and bits 30+31: the
    // salts sit above the bits an id of any of the services reaches (id << 24, at most 0x23).
    const u32 salt = service[3] == ':' ? (service[4] == 'S' ? 0xC0000000u : 0x40000000u) : is_uds ? 0u : 0x80000000u;
    const u32 key = ((id << 24) ^ result) ^ salt;
    int entry = -1;
    for (int i = 0; i < used; ++i) {
        if (seen_key[i] == key) {
            entry = i;
            break;
        }
    }
    for (int i = 0; entry < 0 && i < used_hub; ++i) {
        if (seen_key[kPlainSeen + i] == key) {
            entry = kPlainSeen + i;
        }
    }
    if (entry < 0) {
        const bool hub = service[3] == ':';
        if (!hub && used < kPlainSeen) {
            entry = used++;
        } else if (hub && used_hub < kMaxSeen - kPlainSeen) {
            entry = kPlainSeen + used_hub++;
        }
        if (entry >= 0) {
            seen_key[entry] = key;
            seen_count[entry] = 0;
        }
    }
    if (entry < 0) {
        return;
    }
    if (seen_count[entry] != 0xFF) {
        ++seen_count[entry];
    }
    const u32 limit = result == 0 ? 1 : 4;
    if (seen_count[entry] <= limit) {
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
        const char* const label = kind == kKindUds       ? "nwm::UDS"
                                  : kind == kKindNdm     ? "ndm:u"
                                  : kind == kKindDlpSrvr ? "dlp:SRVR"
                                                         : "dlp:FKCL";
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
            // (mutex_ is released: the engine takes its own lock, and so does the hub)
            {
                LogLine line;
                if (kind >= kKindDlp) {
                    line.Str("dlp: that ").Str(label).Str(" session had called Initialize: its role ends with it");
                } else if (dlp_hub_ != nullptr && dlp_hub_->EngineHeld()) {
                    line.Str("uds: that session had called Initialize: the Download Play lobby holds the network now, nothing is shut down");
                } else {
                    line.Str("uds: that session had called Initialize: the service shuts down with it");
                }
            }
            EndInitializedSession(kind);
        }
    }
}

// What the end of a session that had called Initialize does: nwm::UDS shuts the service down (the real nwm does the same when a
// game quits without Shutdown); a Download Play session ends the lobby's role it began, through the hub (which gives the engine back
// if the role held it). Called with no lock held. Without a hub there is nothing of a dlp session to end (the probe holds no state).
__attribute__((noinline)) void UdsHle::EndInitializedSession(u8 kind) {
    if (kind < kKindDlp) {
        // (While the lobby holds the engine the closing session cannot be the one that owns it: its Initialize was undone by its own
        // Shutdown, and the engine's Shutdown here would end the lobby's network under the hub, which would go on believing it held it.)
        if (dlp_hub_ == nullptr || !dlp_hub_->EngineHeld()) {
            engine_->Shutdown();
        }
    } else if (dlp_hub_ != nullptr) {
        dlp_hub_->OnSessionClosed(kind == kKindDlpSrvr ? DlpKind::Srvr : DlpKind::Fkcl);
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
    } else if ((dlp_hub_ != nullptr || dlp_probe_fake_) && cmd[3] == 8 && std::memcmp(&cmd[1], "dlp:FKCL", 8) == 0) {
        // The wireless check of a game that plays over UDS (azl/dlp_fkcl_hle.hpp): the real module would answer
        // "wireless off" because the console stays on its access point. With a hub (Kirby Triple Deluxe, Planet Robobot)
        // this is the Download Play client, carried by the room.
        kind = kKindDlp;
        label = "dlp:FKCL";
    } else if (dlp_hub_ != nullptr && cmd[3] == 8 && std::memcmp(&cmd[1], "dlp:SRVR", 8) == 0) {
        // The Download Play host of the same games. Without a hub (every other game, Battle Royale included) the REAL
        // dlp:SRVR answers.
        kind = kKindDlpSrvr;
        label = "dlp:SRVR";
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
            EndInitializedSession(kind); // the session is gone, we just did not see it close
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
    } else if (kind >= kKindDlp && dlp_hub_ != nullptr && (id == 0x01 || id == (kind == kKindDlp ? 0x11u : 0x0Fu))) {
        // dlp:FKCL Initialize / InitializeWithName, dlp:SRVR Initialize / InitializeWithName (a refused one counts too): closing
        // the session without Finalize then ends the role (NoteHandleClosed). Not for the probe of Battle Royale (no hub): its
        // session has nothing to end.
        MarkInitialized(handle);
    }
    // Raw ticks: converting to microseconds cost two 64-bit divisions per call (see azl/ticks.h). Only the low 32 bits: the
    // difference is all that is used, and a 64-bit argument of the call below is one that goes through this frame's stack
    // (the hot frame of every IPC call of every thread of the game: the canary of Tri Force Heroes' thread sees it).
    const u32 started = static_cast<u32>(platform::NowTicks());
    // (The capture of a request, RunTraced, writes nothing for a session the hub serves - it notes its commands itself - but it keeps a
    // copy of the request as long as the header declares, 15 words for SRVR InitializeWithName, in a frame that sits on the stack under
    // the hub's whole chain: 536 bytes at debug 1 against the ceiling of 520 (Tests/chain_sim). A hub's session goes to Run directly.)
    if (g_debug_level > 0 && (kind < kKindDlp || dlp_hub_ == nullptr)) {
        RunTraced(kind, ndm_session, id, cmd);
    } else {
        Run(kind, ndm_session, id, cmd);
    }
    // The Download Play services of the hub wait for the network thread (a request, a join) and answer "no more entries" as a
    // normal result: they are neither a latency of the HLE nor a spin to throttle (DESIGN-FASE3 sez. 4.1). Decided here and not in
    // AccountCall: a fifth argument would go through this frame's stack. nwm::UDS, ndm:u and the probe are counted as always.
    if (kind < kKindDlp || dlp_hub_ == nullptr) {
        AccountCall(id, cmd, started);
    }
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
    } else if (kind >= kKindDlp) {
        RunDlp(kind, id, cmd);
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

// The hub's side of the two Download Play services (kept out of UdsHle: nothing here needs the HLE's state, only the hub). Everything
// that waits is in the hub (Serve: a request for the network thread, kQueuedMs at the most) or here (the join, kDlpJoinCapMs at the
// most), and none of it with a lock held. These frames are on the stack of the game's thread together with Serve's, so the log lines
// are in functions of their own and the frame of RunDlpHub stays what a pointer, a counter and a call need.
namespace {

const char* DlpServiceName(bool srvr) {
    return srvr ? "dlp:SRVR" : "dlp:FKCL";
}

// The first command of each service the hub serves: the game has begun to use Download Play (the first line of the log to look for).
__attribute__((noinline)) void NoteDlpFirst(bool srvr, u32 id) {
    static bool said[2]; // (never cleared: once per service in the life of the plugin)
    bool& flag = said[srvr ? 1 : 0];
    if (flag) {
        return;
    }
    flag = true;
    // (the thread that makes the call and its priority, against the network thread's 0x2F: the hub's threads wait for each other's locks,
    // and a LightLock has no priority inheritance - this is what the console test has to tell)
    s32 priority = 0;
    u32 thread_id = 0;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    svcGetThreadId(&thread_id, CUR_THREAD_HANDLE);
    LogLine line;
    line.Str("dlp: the first command of ").Str(DlpServiceName(srvr)).Str(" is 0x").Hex(id, 2)
        .Str(": the Download Play lobby is carried by the room (thread ").Dec(thread_id).Str(", priority 0x")
        .Hex(static_cast<u32>(priority)).Str(")");
}

// A command the hub did not run because no network thread served it - none was alive (failed_fast: refused at once) or it did not
// take the request in kQueuedMs (cancelled) - and that was answered with the module's "wireless off": the first lines, then one in
// kDlpTroubleEvery. The numbers are the hub's totals.
__attribute__((noinline)) void NoteDlpTrouble(const DlpHub& hub, bool srvr, u32 id) {
    static u32 events;
    ++events;
    if (events > kMaxDlpTroubleLogs && events % kDlpTroubleEvery != 0) {
        return;
    }
    LogLine line;
    line.Str("dlp: ").Str(DlpServiceName(srvr)).Str(" command 0x").Hex(id, 2)
        .Str(" answered \"wireless off\": the network side did not run it (so far ").Dec(hub.failed_fast())
        .Str(" refused at once because no network thread was running, ").Dec(hub.cancelled()).Str(" not taken in ")
        .Dec(DlpHub::kQueuedMs / 1000).Str(" s)");
}

// The trace of the commands the hub serves (debug only, DESIGN-FASE3 sez. 6): what the game asked, in the order it asked it, with the
// words of the request and of the reply and how long the call took - the log of a console test is all there is to learn from. The
// hub's sessions skip the debug capture (RunTraced keeps a copy of the request on the stack, which the chain of the hub cannot pay), so
// this is the capture: a line per command, except that a reply the command has already given in the last kDlpTraceEveryMs (the games
// poll GetMyStatus / GetServerState every frame, and GetTitleInfoInOrder(1) then (0) until "no data", so one command alternates between
// a few replies: a group, the next group, "no data") is not written again: it is counted, and the count goes on the next line of that
// command. A reply that is NEW in the window - a state that changed, a group that appeared - is written at once. The request is kept in a slot per service (a game that calls one service from two threads at once may garble a line;
// it is a log). Nothing here runs without the debug level (the two functions test it themselves, first thing: a test of it in RunDlpHub made the
// compiler keep the address of g_debug_level in a register across the whole function - 8 bytes more in the frame of RunDlp at EVERY debug level,
// 32 -> 40, and 304 against 296 bytes in the tightest chain at debug 0 - and a call that returns at once costs a few instructions), and none of it
// is on the stack of the call: the frames are those of siblings.
constexpr u32 kDlpTraceWords = 6;        // words of a request / of a reply that the line shows (after the header)
constexpr u32 kDlpTraceEveryMs = 2000;   // the window in which a reply the command already gave is not written again
constexpr u32 kDlpTraceSigs = 6;         // replies remembered per command in a window: 4 groups, "no data", one more
constexpr u32 kDlpTraceIds = 0x12;       // ids 0 to 0x11 (FKCL has 0x11, SRVR 0x10)
u32 g_dlp_trace_req[2][1 + kDlpTraceWords]; // the header and the words of the request, per service
u32 g_dlp_trace_started_ms[2];
struct DlpTraceSlot {
    u16 sig[kDlpTraceSigs]; // the replies written in this window, folded to 16 bits
    u8 count;               // how many of them
    u32 window_ms;          // when the window began
    u32 skipped;            // calls whose reply was one of them, since the last line
};
DlpTraceSlot g_dlp_trace[2][kDlpTraceIds];

__attribute__((noinline)) void DlpTraceRequest(bool srvr, const u32* cmd) {
    if (g_debug_level <= 0) {
        return; // debug 0: no trace, and nothing remembered for the day the level is 1
    }
    const u32 s = srvr ? 1 : 0;
    const u32 words = std::min<u32>(DeclaredWords(cmd[0]), kDlpTraceWords);
    g_dlp_trace_req[s][0] = cmd[0];
    for (u32 i = 0; i < kDlpTraceWords; ++i) {
        g_dlp_trace_req[s][1 + i] = i < words ? cmd[1 + i] : 0;
    }
    g_dlp_trace_started_ms[s] = static_cast<u32>(platform::NowMs());
}

__attribute__((noinline)) void DlpTraceReply(bool srvr, u32 id, const u32* cmd) {
    if (g_debug_level <= 0 || id >= kDlpTraceIds) {
        return; // debug 0, or an id the services do not have: the unknown-command counter of the service is its trace
    }
    const u32 s = srvr ? 1 : 0;
    const u32 now = static_cast<u32>(platform::NowMs());
    const u32 words = std::min<u32>(DeclaredWords(cmd[0]), kDlpTraceWords);
    u32 hash = 17;
    for (u32 i = 0; i < words; ++i) {
        hash = hash * 31 + cmd[1 + i];
    }
    const u16 sig = static_cast<u16>(hash ^ (hash >> 16));
    DlpTraceSlot& slot = g_dlp_trace[s][id];
    if (slot.count != 0 && now - slot.window_ms >= kDlpTraceEveryMs) {
        slot.count = 0; // a new window: every reply is news again, and the line says how many calls the last one left out
    }
    for (u32 i = 0; i < slot.count; ++i) {
        if (slot.sig[i] == sig) {
            ++slot.skipped;
            return;
        }
    }
    LogLine line;
    line.Str("dlpc: ").Str(DlpServiceName(srvr)).Str(" 0x").Hex(id, 2).Str(" req");
    for (u32 i = 0; i < kDlpTraceWords; ++i) {
        line.Str(" ").Hex(g_dlp_trace_req[s][1 + i], 8);
    }
    line.Str(" -> 0x").Hex(cmd[1], 8).Str(" rep");
    for (u32 i = 1; i < words; ++i) {
        line.Str(" ").Hex(cmd[1 + i], 8);
    }
    line.Str(" (").Dec(now - g_dlp_trace_started_ms[s]).Str(" ms");
    if (slot.skipped != 0) {
        line.Str(", ").Dec(slot.skipped).Str(" more like it since the last line");
    }
    line.Str(")");
    if (slot.count == 0) {
        slot.window_ms = now;
    }
    if (slot.count < kDlpTraceSigs) {
        slot.sig[slot.count++] = sig; // (a seventh distinct reply in one window is written every time and not remembered: loud, never silent)
    }
    slot.skipped = 0;
}

// FKCL StartFakeSession began a join: its reply is not there yet, the lobby's Pump (the network thread) decides when the join ends
// (it gives up by itself after 3 s). The wait is on this thread, in slices, with no lock (PollPending takes the hub's lock for the
// length of a read). If the network thread is gone the join never ends: after kDlpJoinCapMs the reply is "server not found" and
// the end of the client role is asked of the hub (OnSessionClosed cancels the join; with no network thread alive the hub leaves it
// for the next Tick, which is all there is to wait for).
__attribute__((noinline)) void WaitForJoin(DlpHub& hub, u32* cmd) {
    const u64 deadline = platform::NowMs() + kDlpJoinCapMs;
    while (!hub.PollPending(cmd)) {
        if (platform::NowMs() >= deadline) {
            hub.OnSessionClosed(DlpKind::Fkcl);
            cmd[0] = (DlpService::kFkStartFakeSession << 16) | 0x40;
            cmd[1] = kNotFound;
            LogLine line;
            line.Str("dlp: dlp:FKCL StartFakeSession: the join did not end in ").Dec(kDlpJoinCapMs / 1000)
                .Str(" s (the network thread is not running it): answered \"server not found\", the end of the client role is left to the network thread");
            return;
        }
        svcSleepThread(kDlpJoinSleepNs);
    }
}

void RunDlpHub(DlpHub& hub, bool srvr, u32 id, u32* cmd) {
    NoteDlpFirst(srvr, id);
    DlpTraceRequest(srvr, cmd); // (debug only; before the reply replaces the request words)
    const u32 refusals = hub.failed_fast() + hub.cancelled();
    const DlpOutcome outcome = hub.Serve(srvr ? DlpKind::Srvr : DlpKind::Fkcl, cmd, getThreadStaticBuffers());
    // A command the network side could not run was answered with the module's "wireless off" (the game asks the player to turn the
    // wireless on): the log says so. Decided BEFORE the wait for a join: when that wait runs out (WaitForJoin) it asks the hub to end the
    // client role, and with no network thread alive the hub refuses that request too - which is not the answer of this command (it is
    // "server not found") and must not be reported as one.
    // (Finalize, StopScan, StopSession and EndAccepting are answered with SUCCESS when the network side did not run them - the games treat
    // a negative Finalize as fatal - so the reply, not only the counters, tells the two apart.)
    if (hub.failed_fast() + hub.cancelled() != refusals && cmd[1] == DlpHub::kResultWirelessOff) {
        NoteDlpTrouble(hub, srvr, id);
    }
    if (outcome == DlpOutcome::JoinPending) {
        WaitForJoin(hub, cmd);
    }
    // (The trace comes BEFORE NoteCommand, which must stay the last thing RunDlp does: as a tail call it runs after RunDlp's frame is gone, and
    // the first-call line of a command, Log() with a format, is the deepest chain of the hub at debug 1 (412 bytes). Written after it, the
    // frame of RunDlp (32 bytes) would stay under that chain: 444 bytes, all of the smallest thread's, and 452 when the frame was 40 - the
    // order is held by Tests/chain_sim and by L15 of Tests/ipc_test/dlp_lobby_hle_test.cpp.)
    DlpTraceReply(srvr, id, cmd); // (debug only)
    NoteCommand(DlpServiceName(srvr), id, cmd[1]);
}

} // namespace

// One dlp:FKCL / dlp:SRVR request of a fake session. With a hub (a game in Lobby mode) the hub serves both services; without one
// the only fake dlp session is the probe's dlp:FKCL (Battle Royale), served as it always was.
void UdsHle::RunDlp(u8 kind, u32 id, u32* cmd) {
    if (dlp_hub_ != nullptr) {
        RunDlpHub(*dlp_hub_, kind == kKindDlpSrvr, id, cmd);
    } else if (kind == kKindDlp) {
        RunDlpProbe(id, cmd);
    } else {
        // (a dlp:SRVR session whose hub was taken away: not a case the plugin makes, but a request is never left with garbage)
        cmd[0] = (id << 16) | 0x40;
        cmd[1] = DlpHub::kResultWirelessOff;
    }
}

// One dlp:FKCL request of the probe (the fake service: azl/dlp_fkcl_hle.hpp). The lock is held for the call only.
void UdsHle::RunDlpProbe(u32 id, u32* cmd) {
    u32 refused;
    bool changed;
    bool first_init;
    {
        platform::LockGuard lock(mutex_);
        const u32 before = dlp_.refused();
        dlp_.Dispatch(cmd);
        refused = dlp_.refused();
        changed = refused != before;
        first_init = dlp_.initializes() == 1 && (id == DlpFkclHle::kInitialize || id == DlpFkclHle::kInitializeWithName);
    }
    if (first_init) {
        LogLine line;
        line.Str("dlp: dlp:FKCL is answered here: the game only asks whether the wireless is on (Download Play itself "
                 "does not go through the tunnel)");
    }
    if (changed && refused <= kMaxDlpRefusedLogs) {
        LogLine line;
        line.Str("dlp: dlp:FKCL command 0x").Hex(id, 2).Str(" refused (invalid state): this game uses Download Play "
                 "beyond the wireless check (").Dec(refused).Str(" so far)");
    }
    NoteCommand("dlp", id, cmd[1]);
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
    if (kind >= kKindDlp) {
        return; // (served and noted by RunDlp: the capture's line formats are those of nwm::UDS and ndm:u)
    }

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

    // The Download Play lobby of the hub holds the engine (it is the lobby's network, not the game's): until the game's own
    // Initialize gives it back (below) every command of the service answers "wrong state" - a DELIBERATE choice, not a copy of the real
    // service before its Initialize (that one answers a few ids differently: Tests/command_matrix_test, state S0, has success for 0x02, 0x0B,
    // 0x13 and 0x23, and the pure functions 0x0E and 0x1F are never gated): the lobby's own state (its nodes, its channel, its connection) must not leak
    // to a game that has not initialised the service, and one answer for every id needs no table. Shutdown answers success WITHOUT touching
    // the engine: the game would end the lobby's network and the hub would be left with a flag that is not true any more. An id the service
    // does not have is the dispatcher's own answer in any state. The test is a null pointer for every game without a hub.
    if (dlp_hub_ != nullptr && id != 0x01 && id != 0x1B && id >= 0x02 && id <= kUdsLastCommand && dlp_hub_->EngineHeld()) {
        static u32 gated;
        if (++gated <= 3) {
            Log("uds: command 0x%02lx while the Download Play lobby holds the network: %s", static_cast<unsigned long>(id),
                id == 0x03 ? "Shutdown answered, nothing shut down" : "answered \"wrong state\"");
        }
        Reply(cmd, id, id == 0x03 ? res::kSuccess : res::kWrongState);
        return;
    }

    switch (id) {
    case 0x01:   // Initialize (deprecated)
    case 0x1B: { // InitializeWithVersion
        // The game starts its own network: the lobby gives the engine back FIRST, whole (it drops the lobby's node, name, seed and
        // binds), or this Initialize would find the lobby's session and change nothing. Before any other statement of the block.
        if (dlp_hub_ != nullptr) {
            const bool held = dlp_hub_->EngineHeld();
            dlp_hub_->OnGameUdsStarting();
            if (held) {
                Log("dlp: nwm::UDS Initialize (cmd 0x%02lx) while the Download Play lobby held the network: given back first",
                    static_cast<unsigned long>(id));
            }
        }
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
