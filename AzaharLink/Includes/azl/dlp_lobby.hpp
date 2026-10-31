// The logic of the fake Download Play lobby: the two state machines of the games' dlp:FKCL (the searcher) and dlp:SRVR
// (the creator), over a virtual network the adapter gives us (LobbyNet). Design: analysis/dlp-lobby-fake-2026-10-28/
// DESIGN-FASE1.md (binding) and SPEC-DLP.md. Pure C++: no kernel, no engine, no file, no log, no heap, no thread.
//
// HARD LIMITS (rules of the user): at most 4 players (host + 3 clients), ever; minimum memory; minimum stack.
//
// MEMORY. One DlpLobby is a fixed object with NO heap (no vector / string / new / function / exception / RTTI). The 5032-byte
// and 664-byte records of the games are NOT kept: the IPC front-end composes them in the game's own buffer from a
// Group (40 bytes) or from a Track. The balance, in bytes (x86-64 and ARM32 measured, both 272; i386 not compiled here,
// computed about 264), the bound being static_assert(sizeof <= 320):
//     LobbyNet* net_ 8 (4) | u64 filter_ 8 | Identity id_ 32 | Tables u_ 168 (a union of ONE of three shapes, chosen by
//     tab_: Scan 4 x 40 = 160, Session 40 + 4 x 32 = 168, Host 3 x 8 = 24) | 7 x u32 28 | join_mac_ 6 | my_node_ 1 |
//     go_ 11 (the passphrase kept AS the GO message {3, 9, nine bytes}: it is sent from there, no buffer on a stack) |
//     10 x u8 (states, use_, tab_, limits, cursor_, flags_, flags2_) | padding = 272.
//     The searcher and the creator never hold the network together, which is why one table shape is enough.
// STACK. Every function runs on the game thread that made the IPC call, some of which have 464 bytes below the call. No
// local above 40 bytes (the largest: a HeardGroup, 36), no recursion, and the frames are kept apart so that the CHAIN of
// frames at a call into LobbyNet stays short: a frame that holds a buffer is not inlined into its callers, a one-line helper
// is always inlined, and the messages without a body are constant arrays (nothing built on a stack). Measured with
// arm-none-eabi-g++ 16.1 (devkitARM), the flags of the Makefile (-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
// -Os -fomit-frame-pointer -fno-rtti -fno-exceptions -std=gnu++17), -fstack-usage and -fcallgraph-info=su: no function
// frame above 64 bytes (PollScan, which holds the HeardGroup; every other one is 48 or less), and the DEEPEST CHAIN OF
// FRAMES AT A CALL INTO LobbyNet is 96 bytes below Pump / Poll (Poll 8 + PollClient 24 + PollScan 64, or Poll 8 +
// PollServer 32 + Advance 8 + SendPhase 48), 112 below the commands that pump first (their own 16) and 120 below
// ServerDisconnectClient (24). Each public function's comment gives "frame / chain": its own frame and the deepest chain
// from its entry (own frame included). (These are the numbers of Tests/dlp_stack_probe.cpp, which calls every public function
// through a noinline wrapper; Tests/dlp_stack_check.sh, run by Tests/run.sh, compiles it for ARM32 with the flags above and
// FAILS on a frame over 48 bytes (64 for PollScan and DlpService::Dispatch), a dynamic frame, a recursion, a chain over 160
// bytes or a size over its assertion. The inlining in the plugin's own translation unit decides the real numbers: phase 3
// measures them again.)
// LOCKING. Nothing here blocks and nothing is thread-safe: the caller serialises every call (Pump included) with ONE
// mutex. No method of LobbyNet may be called without it.
// TIME. Every instant is a u32 = (u32)NowMs(); an age is (s32)(now - stamp) taken as 0 if negative; a deadline is
// `age >= limit`, never `now >= stamp + limit`. A timer that is not set has a flag, never the value 0.
#pragma once

#include "azl/common.hpp"
#include "azl/dlp_types.hpp"
#include "azl/ticks.h"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

// Private to this header (undefined at its end). The stack of a game thread is counted along a CHAIN of calls, so a frame that
// holds a buffer is kept out of the callers' frames (noinline), and one-line helpers cost no frame at all (always_inline).
#define DLP_NOINLINE __attribute__((noinline))
#define DLP_ALWAYS_INLINE inline __attribute__((always_inline))

/// The virtual network as the lobby sees it. The adapter (phase 2) implements it on the UDS engine, the tests on an
/// in-memory air. Contract: nothing blocks; HostClose() and Disconnect() are synchronous with respect to the engine;
/// Receive() delivers only messages sent by a node of OUR virtual network, with from_node authenticated by the adapter
/// (never read from the body); Send() is always unicast to one node 1..4; HeardGroup::host is the MAC the creator's own
/// nwm::UDS will use for the game's network; ScanRead(i) is the i-th lobby heard FRESH now, of any game (the lobby
/// filters by game); Members() and ReadNode() read the engine's state on the host and on the client alike.
class LobbyNet {
public:
    virtual u64 NowMs() = 0;     // monotonic, never the wall clock
    virtual u64 NowTicks() = 0;  // svcGetSystemTick-based, same source as NowMs
    // creator
    virtual bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) = 0;
    virtual void HostUpdate(const HostAnnounce& a) = 0;  // the announcement changed (accepting)
    virtual void HostClose() = 0;                        // synchronous: stops announcing, drops everybody
    virtual void Kick(u8 node) = 0;                      // ejects one client (an id that is not there: ignored)
    // searcher
    virtual bool ScanOpen() = 0;                         // idempotent; false = cannot
    virtual void ScanClose() = 0;
    virtual bool ScanRead(u32 index, HeardGroup* out) = 0;  // false = no more
    virtual bool ConnectBegin(const Mac& host, const Identity& me) = 0;  // starts joining; false = cannot even start
    virtual int ConnectPoll() = 0;                       // 0 = still joining, 1 = joined, -1 = failed
    virtual void Disconnect() = 0;                       // synchronous: leaves / abandons the join
    // both
    virtual u8 MyNode() = 0;                             // our node id (host = 1), 0 = not in a network
    virtual u32 Members(u8 (&nodes)[kMaxPlayers]) = 0;   // node ids present now, host included, ascending
    virtual bool ReadNode(u8 node, NodeEntry* out) = 0;  // identity of one present node
    virtual bool Send(u8 to_node, const u8* data, u32 size) = 0;  // unicast to ONE node 1..4; size <= kMaxMessage
    virtual bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) = 0;  // pops one message; false = none

protected:
    ~LobbyNet() = default;  // nobody deletes through the interface
};

struct LobbyConfig {  // given once, before the first Initialize: everything the game does not say
    u64 game_tid;     // title id of the running game (only the low 32 bits are used)
    u64 friend_seed;  // the local friend code seed (the adapter makes it unique per console; 0 is allowed)
    u16 name[kNameUnits];  // the default player name (InitializeWithName overrides it)
};

struct Group {          // a lobby heard (40 bytes, no u64: the same on ARM32 and x86-64)
    Mac host;           // +0
    u8 flags;           // +6: bit 0 = in use
    u8 max_nodes;       // +7
    u32 child_tid_low;  // +8
    u32 last_seen_ms;   // +12
    u8 total_nodes;     // +16
    u8 signal;          // +17
    u16 name[kNameUnits];  // +18
};
static_assert(sizeof(Group) == 40, "Group is 40 bytes");

struct GroupView {      // a read-only view of ONE table entry; valid until the next Poll / state change
    const Group* group;
    u64 tick;           // the value for ServerInfo+0x290: NowTicks() minus the age of the entry, saturated at 0
};

enum class JoinStep : u8 { Failed, Pending };

class DlpLobby {
public:
    // Times (ms). See DESIGN sez. 4.4.
    static constexpr u32 kPollMs = 20;          // fastest rate of the network work of Poll(false)
    static constexpr u32 kMaxRx = 8;            // messages per Poll
    static constexpr u32 kGroupTtlMs = 1500;    // an entry ScanRead no longer reports expires (state 2 only)
    static constexpr u32 kJoinMs = 3000;        // from ConnectBegin to the end of the join
    static constexpr u32 kHostGoneMs = 2000;    // the host absent for the searcher in 5/6; our own network lost
    static constexpr u32 kResendMs = 200;       // resend of START / GO
    static constexpr u32 kStartGraceMs = 1500;  // from StartDistribution: clients without START_ACK are removed
    static constexpr u32 kGoGraceMs = 2000;     // from SendPassphrase: state 9 anyway
    static constexpr u32 kRosterMs = 250;       // re-read of the searcher's roster in 5/6
    static_assert(kGroupTtlMs < 5000, "the game drops the entries older than 5000 ms");
    static_assert(kResendMs * 2 < kStartGraceMs && kResendMs * 2 < kGoGraceMs, "at least two resends before the grace");
    static_assert(kPollMs <= kResendMs, "Poll must run at least once per resend period");

    // A client of the creator. state: 0 free, 1 waiting for acceptance, 2 accepted, 3 START_ACK received, 4 GO_ACK
    // received, 5 ejected (waits for the engine to remove it). The slot is node - 2 (clients are nodes 2..4).
    struct Track {
        u32 sent_ms;  // when START / GO / Kick was last sent to it
        u8 state;
    };
    struct ScanTable {
        Group g[kMaxGroups];
    };
    struct SessionTable {
        Group host;                     // the host we joined
        NodeEntry roster[kMaxPlayers];  // the players, compact from index 0 (node 0 = empty)
    };
    struct HostTable {
        Track t[kMaxClients];
    };
    union Tables {
        ScanTable scan;
        SessionTable session;
        HostTable host;
    };
    static_assert(sizeof(ScanTable::g) / sizeof(Group) == kMaxGroups, "the scan table holds kMaxGroups entries");
    static_assert(sizeof(SessionTable::roster) / sizeof(NodeEntry) == kMaxPlayers, "the roster holds kMaxPlayers");
    static_assert(sizeof(HostTable::t) / sizeof(Track) == kMaxClients, "the creator tracks kMaxClients clients");
    static_assert(sizeof(Tables) <= 168, "one of three shapes: at most 168 bytes");

    /// Builds the lobby. `net` outlives it. Cost: one memset of the table and a name copy; no call into LobbyNet.
    DlpLobby(LobbyNet* net, const LobbyConfig& cfg) : net_(net) {
        std::memset(&u_, 0, sizeof(u_));
        id_.seed = cfg.friend_seed;
        CopyName(id_.name, cfg.name);
        tid_low_ = static_cast<u32>(cfg.game_tid);
    }
    DlpLobby(const DlpLobby&) = delete;
    DlpLobby& operator=(const DlpLobby&) = delete;

    // ===========================================================================================================
    // The searcher (dlp:FKCL). Internal states: 0 off, 1 idle, 2 scanning, 3 joining (shown as 1), 5 in session,
    // 6 started, 9 complete (final), 0x80 lost. Never 7.

    /// FKCL Initialize / InitializeWithName. Always 0. Tears down whatever the searcher holds (ScanClose or Disconnect,
    /// a join in progress is cancelled, table and passphrase cleared), then state 1. A given name replaces the current
    /// one; max_beacons is limited to 1..4 (0 counts as 1). Twice in a row is allowed.
    /// Stack (ARM, -Os): frame 16 B, chain 40 B.
    u32 ClientInitialize(const u16* name_or_null, u32 max_beacons) {
        ClientReset();
        cli_state_ = kCliIdle;
        cursor_ = 0;
        filter_ = 0;
        max_groups_ = static_cast<u8>(max_beacons < 1 ? 1 : (max_beacons > kMaxGroups ? kMaxGroups : max_beacons));
        if (name_or_null != nullptr) {
            CopyName(id_.name, name_or_null);
        }
        return 0;
    }

    /// FKCL Finalize. Always 0. State 0 stays 0; otherwise the same teardown and state 1 (as Azahar). The network is
    /// touched only if the searcher holds it (the creator's network is never closed by it).
    /// Stack (ARM, -Os): frame 8 B, chain 32 B.
    u32 ClientFinalize() {
        if (cli_state_ != kCliOff) {
            ClientReset();
            cli_state_ = kCliIdle;
            cursor_ = 0;
        }
        return 0;
    }

    /// FKCL StartScan. Not in state 1 -> kInvalidState; the creator holds the network -> kBusy; ScanOpen() false ->
    /// kBusy; else clears the table and the cursor, keeps the filter, state 2. Does NOT check the channel value and
    /// ignores the MAC filter (as Azahar); the title filter ignores its low byte (the child index).
    /// Stack (ARM, -Os): frame 8 B, chain 8 B.
    u32 ClientStartScan(u64 tid_filter) {
        if (cli_state_ != kCliIdle) {
            return kInvalidState;
        }
        if (use_ == kUseServer || !net_->ScanOpen()) {
            return kBusy;
        }
        ClearTables();
        tab_ = kTabScan;
        use_ = kUseClient;
        cursor_ = 0;
        filter_ = tid_filter;
        cli_state_ = kCliScanning;
        return 0;
    }

    /// FKCL StopScan. Always 0. From 2: ScanClose() and state 1; the table STAYS and freezes (no expiry, no update) until
    /// StartScan / Finalize / Initialize. In any other state nothing changes. Stack (ARM, -Os): frame 8 B, chain 16 B.
    u32 ClientStopScan() {
        if (cli_state_ == kCliScanning) {
            ClientRelease();
            cli_state_ = kCliIdle;
        }
        return 0;
    }

    /// FKCL GetTitleInfo: the entry of `host` in the table (states 1, 2, 3) or, in a session (5, 6, 9), the host joined
    /// (tick = now). Else kNoData; never kInvalidState. out must not be null. Stack (ARM, -Os): frame 0 B, chain 40 B.
    u32 ClientTitleInfo(const Mac& host, GroupView* out) {
        return ViewOf(host, out);
    }

    /// FKCL GetServerInfo: the same lookup as ClientTitleInfo. Stack (ARM, -Os): frame 0 B, chain 40 B.
    u32 ClientServerInfo(const Mac& host, GroupView* out) {
        return ViewOf(host, out);
    }

    /// FKCL GetTitleInfoInOrder. States 1, 2, 3: `restart` zeroes the cursor (a SLOT index 0..3); returns the next used
    /// slot from the cursor and moves it past; at the end of the list only kNoData (the game loops until exactly that).
    /// Slots are fixed: an expiry or DeleteScanInfo does not move the cursor. Other states: kNoData. Never
    /// kInvalidState. Stack (ARM, -Os): frame 8 B, chain 32 B.
    u32 ClientTitleInfoInOrder(bool restart, GroupView* out) {
        if (restart) {
            cursor_ = 0;
        }
        if (tab_ == kTabScan && (cli_state_ == kCliIdle || cli_state_ == kCliScanning || cli_state_ == kCliJoining)) {
            while (cursor_ < kMaxGroups) {
                const Group* g = &u_.scan.g[cursor_++];
                if ((g->flags & kGroupUsed) != 0) {
                    MakeView(g, false, out);
                    return 0;
                }
            }
        }
        return kNoData;
    }

    /// FKCL DeleteScanInfo. States 1 and 2: removes the entry only (no ignore list: a lobby still alive comes back at
    /// the next round); unknown MAC or any other state -> kNoData. Stack (ARM, -Os): frame 8 B, no call into LobbyNet.
    u32 ClientDeleteScanInfo(const Mac& host) {
        Group* g = (cli_state_ == kCliIdle || cli_state_ == kCliScanning) ? FindScan(host) : nullptr;
        if (g == nullptr) {
            return kNoData;
        }
        std::memset(g, 0, sizeof(Group));
        return 0;
    }

    /// FKCL StartFakeSession, first half. Order of the checks: state not 1 (Joining included) -> kInvalidState; MAC not in
    /// the table -> kNoData (both Failed at once, *result written); the creator holds the network -> kBusy (a defensive
    /// check that cannot be reached: taking the network for the creator discards the scan table, so with the creator active
    /// the MAC is never in the table and the answer is the kNoData above); ConnectBegin() false -> kNotFound (0xC880AC05). Else state 3, an old unread outcome is dropped, Pending. The
    /// outcome comes from Poll (ClientJoinPoll). The child title id the command carries is ignored.
    /// Stack (ARM, -Os): frame 16 B, chain 16 B.
    JoinStep ClientJoinBegin(const Mac& host, u32* result) {
        const u32 r = JoinChecks(host);
        if (r != 0) {
            *result = r;
            return JoinStep::Failed;
        }
        cli_state_ = kCliJoining;
        use_ = kUseClient;
        join_mac_ = host;
        t0_ = Now();
        flags_ = static_cast<u8>((flags_ | kFlagJoinActive) & ~(kFlagJoinReady | kFlagGone));
        return JoinStep::Pending;
    }

    /// FKCL StartFakeSession, second half (a Poll(true), then): true = finished, *result written and handed over once; with
    /// no join and no outcome to collect: true with kInvalidState; join still running: false. The end is decided by Poll,
    /// not by the caller: a join nobody asks about times out by itself. Stack (ARM, -Os): frame 16 B, chain 112 B.
    bool ClientJoinPoll(u32* result) {
        Poll(true);
        if ((flags_ & kFlagJoinActive) != 0) {
            return false;
        }
        if ((flags_ & kFlagJoinReady) != 0) {
            *result = join_result_;
            flags_ = static_cast<u8>(flags_ & ~kFlagJoinReady);
        } else {
            *result = kInvalidState;
        }
        return true;
    }

    /// FKCL StopSession. Always 0. In a session or a join (3, 5, 6, 9) and from 0x80: Disconnect (if we hold the network),
    /// state 1, table and passphrase cleared, a join in progress is cancelled. From 0, 1, 2: nothing.
    /// Stack (ARM, -Os): frame 8 B, chain 32 B.
    u32 ClientStopSession() {
        if (cli_state_ >= kCliJoining) {
            ClientReset();
            cli_state_ = kCliIdle;
        }
        return 0;
    }

    /// FKCL GetMyStatus first word: state<<24 | connected<<16 | node. Joining shows as 1; connected and node only in
    /// 5, 6, 9; from 0x80 both are 0. Never fails, in no state. Stack (ARM, -Os): frame 0 B, no call into LobbyNet.
    u32 ClientStatusWord() const {
        const u32 shown = cli_state_ == kCliJoining ? kCliIdle : cli_state_;
        const bool in = InSession();
        return (shown << 24) | ((in ? 1u : 0u) << 16) | (in ? my_node_ : 0u);
    }

    /// FKCL GetConnectingNodes. In 5, 6, 9 the ids of the session roster (host included, ascending), *total = how many;
    /// elsewhere success with *total = 0. Writes min(*total, capacity) ids (capacity <= 4).
    /// Stack (ARM, -Os): frame 4 B, no call into LobbyNet.
    u32 ClientConnectingNodes(u16* ids, u32 capacity, u32* total) const {
        u32 count = 0;
        if (InSession() && tab_ == kTabSession) {
            for (u32 i = 0; i < kMaxPlayers; ++i) {
                if (u_.session.roster[i].node != 0) {
                    if (count < capacity) {
                        ids[count] = u_.session.roster[i].node;
                    }
                    ++count;
                }
            }
        }
        *total = count;
        return 0;
    }

    /// FKCL GetNodeInfo. In 5, 6, 9 the roster entry of `node` (the engine's raw seed and name); an id above 0xFF, an
    /// unknown id or another state -> kNoData. Stack (ARM, -Os): frame 4 B, no call into LobbyNet.
    u32 ClientNodeInfo(u16 node, NodeEntry* out) const {
        if (InSession() && tab_ == kTabSession && node >= 1 && node <= 0xFF) {
            for (u32 i = 0; i < kMaxPlayers; ++i) {
                if (u_.session.roster[i].node == node) {
                    *out = u_.session.roster[i];
                    return 0;
                }
            }
        }
        return kNoData;
    }

    /// FKCL GetWirelessRebootPassphrase. Always 0; the 9 bytes received with GO (zeros before), valid until Finalize /
    /// StopSession / Initialize. Stack (ARM, -Os): frame 0 B, no call into LobbyNet.
    u32 ClientPassphrase(u8 out[kPassphraseSize]) const {
        for (u32 i = 0; i < kPassphraseSize; ++i) {
            out[i] = (flags_ & kFlagPassServer) == 0 ? go_[2 + i] : 0;
        }
        return 0;
    }

    // ===========================================================================================================
    // The creator (dlp:SRVR). States: 0 off, 1 idle (also after EndAccepting), 2 accepting, 6 distributing, 8 passphrase,
    // 9 complete.

    /// SRVR Initialize / InitializeWithName. Always 0. Tears down the creator's own state (HostClose if its network is
    /// open), then state 1. max_clients is limited to 1..3 (0 counts as 1: our choice, the real module takes 1..15); the
    /// child index is the low byte; a given name replaces the current one. The searcher is not touched.
    /// Stack (ARM, -Os): frame 16 B, chain 24 B.
    u32 ServerInitialize(u32 max_clients, u32 child_index, const u16* name_or_null) {
        ServerReset();
        srv_state_ = kSrvIdle;
        max_clients_ = static_cast<u8>(max_clients < 1 ? 1 : (max_clients > kMaxClients ? kMaxClients : max_clients));
        child_index_ = static_cast<u8>(child_index & 0xFF);
        if (name_or_null != nullptr) {
            CopyName(id_.name, name_or_null);
        }
        return 0;
    }

    /// SRVR Finalize. Always 0, in every state. Closes the network if the creator holds it, clears the table and the
    /// passphrase it owns, state 0 (Azahar's defect 4 is not copied). Stack (ARM, -Os): frame 8 B, chain 16 B.
    u32 ServerFinalize() {
        ServerReset();
        srv_state_ = kSrvOff;
        return 0;
    }

    /// SRVR StartAccepting. Checks in this order: channel not in {0, 1, 6, 11} -> kOutOfRange; state not 1 ->
    /// kInvalidState (also "already 2"); the searcher holds the network -> kBusy. Then, if the network is already open
    /// (state 1 after EndAccepting) HostUpdate(accepting = 1), else HostOpen(announce, channel, identity) (false ->
    /// kBusy, state stays 1). State 2. The announcement is built in a frame of its own (Announce), so this one stays small.
    /// Stack (ARM, -Os): frame 16 B, chain 64 B (Announce 48 B).
    u32 ServerStartAccepting(bool manual, u32 channel) {
        if (channel != 0 && channel != 1 && channel != 6 && channel != 11) {
            return kOutOfRange;
        }
        if (srv_state_ != kSrvIdle) {
            return kInvalidState;
        }
        if (use_ == kUseClient) {
            return kBusy;
        }
        const bool was_open = (flags_ & kFlagHostOpen) != 0;
        if (!Announce(1, channel)) {
            return kBusy;
        }
        if (!was_open) {
            ClearTables();
            tab_ = kTabHost;
            use_ = kUseServer;
            flags_ |= kFlagHostOpen;
        }
        SetFlag(kFlagManual, manual);
        flags_ = static_cast<u8>(flags_ & ~kFlagGone);
        srv_state_ = kSrvAccepting;
        return 0;
    }

    /// (deviation: Poll(true) first, so the clients that were admitted a moment ago are tracked and STAY.)
    /// SRVR EndAccepting. Always 0. From 2: HostUpdate(accepting = 0) and state 1; the clients already in STAY and the
    /// network stays open. Elsewhere nothing changes. Stack (ARM, -Os): frame 8 B, chain 104 B.
    u32 ServerEndAccepting() {
        if (srv_state_ == kSrvAccepting) {
            Poll(true);  // (the clients that just came in are tracked before the door shuts: they stay)
            EndAccepting();
        }
        return 0;
    }

    /// SRVR GetServerState: the byte (also 0). Never fails. Stack (ARM, -Os): frame 0 B, no call into LobbyNet.
    u32 ServerState() const {
        return srv_state_;
    }

    /// SRVR GetConnectingClients. State 0 -> kInvalidState; else the ids of the tracked clients still present
    /// (ascending, host excluded), *total = how many (at most 3); in state 9 *total = 0. Writes min(*total, capacity)
    /// ids (capacity <= 4). Stack (ARM, -Os): frame 4 B, no call into LobbyNet.
    u32 ServerConnectingClients(u16* ids, u32 capacity, u32* total) const {
        if (srv_state_ == kSrvOff) {
            return kInvalidState;
        }
        u32 count = 0;
        if (tab_ == kTabHost && srv_state_ != kSrvComplete) {
            for (u32 s = 0; s < kMaxClients; ++s) {
                if (Listed(u_.host.t[s].state)) {
                    if (count < capacity) {
                        ids[count] = static_cast<u16>(s + kFirstClientNode);
                    }
                    ++count;
                }
            }
        }
        *total = count;
        return 0;
    }

    /// SRVR GetClientInfo. State 0 -> kInvalidState. A tracked client: 0 with the engine's NodeEntry (a failing ReadNode
    /// leaves seed and name at zero). Node 1 (the host): kNoData with `out` FILLED with the host's identity (Azahar's
    /// convention). Anything else (also an id above 0xFF): kNoData with out->node == 0. `out->node` is 0 on every return
    /// except the host's record (also with kInvalidState): it is set before the first test.
    /// Stack (ARM, -Os): frame 8 B, chain 8 B.
    u32 ServerClientInfo(u16 node, NodeEntry* out) {
        out->node = 0;  // (set first, in every state: the caller tells "the host's record" from "nothing" by it)
        if (srv_state_ == kSrvOff) {
            return kInvalidState;
        }
        if (node == 1) {
            out->seed = id_.seed;
            CopyName(out->name, id_.name);
            out->node = 1;
            return kNoData;
        }
        if (TrackOf(node) == nullptr) {
            return kNoData;
        }
        if (!net_->ReadNode(static_cast<u8>(node), out)) {
            out->seed = 0;
            std::memset(out->name, 0, sizeof(out->name));
        }
        out->node = static_cast<u8>(node);
        return 0;
    }

    /// SRVR GetClientState. State 0 -> kInvalidState; a node not tracked (the host, an id out of range, above 0xFF) ->
    /// kNoData (Azahar's defect 3 is not copied). *word = external<<24 | joined<<16 | node with external: waiting 0x40,
    /// accepted 5 (6 once the distribution started), START_ACK received or GO sent 7, GO_ACK received 9.
    /// Stack (ARM, -Os): frame 0 B, no call into LobbyNet.
    u32 ServerClientState(u16 node, u32* word) const {
        if (srv_state_ == kSrvOff) {
            return kInvalidState;
        }
        const Track* t = TrackOf(node);
        if (t == nullptr) {
            return kNoData;
        }
        u32 ext = kPeerAccepted;
        u32 joined = 1;
        if (t->state == kTrkPending) {
            ext = kPeerWaiting;
            joined = 0;
        } else if (t->state == kTrkAccepted && srv_state_ >= kSrvDistributing) {
            ext = kPeerStarting;
        } else if (t->state == kTrkStartAcked) {
            ext = kPeerDistributed;
        } else if (t->state == kTrkGoAcked) {
            ext = kPeerComplete;
        }
        *word = (ext << 24) | (joined << 16) | node;
        return 0;
    }

    /// (deviation: Poll(true) first, like the commands the design lists, so a client that just came in is already tracked.)
    /// SRVR AcceptClient. State 0 -> kInvalidState; a node waiting for acceptance (0x40) becomes accepted -> 0; anything
    /// else (unknown, already accepted, the host) -> kNoData (not -1). Stack (ARM, -Os): frame 16 B, chain 112 B.
    u32 ServerAcceptClient(u16 node) {
        if (srv_state_ == kSrvOff) {
            return kInvalidState;
        }
        Poll(true);
        Track* t = TrackOf(node);
        if (t == nullptr || t->state != kTrkPending) {
            return kNoData;
        }
        t->state = kTrkAccepted;
        return 0;
    }

    /// (deviation: Poll(true) first, as AcceptClient.)
    /// SRVR DisconnectClient. State 0 -> kInvalidState; a tracked node: Kick(node), the slot becomes "ejected" (it frees
    /// when the engine no longer lists the node, so a node that lingers one more round does not come back as a new
    /// client) -> 0; unknown -> kNoData (not -1). Stack (ARM, -Os): frame 24 B, chain 120 B.
    u32 ServerDisconnectClient(u16 node) {
        if (srv_state_ == kSrvOff) {
            return kInvalidState;
        }
        Poll(true);
        Track* t = TrackOf(node);
        if (t == nullptr) {
            return kNoData;
        }
        const u32 now = Now();
        Eject(t, static_cast<u8>(node), now);
        Advance(now);
        return 0;
    }

    /// SRVR StartDistribution. Not state 2 -> kInvalidState. Ejects the clients still waiting for acceptance, announces
    /// accepting = 0, state 6, sends START to every accepted client; with none, state 8 at once.
    /// Stack (ARM, -Os): frame 8 B, chain 104 B.
    u32 ServerStartDistribution() {
        Poll(true);
        return StartDistribution();
    }

    /// SRVR SendWirelessRebootPassphrase. Not state 8, or the passphrase already sent -> kInvalidState (our choice, not
    /// -1; the FIRST call wins: the game calls it every frame in state 8 with new random bytes, the others touch
    /// nothing). Else keeps the 9 bytes, sends GO to the clients left, 0; with none, state 9 in this very call.
    /// Stack (ARM, -Os): frame 16 B, chain 112 B.
    u32 ServerSendPassphrase(const u8* nine) {
        Poll(true);
        if (srv_state_ != kSrvPassphrase || (flags_ & kFlagGoSent) != 0) {
            return kInvalidState;
        }
        const u32 now = Now();
        go_[0] = static_cast<u8>(MsgType::Go);
        go_[1] = kPassphraseSize;
        for (u32 i = 0; i < kPassphraseSize; ++i) {
            go_[2 + i] = nine[i];
        }
        flags_ = static_cast<u8>(flags_ | kFlagPassServer | kFlagGoSent);
        t0_ = now;
        if (SendPhase(true, false, now) == 0) {
            Complete();  // nobody left to wait for
        }
        return 0;
    }

    // ===========================================================================================================
    // Events from outside, and the pump.

    /// The game is about to run nwm::UDS Initialize: gives the network back, in order, and returns when the engine is
    /// free (synchronous, never blocks, never fails). The "queue" is the game's own call: the caller runs this before it
    /// lets the Initialize go on. Searcher: scanning -> ScanClose, state 1 (table frozen); joining -> Disconnect, join
    /// cancelled (outcome kInvalidState), state 1, the frozen scan table STAYS (as after a failed join: the same MAC can be
    /// tried again); session 5/6 -> Disconnect, state 1, tables cleared, an outcome not yet collected is dropped; 9 -> Disconnect but it STAYS 9, the
    /// passphrase and the roster stay readable. Creator in 1 (network open), 2, 6, 8: HostClose, state 1, table cleared
    /// (in 9 the network is already closed). Returns true if it released something; the second call returns false.
    /// Stack (ARM, -Os): frame 16 B, chain 40 B.
    bool OnGameUdsStarting() {
        bool released = false;
        if (use_ == kUseClient) {
            released = true;
            if (cli_state_ == kCliComplete) {
                ClientRelease();
            } else if (cli_state_ == kCliScanning || cli_state_ == kCliJoining) {
                ClientRelease();
                flags_ = static_cast<u8>(flags_ & ~kFlagJoinActive);
                cli_state_ = kCliIdle;
            } else {
                ClientReset();
                cli_state_ = kCliIdle;
            }
        }
        if (use_ == kUseServer) {
            ServerReset();
            srv_state_ = kSrvIdle;
            released = true;
        }
        return released;
    }

    /// The network work (Poll(false)): at most once per kPollMs. Stack (ARM, -Os): frame 8 B, chain 96 B (that of Poll).
    void Pump() {
        Poll(false);
    }

    /// True while the lobby holds the network.
    bool active() const {
        return use_ != kUseNone;
    }

    /// The plugin's mode (phase 3, azl/dlp_hub.hpp): the network work is the network thread's and nobody else's. With it on,
    /// Poll() returns at once unless the caller has said it is that thread (SetOnNet), so the commands that read the tables
    /// (and the pump at the start of every Dispatch) call nothing of LobbyNet but the clock on a game thread. Off by default:
    /// the lobby polls where it is called, as every test of phases 1 and 2 expects. Both are set under the caller's lock.
    void SetExternalPoll(bool on) {
        SetF2(kF2External, on);
    }
    /// The name of the player until the game says another one (InitializeWithName): kNameUnits UTF-16 units. The games send Initialize
    /// with no name (Kirby Triple Deluxe, Planet Robobot), so this is the name every list shows: the plugin sets the console's profile name.
    void SetDefaultName(const u16* name) {
        CopyName(id_.name, name);
    }
    void SetOnNet(bool on) {
        SetF2(kF2OnNet, on);
    }

    /// Messages refused (bad format, wrong role, wrong sender), saturating.
    u32 bad_messages() const {
        return bad_messages_;
    }

    /// Raw states for the tests and the log: the searcher (Joining visible) and the creator.
    u8 client_state() const {
        return cli_state_;
    }
    u8 server_state() const {
        return srv_state_;
    }
    /// True from ClientJoinBegin until the join ends.
    bool join_active() const {
        return (flags_ & kFlagJoinActive) != 0;
    }

    /// The network work. Reads Members() and Receive(), applies the messages, resends, updates the tables, expires
    /// entries and phases. Does nothing unless a role holds the network, and (unless `force`) unless kPollMs passed
    /// since the last run. Idempotent, non-blocking, bounded: at most kMaxRx messages, kMaxGroups ScanRead, kMaxPlayers
    /// ReadNode, one resend per client. Stack (ARM, -Os): frame 8 B, chain 96 B.
    void Poll(bool force) {
        if (use_ == kUseNone || (flags2_ & (kF2External | kF2OnNet)) == kF2External || !Due(force)) {
            return;  // (nothing to do, or the pump of the external mode on a thread that is not the network thread's)
        }
        if (use_ == kUseClient) {
            PollClient();
        } else {
            PollServer();
        }
    }

private:
    static constexpr u8 kUseNone = 0;
    static constexpr u8 kUseClient = 1;
    static constexpr u8 kUseServer = 2;
    static constexpr u8 kTabNone = 0;
    static constexpr u8 kTabScan = 1;
    static constexpr u8 kTabSession = 2;
    static constexpr u8 kTabHost = 3;
    static constexpr u8 kTrkFree = 0;
    static constexpr u8 kTrkPending = 1;
    static constexpr u8 kTrkAccepted = 2;
    static constexpr u8 kTrkStartAcked = 3;
    static constexpr u8 kTrkGoAcked = 4;
    static constexpr u8 kTrkEjected = 5;
    static constexpr u8 kGroupUsed = 1;
    static constexpr u8 kFlagManual = 1;       // accept the clients by hand
    static constexpr u8 kFlagHostOpen = 2;     // HostOpen done, HostClose not yet
    static constexpr u8 kFlagGoSent = 4;       // the passphrase was sent
    static constexpr u8 kFlagJoinActive = 8;   // a join is in progress
    static constexpr u8 kFlagJoinReady = 16;   // an outcome waits to be collected
    static constexpr u8 kFlagGone = 32;        // gone_ms_ is set
    static constexpr u8 kFlagPassServer = 64;  // go_ was last written by the creator (else by the searcher)
    static constexpr u8 kFlagPumped = 128;     // pump_ms_ is set
    static constexpr u8 kF2FinalRoster = 1;    // flags2_: a GO arrived, the roster is read once more before it freezes
    static constexpr u8 kF2External = 2;       // flags2_: the network work is the network thread's (SetExternalPoll)
    static constexpr u8 kF2OnNet = 4;          // flags2_: the caller is the network thread (SetOnNet)

    // The messages without a body, as bytes (a constant in the code: nothing to build on a stack). The test of the lobby holds them
    // to the bytes the codec writes.
    static constexpr u8 kStartMsg[2] = {1, 0};
    static constexpr u8 kStartAckMsg[2] = {2, 0};
    static constexpr u8 kGoAckMsg[2] = {4, 0};
    static_assert(kStartMsg[0] == static_cast<u8>(MsgType::Start) && kStartAckMsg[0] == static_cast<u8>(MsgType::StartAck) &&
                      kGoAckMsg[0] == static_cast<u8>(MsgType::GoAck),
                  "the constant messages carry the types of the wire");

    // ---- small helpers -------------------------------------------------------------------------------------------

    /// The rate limit of Poll: true, with the stamp taken, unless `force` is false and less than kPollMs passed since the
    /// last run. A clock that went back (negative age) lets the poll run and re-anchors the stamp. Its frame is gone before
    /// the work starts.
    DLP_NOINLINE bool Due(bool force) {
        const u32 now = Now();
        if (!force && (flags_ & kFlagPumped) != 0) {
            const s32 d = static_cast<s32>(now - pump_ms_);
            if (d >= 0 && static_cast<u32>(d) < kPollMs) {
                return false;
            }
        }
        pump_ms_ = now;
        flags_ |= kFlagPumped;
        return true;
    }
    DLP_ALWAYS_INLINE u32 Now() const {
        return static_cast<u32>(net_->NowMs());
    }
    DLP_ALWAYS_INLINE void SetF2(u8 bit, bool on) {
        flags2_ = static_cast<u8>(on ? (flags2_ | bit) : (flags2_ & ~bit));
    }
    static u32 Elapsed(u32 now, u32 stamp) {
        const s32 d = static_cast<s32>(now - stamp);
        return d < 0 ? 0u : static_cast<u32>(d);
    }
    static void CopyName(u16* dst, const u16* src) {
        for (u32 i = 0; i < kNameUnits; ++i) {
            dst[i] = src[i];
        }
    }
    static bool Contains(const u8* nodes, u32 n, u8 node) {
        for (u32 i = 0; i < n; ++i) {
            if (nodes[i] == node) {
                return true;
            }
        }
        return false;
    }
    static bool Listed(u8 state) {
        return state >= kTrkPending && state <= kTrkGoAcked;
    }
    void SetFlag(u8 bit, bool on) {
        flags_ = static_cast<u8>(on ? (flags_ | bit) : (flags_ & ~bit));
    }
    bool InSession() const {
        return cli_state_ == kCliInSession || cli_state_ == kCliStarted || cli_state_ == kCliComplete;
    }
    void ClearTables() {
        std::memset(&u_, 0, sizeof(u_));
        tab_ = kTabNone;
    }
    void MakeAnnounce(u8 accepting, HostAnnounce* a) const {
        a->child_tid_low = (tid_low_ & 0xFFFFFF00u) | child_index_;
        CopyName(a->name, id_.name);
        a->max_nodes = static_cast<u8>(max_clients_ + 1);
        a->accepting = accepting;
    }
    void Bad() {
        if (bad_messages_ != 0xFFFFFFFFu) {
            ++bad_messages_;
        }
    }
    /// Answers a message with one of the constant messages (a failing Send is not an error: the peer resends).
    DLP_ALWAYS_INLINE void Reply(u8 to, const u8* msg) {
        net_->Send(to, msg, 2);
    }

    /// The part of ServerStartDistribution after the Poll (its own frame: the HostAnnounce is not under the Poll).
    DLP_NOINLINE u32 StartDistribution() {
        if (srv_state_ != kSrvAccepting) {
            return kInvalidState;
        }
        const u32 now = Now();
        EjectState(kTrkPending, now);
        Announce(0, 0);  // (the network is open in state 2: this is a HostUpdate)
        srv_state_ = kSrvDistributing;
        t0_ = now;
        if (SendPhase(false, false, now) == 0) {
            srv_state_ = kSrvPassphrase;  // nobody to wait for
        }
        return 0;
    }
    DLP_NOINLINE void EndAccepting() {
        if (srv_state_ == kSrvAccepting) {
            Announce(0, 0);  // (the network is open in state 2: this is a HostUpdate)
            srv_state_ = kSrvIdle;
        }
    }
    /// Puts the announcement on the air: HostUpdate if the network is open, else HostOpen on `channel` (false = refused).
    /// The ONE place that holds a HostAnnounce (28 bytes), in a frame of its own, so that no caller's frame carries it.
    DLP_NOINLINE bool Announce(u8 accepting, u32 channel) {
        HostAnnounce a;
        MakeAnnounce(accepting, &a);
        if ((flags_ & kFlagHostOpen) != 0) {
            net_->HostUpdate(a);
            return true;
        }
        return net_->HostOpen(a, static_cast<u8>(channel), id_);
    }

    // ---- ownership of the network, teardown ----------------------------------------------------------------------

    /// Gives the network back if the searcher holds it. True if it did.
    bool ClientRelease() {
        if (use_ != kUseClient) {
            return false;
        }
        if (cli_state_ == kCliScanning) {
            net_->ScanClose();
        } else {
            net_->Disconnect();
        }
        use_ = kUseNone;
        flags_ = static_cast<u8>(flags_ & ~kFlagGone);
        return true;
    }
    /// The searcher's full teardown: network, a join in progress, table, passphrase (if it is the searcher's).
    void ClientReset() {
        ClientRelease();
        flags_ = static_cast<u8>(flags_ & ~(kFlagJoinActive | kFlagJoinReady));
        flags2_ = static_cast<u8>(flags2_ & ~kF2FinalRoster);  // (only that bit: kF2External / kF2OnNet are the hub's, not the searcher's)
        if (tab_ == kTabScan || tab_ == kTabSession) {
            ClearTables();
        }
        if ((flags_ & kFlagPassServer) == 0) {
            std::memset(go_, 0, sizeof(go_));
        }
    }
    /// The creator's full teardown: HostClose if it holds the network, table, flags, passphrase (if it is the creator's).
    void ServerReset() {
        if (use_ == kUseServer) {
            net_->HostClose();
            use_ = kUseNone;
            flags_ = static_cast<u8>(flags_ & ~kFlagGone);  // (the host-gone timer is the creator's only while it holds the network)
        }
        if (tab_ == kTabHost) {
            ClearTables();
        }
        flags_ = static_cast<u8>(flags_ & ~(kFlagManual | kFlagHostOpen | kFlagGoSent));
        if ((flags_ & kFlagPassServer) != 0) {
            std::memset(go_, 0, sizeof(go_));
            flags_ = static_cast<u8>(flags_ & ~kFlagPassServer);
        }
    }

    // ---- the searcher: lookups -----------------------------------------------------------------------------------

    Group* FindScan(const Mac& host) {
        if (tab_ != kTabScan) {
            return nullptr;
        }
        for (u32 i = 0; i < kMaxGroups; ++i) {
            Group* g = &u_.scan.g[i];
            if ((g->flags & kGroupUsed) != 0 && g->host == host) {
                return g;
            }
        }
        return nullptr;
    }
    void MakeView(const Group* g, bool fresh, GroupView* out) {
        const u32 age = fresh ? 0u : Elapsed(Now(), g->last_seen_ms);
        const u64 sub = static_cast<u64>(age) * AZL_TICKS_PER_MS;
        const u64 ticks = net_->NowTicks();
        out->group = g;
        out->tick = ticks > sub ? ticks - sub : 0;
    }
    u32 ViewOf(const Mac& host, GroupView* out) {
        if (cli_state_ == kCliIdle || cli_state_ == kCliScanning || cli_state_ == kCliJoining) {
            const Group* g = FindScan(host);
            if (g != nullptr) {
                MakeView(g, false, out);
                return 0;
            }
        } else if (InSession() && tab_ == kTabSession && u_.session.host.host == host) {
            MakeView(&u_.session.host, true, out);
            return 0;
        }
        return kNoData;
    }
    // (The kBusy of the design's third check is kept but cannot be reached from the lobby itself: taking the network for
    // the creator discards the frozen scan table, so "entry in the table AND the creator holds the network" never holds.)
    u32 JoinChecks(const Mac& host) {
        if (cli_state_ != kCliIdle) {
            return kInvalidState;
        }
        if (FindScan(host) == nullptr) {
            return kNoData;
        }
        if (use_ == kUseServer) {
            return kBusy;
        }
        return net_->ConnectBegin(host, id_) ? 0 : kNotFound;
    }

    // ---- the searcher: the network work --------------------------------------------------------------------------

    /// One poll of the searcher: the join in progress, the messages, then what the state needs.
    DLP_NOINLINE void PollClient() {
        const u32 now = pump_ms_;
        if (cli_state_ == kCliJoining) {
            PollJoin();
            if (use_ != kUseClient) {  // the join failed: the network is already released, nothing to receive or scan
                return;
            }
        }
        for (u32 i = 0; i < kMaxRx && ReceiveOne(); ++i) {
        }
        if ((flags2_ & kF2FinalRoster) != 0) {  // a GO arrived: the last look at the roster before it freezes
            flags2_ = static_cast<u8>(flags2_ & ~kF2FinalRoster);
            ReadRoster();
        }
        if (cli_state_ == kCliScanning) {
            PollScan();
        } else if (cli_state_ == kCliInSession || cli_state_ == kCliStarted) {
            PollSession(now);
        }
    }
    /// State 2: reads up to kMaxGroups lobbies, then expires the old entries.
    DLP_NOINLINE void PollScan() {
        if (tab_ != kTabScan) {
            return;
        }
        for (u32 i = 0; i < kMaxGroups; ++i) {
            HeardGroup hg;
            if (!net_->ScanRead(i, &hg)) {
                break;
            }
            ScanAbsorb(hg);
        }
        ScanExpire(Now());
    }
    void ScanExpire(u32 now) {
        for (u32 i = 0; i < kMaxGroups; ++i) {
            Group& g = u_.scan.g[i];
            if ((g.flags & kGroupUsed) != 0 && Elapsed(now, g.last_seen_ms) >= kGroupTtlMs) {
                std::memset(&g, 0, sizeof(Group));
            }
        }
    }
    static bool FilterPass(u64 filter, u32 child_tid_low) {
        return filter == 0 || ((filter ^ (0x0004000100000000ull | child_tid_low)) >> 8) == 0;
    }
    /// Takes one heard lobby: validates it, then inserts / refreshes / removes its entry.
    void ScanAbsorb(const HeardGroup& hg) {
        const HostAnnounce& a = hg.announce;
        if (a.max_nodes < 2 || a.max_nodes > kMaxPlayers || hg.total_nodes < 1 || hg.total_nodes > a.max_nodes) {
            return;
        }
        if ((a.child_tid_low >> 8) != (tid_low_ >> 8) || !FilterPass(filter_, a.child_tid_low)) {
            return;
        }
        Group* g = FindScan(hg.host);
        if (a.accepting == 0) {  // a host that no longer accepts cannot be chosen: absent
            if (g != nullptr) {
                std::memset(g, 0, sizeof(Group));
            }
            return;
        }
        if (g == nullptr) {
            for (u32 i = 0; i < max_groups_ && g == nullptr; ++i) {
                g = (u_.scan.g[i].flags & kGroupUsed) == 0 ? &u_.scan.g[i] : nullptr;
            }
            if (g == nullptr) {
                return;  // no room: ignored, and the same way every round
            }
        }
        g->host = hg.host;
        g->flags = kGroupUsed;
        g->max_nodes = a.max_nodes;
        g->child_tid_low = a.child_tid_low;
        g->last_seen_ms = Now();
        g->total_nodes = hg.total_nodes;
        g->signal = hg.signal > 3 ? 3 : hg.signal;
        CopyName(g->name, a.name);
    }
    /// The join in progress: the engine's answer first, then the deadline (read after the call: it may have taken time).
    void PollJoin() {
        const int r = net_->ConnectPoll();
        if (r > 0) {
            EnterSession();
        } else if (r < 0) {
            FailJoin(kNotFound);
        } else if (Elapsed(Now(), t0_) >= kJoinMs) {
            FailJoin(kTimeout);
        }
    }
    void FailJoin(u32 code) {
        ClientRelease();  // Disconnect, exactly once
        cli_state_ = kCliIdle;
        join_result_ = code;
        flags_ = static_cast<u8>((flags_ & ~kFlagJoinActive) | kFlagJoinReady);
    }
    /// Joined: the table becomes Session {host's entry, roster}, state 5 at once. The host's entry is moved to the front of
    /// the table in place (a swap of words with entry 0: no 40-byte copy on the stack), then the rest is cleared.
    DLP_NOINLINE void EnterSession() {
        Group* g = FindScan(join_mac_);
        if (g == nullptr) {  // (cannot happen: the table is frozen while joining)
            g = &u_.scan.g[0];
            std::memset(g, 0, sizeof(Group));
            g->host = join_mac_;
            g->flags = kGroupUsed;
            g->max_nodes = kMaxPlayers;
            g->total_nodes = 1;
        } else if (g != &u_.scan.g[0]) {
            for (u32 i = 0; i < sizeof(Group); i += 4) {
                u8* a = reinterpret_cast<u8*>(g) + i;
                u8* b = reinterpret_cast<u8*>(&u_.scan.g[0]) + i;
                u32 wa, wb;
                std::memcpy(&wa, a, 4);
                std::memcpy(&wb, b, 4);
                std::memcpy(a, &wb, 4);
                std::memcpy(b, &wa, 4);
            }
        }
        std::memset(reinterpret_cast<u8*>(&u_) + sizeof(Group), 0, sizeof(u_) - sizeof(Group));
        tab_ = kTabSession;
        my_node_ = net_->MyNode();
        ReadRoster();
        cli_state_ = kCliInSession;
        join_result_ = 0;
        flags_ = static_cast<u8>((flags_ & ~(kFlagJoinActive | kFlagGone)) | kFlagJoinReady);
    }
    /// 5 and 6: is the host still there; is it time to re-read the roster.
    DLP_NOINLINE void PollSession(u32 now) {
        u8 nodes[kMaxPlayers];
        const u32 n = ReadMembers(nodes);
        if (net_->MyNode() != 0 && Contains(nodes, n, 1)) {
            flags_ = static_cast<u8>(flags_ & ~kFlagGone);
        } else if ((flags_ & kFlagGone) == 0) {
            flags_ |= kFlagGone;
            gone_ms_ = now;
        } else if (Elapsed(now, gone_ms_) >= kHostGoneMs) {
            ClientRelease();  // 0x80, not 1: the games' error dialog needs it
            cli_state_ = kCliLost;
            ClearTables();
            return;
        }
        if (Elapsed(now, aux_ms_) >= kRosterMs) {
            ReadRoster(nodes, n);
        }
    }
    DLP_ALWAYS_INLINE u32 ReadMembers(u8 (&nodes)[kMaxPlayers]) {
        const u32 n = net_->Members(nodes);
        return n > kMaxPlayers ? kMaxPlayers : n;
    }
    DLP_ALWAYS_INLINE void ReadRoster() {
        u8 nodes[kMaxPlayers];
        ReadRoster(nodes, ReadMembers(nodes));
    }
    /// Re-reads the roster from the engine (a lost read leaves the id with zero name and seed). With no member at all (we
    /// are not in a network) the last good roster is kept.
    void ReadRoster(const u8* nodes, u32 n) {
        aux_ms_ = Now();
        if (n == 0 || tab_ != kTabSession) {
            return;
        }
        SessionTable& s = u_.session;
        for (u32 i = 0; i < kMaxPlayers; ++i) {
            NodeEntry& e = s.roster[i];
            if (i >= n || !net_->ReadNode(nodes[i], &e)) {
                std::memset(&e, 0, sizeof(e));
            }
            e.node = i < n ? nodes[i] : 0;
        }
        s.host.total_nodes = static_cast<u8>(n > s.host.max_nodes ? s.host.max_nodes : n);
    }

    // ---- messages ------------------------------------------------------------------------------------------------

    /// Pops one message and applies it. False when there is none. The buffer is the one frame of the module that holds
    /// message bytes: the reply (if any) is written over it, after everything the message said has been read.
    DLP_NOINLINE bool ReceiveOne() {
        u8 buf[kMaxMessage];
        u8 from = 0;
        u32 size = 0;
        if (!net_->Receive(&from, buf, kMaxMessage, &size)) {
            return false;
        }
        if (!IsMessage(buf, size) || !(use_ == kUseClient ? ClientMessage(from, buf) : ServerMessage(from, buf[0]))) {
            Bad();
        }
        return true;
    }
    /// START and GO, from the host (node 1) only, in 5, 6, 9. The receiver is idempotent and ALWAYS answers: a lost ack
    /// is only recovered if the duplicate is answered. `buf` is a message already checked by DecodeMessage.
    bool ClientMessage(u8 from, u8* buf) {
        if (!InSession() || from != 1) {
            return false;
        }
        const u8 type = buf[0];
        if (type == static_cast<u8>(MsgType::Start)) {
            if (cli_state_ == kCliInSession) {
                cli_state_ = kCliStarted;
            }
            Reply(1, kStartAckMsg);
            return true;
        }
        if (type == static_cast<u8>(MsgType::Go)) {
            if (cli_state_ != kCliComplete) {  // the FIRST passphrase stays
                std::memcpy(go_, buf, sizeof(go_));  // the whole (already checked) message: type, length, the nine bytes
                flags_ = static_cast<u8>(flags_ & ~kFlagPassServer);
                flags2_ |= kF2FinalRoster;  // the roster is read once more at the end of this poll, then it freezes
                cli_state_ = kCliComplete;
            }
            Reply(1, kGoAckMsg);
            return true;
        }
        return false;
    }
    /// START_ACK and GO_ACK, from a tracked client (accepted or beyond).
    bool ServerMessage(u8 from, u8 type) {
        if (tab_ != kTabHost || from < kFirstClientNode || from > kMaxPlayers) {
            return false;
        }
        Track& t = u_.host.t[from - 2];
        if (t.state < kTrkAccepted || t.state > kTrkGoAcked) {
            return false;
        }
        if (type == static_cast<u8>(MsgType::StartAck) && (srv_state_ == kSrvDistributing || srv_state_ == kSrvPassphrase)) {
            if (t.state < kTrkStartAcked) {
                t.state = kTrkStartAcked;
            }
            return true;
        }
        if (type == static_cast<u8>(MsgType::GoAck) && (flags_ & kFlagGoSent) != 0) {
            t.state = kTrkGoAcked;
            return true;
        }
        return false;
    }

    // ---- the creator: the network work ---------------------------------------------------------------------------

    Track* TrackOf(u16 node) {
        if (tab_ != kTabHost || node < kFirstClientNode || node > kMaxPlayers || !Listed(u_.host.t[node - kFirstClientNode].state)) {
            return nullptr;
        }
        return &u_.host.t[node - 2];
    }
    const Track* TrackOf(u16 node) const {
        if (tab_ != kTabHost || node < kFirstClientNode || node > kMaxPlayers || !Listed(u_.host.t[node - kFirstClientNode].state)) {
            return nullptr;
        }
        return &u_.host.t[node - 2];
    }
    DLP_ALWAYS_INLINE void Eject(Track* t, u8 node, u32 now) {
        net_->Kick(node);
        t->state = kTrkEjected;
        t->sent_ms = now;
    }
    /// Ejects every client that is in `state`.
    void EjectState(u8 state, u32 now) {
        for (u32 s = 0; s < kMaxClients; ++s) {
            if (u_.host.t[s].state == state) {
                Eject(&u_.host.t[s], static_cast<u8>(s + kFirstClientNode), now);
            }
        }
    }
    DLP_NOINLINE void PollServer() {
        const u32 now = pump_ms_;
        u8 nodes[kMaxPlayers];
        const u32 n = ReadMembers(nodes);
        if (net_->MyNode() != 0 && Contains(nodes, n, 1)) {
            flags_ = static_cast<u8>(flags_ & ~kFlagGone);
        } else if ((flags_ & kFlagGone) == 0) {
            flags_ |= kFlagGone;
            gone_ms_ = now;
        } else if (Elapsed(now, gone_ms_) >= kHostGoneMs) {
            ServerReset();  // our own network is lost: back to 1
            srv_state_ = kSrvIdle;
            return;
        }
        // (messages first, before the slots are updated: the ack of a client that left right after sending it still counts)
        for (u32 i = 0; i < kMaxRx && ReceiveOne(); ++i) {
        }
        TrackClients(nodes, n, now);
        Advance(now);
    }
    /// One slot per client node (2..4): born when the node shows up in Members while we accept, freed the moment it no
    /// longer does; a node we cannot take is ejected (and again every kResendMs while it lingers). (Deviation: the design
    /// says such a node is "never stored"; it holds its slot in the "ejected" state, which is not a client and is never
    /// listed, only to rate-limit the Kick. An id above 4 has no slot and shares one timer (aux_ms_), so Kick(5..255) may
    /// reach the engine, at most once per kResendMs; the id 0 is not a node and is never passed to the engine. Both can only come
    /// from a faulty engine: the virtual network admits at most kMaxPlayers.)
    DLP_NOINLINE void TrackClients(const u8* nodes, u32 n, u32 now) {
        for (u32 s = 0; s < kMaxClients; ++s) {
            Track& t = u_.host.t[s];
            const u8 node = static_cast<u8>(s + kFirstClientNode);
            if (!Contains(nodes, n, node)) {
                t.state = kTrkFree;
            } else if (t.state == kTrkFree) {
                if (srv_state_ == kSrvAccepting && node <= max_clients_ + 1) {
                    t.state = (flags_ & kFlagManual) != 0 ? kTrkPending : kTrkAccepted;
                } else {
                    Eject(&t, node, now);
                }
            } else if (t.state == kTrkEjected && Elapsed(now, t.sent_ms) >= kResendMs) {
                Eject(&t, node, now);
            }
        }
        for (u32 i = 0; i < n; ++i) {  // an id above 4 can never be stored: a fifth player does not exist
            if (nodes[i] > kMaxPlayers && Elapsed(now, aux_ms_) >= kResendMs) {
                net_->Kick(nodes[i]);
                aux_ms_ = now;
            }
        }
    }
    /// The phases of the creator, recomputed after every event: 6 -> 8 and 8 (GO sent) -> 9.
    DLP_NOINLINE void Advance(u32 now) {
        if (srv_state_ == kSrvDistributing) {
            // START is resent to those who have not confirmed; at kStartGraceMs they are thrown out; nobody left -> 8
            if (Elapsed(now, t0_) >= kStartGraceMs) {
                EjectState(kTrkAccepted, now);
                srv_state_ = kSrvPassphrase;
            } else if (SendPhase(false, true, now) == 0) {
                srv_state_ = kSrvPassphrase;
            }
        } else if (srv_state_ == kSrvPassphrase && (flags_ & kFlagGoSent) != 0) {
            // GO is resent to those who have not confirmed; at kGoGraceMs, or when nobody is left, 9
            if (Elapsed(now, t0_) >= kGoGraceMs || SendPhase(true, true, now) == 0) {
                Complete();
            }
        }
    }
    /// State 9: the virtual network is closed BEFORE the game sees 9, the table is dropped.
    void Complete() {
        net_->HostClose();
        use_ = kUseNone;
        ClearTables();
        flags_ = static_cast<u8>(flags_ & ~(kFlagHostOpen | kFlagGone));
        srv_state_ = kSrvComplete;
    }
    /// Sends START (to the accepted) or GO (to the accepted and those who confirmed START) to the clients that still owe
    /// an answer; with `due_only` only to those not written to for kResendMs (one resend per client per Poll; the stamp
    /// is `now`, not `+=`: a clock jump must not fire a burst). Returns how many still owe an answer.
    DLP_NOINLINE u32 SendPhase(bool go, bool due_only, u32 now) {
        const u8* msg = go ? go_ : kStartMsg;
        const u32 size = go ? sizeof(go_) : sizeof(kStartMsg);
        for (u32 s = 0; s < kMaxClients; ++s) {
            Track& t = u_.host.t[s];
            if (OwesAnswer(t.state, go) && (!due_only || Elapsed(now, t.sent_ms) >= kResendMs)) {
                net_->Send(static_cast<u8>(s + kFirstClientNode), msg, size);
                t.sent_ms = now;
            }
        }
        u32 waiting = 0;
        for (u32 s = 0; s < kMaxClients; ++s) {
            waiting += OwesAnswer(u_.host.t[s].state, go) ? 1 : 0;
        }
        return waiting;
    }
    static bool OwesAnswer(u8 state, bool go) {
        return state == kTrkAccepted || (go && state == kTrkStartAcked);
    }

    // ---- data ----------------------------------------------------------------------------------------------------

    LobbyNet* net_;
    u64 filter_ = 0;       // StartScan's title filter
    Identity id_ = {};     // who we are
    Tables u_;             // ONE of the three shapes, chosen by tab_
    u32 tid_low_ = 0;      // low 32 bits of the game's title id
    u32 pump_ms_ = 0;      // last Poll run (valid if kFlagPumped)
    u32 t0_ = 0;           // start of the join / of a creator's phase
    u32 join_result_ = 0;  // outcome waiting for ClientJoinPoll (valid if kFlagJoinReady)
    u32 gone_ms_ = 0;      // since when the host / our network is missing (valid if kFlagGone)
    u32 aux_ms_ = 0;       // searcher: last roster read; creator: last Kick of an id outside 1..4
    u32 bad_messages_ = 0;
    Mac join_mac_ = {};    // the host being joined / joined
    u8 my_node_ = 0;       // our node in the session we joined
    u8 go_[2 + kPassphraseSize] = {};  // the passphrase AS the GO message {3, 9, nine bytes} (owner: kFlagPassServer)
    u8 cli_state_ = kCliOff;
    u8 srv_state_ = kSrvOff;
    u8 use_ = kUseNone;    // who holds the network
    u8 tab_ = kTabNone;    // which shape of u_ is alive
    u8 max_clients_ = 1;   // 1..3
    u8 child_index_ = 0;
    u8 max_groups_ = 1;    // 1..4 (FKCL Initialize max_beacons)
    u8 cursor_ = 0;        // GetTitleInfoInOrder: next slot
    u8 flags_ = 0;
    u8 flags2_ = 0;        // kF2FinalRoster, kF2External, kF2OnNet (the spare byte of the padding: sizeof does not change)
};

static_assert(sizeof(DlpLobby) <= 320, "DlpLobby must stay small: it lives next to a game that needs the memory");

#undef DLP_NOINLINE
#undef DLP_ALWAYS_INLINE

// <<< game-thread code

}  // namespace azl
