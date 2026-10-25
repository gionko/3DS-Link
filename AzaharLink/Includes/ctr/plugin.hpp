// Plugin glue: configuration, logging to the SD card, and the wiring between the room client, the
// UDS engine, the IPC front-end and the svc hooks.
#pragma once

#include <3ds.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "azl/ini_values.hpp"
#include "azl/link_policy.hpp"
#include "azl/log_files.hpp"
#include "azl/platform.hpp"
#include "azl/room_client.hpp"
#include "azl/room_members.hpp"
#include "azl/screen_text.hpp"
#include "azl/sleep_crumbs.hpp"
#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"
#include "ctr/hook.hpp"
#include "ctr/kernel_ops.hpp"
#include "ctr/uds_hle.hpp"

namespace azl {
namespace ctr {

constexpr const char* kVersion = "1.0.4"; // (the log header and the menu text; the menu's ints and plgInfo say 1.0.4: Tests/set_version.py writes all of them)

struct Config {
    bool enabled = true;                  ///< false: the plugin stays completely out of the game
    std::string host;                     ///< room server address (IP or host name)
    u16 port = room::kDefaultPort;
    std::string nickname;                 ///< empty: use the name set on the console
    std::string password;
    bool tunnel_enabled = true;           ///< false: like enabled=0 (the real local wireless is used)
    bool autoconnect = true;              ///< join the room as soon as the game has booted
    int radio_like = 1;                   ///< 0 all reliable, 1 broadcast game data unreliably like the real radio, 2 unicast too, twice (ini::ParseRadioLike)
    bool socket_buffers = true;           ///< ask the soc service for bigger UDP buffers
    int net_core = -1;                    ///< core of the network thread (-1 any, 0 game core, 1 syscore)
    std::string menu_key = ini::kMenuKeyOff; ///< chord that opens the in-game menu; "off" (or "none", the default): it never opens
    std::string game_name;                ///< empty: read it from the game, shown in the room
    int debug = 0;                        ///< 0 no log at all (no file is written), 1 everything (an old 2 reads as 1)
    int rcvbuf_kb = ini::kRcvbufKbDefault; ///< ini key rcvbuf_kb: 8, 16, 32 or 64; it can only LOWER the receive ring
    int sleep_stop = ini::kSleepStopDefault; ///< ini key sleep_stop: 0 the sleep is only logged, 1 it ends the session of a client, 2 of a host too (see OnSleepEnter)
};

class Plugin {
public:
    static Plugin& Get();

    /// Called from PatchProcess, before the game runs: reads the settings, builds the engine and
    /// patches the svc stubs (unless the plugin is switched off).
    void EarlyInit();
    /// Called from main(): opens the log file, starts the log writer, resolves the identity.
    void LateInit();
    /// Called when the game is closing, from the framework's exit event. Only flag stores: it runs
    /// on a tiny stack.
    void RequestStop();
    /// Bounded, orderly shutdown. Called at the end of main() (the framework waits 1 s for it).
    void Shutdown();

    /// False when the settings say "off" (enabled=0 or tunnel=0): no hooks, no network, nothing.
    bool active() const {
        return active_;
    }

    Config& config() {
        return config_;
    }
    bool LoadConfig();
    bool SaveConfig();

    /// Joins the configured room (background thread). Returns false if it cannot even start.
    bool Connect();
    void Disconnect();

    /// Per-frame bookkeeping: reconnect on lost connections, flush notifications.
    /// `want_connected` is the state requested by the user.
    /// Returns false when the plugin gave up and the request should be cancelled.
    bool Tick(bool want_connected);

    RoomClient& room() {
        return *room_;
    }

    /// The nickname that will be used in the room (configured, or taken from the console).
    std::string EffectiveNickname() const;
    std::string GameName() const;
    /// The nickname used for the current room session (the network thread and the main thread both look at it).
    std::string JoinedNickname() const;

    std::string StatusLine();
    std::string DetailText();

    /// Switches the log on (1) or off (0) now and remembers it in the settings. On: the log files are opened and
    /// everything is written; off: what is left is written, the files are closed and nothing is written again.
    /// Returns false if the capture ring could not be allocated (the main log is on all the same).
    bool SetDebugLevel(int level);
    int debug_level() const {
        return config_.debug;
    }

    /// True when the settings switch the in-game menu off (menu_key=off): it never opens, so everything that
    /// needs it (the room address, joining) has to come from the settings.
    bool menu_off() const {
        return ini::MenuKeyIsOff(config_.menu_key);
    }

    /// Something the user must see on screen: why the plugin switched itself off at start-up, or that the socket buffer is
    /// smaller than asked. Empty when all is well. Set once, in EarlyInit. (The memory the game has left is not in it: that is
    /// read about 20 s into the game, see CheckGameMemory.)
    const std::string& startup_issue() const {
        return startup_issue_;
    }

    /// Posts the start-up problem again once a minute (only while the plugin is OFF because of it).
    void RepeatStartupIssue();

    /// A process event of the framework (sleep, home, swap, exit). Only flag stores: it runs on the
    /// framework's small keep-alive thread; the log writer reports it.
    void NoteProcessEvent(int event);

    /// The game's APT session got "the console goes to sleep now" / "it woke up" (see ctr/hook.cpp). Called on the
    /// game's thread, so short and without waits or IPC. With `sleep_stop=1` in the settings (0, the default, only
    /// logs) the sleep ends the game's session through the engine's StopService, the way a console's sleep ends
    /// the wireless session, and asks the network thread to leave the room - but only for a console that is not hosting:
    /// the one host that went to sleep this way froze (PROVA 1), and a host that does nothing keeps its lobby (PROVA 2).
    /// `sleep_stop=2` forces it for a host too. The wake is only a flag.
    void OnSleepEnter();
    void OnWake();
    /// Whether the sleep that begins now is acted on (see OnSleepEnter): sleep_stop 2, or 1 and not hosting.
    bool SleepActs() const;
    /// The game's APT session or the framework told of something around a sleep (kAptNote* of azl/apt_sleep.hpp, or
    /// SleepCrumbs::kFwEnter / kFwExit): noted for the breadcrumb thread, which writes it down (AzaharLink.sleep.log, with the
    /// log on). Called on the game's thread inside its APT call, or on the framework's keep-alive thread: a few stores and
    /// one event signal.
    static void NoteSleepEvent(u32 kind, u32 value);
    /// The game's APT session got the HOME button notification (ids 1 and 2; only called with the log on). Called on the
    /// game's thread: it only asks the log writer to put the queued lines on the card at once (an event signal; the
    /// SD card is never written on a game thread). The game may be about to close, and what is in memory then is lost.
    void OnHomeButton();

    /// Messages produced on worker threads, to be shown on the main thread.
    std::vector<std::string> TakeNotifications();
    void PostNotification(const std::string& message);

private:
    Plugin() = default;

    /// Where the name shown in the room came from (a stronger source is never replaced by a weaker).
    enum class NameSource { ProcessName = 0, RoomPeer, Icon, Config };

    /// Called by the log writer thread, once a second: the statistics window (at every level: the warnings and the
    /// menu's numbers come from it) and, with the log on, the queued lines and the capture.
    void ServiceLog();
    void LogStats(u64 now_ms);
    /// Keeps the numbers of the window that just closed for the status page of the menu (see DetailText).
    void NoteStatusNumbers(const Stats& s, u64 dt_ms, const RoomClient::LinkStats& link);
    /// The two lines of timing and wire instrumentation that follow the counters in the statistics block.
    void LogStatsTiming(const Stats& s, u64 dt_ms);
    void LogStatsWire(const Stats& s);
    /// The capture's own line of the window: what went into the ring, what did not and why, what the diet counted.
    void LogStatsCapture();
    /// "clock: console clock ..." in the main log and the capture, once a minute and after a wake-up: the lines of both only
    /// have the time since the plugin started, and a tick that stops in a sleep does not show it.
    void LogClockLine(const char* why);
    /// Who is in the room now against who was: a line for every player that came and every one that went.
    void NoteRoomMembers(const room::RoomInfo& info);
    /// Puts on screen what the counters of the last interval show is wrong (rate limited per kind).
    void CheckStatsWarnings(const Stats& s, u64 now_ms);
    /// With the log on: a notice when the log itself slows the game (the SD card busy, the capture dropping records).
    void CheckDebugLoad(const Stats& s);
    /// With the log on: a notice when the room's datagrams stop for 150 ms or more (the WiFi link, seen at the socket).
    void CheckLinkSilence(const Stats& s);
    /// Shows `message` unless this kind of warning was shown within the last minute; true if it was shown.
    bool Warn(int kind, const std::string& message);
    /// Once a second, from the log writer: notes whether the game made a UDS call since the last look (the counters are
    /// cleared every window, so a count that changed is activity, one that did not is not), for the text of "frames lost".
    void SampleUdsActivity(u64 now_ms);
    /// The memory the game has left (azl/screen_text.hpp): the reading about 20 s after the start and every 30 s, for the
    /// toast and the Status page. Nothing while the plugin is off.
    void CheckGameMemory(u64 now_ms);
    void LogAddressMap();
    /// Logs the memory state (nothing is logged with debug=0).
    void LogMemory(const char* when);
    /// With the log on: the application region at the start against the last launch of the same title (AzaharLink.mem).
    void CompareMemoryWithLastLaunch();
    /// Free bytes of the plugin heap (its fixed capacity minus what is allocated), -1 if the capacity is unknown.
    long HeapFreeBytes() const;
    void StartLogWriter();
    void WakeLogWriter();
    /// The breadcrumb thread (azl/sleep_crumbs.hpp): started with the log on, once; it never writes with the log off.
    void StartCrumbs();
    /// Reports the process events the framework signalled since the last call.
    void LogProcessEvents();
    /// Reads the link state the system keeps in the shared page (no IPC) and logs a change.
    void SampleLinkState(u64 now_ms);
    /// What the first socket of the room got for SO_RCVBUF, on screen when it is below the request.
    void CheckSocketBuffer();

    void ResolveIdentity();
    void ResolveGameName();
    void OnRoomInfo(const room::RoomInfo& info);

    /// The room link left Joined (state callback: the network thread, or whoever stopped the client). Returns true if
    /// the screen was told something that takes the place of the generic "Room: <error>" message.
    bool OnRoomLeft(RoomClient::Error error);
    /// The room was joined. Returns true if the screen was told so (the game's session had been ended).
    bool OnRoomJoined();
    /// The game's session ends because the link is gone; says so on screen. Returns true if the screen was told.
    bool DropSession(RoomClient::Error error);
    /// A message for the screen, at most one every 5 s (one per change).
    void Announce(const std::string& text);
    /// The console woke up (the game's notification, or the framework's event when the game never showed us the
    /// sleep): the next attempt to join is at once. Main thread, once per frame.
    void HandleWake(u64 now_ms);

    bool initialized_ = false;
    bool config_from_old_place_ = false; ///< the settings were read from /luma/plugins/AzaharLink.ini (an older app wrote them there)
    bool late_initialized_ = false;
    bool shut_down_ = false;
    bool active_ = true;
    std::atomic<bool> shutting_down_{false};

    Config config_;
    Mac console_mac_{};
    std::string console_name_;      ///< user name set on the console (sanitised), may be empty
    std::string process_name_;      ///< the 8-character name from the game's exheader
    mutable platform::Mutex name_mutex_; ///< guards game_name_ / name_source_ / joined_nickname_
    std::string game_name_;
    NameSource name_source_ = NameSource::ProcessName;
    std::string joined_nickname_;   ///< the nickname used for the current room session
    u64 game_id_ = 0;
    u64 start_ms_ = 0;
    u8 language_ = 1;              ///< console language (CFG_Language), picks the game's title

    CtrKernelOps kernel_;
    std::unique_ptr<RoomClient> room_;
    std::unique_ptr<UdsEngine> engine_;
    std::unique_ptr<UdsHle> hle_;
    HookStats hook_stats_;

    std::unique_ptr<FileLayer> file_layer_;
    std::unique_ptr<LogFiles> log_files_; ///< the log's files and queue (see azl/log_files.hpp): nothing is opened at level 0
    u64 last_log_service_ms_ = 0;
    u64 last_stats_ms_ = 0;
    u64 last_mem_ms_ = 0;
    u64 last_clock_ms_ = 0;         ///< when the last "clock:" line was written (the writer thread)
    CaptureTotals capture_prev_{};  ///< the capture's totals at the end of the previous window (the writer thread)
    platform::Mutex members_mutex_; ///< guards room_members_ (the network thread, and whoever stops the client)
    std::vector<RoomMemberRow> room_members_; ///< the players the room listed at the last RoomInfo
    bool room_members_known_ = false; ///< room_members_ is the room as of the first RoomInfo of this join (no lines for it)
    platform::Thread log_thread_;   ///< runs the statistics window and writes the log out, at low priority, away from the game
    std::atomic<bool> log_stop_{false};
    std::atomic<bool> log_flush_requested_{false}; ///< the HOME button: the writer flushes the main log on its next wake
    Handle log_wake_ = 0;           ///< the writer waits on it (1 s, or 250 ms with the log on): stop and shutdown signal it
    // The trail of a sleep: its own thread, its own event, its own file (nothing shared with the log writer, which can sit
    // in a long SD write or starve behind a spinning game thread).
    SleepCrumbs crumbs_;
    platform::Thread crumb_thread_;
    volatile Handle crumb_wake_ = 0; ///< signalled by NoteSleepEvent (set once, before the thread starts)
    std::atomic<bool> crumb_stop_{false};

    // The numbers of the last statistics window, for the status page of the menu (written by the writer thread,
    // read by the menu).
    mutable platform::Mutex status_mutex_;
    StatusNumbers status_numbers_;
    gamemem::Reading mem_reading_;   ///< the last reading of the game's memory (under status_mutex_)
    gamemem::Watch mem_watch_;       ///< when the next one is due and what it says (the writer thread)
    // The game's UDS activity, for the text of "frames lost" (the writer thread).
    frameslost::Idle uds_idle_;      ///< how long ago a look at Stats::ipc_requests last found it changed

    // Process events of the framework (see NoteProcessEvent): counted by whoever signals, reported by the writer.
    static constexpr int kProcessEvents = 7;
    volatile u32 event_count_[kProcessEvents] = {};
    volatile u32 event_ms_[kProcessEvents] = {};
    std::atomic<u32> event_reported_[kProcessEvents] = {};
    // The link state in the shared page and the clocks: a console that slept keeps counting in osGetTime()
    // while the system tick does not.
    bool link_known_ = false;
    u8 link_strength_ = 0;
    u8 link_state_ = 0;
    u32 link_changes_ = 0;
    u64 next_link_sample_ms_ = 0;
    u64 clock_base_os_ms_ = 0;      ///< osGetTime() when the plugin started ...
    u64 clock_base_ms_ = 0;         ///< ... and the system tick then (ms)
    u64 joined_ms_ = 0;             ///< when the room was joined (0 = not joined)
    u64 joined_os_ms_ = 0;
    u64 warned_ms_[8] = {};         ///< when each kind of warning was last shown

    platform::Mutex notify_mutex_;
    std::vector<std::string> notifications_;

    std::string startup_issue_;
    u64 last_issue_ms_ = 0;
    u64 last_hint_ms_ = 0;          ///< the last "no room address" reminder (only with the in-game menu off, see Tick)
    int last_notified_error_ = -1;  ///< last room error shown on screen: do not repeat the same one

    // The room link and the game's session (see azl/link_policy.hpp).
    RejoinPolicy rejoin_;           ///< when to join again, who to be, when to give up
    LossGuard guard_;               ///< a lost link ends the session, unless it was our own pause
    Announcer announcer_;           ///< the messages about it, at most one every 5 s
    bool want_connected_ = false;   ///< "Connect to room" as Tick saw it last (main thread)
    std::atomic<bool> session_dropped_{false}; ///< the game's session was ended by a lost link: say so when the room is back
    SleepWatch sleep_;              ///< where the console is in a sleep (see OnSleepEnter)
};

} // namespace ctr
} // namespace azl
