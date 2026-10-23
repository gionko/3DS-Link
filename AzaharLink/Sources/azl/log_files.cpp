#include "azl/log_files.hpp"

#include <algorithm>
#include <cstdio>

#include "azl/stats.hpp"
#include "azl/wallclock.hpp"

namespace azl {

namespace {
constexpr const char* kLogName = "AzaharLink.log";
constexpr const char* kDebugName = "AzaharLink.debug.log";
constexpr const char* kCrumbName = "AzaharLink.sleep.log";
/// How many numbers in a row without a file end the look for blocks: more than are ever kept, so the look finds the highest
/// block whatever was deleted by hand below it, and stops soon after it.
constexpr int kBlockScanGap = 64;
constexpr u64 kCrumbMaxBytes = 128 * 1024; ///< a launch that finds the trail's file larger than this starts it afresh
constexpr std::size_t kQueueReserve = 16 * 1024; ///< bytes of main-log lines the queue holds before it has to grow
} // namespace

LogFiles::LogFiles(FileLayer* files, std::string dir, u64 start_ms, const LogFileLimits& limits)
    : files_(files), dir_(std::move(dir)), start_ms_(start_ms), limits_(limits) {}

LogFiles::~LogFiles() {
    platform::LockGuard lock(file_mutex_);
    if (log_file_) {
        files_->Close(log_file_);
    }
    if (debug_file_) {
        files_->Close(debug_file_);
    }
    platform::LockGuard crumb_lock(crumb_mutex_);
    if (crumb_file_) {
        files_->Close(crumb_file_);
    }
}

std::string LogFiles::PathOf(Kind kind, int index) const {
    if (index == 0) {
        return dir_ + (kind == kMain ? kLogName : kDebugName);
    }
    return dir_ + (kind == kMain ? "AzaharLink." : "AzaharLink.debug.") + std::to_string(index) + ".log";
}

void LogFiles::ScanBlocksLocked(Kind kind) {
    if (blocks_scanned_[kind]) {
        return;
    }
    blocks_scanned_[kind] = true;
    int lo = 0, hi = 0, gap = 0;
    for (int i = 1; gap < kBlockScanGap; ++i) {
        if (files_->Exists(PathOf(kind, i))) {
            if (lo == 0) {
                lo = i;
            }
            hi = i;
            gap = 0;
        } else {
            ++gap;
        }
    }
    block_lo_[kind] = lo;
    block_hi_[kind] = hi;
}

void LogFiles::RotateLocked(Kind kind) {
    void*& file = kind == kMain ? log_file_ : debug_file_;
    if (file) {
        files_->Close(file);
        file = nullptr;
    }
    ScanBlocksLocked(kind);
    // The current file becomes the block after the highest one (the blocks read in order: 1 is the oldest)...
    const int next = block_hi_[kind] + 1;
    files_->Rename(PathOf(kind, 0), PathOf(kind, next));
    block_hi_[kind] = next;
    if (block_lo_[kind] == 0) {
        block_lo_[kind] = next;
    }
    // ... and the lowest blocks go while there are more than kept (a number without a file, deleted by hand, counts as gone).
    while (block_hi_[kind] - block_lo_[kind] + 1 > limits_.keep) {
        files_->Remove(PathOf(kind, block_lo_[kind]));
        ++block_lo_[kind];
    }
}

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
void LogFiles::Append(const char* line) {
    // Called from any thread, including the game's own threads (small stacks): never touch the file system here,
    // just queue the line. The log writer thread puts it on the SD card.
    const u64 elapsed = platform::NowMs() - start_ms_;
    // "[%5lu.%03lu] ", made by hand: the game's threads get here (Log() from the hook) and a printf costs them ~1 KB
    // of stack (azl/line.hpp). The 32-bit division is a multiplication; the 64-bit one is a libgcc call.
    u32 seconds;
    u32 millis;
    if (elapsed >> 32 == 0) {
        seconds = static_cast<u32>(elapsed) / 1000;
        millis = static_cast<u32>(elapsed) % 1000;
    } else { // (more than 49 days into a session: never seen, kept exact)
        seconds = static_cast<u32>(elapsed / 1000);
        millis = static_cast<u32>(elapsed % 1000);
    }
    char prefix[24];
    LineBuf stamp(prefix, sizeof(prefix));
    stamp.Ch('[').DecPad(seconds, 5, ' ').Ch('.').DecPad(millis, 3, '0').Str("] ");
    // The queue's lock is the writer's too (Flush swaps the queue out under it): a thread above the writer never waits for
    // it (platform::MarkLogWriterThread), it tries briefly and gives the line up, counted; the writer may wait.
    if (!platform::TryLockBriefly(log_mutex_)) {
        if (!platform::IsLogWriterThread()) {
            __atomic_fetch_add(&append_busy_, 1u, __ATOMIC_RELAXED);
            return;
        }
        log_mutex_.lock();
    }
    if (pending_.size() < limits_.max_pending) {
        pending_.append(prefix, stamp.size());
        pending_ += line;
        pending_ += '\n';
    }
    log_mutex_.unlock();
}
// <<< game-thread code

// The two Open functions are the only places that create a file of the log, and both refuse at level 0: a pass of
// the writer that began before the log was switched off cannot bring a file back.
void LogFiles::OpenLogLocked() {
    if (log_file_ || g_debug_level <= 0) {
        return;
    }
    log_file_ = files_->OpenAppend(dir_ + kLogName);
    log_size_ = log_file_ ? files_->Size(log_file_) : 0;
}

void LogFiles::OpenDebugLocked() {
    if (debug_file_ || g_debug_level <= 0 || !DebugRingReady()) {
        return;
    }
    debug_file_ = files_->OpenAppend(DebugPath(0));
    debug_size_ = debug_file_ ? files_->Size(debug_file_) : 0;
    if (debug_file_) {
        // The first line of every file (a new launch appending to an old one, and every file after a rotation): when it
        // began by the console's clock. The lines of a capture only carry the time since the plugin started, and a file
        // is read on its own, without the main log or the others.
        u64 elapsed = platform::NowMs() - start_ms_;
        // The first file of a launch is opened seconds after the capture began (the card is not usable before): the ring
        // already holds records older than this moment, which the file gets after the line. It stands where the first of them
        // does, so the time column of the file never goes back.
        u32 oldest = 0;
        if (DebugOldestMs(&oldest)) {
            const u32 oldest_since_start = oldest - static_cast<u32>(start_ms_);
            if (oldest_since_start < elapsed) {
                elapsed = oldest_since_start;
            }
        }
        char line[128];
        int n = std::snprintf(line, sizeof(line), "%6lu.%03lu debug capture: this file begins here",
                              static_cast<unsigned long>(elapsed / 1000), static_cast<unsigned long>(elapsed % 1000));
        if (console_clock_ && n > 0 && n < static_cast<int>(sizeof(line)) - 40) {
            char stamp[24];
            FormatConsoleClock(console_clock_(), stamp);
            n += std::snprintf(line + n, sizeof(line) - n, " | console clock %s", stamp);
        }
        if (n > 0) {
            line[n++] = '\n';
            files_->Write(debug_file_, line, static_cast<std::size_t>(n));
            debug_size_ += static_cast<u64>(n);
        }
    }
}

// The third place that creates a file of the log, behind the same level check.
void LogFiles::OpenCrumbLocked() {
    if (crumb_file_ || g_debug_level <= 0) {
        return;
    }
    crumb_file_ = files_->OpenAppend(dir_ + kCrumbName);
    if (crumb_file_ && files_->Size(crumb_file_) >= kCrumbMaxBytes) {
        // (the trail of earlier launches is not what a test is after: start afresh rather than grow for ever)
        files_->Close(crumb_file_);
        files_->Remove(dir_ + kCrumbName);
        crumb_file_ = files_->OpenAppend(dir_ + kCrumbName);
    }
}

void LogFiles::WriteCrumb(const std::string& line) {
    if (g_debug_level <= 0) {
        return;
    }
    platform::LockGuard lock(crumb_mutex_);
    OpenCrumbLocked();
    if (!crumb_file_) {
        return;
    }
    const std::string text = line + '\n';
    files_->Write(crumb_file_, text.data(), text.size());
    files_->Flush(crumb_file_); // (at once: the trail is what is read after a freeze)
}

void LogFiles::RotateLogLocked() {
    // The current file becomes the next block, then a fresh one begins, with a line that says where the part before it went.
    RotateLocked(kMain);
    OpenLogLocked();
    if (log_file_) {
        char marker[96];
        const int n = std::snprintf(marker, sizeof(marker), "---- log rotated: the part before this is AzaharLink.%d.log ----\n",
                                    block_hi_[kMain]);
        if (n > 0 && n < static_cast<int>(sizeof(marker))) {
            files_->Write(log_file_, marker, static_cast<std::size_t>(n));
            files_->Flush(log_file_);
            log_size_ += static_cast<u64>(n);
        }
    }
}

void LogFiles::Open() {
    if (g_debug_level <= 0) {
        return;
    }
    {
        platform::LockGuard lock(file_mutex_);
        OpenLogLocked();
        if (log_file_ && log_size_ >= limits_.log_rotate_bytes) {
            RotateLogLocked(); // (a large file from earlier launches)
        }
        OpenDebugLocked();
    }
    {
        // The room for the lines of the game's threads is made here, on the thread that switches the log on (the start-up
        // thread, or the menu's), not by the first Flush that finds something to write: until then a line would make a game
        // thread grow the queue, and a malloc on a thread with a tiny stack is the same kind of cost as a printf.
        platform::LockGuard lock(log_mutex_);
        if (pending_.capacity() < kQueueReserve) {
            pending_.reserve(kQueueReserve);
        }
    }
    Flush();
}

void LogFiles::ReportBusyDrops() {
    const u32 busy = LogTakeBusyDrops() + __atomic_exchange_n(&append_busy_, 0u, __ATOMIC_RELAXED);
    if (busy != 0) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "log: %lu lines of other threads were given up: a lock of the log was busy (a thread above the writer never waits for it)",
                      static_cast<unsigned long>(busy));
        Append(line); // (a hole in the log is never silent; from the writer this never waits long, and never for a game thread)
    }
}

void LogFiles::Flush() {
    ReportBusyDrops();
    {
        platform::LockGuard lock(log_mutex_);
        if (pending_.empty()) {
            return;
        }
    }
    // The next lines come from any thread, game threads included: room for them, so that Append() does not allocate on a
    // thread with a small stack (it only does when more than this arrives between two flushes). Made OUTSIDE the lock: under
    // it (until 2026-10-20) a malloc was one more thing a thread above the writer found the lock busy for.
    std::string fresh;
    fresh.reserve(kQueueReserve);
    std::string text;
    {
        platform::LockGuard lock(log_mutex_);
        text.swap(pending_);
        pending_.swap(fresh);
    }
    if (text.empty()) {
        return;
    }
    platform::LockGuard lock(file_mutex_);
    WriteMainLocked(text);
}

void LogFiles::WriteMainLocked(const std::string& text) {
    if (!log_file_) {
        // The file system was not ready yet (or the SD card is read-only): keep the text. With the log off there is
        // nobody to keep it for.
        if (g_debug_level > 0) {
            platform::LockGuard pending_lock(log_mutex_);
            if (pending_.size() + text.size() <= limits_.max_pending) {
                pending_.insert(0, text);
            }
        }
        return;
    }
    CardTimer card(&card_wait_ticks_);
    files_->Write(log_file_, text.data(), text.size());
    files_->Flush(log_file_); // (every time: the main log is small, and its last lines are what a post-mortem needs)
    log_size_ += text.size();
    if (log_size_ >= limits_.log_rotate_bytes) {
        RotateLogLocked();
    }
}

bool LogFiles::FlushWithin(u32 within_ms) {
    {
        platform::LockGuard lock(log_mutex_);
        if (pending_.empty()) {
            return true;
        }
    }
    std::string fresh;
    fresh.reserve(kQueueReserve); // (outside the lock, as in Flush)
    std::string text;
    {
        platform::LockGuard lock(log_mutex_);
        text.swap(pending_);
        pending_.swap(fresh);
    }
    if (text.empty()) {
        return true;
    }
    // The writer thread may sit in a long write to the card, holding the file lock: wait for it a little, not for ever.
    const u64 end_ms = platform::NowMs() + within_ms;
    while (!file_mutex_.try_lock()) {
        if (platform::NowMs() >= end_ms) {
            platform::LockGuard lock(log_mutex_);
            if (pending_.size() + text.size() <= limits_.max_pending) {
                pending_.insert(0, text); // (before what came in meanwhile: the lines stay in order)
            }
            return false;
        }
        platform::SleepMs(5);
    }
    WriteMainLocked(text);
    file_mutex_.unlock();
    return true;
}

void LogFiles::BeginShutdown() {
    __atomic_store_n(&shutdown_deadline_ms_, platform::NowMs() + limits_.exit_capture_ms, __ATOMIC_RELAXED);
}

bool LogFiles::DrainDebug(bool final) {
    // One pass at a time: the writer thread and a switch-off (menu thread) may both come here.
    platform::LockGuard drain_lock(drain_mutex_);
    {
        // No file yet (the open failed at start-up, or a rotation could not reopen it): try again
        // and leave the records in the ring until it works, instead of discarding them.
        platform::LockGuard lock(file_mutex_);
        if (!debug_file_) {
            OpenDebugLocked();
        }
        if (!debug_file_) {
            return false; // (nothing to wait for: it is the file that is missing)
        }
    }
    // One buffer for every pass (kept between them: reserving it again every 250 ms was an allocation and a free for
    // nothing). A pass writes what it formatted every 64 KB, so the buffer stays at 68 KB (heap, and only with the log
    // on). Until 2026-10-20 the writes were 16 KB and each was flushed: four times the calls into the file system, and a
    // flush costs the card as much as a small write.
    constexpr std::size_t kWriteBytes = 64 * 1024;   // one write to the card: the most a "service call" of this pass is
    constexpr std::size_t kPassBytes = 256 * 1024;   // ... and the most text one pass of the writer puts on the card
    constexpr u64 kPassMs = 200;
    std::string& out = drain_out_;
    if (out.capacity() < kWriteBytes + 4096) {
        out.reserve(kWriteBytes + 4096);
    }
    // The counts that wait for their second to end (the diet) go into the ring ahead of what is drained.
    DebugFlushAggregates(final);
    const u64 begin_ms = platform::NowMs();
    std::size_t written = 0;
    DebugRecord rec;
    u8 buf[1700];
    bool more = true;
    bool cut = false; // this pass stopped at its bounds with records left
    while (more) {
        out.clear();
        more = false;
        while (DebugPop(&rec, buf, sizeof(buf))) {
            DebugFormat(rec, buf, static_cast<u32>(start_ms_), &out);
            if (out.size() >= kWriteBytes) {
                more = true; // write this much now, then go on
                break;
            }
        }
        // Never hog the writer thread - or the file lock, which every write holds: the rest on the next pass (the end of
        // the launch is the exception: that pass takes what it takes).
        if (more && !final && (written + out.size() >= kPassBytes || platform::NowMs() - begin_ms > kPassMs)) {
            more = false;
            cut = true;
        }
        if (!more) {
            const u32 dropped = DebugTakeDropped();
            if (dropped) {
                char line[128];
                // (the start of this line is what the analysis scripts count holes by; the LOST lines in the capture say where
                // and why - no room in the lane, or the ring's lock busy - so the reason here is only the two of them)
                std::snprintf(line, sizeof(line),
                              "debug: %lu records were dropped (no room in the ring, or its lock was busy: the LOST lines say which)\n",
                              static_cast<unsigned long>(dropped));
                out += line;
            }
        }
        if (out.empty()) {
            break;
        }
        written += out.size();
        WriteDebug(out);
        // The launch is ending (BeginShutdown) and the card cannot keep up: stop after the write in hand and leave the rest of the
        // ring - the game must be able to leave, and the main log's last lines must reach the file (Close() writes them first and
        // then says how much of the capture was not written). A switch-off from the menu has no deadline and drains everything.
        if (more) {
            const u64 deadline = __atomic_load_n(&shutdown_deadline_ms_, __ATOMIC_RELAXED);
            if (deadline != 0 && platform::NowMs() >= deadline) {
                cut = true;
                break;
            }
        }
    }
    // One flush for the whole pass (the file system holds the writes until then): a flush costs the card as much as a small
    // write, and sixteen of them a pass - one per 16 KB write, until 2026-10-20 - were a good part of what the capture asked
    // of the card the game reads from.
    if (written != 0) {
        platform::LockGuard lock(file_mutex_);
        if (debug_file_) {
            CardTimer card(&card_wait_ticks_);
            files_->Flush(debug_file_);
        }
    }
    return cut;
}

void LogFiles::WriteDebug(const std::string& text) {
    platform::LockGuard lock(file_mutex_);
    if (!debug_file_) {
        return; // the rotation could not reopen the file: the records are lost, the next pass retries
    }
    CardTimer card(&card_wait_ticks_);
    files_->Write(debug_file_, text.data(), text.size()); // (flushed once a pass: DrainDebug)
    debug_size_ += text.size();
    if (debug_size_ >= limits_.debug_rotate_bytes) {
        RotateLocked(kDebug);
        OpenDebugLocked();
    }
}

void LogFiles::Close() {
    Flush();
    if (DebugRingReady()) {
        DrainDebug(true);
        // At the end of a launch the drain stops at its deadline when the card is slow: say how much of the capture is missing, in
        // the main log, which is written first and last (a capture that ends without a word reads like a crash).
        const u32 left = DebugBytesQueued();
        if (left != 0 && __atomic_load_n(&shutdown_deadline_ms_, __ATOMIC_RELAXED) != 0) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "shutdown: the capture was cut at the end: %lu KB of it were not written (the card was too slow to take them in %lu ms)",
                          static_cast<unsigned long>((left + 1023) / 1024), static_cast<unsigned long>(limits_.exit_capture_ms));
            Append(line);
            Flush();
        }
    }
    // The trail's handle is closed only if its lock is free: the breadcrumb thread may sit in a write to the card (the freeze
    // it exists for), and the end of a launch has a second to spare, not more. Left open, the handle goes with the process.
    if (crumb_mutex_.try_lock()) {
        if (crumb_file_) {
            files_->Close(crumb_file_);
            crumb_file_ = nullptr;
        }
        crumb_mutex_.unlock();
    }
    platform::LockGuard lock(file_mutex_);
    if (debug_file_) {
        files_->Close(debug_file_);
        debug_file_ = nullptr;
    }
    if (log_file_) {
        files_->Close(log_file_);
        log_file_ = nullptr;
    }
    platform::LockGuard pending_lock(log_mutex_);
    pending_.clear();
}

bool LogFiles::SetLevel(int level, bool capture) {
    bool ring_ok = true;
    if (level >= 1) {
        if (capture) {
            ring_ok = DebugInit();
        }
        g_debug_level = 1;
        Open();
    } else {
        g_debug_level = 0;
        Close();
    }
    return ring_ok;
}

} // namespace azl
