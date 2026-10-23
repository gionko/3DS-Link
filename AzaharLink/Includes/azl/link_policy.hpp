// The decisions between the room link and the game's UDS session. They are kept out of the plugin (and free of
// the 3DS) so that the tests run exactly the code the plugin runs:
//   LossGuard    - when a lost room link ends the game's session, and when it only waits for the room.
//   RejoinPolicy - which attempt to join the room comes next, and what a failed one means.
//   Announcer    - the few messages that go on screen: at most one every 5 s.
//   SleepWatch   - where the console is in a sleep, from the game's APT notifications and the framework's events.
#pragma once

#include <atomic>
#include <string>

#include "azl/platform.hpp"
#include "azl/room_client.hpp"

namespace azl {

/// A lost room link ends the game's UDS session (the game then takes its own way back to the lobby). Unless the
/// plugin itself is the one that went silent: ENet and the room give up on a peer that says nothing for 5 to 30 s,
/// and the network thread can be silent that long without the link being at fault (the framework's menu open, a
/// loading screen starving the core, ...). A loss that follows such a pause waits for the room to take the same
/// member back before it is declared. Thread safe.
class LossGuard {
public:
    /// RoomClient notes pauses of its thread of at least RoomClient::kStallMs (2 s), and a loss is put down to a
    /// pause that ended no longer ago than this (ENet gives up after 30 s at the most).
    static constexpr u32 kLookBackMs = 35000;
    /// A pause longer than this is a loss all the same: the other players gave up on us long ago.
    static constexpr u32 kLongStallMs = 10000;
    /// The room has to take the member back within this, counted from the moment the loss is noticed.
    static constexpr u32 kRejoinMs = 15000;

    enum class Verdict { Drop, Wait };
    /// The link left Joined. `error` is what ended it (None: the plugin stopped it itself, e.g. the user switched
    /// "Connect to room" off); `recent_stall_ms` the longest pause of the network thread within kLookBackMs (0:
    /// none). The member's identity is what a later join has to match.
    Verdict OnLeft(RoomClient::Error error, u32 recent_stall_ms, const Mac& mac, const std::string& nickname,
                   u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        waiting_ = false;
        const bool room_ended_it = error == RoomClient::Error::RoomClosed || error == RoomClient::Error::HostKicked ||
                                   error == RoomClient::Error::HostBanned || error == RoomClient::Error::None;
        if (room_ended_it || recent_stall_ms < RoomClient::kStallMs || recent_stall_ms > kLongStallMs) {
            return Verdict::Drop;
        }
        waiting_ = true;
        deadline_ms_ = now_ms + kRejoinMs;
        mac_ = mac;
        nickname_ = nickname;
        return Verdict::Wait;
    }

    enum class Outcome { Nothing, Kept, Drop };
    /// The room was joined. Nothing: no loss was waiting. Kept: the same member is back in time, the session goes
    /// on. Drop: it is another member (or too late), the session must end now.
    Outcome OnJoined(const Mac& mac, const std::string& nickname, u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        if (!waiting_) {
            return Outcome::Nothing;
        }
        waiting_ = false;
        return mac == mac_ && nickname == nickname_ && now_ms <= deadline_ms_ ? Outcome::Kept : Outcome::Drop;
    }

    /// True once when a wait ran out: the caller ends the session.
    bool Due(u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        if (waiting_ && now_ms > deadline_ms_) {
            waiting_ = false;
            return true;
        }
        return false;
    }

    /// Ends a wait now (the plugin gave up joining, or the user switched the connection off). True if one was on.
    bool Settle() {
        platform::LockGuard<platform::Mutex> lock(m_);
        const bool was = waiting_;
        waiting_ = false;
        return was;
    }

    bool waiting() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return waiting_;
    }

private:
    mutable platform::Mutex m_;
    bool waiting_ = false;
    u64 deadline_ms_ = 0;
    Mac mac_{};
    std::string nickname_;
};

/// When to join the room (again), and what a failed attempt means. The room lists a member for 6 to 30 s after its
/// link died without a goodbye, so the first attempts after a loss may be told "name / MAC / console already in
/// the room": the member is ourselves, and the answer is to wait (35 s from the loss), not to change who we are.
///
/// The schedule is fixed: an attempt that fails is followed by the next one 2 s after it began, always. What is
/// decided before that is whether the console has a WiFi to try with: the system keeps the state of its connection in
/// the shared page, and while that says "not connected" no attempt is made at all (each would only create a socket that
/// fails with ENETDOWN); the state is read every 500 ms, and the moment it says connected again the next attempt is at
/// once. Thread safe (the loss is noted by the network thread, the rest runs once per frame).
class RejoinPolicy {
public:
    static constexpr u32 kRetryMs = 2000;       ///< a failed attempt is followed by the next one this long after it began
    static constexpr u32 kLaunchGraceMs = 45000; ///< the room may still list this console from the game before
    static constexpr u32 kOldLinkMs = 35000;     ///< ... and our own link of before, for ENet's 30 s plus a margin
    static constexpr u32 kWifiPollMs = 500;      ///< how often the console's WiFi state is looked at
    /// The "network state" byte of the shared page (0x1FF81067, libctru's osSharedConfig_s::network_state, which has no
    /// accessor and no list of values). 3dbrew (Configuration Memory) calls it the network state the HOME menu shows: 2
    /// Internet, 3 / 4 / 6 Local, 7 Disabled, any other value just "enabled". What hardware showed (PROVA 1, 2): 2 while
    /// the console is on its access point; a link that goes and comes back reads 0 or 1 (associating) in between, and 5
    /// once for 29.65 s (the system's re-association back-off). So 2 is "connected" and everything else is not; the byte
    /// only holds attempts back once it has read 2 on this console (so a console where it never does is not locked out).
    static constexpr u8 kWifiConnected = 2;

    struct Input {
        bool want_connected = false;  ///< "Connect to room" is on
        RoomClient::State state = RoomClient::State::Idle;
        RoomClient::Error error = RoomClient::Error::None;
        u64 now_ms = 0;
        u64 launch_ms = 0;            ///< when the plugin started
        bool nickname_automatic = true; ///< the name comes from the console: a suffix can tell two of them apart
        u8 wifi_state = kWifiConnected; ///< the byte above, read now (the policy looks at it every kWifiPollMs)
    };
    enum class Note { None, MacTaken, NameTaken, OldLink };
    /// What the WiFi did since the last call that said so.
    enum class WifiNote { None, Down, Back };
    struct Step {
        bool disconnect = false;  ///< stop the room client ("Connect to room" was switched off)
        bool clear_error = false; ///< forget the last error (it was switched on again)
        bool connect = false;     ///< start an attempt now (then AttemptStarted() says how it went)
        bool alive = true;        ///< false: given up, "Connect to room" is to be switched off
        Note note = Note::None;   ///< something to say about the retry
        WifiNote wifi = WifiNote::None; ///< Down: the attempts wait for the WiFi; Back: it is here again (see wifi_down_ms)
        u32 wifi_state = 0;       ///< the byte that was read (Down)
        u32 wifi_down_ms = 0;     ///< how long the WiFi was down (Back)
    };

    Step Update(const Input& in) {
        platform::LockGuard<platform::Mutex> lock(m_);
        Step step;
        PollWifi(in);
        if (!in.want_connected) {
            // Switching "Connect to room" off is the user's way to start over.
            step.disconnect = in.state != RoomClient::State::Idle;
            last_attempt_ms_ = 0;
            gave_up_ = false;
            failures_ = 0;
            attempts_ = 0;
            waiting_wifi_ = false;
            restart_ = true;
            return step;
        }
        RoomClient::Error error = in.error;
        if (restart_) {
            restart_ = false;
            step.clear_error = true;
            error = RoomClient::Error::None;
        }
        if (in.state == RoomClient::State::Joined) {
            attempts_ = 0;
        }
        if (in.state != RoomClient::State::Idle) {
            return step;
        }
        if (gave_up_) {
            step.alive = false;
            return step;
        }
        const bool collision = error == RoomClient::Error::NameCollision || error == RoomClient::Error::MacCollision ||
                               error == RoomClient::Error::ConsoleIdCollision;
        if ((collision || error == RoomClient::Error::RoomIsFull) && lost_ms_ != 0 &&
            in.now_ms - lost_ms_ < kOldLinkMs) {
            // Our own link of before is still listed: keep who we are and try again on the usual schedule.
            if (!old_link_said_) {
                old_link_said_ = true;
                step.note = Note::OldLink;
            }
        } else if (error == RoomClient::Error::MacCollision && prefer_real_mac_) {
            // The room still has our MAC from an earlier session (or someone else uses it): take whatever
            // address the room hands out instead.
            prefer_real_mac_ = false;
            last_attempt_ms_ = 0;
            step.note = Note::MacTaken;
        } else if (error == RoomClient::Error::NameCollision && in.nickname_automatic && !nickname_suffixed_) {
            nickname_suffixed_ = true;
            last_attempt_ms_ = 0;
            step.note = Note::NameTaken;
        } else if (collision && in.now_ms - in.launch_ms < kLaunchGraceMs) {
            // Right after the game started: the room may still list this console from the previous game (it
            // only drops a member after ENet's timeout). Keep trying with the usual back-off.
        } else if (IsFatal(error)) {
            gave_up_ = true;
            step.alive = false;
            return step;
        }
        if (wifi_seen_up_ && !wifi_up_) {
            // No WiFi: nothing to try with. The state is read again in 500 ms.
            if (!waiting_wifi_) {
                waiting_wifi_ = true;
                step.wifi = WifiNote::Down;
                step.wifi_state = wifi_state_;
            }
            return step;
        }
        if (waiting_wifi_) {
            // It is back: the attempt is at once, whatever the schedule says.
            waiting_wifi_ = false;
            step.wifi = WifiNote::Back;
            step.wifi_down_ms = static_cast<u32>(in.now_ms - wifi_down_since_ms_);
            last_attempt_ms_ = 0;
        }
        if (last_attempt_ms_ == 0 || in.now_ms - last_attempt_ms_ >= kRetryMs) {
            last_attempt_ms_ = in.now_ms;
            ++attempts_;
            step.connect = true;
        }
        return step;
    }

    /// The attempt Update() asked for could not even start (no address, or the network layer would not start). That is
    /// not going to fix itself: after a few of them the policy gives up. Returns false then.
    bool AttemptFailed(bool has_host) {
        platform::LockGuard<platform::Mutex> lock(m_);
        ++failures_;
        gave_up_ = !has_host || failures_ >= 3;
        return !gave_up_;
    }
    /// The network layer (sockets) is up: an attempt that fails from here on is the room's doing.
    void NetworkReady() {
        platform::LockGuard<platform::Mutex> lock(m_);
        failures_ = 0;
    }
    /// Attempts in a row that could not start the network layer.
    u32 failures() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return failures_;
    }

    /// The link was lost (not stopped on purpose): the room may list us for a while.
    void NoteLoss(u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        lost_ms_ = now_ms;
        old_link_said_ = false;
    }
    void NoteJoined() {
        platform::LockGuard<platform::Mutex> lock(m_);
        lost_ms_ = 0;
        old_link_said_ = false;
    }
    /// The console woke up: the next attempt is at once (if it has a WiFi), then the schedule starts over, and the WiFi
    /// state is read again right away (it can have changed while the console slept).
    void ResetBackoff() {
        platform::LockGuard<platform::Mutex> lock(m_);
        last_attempt_ms_ = 0;
        wifi_polled_ = false;
    }
    /// True while the attempts wait for the console's WiFi.
    bool waiting_for_wifi() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return waiting_wifi_;
    }

    bool prefer_real_mac() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return prefer_real_mac_;
    }
    bool nickname_suffixed() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return nickname_suffixed_;
    }
    bool gave_up() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return gave_up_;
    }
    /// Attempts since the link was last healthy.
    u32 attempts() const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return attempts_;
    }
    /// Milliseconds to the next attempt (0: now or not scheduled).
    u32 NextAttemptInMs(u64 now_ms) const {
        platform::LockGuard<platform::Mutex> lock(m_);
        if (last_attempt_ms_ == 0 || now_ms - last_attempt_ms_ >= kRetryMs) {
            return 0;
        }
        return static_cast<u32>(kRetryMs - (now_ms - last_attempt_ms_));
    }
    bool InOldLinkWindow(u64 now_ms) const {
        platform::LockGuard<platform::Mutex> lock(m_);
        return lost_ms_ != 0 && now_ms - lost_ms_ < kOldLinkMs;
    }

    /// Errors that more attempts will not fix: the user has to.
    static bool IsFatal(RoomClient::Error e) {
        switch (e) {
        case RoomClient::Error::NameCollision:
        case RoomClient::Error::MacCollision:
        case RoomClient::Error::ConsoleIdCollision:
        case RoomClient::Error::WrongVersion:
        case RoomClient::Error::WrongPassword:
        case RoomClient::Error::RoomIsFull:
        case RoomClient::Error::HostBanned:
        case RoomClient::Error::HostKicked:
            return true;
        default:
            return false;
        }
    }

private:
    /// Looks at the WiFi byte, at most every kWifiPollMs. (m_ held)
    void PollWifi(const Input& in) {
        if (wifi_polled_ && in.now_ms - wifi_polled_ms_ < kWifiPollMs) {
            return;
        }
        wifi_polled_ = true;
        wifi_polled_ms_ = in.now_ms;
        wifi_state_ = in.wifi_state;
        const bool up = in.wifi_state == kWifiConnected;
        wifi_seen_up_ = wifi_seen_up_ || up;
        if (up != wifi_up_) {
            wifi_up_ = up;
            if (!up) {
                wifi_down_since_ms_ = in.now_ms;
            }
        }
    }

    mutable platform::Mutex m_;
    bool prefer_real_mac_ = true;
    bool nickname_suffixed_ = false;
    bool gave_up_ = false;
    bool restart_ = false;
    bool old_link_said_ = false;
    u32 failures_ = 0;
    u32 attempts_ = 0;
    u64 last_attempt_ms_ = 0;
    u64 lost_ms_ = 0;
    // The console's WiFi, as the shared page says (see kWifiConnected): until it has read "connected" once it holds
    // nothing back.
    bool wifi_polled_ = false;
    u64 wifi_polled_ms_ = 0;
    u8 wifi_state_ = kWifiConnected; ///< what the last poll read
    bool wifi_up_ = true;            ///< ... and what it made of it
    bool wifi_seen_up_ = false;
    u64 wifi_down_since_ms_ = 0;
    bool waiting_wifi_ = false;      ///< the attempts are on hold for the WiFi (said once, until it is back)
};

/// Where the console is in a sleep. The game's APT session says when it goes to sleep (notification 5) and when it wakes
/// (6); the framework's SLEEP_ENTER / SLEEP_EXIT events say the same by another road and are the fallback for a game
/// whose APT calls we do not see. Enter() and Woke() are called by the game's thread, Update() once per frame by the
/// main thread.
class SleepWatch {
public:
    /// Without a wake-up in this long (awake time: the clock stops while the console sleeps) the sleep is taken for one
    /// that did not happen.
    static constexpr u32 kWakeWaitMs = 30000;

    // (Enter and Woke run on the game's APT thread, inside the svc hook: Tests/policy_check.sh)
    // >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
    /// "The console goes to sleep now". `acts`: the plugin reacts to it (sleep_stop), so the sleep is tracked.
    void Enter(u32 now_ms, bool acts) {
        enter_ms_ = now_ms;
        ++apt_enters_;
        if (acts) {
            sleeping_ = true;
        }
    }
    /// "The console woke up".
    void Woke() {
        ++apt_wakes_;
    }
    // <<< game-thread code
    /// A sleep was acted on and its wake-up has not been seen yet.
    bool sleeping() const {
        return sleeping_.load();
    }

    struct Result {
        bool apt_wake = false;  ///< the game's notification said it woke up
        bool fw_wake = false;   ///< the framework's event did
        bool missed = false;    ///< the framework saw a sleep that the game's notifications did not show: act on it now
        bool timed_out = false; ///< no wake-up for kWakeWaitMs
        bool woke = false;      ///< a sleep that was acted on is over
    };
    /// Does Update() need `acts` for these counts? Only when the framework reported a wake-up it has not seen yet: the caller
    /// need not work `acts` out (with sleep_stop=1 that takes the engine's lock) on every frame.
    bool NeedsActs(u32 fw_exits) const {
        return fw_exits != exits_handled_;
    }
    /// `fw_enters` / `fw_exits` count the framework's SLEEP_ENTER / SLEEP_EXIT events since the start.
    Result Update(u32 now_ms, u32 fw_enters, u32 fw_exits, bool acts) {
        Result r;
        bool woke = false;
        if (apt_wakes_ != wakes_handled_) {
            wakes_handled_ = apt_wakes_;
            r.apt_wake = woke = true;
        }
        if (fw_exits != exits_handled_) {
            exits_handled_ = fw_exits;
            r.fw_wake = woke = true;
            if (acts && !sleeping_.load() && fw_enters > apt_enters_) {
                r.missed = true;
                enter_ms_ = now_ms;
                sleeping_ = true;
            }
        }
        if (sleeping_.load() && !woke && now_ms - enter_ms_ > kWakeWaitMs) {
            r.timed_out = woke = true;
        }
        r.woke = woke && sleeping_.exchange(false);
        return r;
    }

private:
    std::atomic<u32> apt_enters_{0};
    std::atomic<u32> apt_wakes_{0};
    std::atomic<u32> enter_ms_{0};
    std::atomic<bool> sleeping_{false};
    u32 wakes_handled_ = 0; // (main thread)
    u32 exits_handled_ = 0;
};

/// Messages for the screen (the game's own graphics are used for them): one per change, at most one every 5 s. One
/// that comes too soon is kept for the next free moment, a newer one replaces it. Thread safe.
class Announcer {
public:
    static constexpr u32 kMinGapMs = 5000;

    /// Returns the message if it can go on screen now, else an empty string (it is kept).
    std::string Offer(const std::string& text, u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        if (shown_ && now_ms - last_ms_ < kMinGapMs) {
            pending_ = text;
            return std::string();
        }
        shown_ = true;
        last_ms_ = now_ms;
        pending_.clear();
        return text;
    }
    /// The kept message, once the gap has passed (else an empty string).
    std::string Poll(u64 now_ms) {
        platform::LockGuard<platform::Mutex> lock(m_);
        if (pending_.empty() || now_ms - last_ms_ < kMinGapMs) {
            return std::string();
        }
        last_ms_ = now_ms;
        std::string text;
        text.swap(pending_);
        return text;
    }

private:
    mutable platform::Mutex m_;
    bool shown_ = false;
    u64 last_ms_ = 0;
    std::string pending_;
};

} // namespace azl
