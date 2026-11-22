// The files of the log, and the queue that feeds them: AzaharLink.log (the main log) and AzaharLink.debug.log (the capture),
// both written in blocks: when the current file reaches its size it is closed and renamed AzaharLink.N.log /
// AzaharLink.debug.N.log with N one more than the highest N there (so the blocks read in order, 1 the oldest), and a fresh
// current file begins; when there are more blocks than kept, the lowest N goes. Nothing is ever started afresh at a launch:
// a launch appends to the current file. While the log is off (g_debug_level 0, see stats.hpp) nothing is created, opened or
// written, whatever calls in here. The plugin runs this class on top of CTRPluginFramework's File, the PC tests on top of a
// directory.
#pragma once

#include <string>

#include "azl/capture_pace.hpp"
#include "azl/common.hpp"
#include "azl/platform.hpp"

namespace azl {

/// The file system under the log. Every call is best effort: a file that cannot be opened is a null handle.
class FileLayer {
public:
    virtual ~FileLayer() = default;
    /// Opens `path` for appending, creating it when it is missing.
    virtual void* OpenAppend(const std::string& path) = 0;
    virtual u64 Size(void* file) = 0;
    /// Writes. The file system may hold it back until Flush() (on the console it does: the size in the directory entry
    /// is only updated by a flush, and what was not flushed is lost when the game is killed).
    virtual void Write(void* file, const char* data, std::size_t size) = 0;
    /// Commits what was written. The main log is flushed at every write (its last lines are what a post-mortem needs);
    /// the capture once a pass of the writer (DrainDebug), not at every write of it: a flush costs the card as much as a
    /// small write, and the card is the game's too (2026-10-20).
    virtual void Flush(void* file) = 0;
    virtual void Close(void* file) = 0;
    virtual void Remove(const std::string& path) = 0;
    virtual void Rename(const std::string& from, const std::string& to) = 0;
    /// Is there a file at `path`? (The blocks of a log are numbered by looking at what is there.)
    virtual bool Exists(const std::string& path) = 0;
};

struct LogFileLimits {
    /// The main log: when the current file reaches this size it becomes the next block (AzaharLink.N.log) and a fresh one begins.
    u64 log_rotate_bytes = 64ull * 1024 * 1024;
    /// The capture: the same, in blocks AzaharLink.debug.N.log.
    u64 debug_rotate_bytes = 64ull * 1024 * 1024;
    /// Blocks kept besides the current file, for each of the two logs: with the current one, at most keep + 1 files
    /// (50 of 64 MB = 3.2 GB for the capture of the heaviest sessions). The lowest N goes when there are more.
    int keep = 49;
    std::size_t max_pending = 64 * 1024; ///< lines waiting for the writer; more are dropped
    /// How long the capture may keep writing once the launch is ending (BeginShutdown): what the ring still holds after that is
    /// dropped. The full 128 GB card of the Zelda session (2026-10-14) took ~40 KB/s, so a full 512 KB ring needs ~13 s, and the system gives the game
    /// a moment to leave, not that: the main log's last lines - the only record of how the exit went - must reach the file first.
    u32 exit_capture_ms = 500;
    /// The capture goes to the card in FEW, LARGE passes: a flush costs the card about the same whatever it carries, and on the console
    /// the writer spent 63-77% of its time waiting for the SD card (the game reads from the same card: its music stuttered, 2026-10-28,
    /// Kirby Battle Royale) with a pass every 250 ms for 5-20 KB of text. A pass is due (CaptureDue) when this long has gone by since the
    /// last one, or when the ring holds this much (a ring of 512 KB must not fill while the card is busy), or when something asks at once
    /// (the HOME button, a switch-off). The ring is the memory of the plugin (the heap Luma gave it), not the game's: it exists already.
    u32 capture_every_ms = 2000;
    u32 capture_ring_high = 160 * 1024;
    /// While a pass that is not the last formats the records of its block, the writer gives the core up for a millisecond whenever
    /// this long has gone by since it last did (2026-11-10): the writer is above the network thread and the game's threads, and the
    /// formatting of a 64 KB block is 26 ms of CPU in one go on the console (0.40 ms a KB of text, measured on 479 windows of Fantasy
    /// Life), which every frame that came or went in that time waited out.
    u32 capture_yield_ms = 2;
};

class BeaconFold;

/// The calls of one kind into the file layer over a window of the statistics (LogFiles::TakeCardWindow).
struct CardCalls {
    u32 calls = 0;
    u32 bytes = 0;         ///< written (a flush carries none)
    u64 ticks = 0;         ///< the time in the file layer, all the calls together
    u32 longest_ms = 0;    ///< ... and of the longest of them
    u32 longest_bytes = 0; ///< ... which carried this much
};

/// What the writer asked of the SD card since the last time this was taken, call by call (2026-11-10: until then there was one sum of
/// ticks, and nobody could say whether the seconds of a pass were in the write or in the flush, in the capture or in the main log), and
/// how the capture kept out of the game's way.
struct CardWindow {
    CardCalls capture_write, capture_flush, main_write, main_flush;
    CardCalls other;       ///< rotations (a rename, the removal of the oldest block, the open of the new file)
    u32 slice_bytes = 0;   ///< the size the next write of the capture will have (64 KB, or 8 KB after a slow call)
    u32 passes = 0;        ///< passes of the capture that were not the last
    u32 forced = 0;        ///< ... of which a single small slice after 10 s of stepping aside
    u32 read_skips = 0;    ///< wakes at which the capture stepped aside for a call of the game to the file system (a read, an open, ...)
    u32 game_reads = 0;    ///< calls of the game to the file system that ended (as the writer's looks saw them; reads alone until 2026-11-11)
    u32 realigned = 0;     ///< times the count of the calls under way was put right (an end never counted)
    u32 rest_ms = 0;       ///< how long the passes told the capture to stay off the card, in all
    u32 yields = 0;        ///< times a pass gave the core up while formatting
    u32 beacons_same = 0;  ///< beacons counted and not written (azl/beacon_fold.hpp)
};

class LogFiles {
public:
    /// `dir` ends with a separator ("/3ds/3DSLink/"); `start_ms` is the time the plugin started (the lines show the
    /// time since).
    LogFiles(FileLayer* files, std::string dir, u64 start_ms, const LogFileLimits& limits = LogFileLimits());
    ~LogFiles();
    LogFiles(const LogFiles&) = delete;
    LogFiles& operator=(const LogFiles&) = delete;

    /// A line of the main log: timestamped and queued, never written here (any thread, small stacks included: the
    /// SD card is the writer thread's business). Only Log() calls it, and Log() does nothing at level 0. Never waits: a
    /// thread that is not the writer and finds the queue's lock busy gives the line up, counted; the writer says the count
    /// in the log (platform::MarkLogWriterThread).
    void Append(const char* line);

    /// Opens the files that are due: the main log (a large one from earlier launches is rotated at once) and, when
    /// the capture ring exists, the capture. Writes what was queued before. Does nothing at level 0.
    void Open();
    /// The queue into the main log. At level 0 nothing is ever queued and nothing is written.
    void Flush();
    /// The capture ring into the capture file. A pass that is not the last is ONE write - 64 KB of text, or 8 KB after a call the
    /// card took long over (azl/capture_pace.hpp; `small_slice`: 8 KB whatever the card did) - and one flush: the console has one
    /// file-system service, and a file read of the game waits behind the call in hand (2026-11-10; until then a pass was up to 256 KB,
    /// two writes of 64 KB and a flush on a slow card, and the game read one block of its files per pass). The main log's waiting
    /// lines are written before each write of the capture. `final`, at the end of the launch: all of it, write after write, however
    /// long it takes (BeginShutdown bounds it). Returns true when the pass left records in the ring; the pass also notes it, and how
    /// long the capture is to stay off the card now (NextCaptureTurn, WriterWaitMs).
    bool DrainDebug(bool final, bool small_slice = false);
    /// While `*flag` is not zero, a pass of the capture does not begin, with everything kept in the ring (`final` passes
    /// are not stopped). The plugin gives it the gate of its threads (azl/swap_gate.hpp): the writer must leave the card before an Old 3DS swaps the block out, and on a
    /// slow card (~40 KB/s) a second write of a pass would outlast what the swap can wait (a pass is one write since 2026-11-10).
    void SetStopCapture(const volatile u32* flag) { stop_capture_ = flag; }
    /// The svc hook's two counts of the game's calls to the file system, begun and ended (Sources/ctr/hook.cpp: with the log on only;
    /// every fs command since 2026-11-11, azl/game_card_calls.hpp - "reads" here and below is all of them). Without them
    /// (the PC tests that give none, a plugin whose hooks are out) the capture never steps aside for a read. `posts_skipped`: the gate's
    /// count of the posts it did not call (the swap of the plugin's block on an Old 3DS), asked at every look on the writer thread; the
    /// watch needs it to tell an end that was lost from a game that reads on end (pace::GameReadWatch::Look). None: no post is skipped.
    void SetGameReads(const u32* begun, const u32* done, u32 (*posts_skipped)() = nullptr) {
        reads_begun_ = begun;
        reads_done_ = done;
        posts_skipped_ = posts_skipped;
    }
    /// Is a pass of the capture due now? `ring_used` is how much the ring holds (DebugRingUsedBytes), `early` that something asks for it at
    /// once. See LogFileLimits::capture_every_ms. Only the cadence: NextCaptureTurn adds the rest of the card and the game's reads.
    bool CaptureDue(u64 now_ms, u32 ring_used, bool early) const;
    /// Remembers that a pass of the capture began at `now_ms` (DrainDebug does it; a test may too).
    void NoteCapturePass(u64 now_ms);
    /// What the writer does about the capture at this wake (it asks every time it wakes, the writer thread only):
    ///   - nothing while no pass is due (CaptureDue; a pass that left records in the ring makes the next one due);
    ///   - nothing while the card rests: after a pass the capture stays off the card for as long as the pass kept it, 20 ms at least
    ///     and 5 s at most (pace::RestAfterPass; "capture_not_before");
    ///   - nothing while the game is at its files (SetGameReads: a call to the file system under way, or one ended since the last
    ///     look), for 10 s of looks in a row at the most: then ONE small slice (kSmallPass);
    ///   - `early` (the HOME button) passes at once, whatever the card's rest and the game's reads say. So do the last pass of a launch
    ///     and a switch-off, which do not come through here (Close).
    enum class CaptureTurn : u8 { kWait = 0, kPass = 1, kSmallPass = 2 };
    CaptureTurn NextCaptureTurn(u64 now_ms, u32 ring_used, bool early);
    /// How long the writer sleeps before its next wake with the log on: 250 ms (the main log and the statistics want no less), sooner
    /// when the capture waits for the card's rest to end (20 ms at least) or stepped aside for a read of the game (50 ms).
    u32 WriterWaitMs(u64 now_ms) const;
    /// The time the capture stays off the card until (0: no pass yet).
    u64 capture_not_before_ms() const {
        return __atomic_load_n(&capture_not_before_ms_, __ATOMIC_RELAXED);
    }
    /// The calls to the card and the capture's manners since the last call (the writer's "stats: card:" line takes it every window).
    CardWindow TakeCardWindow();
    /// "stats: card: capture 2 writes 131072 B 9310 ms (longest 4810 ms for 65536 B), 1 flushes 35 ms (longest 35 ms) | main log ...":
    /// a line of its own, after the capture's (the lines the analysis scripts read keep their shape).
    static void FormatCardLine(const CardWindow& w, char* out, std::size_t cap);
    /// Writes what is left, closes the files and forgets the queue. The main log goes first; the capture's last drain is bounded
    /// by BeginShutdown() when the launch is ending (a switch-off from the menu has no such bound: it drains everything).
    void Close();

    /// The launch is ending (Plugin::Shutdown): from now on, for limits.exit_capture_ms, passes of the capture keep writing; after
    /// that every pass stops after the write in hand and leaves the rest in the ring, and Close() says how much was not written.
    /// Only the capture is bounded; the lines of the main log are never dropped for it.
    void BeginShutdown();

    /// Flush() that gives up after `within_ms` if the files are busy (the writer thread sits in a long write to the card): the
    /// lines go back to the front of the queue then. Returns true when nothing is left to write.
    bool FlushWithin(u32 within_ms);

    /// One line of the trail of a sleep (azl/sleep_crumbs.hpp) into AzaharLink.sleep.log, written and flushed at once, on the
    /// calling thread (the breadcrumb thread: nothing else). The file has its own handle and lock - nothing here waits for
    /// the writer's SD writes. A launch that finds it over 128 KB moves it aside as AzaharLink.sleep.1.log (the one before it
    /// goes) and starts a new one: the trail of the launch before is kept. Does nothing at level 0.
    void WriteCrumb(const std::string& line);
    /// The `count` lines of a turn of the breadcrumb thread, as WriteCrumb does one, in ONE write and ONE flush.
    void WriteCrumbs(const std::string* lines, std::size_t count);

    /// Switches the log on (1) or off (0) at run time. On: the capture ring is allocated when `capture` (the main log
    /// does not depend on it), the level is set, the files are opened. Off: the level is set first (nothing is opened
    /// or formatted from then on), then what is left is written and the files are closed. Returns false when the
    /// ring could not be allocated (the main log is on all the same).
    bool SetLevel(int level, bool capture);

    /// The console's wall clock in milliseconds since 1900 (osGetTime), for the first line of every capture file: the lines
    /// of a capture only have the time since the plugin started, and a file is read on its own. Optional (the PC tests
    /// have none: the line then has no clock).
    void SetConsoleClock(u64 (*now_os_ms)()) {
        console_clock_ = now_os_ms;
    }

    bool log_open() const {
        return log_file_ != nullptr;
    }
    bool debug_open() const {
        return debug_file_ != nullptr;
    }
    bool crumb_open() const {
        return crumb_file_ != nullptr;
    }

    /// The time (system ticks) the writes of the main log and the capture have spent in the file layer so far, rotations
    /// included: the writer thread waits for the SD card that long, which is not CPU. The writer takes the difference
    /// across a pass (Plugin: Stats::t_writer_wait) and charges the rest of the pass to t_writer. The trail of a sleep
    /// (WriteCrumb) is its own thread's and is not in it.
    u64 card_wait_ticks() const {
        return __atomic_load_n(&card_wait_ticks_, __ATOMIC_RELAXED);
    }
    /// The time (system ticks) the passes of the capture gave the core up while they formatted (LogFileLimits::capture_yield_ms):
    /// neither CPU nor the card. The writer takes the difference across a pass off what it charges as its CPU (Stats::t_writer).
    u64 yielded_ticks() const {
        return __atomic_load_n(&yielded_ticks_, __ATOMIC_RELAXED);
    }
    /// The lines of the main log dropped since the start because its queue was full (max_pending), as far as the writer has
    /// counted them (ReportBusyDrops, at every Flush, whose line says each count as it comes): for the stats line.
    u32 queue_full_total() const {
        return __atomic_load_n(&queue_full_total_, __ATOMIC_RELAXED);
    }

private:
    /// The kinds of call into the file layer that are timed one by one (CardWindow).
    enum CardSlot { kCardCaptureWrite = 0, kCardCaptureFlush = 1, kCardMainWrite = 2, kCardMainFlush = 3, kCardOther = 4, kCardSlots = 5 };
    /// One call into the file layer (file_mutex_ held): the time from its construction to its end goes to card_wait_ticks_ and to the
    /// counts of its kind, and a call of the capture tells the size of the next slice (pace::SliceSize).
    class CardCall {
    public:
        CardCall(LogFiles* owner, CardSlot slot, std::size_t bytes) : owner_(owner), slot_(slot), bytes_(bytes), began_(platform::NowTicks()) {}
        ~CardCall() {
            owner_->NoteCardCall(slot_, bytes_, platform::NowTicks() - began_);
        }
        CardCall(const CardCall&) = delete;
        CardCall& operator=(const CardCall&) = delete;

    private:
        LogFiles* owner_;
        CardSlot slot_;
        std::size_t bytes_;
        u64 began_;
    };
    void NoteCardCall(CardSlot slot, std::size_t bytes, u64 ticks);

    /// The two logs, each with its current file and its numbered blocks.
    enum Kind { kMain = 0, kDebug = 1 };
    /// The path of a file of `kind`: index 0 is the current file, N >= 1 the block AzaharLink[.debug].N.log.
    std::string PathOf(Kind kind, int index) const;
    std::string DebugPath(int index) const {
        return PathOf(kDebug, index);
    }
    /// The blocks that are there, found once by looking at the directory (file_mutex_ held). Afterwards the bounds are
    /// kept up to date by the rotations themselves.
    void ScanBlocksLocked(Kind kind);
    /// The current file of `kind` is closed and becomes the next block; the lowest blocks go while there are more than
    /// kept; the current file is not reopened here (file_mutex_ held).
    void RotateLocked(Kind kind);
    // file_mutex_ held:
    void OpenLogLocked();
    void OpenDebugLocked();
    // crumb_mutex_ held:
    void OpenCrumbLocked();
    void RotateLogLocked();
    void WriteMainLocked(const std::string& text); ///< file_mutex_ held
    void WriteDebug(const std::string& text);
    /// The lines other threads gave up on because a lock of the log was busy, and those dropped because the queue was full, said
    /// once in the log (the writer's Flush).
    void ReportBusyDrops();
    /// Append's work; `report` lets a line of ReportBusyDrops past a full queue.
    void AppendLine(const char* line, bool report);

    FileLayer* files_;
    std::string dir_;
    u64 start_ms_;
    LogFileLimits limits_;

    platform::Mutex log_mutex_;  ///< guards pending_
    platform::Mutex file_mutex_; ///< guards the files and their rotation (taken before log_mutex_ when both are)
    platform::Mutex drain_mutex_; ///< one pass of DrainDebug at a time (guards drain_out_; taken before file_mutex_)
    std::string pending_;
    u32 append_busy_ = 0; ///< lines Append() gave up on because log_mutex_ was busy (atomic adds; ReportBusyDrops clears)
    u32 queue_full_ = 0;  ///< lines Append() dropped because the queue held max_pending (atomic adds; ReportBusyDrops clears)
    u32 queue_full_total_ = 0; ///< ... all of them since the start (atomic adds, by ReportBusyDrops)
    void* log_file_ = nullptr;
    u64 log_size_ = 0;
    void* debug_file_ = nullptr;
    u64 debug_size_ = 0;
    /// The numbered blocks of each log: the lowest and the highest N there (0 = none), and whether they were looked for yet.
    int block_lo_[2] = {0, 0};
    int block_hi_[2] = {0, 0};
    bool blocks_scanned_[2] = {false, false};
    std::string drain_out_; ///< the capture text of one pass of the writer, kept between passes
    BeaconFold* beacons_ = nullptr; ///< the beacons counted instead of written (drain_mutex_ held; made by the first pass with the log on)
    u32 beacon_reset_ = 0;          ///< a capture file was opened: the table starts again at the next block (atomic accesses)
    u64 (*console_clock_)() = nullptr;
    platform::Mutex crumb_mutex_; ///< guards the trail's file (only ever taken by the breadcrumb thread, and Close)
    void* crumb_file_ = nullptr;
    Ticks64 card_wait_ticks_ = 0;
    Ticks64 yielded_ticks_ = 0; ///< see yielded_ticks()
    // The calls to the card of the window (atomic accesses: written with file_mutex_ held, taken by the writer's statistics without it).
    struct CardCount {
        u32 calls = 0;
        u32 bytes = 0;
        Ticks64 ticks = 0;
        u32 longest_ms = 0;
        u32 longest_bytes = 0;
    };
    CardCount card_[kCardSlots];
    u32 win_passes_ = 0, win_forced_ = 0, win_read_skips_ = 0, win_rest_ms_ = 0, win_yields_ = 0, win_beacons_ = 0; ///< (atomic accesses)
    pace::SliceSize slice_;        ///< the size of the capture's next write (drain_mutex_ held: every call of the capture is made under it)
    u32 slice_bytes_now_ = pace::kSliceLargeBytes; ///< ... as the statistics read it (atomic accesses)
    pace::GameReadWatch read_watch_; ///< the game's file reads as the writer's looks saw them (the writer thread only: NextCaptureTurn)
    const u32* reads_begun_ = nullptr; ///< see SetGameReads
    const u32* reads_done_ = nullptr;
    u32 (*posts_skipped_)() = nullptr;
    Ticks64 capture_not_before_ms_ = 0; ///< the capture stays off the card until then (atomic accesses; a pass sets it)
    u32 capture_backlog_ = 0;      ///< the last pass left records in the ring: the next is due (atomic accesses)
    u32 capture_waiting_ = 0;      ///< why the capture did not pass at the last look: 0 nothing due, 1 the card rests, 2 a read of the game (atomic accesses)
    u64 last_capture_ms_ = 0;      ///< when the last pass of the capture began (atomic accesses; 0: none yet, so the first is due at once)
    const volatile u32* stop_capture_ = nullptr; ///< see SetStopCapture
    u64 shutdown_deadline_ms_ = 0; ///< 0: the launch is not ending; else the time the capture stops writing (atomic accesses)
};

} // namespace azl
