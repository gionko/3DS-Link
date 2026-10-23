// The files of the log, and the queue that feeds them: AzaharLink.log (the main log) and AzaharLink.debug.log (the capture),
// both written in blocks: when the current file reaches its size it is closed and renamed AzaharLink.N.log /
// AzaharLink.debug.N.log with N one more than the highest N there (so the blocks read in order, 1 the oldest), and a fresh
// current file begins; when there are more blocks than kept, the lowest N goes. Nothing is ever started afresh at a launch:
// a launch appends to the current file. While the log is off (g_debug_level 0, see stats.hpp) nothing is created, opened or
// written, whatever calls in here. The plugin runs this class on top of CTRPluginFramework's File, the PC tests on top of a
// directory.
#pragma once

#include <string>

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
    /// the capture once a pass of the writer (DrainDebug), not at every 64 KB of it: a flush costs the card as much as a
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
    /// The capture ring into the capture file. A pass is bounded - 200 ms and 256 KB of text, in writes of 64 KB, flushed
    /// once at the end of the pass - so that
    /// the writer thread comes back to the main log and its other duties in time (`final`, at the end of the launch: all
    /// of it, however long it takes). Returns true when the pass stopped at its bounds with records left in the ring: the
    /// writer then comes back soon instead of after its usual pause.
    bool DrainDebug(bool final);
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
    /// the writer's SD writes - and is started afresh by a launch that finds it over 128 KB. Does nothing at level 0.
    void WriteCrumb(const std::string& line);

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

private:
    /// Adds the time from its construction to its end to card_wait_ticks_.
    class CardTimer {
    public:
        explicit CardTimer(Ticks64* sum) : sum_(sum), began_(platform::NowTicks()) {}
        ~CardTimer() {
            __atomic_fetch_add(sum_, platform::NowTicks() - began_, __ATOMIC_RELAXED);
        }
        CardTimer(const CardTimer&) = delete;
        CardTimer& operator=(const CardTimer&) = delete;

    private:
        Ticks64* sum_;
        u64 began_;
    };

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
    /// The lines other threads gave up on because a lock of the log was busy, said once in the log (the writer's Flush).
    void ReportBusyDrops();

    FileLayer* files_;
    std::string dir_;
    u64 start_ms_;
    LogFileLimits limits_;

    platform::Mutex log_mutex_;  ///< guards pending_
    platform::Mutex file_mutex_; ///< guards the files and their rotation (taken before log_mutex_ when both are)
    platform::Mutex drain_mutex_; ///< one pass of DrainDebug at a time (guards drain_out_; taken before file_mutex_)
    std::string pending_;
    u32 append_busy_ = 0; ///< lines Append() gave up on because log_mutex_ was busy (atomic adds; ReportBusyDrops clears)
    void* log_file_ = nullptr;
    u64 log_size_ = 0;
    void* debug_file_ = nullptr;
    u64 debug_size_ = 0;
    /// The numbered blocks of each log: the lowest and the highest N there (0 = none), and whether they were looked for yet.
    int block_lo_[2] = {0, 0};
    int block_hi_[2] = {0, 0};
    bool blocks_scanned_[2] = {false, false};
    std::string drain_out_; ///< the capture text of one pass of the writer, kept between passes
    u64 (*console_clock_)() = nullptr;
    platform::Mutex crumb_mutex_; ///< guards the trail's file (only ever taken by the breadcrumb thread, and Close)
    void* crumb_file_ = nullptr;
    Ticks64 card_wait_ticks_ = 0;
    u64 shutdown_deadline_ms_ = 0; ///< 0: the launch is not ending; else the time the capture stops writing (atomic accesses)
};

} // namespace azl
