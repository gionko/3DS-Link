// The fake Download Play lobby of the plugin (phase 3): the pieces of phases 1 and 2 - DlpService (the commands), DlpLobby (the
// logic), EngineLobbyNet (the UDS engine as the lobby's network) - behind ONE object that decides on which thread each call runs.
// Design: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE3.md (read it first; the rules below are its sez. 1).
//
// THE RULE. Everything that reaches LobbyNet - the pump of the lobby and every command that opens, closes, scans, joins, kicks or
// reads the engine's nodes - runs on the NETWORK thread (RoomClient's, 32 KB of stack, a round every ~3 ms), in Tick(). A game
// thread that makes a dlp:FKCL / dlp:SRVR call either reads a table of the lobby (IsInline: the clock is all it asks of LobbyNet) or
// LEAVES A REQUEST in the hub's one slot and waits for the network thread's answer on a kernel event. The measured stack of the
// other way (LobbyNet called from the game thread) is 1220-1340 bytes against the 320 / 520 the plugin allows (DESIGN-FASE2 sez. 7).
//
// LOCKS. dlp_mutex_ guards the service, the lobby and the adapter; the engine's own lock comes after it, never before. Nobody
// waits for the network thread while holding dlp_mutex_ (or any other lock of the plugin); the network thread never waits for a
// lock: a Tick that cannot take dlp_mutex_ in a few microseconds (TryLockBriefly) leaves the work for the next round.
//
// NETWORK THREAD GONE. A request that no network thread can serve (no Tick for kAliveMs, Stop(), or not served in kQueuedMs) is
// answered with ResultWirelessOff and never runs - EXCEPT the commands that succeed in every state (Finalize, StopScan, StopSession,
// EndAccepting: DESIGN-FASE1 sez. 4.5): Kirby Triple Deluxe and Planet Robobot throw a fatal error when a Finalize comes back negative
// (in both games), so these answer success, and the ones that end a role (Finalize, StopSession) leave it to
// the next Tick like OnSessionClosed. OnGameUdsStarting() must still free the engine before the game's own Initialize
// reaches it, so it runs on the calling thread then (a nwm::UDS thread: the Initialize that follows needs more stack than the give-back;
// without a room the transport drops every frame, in a room the frames are queued - Tests/chain_sim measures both). OnSessionClosed()
// does NOT: ending a role is the lobby's network work (the engine's Shutdown and its frames, 400 bytes and more on a thread that may have
// few), and nothing waits for it - the close is remembered (closing_) and the next Tick of the network thread ends the role.
#pragma once

#include <atomic>

#include "azl/dlp_ipc.hpp"
#include "azl/dlp_lobby.hpp"
#include "azl/dlp_lobby_net.hpp"
#include "azl/platform.hpp"
#include "azl/uds_engine.hpp"

namespace azl {

/// What the plugin does for a game (the table of plugin.cpp): nothing, answer the wireless check only (azl/dlp_fkcl_hle.hpp:
/// Kirby Battle Royale), or carry the Download Play lobby over the room (Kirby Triple Deluxe, Planet Robobot).
enum class DlpGameMode : u8 { None = 0, ProbeOnly = 1, Lobby = 2 };

class DlpHub {
public:
    /// A request still waiting for the network thread after this long is cancelled (it will never run) and answered with a failure.
    static constexpr u32 kQueuedMs = 2000;
    /// The network thread is "alive" while its last Tick is younger than this.
    static constexpr u32 kAliveMs = 1000;
    /// The reply of a request that cannot be served: the DLP module's ResultWirelessOff (dlp_fkcl_hle.hpp), what the real module says
    /// with the radio off, so the game asks the player to turn the wireless on.
    static constexpr u32 kResultWirelessOff = 0xC940AC04;
    /// The wait of a game thread is cut in slices of this length (a lost wake-up costs a slice, never a hang).
    static constexpr u32 kSliceMs = 100;

    /// `engine` and `kernel` outlive the hub. The hub puts the lobby in the external-poll mode and makes its event (kernel->CreateEvent).
    /// `resolver` maps the addresses of the game's buffers (the plugin's: a range test; the tests': their fake memory).
    DlpHub(UdsEngine* engine, KernelOps* kernel, const LobbyConfig& cfg, const IpcResolver& resolver);
    ~DlpHub();
    DlpHub(const DlpHub&) = delete;
    DlpHub& operator=(const DlpHub&) = delete;

    /// True for the commands a game thread serves itself: tables and constants, and the ids the service does not have. The lobby
    /// calls nothing of LobbyNet but the clock for them (Poll is the network thread's). Pure; DESIGN-FASE3 sez. 1.1.
    static bool IsInline(DlpKind kind, u32 id);

    // ---- game threads (the HLE of dlp:FKCL / dlp:SRVR) -----------------------------------------------------------------------
    /// One command: the reply replaces the command words. Done = the reply is there. JoinPending = FKCL StartFakeSession began a
    /// join and the words are NOT a reply yet: call PollPending(cmd) until it returns true. `sbuf` is the thread's static buffer
    /// pair (word 0 the descriptor, word 1 the address) or null. Never called with a lock held.
    DlpOutcome Serve(DlpKind kind, u32* cmd, const u32* sbuf);
    /// After JoinPending: true when the join ended and `cmd` holds the reply (the lobby decides when, on its own clock).
    bool PollPending(u32* cmd);
    /// The game is about to run nwm::UDS Initialize: gives the engine back, in order, and returns when it is free. Before anything
    /// of the game's session touches the engine (UdsHle::Dispatch, first thing in cases 0x01 / 0x1B).
    void OnGameUdsStarting();
    /// The game closed a dlp session that had called Initialize, without Finalize: the role's Finalize, done by the network thread (a
    /// posted request). If no network thread takes it the close is left for its next Tick (deferred_closes()): nothing of the lobby's
    /// network side runs on the calling thread.
    void OnSessionClosed(DlpKind kind);
    /// The name every list shows for this console's player when the game gives none (kNameUnits UTF-16 units, zero padded): set once by
    /// the plugin when it knows the console's profile name, long before a game opens the wireless menu. Takes dlp_mutex_ briefly.
    void SetPlayerName(const u16* name);
    /// True while the lobby holds the engine (and no game session does). Lock-free.
    bool EngineHeld() const {
        return engine_held_.load(std::memory_order_relaxed);
    }

    // ---- the network thread --------------------------------------------------------------------------------------------------
    /// Once per round of the network thread, after the engine's own Tick: serves the request in the slot and pumps the lobby.
    void Tick();

    // ---- the end of the plugin ---------------------------------------------------------------------------------------------
    /// The network thread will not tick again: a request in the queue is answered with the failure, later ones fail at once.
    void Stop();

    // ---- counters (diagnostics; the HLE logs them) ------------------------------------------------------------------------
    u32 served_inline() const {
        return inline_.load(std::memory_order_relaxed);
    }
    u32 served_posted() const {
        return posted_.load(std::memory_order_relaxed);
    }
    u32 cancelled() const {  // requests not served in kQueuedMs
        return cancelled_.load(std::memory_order_relaxed);
    }
    u32 failed_fast() const {  // requests refused because no network thread was alive
        return failed_fast_.load(std::memory_order_relaxed);
    }
    u32 missed_ticks() const {  // rounds that could not take dlp_mutex_
        return missed_.load(std::memory_order_relaxed);
    }
    u32 ran_here() const {  // OnGameUdsStarting done on the calling thread (no network thread took it)
        return ran_here_.load(std::memory_order_relaxed);
    }
    u32 deferred_closes() const {  // OnSessionClosed calls no network thread took: ended by a later Tick
        return deferred_.load(std::memory_order_relaxed);
    }

private:
    enum Op : u8 { kOpDispatch = 1, kOpGameUds = 2, kOpClosed = 3 };
    // The states of the slot. Free -> Queued (the requester) -> Running (the network thread) -> Done (the network thread) -> Free
    // (the requester); or Queued -> Free (the requester cancels) or Queued -> Done (Stop).
    static constexpr u32 kFree = 0;
    static constexpr u32 kQueued = 1;
    static constexpr u32 kRunning = 2;
    static constexpr u32 kDone = 3;
    static constexpr u32 kClosingFkcl = 1;  // closing_: the client role's session was closed
    static constexpr u32 kClosingSrvr = 2;  // ... the creator's

    struct Slot {  // the one request in flight (post_mutex_)
        u32* cmd;
        const u32* sbuf;
        DlpOutcome outcome;
        DlpKind kind;
        Op op;
        bool ok;  // the network thread ran it
    };

    bool NetAlive() const;
    /// Records that the network thread is running now (Tick, at its start and at its end).
    void Stamp();
    /// Leaves the request in the slot and waits. true = it ran (`*outcome` set); false = it did not (failure to answer).
    bool Post(Op op, DlpKind kind, u32* cmd, const u32* sbuf, DlpOutcome* outcome);
    /// Answers a request that was never run (the network thread could not serve it): the success of the commands that cannot fail, the failure of
    /// the others. Out of line: its locals are not paid by the commands the game thread serves.
    DlpOutcome AnswerUnserved(DlpKind kind, u32* cmd);
    /// True while a requester holds post_mutex_ (a request is in flight). Out of line, like the one above.
    bool RequestInFlight();
    /// The failure reply of a command: [(id << 16) | 0x40, ResultWirelessOff].
    static void WriteFailure(u32* cmd, u32 id);
    /// The commands that succeed in every state (Finalize, StopScan, StopSession of FKCL; Finalize, EndAccepting of SRVR): a request for
    /// the network thread that nobody served is answered with success, not with the failure.
    static bool AlwaysSucceeds(DlpKind kind, u32 id);
    /// ... of those, the ones that end the role of the service (the next Tick does it).
    static bool EndsRole(DlpKind kind, u32 id);
    /// Runs the slot's request (the network thread's, dlp_mutex_ held, SetOnNet(true)).
    void Execute();
    /// A request of the slot done on the calling thread: dlp_mutex_ held by this function. Only OnGameUdsStarting is asked for it now
    /// (the end of a session is deferred, see OnSessionClosed); the other branch is kept so that the function is whole.
    void RunHere(Op op, DlpKind kind);
    /// The roles whose sessions were closed while no network thread served (the network thread's Tick, dlp_mutex_ held).
    void RunDeferred();
    /// Gives the engine back whatever the lobby's state (after OnGameUdsStarting).
    void FreeEngine();
    /// The lock-free hints after any change of state (dlp_mutex_ held).
    void Refresh();

    EngineLobbyNet net_;
    DlpLobby lobby_;
    DlpService svc_;
    IpcResolver resolver_;
    KernelOps* kernel_;
    u32 event_ = 0;
    platform::Mutex dlp_mutex_;   // the service, the lobby and the adapter
    platform::Mutex post_mutex_;  // one requester at a time
    Slot slot_ = {};
    std::atomic<u32> state_{kFree};
    std::atomic<u32> last_tick_ms_{0};  // 0 = never
    std::atomic<bool> stopped_{false};
    std::atomic<bool> active_{false};        // the lobby or the adapter holds something: Tick has work even with no request
    std::atomic<bool> engine_held_{false};
    std::atomic<u32> inline_{0};
    std::atomic<u32> posted_{0};
    std::atomic<u32> cancelled_{0};
    std::atomic<u32> failed_fast_{0};
    std::atomic<u32> missed_{0};
    std::atomic<u32> ran_here_{0};
    std::atomic<u32> closing_{0};   // kClosingFkcl | kClosingSrvr: sessions closed while no network thread served: the next Tick ends their roles
    std::atomic<u32> deferred_{0};
};

} // namespace azl
