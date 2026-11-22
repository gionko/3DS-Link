#include "dlp_guest.hpp"

#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#include "../titleshare_host/delivery.h"
#include "azl/dlp_children.hpp"
#include "copyworker.hpp"
#include "delivery.hpp"
#include "dlp_guest_am.hpp"
#include "dlp_guest_ids.hpp"
#include "dlp_guest_paths.hpp"
#include "dlp_guest_source.hpp"
#include "fileio.hpp"
#include "heap_plan.hpp"
#include "titles.hpp"
#include "ui.hpp"

namespace dlpguest {

namespace {

// The worker's thread: it reads and hashes, and calls into the module's stream (route K): the stack of dlp_prepare.cpp's worker. Every big buffer is on the
// heap (Composed, the stream's chunk, route K's scratch).
constexpr u32 kWorkerStack = 256 * 1024;
constexpr std::size_t kChunkBytes = 256 * 1024;
// What a run needs of the heap besides its own buffers: the module's stream (about 400 KB), the stack, boot9.bin while the keys are made (64 KB).
constexpr u32 kRunSlackBytes = 1u << 20;
constexpr u64 kFolderWaitMs = 60 * 1000; // a copy of the plugin takes a second or two; a card that hangs is given a minute
constexpr std::size_t kLogLines = 400;
constexpr std::size_t kLogBytes = 64u << 10;
constexpr std::size_t kLogReadBytes = 128u << 10;
constexpr const char* kLogName = "3DSLink-dlp-guests.log";
// A run of looks only (Games reads its games, one Probe each): the log is written every few games and at the end, not after each one (the file is read
// and written again whole: a second a few games on a slow card - the review of 2026-11-17 evening).
constexpr std::size_t kProbeFlushJobs = 8;

u64 Ms() {
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

// ---- the run ----------------------------------------------------------------------------------------------------------------------

struct Run {
    std::vector<Planned> jobs;
    std::vector<Waiting> waiting;
    std::string boot9_path;
    std::string plugin_file;
    std::string log_path;
    u8 language = 1;
    u32 planned_seconds = 0;
    u32 keys_stamp = 0;

    Thread thread = nullptr;
    std::atomic<bool> finished{false};
    std::atomic<bool> stop_after{false};
    std::atomic<bool> stop_now{false};
    std::atomic<bool> stopped_by_user{false}; // (the build 2h, U2: stop_now came from B on the run page, not from the app's end)
    LightLock lock; // guards what follows
    int index = 0;
    int stage = kStageRead;
    u64 done = 0, total = 0;
    u64 started_ms = 0;
    std::vector<JobEnd> ends;
    std::vector<std::string> log;
};

std::unique_ptr<Run> g_run;
int g_runs_ended = 0; // (TakeEnds: RunsEnded)
// (the build 2f) the system refused the hidden form once in this session (RunIn::hidden_refused): the next children go to the old form at once.
// Written by the run's thread, read by the next run's: one run at a time.
std::atomic<bool> g_hidden_refused{false};

void JoinLook();       // (the look of Download Play > Enable games, at the end of the file)
bool LookUnderWay();

// The lines of the log: kept under the lock while a job runs, written after it (a few writes of a small file per job, never one per step).
void LogLine(Run* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void LogLine(Run* r, const char* fmt, ...) {
    char line[200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    LightLock_Lock(&r->lock);
    r->log.push_back(line);
    LightLock_Unlock(&r->lock);
}

// Lines added at the end of the log of the children, each with the time (the file is read whole and the last kLogLines kept). The run (FlushLog) and
// the look (LogHomeIconsOnce) write it, never both at once: StartRun does not start while a look is under way, StartGamesLook not while a run is.
void AppendLog(const std::string& path, const std::vector<std::string>& lines) {
    if (lines.empty() || path.empty()) {
        return;
    }
    char stamp[96];
    if (heap_plan::FormatStamp(static_cast<int64_t>(std::time(nullptr)), static_cast<uint32_t>(Ms()), stamp, sizeof(stamp)) <= 0) {
        stamp[0] = '\0';
    }
    std::string text;
    if (setup::ReadTextFile(path, kLogReadBytes, text) == setup::LoadResult::Unreadable) {
        return; // (a file that could not be read is not written over)
    }
    for (const std::string& l : lines) {
        text = heap_plan::KeepLast(text, std::string(stamp) + l, kLogLines, kLogBytes);
    }
    setup::WriteTextFile(path, text);
}

void FlushLog(Run* r) {
    std::vector<std::string> lines;
    LightLock_Lock(&r->lock);
    lines.swap(r->log);
    LightLock_Unlock(&r->lock);
    AppendLog(r->log_path, lines);
}

// The icon of a prepared title read by {tid, SD}, the way HOME is believed to read it (the build 2e, 2026-11-18, analysis/piano-1.1.4-2026-11-15/
// home-figlio-2026-11-18/CAUSA-E-RIMEDIO.md 3.2): FSUSER_OpenFileDirectly on ARCHIVE_SAVEDATA_AND_CONTENT with the title on the SD card, the ExeFS's
// file "icon" (FBI_reference/FBI/source/fbi/task/listtitles.c:30-36, as titles.cpp reads the icons of the scan). HOME's own way is NOT known (its
// cache of the icons, its rights, its FS session are not 3DS Link's): an open rc != 0 says that way fails; rc 0 with magic SMDH does NOT prove that
// HOME reads the icon - it leaves the question of the empty place open. One line of the log of the children: the
// open's Result, the read's Result and bytes, the magic, the SMDH's flags at 0x2028 (bit 0: visible in HOME). A diagnostic only: nothing depends on
// it. The icon's buffer is on the heap and goes with the line. A worker's thread only (the run's, the look's): never the thread of the screen.
// The build 2f: `am` is the id AM has the title by (AmTitleOf: 000400010F8... in the hidden form), the one {tid, SD} names.
std::string HomeIconLine(const char* when, u64 am) {
    const u32 archive_path[4] = {static_cast<u32>(am & 0xFFFFFFFFu), static_cast<u32>(am >> 32), MEDIATYPE_SD, 0};
    static const u32 kIconPath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000}; // the ExeFS, "icon" with its zeros (8 bytes)
    const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
    const FS_Path file = {PATH_BINARY, sizeof(kIconPath), kIconPath};
    char line[200];
    Handle handle = 0;
    const Result open_rc = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0);
    if (R_FAILED(open_rc)) {
        std::snprintf(line, sizeof(line), "guest: home icon %016llX (%s): open rc=0x%08lX", static_cast<unsigned long long>(am), when,
                      static_cast<unsigned long>(open_rc));
        return line;
    }
    std::unique_ptr<u8[]> icon(new (std::nothrow) u8[kSmdhSize]);
    if (!icon) {
        FSFILE_Close(handle);
        std::snprintf(line, sizeof(line), "guest: home icon %016llX (%s): open rc=0x%08lX, no memory to read it", static_cast<unsigned long long>(am),
                      when, static_cast<unsigned long>(open_rc));
        return line;
    }
    std::memset(icon.get(), 0, kSmdhSize);
    u32 got = 0;
    const Result read_rc = FSFILE_Read(handle, &got, 0, icon.get(), kSmdhSize);
    FSFILE_Close(handle);
    char magic[5] = {};
    for (int i = 0; i < 4; ++i) {
        const u8 c = icon[i];
        magic[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
    }
    const u32 flags = got >= kSmdhFlagsAt + 4 ? LoadLe32(icon.get() + kSmdhFlagsAt) : 0;
    std::snprintf(line, sizeof(line), "guest: home icon %016llX (%s): open rc=0x%08lX, read rc=0x%08lX %lu bytes, magic %s, flags 0x%08lX",
                  static_cast<unsigned long long>(am), when, static_cast<unsigned long>(open_rc), static_cast<unsigned long>(read_rc),
                  static_cast<unsigned long>(got), magic, static_cast<unsigned long>(flags));
    return line;
}

// (the build 2h) the name of a form in the log of the children (DPAPP-2j: M3c1 is the hidden form made, the system form the build 2h's)
const char* FormName(u8 form) {
    return form == kFormSystem ? "system (build 2h)" : (form == kFormDlpChild ? "hidden, M3c1" : "old");
}

const char* ActName(Act a) {
    switch (a) {
    case Act::Probe: return "probe";
    case Act::Prepare: return "prepare";
    case Act::Check: return "check";
    case Act::Repair: return "repair";
    case Act::Remove: return "remove";
    case Act::Adopt: return "adopt";
    case Act::Disown: return "disown";
    default: return "nothing";
    }
}

// The system as the jobs see it (dlp_guest_run.hpp).
struct Console final : WorkConsole {
    Run* run = nullptr;
    const Keys* keys = nullptr;
    ContentSource source;
    Handle install = 0;

    int OpenContent(u64 game, bool card, Route* route) override {
        const int rc = source.Open(game, card, keys, route);
        LogLine(run, "guest: source of %016llX: route F 0x%08lX, route K %d -> %d", static_cast<unsigned long long>(game),
                static_cast<unsigned long>(source.route_f()), source.route_k(), rc);
        return rc;
    }
    bool ReadContent(u64 off, u8* buf, u32 len) override {
        return source.Read(off, buf, len);
    }
    void CloseContent() override {
        source.Close();
    }
    bool CardHolds(u64 game) override {
        return CardSlotHolds(game);
    }
    Presence Installed(u64 prepared, u8 form, u16* version) override {
        return PreparedPresence(prepared, form, version);
    }
    bool Product(u64 prepared, u8 form, char* out) override {
        return PreparedProduct(prepared, form, out);
    }
    bool FreeBytes(u64* free) override {
        return FreeSdBytes(free);
    }
    RunResult BeginInstall(u64 prepared, u8 form, bool ticket_expected, bool title_expected) override {
        return dlpguest::BeginInstall(prepared, form, ticket_expected, title_expected, &install);
    }
    RunResult Install(u64 off, const u8* buf, u32 len) override {
        if (install == 0) {
            return static_cast<RunResult>(kResultShortWrite); // (no install begun: nothing was taken)
        }
        const Result rc = WriteInstall(install, off, buf, len);
        return R_FAILED(rc) ? rc : 0; // (DPAPP-2j, R-D: AM's or FS's own Result, up to the line of the log)
    }
    RunResult FinishInstall() override {
        const Result rc = dlpguest::FinishInstall(install);
        install = 0; // (AM closed it, whatever came of it)
        return rc;
    }
    void CancelInstall(u64 prepared, u8 form) override {
        dlpguest::CancelInstall(install, prepared, form);
        install = 0;
    }
    RunResult DeleteTitle(u64 prepared, u8 form, bool drop_ticket) override {
        DeleteReport r;
        const Result rc = DeletePrepared(prepared, form, drop_ticket, &r);
        // (the build 2f) every Result of the removal: AM_DeleteTitle, AM_DeleteUserTitle (the old form only, after a failure), the ticket, the
        // pending part, and whether AM still lists it; the build 2h: the ticket skipped (it may not be ours, R7), the guard of the ids that refused
        if (r.guard_refused) {
            LogLine(run, "guest: delete %016llX (%s form): REFUSED by the guard of the ids (NAND, a process, the card, or a list not read): nothing asked of AM",
                    static_cast<unsigned long long>(AmTitleFor(prepared, form)), FormName(form));
            return rc;
        }
        LogLine(run, "guest: delete %016llX (%s form): title rc=0x%08lX, user rc=0x%08lX, ticket %s0x%08lX, pending rc=0x%08lX, %s",
                static_cast<unsigned long long>(AmTitleFor(prepared, form)), FormName(form), static_cast<unsigned long>(r.title_rc),
                static_cast<unsigned long>(r.user_rc), r.ticket_skipped ? "SKIPPED (not ours, still listed, or list unread) " : "rc=", static_cast<unsigned long>(r.ticket_rc),
                static_cast<unsigned long>(r.pending_rc), r.still_listed ? "STILL LISTED" : "not listed");
        return rc;
    }
    bool Folder(u64 process_id, u64 game, bool on) override {
        // The CopyWorker makes it: the one thread that writes AzaharLink.3gx.part (the start's refresh of every folder goes through it too). The build
        // 2h: the folder of the process's id of the form (the system id, or the key)
        setup::CopyWorker& copies = setup::CopyWorker::Get();
        copies.SubmitGuest(process_id, game, on, run->plugin_file);
        for (u64 waited = 0; copies.Pending(process_id, nullptr) && waited < kFolderWaitMs; waited += 20) {
            svcSleepThread(20 * 1000000LL);
        }
        bool ok = false;
        return copies.TakeGuestDone(process_id, &ok) && ok;
    }
    bool ReadIds(IdCensus* live) override {
        return ReadConsoleIds(live); // (the build 2h: every list again, read only - the guard of every write)
    }
    bool Stop() override {
        return run->stop_now.load();
    }
    void Progress(int stage, u64 done, u64 total) override {
        LightLock_Lock(&run->lock);
        run->stage = stage;
        run->done = done;
        run->total = total;
        LightLock_Unlock(&run->lock);
    }
    void Step(const char* what, u64 tid, RunResult rc) override {
        LogLine(run, "guest: %s %016llX rc=0x%08lX", what, static_cast<unsigned long long>(tid), static_cast<unsigned long>(rc));
    }
    void HomeIcon(u64 prepared) override {
        LogLine(run, "%s", HomeIconLine("after the install", AmTitleOf(prepared)).c_str());
    }
    void Note(const char* line) override {
        LogLine(run, "%s", line);
    }
};

// ---- boot.firm, for the children of method 1 (the build 2e slot25, 2026-11-18: analysis/piano-1.1.4-2026-11-15/luigi-308-2026-11-18/
// CAUSA-E-PROGETTO.md, the way H) ----------------------------------------------------------------------------------------------------------
// The KeyX of slot 0x25 is a constant of Luma3DS's ARM9 code: the composer finds it in the boot.firm the console starts from (FindSlot25). It is read
// here, READ ONLY, only when a child of method 1 asks (never at every start), at most once per run, on the run's thread (never the screen's):
// sdmc:/boot.firm first (boot9strap starts it when it is there), else /boot.firm of the CTRNAND (FS_OPEN_READ on ARCHIVE_NAND_CTR_FS: CtrNandRo,
// DlpLink/rsf/3dslink.rsf). Nothing is ever written there. The bytes stay on the heap until the run ends, then are wiped (they hold the key).
struct FirmCache {
    Run* run = nullptr;
    bool tried = false;
    std::unique_ptr<u8[]> data;
    std::size_t size = 0;
};

constexpr const char* kSdFirm = "sdmc:/boot.firm";
constexpr const char* kNandFirm = "/boot.firm";

bool LoadSdFirm(FirmCache* f, char* why, std::size_t why_size) {
    FILE* file = std::fopen(kSdFirm, "rb");
    if (file == nullptr) {
        std::snprintf(why, why_size, "%s not there", kSdFirm);
        return false;
    }
    long n = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        n = std::ftell(file);
    }
    std::fseek(file, 0, SEEK_SET);
    if (n <= 0 || static_cast<std::size_t>(n) > kFirmMaxBytes) {
        std::fclose(file);
        std::snprintf(why, why_size, "%s of %ld bytes (more than %u: not read)", kSdFirm, n, static_cast<unsigned>(kFirmMaxBytes));
        return false;
    }
    f->data.reset(new (std::nothrow) u8[static_cast<std::size_t>(n)]);
    const bool read = f->data && std::fread(f->data.get(), 1, static_cast<std::size_t>(n), file) == static_cast<std::size_t>(n);
    std::fclose(file);
    if (!read) {
        std::snprintf(why, why_size, "%s of %ld bytes %s", kSdFirm, n, f->data ? "not read whole" : "and no memory for it");
        if (f->data) {
            Wipe(f->data.get(), static_cast<std::size_t>(n));  // (a part of boot.firm may hold the KeyX: wiped before the heap takes it back)
        }
        f->data.reset();
        return false;
    }
    f->size = static_cast<std::size_t>(n);
    std::snprintf(why, why_size, "%s, %ld bytes", kSdFirm, n);
    return true;
}

bool LoadNandFirm(FirmCache* f, char* why, std::size_t why_size) {
    Handle file = 0;
    const Result open = FSUSER_OpenFileDirectly(&file, ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, kNandFirm), FS_OPEN_READ, 0);
    if (R_FAILED(open)) {
        std::snprintf(why, why_size, "ctrnand:%s rc=0x%08lX", kNandFirm, static_cast<unsigned long>(open));
        return false;
    }
    u64 n = 0;
    const Result sized = FSFILE_GetSize(file, &n);
    if (R_FAILED(sized) || n == 0 || n > kFirmMaxBytes) {
        FSFILE_Close(file);
        std::snprintf(why, why_size, "ctrnand:%s of %llu bytes, rc=0x%08lX (not read)", kNandFirm, static_cast<unsigned long long>(n),
                      static_cast<unsigned long>(sized));
        return false;
    }
    f->data.reset(new (std::nothrow) u8[static_cast<std::size_t>(n)]);
    u32 got = 0;
    const Result read = f->data ? FSFILE_Read(file, &got, 0, f->data.get(), static_cast<u32>(n)) : -1;
    FSFILE_Close(file);
    if (R_FAILED(read) || got != n) {
        std::snprintf(why, why_size, "ctrnand:%s of %llu bytes %s rc=0x%08lX", kNandFirm, static_cast<unsigned long long>(n),
                      f->data ? "not read whole" : "and no memory for it", static_cast<unsigned long>(read));
        if (f->data) {
            Wipe(f->data.get(), static_cast<std::size_t>(n));  // (as above)
        }
        f->data.reset();
        return false;
    }
    f->size = static_cast<std::size_t>(n);
    std::snprintf(why, why_size, "ctrnand:%s, %llu bytes", kNandFirm, static_cast<unsigned long long>(n));
    return true;
}

// The composer's FirmFn: boot.firm read the first time a child of method 1 asks in this run; the same bytes for the next ones.
bool FirmOf(void* ctx, const u8** data, std::size_t* size) {
    FirmCache* f = static_cast<FirmCache*>(ctx);
    if (!f->tried) {
        f->tried = true;
        char sd[96], nand[96];
        nand[0] = '\0';
        const bool ok = LoadSdFirm(f, sd, sizeof(sd)) || LoadNandFirm(f, nand, sizeof(nand));
        LogLine(f->run, "guest: slot25 boot.firm %s: %s%s%s", ok ? "read (read only)" : "NOT read", sd, nand[0] != '\0' ? "; " : "", nand);
    }
    if (!f->data) {
        return false;
    }
    *data = f->data.get();
    *size = f->size;
    return true;
}

bool OnlyProbes(const std::vector<Planned>& jobs) {
    for (const Planned& p : jobs) {
        if (p.act != Act::Probe) {
            return false;
        }
    }
    return true;
}

bool NeedsKeys(const std::vector<Planned>& jobs) {
    for (const Planned& p : jobs) {
        if (p.act == Act::Probe || p.act == Act::Check || p.act == Act::Prepare || p.act == Act::Repair) {
            return true;
        }
    }
    return false;
}

// boot9.bin read and made into the keys (on the heap, wiped by the caller): false when it cannot be (the jobs that need them say kErrNoBoot9).
bool LoadKeys(const std::string& path, Keys* keys) {
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::unique_ptr<u8[]> boot9(new (std::nothrow) u8[kBoot9Whole]);
    std::size_t n = boot9 ? std::fread(boot9.get(), 1, kBoot9Whole, f) : 0;
    std::fclose(f);
    const bool ok = boot9 && (n == kBoot9Whole || n == kBoot9Protected) && ComposeKeys(boot9.get(), n, keys) == 0;
    if (boot9) {
        Wipe(boot9.get(), kBoot9Whole);
    }
    return ok;
}

void RunMain(void* arg) {
    Run* r = static_cast<Run*>(arg);
    LightLock_Lock(&r->lock);
    r->started_ms = Ms();
    LightLock_Unlock(&r->lock);
    std::unique_ptr<u8[]> chunk(new (std::nothrow) u8[kChunkBytes]);
    std::unique_ptr<u8[]> work(new (std::nothrow) u8[kWorkBytes]);
    std::unique_ptr<Composed> composed(new (std::nothrow) Composed);
    std::unique_ptr<Keys> keys(new (std::nothrow) Keys);
    const bool memory = chunk && work && composed && keys;
    const bool keyed = memory && NeedsKeys(r->jobs) && LoadKeys(r->boot9_path, keys.get());
    LogLine(r, "guest: run of %d jobs begins: memory %s, keys %s", static_cast<int>(r->jobs.size()), memory ? "ok" : "MISSING",
            keyed ? "made" : (NeedsKeys(r->jobs) ? "NOT MADE" : "not needed"));
    Console console;
    console.run = r;
    console.keys = keyed ? keys.get() : nullptr;
    RunIn in;
    in.keys = console.keys;
    in.language = r->language;
    in.buf = chunk.get();
    in.buf_size = kChunkBytes;
    in.work = work.get();
    in.work_size = kWorkBytes;
    in.composed = composed.get();
    in.waiting = r->waiting;
    in.keys_stamp = r->keys_stamp;
    FirmCache firm; // (the build 2e slot25: read only when a child of method 1 asks, wiped at the end of the run)
    firm.run = r;
    in.firm = &FirmOf;
    in.fctx = &firm;
    in.clock_ms = &Ms;
    in.hidden_refused = &g_hidden_refused; // (the build 2f: the memo of the session)
    // (the build 2h) the cache of the ids the look wrote (ids.cache): every write of the run is held against it, the console read again right before
    // (dlp_guest_ids.hpp R8). None that reads whole: made now from the console. A run of looks only writes nothing to AM: no census
    IdCensus ids;
    const bool probes = OnlyProbes(r->jobs);
    if (!probes) {
        if (LoadCensus(&ids)) {
            LogLine(r, "guest: ids: the cache of the look (%lu titles of the SD card, %lu of the NAND, %lu tickets)", static_cast<unsigned long>(ids.src[kSrcSd].count),
                    static_cast<unsigned long>(ids.src[kSrcNand].count), static_cast<unsigned long>(ids.src[kSrcTicket].count));
        } else {
            RefreshReport rep;
            RefreshConsoleIds(&ids, &rep);
            SaveCensus(ids);
            LogLine(r, "guest: %s", RefreshLine(ids, rep, "no cache that reads whole: made at the run").c_str());
        }
        in.ids = &ids;
    }
    for (std::size_t i = 0; i < r->jobs.size(); ++i) {
        if (r->stop_after.load() || r->stop_now.load()) {
            break;
        }
        LightLock_Lock(&r->lock);
        r->index = static_cast<int>(i);
        r->stage = kStageRead;
        r->done = 0;
        r->total = 0;
        LightLock_Unlock(&r->lock);
        const Planned& job = r->jobs[i];
        LogLine(r, "guest: %s %016llX (%s, version %u, update %u)", ActName(job.act), static_cast<unsigned long long>(job.game), job.card ? "card" : "SD",
                static_cast<unsigned>(job.game_version), static_cast<unsigned>(job.update_version));
        JobEnd end;
        end.job = job;
        end.result = memory ? RunJob(job, in, console, &end.note) : kErrNoMemory;
        console.CloseContent();
        LogLine(r, "guest: %s %016llX ends: %d (%s)", ActName(job.act), static_cast<unsigned long long>(job.game), end.result, Phrase(end.result));
        LightLock_Lock(&r->lock);
        r->ends.push_back(end);
        LightLock_Unlock(&r->lock);
        if (!probes || (i + 1) % kProbeFlushJobs == 0) {
            FlushLog(r); // (a guest prepared or removed: after each job; the looks of Games: every few games, the rest at the end)
        }
    }
    if (keys) {
        Wipe(keys.get(), sizeof(Keys));
    }
    if (composed) {
        Wipe(composed.get(), sizeof(Composed)); // (the meta block and the front: a game's bytes, not left in the heap)
    }
    if (chunk) {
        Wipe(chunk.get(), kChunkBytes);
    }
    if (firm.data) {
        Wipe(firm.data.get(), firm.size); // (boot.firm holds the KeyX of slot 0x25: not left in the heap)
        firm.data.reset();
    }
    if (!probes) {
        SaveCensus(ids); // (what the console held after the run's last write: the next look starts from it)
    }
    LogLine(r, "guest: run ends%s", r->stop_after.load() ? " (stopped by the user after a game)"
                                                         : (r->stop_now.load() ? (r->stopped_by_user.load() ? " (stopped by the user)" : " (the app ends)") : ""));
    FlushLog(r);
    r->finished.store(true);
}

void Join(Run* r) {
    if (r->thread) {
        threadJoin(r->thread, U64_MAX); // (U64_MAX never times out: a timeout would read as success)
        threadFree(r->thread);
        r->thread = nullptr;
    }
}

// ---- what the last look knew (main thread) ------------------------------------------------------------------------------------------

// A look of Games that stopped at the keys (kErrNoBoot9, kErrBoot9NoKeys, kErrNoMovable: not the game's, never in children.cache - FailureKept): kept
// this session with the game's versions, boot9.bin's stamp and the scan, so that the game is not read again at every opening of Games (spec 9 point 5:
// "read when boot9.bin comes"; boot9.bin that comes makes a new scan and another stamp). The review of 2026-11-17 evening.
struct KeysWait {
    u64 game = 0;
    u16 game_version = 0, update_version = 0;
    u32 keys = 0;
    int error = 0;
    int generation = 0;
};

bool StoppedAtKeys(int err) {
    return err == kErrNoBoot9 || err == kErrBoot9NoKeys || err == kErrNoMovable;
}

struct Known {
    std::vector<Guest> records;
    std::vector<ChildSeen> cache;
    std::vector<u64> cards_checked;              // the cards whose child was read this session
    std::vector<KeysWait> keys_waits;            // the looks that stopped at the keys this session (TakeEnds; ToReadNow skips them)
};
Known g_known;

bool Has(const std::vector<u64>& v, u64 x) {
    for (u64 y : v) {
        if (y == x) {
            return true;
        }
    }
    return false;
}

const Guest* RecordOf(const std::vector<Guest>& records, u64 game) {
    for (const Guest& g : records) {
        if (g.game == game && IsOwnedGuestTid(g.prepared)) {
            return &g;
        }
    }
    return nullptr;
}

bool BlockWhole(u64 prepared) {
    struct stat st;
    return stat(BlockPath(prepared).c_str(), &st) == 0 && S_ISREG(st.st_mode) && static_cast<u64>(st.st_size) == azl::dlp_children::kBlockBytes;
}

// The games now, against the records (the main thread, after a scan; AM and two small files: no content is read here). A game is ON when its record is
// there (the user switched it on in Download Play > Enable games: spec 9 point 7) or when it is `wanted`, the game the user is switching on now. `every` looks
// at every game that may have a child too (Download Play > Enable games); otherwise only at the games that are on.
struct Looked {
    std::vector<Look> looks;
    std::vector<Waiting> waiting;
};

Looked LookAround(const std::vector<const titles::Game*>& games, u64 wanted, bool every, std::atomic<int>* done = nullptr, const IdCensus* census = nullptr) {
    Looked out;
    g_known.records = LoadGuests();
    if (!Running()) {
        // (the review of spec 9: the worker writes children.cache through a .bak while it runs - a read in that moment would put the .bak back and lose
        // its write; what the last look read is kept, and the page looks again when the run ends)
        g_known.cache = LoadChildrenCache();
    }
    const std::vector<Guest>& records = g_known.records;
    const bool keys = KeysReady();
    const u32 stamp = keys ? KeysStamp() : 0;
    std::vector<u64> sd;
    const bool listed = ListSdTitles(&sd);
    std::vector<u64> seen_games;
    const auto look = [&](u64 tid, bool present, bool on_card, u16 version, bool guest_game, bool still_listed) {
        const Guest* record = RecordOf(records, tid);
        const bool on = guest_game && (record != nullptr || (wanted != 0 && tid == wanted));
        if (!on && record == nullptr && !(every && guest_game)) {
            return;
        }
        seen_games.push_back(tid);
        Look l;
        Seen& s = l.seen;
        s.game = tid;
        s.on = on;
        s.present = present;
        s.still_listed = !present && still_listed;
        s.card = present ? on_card : (record != nullptr && record->where == 'C');
        s.card_checked = Has(g_known.cards_checked, tid);
        s.game_version = version;
        s.update_version = present ? UpdateVersion(tid) : 0;
        const ChildSeen* c = FindChildSeen(g_known.cache, tid, s.game_version, s.update_version);
        s.child = c == nullptr ? ChildFact::Unknown : (c->has_child ? ChildFact::Has : ChildFact::None);
        const ChildSeen* failed = FindChildFailure(g_known.cache, tid, s.game_version, s.update_version);
        s.look_failed = failed != nullptr && failed->keys == stamp; // (a Check that failed is not made again at every opening of the page)
        if (c != nullptr && c->has_child) {
            s.child_tid = c->child;
            s.child_version = c->child_version;
            std::memcpy(s.product, c->product, sizeof(s.product));
            l.bytes = c->required;
        }
        s.guest = record;
        const u64 prepared = PreparedFor(tid);
        // (the build 2f) the ids of the forms: the record's; with no record the first one AM lists (the system form first, then the old one, then
        // M3c1's), or the form a child is made in. The other forms say whether our child is there too (a migration that was cut) or a title that is
        // not ours (the build 2h: three forms)
        Presence at_of[kFormCount] = {Presence::Absent, Presence::Absent, Presence::Absent};
        u16 version_of[kFormCount] = {};
        char product_of[kFormCount][kProductBytes] = {};
        bool read_of[kFormCount] = {};
        for (const u8 x : kForms) {
            at_of[x] = PresenceIn(listed ? &sd : nullptr, prepared, x, &version_of[x]);
            if (at_of[x] == Presence::Present || at_of[x] == Presence::Broken) {
                read_of[x] = PreparedProduct(prepared, x, product_of[x]);
            }
        }
        u8 form = record != nullptr ? record->form : kPreparedForm;
        for (std::size_t i = kFormCount; record == nullptr && i-- > 0;) {
            const u8 x = kForms[i];
            form = at_of[x] != Presence::Absent ? x : form; // (the first of kForms that AM lists)
        }
        const Presence at = at_of[form];
        s.prepared_form = form;
        s.am_known = at != Presence::Unknown;
        s.prepared_installed = at == Presence::Present || at == Presence::Broken;
        s.prepared_version = version_of[form];
        if (s.prepared_installed) {
            std::memcpy(s.prepared_product, product_of[form], sizeof(s.prepared_product));
        }
        const char* ours = record != nullptr ? record->product : (c != nullptr && c->has_child ? c->product : "");
        bool all_others_ours = true;
        for (const u8 x : kForms) {
            if (x == form || !(at_of[x] == Presence::Present || at_of[x] == Presence::Broken)) {
                continue;
            }
            s.other_form_present = true;
            const bool mine = read_of[x] && ours[0] != '\0' && !OtherProduct(product_of[x], ours) &&
                              (record != nullptr || (c != nullptr && c->has_child && version_of[x] == c->child_version));
            all_others_ours = all_others_ours && mine;
        }
        s.other_form_ours = s.other_form_present && all_others_ours;
        if (record != nullptr) {
            s.plugin_on = setup::ReadSlot(ProcessIdFor(prepared, form)).slot == setup::Slot::On; // (the build 2h: the folder of the form's process id)
            s.block_ok = BlockWhole(prepared);
        }
        if (census != nullptr && census->valid && prepared != 0) {
            // (the build 2h) the ids of the slot from the cache the look just brought up to date: a conflict is shown before anything is asked (the
            // run judges the console again right before it writes). Ours: a title of the record (or, with none, the very child the look knows -
            // the one an adoption takes), and its ticket by TicketMayGo
            Ownership own;
            own.record = record != nullptr;
            for (const u8 x : kForms) {
                const bool listed_x = at_of[x] == Presence::Present || at_of[x] == Presence::Broken;
                const bool mine = record != nullptr ? !(read_of[x] && OtherProduct(product_of[x], record->product))
                                                    : (read_of[x] && ours[0] != '\0' && !OtherProduct(product_of[x], ours));
                own.sd_ours |= listed_x && mine ? FormBit(x) : 0u;
            }
            const bool ours_now = (own.sd_ours & FormBit(kPreparedForm)) != 0 && read_of[kPreparedForm];
            own.ticket_ours = record != nullptr ? TicketMayGo(*record, kPreparedForm, ours_now) : ours_now;
            const IdTaken t = JudgeInstall(SlotIn(*census, prepared), kPreparedForm, own);
            s.id_taken = t == IdTaken::Nand ? kErrIdSystem
                         : (t == IdTaken::Ticket ? kErrIdTicket
                                                 : (t == IdTaken::Running ? kErrIdRunning : (t == IdTaken::Free || t == IdTaken::Unknown ? 0 : kErrIdTaken)));
        }
        s.language = static_cast<u8>(ui::g.language);
        s.keys = keys;
        s.guests = records.size();
        if (on && present && c != nullptr && c->has_child && (record == nullptr || record->state != GuestState::Installed)) {
            Waiting w;
            w.game = tid;
            w.child = c->child;
            w.child_version = c->child_version;
            w.where = on_card ? 'C' : 'S';
            out.waiting.push_back(w);
        }
        out.looks.push_back(l);
    };
    for (const titles::Game* game : games) {
        look(game->tid, true, game->card, game->card ? 0 : game->version, setup::IsGuestGame(game->tid, game->name), false);
        if (done != nullptr) {
            done->fetch_add(1); // (the bar of Download Play > Enable games: one game looked at)
        }
    }
    // The records whose game is in neither list: uninstalled from the SD card (removed), a card that is out (kept), or a game of the SD card the scan
    // did not recognise (kept: AM still lists it, or gave no list).
    for (const Guest& g : records) {
        if (!Has(seen_games, g.game) && IsOwnedGuestTid(g.prepared)) {
            const bool still_listed = g.where != 'C' && StillOnSd(listed ? &sd : nullptr, g.game);
            look(g.game, false, g.where == 'C', 0, setup::IsGuestGame(g.game, std::string()), still_listed);
        }
    }
    return out;
}

std::vector<Waiting> g_waiting; // what the last look found waiting (the rows of the index with prepared 0)

// The games of Download Play > Enable games still to read (spec 9 point 5): every game that may have a child, with no record, of which children.cache knows
// nothing at its versions (ToRead), or whose failure was with another boot9.bin. The thread of the look only (AM is asked each game's update). A game whose
// look stopped at the keys this session, at the same versions, boot9.bin and scan, goes to `no_keys` instead (its row says why: never a run of them all
// at every opening).
std::vector<Planned> ToReadNow(const std::vector<const titles::Game*>& games, std::atomic<int>* done, std::vector<Planned>* no_keys, int* no_keys_error) {
    std::vector<Planned> out;
    const u32 stamp = KeysStamp();
    const std::vector<Guest> records = LoadGuests();
    const std::vector<ChildSeen> cache = LoadChildrenCache();
    for (const titles::Game* game : games) {
        if (done != nullptr) {
            done->fetch_add(1);
        }
        if (!setup::IsGuestGame(game->tid, game->name)) {
            continue;
        }
        Planned p;
        p.game = game->tid;
        p.act = Act::Probe;
        p.card = game->card;
        p.game_version = game->card ? 0 : game->version;
        p.update_version = UpdateVersion(game->tid);
        if (ToRead(true, RecordOf(records, p.game) != nullptr, FindChildSeen(cache, p.game, p.game_version, p.update_version),
                   FindChildFailure(cache, p.game, p.game_version, p.update_version), stamp)) {
            const KeysWait* waits = nullptr;
            for (const KeysWait& w : g_known.keys_waits) {
                if (w.game == p.game && w.game_version == p.game_version && w.update_version == p.update_version && w.keys == stamp) {
                    waits = &w;
                }
            }
            if (waits != nullptr) {
                *no_keys_error = no_keys->empty() ? waits->error : *no_keys_error;
                no_keys->push_back(p);
            } else {
                out.push_back(p);
            }
        }
    }
    return out;
}

// The plan of the game the user switches on in Games (`game`: what the reconciliation would do with it on - Prepare, Adopt, Refuse, Full...), Nothing
// when the plan has nothing of it.
Planned WantedOf(const Plan& plan, u64 game) {
    for (const std::vector<Planned>* list : {&plan.heavy, &plan.quiet, &plan.said}) {
        for (const Planned& p : *list) {
            if (p.game == game) {
                return p;
            }
        }
    }
    Planned none;
    none.game = game;
    return none;
}

} // namespace

bool KeysReady() {
    return ui::g.boot9.found && setup::ClassifyBoot9File(ui::g.boot9.path) != setup::Boot9Verdict::NoKeys;
}

u32 KeysStamp() {
    if (!KeysReady()) {
        return 0;
    }
    // (read once a file: the same path, size and time give the stamp read before - it was read again at every look, the whole file, 2026-11-17)
    static std::string s_path;
    static long s_mtime = 0;
    static u64 s_size = 0;
    static u32 s_stamp = 0;
    struct stat st;
    if (stat(ui::g.boot9.path.c_str(), &st) != 0) {
        return 0;
    }
    if (s_stamp != 0 && s_path == ui::g.boot9.path && s_size == static_cast<u64>(st.st_size) && s_mtime == static_cast<long>(st.st_mtime)) {
        return s_stamp;
    }
    FILE* f = std::fopen(ui::g.boot9.path.c_str(), "rb");
    if (f == nullptr) {
        return 0;
    }
    // (FNV-1a of the file: 32 bits that say "another file", nothing of the keys in it)
    u32 h = 2166136261u;
    u8 buf[4096];
    std::size_t total = 0, n = 0;
    while (total < kBoot9Whole && (n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ buf[i]) * 16777619u;
        }
        total += n;
    }
    std::fclose(f);
    Wipe(buf, sizeof(buf));
    s_path = ui::g.boot9.path;
    s_size = static_cast<u64>(st.st_size);
    s_mtime = static_cast<long>(st.st_mtime);
    s_stamp = h == 0 ? 1u : h;
    return s_stamp;
}

bool CanLook() {
    if (Running() || LookUnderWay() || !titles::Scan().done() || setup::CopyWorker::Get().PendingCount() > 0 || ui::g.module_holds > 0) {
        return false; // (somebody else works on the same things now: the look waits)
    }
    if (ui::g.rescan_wanted) {
        return false; // (a new scan of the titles is wanted - boot9.bin came, titles were installed -: it starts first, main.cpp, the look waits for it)
    }
    return true;
}

int StartRun(const std::vector<Planned>& jobs) {
    if (g_run && !g_run->finished.load()) {
        return kErrCancel;
    }
    if (LookUnderWay()) {
        return kErrCancel; // (the thread of the look reads the records and children.cache, which a run writes: one after the other)
    }
    if (g_run) {
        Join(g_run.get());
        g_run.reset();
    }
    const u32 needed = static_cast<u32>(kChunkBytes + kWorkBytes + sizeof(Composed)) + kWorkerStack + kRunSlackBytes;
    if (dl_heap_free() < needed) {
        return kErrNoMemory;
    }
    std::unique_ptr<Run> r(new (std::nothrow) Run);
    if (!r) {
        return kErrNoMemory;
    }
    // Route K reads the installed title through the delivery module, with the scan's context: it is handed to the module here, on the main thread,
    // before the worker starts (the review of spec 9: only the pages of Titles Delivery and 3DS Link Delivery did it, and a run after a start, or
    // after the scan that followed boot9.bin, found the module with no context - "Needs movable.sed"). Without a scan done there is none to give.
    delivery::AttachModule();
    LightLock_Init(&r->lock);
    r->jobs = jobs;
    r->waiting = g_waiting;
    r->boot9_path = KeysReady() ? ui::g.boot9.path : std::string();
    r->plugin_file = ui::g.paths.plugin_file;
    r->log_path = ui::g.paths.datadir.empty() ? std::string() : setup::JoinPath(ui::g.paths.datadir, kLogName);
    r->language = static_cast<u8>(ui::g.language);
    r->keys_stamp = KeysStamp();
    std::vector<Planned> heavy;
    for (const Planned& p : jobs) {
        if (p.act == Act::Prepare) {
            heavy.push_back(p);
        }
    }
    r->planned_seconds = MinutesFor(heavy) * 60;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // core -2: the app's own core (never the Old 3DS's core 1); a little less important than the screen, which keeps drawing and answering B.
    r->thread = threadCreate(RunMain, r.get(), kWorkerStack, prio < 0x3F ? prio + 1 : prio, -2, false);
    if (!r->thread) {
        return kErrNoMemory;
    }
    g_run = std::move(r);
    return 0;
}

bool Running() {
    return g_run && !g_run->finished.load();
}

RunView View() {
    RunView v;
    if (!g_run) {
        return v;
    }
    Run* r = g_run.get();
    LightLock_Lock(&r->lock);
    v.index = r->index;
    const int stage = r->stage;
    const u64 done = r->done, total = r->total, since = r->started_ms;
    LightLock_Unlock(&r->lock);
    v.count = static_cast<int>(r->jobs.size());
    if (v.index < v.count) {
        v.game = r->jobs[static_cast<std::size_t>(v.index)].game;
        v.act = r->jobs[static_cast<std::size_t>(v.index)].act;
    }
    v.fraction = RunFraction(v.index, v.count, v.act == Act::Prepare ? JobFraction(stage, done, total) : 0.5);
    v.stopping = r->stop_after.load() || r->stop_now.load();
    const double elapsed = since == 0 ? 0.0 : static_cast<double>(Ms() - since) / 1000.0;
    if (v.fraction > 0.05 && elapsed > 5.0) {
        v.seconds_left = static_cast<int>(elapsed / v.fraction * (1.0 - v.fraction) + 0.5);
    } else if (r->planned_seconds > 0) {
        v.seconds_left = r->planned_seconds > elapsed ? static_cast<int>(r->planned_seconds - elapsed) : 0;
    }
    return v;
}

void StopAfterCurrent() {
    if (g_run) {
        g_run->stop_after.store(true);
    }
}

void StopNow() {
    if (g_run) {
        g_run->stopped_by_user.store(true);
        g_run->stop_now.store(true); // (the composition and the stream stop at their next step: kErrCancel, AM's install cancelled, the record as it was)
    }
}

bool TakeEnds(std::vector<JobEnd>* ends) {
    if (!g_run || !g_run->finished.load()) {
        return false;
    }
    Join(g_run.get());
    *ends = g_run->ends;
    ++g_runs_ended;
    for (const JobEnd& e : *ends) {
        if (e.note.card_checked && !Has(g_known.cards_checked, e.job.game)) {
            g_known.cards_checked.push_back(e.job.game);
        }
        if (e.job.act != Act::Probe) {
            continue;
        }
        for (std::size_t i = g_known.keys_waits.size(); i-- > 0;) {
            if (g_known.keys_waits[i].game == e.job.game) {
                g_known.keys_waits.erase(g_known.keys_waits.begin() + static_cast<std::ptrdiff_t>(i)); // (looked at again: what it said now counts)
            }
        }
        if (StoppedAtKeys(e.result)) {
            KeysWait w;
            w.game = e.job.game;
            w.game_version = e.job.game_version;
            w.update_version = e.job.update_version;
            w.keys = g_run->keys_stamp;
            w.error = e.result;
            w.generation = titles::ScanGeneration();
            g_known.keys_waits.push_back(w);
        }
    }
    g_run.reset();
    return true;
}

void Shutdown() {
    JoinLook(); // (the look of Download Play > Enable games ends by itself: AM and two small files)
    if (g_run) {
        g_run->stop_now.store(true); // (the job under way stops where it is: the composer and AM leave nothing half done, the start settles a record)
        Join(g_run.get());
        g_run.reset();
    }
}

std::string GameName(u64 game) {
    if (titles::Scan().done()) {
        for (const std::unique_ptr<titles::Game>& g : titles::Scan().games()) {
            if (g->tid == game) {
                return gfx::Clean(g->name);
            }
        }
    }
    return setup::TidText(game);
}

std::vector<ChildRow> InstalledChildren() {
    std::vector<ChildRow> rows;
    std::vector<u64> sd;
    const bool listed = ListSdTitles(&sd);
    for (const Guest& g : LoadGuests()) {
        ChildRow row;
        row.guest = g;
        row.name = GameName(g.game);
        u16 version = 0;
        row.at = PresenceIn(listed ? &sd : nullptr, g.prepared, g.form, &version); // (the build 2f: by the record's form)
        row.owned = IsOwnedGuestTid(g.prepared);
        row.title_bytes = row.at == Presence::Present ? PreparedBytes(g.prepared, g.form) : 0;
        const setup::SlotInfo slot = setup::ReadSlot(ProcessIdFor(g.prepared, g.form)); // (the build 2h: the form's process id)
        row.plugin_bytes = slot.slot == setup::Slot::On ? slot.copy_size : 0;
        row.block_bytes = BlockWhole(g.prepared) ? azl::dlp_children::kBlockBytes : 0;
        rows.push_back(row);
    }
    return rows;
}

std::vector<GameView> LookAtGames(const std::vector<const titles::Game*>& games, std::atomic<int>* done, const IdCensus* census) {
    const Looked looked = LookAround(games, 0, true, done, census);
    std::vector<GameView> out;
    for (const Look& l : looked.looks) {
        const ChildSeen* failure = FindChildFailure(g_known.cache, l.seen.game, l.seen.game_version, l.seen.update_version);
        Seen on = l.seen;
        on.on = true; // (what the reconciliation would do with the game switched on: Prepare is "to update")
        GameView v;
        v.game = l.seen.game;
        v.shown = ShownOf(l.seen, failure, Decide(on));
        v.error = failure != nullptr ? failure->error : 0;
        v.id_taken = l.seen.guest == nullptr ? l.seen.id_taken : 0; // (the build 2h: a guest of ours is never "taken"; its run judges the console)
        v.child_bytes = l.bytes;
        v.card = l.seen.card;
        v.game_version = l.seen.game_version;
        v.update_version = l.seen.update_version;
        if (v.shown != Shown::Hidden) {
            out.push_back(v);
        }
    }
    return out;
}

// ---- the look of Download Play > Enable games, in a thread of its own (2026-11-17 evening: nothing of it on the thread of the screen) ------------------

namespace {

constexpr u32 kLookStack = 128 * 1024;

// (the build 2e, a diagnostic only) HOME's icon of every prepared title of ours, once an opening of 3DS Link: the first look of the session writes the
// lines, in ITS thread. The start of the app (StartGuests, pages_startup.cpp) runs on the thread of the screen, so it is not there; a look happens when
// a page of Download Play opens. One look at a time: the flag is the looks' alone.
std::atomic<bool> g_home_icons_logged{false};

void LogHomeIconsOnce() {
    if (g_home_icons_logged.exchange(true) || ui::g.paths.datadir.empty()) {
        return;
    }
    std::vector<std::string> lines;
    for (const Guest& g : LoadGuests()) {
        if (g.state == GuestState::Installed && IsOwnedGuestTid(g.prepared)) {
            lines.push_back(HomeIconLine("at the opening", AmTitleFor(g.prepared, g.form)));
        }
    }
    AppendLog(setup::JoinPath(ui::g.paths.datadir, kLogName), lines);
}

struct LookRun {
    bool quiet = false;
    u64 wanted = 0;
    Thread thread = nullptr;
    std::atomic<bool> finished{false};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    GamesLook out;
};
std::unique_ptr<LookRun> g_look;

// (the build 2h) the cache of the ids brought up to date by the look (dlp_guest_ids.hpp): every list read again, only the sources that changed put
// in again, the milliseconds in the log of the children; written only when something changed (or there was none that read whole).
void LookIds(IdCensus* census) {
    const bool had = LoadCensus(census);
    RefreshReport rep;
    RefreshConsoleIds(census, &rep);
    if (!had || rep.changed != 0) {
        SaveCensus(*census);
    }
    if (!ui::g.paths.datadir.empty()) {
        AppendLog(setup::JoinPath(ui::g.paths.datadir, kLogName), {"guest: " + RefreshLine(*census, rep, had ? "the look" : "the look, no cache that reads whole")});
    }
}

void LookMain(void* arg) {
    LookRun* l = static_cast<LookRun*>(arg);
    IdCensus census;
    LookIds(&census);
    std::vector<const titles::Game*> games;
    for (const std::unique_ptr<titles::Game>& game : titles::Scan().games()) {
        games.push_back(game.get()); // (the scan cannot change under the thread: the page holds ui::g.module_holds while it looks)
    }
    if (titles::ReadCardGame(ui::g.language, l->out.card) && !l->out.card.name.empty()) {
        bool listed = false;
        for (const titles::Game* game : games) {
            listed = listed || game->tid == l->out.card.tid;
        }
        if (!listed) {
            l->out.has_card = true;
            games.push_back(&l->out.card);
        }
    }
    if (l->quiet || l->wanted != 0) {
        // (the Download Play page: what Poll did on the thread of the screen - the records against the games, the quiet part of the Plan. A on a game
        // of Games that is off: what PlanFor did there - the plan of that game switched on, with the slot read here, and its folder)
        l->total.store(static_cast<int>(games.size()));
        const Looked looked = LookAround(games, l->wanted, false, &l->done, &census);
        g_waiting = looked.waiting; // (read by StartRun on the thread of the screen after the look: never both at once)
        const Plan plan = Reconcile(looked.looks, g_known.records.size());
        if (l->wanted != 0) {
            l->out.wanted = WantedOf(plan, l->wanted);
            l->out.enable_off = setup::ReadSlot(l->wanted).slot == setup::Slot::Off;
        } else {
            l->out.quiet = plan;
        }
        LogHomeIconsOnce();
        l->finished.store(true);
        return;
    }
    l->total.store(static_cast<int>(games.size()) * 2);
    l->out.children = InstalledChildren();
    l->out.views = LookAtGames(games, &l->done, &census);
    l->out.to_read = ToReadNow(games, &l->done, &l->out.no_keys, &l->out.no_keys_error);
    LogHomeIconsOnce();
    l->finished.store(true);
}

bool LookUnderWay() {
    return g_look && !g_look->finished.load();
}

void JoinLook() {
    if (g_look && g_look->thread) {
        threadJoin(g_look->thread, U64_MAX);
        threadFree(g_look->thread);
        g_look->thread = nullptr;
    }
}

} // namespace

bool StartGamesLook(bool quiet, u64 wanted) {
    if (LookUnderWay() || Running() || !titles::Scan().done()) {
        return false;
    }
    if (g_run && g_run->finished.load()) {
        std::vector<JobEnd> ends;
        TakeEnds(&ends); // (a run that ended and that no page took - its page went - is let go here)
    }
    for (std::size_t i = g_known.keys_waits.size(); i-- > 0;) {
        if (g_known.keys_waits[i].generation != titles::ScanGeneration()) {
            // (a new scan - boot9.bin came, titles were installed -: the module's context too; the games that stopped at the keys are read again)
            g_known.keys_waits.erase(g_known.keys_waits.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    JoinLook();
    g_look.reset();
    std::unique_ptr<LookRun> l(new (std::nothrow) LookRun);
    if (!l) {
        return false;
    }
    l->quiet = quiet;
    l->wanted = wanted;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // core -2: the app's own core (never the Old 3DS's core 1); a little less important than the screen, which keeps drawing.
    l->thread = threadCreate(LookMain, l.get(), kLookStack, prio < 0x3F ? prio + 1 : prio, -2, false);
    if (!l->thread) {
        return false;
    }
    g_look = std::move(l);
    return true;
}

bool GamesLookDone() {
    return g_look && g_look->finished.load();
}

void GamesLookProgress(int* done, int* total) {
    *done = g_look ? g_look->done.load() : 0;
    *total = g_look ? g_look->total.load() : 0;
}

bool TakeGamesLook(GamesLook* out) {
    if (!GamesLookDone()) {
        return false;
    }
    JoinLook();
    *out = std::move(g_look->out);
    g_look.reset();
    return true;
}

void DropGamesLook() {
    JoinLook(); // (AM and a few small files: it ends by itself in a moment)
    g_look.reset();
}

int RunsEnded() {
    return g_runs_ended;
}

} // namespace dlpguest
