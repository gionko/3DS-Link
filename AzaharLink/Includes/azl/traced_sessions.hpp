// The real sessions of the game that the hook follows (ctr/hook.cpp: APT:* for the HOME and power buttons, ndm:u, cfg:u, ptm:u, ...), remembered by handle.
// A session leaves the table when the game closes it through the hook (CloseHandle). Some closes are never seen: around the swap of an Old 3DS the
// gate passes the game's calls straight to the kernel (Sources/ctr/gate.s), and the SDK opens and closes an APT:A session for every APT call (about 13 at each HOME). Before this table, the slots
// leaked one by one and after the 12th HOME of Pokemon Alpha Sapphire (Old 3DS, issue 5, 1.1.1) the game's APT:A sessions were no longer followed:
// no HOME seen (1.1.1 then gave APT:ReceiveParameter back to the game's own call site after a HOME), and the game's thread woke up in the plugin's
// freed memory (dumps 3 and 5).
// Two rules keep the table true without seeing every close:
//   * on the console a handle is (counter << 15) | index in the kernel's handle table (KHandleTable::Add; the counter goes 1..0x7FFF and round): a new
//     handle with the index of an entry means that entry was closed - even with the very same value, after the counter went round;
//   * when the table is full, an APT session takes the slot of the APT session used least recently (the SDK's live for one call; a session the game
//     keeps is used and stays).
// (Azahar packs a handle the other way round, generation | slot << 15: there the first rule can only free a live entry when its generation comes back,
// some 32,000 handles later, and that costs the lines of that service in the log - Azahar has no swap, and nothing else rests on the table there.)
// Runs on the game's threads (srv:GetServiceHandle, CloseHandle, every IPC call): no lock, no wait. A slot is claimed with a compare-and-swap to
// kTracedClaiming, filled, and only then given its handle: nobody sees a slot half filled. (Tests/traced_sessions_test.cpp, Tests/traced_sessions_mutants.py)
#pragma once

#include <cstring>

#include "azl/common.hpp"

// Points where Tests/traced_sessions_test.cpp runs another thread's step (nothing in the plugin).
#ifndef AZL_TRACED_AFTER_CLAIM
#define AZL_TRACED_AFTER_CLAIM()
#endif
#ifndef AZL_TRACED_BEFORE_EVICT
#define AZL_TRACED_BEFORE_EVICT()
#endif

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

constexpr u32 kHandleIndexMask = 0x7FFF;      ///< the index of a handle in the console kernel's handle table (the bits above are a counter)
constexpr u32 kTracedClaiming = 0xFFFFFFFFu;  ///< a slot being filled (no handle has this value: 0xFFFF8000 and up are the pseudo-handles)

inline bool SameHandleSlot(u32 a, u32 b) {
    return (a & kHandleIndexMask) == (b & kHandleIndexMask);
}

struct TracedSession {
    volatile u32 handle; ///< 0 = free, kTracedClaiming = being filled
    char name[12];
    bool apt; ///< an APT session: the system's notifications (home, sleep) come through it
    u32 used; ///< when it was added or last used (the APT session used least recently gives its slot up first)
};

/// What the table had to do without the game's CloseHandle (for the log: a launch with these counts lost closes).
struct TracedStats {
    volatile u32 stale = 0;   ///< entries freed because their handle's index came back with a new session
    volatile u32 evicted = 0; ///< APT entries that gave their slot to a newer APT session
    volatile u32 dropped = 0; ///< sessions not followed: the table was full and the new one was not APT (or no APT entry to replace)
};

template <u32 N>
struct TracedTable {
    TracedSession slots[N] = {};
    volatile u32 clock = 0;
    TracedStats stats;

    TracedSession* Find(u32 handle) {
        if (handle == 0 || handle == kTracedClaiming) {
            return nullptr;
        }
        for (TracedSession& s : slots) {
            if (__atomic_load_n(&s.handle, __ATOMIC_ACQUIRE) == handle) {
                return &s;
            }
        }
        return nullptr;
    }

    /// A call of the game on a followed session (PassThrough): the entry is in use.
    void Touch(TracedSession& s) {
        __atomic_store_n(&s.used, Tick(), __ATOMIC_RELEASE);
    }

    /// The game closed `handle` through the hook.
    void Closed(u32 handle) {
        if (TracedSession* s = Find(handle)) {
            u32 expected = handle;
            __atomic_compare_exchange_n(&s->handle, &expected, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }

    /// A new session the kernel gave the game (srv's answer, followed or not): the entries with its index are closed sessions.
    void Opened(u32 session) {
        if (session == 0 || session == kTracedClaiming) {
            return;
        }
        for (TracedSession& s : slots) {
            u32 old = __atomic_load_n(&s.handle, __ATOMIC_ACQUIRE);
            if (old != 0 && old != kTracedClaiming && SameHandleSlot(old, session) &&
                __atomic_compare_exchange_n(&s.handle, &old, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_add(&stats.stale, 1u, __ATOMIC_RELAXED);
            }
        }
    }

    /// Follows `session` of the service `name`. Call Opened first. Returns true when it is followed.
    bool Add(u32 session, const char* name, bool apt) {
        if (session == 0 || session == kTracedClaiming) {
            return false;
        }
        for (TracedSession& s : slots) {
            u32 expected = 0;
            if (__atomic_compare_exchange_n(&s.handle, &expected, kTracedClaiming, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                AZL_TRACED_AFTER_CLAIM();
                Fill(s, session, name, apt);
                return true;
            }
        }
        if (!apt) {
            __atomic_fetch_add(&stats.dropped, 1u, __ATOMIC_RELAXED);
            return false;
        }
        // Full: the APT entry used least recently gives its slot up, if it still holds the handle seen here. (Another thread may take it
        // first: then this session is not followed, as before.)
        TracedSession* oldest = nullptr;
        u32 oldest_handle = 0;
        u32 oldest_used = 0;
        for (TracedSession& s : slots) {
            const u32 h = __atomic_load_n(&s.handle, __ATOMIC_ACQUIRE);
            if (h == 0 || h == kTracedClaiming || !__atomic_load_n(&s.apt, __ATOMIC_ACQUIRE)) {
                continue;
            }
            const u32 used = __atomic_load_n(&s.used, __ATOMIC_ACQUIRE);
            if (!oldest || static_cast<s32>(used - oldest_used) < 0) {
                oldest = &s;
                oldest_handle = h;
                oldest_used = used;
            }
        }
        AZL_TRACED_BEFORE_EVICT();
        if (oldest && __atomic_compare_exchange_n(&oldest->handle, &oldest_handle, kTracedClaiming, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            Fill(*oldest, session, name, apt);
            __atomic_fetch_add(&stats.evicted, 1u, __ATOMIC_RELAXED);
            return true;
        }
        __atomic_fetch_add(&stats.dropped, 1u, __ATOMIC_RELAXED);
        return false;
    }

private:
    u32 Tick() {
        return __atomic_add_fetch(&clock, 1u, __ATOMIC_RELAXED);
    }

    // The slot holds kTracedClaiming: nobody else reads or takes it until the handle is stored, last.
    void Fill(TracedSession& s, u32 session, const char* name, bool apt) {
        std::strncpy(s.name, name, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';
        __atomic_store_n(&s.apt, apt, __ATOMIC_RELAXED);
        __atomic_store_n(&s.used, Tick(), __ATOMIC_RELAXED);
        __atomic_store_n(&s.handle, session, __ATOMIC_RELEASE);
    }
};

// <<< game-thread code

} // namespace azl
