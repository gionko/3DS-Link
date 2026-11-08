#include "update_worker.hpp"

#include <atomic>
#include <cstring>
#include <ctime>

#include "cacert_pem.h" // certs/cacert.pem, linked in by bin2o: Mozilla's roots, as curl.se publishes them
#include "dlp_prepare_logic.hpp" // (dlp::kAppTid: the title id of the installed app)
#include "fileio.hpp"
#include "public_lobbies.hpp"
#include "settings.hpp"
#include "update_http.hpp"

namespace update {

namespace {

constexpr size_t kThreadStack = 128 * 1024; // (a TLS handshake: mbedTLS's numbers live on the stack)
constexpr u32 kBlock = 256 * 1024;
constexpr long kCheckSeconds = 60;
constexpr long kChangelogSeconds = 30;
constexpr long kListsSeconds = 30;
constexpr Result kNotVerified = static_cast<Result>(0xC8A04FFF); // (no code of AM's: AM finished, and does not show the title at this version)

} // namespace

struct Worker::Impl {
    mutable LightLock lock;
    Status status;
    std::atomic<bool> cancel{false};
    std::atomic<u32> done{0}, total{0};
    Thread thread = nullptr;
    bool http_ready = false;
    // The job
    enum class Job { Check, Install, Lists };
    Job job = Job::Check;
    bool install = false;
    std::string list_path, titles_path;
    ListsStatus lists;
    Target target = Target::None;
    std::string version, path;
};

namespace {

void Finish(Worker::Impl* impl, const Status& status) {
    LightLock_Lock(&impl->lock);
    const std::string latest = impl->status.latest;
    impl->status = status;
    if (impl->status.latest.empty()) {
        impl->status.latest = latest;
    }
    LightLock_Unlock(&impl->lock);
}

void FailNet(Worker::Impl* impl, NetResult result) {
    Status status;
    status.stage = Stage::Failed;
    status.failure = Failure::Net;
    status.net = result;
    Finish(impl, status);
}

void SetStage(Worker::Impl* impl, Stage stage) {
    LightLock_Lock(&impl->lock);
    impl->status.stage = stage;
    LightLock_Unlock(&impl->lock);
}

void RunCheck(Worker::Impl* impl) {
    HttpControl control;
    control.cancel = &impl->cancel;
    control.total_seconds = kCheckSeconds;
    std::string location;
    const NetResult asked = GetRedirectTarget(kLatestUrl, control, location);
    if (asked != NetResult::Ok) {
        FailNet(impl, asked);
        return;
    }
    Status status;
    status.latest = VersionFromLocation(location);
    if (status.latest.empty()) {
        FailNet(impl, NetResult::Server);
        return;
    }
    if (IsNewer(status.latest, setup::kVersion)) {
        // (a summary that cannot be had is no reason not to offer the version)
        control.total_seconds = kChangelogSeconds;
        ByteBuffer text;
        if (Download(ChangelogUrl(status.latest), control, kMaxChangelogBytes, text) == NetResult::Ok) {
            status.whats_new = WhatsNew(std::string(reinterpret_cast<const char*>(text.data()), text.size()), status.latest);
        }
    }
    if (impl->cancel.load()) {
        FailNet(impl, NetResult::Cancelled);
        return;
    }
    status.stage = Stage::Checked;
    Finish(impl, status);
}

// Through AM, the way the installer of 3DS Link does it (DlpLink/child), but the old title is NOT deleted first: this is the title that is running. AM takes the new one
// aside and puts it in place at the Finish, so a failure at any step leaves the old one as it was.
Result InstallCia(const std::uint8_t* data, std::size_t size, u64 title_id, u16 version) {
    bool available = false;
    AM_QueryAvailableExternalTitleDatabase(&available);
    AM_DeletePendingTitle(MEDIATYPE_SD, title_id); // (what an install that was cut may have left)
    Handle handle = 0;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &handle);
    if (R_FAILED(rc)) {
        return rc;
    }
    for (std::size_t off = 0; off < size;) {
        const u32 want = size - off < kBlock ? static_cast<u32>(size - off) : kBlock;
        u32 written = 0;
        rc = FSFILE_Write(handle, &written, off, data + off, want, 0);
        if (R_FAILED(rc) || written != want) {
            AM_CancelCIAInstall(handle);
            return R_FAILED(rc) ? rc : kNotVerified;
        }
        off += want;
    }
    rc = AM_FinishCiaInstall(handle);
    if (R_FAILED(rc)) {
        return rc;
    }
    AM_TitleEntry after;
    std::memset(&after, 0, sizeof(after));
    rc = AM_GetTitleInfo(MEDIATYPE_SD, 1, &title_id, &after);
    return R_FAILED(rc) ? rc : (after.version == version ? 0 : kNotVerified);
}

void RunInstall(Worker::Impl* impl) {
    HttpControl control;
    control.cancel = &impl->cancel;
    control.done = &impl->done;
    control.total = &impl->total;
    ByteBuffer file;
    const NetResult got = Download(AssetUrl(impl->version, impl->target), control, kMaxFileBytes, file);
    if (got != NetResult::Ok) {
        FailNet(impl, got);
        return;
    }
    Status status;
    status.latest = impl->version;
    Version wanted;
    CiaIdentity identity;
    bool right_file = false;
    if (impl->target == Target::Cia) {
        right_file = ParseVersion(impl->version, wanted) && ReadCiaIdentity(file.data(), file.size(), identity) && identity.title_id == dlp::kAppTid &&
                     identity.version == TmdVersion(wanted);
    } else {
        right_file = Looks3dsx(file.data(), file.size());
    }
    if (!right_file) {
        status.stage = Stage::Failed;
        status.failure = Failure::WrongFile;
        Finish(impl, status);
        return;
    }
    SetStage(impl, Stage::Installing);
    if (impl->target == Target::Cia) {
        status.am_rc = InstallCia(file.data(), file.size(), identity.title_id, identity.version);
        if (R_FAILED(status.am_rc)) {
            status.stage = Stage::Failed;
            status.failure = Failure::Am;
            Finish(impl, status);
            return;
        }
    } else {
        // (the same careful writing as the plugin's: a .part file read back, the old file put aside, then the swap)
        status.write = setup::WritePluginFile(impl->path, file.data(), file.size());
        if (status.write != setup::WriteResult::Ok) {
            status.stage = Stage::Failed;
            status.failure = Failure::File;
            Finish(impl, status);
            return;
        }
    }
    status.stage = Stage::Installed;
    Finish(impl, status);
}

void SetLists(Worker::Impl* impl, const ListsStatus& lists) {
    LightLock_Lock(&impl->lock);
    impl->lists = lists;
    LightLock_Unlock(&impl->lock);
}

void FailLists(Worker::Impl* impl, NetResult result) {
    ListsStatus failed;
    failed.stage = ListsStatus::Stage::Failed;
    failed.net = result;
    SetLists(impl, failed);
}

// A whole list that is not the one the page uses waits in <path>.new for the page.
bool WriteIfNew(const std::string& path, const std::string& text, std::size_t max_bytes) {
    std::string old;
    if (setup::ReadTextFile(path, max_bytes + 1, old) == setup::LoadResult::Loaded && old == text) {
        return false; // (a list that waits stays: it is newer than one GitHub's cache may still hand out for a few minutes)
    }
    return setup::WriteTextFile(path + ".new", text);
}

void RunLists(Worker::Impl* impl) {
    HttpControl control;
    control.cancel = &impl->cancel;
    control.total_seconds = kListsSeconds;
    ByteBuffer body;
    const NetResult got = Download(lobbies::kListUrl, control, lobbies::kMaxListBytes, body);
    if (got != NetResult::Ok) {
        FailLists(impl, got);
        return;
    }
    const std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    lobbies::List list;
    if (!lobbies::ParseList(text, list)) {
        FailLists(impl, NetResult::Server);
        return;
    }
    ListsStatus status;
    status.list_changed = WriteIfNew(impl->list_path, text, lobbies::kMaxListBytes);
    std::string titles_text, waiting;
    setup::ReadTextFile(impl->titles_path, lobbies::kMaxTitlesBytes + 1, titles_text);
    if (setup::ReadTextFile(impl->titles_path + ".new", lobbies::kMaxTitlesBytes + 1, waiting) == setup::LoadResult::Loaded && !waiting.empty()) {
        titles_text = waiting; // (titles that came before and wait for the page are as new)
    }
    if (!list.titles_stamp.empty() && lobbies::HeaderValue(titles_text, "titles") != list.titles_stamp && !impl->cancel.load()) {
        ByteBuffer titles_body;
        if (Download(lobbies::kTitlesUrl, control, lobbies::kMaxTitlesBytes, titles_body) == NetResult::Ok) {
            const std::string fresh(reinterpret_cast<const char*>(titles_body.data()), titles_body.size());
            lobbies::Titles titles;
            // (the file must be the one the list names: GitHub's cache can still hand out the last one for a few minutes)
            if (titles.Parse(fresh) && titles.stamp() == list.titles_stamp) {
                status.titles_changed = setup::WriteTextFile(impl->titles_path + ".new", fresh);
            }
        }
    }
    status.stage = ListsStatus::Stage::Done;
    SetLists(impl, status);
}

void ThreadMain(void* arg) {
    Worker::Impl* impl = static_cast<Worker::Impl*>(arg);
    // mbedTLS seeds itself with sslcGenerateRandomData and does not look at its result: without the service open its keys would be predictable. So no service, no request. (This
    // is the only thing the console's own SSL module is asked for: TLS itself is mbedTLS, whatever the firmware.)
    const bool lists = impl->job == Worker::Impl::Job::Lists;
    if (R_FAILED(sslcInit(0))) {
        if (lists) {
            FailLists(impl, NetResult::Failed);
        } else {
            FailNet(impl, NetResult::Failed);
        }
        return;
    }
    if (!impl->http_ready) {
        SetCaBundle(cacert_pem, cacert_pem_size);
        impl->http_ready = HttpInit();
    }
    if (!impl->http_ready) {
        if (lists) {
            FailLists(impl, NetResult::Failed);
        } else {
            FailNet(impl, NetResult::Failed);
        }
    } else if (lists) {
        RunLists(impl);
    } else if (impl->install) {
        RunInstall(impl);
    } else {
        RunCheck(impl);
    }
    sslcExit();
}

} // namespace

std::string FailureText(const Status& status) {
    switch (status.failure) {
    case Failure::Net:
        return NetFailureText(status.net);
    case Failure::WrongFile:
        return kWrongFileText;
    case Failure::Am:
        return InstallFailureText(static_cast<std::uint32_t>(status.am_rc));
    case Failure::File:
        return FileFailureText(setup::WriteFailurePhrase(status.write));
    case Failure::Busy:
        return kBusyText;
    default:
        return std::string();
    }
}

int ConsoleYear() {
    const std::time_t now = std::time(nullptr);
    const std::tm* parts = std::gmtime(&now);
    return parts ? parts->tm_year + 1900 : 0;
}

Worker& Worker::Get() {
    static Worker worker;
    return worker;
}

Worker::Worker() : impl_(new Impl) {
    LightLock_Init(&impl_->lock);
}

bool Worker::Begin(Stage first) {
    if (impl_->thread) {
        // threadJoin answers a positive code on a timeout: only 0 says that the thread is over (R_SUCCEEDED would take the timeout for an end).
        if (threadJoin(impl_->thread, 0) != 0) {
            return false;
        }
        threadFree(impl_->thread);
        impl_->thread = nullptr;
    }
    impl_->cancel.store(false);
    impl_->done.store(0);
    impl_->total.store(0);
    Status status;
    status.stage = first;
    LightLock_Lock(&impl_->lock);
    status.latest = impl_->status.latest;
    impl_->status = status;
    LightLock_Unlock(&impl_->lock);
    if (!Spawn()) {
        FailNet(impl_, NetResult::NoMemory);
    }
    return true;
}

bool Worker::Spawn() {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    impl_->thread = threadCreate(ThreadMain, impl_, kThreadStack, priority < 0x3F ? priority + 1 : priority, -2, false);
    return impl_->thread != nullptr;
}

bool Worker::StartCheck() {
    if (impl_->thread && threadJoin(impl_->thread, 0) != 0) {
        return false; // (the fields below belong to the job that runs)
    }
    impl_->install = false;
    impl_->job = Impl::Job::Check;
    return Begin(Stage::Checking);
}

bool Worker::StartInstall(Target target, const std::string& version, const std::string& path) {
    if (impl_->thread && threadJoin(impl_->thread, 0) != 0) {
        return false; // (the fields below belong to the job that runs)
    }
    impl_->install = true;
    impl_->job = Impl::Job::Install;
    impl_->target = target;
    impl_->version = version;
    impl_->path = path;
    return Begin(Stage::Downloading);
}

bool Worker::StartLists(const std::string& list_path, const std::string& titles_path) {
    if (impl_->thread) {
        if (threadJoin(impl_->thread, 0) != 0) {
            return false; // (the fields below belong to the job that runs)
        }
        threadFree(impl_->thread);
        impl_->thread = nullptr;
    }
    impl_->cancel.store(false);
    impl_->job = Impl::Job::Lists;
    impl_->install = false;
    impl_->list_path = list_path;
    impl_->titles_path = titles_path;
    ListsStatus running;
    running.stage = ListsStatus::Stage::Running;
    SetLists(impl_, running);
    if (!Spawn()) {
        FailLists(impl_, NetResult::NoMemory);
    }
    return true;
}

bool Worker::ListsRunning() const {
    return impl_->thread && impl_->job == Impl::Job::Lists && threadJoin(impl_->thread, 0) != 0;
}

ListsStatus Worker::ReadLists() const {
    LightLock_Lock(&impl_->lock);
    const ListsStatus lists = impl_->lists;
    LightLock_Unlock(&impl_->lock);
    return lists;
}

Status Worker::Read() const {
    LightLock_Lock(&impl_->lock);
    Status status = impl_->status;
    LightLock_Unlock(&impl_->lock);
    status.done = impl_->done.load();
    status.total = impl_->total.load();
    return status;
}

void Worker::Cancel() {
    impl_->cancel.store(true);
}

bool Worker::Stop(u64 timeout_ms) {
    if (!impl_->thread) {
        return true;
    }
    Cancel();
    if (threadJoin(impl_->thread, timeout_ms * 1000000ULL) != 0) {
        return false;
    }
    threadFree(impl_->thread);
    impl_->thread = nullptr;
    return true;
}

void Worker::Shutdown() {
    const bool over = Stop(30000);
    if (over && impl_->http_ready) {
        HttpExit();
        impl_->http_ready = false;
    }
}

} // namespace update
