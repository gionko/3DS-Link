#include "lobby_scan.hpp"

#include <atomic>

namespace lobbies {

namespace {

constexpr size_t kThreadStack = 64 * 1024;
constexpr u32 kStepMs = 5; // (the socket is served this long at a time: a stop is seen within it)

} // namespace

struct Scanner::Impl {
    mutable LightLock lock;
    std::vector<Target> targets;  // the job (the thread reads it only while it runs)
    std::vector<Probe> results;
    std::atomic<u32> generation{0};
    std::atomic<bool> stop{false};
    Thread thread = nullptr;
};

namespace {

void PassMain(void* arg) {
    Scanner::Impl* impl = static_cast<Scanner::Impl*>(arg);
    PassOptions options;
    options.stop = &impl->stop;
    ProbePass pass(impl->targets, options);
    if (!pass.Begin()) {
        pass.Step(0); // (no socket: every room is offline)
    }
    bool more = true;
    while (more && !impl->stop.load()) {
        more = pass.Step(kStepMs);
        const std::vector<std::size_t> changed = pass.TakeChanged();
        if (changed.empty()) {
            continue;
        }
        LightLock_Lock(&impl->lock);
        for (std::size_t i : changed) {
            if (i < impl->results.size()) {
                impl->results[i] = pass.result(i);
            }
        }
        LightLock_Unlock(&impl->lock);
        impl->generation.fetch_add(1);
    }
    if (impl->stop.load()) {
        // (stopped half way: what was still being asked has no answer, and says so)
        LightLock_Lock(&impl->lock);
        for (Probe& p : impl->results) {
            if (p.state == State::Waiting || p.state == State::Asking) {
                p.state = State::Waiting;
            }
        }
        LightLock_Unlock(&impl->lock);
        impl->generation.fetch_add(1);
    }
}

} // namespace

Scanner& Scanner::Get() {
    static Scanner scanner;
    return scanner;
}

Scanner::Scanner() : impl_(new Impl) {
    LightLock_Init(&impl_->lock);
}

bool Scanner::Start(const std::vector<Target>& targets) {
    if (impl_->thread) {
        // threadJoin answers a positive code on a timeout: only 0 says that the thread is over (R_SUCCEEDED would take the timeout for an end).
        if (threadJoin(impl_->thread, 0) != 0) {
            impl_->stop.store(true);
            return false;
        }
        threadFree(impl_->thread);
        impl_->thread = nullptr;
    }
    impl_->stop.store(false);
    impl_->targets = targets;
    LightLock_Lock(&impl_->lock);
    impl_->results.assign(targets.size(), Probe());
    LightLock_Unlock(&impl_->lock);
    impl_->generation.fetch_add(1);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // (a little ABOVE the thread that draws: it sleeps in the socket nearly all the time, and an answer is taken when it comes, so the ping is the
    // room's and not the time the drawing kept the thread waiting)
    impl_->thread = threadCreate(PassMain, impl_, kThreadStack, priority > 0x18 ? priority - 1 : priority, -2, false);
    if (!impl_->thread) {
        // (no thread: the rooms are listed without their answers, nothing else is lost)
        return true;
    }
    return true;
}

bool Scanner::running() const {
    return impl_->thread && threadJoin(impl_->thread, 0) != 0;
}

u32 Scanner::generation() const {
    return impl_->generation.load();
}

void Scanner::Read(std::vector<Probe>& out) const {
    LightLock_Lock(&impl_->lock);
    out = impl_->results;
    LightLock_Unlock(&impl_->lock);
}

void Scanner::Stop() {
    impl_->stop.store(true);
}

bool Scanner::Shutdown(u64 timeout_ms) {
    if (!impl_->thread) {
        return true;
    }
    Stop();
    if (threadJoin(impl_->thread, timeout_ms * 1000000ULL) != 0) {
        return false; // (still in a name lookup: it is left, never freed under itself)
    }
    threadFree(impl_->thread);
    impl_->thread = nullptr;
    return true;
}

} // namespace lobbies
