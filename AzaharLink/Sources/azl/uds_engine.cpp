// Port of Azahar's nwm_uds.cpp. Behaviour is kept identical to the emulator on purpose: the other
// side of the room is Azahar, so the frames we emit and accept must match what it expects.
#include "azl/uds_engine.hpp"

#include <algorithm>
#include <cstdio>

#include "azl/log.hpp"
#include "azl/stats.hpp"

namespace azl {

namespace {

constexpr std::size_t kMaxBeaconFrames = 15;
constexpr std::size_t kMaxSendSize = 0x5C6;
constexpr s64 kConnectTimeoutNs = 5000000000LL; // Azahar uses 5 s to accommodate high ping
constexpr u32 kBeaconIntervalMs = 102;          // 100 TU * 1.024 ms
// ... and the host's beacon timer runs on the exact period, in tenths of a millisecond: re-arming it from the moment the
// beacon was sent (a loop round after it was due) gave 105.5 ms on hardware, and a whole millisecond of rounding in the
// period a drift of its own.
constexpr u64 kBeaconPeriodTenths = 1024;
// A Deauthentication of a node that authenticated again within this long of the one it left with is the second copy of
// that goodbye (see Node::deauth_guard): Azahar's guests send two, 59 ms to 2.04 s apart.
constexpr u64 kLateDeauthMs = 3000;

// A scan listens to each channel of its mask for the dwell time (libctru: 3 x 110 ms) and reports the
// beacons heard since one beacon interval before it started; the nwm chip has no scan timeout, ours
// stops a nonsensical request at 10 s. It waits in slices so that the game's cancel event is seen.
constexpr u16 kScanChannelBits = 0x7FFF; // channels 1..15
constexpr u64 kScanSliceMs = 20;
constexpr u64 kMaxScanWindowMs = 10000;
constexpr u64 kBeaconKeepMs = 3000; // heard longer ago than this is forgotten
// A member of the room that is out of its member lists for this long has left (a player who drops and joins again
// within a moment, the way a ghost collision ends, is not taken for gone).
constexpr u64 kPresenceDebounceMs = 3000;
constexpr u8 kBroadcastFlag = 0x2;

// A bind takes 16 bytes of header and the size it declared, rounded up to 4, of the block the game gave
// Initialize (nwm's heap over it). Whatever size it declared, its ring holds at least 16 KiB: after a stall of
// the network thread (371 ms seen) the room hands over some 100 frames at once, more than the 11.8 KB ring FFE
// declares can take, while the game itself loses nothing it would have kept (spec C2; peak fill is logged).
constexpr u32 kBindHeapHeader = 16;
constexpr u32 kMinRingBytes = 0x4000;

// What Initialize accepts: a game's block is a multiple of 4 KiB, nwm refuses a version above 0x400
// (it throws a fatal error) and, from 0x200 on, tells the room with Deauthentications when a host ends.
constexpr u32 kPageMask = 0xFFF;
constexpr u16 kMaxVersion = 0x400;
constexpr u16 kDeauthVersion = 0x200;
constexpr u16 kDeprecatedVersion = 0x100;
constexpr int kDeauthBurst = 3; // frames in a "everybody out" Deauthentication burst

// NetworkInfo attribute word (UDSNETATTR_*).
constexpr u16 kAttrBlockSpectators = 0x1;
constexpr u16 kAttrBlockClients = 0x2;
constexpr u16 kAttrBlockAll = 0x4;

// Bits of a NetworkStatus in a Gate() mask.
constexpr u32 Bit(NetworkStatus s) {
    return 1u << static_cast<u32>(s);
}
constexpr u32 kAnyStatus = 0;
constexpr u32 kIdle = Bit(NetworkStatus::NotConnected);
constexpr u32 kHost = Bit(NetworkStatus::ConnectedAsHost);
constexpr u32 kConnecting = Bit(NetworkStatus::Connecting);
constexpr u32 kClient = Bit(NetworkStatus::ConnectedAsClient);
constexpr u32 kSpectator = Bit(NetworkStatus::ConnectedAsSpectator);
constexpr u32 kInSession = kHost | kClient | kSpectator;

/// Records a data frame for this node in the debug capture, whole (see stats.hpp).
void DebugSecure(u8 dir, const WifiPacket& packet, const SecureDataHeader& h) {
    const std::size_t at = kLlcHeaderSize + kSecureDataHeaderSize;
    const u8* data = nullptr;
    u32 size = 0;
    if (packet.data.size() > at) {
        data = packet.data.data() + at;
        size = static_cast<u32>(std::min<std::size_t>(h.ActualDataSize(), packet.data.size() - at));
    }
    DebugFrame(dir, h.src_node_id, h.dest_node_id, h.data_channel, data, size);
}

/// ... and a frame that is not for this node, or that is its own frame given back, as its header only: who sent it to whom,
/// on which channel and how long it was (the bytes of the frames for this node are what a capture is read for).
void DebugSecureHeader(u8 dir, const WifiPacket& packet, const SecureDataHeader& h) {
    const std::size_t at = kLlcHeaderSize + kSecureDataHeaderSize;
    const u32 size = packet.data.size() > at
                         ? static_cast<u32>(std::min<std::size_t>(h.ActualDataSize(), packet.data.size() - at))
                         : 0;
    DebugFrame(dir, h.src_node_id, h.dest_node_id, h.data_channel, nullptr, size);
}

// SendTo failures are what a game notices as "the other side does not answer"; say why, but not
// more than a handful of times per reason.
void NoteSendError(const char* why, u16 dest, u8 channel, u32 size, u8 flags) {
    ++g_stats.sendto_errors;
    static u32 shown = 0;
    if (g_debug_level > 0 && shown < 24) { // (with the log off the lines are not used up)
        ++shown;
        Log("uds: SendTo failed: %s (dest node %u, channel %u, %lu bytes, flags 0x%02x)", why, dest,
            channel, static_cast<unsigned long>(size), flags);
    }
}

} // namespace

UdsEngine::UdsEngine(KernelOps* kernel, Transport transport)
    : kernel_(kernel), transport_(std::move(transport)) {
    mu_.Watch({&g_stats.lock_contended, &g_stats.lock_wait_ticks, &g_stats.lock_wait_max, &g_stats.lock_wait_net_ticks});
    connection_status_event_ = kernel_->CreateEvent();
    connect_event_ = kernel_->CreateEvent();
    // The frame numbers start from the clock: a plugin that restarts (the game again) numbers from elsewhere than the
    // others still remember of it (CopyDedupe keeps the last 32 numbers of a node: a 1-in-2000 chance of one dropped frame).
    tx_seq_ = static_cast<u16>(platform::NowMs() & 0x7FFFu);
    std::memset(&connection_status_, 0, sizeof(connection_status_));
    std::memset(&network_info_, 0, sizeof(network_info_));
}

UdsEngine::~UdsEngine() {
    Guard lock(this);
    FreeBinds(false);
    if (connection_status_event_) {
        kernel_->CloseEvent(connection_status_event_);
    }
    if (connect_event_) {
        kernel_->CloseEvent(connect_event_);
    }
}

// ---------------------------------------------------------------------------------------------
// Helpers (mu_ held)
// ---------------------------------------------------------------------------------------------

namespace {

const char* RoleName(u32 role) {
    switch (role) {
    case 0:
        return "off"; // never initialised, or shut down
    case static_cast<u32>(NetworkStatus::NotConnected):
        return "idle";
    case static_cast<u32>(NetworkStatus::Connecting):
        return "connecting";
    case static_cast<u32>(NetworkStatus::ConnectedAsHost):
        return "host";
    case static_cast<u32>(NetworkStatus::ConnectedAsClient):
        return "client";
    case static_cast<u32>(NetworkStatus::ConnectedAsSpectator):
        return "spectator";
    }
    return "?";
}

} // namespace

// The two lines are built by hand (no printf): a game's thread that closes its fake nwm::UDS session while a network is
// up (UdsHle::NoteHandleClosed -> Shutdown, or the sleep -> StopService) gets here from inside the svc hook, on whatever
// stack the thread has (azl/line.hpp; the text is what the printf formats made, Tests/test_main.cpp checks it).
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
void UdsEngine::TraceRoleLocked() {
    const u32 role = initialized_ ? connection_status_.status : 0;
    if (role == traced_role_) {
        return;
    }
    const u32 before = traced_role_;
    traced_role_ = role;
    // A handful of lines per session: the main log gets them too, they are the frame of every
    // other line ("which role was the console in when this happened").
    {
        LogLine line;
        line.Str("uds: role ").Str(RoleName(before)).Str(" -> ").Str(RoleName(role)).Str(" (node ")
            .Dec(connection_status_.network_node_id).Str(", nodes ").Dec(connection_status_.total_nodes).Ch('/')
            .Dec(connection_status_.max_nodes).Str(", reason ").Dec(connection_status_.status_change_reason).Ch(')');
    }
    {
        DebugLine line;
        line.Str("uds role ").Str(RoleName(before)).Str(" -> ").Str(RoleName(role)).Str(" node ")
            .Dec(connection_status_.network_node_id).Str(" nodes ").Dec(connection_status_.total_nodes).Ch('/')
            .Dec(connection_status_.max_nodes).Str(" reason ").Dec(connection_status_.status_change_reason);
    }
}
// <<< game-thread code

void UdsEngine::SendPacket(WifiPacket& packet) {
    if (transport_.send) {
        transport_.send(packet);
    }
}

UdsEngine::Guard::Guard(const UdsEngine* e) : engine(e) {
    e->mu_.lock();
    StatsAdd(g_stats.lock_holds);
    if (g_debug_level > 0) {
        e->hold_start_ticks_ = platform::NowTicks();
    }
}

UdsEngine::Guard::~Guard() {
    // The signals go after the unlock, from a copy on this stack (another thread may queue its own as soon as the lock is
    // free). A small copy: the entry points that reach a game thread queue one signal (the status event) or none; a
    // teardown with many binds gives the rest under the lock, as before. The connect event (FinishConnectLocked) and an
    // event about to be closed (FreeBinds, Unbind) are signalled under the lock on purpose.
    u32 handles[kFlushLocal];
    u32 n = 0;
    for (u32 i = 0; i < engine->pending_signal_count_; ++i) {
        if (n < kFlushLocal) {
            handles[n++] = engine->pending_signals_[i];
        } else {
            engine->kernel_->SignalEvent(engine->pending_signals_[i]);
        }
    }
    engine->pending_signal_count_ = 0;
    if (g_debug_level > 0 && engine->hold_start_ticks_ != 0) {
        const u64 held = platform::NowTicks() - engine->hold_start_ticks_;
        engine->hold_start_ticks_ = 0;
        StatsAdd64(g_stats.lock_hold_ticks, held);
        const u32 clamped = held > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(held);
        if (clamped > g_stats.lock_hold_max) {
            g_stats.lock_hold_max = clamped;
            g_stats.lock_hold_max_net = platform::IsNetThread() ? 1 : 0;
        }
    }
    engine->mu_.unlock();
    for (u32 i = 0; i < n; ++i) {
        engine->kernel_->SignalEvent(handles[i]);
    }
}

void UdsEngine::DrainPendingSignalLocked(u32 handle) const {
    for (u32 i = 0; i < pending_signal_count_;) {
        if (pending_signals_[i] == handle) {
            kernel_->SignalEvent(handle);
            pending_signals_[i] = pending_signals_[--pending_signal_count_];
        } else {
            ++i;
        }
    }
}

void UdsEngine::QueueSignalLocked(u32 handle) const {
    if (handle == 0) {
        return;
    }
    // (every signal is kept, a repeated one too: the counts are the firmware's, which the tests pin)
    if (pending_signal_count_ < kPendingSignals) {
        pending_signals_[pending_signal_count_++] = handle;
    } else {
        kernel_->SignalEvent(handle); // (cannot happen: the table holds every bind and the two events of the engine)
    }
}

Mac UdsEngine::GetMacAddress() {
    return transport_.get_mac ? transport_.get_mac() : Mac{};
}

u16 UdsEngine::GetNextAvailableNodeId() {
    for (u16 index = 0; index < connection_status_.max_nodes && index < kUdsMaxNodes; ++index) {
        if ((connection_status_.node_bitmask & (1u << index)) == 0) {
            return static_cast<u16>(index + 1);
        }
    }
    return 0; // Azahar treats this as unreachable
}

UdsEngine::BindSlot* UdsEngine::FindBindLocked(u8 channel, u16 src, bool* channel_bound) {
    // nwm's lookup (fn_1114f8): in table order, the first bind of the channel that wants this very node wins at
    // once; otherwise the last one that takes every node.
    BindSlot* wildcard = nullptr;
    *channel_bound = false;
    for (BindSlot& bind : binds_) {
        if (bind.id == 0 || bind.channel != channel) {
            continue;
        }
        *channel_bound = true;
        if (bind.src == src) {
            return &bind;
        }
        if (bind.src == kBroadcastNetworkNodeId) {
            wildcard = &bind;
        }
    }
    return wildcard;
}

UdsEngine::BindSlot* UdsEngine::FindBindIdLocked(u32 id) {
    if (id == 0) {
        return nullptr;
    }
    for (BindSlot& bind : binds_) {
        if (bind.id == id) {
            return &bind;
        }
    }
    return nullptr;
}

void UdsEngine::SignalBinds() {
    for (BindSlot& bind : binds_) {
        if (bind.id != 0) {
            QueueSignalLocked(bind.event);
            ++g_stats.binds_signalled;
        }
    }
}

void UdsEngine::SetStatusLocked(NetworkStatus status) {
    connection_status_.status = static_cast<u32>(status);
    role_.store(connection_status_.status, std::memory_order_relaxed);
    if (status == NetworkStatus::ConnectedAsClient || status == NetworkStatus::ConnectedAsSpectator) {
        host_beacon_ms_ = 0; // (the host's clock starts with the session: Tick sets it at its first round)
    }
}

void UdsEngine::ForgetChangedNodesLocked(const std::vector<NodeInfo>& before) {
    for (std::size_t i = 0; i < node_info_.size(); ++i) {
        const bool was = i < before.size() && before[i].network_node_id != 0;
        const bool is = node_info_[i].network_node_id != 0;
        if (was && (!is || node_info_[i].friend_code_seed != before[i].friend_code_seed)) {
            copy_dedupe_.ForgetNode(static_cast<u16>(i + 1));
        }
    }
}

void UdsEngine::FreeBinds(bool signal) {
    for (BindSlot& bind : binds_) {
        if (bind.id == 0) {
            continue;
        }
        DrainPendingSignalLocked(bind.event); // (a teardown queued one: given before the close, as before the round)
        if (signal) {
            kernel_->SignalEvent(bind.event);
        }
        kernel_->CloseEvent(bind.event);
        bind = BindSlot();
    }
    bind_budget_ = 0;
}

// A deliberate deviation from nwm. On a console the binds survive the end of a session (only Shutdown and a
// service stop free them) and, by a static reading of the firmware, so do the frames still queued in them.
// Nothing proves that a game never trips over a stale frame, while Azahar clears everything and the games tested
// through it work: here the frames go when the next session begins (and are counted) and the binds stay. If
// the hardware shows that the frames are wanted, emptying this function restores nwm's behaviour.
void UdsEngine::FlushRingsLocked() {
    u32 flushed = 0;
    for (BindSlot& bind : binds_) {
        if (bind.id != 0) {
            flushed += bind.ring.Flush();
        }
    }
    if (flushed) {
        g_stats.ring_flushed += flushed;
        Log("uds: %lu frame(s) of the last session were still queued when this one began: dropped",
            static_cast<unsigned long>(flushed));
    }
}

bool UdsEngine::GetSendAddress(u16 dest_node_id, u8 flags, Mac* out) {
    if ((flags & kBroadcastFlag) || dest_node_id == kBroadcastNetworkNodeId) {
        *out = kBroadcastMac;
        return true;
    }
    if (dest_node_id == kHostDestNodeId) {
        *out = network_info_.host_mac_address;
        return true;
    }
    for (const auto& node : node_map_) {
        if (node.second.node_id == dest_node_id && node.second.connected) {
            *out = node.first;
            return true;
        }
    }
    // The firmware never looks the destination up: it puts the frame in an aggregate and the radio
    // sends it. A node the game's status block lists but whose MAC we have not learnt (the node
    // list and the Logoff race each other) is reached the way a console reaches it: a client hands
    // the frame to its host, which relays it; a host has no better address than "everybody".
    if (dest_node_id >= 1 && dest_node_id <= kUdsMaxNodes &&
        (connection_status_.node_bitmask & (1u << (dest_node_id - 1)))) {
        *out = connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsHost)
                   ? kBroadcastMac
                   : network_info_.host_mac_address;
        return true;
    }
    return false; // not in the network: the frame would be sent and nobody would take it
}

u32 UdsEngine::Gate(u32 status_mask, bool prefix) const {
    if (prefix) {
        if (service_state_ == kServiceWirelessOff) {
            return res::kWirelessOff;
        }
        if (service_state_ != kServiceRunning) {
            return res::kWrongState;
        }
    }
    if (!initialized_) {
        return res::kWrongState;
    }
    // A stopped service has no status at all, so a command that only looks at the status refuses too.
    if (service_state_ != kServiceRunning && status_mask != kAnyStatus) {
        return res::kWrongState;
    }
    if (status_mask != kAnyStatus && !(status_mask & (1u << connection_status_.status))) {
        return res::kWrongState;
    }
    return res::kSuccess;
}

bool UdsEngine::AdmissionBlocked() const {
    return (admission_attributes_ & (kAttrBlockClients | kAttrBlockAll)) != 0;
}

void UdsEngine::BroadcastNodeMap() {
    // Note: this is not how UDS on a real 3DS does it, but Azahar relies on it.
    std::vector<NodeMapEntry> entries;
    for (const auto& node : node_map_) {
        if (node.second.connected && !node.second.spec) {
            entries.push_back({node.first, node.second.node_id});
        }
    }
    WifiPacket packet;
    packet.channel = network_channel_;
    packet.type = WifiPacket::Type::NodeMap;
    packet.destination_address = kBroadcastMac;
    packet.data = GenerateNodeMapPacket(entries);
    SendPacket(packet);
}

void UdsEngine::Teardown(NetworkStatusChangeReason reason, bool send_deauth) {
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (send_deauth) {
        WifiPacket deauth;
        deauth.channel = network_channel_;
        deauth.type = WifiPacket::Type::Deauthentication;
        if (status == NetworkStatus::ConnectedAsHost) {
            if (deauth_on_destroy_ && version_ >= kDeauthVersion) {
                deauth.destination_address = kBroadcastMac;
                for (int i = 0; i < kDeauthBurst; ++i) {
                    SendPacket(deauth);
                }
            }
        } else if (status == NetworkStatus::ConnectedAsClient ||
                   status == NetworkStatus::ConnectedAsSpectator ||
                   (status == NetworkStatus::Connecting && associated_)) {
            // (a host that never answered has nothing to forget: the chip does not disconnect from it)
            deauth.destination_address = network_info_.host_mac_address;
            SendPacket(deauth);
        }
    }

    // The status block keeps the node id (a game still reads it after it left) and nothing else.
    const u16 node_id = connection_status_.network_node_id;
    std::memset(&connection_status_, 0, sizeof(connection_status_));
    SetStatusLocked(NetworkStatus::NotConnected);
    connection_status_.status_change_reason = static_cast<u32>(reason);
    connection_status_.network_node_id = node_id;
    beacon_active_ = false;
    host_beacon_ms_ = 0;
    associated_ = false;
    sync_valid_ = false;
    sync_streak_ = 0;
    node_map_.clear();
    node_map_text_.clear();
    for (LeftNode& left : left_) {
        left.ms = 0;
    }
    fwd_dedupe_.Reset();
    copy_dedupe_.Reset(); // (the next session may have another host)
    if (status == NetworkStatus::Connecting) {
        FinishConnectLocked(res::kConnectionFailed);
    }
    // The real service signals the status event after every way back to idle, the game's own
    // Destroy and Disconnect included, and wakes whoever waits for data on a bind. The binds stay.
    QueueSignalLocked(connection_status_event_);
    SignalBinds();
}

bool UdsEngine::RemoveNodeLocked(std::map<Mac, Node>::iterator it, RemovedBy cause) {
    const Node node = it->second;
    node_map_.erase(it);
    if (!node.connected) {
        return false; // never got past the handshake: the game does not know it
    }
    copy_dedupe_.ForgetNode(node.node_id); // (the next console with this id numbers its frames from its own start)
    auto node_it = std::find_if(node_info_.begin(), node_info_.end(), [&node](const NodeInfo& info) {
        return info.network_node_id == node.node_id;
    });
    if (node_it == node_info_.end()) {
        return false;
    }
    ++g_stats.node_removed[cause];
    if (!node.spec && node.node_id >= 1 && node.node_id <= kUdsMaxNodes) {
        connection_status_.node_bitmask &= static_cast<u16>(~(1u << (node.node_id - 1)));
        connection_status_.changed_nodes |= static_cast<u16>(1u << (node.node_id - 1));
        connection_status_.nodes[node.node_id - 1] = 0;
        if (connection_status_.total_nodes) {
            connection_status_.total_nodes--;
        }
        if (network_info_.total_nodes) {
            network_info_.total_nodes--;
        }
    }
    node_it->Reset();
    return true;
}

void UdsEngine::FinishConnectLocked(u32 result) {
    if (!connect_pending_) {
        return;
    }
    connect_pending_ = false;
    connect_result_ = result;
    connect_result_gen_ = connect_gen_;
    // At once, under the lock: the waiter re-takes the lock anyway, and a signal given after the unlock could land after the
    // next attempt's ClearEvent (ConnectToNetwork) and end that attempt as a timeout.
    kernel_->SignalEvent(connect_event_);
}

bool UdsEngine::HostBeaconKnown(const Mac& host) const {
    const u64 now = platform::NowMs();
    for (const HeardBeacon& beacon : received_beacons_) {
        if (beacon.packet.transmitter_address == host && beacon.rx_ms + kBeaconKeepMs >= now) {
            return true;
        }
    }
    return false;
}

void UdsEngine::SendEapolLogoffLocked(const Mac& to, u16 assigned_node_id) {
    WifiPacket eapol_logoff;
    eapol_logoff.channel = network_channel_;
    eapol_logoff.data = GenerateEapolLogoffFrame(assigned_node_id, node_info_, network_info_.max_nodes,
                                                 network_info_.total_nodes);
    eapol_logoff.destination_address = to;
    eapol_logoff.type = WifiPacket::Type::Data;
    SendPacket(eapol_logoff);
}

// ---------------------------------------------------------------------------------------------
// Frame handlers (mu_ held)
// ---------------------------------------------------------------------------------------------

void UdsEngine::NotePeerSeedLocked(const NodeInfo& peer) {
    if (seed_said_ || peer.network_node_id == connection_status_.network_node_id ||
        peer.friend_code_seed != current_node_.friend_code_seed) {
        return;
    }
    seed_said_ = true;
    Log("uds: WARNING node %u announces the same friend code seed as this console (%08lx%08lx): a game that tells"
        " consoles apart by it will see one",
        peer.network_node_id, static_cast<unsigned long>(peer.friend_code_seed >> 32),
        static_cast<unsigned long>(peer.friend_code_seed));
}

void UdsEngine::LogNodeMapLocked(const char* why) {
    if (g_debug_level <= 0) {
        return;
    }
    struct Row {
        u32 key; ///< node id, spectators last
        std::string text;
    };
    std::vector<Row> rows;
    const auto describe = [this, &rows](u32 key, u16 node_id, const Mac& mac, bool spectator) {
        char name[11] = {};
        if (!spectator && node_id >= 1 && node_id <= node_info_.size()) {
            const NodeInfo& info = node_info_[node_id - 1];
            for (int i = 0; i < 10 && info.username[i]; ++i) {
                name[i] = info.username[i] < 0x80 && info.username[i] >= 0x20 ? static_cast<char>(info.username[i]) : '?';
            }
        }
        char text[96];
        std::snprintf(text, sizeof(text), "%s%u=%02X:%02X:%02X:%02X:%02X:%02X '%s'", spectator ? "spectator " : "node ",
                      spectator ? 0u : static_cast<unsigned>(node_id), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
        rows.push_back({key, text});
    };
    const bool hosting = connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsHost);
    describe(1, 1, hosting ? GetMacAddress() : network_info_.host_mac_address, false);
    for (const auto& entry : node_map_) {
        if (entry.second.connected && !(entry.second.node_id == 1 && !entry.second.spec)) {
            describe(entry.second.spec ? 0x100 : entry.second.node_id, entry.second.node_id, entry.first, entry.second.spec);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.key < b.key; });
    std::string text;
    for (const Row& row : rows) {
        text += (text.empty() ? "" : ", ") + row.text;
    }
    if (text == node_map_text_) {
        return;
    }
    node_map_text_ = text;
    Log("uds: nodes (%s): %s", why, text.c_str());
}

// The room carries the frames of every network in it, and two lobbies of one game have the same node ids (host 1, then
// 2, 3, 4): a frame from the other lobby looks like a frame of ours. What tells them apart is the transmitter, which the
// SENDER's Azahar sets to its room MAC on every frame (the room relays it unchanged: room_routing_probe): the host of our
// network, or a node it announced (the host's node map is what a client learns the nodes' MACs from, and what a host built
// itself).
bool UdsEngine::IsNetworkMemberLocked(const Mac& transmitter, NetworkStatus /*status*/) const {
    if (transmitter == network_info_.host_mac_address) {
        return true; // (a host's own is in there too: its echo is dropped by the echo rule, not taken for a stranger)
    }
    const auto it = node_map_.find(transmitter);
    return it != node_map_.end() && it->second.connected;
}

bool UdsEngine::RefuseForeignLocked(const WifiPacket& packet, NetworkStatus status, const char* what, u16 src, u16 dest,
                                    bool host_only) {
    const Mac& from = packet.transmitter_address;
    if (host_only ? from == network_info_.host_mac_address : IsNetworkMemberLocked(from, status)) {
        return false;
    }
    const bool refuse = status == NetworkStatus::ConnectedAsHost || client_foreign_filter_enforced_;
    ++(refuse ? g_stats.drop_foreign : g_stats.foreign_shadow);
    // The first few, then one in 1024: a second lobby in the room sends a hundred of these a second.
    if (++foreign_logged_ <= 8 || (foreign_logged_ & 0x3FF) == 0) {
        Log("uds: %s from %02X:%02X:%02X:%02X:%02X:%02X (node %u>%u) is not from this network (%s): %s (%lu so far)", what,
            from[0], from[1], from[2], from[3], from[4], from[5], src, dest,
            host_only ? "not the host" : "neither the host nor one of its nodes",
            refuse ? "dropped" : "passed on, the client filter only counts",
            static_cast<unsigned long>(foreign_logged_));
    }
    return refuse;
}

void UdsEngine::SetClientForeignFilterEnforced(bool enforced) {
    Guard lock(this);
    client_foreign_filter_enforced_ = enforced;
}

// A Deauthentication a host handled, so that the same MAC authenticating again a moment later is known to have a
// second copy of it on its way (see Node::deauth_guard).
void UdsEngine::NoteLeftLocked(const Mac& mac, u64 now_ms) {
    for (LeftNode& left : left_) {
        if (left.ms != 0 && left.mac == mac) {
            left.ms = now_ms ? now_ms : 1;
            return;
        }
    }
    LeftNode& slot = left_[left_next_++ % kLeftTable];
    slot.mac = mac;
    slot.ms = now_ms ? now_ms : 1;
}

bool UdsEngine::LeftRecentlyLocked(const Mac& mac, u64 now_ms) const {
    for (const LeftNode& left : left_) {
        if (left.ms != 0 && left.mac == mac && now_ms - left.ms < kLateDeauthMs) {
            return true;
        }
    }
    return false;
}

void UdsEngine::HandleNodeMapPacket(const WifiPacket& packet) {
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (status == NetworkStatus::ConnectedAsHost) {
        return; // the host is the source of truth
    }
    std::vector<NodeMapEntry> entries;
    if (!ParseNodeMapPacket(packet.data, &entries)) {
        return;
    }
    // Only the host of the network we are in (or about to join) tells us who its nodes are.
    if ((status == NetworkStatus::Connecting || status == NetworkStatus::ConnectedAsClient ||
         status == NetworkStatus::ConnectedAsSpectator) &&
        RefuseForeignLocked(packet, status, "node map", 0, 0, true)) {
        return;
    }
    node_map_.clear();
    for (const auto& entry : entries) {
        Node& node = node_map_[entry.mac];
        node.connected = true;
        node.node_id = entry.node_id;
    }
}

void UdsEngine::HandleBeaconFrame(const WifiPacket& packet) {
    const u64 now = platform::NowMs();
    HeardBeacon* heard = nullptr;
    for (HeardBeacon& beacon : received_beacons_) {
        if (beacon.packet.transmitter_address == packet.transmitter_address) {
            heard = &beacon;
            break;
        }
    }
    if (heard) { // only the newest beacon of a transmitter is kept, where its entry was
        heard->packet = packet;
        heard->rx_ms = now;
    } else {
        if (received_beacons_.size() >= kMaxBeaconFrames) {
            // (a client's GetApplicationData reads the beacon of its host: with a busy room that one is not the one to go)
            const u32 st = connection_status_.status;
            const bool joined = st == static_cast<u32>(NetworkStatus::Connecting) ||
                                st == static_cast<u32>(NetworkStatus::ConnectedAsClient) ||
                                st == static_cast<u32>(NetworkStatus::ConnectedAsSpectator);
            const Mac keep = network_info_.host_mac_address;
            received_beacons_.erase(std::min_element(
                received_beacons_.begin(), received_beacons_.end(), [joined, &keep](const HeardBeacon& a, const HeardBeacon& b) {
                    const bool pa = joined && a.packet.transmitter_address == keep;
                    const bool pb = joined && b.packet.transmitter_address == keep;
                    return pa != pb ? pb : a.rx_ms < b.rx_ms;
                }));
        }
        received_beacons_.push_back({packet, now});
    }
    // How often the host of the network we are in is heard (a real client watches for this; the
    // stats only report it for now).
    if ((connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsClient) ||
         connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsSpectator)) &&
        packet.transmitter_address == network_info_.host_mac_address) {
        host_beacon_ms_ = now;
        ++g_stats.host_beacons;
        CheckBeaconSyncLocked(packet);
    }
}

void UdsEngine::CheckBeaconSyncLocked(const WifiPacket& beacon) {
    const BeaconTags tags = FindBeaconTags(beacon.data);
    // The node list is decrypted only when the beacon's content differs from the last one's (the
    // host repeats the same beacon ten times a second between two changes).
    u32 hash = 2166136261u; // FNV-1a
    auto mix = [&hash](const u8* tag, u32 size) {
        for (u32 i = 0; tag && i < size; ++i) {
            hash = (hash ^ tag[i]) * 16777619u;
        }
    };
    mix(tags.network, tags.network_size);
    mix(tags.nodes[0], tags.nodes_size[0]);
    mix(tags.nodes[1], tags.nodes_size[1]);
    if (!sync_valid_ || hash != sync_hash_) {
        u16 mask = 0;
        if (!GetBeaconNodeMask(tags, beacon.transmitter_address, &mask)) {
            return; // not a beacon of this format (another beacon key, a truncated tag): nothing to compare
        }
        sync_hash_ = hash;
        sync_mask_ = mask;
        sync_valid_ = true;
    }
    ++g_stats.sync_checks;
    if (sync_mask_ == connection_status_.node_bitmask) {
        sync_streak_ = 0;
        return;
    }
    ++g_stats.sync_disagree;
    if (++sync_streak_ > g_stats.sync_streak_max) {
        g_stats.sync_streak_max = sync_streak_;
    }
    // A beacon built just before a join can disagree for one interval; a longer streak is a real
    // difference (what a sync would have to act on): worth a line, a few times per session.
    static u32 shown = 0;
    if (sync_streak_ == 3 && g_debug_level > 0 && shown < 16) {
        ++shown;
        Log("uds: beacon sync (shadow): the host's beacons list nodes 0x%04x, the Logoff frames built"
            " 0x%04x (%u beacons in a row)", sync_mask_, connection_status_.node_bitmask,
            static_cast<unsigned>(sync_streak_));
    }
}

void UdsEngine::HandleAssociationResponseFrame(const WifiPacket& packet) {
    u16 status = 0, assoc_id = 0;
    if (!GetAssociationResult(packet.data, &status, &assoc_id) || status != 0) {
        Log("uds: association rejected (status %u)", status);
        return;
    }
    if (connection_status_.status != static_cast<u32>(NetworkStatus::Connecting)) {
        return;
    }
    associated_ |= packet.transmitter_address == network_info_.host_mac_address;
    WifiPacket eapol_start;
    eapol_start.channel = network_channel_;
    eapol_start.data = GenerateEapolStartFrame(assoc_id, conn_type_, current_node_);
    eapol_start.destination_address = packet.transmitter_address;
    eapol_start.type = WifiPacket::Type::Data;
    SendPacket(eapol_start);
}

void UdsEngine::HandleEapolPacket(const WifiPacket& packet) {
    u16 eapol_type = 0;
    if (!GetEapolFrameType(packet.data, &eapol_type)) {
        return;
    }
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);

    if (eapol_type == kEapolStartMagic) {
        if (status != NetworkStatus::ConnectedAsHost) {
            return;
        }
        auto node_it = node_map_.find(packet.transmitter_address);
        if (node_it == node_map_.end()) {
            return; // the Authentication frame of this client was never received
        }
        if (node_it->second.connected) {
            // A client we admitted asks again (its answer was lost, or it starts over): the same
            // answer, to that client alone, whatever the lobby's attributes say now (nwm looks for the
            // same MAC before it looks at them). A broadcast one would be taken as their own admission by
            // every client that is still Connecting (Azahar adopts any Logoff it sees).
            SendEapolLogoffLocked(packet.transmitter_address, node_it->second.node_id);
            return;
        }
        if (AdmissionBlocked()) {
            return; // (a closed lobby refuses with a Logoff the Azahar side could not read: stay silent)
        }
        if (connection_status_.max_nodes == connection_status_.total_nodes) {
            return;
        }
        EapolStart start;
        if (!ParseEapolStartFrame(packet.data, &start)) {
            return;
        }
        NodeInfo node = start.node;

        if (start.connection_type == static_cast<u8>(ConnectionType::Client)) {
            const u16 node_id = GetNextAvailableNodeId();
            if (node_id == 0 || node_id > node_info_.size()) {
                return;
            }
            node.network_node_id = node_id;
            NotePeerSeedLocked(node);

            connection_status_.node_bitmask |= static_cast<u16>(1u << (node_id - 1));
            connection_status_.changed_nodes |= static_cast<u16>(1u << (node_id - 1));
            connection_status_.nodes[node_id - 1] = node.network_node_id;
            connection_status_.total_nodes++;

            copy_dedupe_.ForgetNode(node_id); // (a new console in this slot numbers its frames from its own start)
            node_info_[node_id - 1] = node;
            network_info_.total_nodes++;

            Node& entry = node_map_[packet.transmitter_address];
            entry.node_id = node.network_node_id;
            entry.connected = true;
            entry.spec = false;

            BroadcastNodeMap();
            LogNodeMapLocked("a client joined");
        } else if (start.connection_type == static_cast<u8>(ConnectionType::Spectator)) {
            Node& entry = node_map_[packet.transmitter_address];
            entry.node_id = kNodeIdSpec;
            entry.connected = true;
            entry.spec = true;
            LogNodeMapLocked("a spectator joined");
        } else {
            Log("uds: client used unknown connection type 0x%x", start.connection_type);
            return; // (a Logoff with id 0 would be adopted by every client that is still Connecting)
        }

        // Azahar broadcasts the EAPoL-Logoff (a real 3DS would only unicast it to the new client).
        SendEapolLogoffLocked(kBroadcastMac, node.network_node_id);

        QueueSignalLocked(connection_status_event_);
    } else if (status == NetworkStatus::Connecting) {
        EapolLogoff logoff;
        if (!ParseEapolLogoffFrame(packet.data, &logoff)) {
            return;
        }
        // (the Logoff that admits us comes from the host we asked: another lobby's host admitting somebody else is not it)
        if (RefuseForeignLocked(packet, status, "EAPoL Logoff", 0, 0, true)) {
            return;
        }
        // The host broadcasts the Logoff of every admission (a real console unicasts it to the joiner: the id it names is
        // the joiner's own). Two clients that join within one round trip would both take the first one and share a node id. The slot
        // a Logoff names must hold THIS console's node info; one that does not list the slot at all is taken as before.
        if (conn_type_ == ConnectionType::Client && logoff.assigned_node_id != 0) {
            for (const NodeInfo& node : logoff.nodes) {
                if (node.network_node_id == logoff.assigned_node_id &&
                    (node.friend_code_seed != current_node_.friend_code_seed ||
                     std::memcmp(node.username, current_node_.username, sizeof(node.username)) != 0)) {
                    static u32 shown = 0;
                    if (shown < 8) {
                        ++shown;
                        Log("uds: a Logoff of the host names node %u for somebody else: still waiting for ours",
                            static_cast<unsigned>(logoff.assigned_node_id));
                    }
                    return;
                }
            }
        }
        const u8 max_nodes = std::min<u8>(logoff.max_nodes, kUdsMaxNodes);

        network_info_.host_mac_address = packet.transmitter_address;
        network_info_.total_nodes = logoff.connected_nodes;
        network_info_.max_nodes = max_nodes;

        connection_status_.network_node_id = logoff.assigned_node_id;
        connection_status_.total_nodes = logoff.connected_nodes;
        connection_status_.max_nodes = max_nodes;

        const std::vector<NodeInfo> before = node_info_;
        node_info_.clear();
        node_info_.resize(max_nodes);
        for (const NodeInfo& node : logoff.nodes) {
            const u16 index = node.network_node_id;
            if (index == 0 || index > max_nodes) {
                continue;
            }
            NotePeerSeedLocked(node);
            connection_status_.node_bitmask |= static_cast<u16>(1u << (index - 1));
            connection_status_.changed_nodes |= static_cast<u16>(1u << (index - 1));
            connection_status_.nodes[index - 1] = index;
            node_info_[index - 1] = node;
        }
        ForgetChangedNodesLocked(before);

        FlushRingsLocked();
        if (conn_type_ == ConnectionType::Client) {
            SetStatusLocked(NetworkStatus::ConnectedAsClient);
        } else if (conn_type_ == ConnectionType::Spectator) {
            SetStatusLocked(NetworkStatus::ConnectedAsSpectator);
        } else {
            Log("uds: unknown connection type 0x%x", static_cast<unsigned>(conn_type_));
        }

        connection_status_.status_change_reason =
            static_cast<u32>(NetworkStatusChangeReason::NetworkIsAvailable);
        QueueSignalLocked(connection_status_event_);
        FinishConnectLocked(res::kSuccess);
        LogNodeMapLocked("the host's Logoff frame"); // (the host's node map came just before it: the MACs are in, the names are now)
    } else if (status == NetworkStatus::ConnectedAsClient ||
               status == NetworkStatus::ConnectedAsSpectator) {
        // Azahar's host does not broadcast node information any other way, so the Logoff frame
        // doubles as a "node list changed" notification for the clients already connected.
        EapolLogoff logoff;
        if (!ParseEapolLogoffFrame(packet.data, &logoff)) {
            return;
        }
        if (RefuseForeignLocked(packet, status, "EAPoL Logoff", 0, 0, true)) {
            return; // (a Logoff of another lobby would rewrite the node list the game sees)
        }
        const u8 max_nodes = network_info_.max_nodes > kUdsMaxNodes ? static_cast<u8>(kUdsMaxNodes)
                                                                      : network_info_.max_nodes;
        network_info_.total_nodes = logoff.connected_nodes;
        connection_status_.total_nodes = logoff.connected_nodes;
        std::memset(connection_status_.nodes, 0, sizeof(connection_status_.nodes));

        const u16 old_bitmask = connection_status_.node_bitmask;
        connection_status_.node_bitmask = 0;

        const std::vector<NodeInfo> before = node_info_;
        node_info_.clear();
        node_info_.resize(max_nodes);
        for (const NodeInfo& node : logoff.nodes) {
            const u16 index = node.network_node_id;
            if (index == 0 || index > max_nodes) {
                continue;
            }
            connection_status_.node_bitmask |= static_cast<u16>(1u << (index - 1));
            connection_status_.nodes[index - 1] = index;
            node_info_[index - 1] = node;
        }
        ForgetChangedNodesLocked(before);
        if (logoff.assigned_node_id != 0 && logoff.assigned_node_id != connection_status_.network_node_id) {
            // (every admission Logoff names the joiner: a console that restarted into its old slot looks the same in the table
            //  but numbers its frames from a new start, which could fall inside what is remembered of it)
            copy_dedupe_.ForgetNode(logoff.assigned_node_id);
        }
        // (nwm ORs the changes in, and only GetConnectionStatus clears them: the status event is one-shot, so two refreshes
        // before the game reads the status must leave both bits, or the SDK never asks about the first node)
        connection_status_.changed_nodes |= static_cast<u16>(old_bitmask ^ connection_status_.node_bitmask);

        QueueSignalLocked(connection_status_event_);
        LogNodeMapLocked("the host's Logoff frame");
    }
}

void UdsEngine::HandleSecureDataPacket(const WifiPacket& packet) {
    SecureDataHeader secure_data;
    if (!ParseSecureDataHeader(packet.data, &secure_data)) {
        return;
    }
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (status != NetworkStatus::ConnectedAsHost && status != NetworkStatus::ConnectedAsClient &&
        status != NetworkStatus::ConnectedAsSpectator) {
        ++g_stats.drop_not_connected;
        return;
    }

    // A host takes a frame only from the nodes of its own network - before the echo rule (a second lobby's host has OUR
    // node id) and before routing (a router forwards what it is given: the stranger's frame would reach our players). A
    // client does the same further down, after the checks TryDiscardEncoded makes for it.
    if (status == NetworkStatus::ConnectedAsHost &&
        RefuseForeignLocked(packet, status, "data frame", secure_data.src_node_id, secure_data.dest_node_id)) {
        DebugFrame(kDirOther, secure_data.src_node_id, secure_data.dest_node_id, secure_data.data_channel, nullptr,
                   secure_data.ActualDataSize());
        return;
    }

    if (secure_data.src_node_id == connection_status_.network_node_id) {
        ++g_stats.drop_echo;
        DebugSecureHeader(kDirEcho, packet, secure_data);
        return; // our own frame echoed back
    }

    if (secure_data.dest_node_id != connection_status_.network_node_id &&
        secure_data.dest_node_id != kBroadcastNetworkNodeId) {
        // Not addressed to us. Only the host acts as a router, and like the real nwm (which excludes
        // the host's own id and tests the 802.3 destination before the node ids) only for a frame that
        // was sent to ITS address: one sent to the broadcast address has reached every member of the
        // room already, a copy would only make the others see it twice.
        if (status == NetworkStatus::ConnectedAsHost && packet.destination_address != kBroadcastMac) {
            if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {
                ++g_stats.drop_dup_copy; // (the sender's second copy: the one that went through is enough for the receiver)
                DebugSecureHeader(kDirDup, packet, secure_data);
                return;
            }
            WifiPacket out_packet = packet;
            out_packet.destination_address = kBroadcastMac;
            out_packet.relayed = true; // (for the room client: a unicast frame of the radio, retried, not a broadcast)
            out_packet.peer_numbers = copy_dedupe_.Numbers(secure_data.dest_node_id);
            SendPacket(out_packet);
            ++g_stats.forwarded;
        } else {
            ++g_stats.drop_not_for_me;
            DebugSecureHeader(kDirOther, packet, secure_data);
        }
        return;
    }

    if (secure_data.is_management) {
        ++g_stats.drop_management;
        return; // management frames are neither sent nor handled
    }

    // The same for a client or spectator: a frame of another lobby in this room that is addressed to our node id (or to
    // everybody). Counted and logged always; dropped only when the filter is switched on.
    if (status != NetworkStatus::ConnectedAsHost &&
        RefuseForeignLocked(packet, status, "data frame", secure_data.src_node_id, secure_data.dest_node_id)) {
        DebugFrame(kDirOther, secure_data.src_node_id, secure_data.dest_node_id, secure_data.data_channel, nullptr,
                   secure_data.ActualDataSize());
        return;
    }

    bool channel_bound = false;
    BindSlot* bind = FindBindLocked(secure_data.data_channel, secure_data.src_node_id, &channel_bound);
    if (!bind) {
        if (channel_bound) {
            ++g_stats.drop_src_filter;
        } else {
            ++g_stats.drop_no_bind;
            if (g_stats.drop_no_bind <= 8 || (g_stats.drop_no_bind & 0x3FF) == 0) {
                Log("uds: frame for unbound data channel %u from node %u (dropped %lu so far)",
                    secure_data.data_channel, secure_data.src_node_id,
                    static_cast<unsigned long>(g_stats.drop_no_bind));
            }
        }
        return;
    }

    // The host's re-broadcast of a frame that was meant for us (see uds_dedupe.hpp). Only a frame for our own
    // node id, sent to the broadcast MAC by a node that is not the host, can have a forwarded twin: the host's
    // own frames, unicast-MAC frames and 0xFFFF broadcasts reach us exactly once. Never while hosting (nobody
    // forwards to a host), never without an id of our own.
    bool twin_unmatched = false; // (a forwarded copy with no direct twin: counted below, once the copy rule let it through)
    if (status != NetworkStatus::ConnectedAsHost && packet.destination_address == kBroadcastMac &&
        connection_status_.network_node_id != 0 && secure_data.dest_node_id == connection_status_.network_node_id &&
        secure_data.src_node_id != kHostDestNodeId) {
        const u32 now = static_cast<u32>(platform::NowMs());
        const u16 size = static_cast<u16>(packet.data.size());
        const u32 hash = HashFrame(packet.data.data(), packet.data.size());
        if (packet.transmitter_address == network_info_.host_mac_address) {
            ++g_stats.fwd_candidates;
            if (fwd_dedupe_.ConsumeForwarded(secure_data.src_node_id, size, hash, now)) {
                ++g_stats.drop_fwd_dup;
                // (header only: the bytes are those of the Rx record of the direct copy)
                DebugFrame(kDirDup, secure_data.src_node_id, secure_data.dest_node_id, secure_data.data_channel,
                           nullptr, secure_data.ActualDataSize());
                return;
            }
            // nothing to pair it with: the only copy there is, delivered (a 3DS host forwards a frame that was
            // sent to its own MAC this way, and a direct copy may have been lost)
            twin_unmatched = true;
        } else if (fwd_dedupe_.NoteDirect(secure_data.src_node_id, size, hash, now)) {
            ++g_stats.fwd_evictions;
        }
    }

    // The copy a radio_like=2 sender adds (the same frame, the same number, a round later): the first one was delivered.
    if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {
        ++g_stats.drop_dup_copy;
        DebugFrame(kDirDup, secure_data.src_node_id, secure_data.dest_node_id, secure_data.data_channel, nullptr,
                   secure_data.ActualDataSize());
        return;
    }
    if (twin_unmatched) {
        ++g_stats.fwd_unmatched;
        DebugEvent("uds twin: node %u>%u ch%u %uB came through the host with no direct copy: delivered",
                   secure_data.src_node_id, secure_data.dest_node_id, secure_data.data_channel,
                   static_cast<unsigned>(secure_data.ActualDataSize()));
    }

    const std::size_t payload_at = kLlcHeaderSize + kSecureDataHeaderSize;
    const u32 n = static_cast<u32>(std::min<std::size_t>(secure_data.ActualDataSize(), packet.data.size() - payload_at));
    const u32 overwritten = bind->ring.overwritten();
    // (the stamp is for the queued -> PullPacket latency of the statistics: a clock read per frame, only with the log on)
    const u32 stamp_ms = g_debug_level > 0 ? static_cast<u32>(platform::NowMs()) : 0;
    if (bind->ring.Enqueue(secure_data.dest_node_id, secure_data.src_node_id, secure_data.data_channel,
                           packet.data.data() + payload_at, n, stamp_ms) != RxRing::Put::Stored) {
        ++g_stats.drop_bad_size;
        return;
    }
    g_stats.ring_overwritten += bind->ring.overwritten() - overwritten;
    ++g_stats.data_delivered;
    DebugSecure(kDirRx, packet, secure_data);
    if (bind->ring.used_units() > g_stats.ring_peak_units) {
        g_stats.ring_peak_units = bind->ring.used_units();
    }
    QueueSignalLocked(bind->event); // (given once the lock is free: the game's PullPacket must not find it taken)
}

void UdsEngine::StartConnectionSequenceLocked(const Mac& server) {
    SetStatusLocked(NetworkStatus::Connecting);
    associated_ = false;
    sync_valid_ = false;
    sync_streak_ = 0;
    fwd_dedupe_.Reset();
    copy_dedupe_.Reset();

    // A host that still has us from before (we left without a Deauthentication) would ignore the
    // Authentication: tell it that we start over. Only a host whose beacon we heard gets this, never
    // a guessed address; the room delivers both frames in this order.
    if (HostBeaconKnown(server)) {
        WifiPacket deauth;
        deauth.channel = network_channel_;
        deauth.type = WifiPacket::Type::Deauthentication;
        deauth.destination_address = server;
        SendPacket(deauth);
    }

    WifiPacket auth_request;
    auth_request.channel = network_channel_;
    auth_request.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
    auth_request.destination_address = server;
    auth_request.type = WifiPacket::Type::Authentication;
    SendPacket(auth_request);
}

void UdsEngine::SendAssociationResponseFrame(const Mac& address) {
    if (connection_status_.status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
        return;
    }
    WifiPacket assoc_response;
    assoc_response.channel = network_channel_;
    // Azahar hands out association id 1 to everybody; nothing uses it.
    assoc_response.data = GenerateAssocResponseFrame(0, 1, network_info_.network_id());
    assoc_response.destination_address = address;
    assoc_response.type = WifiPacket::Type::AssociationResponse;
    SendPacket(assoc_response);
}

void UdsEngine::HandleAuthenticationFrame(const WifiPacket& packet) {
    AuthSeq seq;
    if (!GetAuthenticationSeqNumber(packet.data, &seq)) {
        return;
    }
    // Only SEQ1 needs handling; SEQ2 has no side effects.
    if (seq != AuthSeq::Seq1) {
        return;
    }
    if (connection_status_.status != static_cast<u32>(NetworkStatus::ConnectedAsHost) ||
        AdmissionBlocked()) {
        return;
    }
    auto known = node_map_.find(packet.transmitter_address);
    if (known != node_map_.end() && known->second.connected) {
        // A client we admitted authenticates again: it left without a Deauthentication (a dropped
        // link, a restarted game). The new association replaces the old one, as a real host's station
        // list does; the node is gone for the game until the client's EAPoL-Start brings it back.
        Log("uds: node %u authenticates again: its old association is dropped", known->second.node_id);
        if (RemoveNodeLocked(known, kRemovedReauth)) {
            QueueSignalLocked(connection_status_event_);
        }
    }
    // (a half-open entry of this MAC is simply answered again)
    if (connection_status_.max_nodes == connection_status_.total_nodes) {
        return; // network full (Azahar sends no reject frame); the MAC replaced above no longer counts
    }

    WifiPacket auth_response;
    auth_response.channel = network_channel_;
    auth_response.data = GenerateAuthenticationFrame(AuthSeq::Seq2);
    auth_response.destination_address = packet.transmitter_address;
    auth_response.type = WifiPacket::Type::Authentication;
    Node& entry = node_map_[packet.transmitter_address];
    entry.connected = false;
    // A node that left a moment ago and is back: the second copy of its goodbye may still arrive (Node::deauth_guard).
    const u64 now = platform::NowMs();
    entry.deauth_guard = entry.deauth_guard || LeftRecentlyLocked(packet.transmitter_address, now);
    entry.auth_ms = now;
    SendPacket(auth_response);

    SendAssociationResponseFrame(packet.transmitter_address);
}

void UdsEngine::HandleDeauthenticationFrame(const WifiPacket& packet) {
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (status == NetworkStatus::ConnectedAsClient || status == NetworkStatus::ConnectedAsSpectator ||
        status == NetworkStatus::Connecting) {
        // A client leaves when its host sends the Deauthentication (the host ended the network or threw
        // it out); the chip would check the BSSID, so one from anybody else means nothing. The game
        // is told the way it is for a real console: reason 4 once connected, a failed Connect (0xC92113FB)
        // before that. A join whose host has not answered yet is not cut short: the burst of a network
        // that ended a moment ago may still be on its way through the room.
        if (packet.transmitter_address != network_info_.host_mac_address ||
            (status == NetworkStatus::Connecting && !associated_)) {
            return;
        }
        ++g_stats.deauth_rx[status == NetworkStatus::Connecting ? 2 : 1];
        Log("uds: the host sent a Deauthentication: %s", status == NetworkStatus::Connecting
                                                              ? "the join fails"
                                                              : "this node is out of the network");
        Teardown(status == NetworkStatus::Connecting ? NetworkStatusChangeReason::BeforeCommunication
                                                     : NetworkStatusChangeReason::DiscardedFromNetwork,
                 false);
        return;
    }
    if (status != NetworkStatus::ConnectedAsHost) {
        return;
    }
    auto it = node_map_.find(packet.transmitter_address);
    if (it == node_map_.end()) {
        return;
    }
    const u64 now = platform::NowMs();
    if (it->second.deauth_guard) {
        // This node left a moment ago and authenticated again: Azahar's guests send their goodbye twice, up to 2 s apart,
        // and the second copy must not end the association it has just made. One copy is swallowed, once.
        it->second.deauth_guard = false;
        if (now - it->second.auth_ms < kLateDeauthMs) {
            ++g_stats.deauth_late;
            Log("uds: Deauthentication from %02X:%02X:%02X:%02X:%02X:%02X %lu ms after it authenticated again: the late"
                " second copy of the goodbye it left with, ignored",
                packet.transmitter_address[0], packet.transmitter_address[1], packet.transmitter_address[2],
                packet.transmitter_address[3], packet.transmitter_address[4], packet.transmitter_address[5],
                static_cast<unsigned long>(now - it->second.auth_ms));
            return;
        }
    }
    ++g_stats.deauth_rx[0];
    NoteLeftLocked(packet.transmitter_address, now);
    const u16 left_node = it->second.node_id;
    if (RemoveNodeLocked(it, kRemovedDeauth)) {
        QueueSignalLocked(connection_status_event_);
        Log("uds: node %u sent a Deauthentication: removed", left_node);
        LogNodeMapLocked("a node left");
    }
}

void UdsEngine::HandleDataFrame(const WifiPacket& packet) {
    EtherType type;
    if (!GetFrameEtherType(packet.data, &type)) {
        return;
    }
    switch (type) {
    case EtherType::EAPoL:
        HandleEapolPacket(packet);
        break;
    case EtherType::SecureData:
        HandleSecureDataPacket(packet);
        break;
    }
}

void UdsEngine::OnWifiPacket(const WifiPacket& packet) {
    Guard lock(this);
    RoleTrace trace(this);
    // Beacons are heard in any state, before Initialize included: the first scan of a game finds a host
    // that was already beaconing (the chip scans the air, it does not wait for the service).
    if (!initialized_ && packet.type != WifiPacket::Type::Beacon) {
        return;
    }
    switch (packet.type) {
    case WifiPacket::Type::Beacon:
        HandleBeaconFrame(packet);
        break;
    case WifiPacket::Type::Authentication:
        HandleAuthenticationFrame(packet);
        break;
    case WifiPacket::Type::AssociationResponse:
        HandleAssociationResponseFrame(packet);
        break;
    case WifiPacket::Type::Data:
        HandleDataFrame(packet);
        break;
    case WifiPacket::Type::Deauthentication:
        HandleDeauthenticationFrame(packet);
        break;
    case WifiPacket::Type::NodeMap:
        HandleNodeMapPacket(packet);
        break;
    }
}

bool UdsEngine::OnTransportDown(NetworkStatusChangeReason reason) {
    Guard lock(this);
    RoleTrace trace(this);
    // (an engine that is not up has nothing to lose, a stopped service no session of its own)
    if (!initialized_ || service_state_ != kServiceRunning) {
        return false;
    }
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    // How long ago the host was last heard: what tells a silent host from a link that went with it.
    const u64 heard = host_beacon_ms_.load(std::memory_order_relaxed);
    const u32 heard_ago = heard != 0 ? static_cast<u32>(platform::NowMs() - heard) : 0;
    switch (status) {
    case NetworkStatus::ConnectedAsHost: {
        // The network does not end with the room: it keeps beaconing for whoever the room brings back. The
        // players it had are gone, as if each of them had left.
        bool changed = false;
        for (auto it = node_map_.begin(); it != node_map_.end();) {
            changed |= RemoveNodeLocked(it++, kRemovedLink);
        }
        Log("uds: the room link is gone (reason %u): hosting goes on, %s", static_cast<unsigned>(reason),
            changed ? "its players are gone" : "it had no players");
        if (changed) {
            ++g_stats.transport_down[2];
            QueueSignalLocked(connection_status_event_);
        }
        return changed;
    }
    case NetworkStatus::Connecting:
        // The join cannot go on: it fails the way one does that the host never finished (0xC92113FB).
        ++g_stats.transport_down[1];
        Log("uds: the room link is gone (reason %u): the join in progress fails", static_cast<unsigned>(reason));
        Teardown(NetworkStatusChangeReason::BeforeCommunication, false);
        return true;
    case NetworkStatus::ConnectedAsClient:
    case NetworkStatus::ConnectedAsSpectator:
        ++g_stats.transport_down[0];
        Log("uds: the room link is gone (reason %u): this node is out of the network (the host was heard %lu ms ago)",
            static_cast<unsigned>(reason), static_cast<unsigned long>(heard_ago));
        Teardown(reason, false); // (nobody would hear a goodbye)
        return true;
    default:
        return false;
    }
}

void UdsEngine::ResetRoomPresence() {
    Guard lock(this);
    present_.clear(); // (the first list after this one only says who is there: nobody was seen before it)
    absent_.clear();
    absent_count_.store(0, std::memory_order_relaxed);
}

void UdsEngine::OnRoomMembers(const std::vector<Mac>& members) {
    Guard lock(this);
    const auto listed = [&members](const Mac& mac) {
        return std::find(members.begin(), members.end(), mac) != members.end();
    };
    const u64 now = platform::NowMs();
    // The ones that are back are not gone ...
    for (auto it = absent_.begin(); it != absent_.end();) {
        it = listed(it->mac) ? absent_.erase(it) : it + 1;
    }
    // ... and a member of the lists before that is not in this one starts its debounce time.
    for (const Mac& mac : present_) {
        const bool already = std::any_of(absent_.begin(), absent_.end(), [&mac](const Absent& a) { return a.mac == mac; });
        if (!listed(mac) && !already) {
            absent_.push_back({mac, now});
        }
    }
    present_ = members;
    for (const Absent& a : absent_) {
        present_.push_back(a.mac);
    }
    absent_count_.store(static_cast<u32>(absent_.size()), std::memory_order_relaxed);
}

void UdsEngine::CheckPresenceLocked(u64 now_ms) {
    RoleTrace trace(this);
    for (auto it = absent_.begin(); it != absent_.end();) {
        if (now_ms < it->since_ms + kPresenceDebounceMs) {
            ++it;
            continue;
        }
        const Mac mac = it->mac;
        it = absent_.erase(it);
        present_.erase(std::remove(present_.begin(), present_.end(), mac), present_.end());
        MemberLeftLocked(mac);
    }
    absent_count_.store(static_cast<u32>(absent_.size()), std::memory_order_relaxed);
}

void UdsEngine::MemberLeftLocked(const Mac& mac) {
    if (!initialized_ || service_state_ != kServiceRunning || mac == GetMacAddress()) {
        return;
    }
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (status == NetworkStatus::ConnectedAsClient || status == NetworkStatus::ConnectedAsSpectator ||
        status == NetworkStatus::Connecting) {
        if (mac == network_info_.host_mac_address) {
            // Its host left the room: the network is gone with it. There is no goodbye from a member that
            // vanished, so the game hears what a console hears when the host's radio stops: the link was lost.
            ++g_stats.presence_host_gone;
            Log("uds: the host left the room: the network is gone");
            Teardown(status == NetworkStatus::Connecting ? NetworkStatusChangeReason::BeforeCommunication
                                                         : NetworkStatusChangeReason::ConnectionLost,
                     false);
            return;
        }
    } else if (status != NetworkStatus::ConnectedAsHost) {
        return;
    }
    auto it = node_map_.find(mac);
    if (it == node_map_.end()) {
        return; // not a node of this network (another game's player, or one that never joined)
    }
    Log("uds: node %u left the room: removed", it->second.node_id);
    if (RemoveNodeLocked(it, kRemovedPresence)) {
        QueueSignalLocked(connection_status_event_);
        LogNodeMapLocked("a node left the room");
    }
}

bool UdsEngine::TryDiscardEncoded(const u8* msg, std::size_t size) {
    // The message: [0] id, [1] type, [2] channel, [3..8] transmitter, [9..14] destination, [15..18] big
    // endian length of the 802.11 payload, then the payload = LLC (8) + SecureData header (14) + data.
    // Only what HandleSecureDataPacket drops BEFORE it looks at a bind is decided here, for a client or
    // spectator, in its order of checks (not connected, our own echo, not for us); a management frame,
    // anything while hosting (a host routes frames), a malformed one and everything that reaches the
    // game take the full path.
    constexpr std::size_t kPayloadAt = 19;
    constexpr std::size_t kNeeded = kLlcHeaderSize + kSecureDataHeaderSize;
    if (size < kPayloadAt + kNeeded || msg[1] != static_cast<u8>(WifiPacket::Type::Data)) {
        return false;
    }
    const std::size_t length = LoadBE32(msg + 15);
    if (length > size - kPayloadAt || length < kNeeded) {
        return false;
    }
    const u8* payload = msg + kPayloadAt;
    if (LoadBE16(payload + 6) != static_cast<u16>(EtherType::SecureData)) {
        return false;
    }
    const u8* h = payload + kLlcHeaderSize;
    if (h[6] != 0) {
        return false; // management bit
    }
    const u16 protocol_size = LoadBE16(h + 0);
    const u8 data_channel = h[7];
    const u16 dest_node = LoadBE16(h + 10);
    const u16 src_node = LoadBE16(h + 12);

    Guard lock(this);
    if (!initialized_) {
        return true; // OnWifiPacket() ignores everything but beacons while the engine is not initialised
    }
    const NetworkStatus status = static_cast<NetworkStatus>(connection_status_.status);
    if (status == NetworkStatus::ConnectedAsHost) {
        return false; // a host routes frames for other nodes: HandleSecureDataPacket decides
    }
    if (status != NetworkStatus::ConnectedAsClient && status != NetworkStatus::ConnectedAsSpectator) {
        ++g_stats.drop_not_connected;
        return true;
    }
    // (the same header-only records as HandleSecureDataPacket makes of what it drops: the prefilter must not change the capture)
    const std::size_t data_size = length - kNeeded;
    const u32 actual = protocol_size >= kSecureDataHeaderSize ? protocol_size - kSecureDataHeaderSize : 0;
    const u32 frame_size = static_cast<u32>(std::min<std::size_t>(actual, data_size));
    if (src_node == connection_status_.network_node_id) {
        ++g_stats.drop_echo;
        DebugFrame(kDirEcho, src_node, dest_node, data_channel, nullptr, frame_size);
        return true;
    }
    if (dest_node != connection_status_.network_node_id && dest_node != kBroadcastNetworkNodeId) {
        ++g_stats.drop_not_for_me;
        DebugFrame(kDirOther, src_node, dest_node, data_channel, nullptr, frame_size);
        return true;
    }
    return false;
}

void UdsEngine::SendBeaconLocked() {
    if (connection_status_.status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
        return;
    }
    WifiPacket packet;
    packet.type = WifiPacket::Type::Beacon;
    packet.data = GenerateBeaconFrame(network_info_, node_info_);
    packet.destination_address = kBroadcastMac;
    packet.channel = network_channel_;
    SendPacket(packet);
}

void UdsEngine::Tick(u64 now_ms) {
    // The network thread, every round. Without the lock: the bookkeeping of a client (the age of the host's beacon, from
    // the mirror of the role and the atomic clock of the beacon) and the question whether the lock is needed at all - a
    // beacon that is due, a member whose absence is being timed. Before, the lock was taken every 3 ms, and each time was a
    // chance for a game thread to find it taken.
    const u32 role = role_.load(std::memory_order_relaxed);
    if (role == static_cast<u32>(NetworkStatus::ConnectedAsClient) || role == static_cast<u32>(NetworkStatus::ConnectedAsSpectator)) {
        u64 heard = host_beacon_ms_.load(std::memory_order_relaxed);
        if (heard == 0) {
            host_beacon_ms_.compare_exchange_strong(heard, now_ms, std::memory_order_relaxed); // just joined: the clock starts now
            heard = now_ms;
        }
        // The longest silence of the host in this stats interval (a real client gives up on a
        // host that stays silent for the time the game set with command 0x18).
        const u32 age = now_ms > heard ? static_cast<u32>(now_ms - heard) : 0;
        if (age > g_stats.host_beacon_age_max) {
            g_stats.host_beacon_age_max = age;
        }
    }
    const bool beacon_due = beacon_active_.load(std::memory_order_relaxed) &&
                            static_cast<s32>(static_cast<u32>(now_ms) - beacon_due_ms_.load(std::memory_order_relaxed)) >= 0;
    const bool presence = absent_count_.load(std::memory_order_relaxed) != 0;
    if (!beacon_due && !presence) {
        return;
    }
    Guard lock(this);
    if (!absent_.empty()) {
        CheckPresenceLocked(now_ms);
    }
    const u64 now_tenths = now_ms * 10;
    if (!beacon_active_ || now_tenths < next_beacon_tenths_) {
        return;
    }
    if (connection_status_.status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
        beacon_active_ = false;
        return;
    }
    SendBeaconLocked();
    // An absolute schedule: the next one is due a period after this one was DUE, not after it was sent (the loop rounds
    // that wake us a few ms late would add up). A stall of more than two periods is not made up for: the schedule starts
    // again from now.
    next_beacon_tenths_ += kBeaconPeriodTenths;
    if (next_beacon_tenths_ + 2 * kBeaconPeriodTenths < now_tenths) {
        next_beacon_tenths_ = now_tenths + kBeaconPeriodTenths;
    }
    beacon_due_ms_.store(static_cast<u32>(next_beacon_tenths_ / 10), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

u32 UdsEngine::Initialize(const NodeInfo& node, u32 shared_memory_size, u16 version,
                          u32* game_status_event) {
    Guard lock(this);
    RoleTrace trace(this);
    *game_status_event = 0;
    if (version > kMaxVersion) {
        // nwm throws a fatal error here; a game cannot be answered with that, so it gets the newest
        Log("uds: Initialize asks for version 0x%x, nwm knows up to 0x%x", version, kMaxVersion);
        version = kMaxVersion;
    }
    version_ = version; // every call overwrites it, one that changes nothing too

    if (service_state_ != kServiceRunning) {
        version_ = 0;
        return service_state_ == kServiceWirelessOff ? res::kWirelessOff : res::kWrongState;
    }
    if (initialized_) {
        // The game's second session, or a repeated call: nothing is initialised again (a live host
        // or client stays what it is) and the same event comes back.
        *game_status_event = kernel_->DuplicateForGame(connection_status_event_);
        return res::kSuccess;
    }
    if (shared_memory_size & kPageMask) {
        version_ = 0;
        Log("uds: Initialize with a shared memory size of 0x%lx (not a multiple of 4 KiB)",
            static_cast<unsigned long>(shared_memory_size));
        return res::kOutOfMemory;
    }

    current_node_ = node;
    shared_memory_size_ = shared_memory_size;
    deauth_on_destroy_ = true;
    initialized_ = true;
    seed_said_ = false;

    // After initialisation the status block is all zeroes except for the status itself.
    std::memset(&connection_status_, 0, sizeof(connection_status_));
    SetStatusLocked(NetworkStatus::NotConnected);
    std::memset(&network_info_, 0, sizeof(network_info_));
    admission_attributes_ = 0;
    node_info_.clear();
    node_info_.push_back(current_node_);
    node_map_.clear();
    node_map_text_.clear();
    kernel_->ClearEvent(connection_status_event_); // the real service makes a new event

    *game_status_event = kernel_->DuplicateForGame(connection_status_event_);
    return res::kSuccess;
}

u32 UdsEngine::Shutdown() {
    Guard lock(this);
    RoleTrace trace(this);
    if (!initialized_) {
        return res::kSuccess; // never initialised, or already shut down
    }
    // Like the real nwm (and unlike Azahar) a session that is still up ends here: a host would
    // otherwise keep beaconing, and a client stay "connected", after the game quit.
    if (connection_status_.status != static_cast<u32>(NetworkStatus::NotConnected)) {
        Teardown(NetworkStatusChangeReason::RequestFromSystem, true);
    }
    FreeBinds(true); // every bind is freed: signalled, then closed

    std::memset(&connection_status_, 0, sizeof(connection_status_)); // status 0: not initialised
    service_state_ = kServiceRunning;
    initialized_ = false;
    return res::kSuccess;
}

// (the sleep observer calls this from the game's APT thread, inside the svc hook: its line is built by hand)
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
bool UdsEngine::StopService(int kind) {
    Guard lock(this);
    RoleTrace trace(this);
    if (!initialized_) {
        return false; // the service task exists from the first Initialize to Shutdown
    }
    const u8 state = kind == kServiceWirelessOff || kind == kServiceSleep ? static_cast<u8>(kind)
                                                                          : static_cast<u8>(kServiceShutdown);
    {
        LogLine line;
        line.Str("uds: the service stops (")
            .Str(state == kServiceWirelessOff ? "wireless off" : state == kServiceSleep ? "sleep" : "shutdown")
            .Str(") while ").Str(RoleName(connection_status_.status));
    }
    const bool live = connection_status_.status != static_cast<u32>(NetworkStatus::NotConnected);
    if (live) {
        Teardown(NetworkStatusChangeReason::RequestFromSystem, true); // (signals the status event: the tail)
    }
    FreeBinds(true);
    // The task tells the game when it exits: a live session has signalled the event once already, an idle
    // one never did.
    QueueSignalLocked(connection_status_event_);
    service_state_ = state;
    return live;
}
// <<< game-thread code

u32 UdsEngine::BeginHostingNetwork(const u8* network_info, std::size_t size) {
    Guard lock(this);
    RoleTrace trace(this);
    if (const u32 r = Gate(kIdle)) {
        return r;
    }

    // The two fields the checks need, read from the game's bytes (a short deprecated structure reads as zeros): this
    // runs on the game's thread, which has no room for a copy of the whole structure.
    const u8 max_nodes = size > offsetof(NetworkInfo, max_nodes) ? network_info[offsetof(NetworkInfo, max_nodes)] : 0;
    const u8 data_size = size > offsetof(NetworkInfo, application_data_size)
                             ? network_info[offsetof(NetworkInfo, application_data_size)]
                             : 0;
    // The real service throws a fatal error for these.
    if (max_nodes < 2 || max_nodes > kUdsMaxNodes) {
        Log("uds: BeginHostingNetwork with invalid max_nodes=%u", max_nodes);
        return res::kBadId;
    }
    if (data_size > kApplicationDataSize) {
        return res::kTooLarge;
    }
    std::memset(&network_info_, 0, sizeof(network_info_));
    std::memcpy(&network_info_, network_info, std::min(size, sizeof(network_info_)));
    node_map_.clear();
    node_map_text_.clear();
    // The attribute word the game gave is advertised in the beacon but the host does not enforce it
    // before the first UpdateNetworkAttribute (the real service starts with an empty copy).
    admission_attributes_ = 0;
    FlushRingsLocked();

    SetStatusLocked(NetworkStatus::ConnectedAsHost);
    connection_status_.status_change_reason =
        static_cast<u32>(NetworkStatusChangeReason::NetworkIsAvailable);

    std::memcpy(network_info_.oui_value, kNintendoOui, sizeof(kNintendoOui));
    network_info_.oui_type = 21; // NintendoTagId::NetworkInfo

    connection_status_.max_nodes = network_info_.max_nodes;

    node_info_.clear();
    node_info_.resize(network_info_.max_nodes);

    // The host is the only node for now, and always node 1.
    connection_status_.total_nodes = 1;
    network_info_.total_nodes = 1;
    connection_status_.network_node_id = 1;
    current_node_.network_node_id = 1;
    connection_status_.nodes[0] = connection_status_.network_node_id;
    connection_status_.node_bitmask |= 1;
    connection_status_.changed_nodes |= 1;

    network_info_.host_mac_address = GetMacAddress();
    node_info_[0] = current_node_;

    if (network_info_.channel != 0) {
        network_channel_ = network_info_.channel;
    } else {
        network_info_.channel = kDefaultNetworkChannel;
        network_channel_ = kDefaultNetworkChannel; // (not the channel of the network hosted or joined before)
    }

    QueueSignalLocked(connection_status_event_);

    // Start broadcasting the network, one beacon every 102.4 ms.
    next_beacon_tenths_ = platform::NowMs() * 10 + kBeaconPeriodTenths;
    beacon_due_ms_.store(static_cast<u32>(next_beacon_tenths_ / 10), std::memory_order_relaxed);
    beacon_active_ = true;
    for (LeftNode& left : left_) {
        left.ms = 0; // (a new network: nobody has just left it)
    }
    Log("uds: hosting network (max %u nodes, channel %u)", network_info_.max_nodes,
        network_channel_);
    return res::kSuccess;
}

u32 UdsEngine::EjectClient(u16 network_node_id) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    if (network_node_id == kHostDestNodeId) {
        return res::kBadId; // the host can't be kicked (the firmware checks this before the state)
    }
    if (const u32 r = Gate(kHost)) {
        return r;
    }

    WifiPacket deauth;
    deauth.channel = network_channel_;
    deauth.type = WifiPacket::Type::Deauthentication;
    bool changed = false;
    if (network_node_id == kBroadcastNetworkNodeId) {
        deauth.destination_address = kBroadcastMac;
        for (int i = 0; i < kDeauthBurst; ++i) {
            SendPacket(deauth);
        }
        // Azahar's clients ignore a Deauthentication from the host, so nobody would ever tell us
        // they left: the host drops them itself.
        for (auto it = node_map_.begin(); it != node_map_.end();) {
            changed |= RemoveNodeLocked(it++, kRemovedEject);
        }
    } else {
        for (auto it = node_map_.begin(); it != node_map_.end(); ++it) {
            if (it->second.connected && !it->second.spec && it->second.node_id == network_node_id) {
                deauth.destination_address = it->first;
                SendPacket(deauth);
                changed = RemoveNodeLocked(it, kRemovedEject);
                break;
            }
        }
        // (an unknown node id is not an error)
    }
    if (changed) {
        QueueSignalLocked(connection_status_event_);
    }
    return res::kSuccess;
}

u32 UdsEngine::EjectSpectators() {
    Guard lock(this);
    if (const u32 r = Gate(kHost)) {
        return r;
    }
    // The real service sends the spectators a management frame (the Azahar side cannot read it) and
    // closes the network to new spectators.
    SetAttributeLocked(kAttrBlockSpectators, true);
    return res::kSuccess;
}

void UdsEngine::SetAttributeLocked(u16 mask, bool set) {
    u16 attributes = LoadBE16(network_info_.attributes_be);
    attributes = set ? static_cast<u16>(attributes | mask) : static_cast<u16>(attributes & ~mask);
    if (!(mask & kAttrBlockAll)) {
        attributes &= static_cast<u16>(~kAttrBlockAll); // that bit survives only if the call names it
    }
    StoreBE16(network_info_.attributes_be, attributes); // the beacon carries it from the next one on
    admission_attributes_ = attributes;
}

u32 UdsEngine::UpdateNetworkAttribute(u16 mask, u8 flag) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    SetAttributeLocked(mask, flag != 0);
    return res::kSuccess;
}

u32 UdsEngine::DestroyNetwork() {
    Guard lock(this);
    RoleTrace trace(this);
    if (const u32 r = Gate(kHost)) {
        return r;
    }
    Teardown(NetworkStatusChangeReason::RequestFromMyself, true);
    return res::kSuccess;
}

u32 UdsEngine::ConnectToNetwork(const u8* network_info, std::size_t size, u8 connection_type) {
    u32 gen = 0;
    {
        Guard lock(this);
        RoleTrace trace(this);
        if (const u32 r = Gate(kIdle)) {
            return r; // (before the NetworkInfo of a host that is up is overwritten)
        }
        if (connection_type != static_cast<u8>(ConnectionType::Client) &&
            connection_type != static_cast<u8>(ConnectionType::Spectator)) {
            Log("uds: ConnectToNetwork with connection type %u", connection_type);
            return res::kBadArgument;
        }
        std::memset(&network_info_, 0, sizeof(network_info_));
        std::memcpy(&network_info_, network_info, std::min(size, sizeof(network_info_)));
        conn_type_ = static_cast<ConnectionType>(connection_type);
        if (network_info_.channel != 0) {
            network_channel_ = network_info_.channel;
        }

        kernel_->ClearEvent(connect_event_);
        connect_pending_ = true;
        gen = ++connect_gen_;
        StartConnectionSequenceLocked(network_info_.host_mac_address);
        Log("uds: connecting to host %02X:%02X:%02X:%02X:%02X:%02X", network_info_.host_mac_address[0],
            network_info_.host_mac_address[1], network_info_.host_mac_address[2],
            network_info_.host_mac_address[3], network_info_.host_mac_address[4],
            network_info_.host_mac_address[5]);
    }

    // Block like the real service does (outside the lock: the room thread needs it). Whoever ends
    // the attempt (the host's answer, a Disconnect, a Shutdown) leaves the result behind.
    kernel_->WaitEvent(connect_event_, kConnectTimeoutNs);
    Guard lock(this);
    RoleTrace trace(this);
    if (connect_gen_ != gen) {
        // The attempt this thread waited for was ended by another one (a Disconnect), and the game has begun a
        // new attempt on another thread before this one got to run: the new one is not ours to time out or
        // tear down, and what ours ended with is what was left for it.
        const u32 r = connect_result_gen_ == gen ? connect_result_ : res::kConnectionFailed;
        ++g_stats.connect_result[r == res::kSuccess ? 0 : 2];
        return r;
    }
    if (connect_pending_) {
        connect_pending_ = false; // nobody answered
        if (connection_status_.status == static_cast<u32>(NetworkStatus::Connecting)) {
            Teardown(NetworkStatusChangeReason::BeforeCommunication, true);
        }
        Log("uds: timed out while connecting to the host");
        ++g_stats.connect_result[1];
        return res::kConnectTimeout;
    }
    ++g_stats.connect_result[connect_result_ == res::kSuccess ? 0 : 2];
    return connect_result_;
}

u32 UdsEngine::DisconnectNetwork() {
    Guard lock(this);
    RoleTrace trace(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    switch (static_cast<NetworkStatus>(connection_status_.status)) {
    case NetworkStatus::ConnectedAsHost: {
        // A real 3DS does strange things here; so does Azahar, and so do we: the tables are
        // wiped, the host stays one that nobody can join, and the answer is "wrong state".
        const u32 reason = connection_status_.status_change_reason;
        const u16 node_id = connection_status_.network_node_id;
        std::memset(&connection_status_, 0, sizeof(connection_status_));
        SetStatusLocked(NetworkStatus::ConnectedAsHost);
        connection_status_.status_change_reason = reason;
        connection_status_.network_node_id = node_id;
        node_map_.clear();
        node_map_text_.clear();
        return res::kWrongState;
    }
    case NetworkStatus::Connecting:
    case NetworkStatus::ConnectedAsClient:
    case NetworkStatus::ConnectedAsSpectator:
        Teardown(NetworkStatusChangeReason::RequestFromMyself, true);
        break;
    default: // idle: libctru calls this after every failed connect, and nothing happens
        break;
    }
    return res::kSuccess;
}

ConnectionStatus UdsEngine::GetConnectionStatus() {
    Guard lock(this);
    ConnectionStatus out = connection_status_;
    if (service_state_ != kServiceRunning) {
        // A stopped service has no status of its own: this is what the game is told (and the
        // command still succeeds).
        std::memset(&out, 0, sizeof(out));
        out.status = static_cast<u32>(NetworkStatus::Unavailable);
        out.status_change_reason = static_cast<u32>(NetworkStatusChangeReason::RequestFromSystem);
    }
    // Reset the changed-nodes bitmask after each query so games are not told about the same
    // change twice.
    connection_status_.changed_nodes = 0;

    // Games poll this a lot; only log when what they would see actually changes.
    static u32 last_status = 0xFFFFFFFF, last_bitmask = 0xFFFFFFFF, last_total = 0xFFFFFFFF;
    if (out.status != last_status || out.node_bitmask != last_bitmask ||
        out.total_nodes != last_total) {
        last_status = out.status;
        last_bitmask = out.node_bitmask;
        last_total = out.total_nodes;
        Log("uds: connection status %lu, node %u, nodes %u/%u, bitmask 0x%04x, reason %lu",
            static_cast<unsigned long>(out.status), out.network_node_id, out.total_nodes,
            out.max_nodes, out.node_bitmask, static_cast<unsigned long>(out.status_change_reason));
    }
    return out;
}

u32 UdsEngine::GetPeerSignal(u16 table[kPeerSignalEntries]) {
    Guard lock(this);
    if (const u32 r = Gate(kHost | kConnecting | kClient | kSpectator)) {
        return r;
    }
    // The console measures the signal of every frame it receives; the tunnel has no radio, so every
    // node we know about reports a good, valid sample.
    constexpr u16 kGoodSignal = 40 | (1 << 8);
    std::memset(table, 0, kPeerSignalEntries * sizeof(u16));
    for (u32 slot = 0; slot < kUdsMaxNodes; ++slot) {
        if ((connection_status_.node_bitmask & (1u << slot)) && slot + 1 != connection_status_.network_node_id) {
            table[1 + slot] = kGoodSignal;
            table[0] = kGoodSignal;
        }
    }
    return res::kSuccess;
}

u32 UdsEngine::GetNodeInformation(u16 network_node_id, NodeInfo* out) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    if (network_node_id == 0) {
        // nwm keeps 16 slots and an empty one has id 0 (the output is zeroed, the first slot whose id equals the
        // request is copied, no special case for 0): id 0 is found, as zeros, in every state. Mario Party asks it about 57 times
        // a second, and the table here is only as long as the lobby (a full lobby would have answered "not found").
        out->Reset();
        return res::kSuccess;
    }
    for (const NodeInfo& node : node_info_) {
        if (node.network_node_id == network_node_id) {
            *out = node;
            return res::kSuccess;
        }
    }
    return res::kNodeNotFound;
}

u32 UdsEngine::RecvBeaconBroadcastData(u8* out, u32 out_size, const ScanParams& scan) {
    {
        Guard lock(this);
        if (const u32 r = Gate(kIdle)) {
            return r;
        }
    }
    return ScanBeacons(0x0F, out, out_size, scan);
}

u32 UdsEngine::ScanOnConnection(u8* out, u32 out_size, const ScanParams& scan) {
    {
        Guard lock(this);
        if (const u32 r = Gate(kClient | kSpectator)) {
            return r;
        }
    }
    return ScanBeacons(0x22, out, out_size, scan);
}

void UdsEngine::SetScanFilterEnforced(bool enforced) {
    Guard lock(this);
    scan_filter_enforced_ = enforced;
}

void UdsEngine::WaitForScan(const ScanParams& scan, u64 window_ms) {
    const u64 started = platform::NowMs();
    u64 asked = 0; // what was asked of the kernel: a test kernel that does not sleep still gets to the end
    for (;;) {
        const u64 waited = std::max<u64>(platform::NowMs() - started, asked);
        if (waited >= window_ms) {
            return;
        }
        // The cancel event is the game's own (nwm only holds a copy): it is looked at, never closed. A
        // handle that is 0, invalid or not signalled just lets the scan go on.
        if (scan.cancel != 0 && kernel_->WaitEvent(scan.cancel, 0)) {
            return;
        }
        const u64 slice = std::min<u64>(kScanSliceMs, window_ms - waited);
        kernel_->SleepNs(static_cast<s64>(slice) * 1000000);
        asked += slice;
    }
}

u32 UdsEngine::ScanBeacons(u32 command, u8* out, u32 out_size, const ScanParams& scan) {
    if (out_size < kBeaconDataReplyHeaderSize) {
        return res::kTooLarge;
    }
    // The chip listens to every channel of the mask for the dwell time before it reports. We do the
    // same, with mu_ free (the room thread keeps handing beacons in), and only then look at what was
    // heard: the beacon of a host is repeated every 102 ms, so one that was heard in the interval
    // before the scan started counts too. Nothing is consumed: the next scan hears the host again.
    const u64 started = platform::NowMs();
    const u64 window = std::min<u64>(
        static_cast<u64>(__builtin_popcount(scan.channel_mask & kScanChannelBits)) * scan.dwell_ms,
        kMaxScanWindowMs);
    WaitForScan(scan, window);

    const bool asks_id = scan.wlan_comm_id != 0xFFFFFFFF || scan.id8 != 0xFF;
    const bool odd = scan.type > 1 || scan.mode > 3 || scan.dwell_ms < 10 || scan.ssid_len > 32;
    u32 entries = 0, differ = 0;
    bool enforced = false;
    // The first three networks heard, for the log (a few bytes each: this runs on the game's thread).
    struct Heard {
        Mac mac;
        u32 wlan_comm_id;
        u8 id8;
        bool differs;
    };
    Heard heard[3];
    u32 heard_n = 0;
    {
        Guard lock(this);
        enforced = scan_filter_enforced_;
        const u64 now = platform::NowMs();
        received_beacons_.remove_if([now](const HeardBeacon& b) { return b.rx_ms + kBeaconKeepMs < now; });

        std::size_t cur = kBeaconDataReplyHeaderSize;
        for (const HeardBeacon& heard_beacon : received_beacons_) {
            const WifiPacket& beacon = heard_beacon.packet;
            if (heard_beacon.rx_ms + kBeaconIntervalMs < started) {
                continue; // not beaconing any more
            }
            if (scan.bssid != kBroadcastMac && beacon.transmitter_address != scan.bssid) {
                continue;
            }
            // The vendor tag of a beacon carries the wlancommid (BE32) and id8 of its game; a scan can
            // ask for one game's networks only. The words a game sends are the ones its hosts advertise
            // (the NSMB2 capture of 2026-10-20: asks 0x0007af10/0x00, the host's beacon says the same), so
            // the networks of another game are left out (scan_filter_enforced_: count-only until then).
            const BeaconTags tags = FindBeaconTags(beacon.data);
            const bool has_ids = tags.network && tags.network_size >= 9;
            const u32 wlan_comm_id = has_ids ? LoadBE32(tags.network + 4) : 0;
            const u8 id8 = has_ids ? tags.network[8] : 0;
            const bool differs = asks_id && (!has_ids || (scan.wlan_comm_id != 0xFFFFFFFF && wlan_comm_id != scan.wlan_comm_id) ||
                                             (scan.id8 != 0xFF && id8 != scan.id8));
            differ += differs;
            if (differs && enforced) {
                continue;
            }
            if (heard_n < 3) {
                heard[heard_n++] = {beacon.transmitter_address, wlan_comm_id, id8, differs};
            }
            const std::size_t entry_size = kBeaconEntryHeaderSize + beacon.data.size();
            if (cur + entry_size > out_size) {
                break;
            }
            u8* e = out + cur;
            std::memset(e, 0, kBeaconEntryHeaderSize);
            StoreLE32(e + 0x00, static_cast<u32>(entry_size)); // total_size
            e[0x05] = beacon.channel;                          // wifi_channel
            std::memcpy(e + 0x08, beacon.transmitter_address.data(), 6);
            StoreLE32(e + 0x14, static_cast<u32>(entry_size));             // unk_size
            StoreLE32(e + 0x18, static_cast<u32>(kBeaconEntryHeaderSize)); // header_size
            std::memcpy(e + kBeaconEntryHeaderSize, beacon.data.data(), beacon.data.size());
            cur += entry_size;
            ++entries;
        }
        StoreLE32(out + 0, out_size);              // max_output_size
        StoreLE32(out + 4, static_cast<u32>(cur)); // total_size
        StoreLE32(out + 8, entries);               // total_entries

        ++g_stats.scans;
        g_stats.scan_entries += entries;
        g_stats.scan_id_differs += differ;
        g_stats.scan_odd_input += odd;
    }

    // Every scan of the first ones, then the ones that look wrong: this is what the next hardware log
    // has to answer ("do the words of the game equal the NetworkInfo of the host it joins?").
    static u32 shown = 0, shown_odd = 0;
    if (g_debug_level > 0 && (shown < 24 || ((differ || odd) && shown_odd < 24))) {
        ++shown;
        shown_odd += (differ || odd) ? 1 : 0;
        Log("uds: scan 0x%02x: type %u mode %u channels 0x%04x x %u ms = %lu ms, bssid %02x:%02x:%02x:%02x:%02x:%02x,"
            " asks wlancommid 0x%08lx id8 0x%02x%s -> %u network(s), %u with other ids (%s)",
            static_cast<unsigned>(command), scan.type, scan.mode, scan.channel_mask, scan.dwell_ms,
            static_cast<unsigned long>(window), scan.bssid[0], scan.bssid[1], scan.bssid[2], scan.bssid[3],
            scan.bssid[4], scan.bssid[5], static_cast<unsigned long>(scan.wlan_comm_id), scan.id8,
            odd ? " (odd input)" : "", static_cast<unsigned>(entries), static_cast<unsigned>(differ),
            enforced ? "left out" : "only counted");
        for (u32 i = 0; i < heard_n; ++i) { // (transmitter, wlancommid/id8 of its network; ! = not the ones asked for)
            Log("uds: scan heard %02x:%02x:%02x:%02x:%02x:%02x %08lx/%02x%s", heard[i].mac[0], heard[i].mac[1],
                heard[i].mac[2], heard[i].mac[3], heard[i].mac[4], heard[i].mac[5],
                static_cast<unsigned long>(heard[i].wlan_comm_id), heard[i].id8, heard[i].differs ? "!" : "");
        }
    }
    return res::kSuccess;
}

u32 UdsEngine::SetApplicationData(const u8* data, u32 size) {
    Guard lock(this);
    if (const u32 r = Gate(kHost)) {
        return r;
    }
    if (size > kApplicationDataSize) {
        return res::kTooLarge;
    }
    network_info_.application_data_size = static_cast<u8>(size);
    std::memcpy(network_info_.application_data, data, size);
    return res::kSuccess;
}

u32 UdsEngine::GetApplicationData(u8* out, u32 capacity, u32* actual) {
    Guard lock(this);
    *actual = 0;
    if (const u32 r = Gate(kInSession)) {
        return r;
    }
    const u8* data = network_info_.application_data;
    u32 size = network_info_.application_data_size;
    const bool reads_host_beacon = connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsClient) ||
                                   connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsSpectator);
    if (reads_host_beacon) {
        // A client has no application data of its own: the firmware loads the host's from the host's current
        // beacon (3dbrew, NWMUDS:GetApplicationData). The NetworkInfo the game joined with is the 0x3C bytes of the
        // old Connect (Tekken) and carries none, so this beacon is the only place the host's data is. A game
        // that reads it right after joining (Tekken does, 100 ms after the join, and leaves when it gets 0 bytes)
        // finds the beacon it scanned: they are kept for as long as the host keeps sending them.
        bool from_beacon = false;
        for (const HeardBeacon& heard : received_beacons_) {
            if (heard.packet.transmitter_address != network_info_.host_mac_address) {
                continue;
            }
            const u8* beacon_data = nullptr;
            u32 beacon_size = 0;
            if (GetBeaconApplicationData(FindBeaconTags(heard.packet.data), &beacon_data, &beacon_size)) {
                data = beacon_data;
                size = beacon_size;
                from_beacon = true;
            }
            break;
        }
        static u32 shown = 0;
        if (!from_beacon && shown < 8) {
            ++shown; // (the answer is then the joined structure's: 0 bytes after the old Connect, which is what Tekken left the network for)
            Log("uds: GetApplicationData as a client: no readable beacon of the host, answering %u bytes from the joined structure",
                static_cast<unsigned>(size));
        }
    }
    if (capacity < size) {
        return res::kSuccess; // (size 0: the game's buffer is too small)
    }
    *actual = size;
    if (size) {
        std::memcpy(out, data, size);
    }
    return res::kSuccess;
}

u32 UdsEngine::SetMaxSendDelay(u64 delay_ns) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    if (delay_ns < 5000000 || delay_ns > 100000000) { // the radio flushes every 5 to 100 ms
        return res::kBadArgument;
    }
    return Gate(kIdle);
}

u32 UdsEngine::ApplyRadioProfile(u8 profile) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    if (profile >= 3) {
        return res::kBadArgument;
    }
    return Gate(kInSession);
}

u32 UdsEngine::SetBeaconMissTime(u16 time_tu) {
    Guard lock(this);
    if (const u32 r = Gate(kIdle, false)) {
        return r;
    }
    // 1024 to 51200 time units: the 1 to 50 s the chip's beacon-miss timer takes
    return static_cast<u32>(time_tu) - 1024 >= 0xC401 ? res::kBadArgument : res::kSuccess;
}

u32 UdsEngine::SetApParam(u32 value) {
    Guard lock(this);
    if (const u32 r = Gate(kIdle, false)) {
        return r;
    }
    return value >= 0x20000 ? res::kBadArgument : res::kSuccess;
}

u32 UdsEngine::SetProbeResponseParam() {
    Guard lock(this);
    return Gate(kIdle, false);
}

u32 UdsEngine::SetDeauthOnDestroy(bool enabled) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    deauth_on_destroy_ = enabled;
    return res::kSuccess;
}

u32 UdsEngine::Flush(u8 /*aggregator*/) {
    Guard lock(this);
    return Gate(kHost | kClient); // (frames are sent at once: there is nothing to flush)
}

u32 UdsEngine::Bind(u32 bind_node_id, u32 recv_buffer_size, u8 data_channel, u16 network_node_id,
                    u32* game_event) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    if (data_channel == 0 || bind_node_id == 0) {
        return res::kBadId;
    }
    // Everything else nwm refuses is "out of resource": a ring too small for the largest frame, no free entry
    // in the table of 16, no room left in the block the game gave Initialize (ids, channels and sources are
    // not checked for duplicates).
    if (recv_buffer_size < RxRing::kMinBytes) {
        return res::kOutOfMemory;
    }
    BindSlot* slot = nullptr;
    for (BindSlot& bind : binds_) {
        if (bind.id == 0) {
            slot = &bind;
            break;
        }
    }
    if (!slot) {
        Log("uds: Bind refused, all %lu bind nodes are in use", static_cast<unsigned long>(kMaxBinds));
        return res::kOutOfMemory;
    }
    const u64 cost = kBindHeapHeader + ((static_cast<u64>(recv_buffer_size) + 3) & ~3ull);
    if (shared_memory_size_ != 0 && bind_budget_ + cost > static_cast<u64>(shared_memory_size_) - kBindHeapHeader) {
        Log("uds: Bind refused: %lu bytes do not fit the %lu the game gave Initialize (%lu taken by other binds)",
            static_cast<unsigned long>(recv_buffer_size), static_cast<unsigned long>(shared_memory_size_),
            static_cast<unsigned long>(bind_budget_));
        return res::kOutOfMemory;
    }
    const u32 event = kernel_->CreateEvent();
    if (!event) {
        Log("uds: Bind failed, cannot create an event");
        return res::kOutOfMemory;
    }
    if (!slot->ring.Init(std::max(recv_buffer_size, kMinRingBytes))) {
        kernel_->CloseEvent(event);
        Log("uds: Bind failed, no memory for a ring of %lu bytes", static_cast<unsigned long>(recv_buffer_size));
        return res::kOutOfMemory;
    }
    slot->id = bind_node_id;
    slot->channel = data_channel;
    slot->src = network_node_id;
    slot->event = event;
    slot->budget = cost;
    bind_budget_ += cost;
    Log("uds: Bind node %lu channel %u from node 0x%x (recv buffer %lu, ring %lu bytes, %lu of the game's %lu used)",
        static_cast<unsigned long>(bind_node_id), data_channel, network_node_id,
        static_cast<unsigned long>(recv_buffer_size), static_cast<unsigned long>(slot->ring.total_units() * 32),
        static_cast<unsigned long>(bind_budget_), static_cast<unsigned long>(shared_memory_size_));
    *game_event = kernel_->DuplicateForGame(event);
    return res::kSuccess;
}

u32 UdsEngine::Unbind(u32 bind_node_id, UnbindInfo* info) {
    Guard lock(this);
    if (info) {
        *info = UnbindInfo();
    }
    if (!initialized_ || service_state_ != kServiceRunning) {
        return res::kSuccess; // no service task: nothing to unbind (the firmware leaves the reply alone)
    }
    if (bind_node_id == 0) {
        return res::kBadId;
    }
    BindSlot* bind = FindBindIdLocked(bind_node_id);
    if (!bind) {
        return res::kSuccess; // (an unknown id is no error)
    }
    Log("uds: Unbind node %lu (channel %u, %lu frames stored, %lu pushed out)",
        static_cast<unsigned long>(bind_node_id), bind->channel, static_cast<unsigned long>(bind->ring.enqueued()),
        static_cast<unsigned long>(bind->ring.overwritten()));
    if (info) {
        info->channel = bind->channel;
        info->src = bind->src;
        info->enqueued = bind->ring.enqueued();
        info->overwritten = bind->ring.overwritten();
    }
    DrainPendingSignalLocked(bind->event);
    kernel_->SignalEvent(bind->event);
    kernel_->CloseEvent(bind->event);
    bind_budget_ -= bind->budget;
    *bind = BindSlot();
    return res::kSuccess;
}

u32 UdsEngine::PullPacket(u32 bind_node_id, u32 max_out_buff_size, u8* out, u32 out_capacity, u32* data_size,
                          u16* src_node_id) {
    *data_size = 0;
    *src_node_id = 0;

    Guard lock(this);
    ++g_stats.pull_calls;
    if (const u32 r = Gate(kInSession)) {
        return r;
    }
    BindSlot* bind = FindBindIdLocked(bind_node_id);
    if (!bind) {
        return res::kBadId;
    }
    u32 n = 0;
    u32 queued_ms = 0;
    switch (bind->ring.Dequeue(max_out_buff_size, out, out_capacity, &n, src_node_id, &queued_ms)) {
    case RxRing::Pull::Empty:
        ++g_stats.pull_empty;
        return res::kSuccess;
    case RxRing::Pull::TooLarge:
        ++g_stats.pull_too_large;
        if (g_stats.pull_too_large <= 5) {
            Log("uds: PullPacket buffer too small (%lu bytes asked): the frame stays queued",
                static_cast<unsigned long>(max_out_buff_size));
        }
        return res::kTooLarge; // the frame stays queued
    case RxRing::Pull::Frame:
        break;
    }
    *data_size = n;
    if (queued_ms != 0 && g_debug_level > 0) {
        NotePullLatency(static_cast<u32>(platform::NowMs()) - queued_ms); // (from the ring's stamp: how long the game let it wait)
    }
    return res::kSuccess;
}

u32 UdsEngine::SendTo(u16 dest_node_id, u8 data_channel, u32 data_size, u8 flags, const u8* data,
                      std::size_t available) {
    // The lock covers the checks and the addresses only: the frame is built (an allocation) and handed to the room (its
    // own lock) after it, so that the network thread never finds the engine's lock taken for the length of a SendTo.
    Mac dest_address;
    u16 own_node_id = 0;
    u8 channel = 0;
    u16 sequence = 0;
    bool peer_numbers = false;
    {
        Guard lock(this);
        ++g_stats.sendto_calls;
        // The firmware's order: the service, the size, the state, the destination being ourselves.
        if (const u32 r = Gate(kAnyStatus)) {
            NoteSendError("service stopped", dest_node_id, data_channel, data_size, flags);
            return r;
        }
        if (data_size > kMaxSendSize) {
            NoteSendError("frame too large", dest_node_id, data_channel, data_size, flags);
            return res::kTooLarge;
        }
        if (const u32 r = Gate(kHost | kClient)) {
            NoteSendError("not connected", dest_node_id, data_channel, data_size, flags);
            return r;
        }
        if (dest_node_id == connection_status_.network_node_id) {
            NoteSendError("destination is this node", dest_node_id, data_channel, data_size, flags);
            return res::kNodeNotFound;
        }
        if (data_size > available) {
            data_size = static_cast<u32>(available);
        }

        // A node that is not in the network is not an error: the radio sends the frame and nobody takes it.
        if (!GetSendAddress(dest_node_id, flags, &dest_address)) {
            return res::kSuccess;
        }
        g_stats.sendto_bytes += data_size;
        own_node_id = connection_status_.network_node_id;
        channel = network_channel_;
        // The real nwm numbers its frames; Azahar sends 0 and reads nothing of it. The number is what lets the plugin on the
        // other console drop the copy a radio_like=2 sender adds (CopyDedupe); 0 is never used (it means "unnumbered").
        sequence = ++tx_seq_;
        if (sequence == 0) {
            sequence = tx_seq_ = 1;
        }
        // (a radio_like=2 sender doubles a unicast frame only for a node that numbers its own: one with this plugin, which
        //  drops the copy; an Azahar or an older plugin would hand both to its game, so it gets the frame once, reliable)
        peer_numbers = dest_node_id != kBroadcastNetworkNodeId && copy_dedupe_.Numbers(dest_node_id);
    }
    DebugFrame(kDirTx, own_node_id, dest_node_id, data_channel, data, data_size);

    WifiPacket packet;
    packet.destination_address = dest_address;
    packet.channel = channel;
    packet.data = GenerateDataPayload(data, data_size, data_channel, dest_node_id, own_node_id, sequence);
    packet.type = WifiPacket::Type::Data;
    packet.peer_numbers = peer_numbers;
    SendPacket(packet);
    return res::kSuccess;
}

u32 UdsEngine::GetChannel(u8* channel) {
    Guard lock(this);
    if (const u32 r = Gate(kAnyStatus)) {
        return r;
    }
    *channel = network_info_.channel; // the last network hosted or joined (0 before any)
    return res::kSuccess;
}

u32 UdsEngine::DecryptBeaconData(const NetworkInfo& net_info, const u8* tag0, std::size_t tag0_size,
                                 const u8* tag1, std::size_t tag1_size, std::vector<u8>* out) {
    // Each tag buffer starts with the OUI (3 bytes) and the OUI type (1 byte).
    constexpr u8 kEncryptedData0 = 24;
    if (tag0_size < 4 || std::memcmp(tag0, kNintendoOui, 3) != 0 || tag0[3] != kEncryptedData0) {
        return res::kInvalidBeacon;
    }
    std::vector<u8> beacon_data(tag0 + 4, tag0 + tag0_size);
    if (tag1_size > 4) {
        beacon_data.insert(beacon_data.end(), tag1 + 4, tag1 + tag1_size);
    }

    DecryptBeacon(net_info, beacon_data.data(), beacon_data.size());

    // (Azahar does not verify the MD5 in the beacon header either.)
    constexpr std::size_t kHeader = 0x12;
    constexpr std::size_t kNodeSize = 0x1E;
    const std::size_t num_nodes = std::min<std::size_t>(net_info.max_nodes, kUdsMaxNodes);

    out->assign(kNodeInfoSize * kUdsMaxNodes, 0);
    for (std::size_t i = 0; i < num_nodes; ++i) {
        const std::size_t at = kHeader + i * kNodeSize;
        if (at + kNodeSize > beacon_data.size()) {
            break;
        }
        const u8* info = beacon_data.data() + at;
        NodeInfo node;
        node.friend_code_seed = LoadBE64(info);
        for (int j = 0; j < 10; ++j) {
            node.username[j] = LoadBE16(info + 8 + j * 2);
        }
        node.network_node_id = LoadBE16(info + 0x1C);
        node.Store(out->data() + i * kNodeInfoSize);
    }
    return res::kSuccess;
}

NetworkStatus UdsEngine::status() const {
    Guard lock(this);
    return static_cast<NetworkStatus>(connection_status_.status);
}

bool UdsEngine::initialized() const {
    Guard lock(this);
    return initialized_;
}

} // namespace azl
