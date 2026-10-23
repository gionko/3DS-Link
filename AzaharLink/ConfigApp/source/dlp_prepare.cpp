#include "dlp_prepare.hpp"

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../dlpc/dlpc.h"
#include "../titleshare_host/delivery.h"
#include "child_model_bin.h" // the model of the child: the resource that data/child_model.bin is linked in as (devkitPro's bin2o)
#include "fileio.hpp"
#include "paths.hpp"
#include "plugin_resource.hpp"
#include "ui.hpp"

namespace dlp {

namespace {

// The thread has the stack of a worker that reads and hashes, and calls into the module's stream: 256 KB, the same as the thread of the titles' screens.
constexpr u32 kWorkerStack = 256 * 1024;
// What the app's CIA is taken to weigh until a file or the installed title says (the CIA of the tree is 1.5 MB: the app, the model of the child inside it, and the plugin).
constexpr u64 kUsualAppBytes = 1600ull << 10;
// What the work holds besides the composer's own memory: the stream of the installed title (about 400 KB), the thread's stack, the buffer of the copy.
constexpr u32 kWorkerSlackBytes = 1ull << 20;
// The copy of the app's CIA is made in blocks of this size (the progress is told after each).
constexpr std::size_t kCopyBlock = 64 * 1024;

u64 SizeOfFile(FILE* f) {
    if (std::fseek(f, 0, SEEK_END) != 0) {
        return 0;
    }
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    return n > 0 ? static_cast<u64>(n) : 0;
}

bool ReadFileAtOffset(FILE* f, u64 off, u8* buf, std::size_t len) {
    return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 && std::fread(buf, 1, len, f) == len;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// What is held while the work runs
// ---------------------------------------------------------------------------------------------

void WorkHolds::Take() {
    if (held_) {
        return;
    }
    ui::g.HoldHome();
    aptSetSleepAllowed(false);
    ++ui::g.module_holds;
    held_ = true;
}

void WorkHolds::Give() {
    if (!held_) {
        return;
    }
    --ui::g.module_holds;
    aptSetSleepAllowed(true);
    ui::g.ReleaseHome();
    held_ = false;
}

// ---------------------------------------------------------------------------------------------
// The files
// ---------------------------------------------------------------------------------------------

FileFacts ReadCiaFile(const std::string& path) {
    FileFacts facts;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return facts;
    }
    facts.exists = true;
    facts.size = static_cast<u64>(st.st_size);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return facts; // (there, and not readable: it is not a CIA that can be used)
    }
    facts.cia = InspectCia([f](u64 off, u8* buf, std::size_t len) { return ReadFileAtOffset(f, off, buf, len); }, facts.size);
    std::fclose(f);
    return facts;
}

Verdict CheckChildFile(const std::string& path, unsigned version16) {
    const FileFacts facts = ReadCiaFile(path);
    return JudgeChild(facts.exists, facts.size, facts.cia, version16);
}

Verdict CheckAppCiaFile(const std::string& path, unsigned version16, u64* size, unsigned* found_version) {
    const FileFacts facts = ReadCiaFile(path);
    if (size) {
        *size = facts.size;
    }
    if (found_version) {
        *found_version = facts.cia.parsed ? facts.cia.version : 0;
    }
    return JudgeAppCia(facts.exists, facts.size, facts.cia, version16);
}

namespace {
// The model is a constant resource of the build, and the composer hashes all of it (SHA-256 of 250 KB) to say its version: it is asked once. The page and the work
// ask from the screen's thread only (a race of two threads would only compute the same answer twice).
unsigned g_model_version = 0;
bool g_model_version_known = false;
} // namespace

unsigned ModelVersion() {
    if (!g_model_version_known) {
        g_model_version = dlpc_model_version(child_model_bin, child_model_bin_size);
        g_model_version_known = true;
    }
    return g_model_version;
}

int DiedRuns(const std::string& marker_path) {
    std::string text;
    switch (setup::ReadTextFile(marker_path, 256, text)) {
    case setup::LoadResult::Missing:
        return 0;
    case setup::LoadResult::Loaded:
        return MarkerAttempts(text);
    default:
        return 1; // (there, and not readable: a run began and nobody knows how it ended)
    }
}

// ---------------------------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------------------------

struct Preparer::Hooks {
    static void ThreadMain(void* arg) {
        static_cast<Preparer*>(arg)->Run();
    }
    // The composer's callbacks.
    static int ReadFile(void* user, uint64_t off, uint8_t* buf, size_t len) {
        return ReadFileAtOffset(static_cast<FILE*>(user), off, buf, len) ? 0 : 1;
    }
    // The plugin is NOT read from the card: it is the resource of this app (plugin_resource.hpp), so the package always carries exactly the plugin of this build, whatever is
    // on the card (a file cut short, damaged or of another version cannot be handed out), and the page needs no plugin file to exist.
    static int ReadEmbeddedPlugin(void*, uint64_t off, uint8_t* buf, size_t len) {
        const uint64_t size = setup::EmbeddedPluginSize();
        if (off > size || len > size - off) {
            return 1;
        }
        std::memcpy(buf, setup::EmbeddedPluginData() + static_cast<size_t>(off), len);
        return 0;
    }
    static int Progress(void* user, int stage, uint64_t done, uint64_t total) {
        Preparer* p = static_cast<Preparer*>(user);
        p->SetProgress(stage, done, total);
        return p->cancel_.load() ? 1 : 0; // (nonzero stops the composer: it leaves nothing behind)
    }
};

Preparer::Preparer() {
    LightLock_Init(&lock_);
}

Preparer::~Preparer() {
    Cancel();
    if (thread_) {
        threadJoin(thread_, U64_MAX); // (U64_MAX never times out: a timeout would read as success)
        threadFree(thread_);
        thread_ = nullptr;
    }
}

void Preparer::Log(const char* fmt, ...) {
    char line[200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    dl_log_line(line); // (the module's log: written only while the page has it open, and safe from any thread)
}

void Preparer::SetDetail(const std::string& text) {
    LightLock_Lock(&lock_);
    detail_ = text;
    LightLock_Unlock(&lock_);
}

std::string Preparer::detail() const {
    std::string copy;
    LightLock_Lock(const_cast<LightLock*>(&lock_));
    copy = detail_;
    LightLock_Unlock(const_cast<LightLock*>(&lock_));
    return copy;
}

void Preparer::SetProgress(int stage, u64 done, u64 total) {
    LightLock_Lock(&lock_);
    stage_ = stage;
    done_ = done;
    total_ = total;
    LightLock_Unlock(&lock_);
}

// The plan for a job of these sizes (the sizes became real, or the installed title said how big the app's CIA is): the bar and the clock go on.
void Preparer::SetPlan(bool copy, u64 app_bytes) {
    const dlpc_job job = SizesJob(child_model_bin, child_model_bin_size, app_bytes, plugin_bytes_, job_.version16, job_.version_text.c_str());
    const Plan plan = PlanFor(job, copy);
    LightLock_Lock(&lock_);
    plan_ = plan;
    plan_copy_ = copy;
    LightLock_Unlock(&lock_);
}

// A plan that begins the estimate again: the bar is at its start and the clock runs from now, so that the time left never counts the work of a plan that was dropped
// (the file the composer refused, and then the copy from the installed title: the pace of the first attempt says nothing about the second).
void Preparer::RestartPlan(bool copy, u64 app_bytes) {
    const dlpc_job job = SizesJob(child_model_bin, child_model_bin_size, app_bytes, plugin_bytes_, job_.version16, job_.version_text.c_str());
    const Plan plan = PlanFor(job, copy);
    LightLock_Lock(&lock_);
    plan_ = plan;
    plan_copy_ = copy;
    stage_ = kStageCopy;
    done_ = 0;
    total_ = 0;
    pace_ms_ = ui::NowMs();
    LightLock_Unlock(&lock_);
}

PrepareView Preparer::View() const {
    Plan plan;
    int stage;
    u64 done, total, since;
    LightLock* lock = const_cast<LightLock*>(&lock_);
    LightLock_Lock(lock);
    plan = plan_;
    stage = stage_;
    done = done_;
    total = total_;
    since = pace_ms_;
    LightLock_Unlock(lock);
    const u64 now = ui::NowMs();
    const double elapsed_s = now > since ? static_cast<double>(now - since) / 1000.0 : 0.0;
    PrepareView view;
    view.stage = stage;
    view.fraction = Fraction(plan, stage, done, total);
    view.seconds_left = SecondsLeft(plan, view.fraction, elapsed_s);
    view.measured = plan.total > 0.0 && PaceKnown(view.fraction, elapsed_s);
    view.plan_seconds = plan.total > 0.0 ? static_cast<int>(WholeSeconds(plan, view.fraction, elapsed_s) + 0.5) : 0;
    return view;
}

int Preparer::Start(const PrepareJob& job) {
    job_ = job;
    if (ModelVersion() == 0) {
        return kPrepNoModel;
    }
    plugin_bytes_ = setup::EmbeddedPluginSize();
    const u64 app_bytes = job.app_bytes > 0 ? job.app_bytes : kUsualAppBytes;
    RestartPlan(!job.use_file, app_bytes); // (the clock of the estimate begins here)
    // The composer keeps the package in memory while it works; the stream of the installed title, the stack and the buffer come on top. A heap that cannot hold
    // it is told now, before anything is written, in words (the composer would only say "no memory").
    const dlpc_job sizes = SizesJob(child_model_bin, child_model_bin_size, app_bytes, plugin_bytes_, job_.version16, job_.version_text.c_str());
    if (dlpc_memory_needed(&sizes) + kWorkerSlackBytes + kWorkerStack > dl_heap_free()) {
        return kPrepNoMemory;
    }
    state_.store(static_cast<int>(PrepareState::Running));
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // A little less important than the screen: the page keeps drawing and B keeps answering while the card and the hashes are worked.
    thread_ = threadCreate(Hooks::ThreadMain, this, kWorkerStack, prio < 0x3F ? prio + 1 : prio, -2, false);
    if (!thread_) {
        state_.store(static_cast<int>(PrepareState::Idle));
        return kPrepNoThread;
    }
    return 0;
}

void Preparer::Cancel() {
    cancel_.store(true);
}

void Preparer::Run() {
    Log("preparation of the package begins (run %d, version %s): output %s, app %s (%s), plugin: the one inside this app (%llu bytes)", job_.attempt, job_.version_text.c_str(),
        job_.out_path.c_str(), job_.app_cia_path.c_str(), job_.use_file ? "a good file" : "from the installed title", static_cast<unsigned long long>(setup::EmbeddedPluginSize()));
    setup::MakeDirs(setup::ParentDir(job_.marker_path));
    // The marker goes down first and comes up last: a run that does not reach the end leaves it, and the page asks about it next time.
    if (!setup::WriteTextFile(job_.marker_path, MarkerText(job_.attempt, job_.version_text))) {
        Log("the marker could not be written: %s", job_.marker_path.c_str());
    }
    std::remove((job_.out_path + ".part").c_str()); // (what a run that died left; the composer would write over it)
    std::remove((job_.app_cia_path + ".part").c_str());

    const int rc = Execute();

    std::remove(job_.marker_path.c_str());
    dl_cia_close();
    result_.store(rc);
    Log("preparation of the package ends: %d (%s)", rc, rc == DLPC_OK ? "done" : (rc < 100 ? dlpc_result_text(static_cast<dlpc_result>(rc)) : (PrepPhrase(rc) ? PrepPhrase(rc) : "?")));
    state_.store(static_cast<int>(rc == DLPC_OK ? PrepareState::Done : (rc == DLPC_E_CANCEL ? PrepareState::Cancelled : PrepareState::Failed)));
    finished_.store(true);
}

// The two ways to a package: compose from the file; and, when the file is not there or the composer does not take it, make the file from the installed title first.
int Preparer::Execute() {
    if (job_.use_file) {
        const int rc = Compose(job_.app_cia_path);
        if (rc == DLPC_OK || rc == DLPC_E_CANCEL || !job_.allow_title || (rc != DLPC_E_APP_BAD && rc != DLPC_E_READ_APP)) {
            return rc;
        }
        Log("the composer does not take %s (%d): it is made again from the installed title", job_.app_cia_path.c_str(), rc);
        file_refused_.store(true);
        SetDetail(std::string());
        RestartPlan(true, job_.app_bytes > 0 ? job_.app_bytes : kUsualAppBytes); // (the bar and the clock begin again: the first attempt's time is not part of this one)
    }
    if (!job_.allow_title) {
        return kPrepNoCopy;
    }
    int rc = CopyFromTitle();
    if (rc != DLPC_OK) {
        return rc;
    }
    return Compose(job_.app_cia_path);
}

int Preparer::CopyFromTitle() {
    SetProgress(kStageCopy, 0, 1);
    u64 size = 0;
    char why[160];
    const int open = dl_cia_open(kAppTid, &size, why, sizeof(why));
    if (open != DL_CIA_OK) {
        Log("the installed title could not be read: %d %s", open, why);
        title_error_.store(open);
        SetDetail(why);
        return kPrepNoCopy;
    }
    SetPlan(true, size);
    Log("the installed 3DS Link is %llu bytes as a CIA", static_cast<unsigned long long>(size));
    const CiaFacts facts = InspectCia([](u64 off, u8* buf, std::size_t len) { return dl_cia_read(off, buf, len) == 0; }, size);
    if (!facts.parsed || facts.tid != kAppTid || facts.version != job_.version16) {
        Log("the installed title is %016llX version %u (wanted version %u)", static_cast<unsigned long long>(facts.tid), static_cast<unsigned>(facts.version),
            job_.version16);
        return kPrepAppVersion;
    }
    const std::string part = job_.app_cia_path + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        return DLPC_E_WRITE;
    }
    std::vector<u8> block(kCopyBlock);
    int rc = DLPC_OK;
    for (u64 off = 0; off < size && rc == DLPC_OK;) {
        if (cancel_.load()) {
            rc = DLPC_E_CANCEL;
            break;
        }
        const std::size_t n = size - off < kCopyBlock ? static_cast<std::size_t>(size - off) : kCopyBlock;
        if (dl_cia_read(off, block.data(), n) != 0) {
            rc = kPrepNoCopy;
        } else if (std::fwrite(block.data(), 1, n, out) != n) {
            rc = DLPC_E_WRITE;
        }
        off += n;
        SetProgress(kStageCopy, off, size);
    }
    if (std::fclose(out) != 0 && rc == DLPC_OK) {
        rc = DLPC_E_WRITE;
    }
    dl_cia_close();
    if (rc == DLPC_OK) {
        // On the card a rename does not replace: the file that was there (the one that was no good) goes first. A cut between the two leaves no file, and the next run
        // makes it again.
        std::remove(job_.app_cia_path.c_str());
        if (std::rename(part.c_str(), job_.app_cia_path.c_str()) != 0) {
            rc = DLPC_E_WRITE;
        }
    }
    if (rc != DLPC_OK) {
        std::remove(part.c_str());
    }
    return rc;
}

int Preparer::Compose(const std::string& app_path) {
    FILE* app = std::fopen(app_path.c_str(), "rb");
    if (!app) {
        return DLPC_E_READ_APP;
    }
    const u64 app_size = SizeOfFile(app);
    const u64 plugin_size = setup::EmbeddedPluginSize();
    plugin_bytes_ = plugin_size;
    SetPlan(plan_copy_, app_size); // (the sizes are the real ones now)

    dlpc_job job = SizesJob(child_model_bin, child_model_bin_size, app_size, plugin_size, job_.version16, job_.version_text.c_str());
    job.read_app = &Hooks::ReadFile;
    job.app_user = app;
    job.read_plugin = &Hooks::ReadEmbeddedPlugin;
    job.plugin_user = nullptr;
    job.out_path = job_.out_path.c_str();
    job.emu_variant = 0; // (the package that is delivered: the one that only Azahar installs is for the tests)
    job.progress = &Hooks::Progress;
    job.progress_user = this;

    char err[160] = "";
    const int rc = static_cast<int>(dlpc_compose(&job, err, sizeof(err)));
    std::fclose(app);
    if (rc != DLPC_OK) {
        Log("the composer failed: %d (%s) %s", rc, dlpc_result_text(static_cast<dlpc_result>(rc)), err);
        SetDetail(err);
    }
    return rc;
}

} // namespace dlp
