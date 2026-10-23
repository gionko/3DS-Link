#include "copyworker.hpp"

#include <deque>
#include <vector>

namespace setup {

namespace {

struct Job {
    u64 tid = 0;
    CopyKind kind = CopyKind::Enable;
    std::string source;
    bool refresh = false; // a quiet look: the copy is made again only when it differs, and nobody is told unless it was
};

} // namespace

struct CopyWorker::Impl {
    LightLock lock;
    std::deque<Job> queue;
    std::deque<CopyDone> done;
    int refreshed = 0;    // the copies the quiet looks made (not yet asked for)
    bool running = false; // a job is being made (it is not in the queue any more)
    Job current;
    bool stop = false;
    Thread thread = nullptr;
};

namespace {

// Makes one job: copies the plugin into the title's folder, or takes the copy away.
CopyDone RunJob(const Job& job, bool* refreshed = nullptr) {
    CopyDone result;
    result.tid = job.tid;
    result.kind = job.kind;
    if (job.refresh) {
        bool copied = false;
        result.enable_result = RefreshGameCopy(job.tid, job.source.c_str(), &copied);
        if (refreshed) {
            *refreshed = copied;
        }
        return result;
    }
#ifdef AZL_TEST_COPY_DELAY_MS
    svcSleepThread(static_cast<s64>(AZL_TEST_COPY_DELAY_MS) * 1000000LL); // (a build made to look at the screens of a slow card on an emulator)
#endif
    if (job.kind == CopyKind::Disable) {
        result.disable_ok = DisableGame(job.tid);
    } else {
        result.enable_result = EnableGame(job.tid, job.source.c_str());
    }
    return result;
}

void WorkerMain(void* arg) {
    CopyWorker::Impl* impl = static_cast<CopyWorker::Impl*>(arg);
    for (;;) {
        LightLock_Lock(&impl->lock);
        if (impl->queue.empty()) {
            const bool leave = impl->stop;
            LightLock_Unlock(&impl->lock);
            if (leave) {
                return;
            }
            svcSleepThread(30 * 1000000LL); // (idle: a look every 30 ms costs nothing)
            continue;
        }
        impl->current = impl->queue.front();
        impl->queue.pop_front();
        impl->running = true;
        const Job job = impl->current;
        LightLock_Unlock(&impl->lock);

        bool refreshed = false;
        const CopyDone result = RunJob(job, &refreshed);

        LightLock_Lock(&impl->lock);
        if (job.refresh) {
            impl->refreshed += refreshed ? 1 : 0; // (a quiet look reports only that it made a copy)
        } else {
            impl->done.push_back(result);
        }
        impl->running = false;
        LightLock_Unlock(&impl->lock);
    }
}

} // namespace

CopyWorker::CopyWorker() : impl_(new Impl) {
    LightLock_Init(&impl_->lock);
}

CopyWorker& CopyWorker::Get() {
    static CopyWorker* worker = new CopyWorker; // (lives as long as the program: the thread may be running when it ends)
    return *worker;
}

void CopyWorker::SubmitRefresh(u64 tid, const std::string& source) {
    Submit(tid, CopyKind::Update, source, true);
}

int CopyWorker::TakeRefreshedCount() {
    LightLock_Lock(&impl_->lock);
    const int n = impl_->refreshed;
    impl_->refreshed = 0;
    LightLock_Unlock(&impl_->lock);
    return n;
}

void CopyWorker::Submit(u64 tid, CopyKind kind, const std::string& source) {
    Submit(tid, kind, source, false);
}

void CopyWorker::Submit(u64 tid, CopyKind kind, const std::string& source, bool refresh) {
    LightLock_Lock(&impl_->lock);
    Job job;
    job.tid = tid;
    job.kind = kind;
    job.source = source;
    job.refresh = refresh;
    impl_->queue.push_back(job);
    const bool need_thread = impl_->thread == nullptr;
    LightLock_Unlock(&impl_->lock);
    if (need_thread) {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        // A little below the thread that draws, so the list keeps moving while the card is written.
        impl_->thread = threadCreate(WorkerMain, impl_, 64 * 1024, priority + 1, -2, false);
        if (!impl_->thread) {
            // No thread: the jobs are made here, one after the other (the page stands still meanwhile, as it did before there was a thread).
            LightLock_Lock(&impl_->lock);
            std::deque<Job> jobs;
            jobs.swap(impl_->queue);
            LightLock_Unlock(&impl_->lock);
            for (const Job& j : jobs) {
                bool refreshed = false;
                const CopyDone result = RunJob(j, &refreshed);
                LightLock_Lock(&impl_->lock);
                if (j.refresh) {
                    impl_->refreshed += refreshed ? 1 : 0;
                } else {
                    impl_->done.push_back(result);
                }
                LightLock_Unlock(&impl_->lock);
            }
        }
    }
}

bool CopyWorker::Pending(u64 tid, CopyKind* kind) const {
    LightLock_Lock(&impl_->lock);
    bool found = false;
    if (impl_->running && impl_->current.tid == tid) {
        found = true;
        if (kind) {
            *kind = impl_->current.kind;
        }
    }
    for (const Job& job : impl_->queue) {
        if (!found && job.tid == tid) {
            found = true;
            if (kind) {
                *kind = job.kind;
            }
        }
    }
    LightLock_Unlock(&impl_->lock);
    return found;
}

int CopyWorker::PendingCount() const {
    LightLock_Lock(&impl_->lock);
    const int n = static_cast<int>(impl_->queue.size()) + (impl_->running ? 1 : 0);
    LightLock_Unlock(&impl_->lock);
    return n;
}

bool CopyWorker::TakeDone(CopyDone& out) {
    LightLock_Lock(&impl_->lock);
    const bool any = !impl_->done.empty();
    if (any) {
        out = impl_->done.front();
        impl_->done.pop_front();
    }
    LightLock_Unlock(&impl_->lock);
    return any;
}

bool CopyWorker::WaitIdle(u64 timeout_ms) {
    for (u64 waited = 0; PendingCount() > 0; waited += 20) {
        if (waited >= timeout_ms) {
            return false;
        }
        svcSleepThread(20 * 1000000LL);
    }
    return true;
}

void CopyWorker::Stop() {
    LightLock_Lock(&impl_->lock);
    impl_->stop = true;
    Thread thread = impl_->thread;
    LightLock_Unlock(&impl_->lock);
    if (thread && threadJoin(thread, 5ULL * 1000000000ULL) == 0) { // (not R_SUCCEEDED: a timeout, 0x09401BFE, is not negative)
        threadFree(thread); // (a thread that is still copying keeps its stack: it ends with the program)
        impl_->thread = nullptr;
    }
}

} // namespace setup
