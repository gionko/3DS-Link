// The adapter of the REAL Download Play (1.1.3, DlpGameMode::RealDlp): LobbyNet (azl/dlp_lobby.hpp) realised on a UdsEngine that speaks
// on the wire the Download Play Azahar emulates - the "fake client" case, every player has the game, nothing is downloaded - instead of
// the dialect of EngineLobbyNet. The lobby above it (DlpLobby, its states, its graces) is the same object; only the wire changes.
// Design: analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md, sections 4 (the host), 5 (the searcher), 6.3/6.4 (threads,
// memory); the bytes are azl/dlp_real_wire.hpp's.
//
// THE HOST (phase F3). HostOpen takes the engine (InitializeForLobby), hosts the real Download Play's network (BeginHostingDlp: the
// Download Play's ids, NO application data) and binds channel 2 for every node. Then the network thread's Pump:
//   - the announcement, a group of 5 frames on channel 1 to 0xFFFF, ONLY while the host accepts and a spectator is connected (a
//     searcher reads it as a spectator: Azahar's client, and ours in F4): a group at once when a spectator comes, then one every
//     kAnnounceMs, ONE frame per Pump (the network thread holds the hub's mutex that the game threads wait for);
//   - per client node (2..4) the dialogue: Auth (A1) as soon as the node is in, StartDistribution (A3) when the lobby says START,
//     FinishContentUpload (A5) at its answer, then a START_ACK for the lobby; the final with the passphrase (A7) when the lobby says GO,
//     the notice (A8) every kNoticeMs, and at its answer (A9) a GO_ACK for the lobby. Each packet is resent on its own timer.
// The lobby's Send(START / GO) only records what it wants (no frame leaves on the caller's thread: a game thread runs StartDistribution
// and SendWirelessRebootPassphrase); it is idempotent: a START or a GO repeated every 200 ms changes nothing, and the FIRST passphrase is
// the one sent (the game calls BeginGame again and again with new ones; nobody checks it). The answers come back to the lobby as the
// dialect's START_ACK / GO_ACK through Receive(), from the node that earned them.
// A client that asks for the content (a download: not this mode) is ejected. A packet that is not what its kind needs is counted, never
// acted on; nothing here reads past what the engine gave.
//
// THE SEARCHER (phase F4, design 5). A searcher learns what a host offers only as a SPECTATOR of its network (the announcement is not in
// the beacon). ScanOpen takes nothing (the engine files the beacons of the real Download Play's kind, SetLobbyBeaconKind in the
// constructor); the network thread's Pump then probes, one host at a time, every host whose beacon is fresh and that it does not know yet:
//   - it takes the engine (InitializeForLobby) for the probe ONLY, connects as a spectator (ConnectBeginDlp, the beacon's channel), turns
//     the client's foreign-lobby filter on (two hosts of the Download Play in one room are both node 1 and both send on channel 1), binds
//     channel 1 from node 1 and collects the 5 frames of a group (at most kPullsPerProbe per Pump); with the 5, the host's name is node 1's;
//   - the host goes into a cache of kProbeCache entries: verified (its child title id, max_nodes, name), ignored (a malformed frame, or no
//     frame at all in kProbeCollectMs, until the next ScanOpen, as Azahar), or to be tried again after kProbeRetryMs (no answer to the
//     connection within kProbeConnectMs: Azahar does not ignore those either);
//   - then Unbind (on the network thread), DisconnectNetwork, Release (the engine given back, the filter off).
// ScanRead lists the verified hosts whose beacon is fresh, of any game (the lobby filters by game; a host of another game stays in the cache
// and is not probed again). ScanClose ends a probe and empties the cache. ConnectBegin takes the engine, connects as a client on the channel
// of the host's beacon (the engine's table of beacons, never the probe's cache: a join after StopScan needs nothing of the scan), binds
// channel 2 from node 1 and turns the filter on for the session. The Pump answers the host at once, whatever the lobby's state (A2 to the
// Auth, A4 without content to StartDistribution, A6 to the Finish, A9 to the notice); StartDistribution becomes the lobby's START and the
// final its GO with the passphrase (the FIRST final's), each handed once through Receive(); the lobby's START_ACK / GO_ACK send nothing.
// Disconnect = Release.
//
// IN THE PLUGIN (phase F5). Only a game whose row of kDlpGames (Sources/ctr/plugin.cpp) says DlpGameMode::RealDlp gets one: the plugin makes it
// once, at start, and gives it to its DlpHub as the lobby's network (the backend; every other game keeps the hub's own EngineLobbyNet and never
// has one of these). The hub pumps it on the network thread (its Tick: this Pump, then the lobby's) and asks it whether the engine is held
// (Holds); on a game thread with no network thread (the hub's RunHere at the game's nwm::UDS Initialize) only Holds, ScanClose, Disconnect and
// HostClose = Release run.
//
// THREADS. As EngineLobbyNet: nothing here is thread-safe, the caller serialises every call (Pump included) with ONE mutex; the engine
// takes its own lock inside each call. On a game thread the lobby calls only the clock, HostOpen / HostUpdate / HostClose / Kick, Send /
// Receive (no frame), Members / ReadNode / MyNode, ScanOpen / ScanClose / ScanRead / ConnectBegin / ConnectPoll / Disconnect (no frame
// of the dialogue: the connection's handshake is the engine's) and, with the hub's RunHere, HostClose / Disconnect = Release. Nothing blocks.
// MEMORY. One object (heap, once, only for a game of that mode), about 1.8 KB: the one frame buffer (kBufferBytes), the probes' cache
// (kProbeCache x 36) and some words. No allocation of its own; the engine allocates as it does for the games (the ring of a bind: 2 KB for
// the dialogue, 8 KB for a probe only while it lasts; a WifiPacket per SendTo).
#pragma once

#include "azl/dlp_lobby.hpp"
#include "azl/dlp_real_wire.hpp"
#include "azl/uds_engine.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

class RealDlpNet final : public LobbyNet {
public:
    // Times (ms), design 4.5.
    static constexpr u32 kAnnounceMs = 500;       // a group of the announcement (Azahar's interval; here only with a spectator)
    static constexpr u32 kAuthResendMs = 300;     // A1 until the AuthAck (Azahar's client takes one packet per 100 ms then)
    static constexpr u32 kDistResendMs = 400;     // A3 until its answer (Azahar sends it once; we resend it)
    static constexpr u32 kFinishResendMs = 200;   // A5 until its answer
    static constexpr u32 kNoticeMs = 100;         // A8 while the final waits for its answer
    static constexpr u32 kFinalResendMs = 500;    // A7 again ...
    static constexpr u32 kFinalTries = 4;         // ... at most 4 times in all (the last at +1500 ms, inside the lobby's 2000)
    static constexpr u32 kPumpMs = 10;            // the fastest rate of the network work
    static constexpr u32 kPullsPerPump = 4;       // packets of channel 2 taken per Pump
    static_assert((kFinalTries - 1) * kFinalResendMs < DlpLobby::kGoGraceMs, "every final leaves before the lobby's GO grace ends");
    static_assert(kAuthResendMs < DlpLobby::kStartGraceMs && kDistResendMs * 2 < DlpLobby::kStartGraceMs,
                  "at least two A3 before the lobby's START grace ends");
    // The wire.
    static constexpr u8 kChannelAnnounce = 1;     // host -> 0xFFFF, the spectators
    static constexpr u8 kChannelDialogue = 2;     // host <-> client, unicast
    static constexpr u32 kHostBind = 0x44;        // the id of the host's bind (a bind id of the plugin's own: the game has none here)
    static constexpr u32 kDialogueRing = 2048;    // its ring
    static constexpr u32 kPullMax = 0xFFFF;       // what PullPacket may take out: any length a frame can say, so none stays stuck
    static constexpr u16 kBlockClients = 0x2;     // UDSNETATTR_DISABLE_CONNECT
    static constexpr u8 kPassphraseOffset = 2;    // the passphrase in the dialect's GO message {3, 9, nine bytes}
    static_assert(dlp_real::kPassphraseBytes == kPassphraseSize, "the GO message carries the final's nine bytes");
    // The searcher (design 5.2, 5.4).
    static constexpr u32 kProbeEveryMs = 20;      // a new probe is looked for at most this often
    static constexpr u32 kProbeFreshMs = 400;     // a host is probed only while its beacon is this fresh (the lobby's own freshness)
    static constexpr u32 kProbeConnectMs = 3000;  // the spectator's connection (Azahar waits as long)
    static constexpr u32 kProbeCollectMs = 1500;  // the 5 frames from the bind (Azahar 1000; a group every 500 ms: at least two groups)
    static constexpr u32 kProbeRetryMs = 5000;    // a host that did not answer the connection is tried again after this
    static constexpr u32 kPullsPerProbe = 2;      // frames of the announcement taken per Pump (1.5 KB each, under the hub's mutex)
    static constexpr u32 kProbeCache = UdsEngine::kHeardLobbies;  // hosts known (a room of 4 holds at most 4 hosts)
    static constexpr u32 kProbeBind = 0x45;       // the probe's bind (channel 1 from node 1)
    static constexpr u32 kProbeRing = 8192;       // its ring: more than a group (6576 B), only for the probe
    static constexpr u32 kSessionBind = 0x46;     // the session's bind (channel 2 from node 1)
    static constexpr u16 kHostNode = 1;
    static_assert(kProbeRing >= dlp_real::kAnnounceGroupBytes, "the probe's ring holds a whole group of the announcement");
    static_assert(kProbeCollectMs >= 2 * kAnnounceMs + kPumpMs, "a probe sees two groups of an Azahar host before it gives up");

    /// What the host has done, for the tests and the log (saturating u16).
    struct Counters {
        u16 announce;   // frames of the announcement sent
        u16 auth;       // A1
        u16 dist;       // A3
        u16 finish;     // A5
        u16 final;      // A7
        u16 notice;     // A8
        u16 bad;        // packets of channel 2 refused (format, sender)
        u16 ignored;    // well-formed packets that came in a state that does not take them (a late copy)
        u16 content;    // clients ejected because they asked for the content
        // the searcher
        u16 probes;     // probes begun (a spectator's connection asked)
        u16 verified;   // probes that read the 5 frames
        u16 ignored_hosts;  // hosts put on the ignored list (a malformed frame, or nothing)
        u16 answers;    // answers sent to a host (A2, A4, A6, A9)
        u16 host_bad;   // packets of the session refused (format, sender)
    };

    /// `engine` outlives the adapter; it is the game's, and the game is of this mode for the whole session: its beacons are filed as the
    /// real Download Play's from now on (design 5.1). Out of line (Sources/azl/dlp_real_net.cpp): the plugin makes the adapter once, and the
    /// chain test of the plugin's ELF (Tests/chain_sim) builds one with it.
    explicit RealDlpNet(UdsEngine* engine);
    RealDlpNet(const RealDlpNet&) = delete;
    RealDlpNet& operator=(const RealDlpNet&) = delete;

    u64 NowMs() override;
    u64 NowTicks() override;
    // creator
    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override;
    void HostUpdate(const HostAnnounce& a) override;
    void HostClose() override;
    void Kick(u8 node) override;
    // searcher (phase F4)
    bool ScanOpen() override;
    void ScanClose() override;
    bool ScanRead(u32 index, HeardGroup* out) override;
    bool ConnectBegin(const Mac& host, const Identity& me) override;
    int ConnectPoll() override;
    void Disconnect() override;
    // both
    u8 MyNode() override;
    u32 Members(u8 (&nodes)[kMaxPlayers]) override;
    bool ReadNode(u8 node, NodeEntry* out) override;
    bool Send(u8 to_node, const u8* data, u32 size) override;
    bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) override;

    /// The network work (the network thread, under the caller's mutex): at most once per kPumpMs. Bounded: one frame of the
    /// announcement, kPullsPerPump packets taken, one packet per client. The hub calls it in its Tick, before the lobby's pump (LobbyNet::Pump).
    void Pump() override;
    /// True while the adapter holds the engine (it initialised it and has not given it back): the hub's EngineHeld (LobbyNet::Holds).
    bool Holds() const override {
        return held_;
    }
    /// Who the probes are (a spectator's NodeInfo: nobody shows it; the plugin gives the lobby's identity). HostOpen and ConnectBegin take
    /// the lobby's own.
    void SetIdentity(const Identity& me) {
        me_ = me;
    }
    /// What the cache knows of `host` (tests): kHeardNone, kHeardVerified, kHeardIgnored, kHeardRetry.
    u8 HeardState(const Mac& host) const;
    static constexpr u8 kHeardNone = 0;
    static constexpr u8 kHeardVerified = 1;
    static constexpr u8 kHeardIgnored = 2;
    static constexpr u8 kHeardRetry = 3;
    /// True while a probe holds the engine (tests).
    bool Probing() const {
        return probe_.st != kProbeIdle;
    }
    const Counters& counters() const {
        return count_;
    }
    /// The state of a client's dialogue (tests): see the k* constants below; node 2..4.
    u8 PeerState(u8 node) const {
        return node >= 2 && node <= kMaxPlayers ? peers_[node - 2].st : 0;
    }

    // The dialogue of one client (design 4.4).
    static constexpr u8 kFree = 0;      // no node, or a node the Pump has not seen yet: A1 at once
    static constexpr u8 kAuthWait = 1;  // A1 sent, waiting for A2
    static constexpr u8 kAuthed = 2;    // A2 in: A3 when the lobby says START
    static constexpr u8 kDistWait = 3;  // A3 sent, waiting for A4
    static constexpr u8 kFinWait = 4;   // A5 sent, waiting for A6
    static constexpr u8 kReady = 5;     // A6 in (START_ACK owed to the lobby): A7 when the lobby says GO
    static constexpr u8 kGoWait = 6;    // A7 sent, A8 every kNoticeMs, waiting for A9
    static constexpr u8 kGoAcked = 7;   // A9 in (GO_ACK owed to the lobby): nothing more is sent
    static constexpr u8 kEjected = 8;   // ejected: nothing is sent until the node is gone

private:
    struct Peer {
        u32 t_ms;   // last packet of the state's own timer (A1, A3, A5, A8)
        u32 t7_ms;  // last final (A7)
        u8 st;
        u8 rid[2];  // the client's id (+0x16 of its AuthAck): in every packet the host sends it
        u8 flags;   // kWantStart | kWantGo | kOweStartAck | kOweGoAck, and the finals sent in the high nibble
    };
    static constexpr u8 kWantStart = 1;
    static constexpr u8 kWantGo = 2;
    static constexpr u8 kOweStartAck = 4;
    static constexpr u8 kOweGoAck = 8;
    static constexpr u8 kTriesShift = 4;

    /// Takes the engine if nobody has it; false = a game's session is up. Held already: true.
    bool Acquire(const Identity& me);
    /// Gives it back whole: the service shut down (the network ends with its Deauthentications, the binds go), the client's
    /// foreign-lobby filter off whatever happened (design 5.2, Q8), every state of the adapter cleared.
    void Release();
    void ClearHost();
    void ForgetGone(u16 mask);
    u32 Now() const;
    void PumpHost(u32 now);
    void TakePackets(u32 now);
    void OnPacket(u8 node, const dlp_real::Packet& p, u32 now);
    void StepPeer(u8 node, u32 now);
    void Announce(u32 now);
    /// Seals nothing: the codec's builders did. Sends buf_[0..len) to `node` on the dialogue's channel.
    void SendDialogue(u8 node, std::size_t len);

    // The searcher.
    struct Heard {           // a host the probes know (36 bytes)
        Mac host;
        u8 st;               // kHeardNone (free), kHeardVerified, kHeardIgnored, kHeardRetry
        u8 max_nodes;        // frame 0, +0x20
        u32 child_tid_low;   // frame 0, +0x10 (the low word)
        u32 t_ms;            // verified: last listed; ignored / retry: when it happened
        u16 name[kNameUnits];  // node 1's
    };
    struct Probe {
        Mac host;
        u8 st;               // kProbeIdle, kProbeConnecting, kProbeCollect
        u8 mask;             // the frames read (bit = index)
        u32 t_ms;            // the connection asked / the bind
        u32 child_tid_low;
        u8 max_nodes;
    };
    static constexpr u8 kProbeIdle = 0;
    static constexpr u8 kProbeConnecting = 1;
    static constexpr u8 kProbeCollect = 2;
    // What the session owes the lobby, and what it has seen (session_flags_).
    static constexpr u8 kOweStart = 1;
    static constexpr u8 kOweGo = 2;
    static constexpr u8 kSawStart = 4;
    static constexpr u8 kSawFinal = 8;

    void ClearClient();
    Heard* FindHeard(const Mac& host);
    /// A fresh beacon of `host` in the engine's table (false = not heard within `max_age_ms`).
    bool BeaconOf(const Mac& host, u32 max_age_ms, UdsEngine::HeardLobby* out) const;
    void PumpProbe(u32 now);
    void StartProbe(u32 now);
    void Collect(u32 now);
    /// The probe ends: the host's entry becomes `verdict` (kHeardNone = try again at once), the engine is given back.
    void EndProbe(u8 verdict, u32 now);
    void PumpSession();
    static void Bump(u16* c) {
        if (*c != 0xFFFF) {
            ++*c;
        }
    }

    UdsEngine* engine_;
    Counters count_ = {};
    Peer peers_[kMaxClients] = {};
    u64 child_tid_ = 0;     // the announcement's child title id
    u32 pump_ms_ = 0;       // last Pump (valid if pumped_)
    u32 group_ms_ = 0;      // start of the last group of the announcement (valid if grouped_)
    u32 spectators_ = 0;    // spectators at the last Pump
    u8 pass_[kPassphraseSize] = {};  // the FIRST passphrase the lobby gave (valid if pass_set_)
    u8 max_nodes_ = 0;      // the announcement's max_nodes
    u8 frame_ = dlp_real::kAnnounceFrames;  // next frame of the group (kAnnounceFrames = no group under way)
    bool held_ = false;
    bool hosting_ = false;  // HostOpen succeeded and no HostClose yet
    bool accepting_ = false;
    bool pass_set_ = false;
    bool pumped_ = false;
    bool grouped_ = false;
    bool group_now_ = false;  // a spectator came: the next group starts without waiting kAnnounceMs
    // the searcher
    Identity me_ = {};        // the probes' identity
    Heard heard_[kProbeCache] = {};
    Probe probe_ = {};
    u32 probe_ms_ = 0;        // last time a probe was looked for (valid if probe_looked_)
    bool scanning_ = false;   // ScanOpen and no ScanClose yet
    bool probe_looked_ = false;
    bool session_ = false;    // ConnectBegin succeeded and no Disconnect yet
    u8 session_flags_ = 0;    // kOweStart | kOweGo | kSawStart | kSawFinal (the final's passphrase is pass_)
    u8 buf_[dlp_real::kBufferBytes];  // the ONE frame buffer: every packet is built and read here, under the caller's mutex
};

static_assert(sizeof(RealDlpNet) <= 2048, "the adapter of the real Download Play stays under 2 KB");

// <<< game-thread code

}  // namespace azl
