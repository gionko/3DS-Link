// IPC front-end of the emulated nwm::UDS service: decodes the command buffer of the calling
// thread, drives the UdsEngine and writes the reply, exactly where the real service would.
#pragma once

#include <3ds.h>

#include "azl/dlp_fkcl_hle.hpp"
#include "azl/ndm_hle.hpp"
#include "azl/platform.hpp"
#include "azl/uds_engine.hpp"

namespace azl {

class DlpHub; // azl/dlp_hub.hpp: the fake Download Play lobby (only the games in Lobby mode have one)

namespace ctr {

class UdsHle {
public:
    explicit UdsHle(UdsEngine* engine);

    /// Called from the svcSendSyncRequest hook. Returns true when the request was fully handled
    /// (`*svc_result` is then the value svcSendSyncRequest must return); false means "pass it on
    /// to the real kernel".
    bool HandleSendSyncRequest(Handle handle, Result* svc_result);

    /// Called from the svcCloseHandle hook so that a recycled handle value is never mistaken for
    /// our fake service session. A session that ran Initialize also shuts the service down when it
    /// closes, like the real nwm does (a game that quits without Shutdown leaves nothing behind).
    void NoteHandleClosed(Handle handle);

    /// Number of requests served / sessions opened (diagnostics).
    u32 requests() const {
        return requests_;
    }
    u32 sessions() const {
        return sessions_;
    }
    /// ndm:u requests the real service would have refused for their header (diagnostics).
    u32 ndm_header_anomalies();

    /// Answers dlp:FKCL here (see azl/dlp_fkcl_hle.hpp) instead of leaving it to the real module: for the games whose
    /// wireless mode only asks it whether the wireless is on. Off by default; the plugin turns it on per title.
    void SetDlpProbeFake(bool on) {
        dlp_probe_fake_ = on;
    }

    /// Carries the Download Play of the game over the room (azl/dlp_hub.hpp): with a hub BOTH dlp:FKCL and dlp:SRVR are fake and
    /// served by it (the probe above is then not used); without one (the default) dlp:SRVR is the real service and dlp:FKCL is
    /// the probe's or the real one, exactly as before. Set once, before the hooks are installed and before any game thread can
    /// make a call: the game threads read the pointer without a lock. The hub outlives the HLE (the plugin never destroys either).
    void SetDlpHub(DlpHub* hub) {
        dlp_hub_ = hub;
    }

private:
    // Two fake sessions per UDS use (the game opens nwm::UDS twice) plus ndm:u: with only 8 slots
    // the 4th use handed the REAL service to the game.
    static constexpr int kMaxFakeHandles = 32;

    // kKindDlp is dlp:FKCL (the client: Kirby Triple Deluxe / Planet Robobot's searcher, Battle Royale's wireless check),
    // kKindDlpSrvr is dlp:SRVR (the host); the latter exists only with a hub. Both are >= kKindDlp.
    enum Kind : u8 { kKindUds = 1, kKindNdm = 2, kKindDlp = 3, kKindDlpSrvr = 4 };

    /// Returns the kind of fake session `handle` is (0 if it is not one of ours); for an ndm:u one
    /// `*ndm_session` (if given) is its NdmHle session.
    u8 FakeKind(Handle handle, int* ndm_session = nullptr);
    /// Notes that the game called Initialize (successful or not) through this session.
    void MarkInitialized(Handle handle);
    /// Forgets the session (and, for an ndm:u one, closes its NdmHle session once no handle uses it any
    /// more: the real service releases what a session holds when it closes); returns true if it had
    /// called Initialize, i.e. the service has to be shut down now that the session is gone.
    bool ForgetFakeHandle(Handle handle);
    /// The session of `kind` had called Initialize and is gone (closed by the game, or found dead): nwm::UDS shuts the service
    /// down, a Download Play session ends its role in the hub's lobby (hub->OnSessionClosed). No lock held.
    void EndInitializedSession(u8 kind);
    bool InterceptGetServiceHandle(Result* svc_result);
    /// Runs one request of a fake session; RunTraced also writes it to the debug capture.
    void Run(u8 kind, int ndm_session, u32 id, u32* cmd);
    void RunTraced(u8 kind, int ndm_session, u32 id, u32* cmd);
    void RunNdm(int ndm_session, u32 id, u32* cmd);
    /// One dlp:FKCL / dlp:SRVR request (`kind` is kKindDlp or kKindDlpSrvr): the hub's when there is one (serve, wait for a join the
    /// hub began - 8 s at the most, outside every lock - and log what the hub could not do), else the probe's.
    void RunDlp(u8 kind, u32 id, u32* cmd);
    /// The probe of Battle Royale (azl/dlp_fkcl_hle.hpp), dlp:FKCL only: the code RunDlp had before the hub.
    void RunDlpProbe(u32 id, u32* cmd);
    void Dispatch(u32* cmd);
    /// A game thread that calls a command in a tight loop while we keep answering it with an error (FFE polls PullPacket
    /// at 8,000 calls a second when the service has been stopped by a sleep, and our answer takes no time): past a number
    /// of failing replies in a window, each further one waits a moment, so that the other threads of the core - the
    /// network thread, the log writer, the game's own - get to run. The reply is the same, it only comes later.
    void ThrottleFailing(u64 now_ticks);
    /// The statistics of a request that was served, and the throttle for a reply that failed.
    void AccountCall(u32 id, const u32* cmd, u32 started_ticks);
    /// Commands 0x0F and 0x22: the scan input and the game's output buffer at word `desc`.
    void Scan(u32* cmd, u32 id, unsigned desc, bool on_connection);

    // Lock order: mutex_ is never held while calling into the engine (the engine takes its own lock
    // and signals events), and NdmHle never calls back into it. NdmHle has no lock of its own: every
    // call into it is made under mutex_.
    UdsEngine* engine_;
    NdmHle ndm_;
    DlpFkclHle dlp_; // under mutex_ like ndm_
    volatile bool dlp_probe_fake_ = false;
    DlpHub* dlp_hub_ = nullptr; // set once before the hooks (SetDlpHub); the hub has locks of its own, mutex_ is never held in a call into it
    platform::Mutex mutex_; // guards writes to fake_ and everything in ndm_
    volatile Handle fake_[kMaxFakeHandles];
    volatile u8 fake_kind_[kMaxFakeHandles];
    volatile u8 fake_initialized_[kMaxFakeHandles]; // this session called Initialize
    volatile s8 fake_ndm_[kMaxFakeHandles];         // the NdmHle session of an ndm:u handle
    // When all the real service's sessions are taken the game still gets a fake one (never the real
    // ndm:u, whose Enter would drop the WiFi): it shares the newest session, so a session is closed
    // when the last handle on it is.
    int ndm_handles_[NdmHle::kMaxSessions] = {};
    int last_ndm_session_ = 0;
    volatile u32 requests_ = 0;
    volatile u32 sessions_ = 0;
    // The failing replies of the window ThrottleFailing counts in (every game thread: lost updates only blur the count).
    volatile u64 fail_window_start_ = 0;
    volatile u32 fail_window_count_ = 0;
    volatile u32 throttled_ = 0;
};

} // namespace ctr
} // namespace azl
