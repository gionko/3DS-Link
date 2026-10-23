#include "azl/room_client.hpp"

#include <enet/enet.h>

#include <cerrno>

#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "azl/ticks.h"
#include "azl/uds_dedupe.hpp"

namespace azl {

namespace {

constexpr u32 kConnectionTimeoutMs = 5000;
// (How long the loop waits for the socket before looking at the outgoing queue again is ServiceTimeoutFor in
// room_client.hpp: 3 ms at rest, 1 ms for 10 ms after the game queued a frame, none when one is waiting.)
constexpr u32 kLinkStatsIntervalMs = 1000;
// How long a quick stop waits for the room to acknowledge the last frames the game queued (a Deauthentication) before it leaves.
constexpr u32 kGoodbyeWaitMs = 120;
// ENet's reliable backlog (bytes sent and not acknowledged plus bytes waiting for the window) beyond
// which game data stops being queued as reliable: the window is 64 KB, more could only be stale.
constexpr u32 kMaxReliableBacklog = 65536;
// ENet's defaults for what one packet / the unprocessed data of a peer may be are 32 MB each, and a
// failed enet_malloc aborts (callbacks.c). The room sends UDS frames (~1.5 KB at most) and room
// information (a few KB): 64 KB per packet is generous, and 192 KB of waiting data is 1.8 x what a
// full soc ring (256 datagrams of ~416 B) can deliver in one go.
constexpr std::size_t kMaxPacketSize = 64 * 1024;
constexpr std::size_t kMaxWaitingData = 192 * 1024;

u32 Ms(u64 v) {
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(v);
}

/// Reliable bytes of the peer that are not acknowledged yet: in flight plus waiting for the window.
u32 ReliableBacklog(const ENetPeer* peer) {
    u32 bytes = peer->reliableDataInTransit;
    for (ENetListIterator it = enet_list_begin(&peer->outgoingSendReliableCommands);
         it != enet_list_end(&peer->outgoingSendReliableCommands); it = enet_list_next(it)) {
        bytes += reinterpret_cast<const ENetOutgoingCommand*>(it)->fragmentLength;
    }
    return bytes;
}

/// Debug capture of the 802.11 frames that are not game data (handshake, node maps, beacons...).
/// Game data frames are traced by the engine with their payload; beacons (about 10 a second per
/// host, ~400 bytes) are kept whole, so that a capture can be replayed.
/// (Called from SendWifiPacket, i.e. from the thread that sends the frame: the game's own when a session ends with the
/// game's thread inside the svc hook - no printf here, see DebugWifiLine.)
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
void DebugWifi(const char* dir, const WifiPacket& p) {
    if (g_debug_level <= 0) {
        return;
    }
    if (p.type == WifiPacket::Type::Beacon) {
        DebugBeacon(dir[0] == 't' ? kDirTx : kDirRx, p.transmitter_address.data(), p.channel,
                    p.data.data(), static_cast<u32>(p.data.size()));
        return;
    }
    if (p.type == WifiPacket::Type::Data) {
        EtherType ether;
        if (GetFrameEtherType(p.data, &ether) && ether == EtherType::SecureData) {
            return;
        }
    }
    DebugWifiLine(dir, static_cast<u32>(p.type), static_cast<u32>(p.data.size()), p.channel, p.transmitter_address.data(),
                  p.destination_address.data());
}
// <<< game-thread code

} // namespace

RoomClient::LinkStats RoomClient::link_stats() const {
    platform::LockGuard lock(mutex_);
    return link_;
}

u32 RoomClient::RecentStallMs(u64 now_ms, u32 within_ms) const {
    platform::LockGuard lock(mutex_);
    return stall_ms_ != 0 && now_ms - stall_end_ms_ <= within_ms ? stall_ms_ : 0;
}

u32 EnetPacketFlags(bool reliable) {
    // The handshake and unicast game data are reliable (the radio retries unicast frames). Beacons and broadcast game data are
    // not retried by the radio either, and the room must never hold them behind a reliable packet it is still waiting for
    // (ENet delivers a plain unreliable packet only after the reliable ones before it): UNSEQUENCED goes through at once.
    // A frame larger than ENet's fragment length (a UDS payload above ~1320 bytes) would otherwise go as RELIABLE fragments:
    // UNRELIABLE_FRAGMENT keeps it what it is.
    return reliable ? static_cast<u32>(ENET_PACKET_FLAG_RELIABLE)
                    : static_cast<u32>(ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

// The room's messages ENet may hand over ahead of a hole (azlDeliverOutOfOrder, radio_like=2): a relayed 802.11 frame of game
// data (SecureData: a radio frame may come late) or a beacon. The handshake frames (Authentication, EAPoL, Deauthentication,
// node map) and the room's own messages keep ENet's order: the engine relies on it (a Deauthentication before an
// Authentication, the Logoff tables one after the other).
int EarlyFilter(const enet_uint8* data, size_t length) {
    constexpr size_t kHead = 1 + 1 + 1 + 6 + 6; // id, type, channel, two MACs (room::EncodeWifiPacket), then a 32-bit length
    if (length < kHead + 4 + kLlcHeaderSize || data[0] != room::IdWifiPacket) {
        return 0;
    }
    const u8 type = data[1];
    if (type == static_cast<u8>(WifiPacket::Type::Beacon)) {
        return 1;
    }
    if (type != static_cast<u8>(WifiPacket::Type::Data)) {
        return 0;
    }
    const enet_uint8* llc = data + kHead + 4;
    const u16 ether = static_cast<u16>((static_cast<u16>(llc[6]) << 8) | llc[7]);
    return ether == static_cast<u16>(EtherType::SecureData) ? 1 : 0;
}

void RoomClient::NoteLoopPause(u64 ms, u64 now_ms) {
    if (ms < kStallMs) {
        return;
    }
    {
        platform::LockGuard lock(mutex_);
        stall_ms_ = Ms(ms); // (the newest one is the one a loss is put down to)
        stall_end_ms_ = now_ms;
    }
    ++g_stats.net_stalls;
    static u32 logged = 0;
    if (g_debug_level > 0 && ++logged <= 12) { // (the lines are the log's: with it off they do not use up the twelve)
        Log("net: the network thread was away for %lu ms (the room may give up on a peer that is silent that long)",
            static_cast<unsigned long>(ms));
    }
}

RoomClient::RoomClient() = default;

RoomClient::~RoomClient() {
    Stop();
}

const char* RoomClient::StateName(State s) {
    switch (s) {
    case State::Idle:
        return "Idle";
    case State::Connecting:
        return "Connecting";
    case State::Joining:
        return "Joining";
    case State::Joined:
        return "Joined";
    }
    return "?";
}

const char* RoomClient::ErrorName(Error e) {
    switch (e) {
    case Error::None:
        return "None";
    case Error::CouldNotConnect:
        return "Could not connect to the room";
    case Error::NameCollision:
        return "Nickname already in use";
    case Error::MacCollision:
        return "MAC address already in use";
    case Error::ConsoleIdCollision:
        return "This console is already in the room";
    case Error::WrongVersion:
        return "Room version mismatch";
    case Error::WrongPassword:
        return "Wrong password";
    case Error::RoomIsFull:
        return "Room is full";
    case Error::HostBanned:
        return "Banned from the room";
    case Error::HostKicked:
        return "Kicked from the room";
    case Error::LostConnection:
        return "Connection lost";
    case Error::RoomClosed:
        return "Room closed by its host";
    case Error::NetworkUnavailable:
        return "Network unavailable (WiFi down?)";
    case Error::UnknownError:
        return "Unknown error";
    }
    return "?";
}

Mac RoomClient::mac() const {
    platform::LockGuard lock(mutex_);
    return mac_;
}

room::RoomInfo RoomClient::room_info() const {
    platform::LockGuard lock(mutex_);
    return room_info_;
}

void RoomClient::SetWifiCallback(std::function<void(const WifiPacket&)> cb) {
    platform::LockGuard lock(mutex_);
    wifi_cb_ = std::move(cb);
    cb_generation_.fetch_add(1);
}

void RoomClient::SetWifiPrefilter(std::function<bool(const u8*, std::size_t)> cb) {
    platform::LockGuard lock(mutex_);
    wifi_prefilter_ = std::move(cb);
    cb_generation_.fetch_add(1);
}

void RoomClient::SetTickCallback(std::function<void(u64)> cb) {
    platform::LockGuard lock(mutex_);
    tick_cb_ = std::move(cb);
    cb_generation_.fetch_add(1);
}

void RoomClient::SetStateCallback(std::function<void(State, Error)> cb) {
    platform::LockGuard lock(mutex_);
    state_cb_ = std::move(cb);
}

void RoomClient::SetRoomInfoCallback(std::function<void(const room::RoomInfo&)> cb) {
    platform::LockGuard lock(mutex_);
    room_info_cb_ = std::move(cb);
}

void RoomClient::UpdateGameInfo(const std::string& name, u64 id) {
    if (state() == State::Idle) {
        return;
    }
    TxQueue::Item item;
    item.bytes = room::EncodeSetGameInfo(name, id);
    const u64 now = platform::NowMs();
    item.queued_ms = now;
    TxQueue::Drops drops;
    platform::LockGuard lock(mutex_);
    tx_.Push(std::move(item), now, &drops); // reliable and ordered: it follows the join request
    tx_pending_.store(1, std::memory_order_release);
    StatsAdd(g_stats.tx_cap_drops, drops.cap);
}

void RoomClient::SetState(State s) {
    state_.store(static_cast<int>(s));
    std::function<void(State, Error)> cb;
    {
        platform::LockGuard lock(mutex_);
        cb = state_cb_;
    }
    if (cb) {
        cb(s, error());
    }
}

void RoomClient::Fail(Error e) {
    error_.store(static_cast<int>(e));
    SetState(State::Idle);
}

bool RoomClient::Start(const Config& config) {
    Stop();
    // The session's number is taken before the flags are cleared: a thread that Stop() gave up on and that wakes up now
    // sees another number, whatever it reads in stop_requested_.
    const u32 generation = generation_.fetch_add(1) + 1;
    stop_requested_.store(false);
    quick_stop_.store(false);
    error_.store(static_cast<int>(Error::None));
    last_errno_.store(0); // (this attempt's: an errno of an earlier one says nothing about it)
    {
        platform::LockGuard lock(mutex_);
        tx_.Clear();
        mac_ = Mac{};
        room_info_ = room::RoomInfo{};
        stall_ms_ = 0; // (a pause of the thread before this attempt explains nothing that happens in it)
    }
    SetState(State::Connecting);
    start_request_ms_ = platform::NowMs();
    if (!thread_.Start([this, config, generation]() { Run(config, generation); })) {
        Log("room: could not start the network thread");
        Fail(Error::UnknownError);
        return false;
    }
    return true;
}

void RoomClient::Stop() {
    stop_requested_.store(true);
    // Bounded: a network thread that is stuck in a service call (or starved) must not hang whoever stops the client - the
    // menu, the next attempt, the end of the game. It is left to finish on its own, its session void.
    if (!thread_.JoinTimeout(kStopJoinMs)) {
        generation_.fetch_add(1);
        Log("room: the network thread did not stop within %lu ms: abandoned (it leaves on its own when it wakes up)",
            static_cast<unsigned long>(kStopJoinMs));
    }
    if (state() != State::Idle) {
        SetState(State::Idle);
    }
}

void RoomClient::RequestStop(bool quick) {
    quick_stop_.store(quick);
    stop_requested_.store(true);
}

bool RoomClient::StopWithin(u32 timeout_ms) {
    RequestStop(true);
    if (!thread_.JoinTimeout(timeout_ms)) {
        return false;
    }
    if (state() != State::Idle) {
        SetState(State::Idle);
    }
    return true;
}

void RoomClient::SendWifiPacket(const WifiPacket& packet) {
    if (state() != State::Joined) {
        return;
    }
    EtherType ether{};
    const bool secure_data = packet.type == WifiPacket::Type::Data && GetFrameEtherType(packet.data, &ether) &&
                             ether == EtherType::SecureData;
    // Like the real radio: broadcast frames are fire-and-forget, unicast frames are acknowledged and retried by the hardware
    // (so they stay reliable here at level 1; at level 2 they go twice instead: DeliveryFor).
    // (a host's relayed copy goes to the broadcast MAC of the room but is a unicast frame of the radio: retried, or doubled)
    const FrameDelivery delivery = DeliveryFor(fast_level_.load(), packet.type == WifiPacket::Type::Beacon, secure_data,
                                               packet.destination_address == kBroadcastMac && !packet.relayed, packet.peer_numbers);
    const u32 type_index = static_cast<u32>(packet.type);
    DebugWifi("tx", packet);
    TxQueue::Item item;
    item.bytes = room::EncodeWifiPacket(packet);
    const u64 now = platform::NowMs();
    item.queued_ms = now;
    item.reliable = delivery.reliable;
    item.duplicate = delivery.duplicate;
    // Game data and beacons may be lost on a stalled link like on the radio, the handshake may not.
    item.expendable = secure_data || packet.type == WifiPacket::Type::Beacon;
    const u32 size = static_cast<u32>(item.bytes.size());
    TxQueue::Drops drops;
    platform::LockGuard lock(mutex_);
    if (tx_.Push(std::move(item), now, &drops)) {
        tx_pending_.store(1, std::memory_order_release);
        last_enqueue_ms_.store(static_cast<u32>(now), std::memory_order_relaxed);
        if (type_index < 6) {
            ++g_stats.tx_frames[type_index];
        }
        g_stats.tx_bytes += size;
        if (tx_.size() > g_stats.queue_max) {
            g_stats.queue_max = static_cast<u32>(tx_.size());
        }
        if (tx_.bytes() > g_stats.tx_queue_bytes_max) {
            g_stats.tx_queue_bytes_max = static_cast<u32>(tx_.bytes());
        }
    }
    // (the network thread counts the drops of its own Take() in the same two counters, outside this lock)
    StatsAdd(g_stats.tx_stale_drops, drops.stale);
    StatsAdd(g_stats.tx_cap_drops, drops.cap);
}

std::size_t RoomClient::queued_frames() const {
    platform::LockGuard lock(mutex_);
    return tx_.size();
}

std::size_t RoomClient::queued_bytes() const {
    platform::LockGuard lock(mutex_);
    return tx_.bytes();
}

void RoomClient::Run(Config config, u32 generation) {
    // Where and when the thread really started: a thread that is created on a core the game has no
    // CPU time on can sit unscheduled for a long time.
    Log("net: thread running on core %d, %lu ms after the request", platform::CurrentCore(),
        static_cast<unsigned long>(platform::NowMs() - start_request_ms_));
    // This is the thread whose waits (soc calls, the engine's lock, its pauses) are taken off its CPU time below.
    const platform::NetThreadMark net_thread;
    // A thread that Stop() gave up on (it was stuck) belongs to a session that is over: it says nothing about the client's
    // state any more, whatever it finds when it wakes up.
    const auto fail = [this, generation](Error e) {
        if (Live(generation)) {
            Fail(e);
        }
    };
    const auto set_state = [this, generation](State s) {
        if (Live(generation)) {
            SetState(s);
        }
    };
    const auto stopping = [this, generation]() { return stop_requested_.load() || !Live(generation); };

    if (enet_initialize() != 0) {
        Log("net: enet_initialize failed");
        fail(Error::UnknownError);
        return;
    }

    ENetHost* client = enet_host_create(nullptr, 1, room::kNumChannels, 0, 0);
    if (!client) {
        // Almost always the socket(): no WiFi, or the soc service refusing. Retryable.
        const int err = errno;
        last_errno_.store(err);
        Log("net: enet_host_create failed (no socket), errno %d", err);
        fail(Error::NetworkUnavailable);
        enet_deinitialize();
        return;
    }
    client->maximumPacketSize = kMaxPacketSize;
    client->maximumWaitingData = kMaxWaitingData;

    ENetAddress address{};
    const u64 resolve_started_ms = platform::NowMs();
    if (enet_address_set_host(&address, config.host.c_str()) != 0) {
        const int err = errno;
        last_errno_.store(err);
        Log("net: could not resolve '%s', errno %d, after %lu ms", config.host.c_str(), err,
            static_cast<unsigned long>(platform::NowMs() - resolve_started_ms));
        fail(Error::CouldNotConnect);
        enet_host_destroy(client);
        enet_deinitialize();
        return;
    }
    {
        // What the name came to and how long the resolver took (a dotted quad needs none: 0 ms): a slow or wrong answer of
        // the DNS is the first thing to rule out when a join takes seconds.
        const u8* ip = reinterpret_cast<const u8*>(&address.host);
        Log("net: '%s' resolves to %u.%u.%u.%u in %lu ms", config.host.c_str(), ip[0], ip[1], ip[2], ip[3],
            static_cast<unsigned long>(platform::NowMs() - resolve_started_ms));
    }
    address.port = config.port;

    ENetPeer* server = enet_host_connect(client, &address, room::kNumChannels, 0);
    if (!server) {
        const int err = errno;
        last_errno_.store(err);
        Log("net: enet_host_connect failed, errno %d", err);
        fail(Error::UnknownError);
        enet_host_destroy(client);
        enet_deinitialize();
        return;
    }
    // Wait for the ENet-level connection (in slices so that Stop() stays responsive). A socket that fails at once does
    // not turn this into a spin: the pauses of the main loop apply (ServiceErrorPauseMs), and the errno is kept.
    bool connected = false;
    u32 connect_errors = 0;
    int connect_errno = 0;
    for (const u64 until = platform::NowMs() + kConnectionTimeoutMs; platform::NowMs() < until && !stopping();) {
        ENetEvent event;
        const int served = enet_host_service(client, &event, 100);
        if (served > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
            break;
        }
        if (served < 0) {
            connect_errno = errno;
            platform::SleepMs(ServiceErrorPauseMs(++connect_errors));
        } else {
            connect_errors = 0;
        }
    }
    if (!connected) {
        enet_peer_reset(server);
        if (!stopping()) {
            if (connect_errno != 0) {
                last_errno_.store(connect_errno);
            }
            Log("net: the room did not answer within %lu ms (last socket error while waiting: errno %d)",
                static_cast<unsigned long>(kConnectionTimeoutMs), connect_errno);
            fail(Error::CouldNotConnect);
        }
        enet_host_destroy(client);
        enet_deinitialize();
        return;
    }
    // ENet's packet throttle is congestion control for bulk transfers: with its defaults every round-trip sample above the
    // estimate (a WiFi hiccup, a pause of this thread) lowers it by 2/32, and at 30/32 it DESTROYS 1 unreliable packet in 16 at
    // the sender, at 20/32 more than a third: beacons and, with radio_like, every broadcast game frame (Super Street Fighter 4
    // lost 6 % of its frames this way, 20-41 % in the bad windows, 2026-10-19). Game data at 20-90 KB/s is not a bulk
    // transfer: the throttle never decelerates (a sample above the estimate changes nothing), so no frame is destroyed here.
    // Only once the connection is up: the room refuses this command from a peer that is not connected yet, and a refused
    // command ends the handling of the datagram it came in (2026-10-20: with the command queued before the connect, the room
    // never answered the connect).
    enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);


    set_state(State::Joining);
    {
        TxQueue::Item game_info;
        game_info.bytes = room::EncodeSetGameInfo(config.game_name, config.game_id);
        game_info.queued_ms = platform::NowMs();
        TxQueue::Item join;
        join.bytes = room::EncodeJoinRequest(config.nickname, config.console_id_hash, config.preferred_mac,
                                             config.password, "");
        join.queued_ms = game_info.queued_ms;
        platform::LockGuard lock(mutex_);
        tx_.PushFront(std::move(game_info));
        tx_.PushFront(std::move(join));
        tx_pending_.store(1, std::memory_order_release);
    }

    bool keep_running = true;
    u64 last_iteration_ms = platform::NowMs();
    u64 last_link_stats_ms = 0;
    // The callbacks and the batch of outgoing frames are copied/created once, not once per round or per
    // frame (a std::function copy under the lock and a std::deque, two heap blocks, per round were
    // ~1.5k instructions of every idle round). A setter bumps cb_generation_ and the loop copies again.
    std::function<void(const WifiPacket&)> wifi_cb;
    std::function<bool(const u8*, std::size_t)> wifi_prefilter;
    std::function<void(u64)> tick;
    u32 cb_generation = ~0u;
    std::deque<TxQueue::Item> to_send;
    std::deque<std::vector<u8>> repeat; // the copies of the last round's doubled frames (radio_like=2): sent first thing next round
    constexpr std::size_t kRepeatMax = 64;
    static_assert(kRepeatMax * 2 <= CopyDedupe::kSeen, "the receiver remembers a whole batch of copies and what came between");
    client->azlEarlyFilter = EarlyFilter;
    // Clock reads are system ticks (one SVC), converted without a division; each round's end is the
    // next round's start, and a round that did nothing reads the clock only twice.
    u64 round_start = platform::NowTicks();
    bool pause_noted = false; // the pause of this round is already known (it was in the service call)
    u32 errors_in_a_row = 0;     // socket errors of enet_host_service() since the last call that worked ...
    u64 error_episode_ms = 0;    // ... and when the first of them was
    int error_episode_errno = 0;
    bool error_episode_logged = false;
    // The time sums of the statistics (azl/stats.hpp) are CPU: the wall time of each section of a round less the waits that fell
    // in it (the soc calls, the engine's lock, the pause after a failing call), which the soc layer, the lock and this loop
    // note as they happen (platform::NoteNetWait). `waited` is the running sum at the previous cut.
    u64 waited = platform::NetWaitTicks();
    const auto cut = [&waited](Ticks64& sum, u64 wall) {
        const u64 now_waited = platform::NetWaitTicks();
        const u64 inside = now_waited - waited;
        waited = now_waited;
        StatsAdd64(sum, wall > inside ? wall - inside : 0);
    };
    while (keep_running && !stopping()) {
        if (cb_generation != cb_generation_.load()) {
            platform::LockGuard lock(mutex_);
            cb_generation = cb_generation_.load();
            wifi_cb = wifi_cb_;
            wifi_prefilter = wifi_prefilter_;
            tick = tick_cb_;
        }
        const u64 loop_start_ms = azl_ticks_to_ms(round_start);
        const u64 gap = loop_start_ms - last_iteration_ms;
        last_iteration_ms = loop_start_ms;
        ++g_stats.loop_iters;
        if (gap > g_stats.loop_gap_max) {
            g_stats.loop_gap_max = Ms(gap);
        }
        if (!pause_noted) {
            NoteLoopPause(gap, loop_start_ms); // (the whole previous round: a pause anywhere in it)
        }
        pause_noted = false;

        ENetEvent event;
        client->azlDeliverOutOfOrder = fast_level_.load() >= 2 ? 1u : 0u; // (the room's reliable stream handed over as it arrives)
        const u32 timeout_ms = ServiceTimeoutFor(tx_pending_.load(std::memory_order_acquire) != 0 || !repeat.empty(),
                                                 static_cast<u32>(loop_start_ms), last_enqueue_ms_.load(std::memory_order_relaxed));
        if (timeout_ms == 0) {
            StatsAdd(g_stats.polls_now);
        } else if (timeout_ms == kQuickPollMs) {
            StatsAdd(g_stats.polls_quick);
        }
        const int service_result = enet_host_service(client, &event, timeout_ms);
        const int service_errno = service_result < 0 ? errno : 0; // (before anything else can touch it)
        const u64 service_end = platform::NowTicks();
        {
            // ENet's own counters are cumulative; the statistics want this window's share.
            static u32 throttled_seen = 0, skipped_seen = 0, early_seen = 0;
            const u32 throttled = enet_azl_throttled_packets, skipped = enet_azl_ack_samples_skipped, early = enet_azl_early_dispatched;
            StatsAdd(g_stats.tx_throttled, throttled - throttled_seen);
            StatsAdd(g_stats.ack_samples_skipped, skipped - skipped_seen);
            StatsAdd(g_stats.rx_early, early - early_seen);
            throttled_seen = throttled;
            skipped_seen = skipped;
            early_seen = early;
        }
        cut(g_stats.t_net_service, service_end - round_start);
        if (!Live(generation)) {
            // The session this thread belongs to was given up on while it sat in the call: nothing it holds is anybody's
            // any more. (A datagram it read is lost with it.)
            if (service_result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
            keep_running = false;
            break;
        }
        // A pause before or inside the call (the thread not scheduled, a stuck soc call) is what makes ENet
        // give up on the room in this very call: it has to be known before the event below is acted on.
        if (service_end - round_start >= static_cast<u64>(kStallMs) * AZL_TICKS_PER_MS) {
            NoteLoopPause(azl_ticks_to_ms(service_end - round_start), azl_ticks_to_ms(service_end));
            pause_noted = true;
        }
        const u32 drained = StatsServiceCall(static_cast<u32>(service_end - round_start));
        u64 pause_to_note = 0; // the pause after a failing call: noted after the events section, because it lies in the send one
        if (service_result < 0) {
            ++g_stats.service_errors;
            last_errno_.store(service_errno);
            if (errors_in_a_row++ == 0) {
                error_episode_ms = azl_ticks_to_ms(service_end);
                error_episode_errno = service_errno;
                static u32 episodes = 0; // (a link that flaps: the first two dozen episodes, then one in 25)
                error_episode_logged = ++episodes <= 24 || (episodes % 25) == 0;
                if (error_episode_logged) {
                    Log("net: socket error in enet_host_service, errno %d: the calls are spaced out until it works again",
                        service_errno);
                }
            }
            // A failing socket must not turn this loop into a busy spin (the console woke up from sleep with no WiFi:
            // 680 failing calls in 1.8 s with a fixed 2 ms): the pause grows with every error in a row.
            const u64 pause_began = platform::NowTicks();
            platform::SleepMs(ServiceErrorPauseMs(errors_in_a_row));
            pause_to_note = platform::NowTicks() - pause_began;
            StatsAdd64(g_stats.t_net_pause, pause_to_note);
        } else if (errors_in_a_row != 0) {
            if (error_episode_logged) {
                Log("net: the socket works again after %lu failed calls in %lu ms (first errno %d)",
                    static_cast<unsigned long>(errors_in_a_row),
                    static_cast<unsigned long>(azl_ticks_to_ms(service_end) - error_episode_ms), error_episode_errno);
            }
            errors_in_a_row = 0;
        }
        if (service_result > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE: {
                const u8* data = event.packet->data;
                const std::size_t size = event.packet->dataLength;
                if (size > 0) {
                    switch (data[0]) {
                    case room::IdWifiPacket: {
                        if (wifi_prefilter && size > 1 && wifi_prefilter(data, size)) {
                            // dropped without decoding (the engine counted why): the statistics still
                            // count the frame
                            if (data[1] < 6) {
                                ++g_stats.rx_frames[data[1]];
                            }
                            g_stats.rx_bytes += static_cast<u32>(size);
                            break;
                        }
                        WifiPacket packet;
                        if (room::DecodeWifiPacket(data, size, &packet)) {
                            const u32 type_index = static_cast<u32>(packet.type);
                            DebugWifi("rx", packet);
                            if (type_index < 6) {
                                ++g_stats.rx_frames[type_index];
                            }
                            g_stats.rx_bytes += static_cast<u32>(size);
                            if (wifi_cb) {
                                wifi_cb(packet);
                            }
                        }
                        break;
                    }
                    case room::IdRoomInformation: {
                        room::RoomInfo info;
                        if (room::DecodeRoomInformation(data, size, &info)) {
                            std::function<void(const room::RoomInfo&)> cb;
                            {
                                platform::LockGuard lock(mutex_);
                                room_info_ = info;
                                cb = room_info_cb_;
                            }
                            if (cb) {
                                cb(info);
                            }
                        }
                        break;
                    }
                    case room::IdJoinSuccess:
                    case room::IdJoinSuccessAsMod: {
                        Mac assigned{};
                        if (room::DecodeJoinSuccess(data, size, &assigned)) {
                            {
                                platform::LockGuard lock(mutex_);
                                mac_ = assigned;
                            }
                            set_state(State::Joined);
                        }
                        break;
                    }
                    case room::IdRoomIsFull:
                        fail(Error::RoomIsFull);
                        keep_running = false;
                        break;
                    case room::IdNameCollision:
                        fail(Error::NameCollision);
                        keep_running = false;
                        break;
                    case room::IdMacCollision:
                        fail(Error::MacCollision);
                        keep_running = false;
                        break;
                    case room::IdConsoleIdCollision:
                        fail(Error::ConsoleIdCollision);
                        keep_running = false;
                        break;
                    case room::IdVersionMismatch:
                        fail(Error::WrongVersion);
                        keep_running = false;
                        break;
                    case room::IdWrongPassword:
                        fail(Error::WrongPassword);
                        keep_running = false;
                        break;
                    case room::IdCloseRoom:
                        fail(Error::RoomClosed);
                        keep_running = false;
                        break;
                    case room::IdHostKicked:
                        fail(Error::HostKicked);
                        keep_running = false;
                        break;
                    case room::IdHostBanned:
                        fail(Error::HostBanned);
                        keep_running = false;
                        break;
                    default:
                        break; // chat, status messages, moderation: not needed here
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                fail(Error::LostConnection);
                keep_running = false;
                break;
            default:
                break;
            }
        }

        // The callbacks above (the game's frame handler, the room list, a Log() that waits for the SD card) are where a thread
        // gets stuck: if Stop() gave up on this one meanwhile it must not go on to the queue of the session that replaced it, and
        // send that session's frames through its own dead peer.
        if (!keep_running || !Live(generation)) {
            break;
        }

        // A round with a frame to handle reads the clock a third time; one without reuses the reading
        // that ended the service call.
        const u64 events_end = service_result > 0 ? platform::NowTicks() : service_end;
        cut(g_stats.t_net_events, events_end - service_end);
        if (pause_to_note != 0) {
            platform::NoteNetWait(pause_to_note); // (it is in the wall time of the send section below, which it comes off)
        }
        const u64 now_ms = azl_ticks_to_ms(events_end);
        if (tx_pending_.load(std::memory_order_acquire) != 0) { // (the game's thread sets it under the lock: nothing pushed, no lock)
            platform::LockGuard lock(mutex_);
            tx_pending_.store(0, std::memory_order_relaxed);
            if (!tx_.empty()) {
                TxQueue::Drops drops;
                tx_.Take(&to_send, now_ms, &drops);
                StatsAdd(g_stats.tx_stale_drops, drops.stale); // waited more than a second: the newest state wins
            }
        }
        // The copies of the doubled frames of the LAST round go first (a round later than the originals: a datagram lost on
        // the way loses one copy, the other is a few ms behind it; the receiver drops whichever comes second by its number).
        bool sent_any = false;
        for (const std::vector<u8>& bytes : repeat) {
            if (ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), EnetPacketFlags(false))) {
                enet_peer_send(server, 0, packet);
                ++g_stats.tx_duplicates;
                sent_any = true;
            }
        }
        repeat.clear();
        // Game data stops going into ENet as reliable while its backlog is over the window: it would
        // only arrive stale. The handshake always goes.
        u32 backlog = 0;
        bool backlog_known = false;
        for (const TxQueue::Item& item : to_send) {
            if (item.reliable) {
                if (!backlog_known) {
                    backlog = ReliableBacklog(server);
                    backlog_known = true;
                }
                if (item.expendable && backlog >= kMaxReliableBacklog) {
                    StatsAdd(g_stats.tx_cap_drops); // (outside the lock: the game thread adds to it under the lock)
                    continue;
                }
                backlog += static_cast<u32>(item.bytes.size());
            }
            const u32 flags = EnetPacketFlags(item.reliable);
            ENetPacket* packet = enet_packet_create(item.bytes.data(), item.bytes.size(), flags);
            if (packet) {
                enet_peer_send(server, 0, packet);
                ++(item.reliable ? g_stats.tx_reliable : g_stats.tx_unreliable);
                sent_any = true;
                if (item.duplicate) {
                    if (repeat.size() >= kRepeatMax) {
                        repeat.pop_front(); // (a stalled link: the oldest copy is the least useful)
                        StatsAdd(g_stats.tx_cap_drops);
                    }
                    repeat.push_back(item.bytes);
                }
            }
            const u32 waited = now_ms > item.queued_ms ? Ms(now_ms - item.queued_ms) : 0;
            g_stats.delay_sum += waited;
            ++g_stats.delay_n;
            if (waited > g_stats.delay_max) {
                g_stats.delay_max = waited;
            }
        }
        if (sent_any) {
            if (errors_in_a_row == 0) {
                enet_host_flush(client); // (the service call sends what is ENet's own: acknowledgements, pings)
            } // else: the socket is failing, a flush would only fail too; the next call that works sends it
        }
        to_send.clear();

        if (tick) {
            tick(now_ms); // (the clock of this round: the engine has no need to read it again)
        }
        if (!Live(generation)) {
            break; // (the tick call is where this thread is held in the tests; on the console, a long pause anywhere)
        }

        const u64 done_ticks = platform::NowTicks();
        const u64 done_ms = azl_ticks_to_ms(done_ticks);
        cut(g_stats.t_net_send, done_ticks - events_end);
        StatsAdd64(g_stats.t_net_wall, done_ticks - round_start);
        const u32 round_ticks = static_cast<u32>(done_ticks - round_start);
        if (round_ticks > kSlowTicks) {
            // A round this long stalls the whole data path (frames in both directions wait): say where
            // the time went. Inside enet_host_service = the soc service / the WiFi, in "frames+engine" =
            // the engine's lock (a game thread holds it), in neither = the thread was not scheduled.
            ++g_stats.net_slow_rounds;
            static u32 slow_logged = 0;
            if (g_debug_level > 0 && (++slow_logged <= 12 || (slow_logged % 25) == 0)) {
                Log("net: slow round %lu ms: enet_host_service %lu ms (%lu datagrams read), frames+engine %lu ms,"
                    " queue+tick %lu ms",
                    static_cast<unsigned long>(round_ticks / AZL_TICKS_PER_MS),
                    static_cast<unsigned long>((service_end - round_start) / AZL_TICKS_PER_MS),
                    static_cast<unsigned long>(drained),
                    static_cast<unsigned long>((events_end - service_end) / AZL_TICKS_PER_MS),
                    static_cast<unsigned long>((done_ticks - events_end) / AZL_TICKS_PER_MS));
            }
        }
        round_start = done_ticks;
        if (done_ms - loop_start_ms > g_stats.service_max) {
            g_stats.service_max = Ms(done_ms - loop_start_ms);
        }
        if (done_ms - last_link_stats_ms >= kLinkStatsIntervalMs) {
            last_link_stats_ms = done_ms;
            LinkStats link;
            link.rtt_ms = server->roundTripTime;
            link.rtt_var_ms = server->roundTripTimeVariance;
            link.loss_permille = server->packetLoss * 1000u / ENET_PEER_PACKET_LOSS_SCALE;
            link.lost_total = server->packetsLost;
            link.throttle = server->packetThrottle;
            link.in_transit = server->reliableDataInTransit;
            link.sent_total = server->packetsSent;
            platform::LockGuard lock(mutex_);
            link_ = link;
        }
    }

    // (A thread whose session was given up on says no goodbye: it would be saying it for a connection that is not the
    // client's any more, and the next session may already be using the flags and the queue.)
    const bool live = Live(generation);
    // When the process is going away: one DISCONNECT datagram, no waiting. Without it the room keeps
    // this console as a ghost member until ENet's timeout and a quick relaunch is told "name /
    // MAC / console already in the room".
    if (live && server->state == ENET_PEER_STATE_CONNECTED && quick_stop_.load()) {
        // What the game queued just before (the Deauthentications of a Shutdown, of a sleep) goes first: the
        // disconnect throws away whatever ENet still holds.
        {
            platform::LockGuard lock(mutex_);
            if (!tx_.empty()) {
                TxQueue::Drops drops;
                tx_.Take(&to_send, platform::NowMs(), &drops);
                StatsAdd(g_stats.tx_stale_drops, drops.stale);
            }
        }
        for (const TxQueue::Item& item : to_send) {
            if (ENetPacket* packet = enet_packet_create(item.bytes.data(), item.bytes.size(), EnetPacketFlags(item.reliable))) {
                enet_peer_send(server, 0, packet);
            }
        }
        if (!to_send.empty()) {
            to_send.clear();
            enet_host_flush(client);
            // The room throws away what it has received and not handled yet when the DISCONNECT arrives: the
            // goodbye has to be acknowledged first (a moment, not more).
            // (a deadline, not a count of waits: datagrams that keep arriving must not stretch it)
            for (const u64 until = platform::NowMs() + kGoodbyeWaitMs;
                 platform::NowMs() < until && ReliableBacklog(server) > 0;) {
                ENetEvent event;
                const int served = enet_host_service(client, &event, 5);
                if (served < 0) {
                    break;
                }
                if (served > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                }
            }
        }
        enet_peer_disconnect_now(server, 0);
        enet_host_flush(client);
    }
    // Graceful disconnect (best effort, bounded; skipped when the process is going away).
    if (live && server->state == ENET_PEER_STATE_CONNECTED && !quick_stop_.load()) {
        enet_peer_disconnect(server, 0);
        bool done = false;
        for (const u64 until = platform::NowMs() + 500; !done && platform::NowMs() < until;) {
            ENetEvent event;
            const int served = enet_host_service(client, &event, 50);
            if (served < 0) {
                break;
            }
            if (served > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    done = true;
                }
            }
        }
    }
    enet_peer_reset(server);
    enet_host_destroy(client);
    enet_deinitialize();

    if (Live(generation) && state() != State::Idle) {
        SetState(State::Idle);
    }
}

} // namespace azl
