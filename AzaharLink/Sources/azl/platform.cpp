#include "azl/platform.hpp"

#include "azl/log.hpp"
#include "azl/ticks.h"

#if defined(__3DS__) && defined(AZL_USE_CTRPF)
#include <CTRPluginFramework.hpp>
#elif defined(__3DS__)
#include <3ds.h>
#else
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#endif

namespace azl {
namespace platform {

#ifdef __3DS__

namespace {
int g_preferred_core = -1;
} // namespace

void SetPreferredCore(int core) {
    g_preferred_core = core;
}

// The clocks are read by the svc hook on the game's threads (the stamp of a line, the time of a call): nothing in them
// may format (Tests/policy_check.sh).
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
u64 NowTicks() {
    return svcGetSystemTick();
}

u64 NowMs() {
    // Monotonic: the system tick runs at SYSCLOCK_ARM11 Hz regardless of RTC changes. (A multiplication
    // instead of a 64-bit division, see azl/ticks.h.)
    return azl_ticks_to_ms(svcGetSystemTick());
}

u64 NowUs() {
    return azl_ticks_to_us(svcGetSystemTick());
}
// <<< game-thread code

// The network thread's waits (azl/platform.hpp). The thread is told apart by its thread-local-storage block: one register
// read, no call (the engine's lock asks from the game's threads too, and a C++ thread_local would be a call into the
// framework's thread bookkeeping there).
namespace {
volatile uintptr_t g_net_thread = 0;
u64 g_net_wait = 0; // (written and read by the network thread alone)
} // namespace

void MarkNetThread() {
    g_net_thread = reinterpret_cast<uintptr_t>(getThreadLocalStorage());
}

void ForgetNetThread() {
    if (g_net_thread == reinterpret_cast<uintptr_t>(getThreadLocalStorage())) {
        g_net_thread = 0;
    }
}

bool IsNetThread() {
    return g_net_thread != 0 && g_net_thread == reinterpret_cast<uintptr_t>(getThreadLocalStorage());
}

// The log writer thread (azl/platform.hpp), told apart the same way.
namespace {
volatile uintptr_t g_log_writer_thread = 0;
} // namespace

void MarkLogWriterThread() {
    g_log_writer_thread = reinterpret_cast<uintptr_t>(getThreadLocalStorage());
}

bool IsLogWriterThread() {
    return g_log_writer_thread != 0 && g_log_writer_thread == reinterpret_cast<uintptr_t>(getThreadLocalStorage());
}

void NoteNetWait(u64 ticks) {
    if (IsNetThread()) {
        g_net_wait += ticks;
    }
}

u64 NetWaitTicks() {
    return g_net_wait;
}

int CurrentCore() {
    return static_cast<int>(svcGetProcessorID());
}

void SleepMs(u32 ms) {
    svcSleepThread(static_cast<s64>(ms) * 1000000LL);
}

#ifdef AZL_USE_CTRPF

struct Thread::Impl {
    Impl(std::function<void()> f, int affinity, u32 priority)
        : fn(std::move(f)),
          thread(&Impl::Entry, kStackSize, priority, affinity) {}

    static void Entry(void* arg) {
        static_cast<Impl*>(arg)->fn();
    }

    static constexpr u32 kStackSize = 0x8000;

    std::function<void()> fn;
    CTRPluginFramework::ThreadEx thread;
    bool started = false;
    int affinity_used = -2;
};

Thread::Thread() = default;

Thread::~Thread() {
    Join();
}

bool Thread::Start(std::function<void()> fn, int core, int priority) {
    Join();
    // The requested core first (-1 lets the kernel pick; the syscore (1) is only usable if the game
    // was granted CPU time there), then every other option: some configurations refuse some cores.
    const int first = core == kCoreDefault ? g_preferred_core : core;
    const int kAffinities[] = {first, -1, 0, 1};
    for (int affinity : kAffinities) {
        impl_.reset(new Impl(fn, affinity, static_cast<u32>(priority)));
        const Result rc = impl_->thread.Start(impl_.get());
        if (R_SUCCEEDED(rc)) {
            impl_->started = true;
            impl_->affinity_used = affinity;
            if (affinity != first) {
                Log("thread: affinity %d refused, running with affinity %d", first, affinity);
            }
            return true;
        }
        Log("thread: creating a thread with affinity %d failed (0x%08lx)", affinity,
            static_cast<unsigned long>(rc));
        impl_.reset();
    }
    return false;
}

bool Thread::JoinTimeout(u32 ms) {
    if (!impl_) {
        return true;
    }
    if (!impl_->started) {
        impl_.reset();
        return true;
    }
    const Result rc = impl_->thread.JoinTimeout(true, static_cast<u64>(ms) * 1000000ull);
    // Exactly 0 means "joined": the kernel's timeout code (0x09401BFE) is not negative, so
    // R_SUCCEEDED() would call a timeout a success.
    if (rc == 0) {
        impl_.reset();
        return true;
    }
    // The thread never finished (starved, or stuck in a service call). Its stack and bookkeeping
    // must stay valid for as long as it may still run: leak them on purpose.
    (void)impl_.release();
    return false;
}

void Thread::Join() {
    if (impl_) {
        if (impl_->started) {
            impl_->thread.Join(true);
        }
        impl_.reset();
    }
}

bool Thread::running() const {
    return impl_ && impl_->started &&
           impl_->thread.GetStatus() != CTRPluginFramework::ThreadEx::FINISHED;
}

int Thread::core() const {
    return impl_ ? impl_->affinity_used : -2;
}

#else // plain libctru (homebrew application)

struct Thread::Impl {
    explicit Impl(std::function<void()> f) : fn(std::move(f)) {}

    static void Entry(void* arg) {
        static_cast<Impl*>(arg)->fn();
    }

    std::function<void()> fn;
    ::Thread thread = nullptr; // libctru's handle type
};

Thread::Thread() = default;

Thread::~Thread() {
    Join();
}

bool Thread::Start(std::function<void()> fn, int /*core*/, int /*priority*/) {
    Join();
    impl_.reset(new Impl(std::move(fn)));
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    impl_->thread = threadCreate(&Impl::Entry, impl_.get(), 0x8000, priority - 1, -2, false);
    if (!impl_->thread) {
        impl_.reset();
        return false;
    }
    return true;
}

void Thread::Join() {
    if (impl_) {
        if (impl_->thread) {
            threadJoin(impl_->thread, U64_MAX);
            threadFree(impl_->thread);
        }
        impl_.reset();
    }
}

bool Thread::JoinTimeout(u32 ms) {
    if (!impl_) {
        return true;
    }
    if (impl_->thread && threadJoin(impl_->thread, static_cast<u64>(ms) * 1000000ull) != 0) {
        (void)impl_.release(); // timed out (the timeout code is not a negative result)
        return false;
    }
    if (impl_->thread) {
        threadFree(impl_->thread);
    }
    impl_.reset();
    return true;
}

bool Thread::running() const {
    return impl_ && impl_->thread;
}

int Thread::core() const {
    return -2;
}

#endif // AZL_USE_CTRPF

#else // host

void SetPreferredCore(int) {}

// The network thread's waits (azl/platform.hpp): the thread is told apart by the address of a thread-local byte.
namespace {
thread_local char t_thread_tag;
std::atomic<uintptr_t> g_net_thread{0};
std::atomic<u64> g_net_wait{0};
} // namespace

void MarkNetThread() {
    g_net_thread.store(reinterpret_cast<uintptr_t>(&t_thread_tag));
}

void ForgetNetThread() {
    uintptr_t me = reinterpret_cast<uintptr_t>(&t_thread_tag);
    g_net_thread.compare_exchange_strong(me, 0);
}

bool IsNetThread() {
    return g_net_thread.load() == reinterpret_cast<uintptr_t>(&t_thread_tag);
}

namespace {
std::atomic<uintptr_t> g_log_writer_thread{0};
} // namespace

void MarkLogWriterThread() {
    g_log_writer_thread.store(reinterpret_cast<uintptr_t>(&t_thread_tag));
}

bool IsLogWriterThread() {
    const uintptr_t writer = g_log_writer_thread.load();
    return writer != 0 && writer == reinterpret_cast<uintptr_t>(&t_thread_tag);
}

void NoteNetWait(u64 ticks) {
    if (IsNetThread()) {
        g_net_wait.fetch_add(ticks);
    }
}

u64 NetWaitTicks() {
    return g_net_wait.load();
}

int CurrentCore() {
    return -1;
}

u64 NowTicks() {
    using namespace std::chrono;
    return static_cast<u64>(
        static_cast<double>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) *
        0.268111856);
}

// (Milliseconds and microseconds come from the same tick as on the console: code that reads the clock as ticks and
// as milliseconds, like the network loop, must never see the two disagree by a millisecond. A separate
// truncation of the clock to milliseconds did, once in a few dozen runs, and a pause of 49 days was "measured".)
u64 NowMs() {
    return azl_ticks_to_ms(NowTicks());
}

u64 NowUs() {
    return azl_ticks_to_us(NowTicks());
}

void SleepMs(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Thread::Impl {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
};

Thread::Thread() = default;

Thread::~Thread() {
    Join();
}

bool Thread::Start(std::function<void()> fn, int, int) {
    Join();
    impl_.reset(new Impl);
    const std::shared_ptr<std::atomic<bool>> done = impl_->done;
    impl_->thread = std::thread([fn = std::move(fn), done]() {
        fn();
        done->store(true);
    });
    return true;
}

void Thread::Join() {
    if (impl_) {
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
        impl_.reset();
    }
}

bool Thread::JoinTimeout(u32 ms) {
    if (!impl_) {
        return true;
    }
    // Like the console's: a thread that is not done in time is abandoned (detached; whatever it uses must outlive it).
    for (const u64 until = NowMs() + ms; !impl_->done->load() && NowMs() < until;) {
        SleepMs(1);
    }
    if (!impl_->done->load()) {
        impl_->thread.detach();
        impl_.reset();
        return false;
    }
    Join();
    return true;
}

bool Thread::running() const {
    return impl_ && impl_->thread.joinable();
}

int Thread::core() const {
    return -2;
}

#endif

} // namespace platform
} // namespace azl
