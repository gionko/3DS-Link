// The IPC front-end of the fake Download Play lobby: the dlp:FKCL and dlp:SRVR commands as the games send them (design:
// analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md sez. 6; commands, headers and records: SPEC-DLP.md sez. 2-5). Pure C++:
// no kernel, no engine, no file, no log, no heap, no thread.
//
// WHAT IT DOES. For each command of FKCL (0x01-0x11) and SRVR (0x01-0x10) it reads the words of the request, calls
// DlpLobby (the logic: it keeps no group state of its own), and writes the reply over the words of the request. The three
// records of the game (DLPTitleInfo 0x13A8, DLPServerInfo 0x298, DLPNodeInfo 0x28) are composed DIRECTLY in the buffer of the
// game, field by field with StoreLE* on bytes: no struct is laid over a buffer of the game (it may be unaligned) and no record
// is kept in memory. The layout of DLPNodeInfo is written in ONE place (WriteNodeInfo).
//
// TWO MODES (decided by SetLobby). ProbeOnly (no lobby; the default): what DlpFkclHle does, word for word and counter for
// counter, for the games that only ask whether the wireless is on (Battle Royale); on dlp:SRVR only IsChild answers (success,
// false - true after SetChild, the Download Play child 3DS Link prepared: 1.1.4, G4), everything else is "invalid state". Lobby: the
// whole service, over a DlpLobby (whose IsChild is false, SetChild or not).
//
// LIMITS (rules of the user). At most kMaxPlayers players in the tables, and for every title its own ceiling (the lobby's:
// four unless the title says more): no array of this module is longer than kMaxPlayers (8 bytes of node ids: one BYTE each,
// so that eight ids are the 8 bytes four u16 were), kMaxClients (7) or kNameUnits (20 bytes of name); GetConnectingNodes never
// writes more than kMaxPlayers ids and GetConnectingClients more than kMaxClients, whatever the game asks - and for a title of
// four never more than 4 and 3, because the lobby has no more to give. MEMORY: the service is 48 bytes on x86-64 and 40 on ARM32
// (static_assert <= 64); nothing is allocated, no record or buffer is a member.
// STACK. The code runs on the game thread that made the IPC call, some of which have 464 bytes below it: no local above 32
// bytes (the largest: a NodeEntry), no recursion, every handler is a frame of its own (noinline) so that Dispatch stays small.
// Measured with arm-none-eabi-g++ 16.1 (devkitARM), the flags of the Makefile (-march=armv6k -mtune=mpcore -mfloat-abi=hard
// -mtp=soft -Os -fomit-frame-pointer -fno-rtti -fno-exceptions -std=gnu++17), -fstack-usage and -fcallgraph-info=su:
//   frames: Dispatch 24, PollPending 24, OnSessionClosed 8; handlers 8 to 48 (FkServerInfo, FkTitleInfo, FkConnectingNodes
//     and SvConnectingClients 48; FkNodeInfo, FkTitleInfoInOrder, FkInitializeWithName, SvInitializeWithName and SvClientInfo
//     40; the rest 32 or less). NO frame of this class is over 48: SvStartAccepting (56 once) fell to 8 when the lobby moved
//     the building of the announcement into a frame of its own (DlpLobby::Announce, 48), and the two handlers of the ids stay
//     at 48 with eight players because the ids are bytes there (as u16 they measured 56: 2026-11-13).
//   chains, down to the first call into LobbyNet (the engine's side), own frame included: Dispatch 152 at most (Dispatch 24 +
//     SvDisconnectClient 8 + the lobby's ServerDisconnectClient 24 + its Poll 96), the pump at the start of every command
//     24 + 96 = 120, PollPending 136 (24 + ClientJoinPoll 16 + 96), OnSessionClosed 40. The design proposed 160 for the sum
//     from Dispatch to the first LobbyNet call: 152 measured. These numbers are HELD, not just written: Tests/dlp_stack_probe.cpp
//     (a noinline wrapper for every public function) is compiled for ARM32 by Tests/dlp_stack_check.sh, which Tests/run.sh runs,
//     and the check fails on a frame over 48 bytes (Dispatch and DlpLobby::PollScan 64), a chain over 160, a recursion, a dynamic
//     frame or a size over its assertion. The plugin's own translation unit decides the real inlining: phase 3 measures it again.
//
// ORDER. The reply replaces the request words, so EVERY word of the request is read (into a local of at most 20 bytes, or
// consumed by the call into the lobby) before cmd[0] is written. The buffers of the game are checked BEFORE the lobby is
// called: a bad pointer has no side effect on the lobby (the cursor of GetTitleInfoInOrder does not move).
//
// RESULTS. The module codes of the specification (0 = success; kInvalidState, kNoData, ...); every reply with a result other
// than 0 has the header (id << 16) | 0x40 and no payload word, except GetClientInfo of node 1 (the host): kNoData WITH the
// full header and the 10 words of the host's record (the convention of Azahar). An id the service does not have gets
// [0x00000040, 0xD9001830] in both modes and for both services (what DlpFkclHle answers). A request whose header does not
// match its id is NOT refused: the reply follows the id (real games send other headers for the same command).
//
// WIDTHS. Games send rubbish in the high bytes (Luigi's Mansion sends StartAccepting [2] = 0x03a12006: the channel is 6).
// & 0xFF: SRVR Initialize [2] (max clients) and [4] (child index), StartAccepting [1] (manual = byte != 0) and [2] (channel),
// GetTitleInfoInOrder [1] (restart = byte != 0). & 0xFFFF: node ids (the commands take a u16), the element counts of
// GetConnectingNodes / GetConnectingClients (the upper half of such a word is not read: 0x10002 is node 2, 2 elements). u32:
// FKCL Initialize [2]. A node id stays u16 up to the comparison in the lobby (never a cast to u8: 0x0102 must not hit node 2).
//
// LOCKING. None: nothing here blocks and nothing is thread-safe. The caller serialises every call (Dispatch, PollPending,
// OnSessionClosed and the lobby's own Pump / OnGameUdsStarting) with ONE mutex.
#pragma once

#include "azl/common.hpp"
#include "azl/dlp_clnt_book.hpp"  // (1.1.4, G1: what dlp:CLNT keeps beside the lobby)
#include "azl/dlp_lobby.hpp"
#include "azl/dlp_types.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

// Private to this header (undefined at its end): the frame of a handler is kept out of Dispatch's frame (noinline), and the
// one-line helpers cost no frame at all (always_inline).
#define DLPIPC_NOINLINE __attribute__((noinline))
#define DLPIPC_ALWAYS_INLINE inline __attribute__((always_inline))

/// Maps an address the game put in a request / its static buffer to writable memory of this process. `map` returns nullptr
/// for an address (or a range) that is not valid. Addresses are u32 (a 64-bit PC cannot hold a pointer in a word); the
/// function is called with the size the module is going to write, except for GetConnectingClients: there `size` is the length of
/// the writable buffer the game declared in its request (it is mapped before the lobby is asked, the module writes at most 2 x kMaxClients =
/// 14 bytes; 6 for a title of four players).
struct IpcResolver {
    void* ctx;
    u8* (*map)(void* ctx, u32 address, u32 size);
};

/// Clnt (1.1.4, G1): dlp:CLNT of the system's Download Play application, served only over a lobby whose network has a book (ClntBook).
enum class DlpKind : u8 { Fkcl, Srvr, Clnt };
enum class DlpMode : u8 { ProbeOnly, Lobby };
/// Done: the reply is in the command words. JoinPending: FKCL StartFakeSession began a join; the command words are NOT a
/// reply yet (nothing was written to them): the caller repeats PollPending(cmd) until it returns true.
enum class DlpOutcome : u8 { Done, JoinPending };

class DlpService {
public:
    // The commands (ids). Written out here, once.
    static constexpr u32 kFkInitialize = 0x01;
    static constexpr u32 kFkFinalize = 0x02;
    static constexpr u32 kFkGetEventDesc = 0x03;
    static constexpr u32 kFkGetChannels = 0x04;
    static constexpr u32 kFkStartScan = 0x05;
    static constexpr u32 kFkStopScan = 0x06;
    static constexpr u32 kFkGetServerInfo = 0x07;
    static constexpr u32 kFkGetTitleInfo = 0x08;
    static constexpr u32 kFkGetTitleInfoInOrder = 0x09;
    static constexpr u32 kFkDeleteScanInfo = 0x0A;
    static constexpr u32 kFkStartFakeSession = 0x0B;
    static constexpr u32 kFkGetMyStatus = 0x0C;
    static constexpr u32 kFkGetConnectingNodes = 0x0D;
    static constexpr u32 kFkGetNodeInfo = 0x0E;
    static constexpr u32 kFkGetPassphrase = 0x0F;
    static constexpr u32 kFkStopSession = 0x10;
    static constexpr u32 kFkInitializeWithName = 0x11;
    static constexpr u32 kFkLast = 0x11;
    static constexpr u32 kSvInitialize = 0x01;
    static constexpr u32 kSvFinalize = 0x02;
    static constexpr u32 kSvGetServerState = 0x03;
    static constexpr u32 kSvGetEventDescription = 0x04;
    static constexpr u32 kSvStartAccepting = 0x05;
    static constexpr u32 kSvEndAccepting = 0x06;
    static constexpr u32 kSvStartDistribution = 0x07;
    static constexpr u32 kSvSendPassphrase = 0x08;
    static constexpr u32 kSvAcceptClient = 0x09;
    static constexpr u32 kSvDisconnectClient = 0x0A;
    static constexpr u32 kSvGetConnectingClients = 0x0B;
    static constexpr u32 kSvGetClientInfo = 0x0C;
    static constexpr u32 kSvGetClientState = 0x0D;
    static constexpr u32 kSvIsChild = 0x0E;
    static constexpr u32 kSvInitializeWithName = 0x0F;
    static constexpr u32 kSvGetDupNoticeNeed = 0x10;
    static constexpr u32 kSvLast = 0x10;

    /// ProbeOnly, no lobby, counters at zero. Cost: nothing.
    DlpService() = default;

    /// Lobby mode over `lobby` (it outlives the service); nullptr = ProbeOnly. A join in progress is forgotten.
    void SetLobby(DlpLobby* lobby) {
        lobby_ = lobby;
        join_pending_ = false;
        join_id_ = kFkStartFakeSession;
    }
    DlpMode mode() const {
        return lobby_ != nullptr ? DlpMode::Lobby : DlpMode::ProbeOnly;
    }
    /// 1.1.4 (G4): ProbeOnly's IsChild answers true - the Download Play child 3DS Link prepared (title_rules::IsPreparedId with its row of the
    /// index: ctr/uds_hle.cpp, UdsHle::SetDlpChild), whose one dlp:SRVR request is IsChild. A lobby's IsChild stays false: a hub never says "child".
    void SetChild(bool on) {
        child_ = on;
    }

    /// Handles one request; the reply replaces the request words. `sbuf` points to the thread's static buffer pairs (word 0 =
    /// descriptor of static buffer 0, word 1 = its address), or is nullptr if the caller has none. In Lobby mode the lobby is
    /// pumped first. Never blocks. Stack (ARM, -Os): frame 24, chain 152.
    DlpOutcome Dispatch(DlpKind kind, u32* cmd, const u32* sbuf, const IpcResolver& r) {
        if (kind == DlpKind::Clnt) {
            return DispatchClnt(cmd, sbuf, r);  // (1.1.4, G1: the Download Play application's service, in a frame of its own)
        }
        Bump(commands_);
        if (lobby_ != nullptr) {
            lobby_->Pump();  // (before anything of the request is read: the pump does not touch the command words)
        }
        const u32 id = cmd[0] >> 16;
        last_id_ = id;
        DlpOutcome out = DlpOutcome::Done;
        if (lobby_ == nullptr) {
            Probe(kind, id, cmd);
        } else if (kind == DlpKind::Fkcl) {
            out = Fkcl(id, cmd, sbuf, r);
        } else {
            Srvr(id, cmd, r);
        }
        if (out == DlpOutcome::Done) {
            Account(cmd[1]);
        }
        return out;
    }

    /// After JoinPending: true when the join has ended and `cmd` holds the reply of StartFakeSession (the outcome is decided
    /// by the lobby's Poll, not by the caller: a join nobody asks about times out by itself). False: still joining, `cmd`
    /// untouched. Without a join in progress (never begun, already collected, cancelled by Finalize / StopSession /
    /// OnSessionClosed): true with the reply 0xE0A0AC01 of command 0x0B. A StartSession of dlp:CLNT that ClntLeave left (HOME during the join):
    /// true with 0 under 0x0D, as a refusal - the application sees 0x80 at its next GetMyStatus. Stack (ARM, -Os): frame 24, chain 136.
    bool PollPending(u32* cmd) {
        u32 res = join_id_ == clnt::kStartSession ? 0u : kInvalidState;  // (join_id_ is 0x0D only for a StartSession of dlp:CLNT not collected yet)
        if (lobby_ != nullptr && join_pending_ && !lobby_->ClientJoinPoll(&res)) {
            return false;
        }
        join_pending_ = false;
        last_id_ = join_id_;  // (the command that began the join: FKCL StartFakeSession 0x0B, or CLNT StartSession 0x0D)
        Plain(cmd, join_id_, res);
        join_id_ = kFkStartFakeSession;
        Account(res);
        return true;
    }
    /// True from a Dispatch that returned JoinPending until the PollPending that collected the outcome.
    bool join_pending() const {
        return join_pending_;
    }

    // ---- dlp:CLNT and its hub (1.1.4, G1: the system's Download Play application only) -------------------------------------------------
    /// A posted dlp:CLNT command that no network thread ran (DlpHub::AnswerUnservedClnt, the hub's mutex held): what can wait is deferred - Initialize,
    /// StartScan, StopScan, StopSession and Finalize answer 0 and the next Tick does them (ClntApplyDeferred) - and the rest gets the module's "wireless
    /// off". Writes [(id << 16) | 0x40, result]; true = deferred. No lobby or no book: false, "wireless off". Route B (clnt::kSystemAppJoins false):
    /// StartSession is ClntStartSession's refusal, which needs no network - 0, then 0x80, and the notice - not "wireless off" (the final review of 1.1.4).
    DLPIPC_NOINLINE bool ClntUnserved(u32* cmd) {
        DlpClntBook* b = Book();
        const u32 id = cmd[0] >> 16;
        if (!clnt::kSystemAppJoins && id == clnt::kStartSession && b != nullptr) {
            ClntStartSession(cmd, *b);  // (route B: refused whatever the host, nothing sent - the answer the network thread gives it)
            return false;
        }
        u32 res = DlpClntBook::kWirelessOff;
        const bool deferred = b != nullptr && b->Defer(id, cmd[2] & 0xFF, &res);
        Plain(cmd, id, res);
        return deferred;
    }
    /// The network thread (the hub's Tick, its mutex held): the work ClntUnserved deferred, on the lobby.
    DLPIPC_NOINLINE void ClntApplyDeferred() {
        DlpClntBook* b = Book();
        if (b == nullptr) {
            return;
        }
        u8 beacons = 0;
        const u8 d = b->TakeDeferred(&beacons);
        if (d == DlpClntBook::kDeferIdle || d == DlpClntBook::kDeferScan) {
            lobby_->ClientInitialize(nullptr, beacons);
        }
        if (d == DlpClntBook::kDeferScan) {
            lobby_->ClientStartScan(0);
        } else if (d == DlpClntBook::kDeferStop) {
            lobby_->ClientStopScan();
        } else if (d == DlpClntBook::kDeferStopSession) {
            lobby_->ClientStopSession();
        } else if (d == DlpClntBook::kDeferFinal) {
            lobby_->ClientFinalize();
        }
    }
    /// The network thread, after HOME or the swap (DlpHub::RequestClntLeave): a session joining, joined or started is left and the application
    /// sees 0x80 ("the link was lost") at its next GetMyStatus. A StartSession still waiting is answered 0 by the next PollPending (join_id_ stays
    /// 0x0D), never an error. In 9, 0x80 and every other state nothing changes.
    DLPIPC_NOINLINE void ClntLeave() {
        DlpClntBook* b = Book();
        const u8 st = lobby_ != nullptr ? lobby_->client_state() : kCliOff;
        if (b != nullptr && (st == kCliJoining || st == kCliInSession || st == kCliStarted)) {
            join_pending_ = false;
            lobby_->ClientStopSession();
            b->ForceLost();
        }
    }

    /// The game closed the session handle without Finalize (or never got to it): the role's Finalize, only if the lobby has
    /// that role initialised; a join in progress is cancelled. Does nothing in ProbeOnly. The caller (phase 3) knows the kind.
    void OnSessionClosed(DlpKind kind) {
        if (lobby_ == nullptr) {
            return;
        }
        if (kind == DlpKind::Fkcl) {
            join_pending_ = false;
            if (lobby_->client_state() != kCliOff) {
                lobby_->ClientFinalize();
            }
        } else if (lobby_->server_state() != kSrvOff) {
            lobby_->ServerFinalize();
        }
    }

    // Counters (saturating). Everything the log of phase 3 needs, so that no printf runs on a game thread.
    u32 commands() const {  // every Dispatch
        return commands_;
    }
    u32 initializes() const {  // Initialize and InitializeWithName (ProbeOnly SRVR refuses them: not counted)
        return initializes_;
    }
    u32 finalizes() const {
        return finalizes_;
    }
    u32 refused() const {  // replies with an error result other than NoData (ProbeOnly FKCL: exactly DlpFkclHle's)
        return refused_;
    }
    u32 unknown_commands() const {  // ids the service does not have: a SUBSET of refused()
        return unknown_;
    }
    u32 bad_pointer() const {  // replies 0xD8E0AFF6: a SUBSET of refused()
        return bad_pointer_;
    }
    u32 last_id() const {  // the id of the last request (the request's, also for an id the service does not have)
        return last_id_;
    }
    u32 last_result() const {  // the result word of the last COMPLETE reply (not of a join still pending)
        return last_result_;
    }

private:
    static constexpr u32 kStaticDescMask = 0x3C0F;  // descriptor type (bits 0-3) and buffer id (bits 10-13) of a static buffer
    static constexpr u32 kStaticDescKind = 2;       // ... static buffer 0
    static constexpr u32 kMappedWriteKind = 0xC;    // the mapped-write descriptor of GetConnectingClients [2]
    static constexpr u32 kUnknownId = 0;            // the id of the reply to an id the service does not have

    static DLPIPC_ALWAYS_INLINE void Bump(u32& counter) {
        if (counter != 0xFFFFFFFFu) {
            ++counter;
        }
    }
    static u32 Min(u32 a, u32 b) {
        return a < b ? a : b;
    }
    /// The books of a complete reply.
    DLPIPC_ALWAYS_INLINE void Account(u32 result) {
        last_result_ = result;
        if (result != 0 && result != kNoData) {
            Bump(refused_);
        }
        if (result == kInvalidPointer) {
            Bump(bad_pointer_);
        }
    }

    // ---- reply pieces ---------------------------------------------------------------------------------------------

    /// One normal word, no translate words: [header, result].
    static DLPIPC_ALWAYS_INLINE void Plain(u32* cmd, u32 id, u32 result) {
        cmd[0] = Header(id, 1, 0);
        cmd[1] = result;
    }
    /// An error (or a plain success of a command with no payload): the header carries the id of the request.
    static DLPIPC_ALWAYS_INLINE void Fail(u32* cmd, u32 id, u32 result) {
        Plain(cmd, id, result);
    }
    /// [header(id, normal, 0), 0, zeros...]: a success whose `normal` words (the result included) after the header are
    /// all zero; the caller overwrites those it fills.
    static DLPIPC_ALWAYS_INLINE void Zeroed(u32* cmd, u32 id, u32 normal) {
        cmd[0] = Header(id, normal, 0);
        for (u32 i = 1; i <= normal; ++i) {
            cmd[i] = 0;
        }
    }
    /// The reply of a command whose payload is one record in static buffer 0: [header(id, 1, 2), result, descriptor, address].
    static DLPIPC_ALWAYS_INLINE void StaticReply(u32* cmd, u32 id, u32 size, u32 address) {
        cmd[0] = Header(id, 1, 2);
        cmd[1] = 0;
        cmd[2] = StaticBufferDesc(size, 0);
        cmd[3] = address;
    }
    static DLPIPC_ALWAYS_INLINE void ReadMac(const u32* cmd, Mac* mac) {
        StoreLE32(mac->data(), cmd[1]);  // bytes 0-3 of the MAC: word 1, little endian
        (*mac)[4] = static_cast<u8>(cmd[2]);
        (*mac)[5] = static_cast<u8>(cmd[2] >> 8);
    }
    /// 10 UTF-16 units from 5 consecutive request words, as the game laid them in memory (never a cast to u16*).
    static DLPIPC_ALWAYS_INLINE void ReadName(const u32* words, u16 (&name)[kNameUnits]) {
        const u8* p = reinterpret_cast<const u8*>(words);
        for (u32 i = 0; i < kNameUnits; ++i) {
            name[i] = LoadLE16(p + 2 * i);
        }
    }

    // ---- the records of the game ----------------------------------------------------------------------------------
    // The only places that know the offsets. All zero first, then the fields. The title, icon, size, version and age
    // ratings of DLPTitleInfo stay zero for ever (TD and PR never read them).

    /// DLPTitleInfo: +0 unique_id = child title id low >> 8, +4 variation = its low byte (the child index), +8 MAC.
    static void WriteTitleInfo(u8* dst, const Group& g) {
        std::memset(dst, 0, kTitleInfoSize);
        StoreLE32(dst + kTitleInfoUniqueId, g.child_tid_low >> 8);
        StoreLE32(dst + kTitleInfoVariation, g.child_tid_low & 0xFF);
        std::memcpy(dst + kTitleInfoMac, g.host.data(), 6);
    }
    /// DLPServerInfo: +0 MAC, +7 signal (0..3, as the lobby keeps it), +8 max clients = max_nodes and +9 clients joined =
    /// min(total, max_nodes), BOTH with the host counted (the convention of Azahar, copied on purpose: the game hides a group
    /// whose +9 equals +8), the host's name at +0x1C (node_info[0].username), and at +0x290 the u64 tick the entry was last
    /// heard at (the game drops the entries older than 5000 ms by it). The other 15 node_info stay zero.
    static void WriteServerInfo(u8* dst, const GroupView& v) {
        const Group& g = *v.group;
        std::memset(dst, 0, kServerInfoSize);
        std::memcpy(dst + kServerInfoMac, g.host.data(), 6);
        dst[kServerInfoSignal] = g.signal;
        dst[kServerInfoMaxClients] = g.max_nodes;
        dst[kServerInfoJoined] = g.total_nodes < g.max_nodes ? g.total_nodes : g.max_nodes;
        for (u32 i = 0; i < kNameUnits; ++i) {
            StoreLE16(dst + kServerInfoHostName + 2 * i, g.name[i]);
        }
        StoreLE64(dst + kServerInfoTick, v.tick);
    }
    /// DLPNodeInfo: +0 friend code seed (u64), +0x0C name (10 units), +0x24 network node id (u32); the rest zero.
    static void WriteNodeInfo(u8* dst, const NodeEntry& e) {
        std::memset(dst, 0, kDlpNodeInfoSize);
        StoreLE64(dst + kNodeInfoSeed, e.seed);
        for (u32 i = 0; i < kNameUnits; ++i) {
            StoreLE16(dst + kNodeInfoName + 2 * i, e.name[i]);
        }
        StoreLE32(dst + kNodeInfoNodeId, e.node);
    }

    // ---- the buffers of the game ----------------------------------------------------------------------------------

    /// The writable memory behind static buffer 0 of the thread for a record of `size` bytes, or nullptr: no pair of
    /// static buffers, the descriptor is not static buffer 0, the address is 0, the declared capacity (descriptor >> 14)
    /// is smaller than `size`, no resolver, or the resolver refuses the range.
    static u8* StaticOut(const u32* sbuf, const IpcResolver& r, u32 size) {
        if (sbuf == nullptr || (sbuf[0] & kStaticDescMask) != kStaticDescKind || sbuf[1] == 0 || (sbuf[0] >> 14) < size ||
            r.map == nullptr) {
            return nullptr;
        }
        return r.map(r.ctx, sbuf[1], size);
    }

    // ---- ProbeOnly ------------------------------------------------------------------------------------------------

    /// ProbeOnly: FKCL is DlpFkclHle (Initialize, InitializeWithName, Finalize succeed; the rest of 1..0x11 is "invalid
    /// state"; any other id is the dispatcher's own answer). SRVR: IsChild succeeds with false (true after SetChild: the prepared
    /// child, 1.1.4 G4), the rest of 1..0x10 is "invalid state". No local at all.
    DLPIPC_NOINLINE void Probe(DlpKind kind, u32 id, u32* cmd) {
        if (kind == DlpKind::Fkcl) {
            if (id == kFkInitialize || id == kFkInitializeWithName) {
                Bump(initializes_);
                Plain(cmd, id, 0);
            } else if (id == kFkFinalize) {
                Bump(finalizes_);
                Plain(cmd, id, 0);
            } else if (id >= kFkInitialize && id <= kFkLast) {
                Plain(cmd, id, kInvalidState);
            } else {
                Unknown(cmd);
            }
        } else if (id == kSvIsChild) {
            Zeroed(cmd, kSvIsChild, 2);  // [0x000E0080, 0 (success), child_]
            cmd[2] = child_ ? 1u : 0u;  // (1.1.4, G4: true only for the prepared child, set once before the hooks)
        } else if (id >= kSvInitialize && id <= kSvLast) {
            Plain(cmd, id, kInvalidState);
        } else {
            Unknown(cmd);
        }
    }
    /// An id the service does not have: [0x00000040, 0xD9001830] (the answer of DlpFkclHle), counted.
    void Unknown(u32* cmd) {
        Bump(unknown_);
        Plain(cmd, kUnknownId, kBadHeader);
    }
    /// The book of dlp:CLNT of the lobby's network, or null (every network but the adapter of the Download Play application's).
    DLPIPC_ALWAYS_INLINE DlpClntBook* Book() const {
        return lobby_ != nullptr ? lobby_->net()->ClntBook() : nullptr;
    }
    /// A reply of an FKCL handler given for the dlp:CLNT id `id` (the same semantics under another number): only the id of the header changes.
    static DLPIPC_ALWAYS_INLINE void Renumber(u32* cmd, u32 id) {
        cmd[0] = (id << 16) | (cmd[0] & 0xFFFF);
    }

    // ---- the dispatch: one switch per service, every case a call (the frame of Dispatch stays small) -----------------

    /// dlp:FKCL. Returns JoinPending only for StartFakeSession. (Inlined into Dispatch: it holds no local.)
    DLPIPC_ALWAYS_INLINE DlpOutcome Fkcl(u32 id, u32* cmd, const u32* sbuf, const IpcResolver& r) {
        switch (id) {
        case kFkInitialize:
            Bump(initializes_);
            FkInitialize(cmd);
            break;
        case kFkInitializeWithName:
            Bump(initializes_);
            FkInitializeWithName(cmd);
            break;
        case kFkFinalize:
            Bump(finalizes_);
            FkFinalize(cmd);
            break;
        case kFkGetEventDesc:
            Zeroed(cmd, id, 7);  // [0x000301C0, 0, 6 zero words]: 0x18 bytes, nothing a game reads
            break;
        case kFkGetChannels:
            Zeroed(cmd, id, 2);  // [0x00040080, 0, 0x0421]
            cmd[2] = 0x0421;
            break;
        case kFkStartScan:
            FkStartScan(cmd);
            break;
        case kFkStopScan:
            FkStopScan(cmd);
            break;
        case kFkGetServerInfo:
            FkServerInfo(cmd, sbuf, r);
            break;
        case kFkGetTitleInfo:
            FkTitleInfo(cmd, sbuf, r);
            break;
        case kFkGetTitleInfoInOrder:
            FkTitleInfoInOrder(cmd, sbuf, r);
            break;
        case kFkDeleteScanInfo:
            FkDeleteScanInfo(cmd);
            break;
        case kFkStartFakeSession:
            return FkStartFakeSession(cmd);
        case kFkGetMyStatus:
            Zeroed(cmd, id, 6);  // [0x000C0180, 0, status word, 0, 0, 0, 0]
            cmd[2] = lobby_->ClientStatusWord();
            break;
        case kFkGetConnectingNodes:
            FkConnectingNodes(cmd, sbuf, r);
            break;
        case kFkGetNodeInfo:
            FkNodeInfo(cmd);
            break;
        case kFkGetPassphrase:
            FkPassphrase(cmd);
            break;
        case kFkStopSession:
            FkStopSession(cmd);
            break;
        default:
            Unknown(cmd);
            break;
        }
        return DlpOutcome::Done;
    }

    /// dlp:SRVR. (Inlined into Dispatch: it holds no local.)
    DLPIPC_ALWAYS_INLINE void Srvr(u32 id, u32* cmd, const IpcResolver& r) {
        switch (id) {
        case kSvInitialize:
            Bump(initializes_);
            SvInitialize(cmd);
            break;
        case kSvInitializeWithName:
            Bump(initializes_);
            SvInitializeWithName(cmd);
            break;
        case kSvFinalize:
            Bump(finalizes_);
            SvFinalize(cmd);
            break;
        case kSvGetServerState:
            Zeroed(cmd, id, 2);  // [0x00030080, 0, state]
            cmd[2] = lobby_->ServerState();
            break;
        case kSvGetEventDescription:
            Zeroed(cmd, id, 8);  // [0x00040200, 0, 7 zero words]
            break;
        case kSvStartAccepting:
            SvStartAccepting(cmd);
            break;
        case kSvEndAccepting:
            SvEndAccepting(cmd);
            break;
        case kSvStartDistribution:
            SvStartDistribution(cmd);
            break;
        case kSvSendPassphrase:
            SvSendPassphrase(cmd);
            break;
        case kSvAcceptClient:
            SvAcceptClient(cmd);
            break;
        case kSvDisconnectClient:
            SvDisconnectClient(cmd);
            break;
        case kSvGetConnectingClients:
            SvConnectingClients(cmd, r);
            break;
        case kSvGetClientInfo:
            SvClientInfo(cmd);
            break;
        case kSvGetClientState:
            SvClientState(cmd);
            break;
        case kSvIsChild:           // every process is "not a child": [0x000E0080, 0, 0]
        case kSvGetDupNoticeNeed:  // [0x00100080, 0, 0]; a negative answer would fail the whole Initialize of the game
            Zeroed(cmd, id, 2);
            break;
        default:
            Unknown(cmd);
            break;
        }
    }

    /// dlp:CLNT (1.1.4, G1; the map of 1-G1.md section 3): 01-0A as FKCL but for the book (Initialize and Finalize reset it, StartScan begins a
    /// scan of it, GetServerInfo never fails, the records are dlp:CLNT's own), 0B/0C/13/14 the answers of a system that needs no update, 0D the
    /// join of a host whose child is READY only (else 0, and 0x80 at the next GetMyStatus: nothing is sent), 0E the word of the book, 0F-12 the
    /// FKCL handlers 0D-10 under their own number. Without a book: an id the service does not have. Returns JoinPending only for StartSession.
    /// Dispatch of dlp:CLNT: the same books (a command counted, the lobby pumped first, the last id, a complete reply accounted); no lobby or no book
    /// (every game, the probe): an id the service does not have.
    DLPIPC_NOINLINE DlpOutcome DispatchClnt(u32* cmd, const u32* sbuf, const IpcResolver& r) {
        Bump(commands_);
        DlpClntBook* b = Book();
        if (b != nullptr) {
            lobby_->Pump();  // (before anything of the request is read, as Dispatch)
        }
        const u32 id = cmd[0] >> 16;
        last_id_ = id;
        DlpOutcome out = DlpOutcome::Done;
        if (b == nullptr) {
            Unknown(cmd);
        } else {
            out = Clnt(id, cmd, sbuf, r, *b);
        }
        if (out == DlpOutcome::Done) {
            Account(cmd[1]);
        }
        return out;
    }
    DLPIPC_NOINLINE DlpOutcome Clnt(u32 id, u32* cmd, const u32* sbuf, const IpcResolver& r, DlpClntBook& book) {
        DlpClntBook* b = &book;
        switch (id) {
        case clnt::kInitialize:
            Bump(initializes_);
            ClntInitialize(cmd, *b);
            break;
        case clnt::kFinalize:
            Bump(finalizes_);
            ClntFinalize(cmd, *b);
            break;
        case clnt::kGetEventDescription:
            Zeroed(cmd, id, 7);  // [0x000301C0, 0, 6 zero words], as FKCL
            break;
        case clnt::kGetChannels:
            Zeroed(cmd, id, 2);  // [0x00040080, 0, 0x0421]
            cmd[2] = kClntChannels;
            break;
        case clnt::kStartScan:
            ClntStartScan(cmd, *b);
            break;
        case clnt::kStopScan:
            FkStopScan(cmd);  // (06 is FKCL's 06)
            break;
        case clnt::kGetServerInfo:
            ClntServerInfo(cmd, sbuf, r, *b);
            break;
        case clnt::kGetTitleInfo:
            ClntTitleInfo(cmd, sbuf, r, *b);
            break;
        case clnt::kGetTitleInfoInOrder:
            ClntTitleInfoInOrder(cmd, sbuf, r, *b);
            break;
        case clnt::kDeleteScanInfo:
            FkDeleteScanInfo(cmd);  // (the book keeps its record: GetServerInfo never fails)
            break;
        case clnt::kPrepareForSystemDownload:
        case clnt::kGetDupAvailability:
            Zeroed(cmd, id, 2);  // [0x000B0080 / 0x00140080, 0, 0]: no update of the system is needed
            break;
        case clnt::kStartSystemDownload:
            Plain(cmd, id, kNoSystemDownload);  // (Azahar's answer; never reached: nothing needs an update)
            break;
        case clnt::kGetCupVersion:
            Zeroed(cmd, id, 3);  // [0x001300C0, 0, 0, 0]
            break;
        case clnt::kStartSession:
            return ClntStartSession(cmd, *b);
        case clnt::kGetMyStatus:
            ClntMyStatus(cmd, *b);
            break;
        case clnt::kGetConnectingNodes:
            FkConnectingNodes(cmd, sbuf, r);
            Renumber(cmd, id);
            break;
        case clnt::kGetNodeInfo:
            FkNodeInfo(cmd);
            Renumber(cmd, id);
            break;
        case clnt::kGetWirelessRebootPassphrase:
            FkPassphrase(cmd);
            Renumber(cmd, id);
            break;
        case clnt::kStopSession:
            ClntStopSession(cmd, *b);
            break;
        default:
            Unknown(cmd);
            break;
        }
        return DlpOutcome::Done;
    }

    // ---- dlp:CLNT handlers (1.1.4, G1): as the FKCL ones, a frame each, the request read before the reply is written --------------------------
    static constexpr u32 kClntChannels = 0x0421;          // GetChannels: the channels 1, 6 and 11, as FKCL
    static constexpr u32 kNoSystemDownload = 0xD960AC02;  // StartSystemDownload: Azahar's answer

    /// [1] shared memory size, [2] the beacons (u8: 16 from the application, the lobby keeps 1..4), [3] [4] sizes, [5] [6] the handles of the shared
    /// memory and of the event: NEVER read, never touched, never closed (the application closes them itself).
    DLPIPC_NOINLINE void ClntInitialize(u32* cmd, DlpClntBook& b) {
        b.Reset();
        Plain(cmd, clnt::kInitialize, lobby_->ClientInitialize(nullptr, cmd[2] & 0xFF));
    }
    DLPIPC_NOINLINE void ClntFinalize(u32* cmd, DlpClntBook& b) {
        b.Reset();
        Plain(cmd, clnt::kFinalize, lobby_->ClientFinalize());
    }
    /// [2][3] the title filter (0 from the application). A scan under way is stopped first: the application starts one every few seconds.
    DLPIPC_NOINLINE void ClntStartScan(u32* cmd, DlpClntBook& b) {
        b.NewScan();
        if (lobby_->client_state() == kCliScanning) {
            lobby_->ClientStopScan();
        }
        Plain(cmd, clnt::kStartScan, lobby_->ClientStartScan((static_cast<u64>(cmd[3]) << 32) | cmd[2]));
    }
    /// GetServerInfo: [1][2] MAC. NEVER an error but for a bad buffer: the record of a host the lobby lists (the book's record refreshed with
    /// it), else the book's, else zeros with the MAC (the application reads it at every frame it draws, and before the jump, without the result).
    DLPIPC_NOINLINE void ClntServerInfo(u32* cmd, const u32* sbuf, const IpcResolver& r, DlpClntBook& b) {
        Mac mac;
        ReadMac(cmd, &mac);
        u8* dst = StaticOut(sbuf, r, kServerInfoSize);
        if (dst == nullptr) {
            Fail(cmd, clnt::kGetServerInfo, kInvalidPointer);
            return;
        }
        ClntServerRecord(dst, mac, b);
        StaticReply(cmd, clnt::kGetServerInfo, kServerInfoSize, sbuf[1]);
    }
    /// The record of GetServerInfo (its own frame: the handler's stays small): the book's record of `mac`, refreshed with what the lobby knows of
    /// it now, with its verdict and the tick of now; zeros and the MAC for a host the book was never given.
    DLPIPC_NOINLINE void ClntServerRecord(u8* dst, const Mac& mac, DlpClntBook& b) {
        if (const Group* live = lobby_->ClientGroupOf(mac)) {
            b.Refresh(*live);
        }
        const Group* g = b.Find(mac);
        if (g != nullptr) {
            WriteClntServerInfo(dst, *g, b.VerdictOf(*g), lobby_->net()->NowTicks());
        } else {
            std::memset(dst, 0, kServerInfoSize);
            std::memcpy(dst + kServerInfoMac, mac.data(), 6);
        }
    }
    /// The record of GetTitleInfo / GetTitleInfoInOrder for the book's record `g` (its own frame): the title heard, the block, the verdict.
    DLPIPC_NOINLINE void ClntTitleRecord(u8* dst, const Group& g, DlpClntBook& b) {
        WriteClntTitleInfo(dst, g, b.TitleOf(g.host), b.BlockFor(g), b.VerdictOf(g));
    }
    /// GetTitleInfo (the application does not call it): [1][2] MAC; the book's record, or kNoData.
    DLPIPC_NOINLINE void ClntTitleInfo(u32* cmd, const u32* sbuf, const IpcResolver& r, DlpClntBook& b) {
        Mac mac;
        ReadMac(cmd, &mac);
        u8* dst = StaticOut(sbuf, r, kTitleInfoSize);
        const Group* g = b.Find(mac);
        if (dst == nullptr || g == nullptr) {
            Fail(cmd, clnt::kGetTitleInfo, dst == nullptr ? kInvalidPointer : kNoData);
            return;
        }
        ClntTitleRecord(dst, *g, b);
        StaticReply(cmd, clnt::kGetTitleInfo, kTitleInfoSize, sbuf[1]);
    }
    /// GetTitleInfoInOrder: [1] restart (always 0 from the application). The next host of the lobby's scan table not delivered in this scan, delivered
    /// (the book keeps it); at the end kNoData - NEVER kInvalidState, which the application takes for an error. A host that appears after the others
    /// were delivered is delivered too (the slots are walked again at every call). A bad buffer delivers nothing.
    DLPIPC_NOINLINE void ClntTitleInfoInOrder(u32* cmd, const u32* sbuf, const IpcResolver& r, DlpClntBook& b) {
        const bool restart = (cmd[1] & 0xFF) != 0;
        u8* dst = StaticOut(sbuf, r, kTitleInfoSize);
        if (dst == nullptr) {
            Fail(cmd, clnt::kGetTitleInfoInOrder, kInvalidPointer);
            return;
        }
        if (restart) {
            b.NewScan();
        }
        for (u32 slot = 0; slot < kMaxGroups; ++slot) {
            const Group* live = lobby_->ClientScanEntry(slot);
            if (live == nullptr || b.DeliveredThisScan(live->host)) {
                continue;
            }
            if (!b.Deliver(*live)) {
                continue;  // (16 records and a 17th host: it waits, nothing known is evicted, and the known hosts in the slots after it are still delivered)
            }
            ClntTitleRecord(dst, *b.Find(live->host), b);
            StaticReply(cmd, clnt::kGetTitleInfoInOrder, kTitleInfoSize, sbuf[1]);
            return;
        }
        Fail(cmd, clnt::kGetTitleInfoInOrder, kNoData);
    }
    /// StartSession: [1][2] the host's MAC, [3][4] the child (not read: the record's is the one). 1.1.4, route B (spec 8 D-B5): every host is
    /// refused with 0 and no connection at all - no frame leaves - and the application sees 0x80 next: the join is the 3DS Link app's
    /// (clnt::kSystemAppJoins false), and a host whose child is ready is told so by the plugin's notice. G1's join path stays, built only by the tests
    /// that set AZL_CLNT_SYSTEM_JOINS: a host whose child is not READY on this console (dlp_children::MayJoin) is refused the same way; a ready one
    /// is joined (JoinPending, PollPending answers under 0x0D) and the seam (K7) is armed with the prepared title of its row.
    DLPIPC_NOINLINE DlpOutcome ClntStartSession(u32* cmd, DlpClntBook& b) {
        Mac mac;
        ReadMac(cmd, &mac);
        const Group* g = b.Find(mac);
        const dlp_children::Verdict v = g != nullptr ? b.VerdictOf(*g) : dlp_children::Verdict::NoRow;
        if (g == nullptr || !clnt::kSystemAppJoins || !dlp_children::MayJoin(v)) {
            b.NoteRefusal(v);
            b.ForceLost();
            Plain(cmd, clnt::kStartSession, 0);
            return DlpOutcome::Done;
        }
        if (lobby_->client_state() == kCliScanning) {
            lobby_->ClientStopScan();
        }
        lobby_->ClientAdoptGroup(*g);
        u32 res = 0;
        if (lobby_->ClientJoinBegin(mac, &res) == JoinStep::Pending) {
            join_pending_ = true;
            join_id_ = static_cast<u8>(clnt::kStartSession);
            b.Arm(b.PreparedFor(*g));
            return DlpOutcome::JoinPending;
        }
        Plain(cmd, clnt::kStartSession, res);
        return DlpOutcome::Done;
    }
    /// GetMyStatus: [header(6), 0, the book's word, 0, 0, 0, 0] - total and downloaded stay 0 (nothing is downloaded: the bar makes its own ramp).
    /// The seam to G3 follows the lobby: the passphrase is ready at 9; a session lost, or a join that ended without one, disarms it.
    DLPIPC_NOINLINE void ClntMyStatus(u32* cmd, DlpClntBook& b) {
        const u32 word = b.Shown(lobby_->ClientStatusWord());
        const u8 st = lobby_->client_state();
        if (st == kCliComplete) {
            b.NotePassphrase();
        } else if (st == kCliLost || st == kCliOff || (st == kCliIdle && !join_pending_ && !b.passphrase_ready())) {
            b.Disarm();
        }
        Zeroed(cmd, clnt::kGetMyStatus, 6);
        cmd[2] = word;
    }
    /// StopSession: FKCL's 0x10, always 0; the state 0x80 forced is lifted (the application stops a session before it scans again).
    DLPIPC_NOINLINE void ClntStopSession(u32* cmd, DlpClntBook& b) {
        FkStopSession(cmd);
        b.ClearOverride();
        Renumber(cmd, clnt::kStopSession);
    }

    // ---- dlp:FKCL handlers ------------------------------------------------------------------------------------------
    // Each one is a frame of its own (noinline): the lobby's inline bodies do not grow the frame of Dispatch. Each reads
    // the words of its request first and writes cmd[0] last.

    /// [1] shared memory size, [2] the number of beacons (u32, limited to 1..4 by the lobby), [3..5] the handles (not touched:
    /// no game waits for the event).
    DLPIPC_NOINLINE void FkInitialize(u32* cmd) {
        Plain(cmd, kFkInitialize, lobby_->ClientInitialize(nullptr, cmd[2]));
    }
    /// [1] shared memory size, [2] beacons, [3..7] the user name (5 words = 10 units), [8] one word more, [9..11] the handles.
    DLPIPC_NOINLINE void FkInitializeWithName(u32* cmd) {
        u16 name[kNameUnits];
        ReadName(cmd + 3, name);
        Plain(cmd, kFkInitializeWithName, lobby_->ClientInitialize(name, cmd[2]));
    }
    DLPIPC_NOINLINE void FkFinalize(u32* cmd) {
        Plain(cmd, kFkFinalize, lobby_->ClientFinalize());
    }
    /// [2][3] the title id filter (low, high). [1] (the channel mask) and [4][5] (the MAC filter) are not read.
    DLPIPC_NOINLINE void FkStartScan(u32* cmd) {
        Plain(cmd, kFkStartScan, lobby_->ClientStartScan((static_cast<u64>(cmd[3]) << 32) | cmd[2]));
    }
    DLPIPC_NOINLINE void FkStopScan(u32* cmd) {
        Plain(cmd, kFkStopScan, lobby_->ClientStopScan());
    }
    DLPIPC_NOINLINE void FkStopSession(u32* cmd) {
        Plain(cmd, kFkStopSession, lobby_->ClientStopSession());
    }
    DLPIPC_NOINLINE void FkDeleteScanInfo(u32* cmd) {
        Mac mac;
        ReadMac(cmd, &mac);
        Plain(cmd, kFkDeleteScanInfo, lobby_->ClientDeleteScanInfo(mac));
    }
    /// The first half of StartFakeSession: [1][2] the MAC of the host, [3][4] the child title id (ignored). JoinPending leaves
    /// the command words as they are.
    DLPIPC_NOINLINE DlpOutcome FkStartFakeSession(u32* cmd) {
        Mac mac;
        ReadMac(cmd, &mac);
        u32 res = 0;
        if (lobby_->ClientJoinBegin(mac, &res) == JoinStep::Pending) {
            join_pending_ = true;
            return DlpOutcome::JoinPending;
        }
        Plain(cmd, kFkStartFakeSession, res);
        return DlpOutcome::Done;
    }
    /// GetServerInfo: [1][2] MAC; static buffer 0 receives the 0x298-byte record. The buffer is checked first.
    DLPIPC_NOINLINE void FkServerInfo(u32* cmd, const u32* sbuf, const IpcResolver& r) {
        Mac mac;
        ReadMac(cmd, &mac);
        u8* dst = StaticOut(sbuf, r, kServerInfoSize);
        GroupView v;
        const u32 res = dst == nullptr ? kInvalidPointer : lobby_->ClientServerInfo(mac, &v);
        if (res != 0) {
            Fail(cmd, kFkGetServerInfo, res);
            return;
        }
        WriteServerInfo(dst, v);
        StaticReply(cmd, kFkGetServerInfo, kServerInfoSize, sbuf[1]);
    }
    /// GetTitleInfo: [1][2] MAC, [3][4] title id (ignored); static buffer 0 receives the 0x13A8-byte record.
    DLPIPC_NOINLINE void FkTitleInfo(u32* cmd, const u32* sbuf, const IpcResolver& r) {
        Mac mac;
        ReadMac(cmd, &mac);
        u8* dst = StaticOut(sbuf, r, kTitleInfoSize);
        GroupView v;
        const u32 res = dst == nullptr ? kInvalidPointer : lobby_->ClientTitleInfo(mac, &v);
        if (res != 0) {
            Fail(cmd, kFkGetTitleInfo, res);
            return;
        }
        WriteTitleInfo(dst, *v.group);
        StaticReply(cmd, kFkGetTitleInfo, kTitleInfoSize, sbuf[1]);
    }
    /// GetTitleInfoInOrder: [1] restart (the low byte != 0). A bad buffer does not move the cursor.
    DLPIPC_NOINLINE void FkTitleInfoInOrder(u32* cmd, const u32* sbuf, const IpcResolver& r) {
        const bool restart = (cmd[1] & 0xFF) != 0;
        u8* dst = StaticOut(sbuf, r, kTitleInfoSize);
        GroupView v;
        const u32 res = dst == nullptr ? kInvalidPointer : lobby_->ClientTitleInfoInOrder(restart, &v);
        if (res != 0) {
            Fail(cmd, kFkGetTitleInfoInOrder, res);
            return;
        }
        WriteTitleInfo(dst, *v.group);
        StaticReply(cmd, kFkGetTitleInfoInOrder, kTitleInfoSize, sbuf[1]);
    }
    /// GetConnectingNodes: [1] the number of elements the game's array holds (u16). Replies [header(2, 2), 0, TOTAL nodes,
    /// descriptor of the ids written, address]. Writes min(total, n, kMaxPlayers, capacity / 2) u16 ids; nothing is mapped when none.
    /// (The ids are kept as bytes in this frame and widened as they are stored: 8 bytes for 8 players, what 4 u16 took.)
    DLPIPC_NOINLINE void FkConnectingNodes(u32* cmd, const u32* sbuf, const IpcResolver& r) {
        u8 ids[kMaxPlayers];
        static_assert(sizeof(ids) / sizeof(ids[0]) == kMaxPlayers, "at most kMaxPlayers node ids");
        const u32 want = Min(cmd[1] & 0xFFFF, kMaxPlayers);
        u32 total = 0;
        lobby_->ClientConnectingNodes(ids, want, &total);
        u32 count = Min(total, want);
        u8* dst = nullptr;
        if (count != 0) {
            if (sbuf == nullptr || (sbuf[0] & kStaticDescMask) != kStaticDescKind || sbuf[1] == 0 || r.map == nullptr) {
                Fail(cmd, kFkGetConnectingNodes, kInvalidPointer);
                return;
            }
            count = Min(count, (sbuf[0] >> 14) / 2);
            if (count != 0 && (dst = r.map(r.ctx, sbuf[1], 2 * count)) == nullptr) {
                Fail(cmd, kFkGetConnectingNodes, kInvalidPointer);
                return;
            }
        }
        for (u32 i = 0; i < count; ++i) {
            StoreLE16(dst + 2 * i, ids[i]);
        }
        cmd[0] = Header(kFkGetConnectingNodes, 2, 2);
        cmd[1] = 0;
        cmd[2] = total;
        cmd[3] = StaticBufferDesc(2 * count, 0);
        cmd[4] = sbuf != nullptr ? sbuf[1] : 0;
    }
    /// GetNodeInfo: [1] the node id (u16); the reply carries the 10 words of the record.
    DLPIPC_NOINLINE void FkNodeInfo(u32* cmd) {
        NodeEntry e;
        const u32 res = lobby_->ClientNodeInfo(static_cast<u16>(cmd[1]), &e);
        if (res != 0) {
            Fail(cmd, kFkGetNodeInfo, res);
            return;
        }
        cmd[0] = Header(kFkGetNodeInfo, 11, 0);
        cmd[1] = 0;
        WriteNodeInfo(reinterpret_cast<u8*>(cmd + 2), e);
    }
    /// GetWirelessRebootPassphrase: 9 bytes in memory order over [2..4], the last 3 bytes zero.
    DLPIPC_NOINLINE void FkPassphrase(u32* cmd) {
        Zeroed(cmd, kFkGetPassphrase, 4);
        cmd[1] = lobby_->ClientPassphrase(reinterpret_cast<u8*>(cmd + 2));
    }

    // ---- dlp:SRVR handlers ------------------------------------------------------------------------------------------

    /// [1] shared memory size, [2] max clients (u8), [3] process id, [4] child index (u8), [5] [6] block size and count, [7..9]
    /// the handles. Only [2] and [4] are read.
    DLPIPC_NOINLINE void SvInitialize(u32* cmd) {
        Plain(cmd, kSvInitialize, lobby_->ServerInitialize(cmd[2] & 0xFF, cmd[4] & 0xFF, nullptr));
    }
    /// As Initialize up to [6], then [7..12] the name (6 words; 10 units are read), [13..15] the handles.
    DLPIPC_NOINLINE void SvInitializeWithName(u32* cmd) {
        u16 name[kNameUnits];
        ReadName(cmd + 7, name);
        Plain(cmd, kSvInitializeWithName, lobby_->ServerInitialize(cmd[2] & 0xFF, cmd[4] & 0xFF, name));
    }
    DLPIPC_NOINLINE void SvFinalize(u32* cmd) {
        Plain(cmd, kSvFinalize, lobby_->ServerFinalize());
    }
    /// [1] manual (the low byte != 0), [2] the channel (the low byte).
    DLPIPC_NOINLINE void SvStartAccepting(u32* cmd) {
        Plain(cmd, kSvStartAccepting, lobby_->ServerStartAccepting((cmd[1] & 0xFF) != 0, cmd[2] & 0xFF));
    }
    /// The lobby's ServerClientInfo with a NodeEntry (32 bytes) of ours around it would make a frame of 56 bytes if the compiler
    /// folded it into the handler's: a frame of its own keeps every frame of this class at 48 or less.
    DLPIPC_NOINLINE u32 ClientInfoOf(u16 node, NodeEntry* e) {
        return lobby_->ServerClientInfo(node, e);
    }
    DLPIPC_NOINLINE void SvEndAccepting(u32* cmd) {
        Plain(cmd, kSvEndAccepting, lobby_->ServerEndAccepting());
    }
    DLPIPC_NOINLINE void SvStartDistribution(u32* cmd) {
        Plain(cmd, kSvStartDistribution, lobby_->ServerStartDistribution());
    }
    /// [1..3] the 9 bytes in memory order, consumed by the call before the reply is written over them.
    DLPIPC_NOINLINE void SvSendPassphrase(u32* cmd) {
        Plain(cmd, kSvSendPassphrase, lobby_->ServerSendPassphrase(reinterpret_cast<const u8*>(cmd + 1)));
    }
    DLPIPC_NOINLINE void SvAcceptClient(u32* cmd) {
        Plain(cmd, kSvAcceptClient, lobby_->ServerAcceptClient(static_cast<u16>(cmd[1])));
    }
    DLPIPC_NOINLINE void SvDisconnectClient(u32* cmd) {
        Plain(cmd, kSvDisconnectClient, lobby_->ServerDisconnectClient(static_cast<u16>(cmd[1])));
    }
    /// GetConnectingClients: [1] the number of elements (u16), [2] the mapped-write descriptor ((bytes << 4) | 0xC), [3] the
    /// address. The reply echoes [2] and [3] of the REQUEST in [3] and [4] (read first, then written over). Writes
    /// min(total, n, kMaxClients, bytes / 2) u16 ids (kept as bytes in this frame, as in FkConnectingNodes).
    DLPIPC_NOINLINE void SvConnectingClients(u32* cmd, const IpcResolver& r) {
        const u32 n = cmd[1] & 0xFFFF;
        const u32 desc = cmd[2];
        const u32 address = cmd[3];
        const u32 bytes = desc >> 4;
        u8* dst = nullptr;
        if ((desc & 0xF) != kMappedWriteKind ||
            (bytes != 0 && (address == 0 || r.map == nullptr || (dst = r.map(r.ctx, address, bytes)) == nullptr))) {
            Fail(cmd, kSvGetConnectingClients, kInvalidPointer);
            return;
        }
        u8 ids[kMaxClients];
        static_assert(sizeof(ids) / sizeof(ids[0]) == kMaxClients, "at most kMaxClients client ids");
        const u32 capacity = Min(Min(n, kMaxClients), bytes / 2);
        u32 total = 0;
        const u32 res = lobby_->ServerConnectingClients(ids, capacity, &total);
        if (res != 0) {
            Fail(cmd, kSvGetConnectingClients, res);
            return;
        }
        const u32 count = Min(total, capacity);
        for (u32 i = 0; i < count; ++i) {
            StoreLE16(dst + 2 * i, ids[i]);
        }
        cmd[0] = Header(kSvGetConnectingClients, 2, 2);
        cmd[1] = 0;
        cmd[2] = total;
        cmd[3] = desc;
        cmd[4] = address;
    }
    /// GetClientInfo: [1] the node id (u16). Node 1 (the host): kNoData with the full header and the host's record (the lobby
    /// says so by leaving a node id in `e`: it sets e.node before it returns kNoData, and before it returns 0).
    DLPIPC_NOINLINE void SvClientInfo(u32* cmd) {
        NodeEntry e;
        e.node = 0;
        const u32 res = ClientInfoOf(static_cast<u16>(cmd[1]), &e);
        if (res != 0 && !(res == kNoData && e.node != 0)) {
            Fail(cmd, kSvGetClientInfo, res);
            return;
        }
        cmd[0] = Header(kSvGetClientInfo, 11, 0);
        cmd[1] = res;
        WriteNodeInfo(reinterpret_cast<u8*>(cmd + 2), e);
    }
    /// GetClientState: [1] the node id (u16); the reply is [header(6), 0, state<<24 | joined<<16 | node, 0, 0, 0, 0].
    DLPIPC_NOINLINE void SvClientState(u32* cmd) {
        u32 word = 0;
        const u32 res = lobby_->ServerClientState(static_cast<u16>(cmd[1]), &word);
        if (res != 0) {
            Fail(cmd, kSvGetClientState, res);
            return;
        }
        Zeroed(cmd, kSvGetClientState, 6);
        cmd[2] = word;
    }

    DlpLobby* lobby_ = nullptr;
    u32 commands_ = 0;
    u32 initializes_ = 0;
    u32 finalizes_ = 0;
    u32 refused_ = 0;
    u32 unknown_ = 0;
    u32 bad_pointer_ = 0;
    u32 last_id_ = 0;
    u32 last_result_ = 0;
    bool join_pending_ = false;
    u8 join_id_ = kFkStartFakeSession;  // 1.1.4 (G1): the command whose reply PollPending writes (FKCL 0x0B, CLNT StartSession 0x0D); in the padding
    bool child_ = false;  // 1.1.4 (G4): ProbeOnly's IsChild answers true (SetChild); in the padding, as join_id_ (40 bytes on ARM32, 48 on x86-64)
};
static_assert(sizeof(DlpService) <= 64, "the service is a handful of words: no buffer, no record");

#undef DLPIPC_NOINLINE
#undef DLPIPC_ALWAYS_INLINE

// <<< game-thread code

}  // namespace azl
