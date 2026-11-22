// The engine of the Download Play page of 3DS Link (1.1.4, route B: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md, approved by the
// user on 2026-11-16, sections 1, 3.3 and D-B1..D-B10). The app joins the room of the settings and runs the Download Play session with a host there
// itself, with the code the plugin already has for the system's Download Play application: the adapter of the real Download Play (RealDlpNet: the
// spectator's probes, the join, the answers A2 / A4 without content / A6 / A9, the first final's passphrase), the lobby of any game (DlpLobby) and the
// book of the titles the hosts announce (DlpClntBook). At the passphrase the 16 bytes go to ns:s (dlp_join_sys.hpp) and the app chain-loads the
// hidden prepared child (main.cpp): the child's GetWirelessRebootInfo is the SYSTEM's answer. Nothing of the child ever travels through the room,
// and the content is never asked (RealDlpNet answers A4 without it whatever happens).
//   - Core: pure of libctru (Tests/dlp_join_test.cpp builds it on the in-memory room). The UI thread posts INTENTS (AskScan, AskJoin, AskLeave, AskEnd);
//     the room's network thread runs them in Step, then the adapter's and the lobby's pumps, all under ONE lock: the UI never opens a scan nor a
//     connection itself. Step counts its rounds: the polite leave waits for two of them after the lobby let go (D-B10), so that the Deauthentication
//     the release queued in the room's queue goes out before the room is left (a RoomClient::Stop drops what is still queued).
//   - Session (the console only): the room (RoomClient with the identity of D-B4: the console's MAC and nickname, kAppTid, "3ds-<mac>-dlp"), the
//     engine on AppKernel, the table of the children from the index file the plugin reads (BuildTable), the Core on a heap block wiped after its
//     destructor. Stop = LeaveRounds, then RoomClient::Stop, then one line of the log: whether the room answered.
// Privacy: the host's MAC and the passphrase live in the lobby until TakeReboot copies them out and lets the lobby go (ClientStopSession); they are
// never logged and never written to a file (Tests/policy_check.sh). The log lines say states and times only.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "azl/dlp_clnt_book.hpp"
#include "azl/dlp_lobby.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/platform.hpp"
#include "azl/uds_engine.hpp"
#include "dlp_join_logic.hpp"

namespace dlpjoin {

/// The lobby's beacons at most (the engine's kHeardLobbies: the adapter probes as many).
constexpr u32 kMaxBeacons = 4;
/// D-B10: how long the polite leave waits for the two rounds of the network thread (a round is a few ms: ENet's service timeout is 3).
constexpr u32 kLeaveWaitMs = 150;
/// D-B10: the rounds after the release (the round that queued the Deauthentication may have flushed the queue before it; the next one flushes it).
constexpr u32 kLeaveRounds = 2;

class Core {
public:
    /// `engine` outlives the core and is the app's own (the room's frames reach it); `me` the console's MAC (the lobby's seed, FriendSeedOf);
    /// `nickname` the player's name (UTF-16, the first kNameUnits units); `table` the children (outlives the core); `missing_row` the children of
    /// "index not written" (BuildTable). The first Step initialises the lobby and starts the scan.
    Core(azl::UdsEngine* engine, const azl::Mac& me, const std::u16string& nickname, const azl::dlp_children::Table* table,
         const std::vector<u64>& missing_row);
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    /// The network thread (RoomClient's tick, after the engine's Tick): the intents, then the adapter's and the lobby's pumps, then the end of a join;
    /// one more round counted.
    void Step(u64 now_ms);
    /// The UI thread: intents, run by the next Step.
    void AskScan();                        ///< a new scan (SELECT): the table starts again, every host is probed again
    void AskJoin(const azl::Mac& host);    ///< ClntStartSession's sequence for a host the list showed as Join (refused otherwise: no frame)
    void AskLeave();                       ///< B in 3/5/6: ClientStopSession, then the scan again; the session stays in the room
    void AskEnd();                         ///< the page goes: ClientFinalize (a probe, a join, a session end there); an earlier release forgotten
    /// The list (under the lock): the scan's hosts, with their verdict, their prepared title, full or not, in the index file or not.
    std::vector<Host> Hosts();
    u32 Word();                            ///< the lobby's GetMyStatus word (state << 24 | in-session << 16 | node)
    bool Joining() const;                  ///< a join under way (the word says 1 then)
    u32 Rounds() const;                    ///< Steps run so far
    /// What the page reads in one go: the word, the join flag, the rounds and the refusals.
    struct View {
        u32 word = 0;
        bool joining = false;
        u32 rounds = 0;
        u32 refusals = 0;
    };
    /// All four under the lock, so they are one Step's answer, never half of two. (Word() and then Joining() could straddle the Step that ends
    /// a join: the word of before (1, shown as idle) with the flag of after (false) is "Scanning", and the page took the join as refused.)
    View Look();
    /// D-B10: the lobby let go (an intent, TakeReboot) and two rounds of the network thread ran since: the room's queue sent the Deauthentication.
    /// AskEnd forgets any earlier release first: after it, only the release of the end (or one later) counts.
    bool LeaveFlushed() const;
    u32 refusals() const;                  ///< AskJoin refused (a host not Join, or gone from the list)
    u32 last_join_result() const;          ///< the last join that ended with an error (0: none)
    u64 passphrase_ms();                   ///< the network thread's clock when the state 9 was first seen (0: never)
    /// State 9 only: the joined host's MAC and the passphrase into ComposeRebootInfo, the prepared title of its row into *prepared, then
    /// ClientStopSession (the lobby forgets the passphrase). False in any other state (nothing written). Never logged.
    bool TakeReboot(u8 (&info)[16], u64* prepared);

private:
    friend struct CoreTestAccess;  // (Tests/dlp_join_test.cpp: the lobby and the adapter, read under no lock once the network thread is stopped)
    static constexpr u32 kWantScan = 1, kWantJoin = 2, kWantLeave = 4, kWantEnd = 8;
    Host HostOf(const azl::Group& g);
    void DoJoin(bool rescanned);
    void MarkRelease();
    bool Missing(u32 child_low) const;

    azl::platform::Mutex mu_;
    azl::RealDlpNet net_;
    azl::DlpClntBook book_;
    azl::DlpLobby lobby_;
    std::vector<u64> missing_row_;
    azl::Group seen_[2 * azl::kMaxGroups] = {};  // the hosts Hosts() showed: a join adopts the one the user chose (a new scan clears the table)
    bool seen_used_[2 * azl::kMaxGroups] = {};
    std::size_t seen_next_ = 0;
    azl::Mac ask_host_ = {};                 // AskJoin's host (under the lock)
    azl::Mac joined_ = {};                   // the host of the join (until TakeReboot / the end)
    u64 joined_prepared_ = 0;
    u64 passphrase_ms_ = 0;                  // the first Step that saw the state 9 (the log's "from the passphrase to the room exit")
    std::atomic<u32> want_{0};
    std::atomic<u32> rounds_{0};
    std::atomic<u32> release_mark_{0};
    std::atomic<bool> released_{false};
    std::atomic<bool> joining_{false};
    std::atomic<u32> refusals_{0};
    std::atomic<u32> last_result_{0};
    bool started_ = false;
    bool ended_ = false;
};

/// The Core on a block of its own (`size` >= sizeof(Core)), and its end: the destructor, then every byte of the block wiped (volatile) - the lobby
/// held a passphrase and a host's MAC - before the caller frees it (J10).
Core* MakeCore(void* block, std::size_t size, azl::UdsEngine* engine, const azl::Mac& me, const std::u16string& nickname,
               const azl::dlp_children::Table* table, const std::vector<u64>& missing_row);
void EndCore(Core* core);

/// D-B10, the polite leave of the room's lobby: AskEnd, then wait (at most kLeaveWaitMs) until LeaveFlushed - two rounds of the network thread after
/// the release, so that the Deauthentication to the host left the room's queue. True when the rounds came (false: no network thread, or a slow one).
bool LeaveRounds(Core& core, u64 (*now_ms)(), void (*sleep_ms)(u32));

/// State 9, the hand-over: TakeReboot, the 16 bytes to `send` (ns:s SetWirelessRebootInfo on the console), the copy wiped (volatile) whatever it
/// answered. The prepared title to jump to ONLY when `send` answered 0; on a failure 0 (no jump: the session with the host is gone anyway).
/// *rc = what `send` answered, or -1 when the lobby was not in its state 9 (nothing sent).
u64 HandOver(Core& core, s32 (*send)(const u8 (&info)[16]), s32* rc);

/// The index file as the plugin reads it ("sdmc:" + dlp_children::kIndexPath, cut at kIndexMaxBytes as Plugin::ReadDataFile does). False: no file.
bool ReadIndexFile(std::string* text);
/// The table of the children of this console: the index file, the app's records, `present` (AM) and `plugin_there` (the plugin's folder); the
/// blocks from the app's folder (dlpguest::BlockPath). BuildTable's answer.
bool LoadChildren(const PresenceFn& present, const FileFn& plugin_there, Children* out, std::vector<u64>* missing_row);

/// One line, with the time, at the end of 3DSLink-dlp-join.log in `datadir` (the last 200 lines). States and times only: never a MAC or a passphrase.
void AppendLog(const std::string& datadir, const std::string& line);
/// (the build 2f) Several lines the same way, the file read and written once (the lines of the scans, ScanLog).
void AppendLogLines(const std::string& datadir, const std::vector<std::string>& lines);

/// The end of the parts of a session the room's network thread uses - the room client, the engine, the core, the table, all of one heap block
/// (the final review of 1.1.4): `stop` leaves the room (RoomClient::Stop) and says whether that thread ended; only then does `free` run. A thread
/// Stop abandoned (stuck in a name lookup or a soc call for more than kStopJoinMs) still holds the room client and may run one more round of its
/// callbacks when it wakes up: the block stays allocated for good, as probe.cpp's StopAndFree leaves its client (memory kept, never a write into
/// memory that is gone). True when it was freed.
template <class StopFn, class FreeFn>
bool EndNetParts(StopFn stop, FreeFn free) {
    if (!stop()) {
        return false;
    }
    free();
    return true;
}

}  // namespace dlpjoin

#ifdef __3DS__
#include <memory>

#include "azl/room_client.hpp"
#include "dlp_join_kernel.hpp"

namespace dlpjoin {

/// What the session takes from the app.
struct SessionIn {
    std::string host;
    u16 port = 0;
    std::string password;
    std::string nickname;  ///< g.EffectiveNickname(): the plugin's rule, so the child comes back under the same name
    azl::Mac mac = {};     ///< the console's MAC (setup::ReadConsoleMac): the child comes back with it
    std::string datadir;   ///< where 3DSLink-dlp-join.log goes
};
/// A member of the room that plays a game whose child is Ready here but has not opened its Download Play yet (the grey rows).
struct Member {
    std::string nickname;
    u64 game_id = 0;
    azl::Mac mac = {};  ///< (the session's own: WaitingMembers compares it with the lobbies' hosts; the page never reads it)
};

/// The console's side: the room, the engine, the table, the core.
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// JoinGate(f) first: a refusal starts nothing. Then the table (LoadChildren), the engine, the core, the room. Ok, or why not (NoGuests when the
    /// table has no Ready row after all; NoRoom when the room's thread could not start).
    Why Start(const Facts& f, const SessionIn& in);
    /// D-B10: LeaveRounds, then RoomClient::Stop (its polite disconnect), then the line of whether the room answered. Then everything goes (the core
    /// wiped). Twice is nothing.
    void Stop();
    /// Stop with its lines given to the caller (the scans, then the leaving): written by the caller in ONE write of the log (the review of the build 2f).
    void StopInto(std::vector<std::string>* lines);
    /// State 9: HandOver with ns:s (dlp_join_sys.hpp), then Stop; the prepared title to jump to (0: none, *rc says why), the two times in the log.
    u64 HandOverAndLeave(s32* rc);
    /// (DPAPP-2j, R-A) the gate at A: a closed game of the guest `prepared` still in memory (zombie_free::Remaining - the form M3c1 leaves one,
    /// freed at the start of 3DS Link; or the process list not read on a console). Said in the session's log with its milliseconds. true: no join.
    /// (DPAPP-2j-rev) `unread`: true for the list not read alone (no closed game seen) - the log and the page say that, not "a closed game"
    bool ClosedGameLeft(u64 prepared, bool* unread);
    Core* core() {
        return net_ != nullptr ? net_->core : nullptr;
    }
    bool started() const {
        return net_ != nullptr && net_->core != nullptr;
    }
    /// The phase from one Core::Look; `view` (optional) gets that same look, for the page's rounds and refusals of the same instant.
    Phase phase(Core::View* view = nullptr);
    azl::RoomClient::State room_state() const {
        return net_ != nullptr ? net_->room.state() : azl::RoomClient::State::Idle;
    }
    azl::RoomClient::Error room_error() const {
        return net_ != nullptr ? net_->room.error() : last_error_;
    }
    bool left_acknowledged() const {
        return net_ != nullptr ? net_->room.LeaveAcknowledged() : left_ack_;
    }
    u64 left_room_ms() const {  ///< when the last Stop left the room (platform::NowMs; 0: never)
        return left_ms_;
    }
    const Children* children() const {
        return net_ != nullptr ? net_->children.get() : nullptr;
    }
    /// The members that wait (see Member), less those that host one of `hosts` already (HostsALobby: by MAC, not by name).
    std::vector<Member> WaitingMembers(const std::vector<Host>& hosts);
    /// (the build 2f, B3) a line of the scans (ScanLine): kept in memory (no I/O while the page looks), and a line equal to the last one is not kept
    /// again. Written AFTER the room is left, in the ONE write of the log the end of the session makes anyway (Stop; on the way out of the jump,
    /// HandOverAndLeave, together with its line): no write more than before B3 - the review of the build 2f, whose comment said "never on the
    /// way out of the jump" while Stop wrote one more there.
    void NoteScan(const std::string& line) {
        scan_log_.Note(line, azl::platform::NowMs());
    }

private:
    /// What the room's network thread touches (the room client itself, and what its callbacks and the engine's transport use): one heap block
    /// that the callbacks capture - never the Session, which goes with the page - freed only when that thread ended (EndNetParts).
    struct Net {
        azl::RoomClient room;
        AppKernel kernel;
        std::unique_ptr<azl::UdsEngine> engine;
        std::unique_ptr<Children> children;
        void* core_block = nullptr;
        Core* core = nullptr;
        azl::platform::Mutex members_mu;
        std::vector<Member> members;
    };
    /// A block no network thread uses (none started, or it ended): the core wiped (EndCore), then everything.
    static void FreeNet(Net* n);
    void Log(const std::string& line);
    Net* net_ = nullptr;
    std::vector<u64> missing_;
    std::string datadir_;
    u64 left_ms_ = 0;
    ScanLog scan_log_;  // (the build 2f, B3) the lines of the scans of this session
    azl::RoomClient::Error last_error_ = azl::RoomClient::Error::None;  // (the room's, kept when its block goes)
    bool left_ack_ = false;
};

}  // namespace dlpjoin
#endif
