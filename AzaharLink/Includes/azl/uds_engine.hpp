// Port of Azahar's NWM_UDS HLE (src/core/hle/service/nwm/nwm_uds.cpp) to a standalone engine.
//
// The engine knows nothing about IPC: it implements the *semantics* of each nwm::UDS command and
// exchanges synthetic 802.11 frames (WifiPacket) with whatever transport it is given (the room
// client on the console, an in-memory relay in the unit tests).
#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "azl/platform.hpp"
#include "azl/uds_dedupe.hpp"
#include "azl/uds_frames.hpp"
#include "azl/uds_ring.hpp"
#include "azl/uds_types.hpp"

namespace azl {

/// Kernel services needed by the engine. On the 3DS these are real kernel events.
class KernelOps {
public:
    virtual ~KernelOps() = default;
    /// Creates a one-shot event owned by the engine. Returns 0 on failure.
    virtual u32 CreateEvent() = 0;
    /// Returns a new handle to the same event, that the game is free to close.
    virtual u32 DuplicateForGame(u32 handle) = 0;
    virtual void SignalEvent(u32 handle) = 0;
    virtual void ClearEvent(u32 handle) = 0;
    /// Returns true if the event was signalled before the timeout (nanoseconds).
    virtual bool WaitEvent(u32 handle, s64 timeout_ns) = 0;
    virtual void CloseEvent(u32 handle) = 0;
    virtual void SleepNs(s64 ns) = 0;
};

/// What a game asks a scan to do (the input words of RecvBeaconBroadcastData / ScanOnConnection). The
/// defaults are libctru's scan, so a caller that only filters by BSSID can pass the Mac.
struct ScanParams {
    ScanParams() = default;
    ScanParams(const Mac& bssid_filter) : bssid(bssid_filter) {}
    Mac bssid = kBroadcastMac;     ///< only this transmitter (broadcast = any)
    u16 type = 1;                  ///< 0 active, 1 passive
    u16 mode = 2;                  ///< 0..3; libctru's 2 filters by the vendor tag
    u16 channel_mask = 0x0421;     ///< bit n = channel n+1; the scan listens to each one for the dwell time
    u16 dwell_ms = 110;
    u8 ssid_len = 0;
    u32 wlan_comm_id = 0xFFFFFFFF; ///< only networks of this game (all ones = any)
    u8 id8 = 0xFF;                 ///< ... and of this id8 (0xFF = any)
    u32 cancel = 0;                ///< the game's event: once it is signalled the scan stops waiting
};

class UdsEngine {
public:
    struct Transport {
        /// Sends a frame to the room. The callee fills in the transmitter address and must drop
        /// the frame silently when not connected to a room.
        std::function<void(WifiPacket&)> send;
        /// Our MAC address as known by the room (or a fallback when not joined).
        std::function<Mac()> get_mac;
    };

    UdsEngine(KernelOps* kernel, Transport transport);
    ~UdsEngine();
    UdsEngine(const UdsEngine&) = delete;
    UdsEngine& operator=(const UdsEngine&) = delete;

    // ---- nwm::UDS commands (all return a raw result code, 0 = success) ------------------------
    // The state is global, like the real service's (one manager for every session of the game), and
    // every command answers what the firmware answers in the state it is called in: Gate() holds the
    // rules, Teardown() is the only way back to "not connected".

    /// `version` is 0x100 for the deprecated Initialize, the game's word for InitializeWithVersion.
    /// A second call while initialised changes nothing and hands out the same event again.
    u32 Initialize(const NodeInfo& node, u32 shared_memory_size, u16 version, u32* game_status_event);
    /// Never fails and is the only way out of a stopped service.
    u32 Shutdown();
    /// The service stops (`kind` is a ServiceState: wireless switch, sleep, nwm exiting): the session
    /// is torn down like at Shutdown, every bind is freed, and every command answers "wrong state"
    /// (or "wireless off") until the game calls Shutdown. True if a session was up.
    bool StopService(int kind);

    /// `size` is 0x108 for BeginHostingNetwork or 0x3C for the deprecated variant.
    u32 BeginHostingNetwork(const u8* network_info, std::size_t size);
    u32 EjectClient(u16 network_node_id);
    u32 EjectSpectators();
    /// libctru's udsSetNewConnectionsBlocked / udsAllowSpectators: sets or clears `mask` in the
    /// attribute word of the network.
    u32 UpdateNetworkAttribute(u16 mask, u8 flag);
    u32 DestroyNetwork();
    /// Blocks (up to 5 s) until the connection sequence ends, like the real service.
    u32 ConnectToNetwork(const u8* network_info, std::size_t size, u8 connection_type);
    u32 DisconnectNetwork();

    ConnectionStatus GetConnectionStatus();
    /// The unnamed command 0x0C: `table[0]` is the current peer, `table[1 + slot]` the node in that
    /// slot; each entry is {sample, valid << 8}.
    static constexpr std::size_t kPeerSignalEntries = 1 + kUdsMaxNodes;
    u32 GetPeerSignal(u16 table[kPeerSignalEntries]);
    u32 GetNodeInformation(u16 network_node_id, NodeInfo* out);

    /// Fills `out` (BeaconDataReplyHeader + entries) with the networks heard during the scan. Listens
    /// like a real scan: the dwell time on every channel of the mask (330 ms for libctru's), or less if
    /// the game signals its cancel event. Never blocks on mu_ while it waits.
    u32 RecvBeaconBroadcastData(u8* out, u32 out_size, const ScanParams& scan);
    /// The same scan while connected (not available while idle or hosting).
    u32 ScanOnConnection(u8* out, u32 out_size, const ScanParams& scan);
    /// Makes a scan leave out the networks of another wlancommid / id8 than the one asked for: ON by default since
    /// 2026-10-20 (the capture of a New Super Mario Bros. 2 session showed the words a game sends equal the ids in the
    /// beacons of its hosts, spec C7, and the count-only mode let the game list the emulator's Tekken lobby in the same
    /// room and join it, in a loop). Off: they are only counted and logged (the tests keep that mode).
    void SetScanFilterEnforced(bool enforced);

    u32 SetApplicationData(const u8* data, u32 size);
    /// Copies up to `capacity` bytes; `*actual` is 0 if the game's buffer is too small.
    u32 GetApplicationData(u8* out, u32 capacity, u32* actual);

    // Commands with no effect on the wire: they check their arguments and the state, like the firmware.
    u32 SetMaxSendDelay(u64 delay_ns);
    u32 ApplyRadioProfile(u8 profile);
    u32 SetBeaconMissTime(u16 time_tu);
    u32 SetApParam(u32 value);
    u32 SetProbeResponseParam();
    u32 SetDeauthOnDestroy(bool enabled);
    u32 Flush(u8 aggregator);

    /// A bind is a receive queue for the frames of one data channel (from one node or from any). Like nwm's,
    /// binds are not touched by the end of a session: only Shutdown and a service stop free them.
    u32 Bind(u32 bind_node_id, u32 recv_buffer_size, u8 data_channel, u16 network_node_id,
             u32* game_event);
    /// What Unbind tells the game besides the id: the channel and source of the bind and how many frames
    /// it stored and pushed out since Bind.
    struct UnbindInfo {
        u8 channel = 0;
        u16 src = 0;
        u32 enqueued = 0;
        u32 overwritten = 0;
    };
    /// An unknown id is no error (`*info` stays zero), id 0 is.
    u32 Unbind(u32 bind_node_id, UnbindInfo* info = nullptr);
    /// Takes the oldest frame out of the bind: `*data_size` is its payload length (0 = nothing queued, `out`
    /// untouched), at most `out_capacity` bytes of it are copied to `out`. A frame longer than
    /// `max_out_buff_size` stays queued (`*src_node_id` still says whose it is).
    u32 PullPacket(u32 bind_node_id, u32 max_out_buff_size, u8* out, u32 out_capacity, u32* data_size,
                   u16* src_node_id);
    u32 SendTo(u16 dest_node_id, u8 data_channel, u32 data_size, u8 flags, const u8* data,
               std::size_t available);
    u32 GetChannel(u8* channel);

    /// Decrypts the node list of a scanned beacon. `out` gets 16 * 40 bytes.
    u32 DecryptBeaconData(const NetworkInfo& net_info, const u8* tag0, std::size_t tag0_size,
                          const u8* tag1, std::size_t tag1_size, std::vector<u8>* out);

    // ---- Transport side -----------------------------------------------------------------------
    /// The room link is gone (ENet gave up, the room closed or kicked us, the user switched the connection off): the
    /// radio under the game's session has vanished. A client or spectator drops to idle with `reason` (no Deauthentication
    /// goes anywhere, the binds stay), a Connect in progress fails, a host stays a host and loses every node (it keeps
    /// beaconing for whoever comes back). Nothing happens while idle, stopped or not initialised. Returns true if the
    /// game was told something (its status event was signalled). Short: the network thread calls it.
    bool OnTransportDown(NetworkStatusChangeReason reason);
    /// The members of the room as the room lists them (MACs). A member that was in an earlier list of this join and
    /// stays out of the lists for 3 s has left: a host removes its node, a client its node or, if it was the host, the
    /// network. The first list of a join says who is there and concludes nothing. Nothing is sent.
    void OnRoomMembers(const std::vector<Mac>& members);
    /// The room link ended or begins anew: what is known of who was in the room is void.
    void ResetRoomPresence();
    void OnWifiPacket(const WifiPacket& packet);
    /// Looks at a relayed frame the way the room sent it (room::EncodeWifiPacket: id, type, channel, two
    /// MACs, a length-prefixed 802.11 payload) and, if OnWifiPacket() would only drop it, does exactly what
    /// that would do for a drop (counters, debug capture) WITHOUT building a WifiPacket: at 4 players two
    /// thirds of the frames the room relays are for other nodes or our own echo. Returns false when the
    /// frame needs the full path (it is delivered, routed by a host, a management frame, malformed or no
    /// secure data frame at all): the caller then decodes it and calls OnWifiPacket() as before.
    bool TryDiscardEncoded(const u8* message, std::size_t size);
    /// Drives the beacon timer; call every ~10 ms.
    void Tick(u64 now_ms);

    // ---- The fake Download Play lobby (azl/dlp_lobby_net.hpp) ------------------------------------------------------------
    // The lobby is a UDS network of its own that two plugins host and join for the games that find their group with
    // dlp:FKCL / dlp:SRVR (design: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE2.md). These are the pieces its adapter needs
    // and the game's commands do not give. None of them blocks or is a nwm::UDS command: they are short enough to be called from a
    // game's thread, with the engine's lock and nothing else. What they keep (the lobbies heard, the ring of messages: LobbyState,
    // about 350 bytes) is allocated the first time a lobby is used (PrepareLobby, which the adapter's ScanOpen / HostOpen /
    // ConnectBegin call): a game that never uses one pays a pointer, and the engine does not even look at the beacons for it.

    /// BeginHostingNetwork with a NetworkInfo the engine composes itself (no 264-byte structure on the caller's stack):
    /// `channel` 0 = the default, the network id comes from our MAC, the attribute word is empty (UpdateNetworkAttribute
    /// closes the lobby). Answers what BeginHostingNetwork answers; on success the lobby's inbox is on (see SetLobbyInbox) and so is
    /// the foreign-lobby filter of its clients (RefuseForeignLocked), in the same breath as the network begins.
    u32 BeginHostingLobby(u32 wlan_comm_id, u8 id8, u8 channel, u8 max_nodes, const u8* app_data, u32 app_size);

    /// The Download Play's network ids: the lobby's virtual network is the network of no game, and these are the ones its beacons carry.
    static constexpr u32 kLobbyWlanCommId = 0x00002810;
    static constexpr u8 kLobbyId8 = 0x55;

    /// One lobby heard lately: whose beacon it is, on which channel, how many nodes it says it has and the first bytes of its
    /// application data (`size` counts what is copied, at most kHeardDataMax).
    static constexpr u32 kHeardDataMax = 32;
    static constexpr u32 kHeardLobbies = 4; ///< a room of 4 players holds at most 4 hosts: the engine keeps the 4 newest
    struct HeardLobby {
        Mac host;
        u8 channel;
        u8 total_nodes;
        u8 max_nodes;
        u8 size;
        u8 data[kHeardDataMax];
    };
    /// The `index`-th (0..) lobby - a beacon of kLobbyWlanCommId / kLobbyId8 with application data - whose last beacon was heard
    /// at most `max_age_ms` ago, in the order the engine keeps them (stable between two calls: a lobby keeps its place). The
    /// network thread files the beacons as they come, so this is a copy under the lock and nothing more; it needs no Initialize
    /// (beacons are heard in any state) but PrepareLobby (before it nothing is filed). A lobby whose host now beacons under other ids
    /// (its game's network) is forgotten at that beacon, and any beacon heard sweeps the ones silent for more than kBeaconKeepMs.
    /// False = there is no such lobby.
    bool HeardLobbyAt(u32 index, u32 max_age_ms, HeardLobby* out) const;

    /// The service for a lobby when no game has it: Initialize without a game's block, version or status event (nothing is handed
    /// out, so nothing needs closing). kWrongState = it is initialised already (the game's, or the lobby's own): nothing is touched.
    /// `username` is 10 UTF-16 units.
    u32 InitializeForLobby(u64 friend_code_seed, const u16* username);

    /// The first half of ConnectToNetwork, without the wait (ConnectToNetwork is this and a wait for the answer): 0 = the
    /// connection sequence began, else what ConnectToNetwork answers when it refuses. `*attempt` is the number of the attempt.
    u32 ConnectBegin(const u8* network_info, std::size_t size, u8 connection_type, u32* attempt = nullptr);
    /// ConnectBegin as a client of the lobby whose beacon was heard from `host` (3 s at most ago): the channel is the beacon's, so
    /// the caller needs no NetworkInfo. kNoHostFound = no such beacon. On success the lobby's inbox is on (see BeginHostingLobby).
    u32 ConnectBeginLobby(const Mac& host);
    /// How the attempt goes: 0 = the host has not answered yet, 1 = this node is a client (spectator) of its network, -1 = the
    /// attempt ended without a connection (or there never was one). Whoever waits ends it with DisconnectNetwork / Shutdown.
    int ConnectPoll() const;

    /// The lobby's messages: frames of this data channel, addressed to this node, from the node that the engine knows to be the
    /// MAC they came from (the host for a client, the node of that MAC for a host - a stranger's frame is dropped, whatever
    /// node id it claims) are kept in a small ring of the engine and never reach a bind. Off by default; BeginHostingLobby and
    /// ConnectBeginLobby switch it on, Shutdown off; SetLobbyInbox(true) empties the ring (and allocates the lobby's state if need
    /// be), (false) too. The oldest message goes when the ring is full, and a node that leaves takes its unread messages with it.
    /// While it is on, a client also drops the frames, node maps and Logoffs of any other network (a second lobby of the game in the
    /// same room has the same node ids and would rewrite its roster).
    static constexpr u8 kLobbyChannel = 0xD1;
    static constexpr u32 kLobbyMessageMax = 16;
    static constexpr u32 kLobbyInbox = 8;
    void SetLobbyInbox(bool on);
    /// Allocates what a lobby keeps (the lobbies heard, the ring) if it is not there yet; false = no memory. Idempotent.
    bool PrepareLobby();
    /// Takes the oldest message out: `*from_node` is the node that sent it, `*size` its length (at most `capacity`: a longer
    /// message is cut). False = none.
    bool PopLobbyMessage(u8* from_node, u8* data, u32 capacity, u32* size);

    /// A picture of the session that GetConnectionStatus does not spoil (it clears the changed-nodes word and logs): our node id
    /// and the mask of the nodes present (bit n-1 = node n). False, with both 0, when we are not in a network (a host, a client or
    /// a spectator).
    bool NodeSnapshot(u16* my_node, u16* node_bitmask) const;

    /// The foreign-lobby filter of a CLIENT (or spectator): a data frame, a node map or a Logoff whose transmitter is
    /// neither the host nor a node the host announced belongs to another network in the same room (a second lobby of
    /// the game has the same node ids). Off (the default), such frames are only counted and logged, as the scan filter
    /// is, until a hardware session shows that nothing the game needs is among them; on, they are dropped. A HOST
    /// always drops them: the node map it built is exact.
    void SetClientForeignFilterEnforced(bool enforced);

    // ---- Introspection (for the UI) -------------------------------------------------------------
    NetworkStatus status() const;
    bool initialized() const;

private:
    struct Node {
        bool connected = false;
        bool spec = false;
        u16 node_id = 0;
        /// Host side: this MAC left (a Deauthentication) a moment before it authenticated again, so the second copy
        /// of its goodbye - Azahar's guests send two, up to 2 s apart - may still be on its way and must not end the
        /// new association. Cleared by the first Deauthentication it swallows, or when `auth_ms` is 3 s old.
        bool deauth_guard = false;
        u64 auth_ms = 0; ///< when the Authentication that made this entry was handled
    };
    // nwm's bind table: 16 entries, an entry is in use while its id is not 0.
    static constexpr u32 kMaxBinds = 16;
    struct BindSlot {
        u32 id = 0;
        u8 channel = 0;
        u16 src = 0;    // 0xFFFF: frames of every node
        u32 event = 0;  // engine-owned handle, signalled for every frame stored
        u64 budget = 0; // what it takes of the game's UDS block (16 + the declared size rounded up to 4)
        RxRing ring;
    };

    // All of these assume mu_ is held.
    void SendPacket(WifiPacket& packet);
    u16 GetNextAvailableNodeId();
    void BroadcastNodeMap();
    /// Where a SendTo to `dest_node_id` goes: false = nowhere (the node is not in the network). `*addressed`: a frame the game sent with the
    /// broadcast flag to a node the room has is addressed to that node's own MAC (the room then delivers it to that member alone).
    bool GetSendAddress(u16 dest_node_id, u8 flags, Mac* out, bool* addressed);
    /// The MAC of a node of this network (the host's for node 1), false if it is not known.
    bool NodeMacLocked(u16 node_id, Mac* out) const;
    /// True if the room's member list has `mac` and it has not left it (a member that left is in the list for the debounce time, `absent_`).
    bool InRoomLocked(const Mac& mac) const;
    Mac GetMacAddress();
    /// The bind a frame of `src` on `channel` goes to: an exact source match first (table order), else the
    /// LAST bind that takes every node; null if there is none (`*channel_bound` says whether the channel has
    /// binds at all, i.e. whether the source filter was what kept the frame out).
    BindSlot* FindBindLocked(u8 channel, u16 src, bool* channel_bound);
    BindSlot* FindBindIdLocked(u32 id);
    /// A session ended: every bind is woken once (the real service's tail), none is freed.
    void SignalBinds();
    /// Shutdown and service stop: every bind is woken, closed and freed. The destructor too, silently.
    void FreeBinds(bool signal);
    /// A session begins: what the last one left queued is not for this one.
    void FlushRingsLocked();

    /// The check every command starts with: 0 if it may run, else the firmware's refusal. A stopped
    /// service answers "wireless off" / "wrong state" (unless `prefix` is false: the three commands
    /// that look at the status only), a service that was never initialised "wrong state" (the real
    /// one dereferences a null pointer), and a status outside `status_mask` (bits of NetworkStatus
    /// values; 0 = any) "wrong state".
    u32 Gate(u32 status_mask, bool prefix = true) const;
    /// The one way back to "not connected" (own Destroy / Disconnect / Shutdown, service stop, a
    /// Deauthentication from the host): the session ends with `reason`, the status event and every
    /// bind event are signalled once (the binds stay), a Connect in progress fails. With `send_deauth` the peers are
    /// told: a host broadcasts three Deauthentications (if the game left that on and initialised
    /// with version >= 0x200), a client or spectator (or one whose host already answered its
    /// Authentication) sends one to its host.
    void Teardown(NetworkStatusChangeReason reason, bool send_deauth);
    /// Removes a node that left (Deauthentication, EjectClient) from every table the game can see.
    /// Returns true if the game has to be told (status event). `cause` indexes Stats::node_removed.
    enum RemovedBy { kRemovedDeauth = 0, kRemovedReauth = 1, kRemovedEject = 2, kRemovedPresence = 3, kRemovedLink = 4 };
    bool RemoveNodeLocked(std::map<Mac, Node>::iterator node, RemovedBy cause);
    /// A member of the room that was in the lists before is out of them for good (debounced): ends its node.
    void MemberLeftLocked(const Mac& mac);
    void CheckPresenceLocked(u64 now_ms);
    /// Ends the Connect in progress with `result` and wakes its thread.
    void FinishConnectLocked(u32 result);
    /// The scan both scan commands (`command` 0x0F / 0x22) run: waits first, with mu_ NOT held, then
    /// reports what was heard.
    u32 ScanBeacons(u32 command, u8* out, u32 out_size, const ScanParams& scan);
    void WaitForScan(const ScanParams& scan, u64 window_ms);
    /// True if a beacon of `host` was heard lately: only then is it a host we know exists.
    bool HostBeaconKnown(const Mac& host) const;
    /// Sends the EAPoL-Logoff that admits a client (or repeats the answer to its Start) to `to`.
    void SendEapolLogoffLocked(const Mac& to, u16 assigned_node_id);
    /// Shadow of the beacon sync (spec F14, C6): compares the node set the host's beacon announces with
    /// the one the Logoff frames built, counts the differences and changes nothing.
    void CheckBeaconSyncLocked(const WifiPacket& beacon);
    /// Sets or clears attribute bits (UpdateNetworkAttribute, EjectSpectators).
    void SetAttributeLocked(u16 mask, bool set);
    /// True while the host refuses new members (the admission copy of the attribute word).
    bool AdmissionBlocked() const;

    /// Says so (once per session) when a peer announces the same friend code seed as this console.
    void NotePeerSeedLocked(const NodeInfo& peer);
    bool seed_said_ = false;

    /// Says which MAC every node of the network has, and the name its game gave it (host: after a client is admitted or leaves;
    /// client: when the host's node map arrives), so that the node ids of the capture can be put against the players of the
    /// room. A line only when the list is not the one the last line said. Runs on the network thread, never on the game's.
    void LogNodeMapLocked(const char* why);
    std::string node_map_text_;

    /// The foreign-lobby filter (see SetClientForeignFilterEnforced): is `transmitter` the host of the network we are in,
    /// or one of the nodes it announced (a host: one of its clients)?
    bool IsNetworkMemberLocked(const Mac& transmitter, NetworkStatus status) const;
    /// True if the frame must not go on: a host always refuses a foreign frame, a client does when the filter is on (else
    /// it is only counted: the "shadow"). Counts and logs (the first few, then one in 1024). `what` names the frame.
    /// `host_only`: control frames (node map, Logoff), which only the host of the network may send.
    bool RefuseForeignLocked(const WifiPacket& packet, NetworkStatus status, const char* what, u16 src, u16 dest,
                             bool host_only = false);
    bool client_foreign_filter_enforced_ = false;
    u32 foreign_logged_ = 0;
    /// Host side, the late second Deauthentication: the MACs that sent one lately (a small ring) and when.
    void NoteLeftLocked(const Mac& mac, u64 now_ms);
    bool LeftRecentlyLocked(const Mac& mac, u64 now_ms) const;
    struct LeftNode {
        Mac mac;
        u64 ms = 0; ///< 0: unused
    };
    static constexpr u32 kLeftTable = 4;
    LeftNode left_[kLeftTable];
    u32 left_next_ = 0;
    void HandleNodeMapPacket(const WifiPacket& packet);
    void HandleBeaconFrame(const WifiPacket& packet);
    void HandleAssociationResponseFrame(const WifiPacket& packet);
    void HandleEapolPacket(const WifiPacket& packet);
    void HandleSecureDataPacket(const WifiPacket& packet);
    void HandleAuthenticationFrame(const WifiPacket& packet);
    void HandleDeauthenticationFrame(const WifiPacket& packet);
    void HandleDataFrame(const WifiPacket& packet);

    void StartConnectionSequenceLocked(const Mac& server);
    /// What both BeginHosting* do once network_info_ holds the network: the host is node 1, the beacons begin.
    void StartHostingLocked();
    /// What both ConnectBegin* do once network_info_ holds the host's address: the connection sequence begins.
    void StartConnectLocked(ConnectionType type, u32* attempt);
    /// The service becomes initialised for a node that current_node_ already is (Initialize and InitializeForLobby).
    void BeginServiceLocked(u32 shared_memory_size);
    /// A beacon of the lobby's ids is filed in lobby_->heard (the network thread, mu_ held); one of other ids from a MAC that has a
    /// slot clears it; any beacon sweeps the slots silent for more than kBeaconKeepMs. Nothing at all while lobby_ is null.
    void NoteLobbyBeaconLocked(const WifiPacket& packet, u64 now);
    /// A data frame on kLobbyChannel for this node (mu_ held): kept in the lobby ring if its sender is who it says it is.
    void HandleLobbyFrameLocked(const WifiPacket& packet, const SecureDataHeader& secure_data);
    /// The lobby's state exists (allocated if need be); false = no memory.
    bool EnsureLobbyLocked();
    /// A node left: its unread lobby messages go with it.
    void PurgeLobbyMessagesLocked(u16 node);
    /// The creator's remaining clients are told who is left, with the unicast Logoff of the admission handshake (the engine's hosts
    /// send no other news of a departure, and a client's roster would show the one that left until the next admission).
    void RefreshClientsLocked();
    struct LobbyMessage {
        u8 from;
        u8 size;
        u8 data[kLobbyMessageMax];
    };
    struct HeardLobbySlot {
        HeardLobby lobby;
        bool used;
        u32 rx_ms; ///< the low 32 bits of the clock when its last beacon was heard
    };
    static_assert(sizeof(HeardLobbySlot) == 48, "a lobby heard is 48 bytes: 4 of them are 192");
    struct LobbyState {
        HeardLobbySlot heard[kHeardLobbies];
        LobbyMessage rx[kLobbyInbox];
        u8 rx_first;
        u8 rx_count;
        u32 refused;
    };
    std::unique_ptr<LobbyState> lobby_; ///< null until a lobby is used (PrepareLobby)
    bool lobby_inbox_ = false;
    /// The one place connection_status_.status changes (the mirror Tick reads without the lock follows it).
    void SetStatusLocked(NetworkStatus status);
    /// After the node table was rebuilt from the host's Logoff: a slot that emptied or holds another console now loses the
    /// frame numbers remembered for it (CopyDedupe), so the next console with that id is never taken for a copy.
    void ForgetChangedNodesLocked(const std::vector<NodeInfo>& before);
    void SendAssociationResponseFrame(const Mac& address);
    void SendBeaconLocked();

    /// Logs one line (main log and debug capture) whenever the role the game sees changes:
    /// off / idle / connecting / host / client / spectator. mu_ held.
    void TraceRoleLocked();
    /// Declared right after the lock guard of an entry point: traces the role on the way out.
    struct RoleTrace {
        explicit RoleTrace(UdsEngine* e) : engine(e) {}
        ~RoleTrace() {
            // (the check is here so that a frame that changes nothing costs two loads and a compare)
            if ((engine->initialized_ ? engine->connection_status_.status : 0u) != engine->traced_role_) {
                engine->TraceRoleLocked();
            }
        }
        UdsEngine* engine;
    };

    KernelOps* kernel_;
    Transport transport_;

    mutable platform::ContendedMutex mu_; // (counts the times it made a thread wait: Stats::lock_*)

    /// Every entry point takes mu_ through this guard. An event signalled under the lock wakes a game thread that may
    /// preempt the signaller at once (LightLock has no priority inheritance): the woken thread's next call then waits
    /// for the lock until the preempted thread runs again, a whole scheduling slice on a busy core 0 (the 10-32 ms
    /// stalls of Tetris and SSF4 in the logs of 2026-10-19). So the lock's holders only QUEUE the signals
    /// (QueueSignalLocked) and the guard gives them on the way out, after unlocking. A signal that must precede a close
    /// (FreeBinds, Unbind) is still given at once. At debug > 0 the guard also measures how long the lock was held
    /// (Stats::lock_hold_*).
    struct Guard {
        explicit Guard(const UdsEngine* e);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        const UdsEngine* engine;
    };
    static constexpr u32 kPendingSignals = kMaxBinds + 4; // every bind, the status event (twice at a stop), the connect event
    static constexpr u32 kFlushLocal = 4; // signals the guard carries on its own stack (more are given under the lock: a teardown with many binds)
    mutable u32 pending_signals_[kPendingSignals];
    mutable u32 pending_signal_count_ = 0;
    mutable u64 hold_start_ticks_ = 0;
    void QueueSignalLocked(u32 handle) const;
    /// A signal queued for `handle` is given now (the handle is about to be closed: the guard would find it closed).
    void DrainPendingSignalLocked(u32 handle) const;

    bool initialized_ = false;
    u8 service_state_ = kServiceRunning; // ServiceState: only a service stop sets it, only Shutdown clears it
    u32 shared_memory_size_ = 0;         // what the game gave Initialize (its UDS block)
    u16 version_ = 0;                    // of the last Initialize; 0 after a failed one
    bool deauth_on_destroy_ = true;      // command 0x1C
    NodeInfo current_node_;
    ConnectionStatus connection_status_{};
    NetworkInfo network_info_{};
    u16 admission_attributes_ = 0; // the attribute word the host enforces (see UpdateNetworkAttribute)
    std::vector<NodeInfo> node_info_;
    ConnectionType conn_type_ = ConnectionType::Client;
    u8 network_channel_ = kDefaultNetworkChannel;
    std::map<Mac, Node> node_map_;
    BindSlot binds_[kMaxBinds];
    u64 bind_budget_ = 0; // ... the sum of their budgets, against shared_memory_size_
    ForwardDedupe fwd_dedupe_; // client side: drops the host's re-broadcast of a frame already delivered (1 KB)
    CopyDedupe copy_dedupe_;   // drops the copy a radio_like=2 sender adds to a unicast frame (by its sequence number; 1.2 KB)
    u16 tx_seq_ = 0;           // the sequence number of the last frame SendTo built (1, 2, 3 ...; the real nwm numbers its frames)
    /// The newest beacon of every transmitter heard lately (also before Initialize and between scans:
    /// a scan reports what was heard while it listened, it does not consume anything).
    struct HeardBeacon {
        WifiPacket packet;
        u64 rx_ms;
    };
    std::list<HeardBeacon> received_beacons_;
    bool scan_filter_enforced_ = true;

    u32 connection_status_event_ = 0; // engine-owned, persistent
    u32 connect_event_ = 0;           // engine-owned, signalled when ConnectToNetwork completes
    bool connect_pending_ = false;    // a ConnectToNetwork is waiting for its answer
    bool associated_ = false;         // ... and its host has answered the Authentication
    u32 connect_result_ = 0;          // ... which FinishConnectLocked() leaves here
    u32 connect_gen_ = 0;             // the attempt in progress or the last one (a woken thread may run after a new one began)
    u32 connect_result_gen_ = 0;      // the attempt connect_result_ belongs to

    // Tick runs on the network thread every round (every few ms) and takes the lock only when there is something to do with
    // it: these three are what it reads first, without the lock (the plain fields are kept in step under the lock).
    std::atomic<bool> beacon_active_{false};
    u64 next_beacon_tenths_ = 0; ///< when the next beacon is due, in tenths of a millisecond (see Tick)
    std::atomic<u32> beacon_due_ms_{0}; ///< next_beacon_tenths_ in whole ms, truncated to 32 bits
    std::atomic<u32> absent_count_{0};  ///< absent_.size()
    std::atomic<u32> role_{0};          ///< connection_status_.status (SetStatusLocked keeps it): Tick's client bookkeeping reads it
    std::atomic<u64> host_beacon_ms_{0}; // a client's clock reading of the last beacon of its host (0 = none yet); Tick reads it without the lock
    // Beacon sync shadow: the node mask of the last host beacon whose content changed, and how many
    // beacons in a row disagreed with the Logoff-derived node set.
    u32 sync_hash_ = 0;
    u16 sync_mask_ = 0;
    bool sync_valid_ = false; ///< sync_mask_ is the mask of the beacon with hash sync_hash_
    u32 sync_streak_ = 0;

    // Who is in the room, from its member lists (see OnRoomMembers): `present_` is the last list plus the members
    // that are out of it for less than the debounce time, `absent_` those with the time they went.
    struct Absent {
        Mac mac;
        u64 since_ms;
    };
    std::vector<Mac> present_;
    std::vector<Absent> absent_;

    u32 traced_role_ = 0; // 0 = off, else the NetworkStatus last logged by TraceRoleLocked
};

} // namespace azl
