// Tiny platform abstraction so the protocol/UDS core builds both for the 3DS (libctru) and on
// a PC (used by the unit tests).
#pragma once

#include <functional>
#include <memory>

#include "azl/common.hpp"

#ifdef __3DS__
#include <3ds.h>
#else
#include <mutex>
#endif

namespace azl {
namespace platform {

#ifdef __3DS__
class Mutex {
public:
    Mutex() {
        LightLock_Init(&lock_);
    }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock() {
        LightLock_Lock(&lock_);
    }
    bool try_lock() {
        return LightLock_TryLock(&lock_) == 0;
    }
    void unlock() {
        LightLock_Unlock(&lock_);
    }

private:
    LightLock lock_;
};
#else
using Mutex = std::mutex;
#endif

/// Monotonic system ticks (3DS: svcGetSystemTick, 268111856 Hz; the host: the same scale from the steady
/// clock). The cheapest clock read: azl/ticks.h turns it into ms / us without a division.
u64 NowTicks();

/// The network thread's own waits. Whatever it is blocked on - a call to the soc process, the engine's lock, a pause after a
/// socket error - is time it is not running, and the CPU of its loop is the wall time of a section minus the waits that fell
/// in it (Stats: t_net_service ...). The thread marks itself once; a wait noted by any other thread is ignored, so the calls
/// can sit in shared code (the soc layer's timing, the lock) without knowing who runs them.
void MarkNetThread();
/// The calling thread is no longer the network thread (only if it still is the one that is marked).
void ForgetNetThread();
bool IsNetThread();
/// The log writer thread marks itself once (Plugin::StartLogWriter): it is the one thread that may WAIT for the log's
/// locks (it runs above the game's threads and blocks in the file system: nobody starves behind it). Every other thread tries them briefly and gives
/// the line up (azl/log.cpp LockLogBriefly, LogFiles::Append): a thread of the game that waited for a lock the preempted
/// writer held - a LightLock has no priority inheritance - would wait for every thread above the writer to leave the core,
/// and a game that spins at its splash screen never does (Tekken, 2026-10-20: stuck at the publisher's screen with the log
/// on, nothing stuck with it off).
void MarkLogWriterThread();
bool IsLogWriterThread();
/// A lock for a thread that must not wait: a few microseconds of trying (256 attempts), then false.
template <typename M>
inline bool TryLockBriefly(M& mutex) {
    for (unsigned i = 0; i < 256; ++i) {
        if (mutex.try_lock()) {
            return true;
        }
    }
    return false;
}
/// `ticks` the calling thread waited; counts only on the network thread.
void NoteNetWait(u64 ticks);
/// The running sum of the waits noted by the network thread (read by the network thread itself, to cut a section).
u64 NetWaitTicks();
/// The calling thread is the network thread for the life of this object (RoomClient::Run).
class NetThreadMark {
public:
    NetThreadMark() {
        MarkNetThread();
    }
    ~NetThreadMark() {
        ForgetNetThread();
    }
    NetThreadMark(const NetThreadMark&) = delete;
    NetThreadMark& operator=(const NetThreadMark&) = delete;
};

/// Counters a ContendedMutex fills (ticks are system ticks).
struct LockCounters {
    u32* contended = nullptr; ///< times lock() found the mutex taken
    Ticks64* wait_ticks = nullptr; ///< total time those calls waited
    u32* wait_max = nullptr;       ///< the longest wait
    Ticks64* wait_net_ticks = nullptr; ///< ... of the total, the part the network thread waited
};

/// A Mutex that notes how often (and for how long) lock() had to wait: it tries first and only reads
/// the clock when it has to block. The uncontended cost is that of a plain lock.
class ContendedMutex {
public:
    void Watch(const LockCounters& counters) {
        counters_ = counters;
    }
    void lock() {
        if (m_.try_lock()) {
            return;
        }
        const u64 begin = NowTicks();
        m_.lock();
        if (counters_.contended) {
            const u64 waited = NowTicks() - begin;
            ++*counters_.contended;
            // (atomic: the window's taker clears the sum without this lock)
            __atomic_fetch_add(counters_.wait_ticks, waited, __ATOMIC_RELAXED);
            const u32 clamped = waited > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(waited);
            if (clamped > *counters_.wait_max) {
                *counters_.wait_max = clamped;
            }
            if (IsNetThread()) {
                if (counters_.wait_net_ticks) {
                    __atomic_fetch_add(counters_.wait_net_ticks, waited, __ATOMIC_RELAXED);
                }
                NoteNetWait(waited);
            }
        }
    }
    void unlock() {
        m_.unlock();
    }

private:
    Mutex m_;
    LockCounters counters_;
};

template <class M>
class LockGuard {
public:
    explicit LockGuard(M& m) : m_(m) {
        m_.lock();
    }
    ~LockGuard() {
        m_.unlock();
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    M& m_;
};

/// Monotonic milliseconds.
u64 NowMs();
/// Monotonic microseconds (for timing short operations).
u64 NowUs();
void SleepMs(u32 ms);

/// Core the next worker threads should prefer (3DS only; -1 = let the kernel choose, 0 = the
/// game's core, 1 = the syscore). Only threads started with kCoreDefault use it.
void SetPreferredCore(int core);
/// The core the calling thread is running on (-1 on the host).
int CurrentCore();
constexpr int kCoreDefault = -100;

/// A joinable thread running a std::function.
class Thread {
public:
    Thread();
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    /// `core` / `priority` are hints for the 3DS (a lower priority number is more important).
    bool Start(std::function<void()> fn, int core = kCoreDefault, int priority = 0x2F);
    /// Waits for the thread to finish, at most `ms`. On timeout returns false and abandons the
    /// thread (its memory is leaked on purpose: it may still be running).
    bool JoinTimeout(u32 ms);
    void Join();
    bool running() const;
    /// Core the thread was created on (3DS: 0 app core, 1 syscore, -1 kernel's choice; -2 n/a).
    int core() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
} // namespace azl
