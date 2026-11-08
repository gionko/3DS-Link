// The pure part of the look 3DS Link takes at a closed game that is still in memory (zombie_census.cpp does the system calls). Plain C++, no
// 3ds.h: Tests/settings_test/zombie_plan_test.cpp builds it on the PC.
//
// Why: after HOME -> Close a game can stay TERMINATED but never finalized (2026-11-07, Monster Hunter XX after a session with the chat): all its
// pages stay in the APPLICATION region until the console restarts, and the next launch of a game that sizes its heap to the byte panics. 3DS Link
// sees it at start (heap_plan: "foreign"). This look writes down, for the next analysis, which classes of the closed game's kernel objects are still
// alive (the APPLICATION limit's counters), the process itself (state, threads, references) and its shared memory blocks (the plugin's socket block
// at 0x0dfe0000 is one), and how long it stays. It reads only: no handle of another process is copied, nothing is written to the kernel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace zombie_plan {

// The kernel's resource classes, in the order of the APPLICATION limit's counters (ResourceLimitType 1..8 of libctru).
constexpr int kClasses = 8;
inline const char* ClassName(int i) {
    static const char* const kNames[kClasses] = {"commit KiB", "thread", "event", "mutex", "semaphore", "timer", "shmem", "arbiter"};
    return i >= 0 && i < kClasses ? kNames[i] : "?";
}

// A title id of a game (what a closed game left behind can only be one): 0004000x, with x = 0 (application), 1 (a Download Play child: Kirby's,
// 3DS Link Delivery's) or 2 (demo).
inline bool IsGameTitle(uint64_t tid) {
    const uint32_t high = static_cast<uint32_t>(tid >> 32);
    return high == 0x00040000u || high == 0x00040001u || high == 0x00040002u;
}

// svcWaitSynchronization(process, 0): 0 = signaled (the process is TERMINATED); the kernel's timeout 0x09401BFE is a POSITIVE result (R_SUCCEEDED
// would take it for success): not terminated yet. Anything else: unknown. 1, 0, -1.
inline int WaitState(uint32_t result) {
    return result == 0 ? 1 : (result == 0x09401BFEu ? 0 : -1);
}

// svcGetHandleInfo(handle, 1) answers the object's reference count less the temporary reference it took itself; the handle we hold is one more:
// the references held by others.
inline long long OthersFromHandleInfo(long long v) {
    return v - 1;
}

// The next address of a walk of the address space after the run [base, base + size): false when the walk is over (an empty run, a wrap, or the end
// of the user space at 0x40000000).
inline bool NextAddress(uint32_t base, uint32_t size, uint32_t* next) {
    if (size == 0) {
        return false;
    }
    const uint32_t n = base + size;
    if (n <= base || n >= 0x40000000u) {
        return false;
    }
    *next = n;
    return true;
}

struct Proc {
    uint32_t pid = 0;
    uint64_t tid = 0;
    char name[9] = {};
};

// The processes that can be a closed game still in memory: games (IsGameTitle), not 3DS Link itself.
inline std::vector<Proc> Candidates(const std::vector<Proc>& all, uint32_t self_pid) {
    std::vector<Proc> out;
    for (const Proc& p : all) {
        if (p.pid != self_pid && IsGameTitle(p.tid)) {
            out.push_back(p);
        }
    }
    return out;
}

// Is `pid` still in a process list?
inline bool Listed(const std::vector<uint32_t>& pids, uint32_t pid) {
    for (uint32_t p : pids) {
        if (p == pid) {
            return true;
        }
    }
    return false;
}

// One run of the closed game's address space worth writing down: shared (6) or locked (11) memory (a block it shared with a service, or one a
// service still maps), or the socket block's address whatever it is.
constexpr uint32_t kStateShared = 6;
constexpr uint32_t kStateLocked = 11;
constexpr uint32_t kSocBlock = 0x0dfe0000u; // (the plugin's 128 KB socket block: soc.cpp of the plugin, "top of the heap area")
inline bool WorthWriting(uint32_t base, uint32_t size, uint32_t state) {
    return state == kStateShared || state == kStateLocked || (base <= kSocBlock && kSocBlock - base < size);
}

// The lines (no date: the caller puts the stamp in front).
inline std::string ClassesLine(const int64_t (&cur)[kClasses], const int64_t (&lim)[kClasses]) {
    std::string s = "classes of the APPLICATION limit (3DS Link's own included):";
    char buf[64];
    for (int i = 0; i < kClasses; ++i) {
        const int64_t c = i == 0 ? cur[i] / 1024 : cur[i];
        const int64_t l = i == 0 ? lim[i] / 1024 : lim[i];
        std::snprintf(buf, sizeof(buf), "%s %s %lld/%lld", i == 0 ? "" : ",", ClassName(i), static_cast<long long>(c), static_cast<long long>(l));
        s += buf;
    }
    return s;
}

inline std::string ProcLine(const Proc& p, int signaled, int threads, long long refs_others) {
    char buf[280];
    std::snprintf(buf, sizeof(buf), "closed game still in memory? pid %lu '%s' title %016llx: %s, %d thread(s) left, %lld reference(s) besides ours (Rosalina's own one counts until its exit task)",
                  static_cast<unsigned long>(p.pid), p.name, static_cast<unsigned long long>(p.tid),
                  signaled == 1 ? "terminated (handle signaled)" : (signaled == 0 ? "not terminated yet (handle not signaled)" : "state unknown"), threads,
                  refs_others);
    return buf;
}

inline std::string RunLine(uint32_t pid, uint32_t base, uint32_t size, uint32_t state, uint32_t perm) {
    char buf[224];
    std::snprintf(buf, sizeof(buf), "  pid %lu memory 0x%08lx + 0x%lx: state %lu%s perm 0x%lx%s", static_cast<unsigned long>(pid),
                  static_cast<unsigned long>(base), static_cast<unsigned long>(size), static_cast<unsigned long>(state),
                  state == kStateLocked ? " (locked: a memory block's source, a stack or TLS)" : (state == kStateShared ? " (shared)" : ""),
                  static_cast<unsigned long>(perm), (base <= kSocBlock && kSocBlock - base < size) ? " <- the plugin's socket block" : "");
    return buf;
}

inline std::string GoneLine(uint32_t pid, uint64_t seconds) {
    char buf[120];
    std::snprintf(buf, sizeof(buf), "closed game pid %lu left memory %llu s after this start", static_cast<unsigned long>(pid),
                  static_cast<unsigned long long>(seconds));
    return buf;
}

inline std::string StillLine(uint32_t pid, uint64_t seconds) {
    char buf[120];
    std::snprintf(buf, sizeof(buf), "closed game pid %lu still in memory %llu s after this start", static_cast<unsigned long>(pid),
                  static_cast<unsigned long long>(seconds));
    return buf;
}

// When the "still there" line is written again while the app runs (seconds after the start): 60 s, 5 min, 15 min, then every 30 min.
inline bool StillDue(uint64_t last_written_s, uint64_t now_s) {
    static const uint64_t kMarks[] = {60, 300, 900};
    for (uint64_t m : kMarks) {
        if (last_written_s < m && now_s >= m) {
            return true;
        }
    }
    return now_s >= 1800 && now_s / 1800 > last_written_s / 1800;
}

} // namespace zombie_plan
