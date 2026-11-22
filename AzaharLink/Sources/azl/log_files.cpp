#include "azl/log_files.hpp"

#include <algorithm>
#include <cstdio>
#include <new>

#include "azl/beacon_fold.hpp"
#include "azl/stats.hpp"
#include "azl/wallclock.hpp"

namespace azl {

namespace {
constexpr const char* kLogName = "AzaharLink.log";
constexpr const char* kDebugName = "AzaharLink.debug.log";
constexpr const char* kCrumbName = "AzaharLink.sleep.log";
constexpr const char* kCrumbOldName = "AzaharLink.sleep.1.log"; ///< the trail of the launches before, moved aside (OpenCrumbLocked)
/// How many numbers in a row without a file end the look for blocks: more than are ever kept, so the look finds the highest
/// block whatever was deleted by hand below it, and stops soon after it.
constexpr int kBlockScanGap = 64;
constexpr u64 kCrumbMaxBytes = 128 * 1024; ///< a launch that finds the trail's file larger than this moves it aside, starts a new one
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
    delete beacons_;
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
// `report`: a line that says what was dropped (ReportBusyDrops, the writer's): it goes in even with the queue full, one line past the
// limit, or the report of a full queue would itself be dropped and counted, and say "1 line" at the next flush.
__attribute__((always_inline)) inline void LogFiles::AppendLine(const char* line, bool report) {
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
    if (report || pending_.size() < limits_.max_pending) {
        pending_.append(prefix, stamp.size());
        pending_ += line;
        pending_ += '\n';
    } else {
        __atomic_fetch_add(&queue_full_, 1u, __ATOMIC_RELAXED); // (a hole in the log is never silent: ReportBusyDrops says it)
    }
    log_mutex_.unlock();
}

void LogFiles::Append(const char* line) {
    AppendLine(line, false);
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
        // (a file is read on its own: the first beacon of every transmitter in it is whole - azl/beacon_fold.hpp; the pass that is
        // under way starts its table again at its next block)
        __atomic_store_n(&beacon_reset_, 1u, __ATOMIC_RELAXED);
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
        // (not grown for ever, and not erased either: the trail of the launch before a crash is the one file that reached the card, so
        // the full file is moved aside as AzaharLink.sleep.1.log - the one before it goes - and this launch starts a new one)
        files_->Close(crumb_file_);
        files_->Remove(dir_ + kCrumbOldName);
        files_->Rename(dir_ + kCrumbName, dir_ + kCrumbOldName);
        crumb_file_ = files_->OpenAppend(dir_ + kCrumbName);
    }
}

void LogFiles::WriteCrumb(const std::string& line) {
    WriteCrumbs(&line, 1);
}

void LogFiles::WriteCrumbs(const std::string* lines, std::size_t count) {
    if (g_debug_level <= 0 || count == 0) {
        return;
    }
    platform::LockGuard lock(crumb_mutex_);
    OpenCrumbLocked();
    if (!crumb_file_) {
        return;
    }
    // (one write and one flush for all the lines of a turn of the breadcrumb thread, not one of each a line: a flush costs the card as
    // much as a small write, and the card is the game's too)
    std::size_t size = 0;
    for (std::size_t i = 0; i < count; ++i) {
        size += lines[i].size() + 1;
    }
    std::string text;
    text.reserve(size);
    for (std::size_t i = 0; i < count; ++i) {
        text += lines[i];
        text += '\n';
    }
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
    const u32 full = __atomic_exchange_n(&queue_full_, 0u, __ATOMIC_RELAXED);
    if (full != 0) {
        __atomic_fetch_add(&queue_full_total_, full, __ATOMIC_RELAXED);
        char line[160];
        std::snprintf(line, sizeof(line), "log: %lu lines were dropped: the queue of the main log was full (%lu KB waiting for the card)",
                      static_cast<unsigned long>(full), static_cast<unsigned long>(limits_.max_pending / 1024));
        AppendLine(line, true);
    }
    const u32 busy = LogTakeBusyDrops() + __atomic_exchange_n(&append_busy_, 0u, __ATOMIC_RELAXED);
    if (busy != 0) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "log: %lu lines of other threads were given up: a lock of the log was busy (a thread above the writer never waits for it)",
                      static_cast<unsigned long>(busy));
        AppendLine(line, true); // (a hole in the log is never silent; from the writer this never waits long, and never for a game thread)
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
    {
        CardCall card(this, kCardMainWrite, text.size());
        files_->Write(log_file_, text.data(), text.size());
    }
    {
        CardCall card(this, kCardMainFlush, 0);
        files_->Flush(log_file_); // (every time: the main log is small, and its last lines are what a post-mortem needs)
    }
    log_size_ += text.size();
    if (log_size_ >= limits_.log_rotate_bytes) {
        CardCall card(this, kCardOther, 0);
        RotateLogLocked();
    }
}

// One call into the file layer ended (CardCall; file_mutex_ held, so one at a time). The counts are read and cleared by the writer's
// statistics with no lock, a field at a time: a call that ends meanwhile is in this window or in the next.
void LogFiles::NoteCardCall(CardSlot slot, std::size_t bytes, u64 ticks) {
    __atomic_fetch_add(&card_wait_ticks_, ticks, __ATOMIC_RELAXED);
    CardCount& c = card_[slot];
    const u64 ms = azl_ticks_to_ms(ticks);
    const u32 shown_ms = ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(ms);
    __atomic_fetch_add(&c.calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c.bytes, static_cast<u32>(bytes), __ATOMIC_RELAXED);
    __atomic_fetch_add(&c.ticks, ticks, __ATOMIC_RELAXED);
    if (shown_ms >= __atomic_load_n(&c.longest_ms, __ATOMIC_RELAXED)) {
        __atomic_store_n(&c.longest_ms, shown_ms, __ATOMIC_RELAXED);
        __atomic_store_n(&c.longest_bytes, static_cast<u32>(bytes), __ATOMIC_RELAXED);
    }
    if (slot == kCardCaptureWrite || slot == kCardCaptureFlush) {
        // (every call of the capture is made by a pass, under drain_mutex_: the size of the slice is the pass's own)
        slice_.NoteCall(ms);
        __atomic_store_n(&slice_bytes_now_, slice_.Bytes(), __ATOMIC_RELAXED);
    }
}

CardWindow LogFiles::TakeCardWindow() {
    CardWindow w;
    CardCalls* const out[kCardSlots] = {&w.capture_write, &w.capture_flush, &w.main_write, &w.main_flush, &w.other};
    for (int i = 0; i < kCardSlots; ++i) {
        out[i]->calls = __atomic_exchange_n(&card_[i].calls, 0u, __ATOMIC_RELAXED);
        out[i]->bytes = __atomic_exchange_n(&card_[i].bytes, 0u, __ATOMIC_RELAXED);
        out[i]->ticks = __atomic_exchange_n(&card_[i].ticks, u64(0), __ATOMIC_RELAXED);
        out[i]->longest_ms = __atomic_exchange_n(&card_[i].longest_ms, 0u, __ATOMIC_RELAXED);
        out[i]->longest_bytes = __atomic_exchange_n(&card_[i].longest_bytes, 0u, __ATOMIC_RELAXED);
    }
    w.slice_bytes = __atomic_load_n(&slice_bytes_now_, __ATOMIC_RELAXED);
    w.passes = __atomic_exchange_n(&win_passes_, 0u, __ATOMIC_RELAXED);
    w.forced = __atomic_exchange_n(&win_forced_, 0u, __ATOMIC_RELAXED);
    w.read_skips = __atomic_exchange_n(&win_read_skips_, 0u, __ATOMIC_RELAXED);
    w.rest_ms = __atomic_exchange_n(&win_rest_ms_, 0u, __ATOMIC_RELAXED);
    w.yields = __atomic_exchange_n(&win_yields_, 0u, __ATOMIC_RELAXED);
    w.beacons_same = __atomic_exchange_n(&win_beacons_, 0u, __ATOMIC_RELAXED);
    // (the watch of the game's reads is the writer thread's own, and so is this call: the statistics run on it)
    w.game_reads = read_watch_.reads;
    w.realigned = read_watch_.realigned;
    read_watch_.reads = 0;
    read_watch_.realigned = 0;
    return w;
}

void LogFiles::FormatCardLine(const CardWindow& w, char* out, std::size_t cap) {
    const auto ms = [](const CardCalls& c) { return static_cast<unsigned long>(azl_ticks_to_ms(c.ticks)); };
    const auto ul = [](u32 v) { return static_cast<unsigned long>(v); };
    std::snprintf(out, cap,
                  "stats: card: capture %lu writes %lu B %lu ms (longest %lu ms for %lu B), %lu flushes %lu ms (longest %lu ms)"
                  " | main log %lu writes %lu B %lu ms (longest %lu ms for %lu B), %lu flushes %lu ms (longest %lu ms)"
                  " | other %lu calls %lu ms | slice %lu KB, %lu passes (%lu forced small), %lu skipped for the game's file calls"
                  " (%lu calls, %lu realigned), rest %lu ms, %lu yields, %lu beacons counted not written",
                  ul(w.capture_write.calls), ul(w.capture_write.bytes), ms(w.capture_write), ul(w.capture_write.longest_ms),
                  ul(w.capture_write.longest_bytes), ul(w.capture_flush.calls), ms(w.capture_flush), ul(w.capture_flush.longest_ms),
                  ul(w.main_write.calls), ul(w.main_write.bytes), ms(w.main_write), ul(w.main_write.longest_ms),
                  ul(w.main_write.longest_bytes), ul(w.main_flush.calls), ms(w.main_flush), ul(w.main_flush.longest_ms),
                  ul(w.other.calls), ms(w.other), ul(w.slice_bytes / 1024), ul(w.passes), ul(w.forced), ul(w.read_skips),
                  ul(w.game_reads), ul(w.realigned), ul(w.rest_ms), ul(w.yields), ul(w.beacons_same));
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

bool LogFiles::CaptureDue(u64 now_ms, u32 ring_used, bool early) const {
    const u64 last = __atomic_load_n(&last_capture_ms_, __ATOMIC_RELAXED);
    return early || last == 0 || ring_used >= limits_.capture_ring_high || now_ms - last >= limits_.capture_every_ms;
}

void LogFiles::NoteCapturePass(u64 now_ms) {
    __atomic_store_n(&last_capture_ms_, now_ms == 0 ? 1 : now_ms, __ATOMIC_RELAXED);
}

LogFiles::CaptureTurn LogFiles::NextCaptureTurn(u64 now_ms, u32 ring_used, bool early) {
    if (!DebugRingReady()) {
        __atomic_store_n(&capture_waiting_, 0u, __ATOMIC_RELAXED);
        return CaptureTurn::kWait; // (no capture: nothing to pace)
    }
    const bool due = __atomic_load_n(&capture_backlog_, __ATOMIC_RELAXED) != 0 || CaptureDue(now_ms, ring_used, early);
    const bool rested = early || now_ms >= __atomic_load_n(&capture_not_before_ms_, __ATOMIC_RELAXED);
    const bool wanted = due && rested;
    // The game's calls to the file system (reads, opens, closes ...: azl/game_card_calls.hpp), looked at every time the writer wakes (the
    // two words of the svc hook; `done` first: capture_pace.hpp).
    // The gate's count of skipped posts is read BEFORE the two (2026-11-11): read after them, a read that began and lost its end in
    // between showed its skipped post beside counts that did not hold it yet (GameReadWatch::Look).
    pace::GameReadWatch::Verdict reads = pace::GameReadWatch::kPass;
    if (reads_begun_ && reads_done_) {
        const u32 skipped = posts_skipped_ ? posts_skipped_() : 0u;
        const u32 done = __atomic_load_n(reads_done_, __ATOMIC_RELAXED);
        const u32 begun = __atomic_load_n(reads_begun_, __ATOMIC_RELAXED);
        reads = read_watch_.Look(begun, done, now_ms, wanted && !early, skipped);
    }
    if (!wanted) {
        __atomic_store_n(&capture_waiting_, due ? 1u : 0u, __ATOMIC_RELAXED); // (due, but the card rests)
        return CaptureTurn::kWait;
    }
    __atomic_store_n(&capture_waiting_, 0u, __ATOMIC_RELAXED);
    if (early) {
        read_watch_.PassedAnyway(); // (the HOME button: the capture goes to the card now, whoever reads)
        return CaptureTurn::kPass;
    }
    if (reads == pace::GameReadWatch::kSkip) {
        // The game is at its files: its call (a read, an open ...) would wait behind our write for as long as the card takes. Not now; a
        // look again soon.
        __atomic_fetch_add(&win_read_skips_, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&capture_waiting_, 2u, __ATOMIC_RELAXED);
        return CaptureTurn::kWait;
    }
    if (reads == pace::GameReadWatch::kForced) {
        __atomic_fetch_add(&win_forced_, 1u, __ATOMIC_RELAXED);
        return CaptureTurn::kSmallPass;
    }
    return CaptureTurn::kPass;
}

u32 LogFiles::WriterWaitMs(u64 now_ms) const {
    const u32 waiting = __atomic_load_n(&capture_waiting_, __ATOMIC_RELAXED);
    if (waiting == 2) {
        return pace::kReadPollMs;
    }
    if (waiting == 1 || __atomic_load_n(&capture_backlog_, __ATOMIC_RELAXED) != 0) {
        // The capture has something for the card and waits for its rest to end: wake then, and no later than the usual wake (the main
        // log and the statistics do not wait for the capture's rest).
        const u64 not_before = __atomic_load_n(&capture_not_before_ms_, __ATOMIC_RELAXED);
        const u64 left = not_before > now_ms ? not_before - now_ms : 0;
        return left < pace::kRestMinMs ? pace::kRestMinMs : left > pace::kWakeMs ? pace::kWakeMs : static_cast<u32>(left);
    }
    return pace::kWakeMs;
}

bool LogFiles::DrainDebug(bool final, bool small_slice) {
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
            __atomic_store_n(&capture_backlog_, 0u, __ATOMIC_RELAXED); // (no coming back soon for a file that is not there)
            return false; // (nothing to wait for: it is the file that is missing)
        }
    }
    if (!final && stop_capture_ && __atomic_load_n(stop_capture_, __ATOMIC_ACQUIRE) != 0) {
        __atomic_store_n(&capture_backlog_, 1u, __ATOMIC_RELAXED);
        return true; // (SetStopCapture: the writer comes back soon, once it may)
    }
    NoteCapturePass(platform::NowMs());
    // One buffer for every pass (kept between them: reserving it again every 250 ms was an allocation and a free for
    // nothing), as large as the largest write: 68 KB of heap, and only with the log on. ONE write a pass that is not the last
    // (2026-11-10): the file system serves one call at a time, a file read of the game waits behind the write in hand, and a
    // pass of two writes of 64 KB and a flush on a slow card was 9 s of that (until 2026-10-20 the writes were 16 KB and each
    // was flushed: four times the calls, and a flush costs the card as much as a small write). The write is 64 KB while the
    // card answers and 8 KB after a call it took long over (pace::SliceSize); the last pass of a launch writes slice after slice.
    std::string& out = drain_out_;
    if (out.capacity() < pace::kSliceLargeBytes + 4096) {
        out.reserve(pace::kSliceLargeBytes + 4096);
    }
    if (!beacons_) {
        beacons_ = new (std::nothrow) BeaconFold(); // (the plugin's heap, with the log on only; without it every beacon is written)
    }
    // The counts that wait for their second to end (the diet) go into the ring ahead of what is drained.
    DebugFlushAggregates(final);
    const u32 start_ms = static_cast<u32>(start_ms_);
    // The gate closed after this pass began (an Old 3DS is about to swap the plugin's block out, and waits for the writer to leave the
    // card): the pass is to end as soon as it can - see the two places below (2026-11-11). Never the last pass, which is not stopped.
    const auto gate_closing = [this, final]() { return !final && stop_capture_ && __atomic_load_n(stop_capture_, __ATOMIC_ACQUIRE) != 0; };
    const u64 card_before = card_wait_ticks();
    const u64 yield_ticks = static_cast<u64>(limits_.capture_yield_ms) * AZL_TICKS_PER_MS;
    u64 paused_at = platform::NowTicks();
    u32 yields = 0;
    u64 yielded = 0; // ticks given up: not the writer's CPU (yielded_ticks())
    std::size_t written = 0;
    DebugRecord rec;
    u8 buf[1700];
    bool cut = false; // this pass stopped with records left
    for (;;) {
        out.clear();
        const std::size_t slice_bytes = small_slice ? pace::kSliceSmallBytes : slice_.Bytes(); // (the write before may have changed it)
        if (beacons_ && __atomic_exchange_n(&beacon_reset_, 0u, __ATOMIC_RELAXED) != 0) {
            beacons_->Reset(); // (a new file: its first beacons are whole)
        }
        bool more = false;
        while (DebugPop(&rec, buf, sizeof(buf))) {
            // A beacon that is the one before of its transmitter is counted, not written (azl/beacon_fold.hpp).
            if (!beacons_ || beacons_->Keep(rec, buf, start_ms, &out)) {
                DebugFormat(rec, buf, start_ms, &out);
            }
            if (out.size() >= slice_bytes) {
                more = true; // write this much now
                break;
            }
            // The writer is above the network thread and the game's: it does not format a whole block in one go (LogFileLimits::
            // capture_yield_ms). Never in the last pass: the end of a launch has half a second. And no more of it once the gate of the
            // swap closes: the swap waits for this pass, and a sleep here is at least a millisecond of that wait, often more.
            if (!final && platform::NowTicks() - paused_at > yield_ticks && !gate_closing()) {
                const u64 pause_from = platform::NowTicks();
                platform::SleepMs(1);
                paused_at = platform::NowTicks();
                yielded += paused_at - pause_from;
                ++yields;
            }
        }
        if (beacons_) {
            beacons_->Summaries(start_ms, &out); // (before the write: a rotation after it finds nothing counted and not said)
        }
        // What the ring dropped is said by every pass that is not the last, and by the last one when it has emptied the ring
        // (2026-11-11: a pass that is not the last is ONE block, and with a capture the card cannot keep up with - the very case - it
        // never empties the ring: said only then, the count stayed in memory for the whole match and was lost with a game that fell).
        if (!more || !final) {
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
        // The main log first: its lines are few and they are what a post-mortem reads; they do not wait in memory behind a
        // write of the capture that may take seconds (2026-11-10). Not with the gate of the swap closing: that is a write and a flush
        // more for the swap to wait for; the lines stay in the queue and the writer's next round writes them, once the gate is open.
        if (!gate_closing()) {
            Flush();
        }
        written += out.size();
        WriteDebug(out);
        if (!more) {
            break;
        }
        if (!final) {
            cut = DebugBytesQueued() != 0; // one write a pass: the rest of the ring on the next, after the card has rested
            break;
        }
        // The launch is ending (BeginShutdown) and the card cannot keep up: stop after the write in hand and leave the rest of the
        // ring - the game must be able to leave, and the main log's last lines must reach the file (Close() writes them first and
        // then says how much of the capture was not written). A switch-off from the menu has no deadline and drains everything.
        const u64 deadline = __atomic_load_n(&shutdown_deadline_ms_, __ATOMIC_RELAXED);
        if (deadline != 0 && platform::NowMs() >= deadline) {
            cut = true;
            break;
        }
    }
    // One flush for the whole pass (the file system holds the writes until then): a flush costs the card as much as a small
    // write, and sixteen of them a pass - one per 16 KB write, until 2026-10-20 - were a good part of what the capture asked
    // of the card the game reads from.
    if (written != 0) {
        platform::LockGuard lock(file_mutex_);
        if (debug_file_) {
            CardCall card(this, kCardCaptureFlush, 0);
            files_->Flush(debug_file_);
        }
    }
    // The card rests for as long as this pass kept it (its writes, its flush and the main log's lines it wrote first): the game reads
    // from the same card, and the capture is at it for half of the time at the most (a pass of more than 5 s rests 5 s). The next
    // pass is not before then.
    const u32 rest_ms = pace::RestAfterPass(azl_ticks_to_ms(card_wait_ticks() - card_before));
    __atomic_store_n(&capture_not_before_ms_, platform::NowMs() + rest_ms, __ATOMIC_RELAXED);
    __atomic_store_n(&capture_backlog_, cut ? 1u : 0u, __ATOMIC_RELAXED);
    if (!final) {
        __atomic_fetch_add(&win_passes_, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&win_rest_ms_, rest_ms, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&win_yields_, yields, __ATOMIC_RELAXED);
    __atomic_fetch_add(&yielded_ticks_, yielded, __ATOMIC_RELAXED);
    if (beacons_) {
        __atomic_fetch_add(&win_beacons_, beacons_->TakeFolded(), __ATOMIC_RELAXED);
    }
    return cut;
}

void LogFiles::WriteDebug(const std::string& text) {
    platform::LockGuard lock(file_mutex_);
    if (!debug_file_) {
        return; // the rotation could not reopen the file: the records are lost, the next pass retries
    }
    {
        CardCall card(this, kCardCaptureWrite, text.size());
        files_->Write(debug_file_, text.data(), text.size()); // (flushed once a pass: DrainDebug)
    }
    debug_size_ += text.size();
    if (debug_size_ >= limits_.debug_rotate_bytes) {
        CardCall card(this, kCardOther, 0);
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
