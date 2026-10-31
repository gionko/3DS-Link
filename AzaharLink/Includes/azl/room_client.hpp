// ENet client for an Azahar/Citra multiplayer room (the equivalent of Network::RoomMember).
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "azl/platform.hpp"
#include "azl/room_proto.hpp"
#include "azl/tx_queue.hpp"
#include "azl/uds_frames.hpp"

namespace azl {

/// The ENet flags a frame of the room goes out with: reliable (the handshake, unicast game data) or UNSEQUENCED (beacons, broadcast
/// game data: never held behind a reliable packet the room is waiting for).
u32 EnetPacketFlags(bool reliable);

class RoomClient {
public:
    enum class State : int { Idle = 0, Connecting, Joining, Joined };
    enum class Error : int {
        None = 0,
        CouldNotConnect,
        NameCollision,
        MacCollision,
        ConsoleIdCollision,
        WrongVersion,
        WrongPassword,
        RoomIsFull,
        HostBanned,
        HostKicked,
        LostConnection,
        RoomClosed,         ///< the room's host closed the room
        NetworkUnavailable, ///< the local socket could not even be created (WiFi down, soc busy)
        UnknownError,
    };

    struct Config {
        std::string host;
        u16 port = room::kDefaultPort;
        std::string nickname;
        std::string password;
        std::string console_id_hash;
        std::string game_name;
        u64 game_id = 0;
        Mac preferred_mac = kNoPreferredMac;
    };

    RoomClient();
    ~RoomClient();
    RoomClient(const RoomClient&) = delete;
    RoomClient& operator=(const RoomClient&) = delete;

    /// Starts connecting in a background thread. Any previous session is stopped first.
    bool Start(const Config& config);
    /// Leaves the room and waits for the thread - at most kStopJoinMs. A network thread that does not leave in that time
    /// (stuck in a service call, or not scheduled) is abandoned: it is told that it belongs to a past session, and
    /// whatever it does when it wakes up is void (it touches no state of the next one).
    static constexpr u32 kStopJoinMs = 2000;
    void Stop();
    /// Only asks the thread to finish (no waiting). `quick` skips the polite disconnect handshake.
    void RequestStop(bool quick);
    /// RequestStop(true) and wait at most `timeout_ms` for the thread. Returns false if it did not
    /// finish in time (the thread is then abandoned: do not tear down what it uses).
    bool StopWithin(u32 timeout_ms);

    /// How long the network thread pauses after a socket error: the n-th in a row (1 for the first) waits 2, 4, 8, 16, 32,
    /// 64 and then 128 ms. A link that is gone (the console woke up from sleep with no WiFi yet: ENETDOWN) makes every
    /// service call fail at once, and a fixed 2 ms made 680 failing calls in 1.8 s; with this it is about 19 in the same
    /// time, and a link that comes back is noticed within 128 ms.
    static constexpr u32 ServiceErrorPauseMs(u32 in_a_row) {
        return in_a_row == 0 ? 0u : in_a_row >= 7 ? 128u : (2u << (in_a_row - 1));
    }
    /// errno of the last socket call that failed in the network thread (0: none yet).
    int last_errno() const {
        return last_errno_.load();
    }

    State state() const {
        return static_cast<State>(state_.load());
    }
    Error error() const {
        return static_cast<Error>(error_.load());
    }
    /// Forgets the last error (only meaningful while Idle): a fatal error such as a wrong password
    /// would otherwise block every later attempt, even after the user fixed the cause.
    void ClearError() {
        if (state() == State::Idle) {
            error_.store(static_cast<int>(Error::None));
        }
    }
    bool joined() const {
        return state() == State::Joined;
    }
    /// MAC address assigned by the room (valid once joined).
    Mac mac() const;
    room::RoomInfo room_info() const;

    /// Queues a frame for the room. Thread safe; silently dropped unless joined. The queue is bounded
    /// (TxQueue): game data and beacons go oldest first when it is full or they are more than a second old.
    void SendWifiPacket(const WifiPacket& packet);
    /// What the queue and the ENet backlog hold right now (frames, bytes), for the tests.
    std::size_t queued_frames() const;
    std::size_t queued_bytes() const;

    /// "Radio-like delivery" (ini radio_like). 1: beacons and broadcast game data go out unreliably, like on the real radio,
    /// so one lost datagram on a weak WiFi link does not stall everything behind it until ENet retransmits (hundreds of
    /// ms); unicast frames and the handshake (auth, EAPoL, node map) stay reliable, as the hardware would retry them.
    /// 2: unicast game data too, each frame sent TWICE (the copy a round later; the plugin on the other console drops it by
    /// the frame's number) - but only to a node that has shown it numbers its frames, i.e. one with this plugin; to an
    /// Azahar or an older plugin, which would hand both copies to its game, the frame goes once and reliable, as at level 1
    /// - and the room's reliable stream handed over as it arrives (ENet's azlDeliverOutOfOrder, for game frames and beacons:
    /// a lost datagram no longer holds the ones behind it). 0: everything reliable. Can be changed live.
    void SetFastData(int level) {
        fast_level_.store(level < 0 ? 0 : level > 2 ? 2 : level);
    }
    void SetFastData(bool fast) {
        SetFastData(fast ? 1 : 0);
    }
    int fast_level() const {
        return fast_level_.load();
    }
    bool fast_data() const {
        return fast_level_.load() > 0;
    }

    /// What ENet knows about the link to the room server, refreshed about once a second.
    struct LinkStats {
        u32 rtt_ms = 0;
        u32 rtt_var_ms = 0;
        u32 loss_permille = 0; ///< recent packet loss, per thousand
        u32 lost_total = 0;    ///< reliable commands that timed out and were resent
        u32 throttle = 0;      ///< ENet's throttle, 32 = full speed
        u32 in_transit = 0;    ///< reliable bytes sent and not acknowledged yet
        u32 sent_total = 0;
    };
    LinkStats link_stats() const;

    /// The longest pause of the network thread of kStallMs or more that ended within the last `within_ms` (0:
    /// none). ENet and the room give up on a peer that says nothing for 5 to 30 s: a link that ends after such a
    /// pause may be our own doing (see LossGuard).
    static constexpr u32 kStallMs = 2000;
    u32 RecentStallMs(u64 now_ms, u32 within_ms) const;

    /// Called from the network thread for every frame relayed by the room.
    void SetWifiCallback(std::function<void(const WifiPacket&)> cb);
    /// Called from the network thread with the raw message of every relayed 802.11 frame BEFORE it is
    /// decoded; true means "handled, do not decode" (UdsEngine::TryDiscardEncoded). The frame is still
    /// counted in the statistics.
    void SetWifiPrefilter(std::function<bool(const u8*, std::size_t)> cb);
    /// Called from the network thread once per loop round (every few ms) while the thread is alive, with the
    /// clock (platform::NowMs scale) of that round: nobody needs to read it again.
    void SetTickCallback(std::function<void(u64 now_ms)> cb);
    /// Called from the network thread whenever state/error changes.
    void SetStateCallback(std::function<void(State, Error)> cb);
    /// Called from the network thread every time the room sends its information (members, games).
    void SetRoomInfoCallback(std::function<void(const room::RoomInfo&)> cb);

    /// Changes the game shown next to our name in the room (any time after Start()).
    void UpdateGameInfo(const std::string& name, u64 id);

    static const char* StateName(State s);
    static const char* ErrorName(Error e);

private:
    void Run(Config config, u32 generation);
    void SetState(State s);
    void Fail(Error e);
    /// False once the session this thread was started for is over (Start() began another, or Stop() gave up waiting for
    /// the thread): it must not touch the client's state any more.
    bool Live(u32 generation) const {
        return generation_.load() == generation;
    }
    /// The loop was away for `ms` (ended `now_ms`): remembered if it was long enough to matter.
    void NoteLoopPause(u64 ms, u64 now_ms);

    std::atomic<int> state_{static_cast<int>(State::Idle)};
    std::atomic<int> error_{static_cast<int>(Error::None)};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> fast_level_{0};
    std::atomic<u32> tx_pending_{0};     ///< 1 while something was pushed into tx_ and not taken: the loop looks at the queue only then
    std::atomic<u32> last_enqueue_ms_{0}; ///< when the game last queued a frame (ms, truncated): the loop polls faster for a while after it
    std::atomic<bool> quick_stop_{false};
    std::atomic<u32> generation_{0}; ///< bumped by every Start() and by a Stop() that abandons the thread
    std::atomic<int> last_errno_{0};
    u64 start_request_ms_ = 0;

    mutable platform::Mutex mutex_; // guards everything below
    Mac mac_{};
    room::RoomInfo room_info_;
    LinkStats link_;
    u32 stall_ms_ = 0;     ///< the longest pause of the loop of 2 s or more seen lately ...
    u64 stall_end_ms_ = 0; ///< ... and when it ended
    TxQueue tx_;
    std::function<void(const WifiPacket&)> wifi_cb_;
    std::function<bool(const u8*, std::size_t)> wifi_prefilter_;
    std::function<void(u64)> tick_cb_;
    std::atomic<u32> cb_generation_{0}; // bumped by every setter above: the network loop copies the callbacks again
    std::function<void(State, Error)> state_cb_;
    std::function<void(const room::RoomInfo&)> room_info_cb_;

    platform::Thread thread_;
};

/// How long the network thread may sit in the socket this round (ms). 0 when a frame of the game is already queued (it goes
/// out this round, not after a wait); kQuickPollMs for kQuickPollWindowMs after the game queued one (the frames of a burst
/// wait a millisecond each instead of up to three: a poll is one IPC to the soc process, so this costs up to a few hundred
/// IPCs a second while a game sends); kServiceTimeoutMs otherwise. The clocks are truncated milliseconds of one scale.
constexpr u32 kServiceTimeoutMs = 3;
constexpr u32 kQuickPollMs = 1;
constexpr u32 kQuickPollWindowMs = 10;
inline u32 ServiceTimeoutFor(bool queued, u32 now_ms, u32 last_enqueue_ms) {
    if (queued) {
        return 0;
    }
    return now_ms - last_enqueue_ms < kQuickPollWindowMs ? kQuickPollMs : kServiceTimeoutMs;
}

/// The room link's probe, the answer to a silence the link has already left behind. ENet doubles the timer of a packet that gets no
/// answer, and gives the peer up at the first check that finds the oldest such packet 5 s old with six tries behind it: a WiFi that is
/// away for 3 to 4 s and comes back is declared lost anyway when the next try - seconds apart by then - falls past those 5 s (checked before
/// the try, which the link would have answered). Any answer to a packet of ours restarts that count, so when the room speaks (a datagram
/// came) while the oldest packet of ours it has not answered has waited kSilentPeerMs (`earliest_timeout`: ENet's, 0 = none; `service_time`:
/// ENet's clock, both in its 32-bit ms), a ping gets that answer at once. At most one in kProbeGapMs (`now_ms`, `last_probe_ms`: one clock).
/// There is no upper bound on the age: the model (Tests/vsim) keeps the room through silences of 5.5 s in a game that sends a frame in
/// every round, and a bound at 4.5 s loses them.
inline constexpr u32 kSilentPeerMs = 500;
inline constexpr u32 kProbeGapMs = 250;
inline bool ShouldProbeRoom(u32 service_time, u32 earliest_timeout, u64 now_ms, u64 last_probe_ms) {
    return earliest_timeout != 0 && service_time - earliest_timeout >= kSilentPeerMs && now_ms - last_probe_ms >= kProbeGapMs;
}

/// How a frame of the room goes out at a radio-like level (SetFastData): `reliable` = ENet retries it, `duplicate` = it is
/// sent a second time a round later. The handshake (anything but a beacon or game data) is always reliable and never
/// doubled; at level 0 so is everything. A unicast game frame is doubled only at level 2 and only when its destination
/// numbers its frames (`peer_numbers`: it runs this plugin and drops the copy); otherwise it stays reliable.
struct FrameDelivery {
    bool reliable = true;
    bool duplicate = false;
};
/// Does the frame go out the way a broadcast does (unreliable, never doubled)? One sent to the broadcast MAC (not a host's relayed copy, a
/// unicast frame of the radio), and one the game sent with the broadcast flag, which the radio sends once and does not retry whatever MAC
/// it is addressed to (UdsEngine::GetSendAddress puts the destination node's own MAC on it when the room has that node).
inline bool IsBroadcastDelivery(const WifiPacket& p) {
    return (p.destination_address == kBroadcastMac && !p.relayed) || p.broadcast_flag;
}

inline FrameDelivery DeliveryFor(int level, bool beacon, bool game_data, bool broadcast, bool peer_numbers) {
    FrameDelivery d;
    if (level <= 0 || (!beacon && !game_data)) {
        return d;
    }
    if (beacon || broadcast) {
        d.reliable = false;
    } else if (level >= 2 && peer_numbers) {
        d.reliable = false;
        d.duplicate = true;
    }
    return d;
}

} // namespace azl
