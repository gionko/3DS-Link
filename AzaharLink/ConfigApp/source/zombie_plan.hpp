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

#include "azl/title_rules.hpp"

namespace zombie_plan {

// The kernel's resource classes, in the order of the APPLICATION limit's counters (ResourceLimitType 1..8 of libctru).
constexpr int kClasses = 8;
inline const char* ClassName(int i) {
    static const char* const kNames[kClasses] = {"commit KiB", "thread", "event", "mutex", "semaphore", "timer", "shmem", "arbiter"};
    return i >= 0 && i < kClasses ? kNames[i] : "?";
}

// A title id of a game (what a closed game left behind can only be one): 0004000x, with x = 0 (application), 1 (a Download Play child: Kirby's,
// 3DS Link Delivery's) or 2 (demo). (DPAPP-2h-sec) And the Download Play guest 3DS Link prepares, whose process is 00040010 in the band of the
// prepared children (low word 0x0F800000..0x0FFFFFFF, azl::title_rules::PreparedSlotOfProcess): the one closed game the build 2h can leave in
// memory - never another 00040010 (the system's applications, its own Download Play).
inline bool IsGameTitle(uint64_t tid) {
    const uint32_t high = static_cast<uint32_t>(tid >> 32);
    return high == 0x00040000u || high == 0x00040001u || high == 0x00040002u || azl::title_rules::PreparedSlotOfProcess(tid) != 0;
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

// (DPAPP-2j-rev) The processes of the list that could not be opened (`left_out`) are all gone from the list read again (`pids`): they left between
// the list and the open, and the list is whole; one still listed that could not be opened is not known (the list is not read whole).
inline bool NoneListed(const std::vector<uint32_t>& pids, const std::vector<uint32_t>& left_out) {
    for (uint32_t p : left_out) {
        if (Listed(pids, p)) {
            return false;
        }
    }
    return true;
}

// (DPAPP-2j-rev) Whether the list is worth reading again for 500 ms after pm was asked: only when the references besides ours fell while our handle
// was still open (pm closes its own before it answers, manager.c:86-87). With no fall no reference of pm's was there: nothing can leave when ours
// go, and the screen's thread does not wait for 500 ms at every start (a reference that is not pm's). A count not read: the wait, as before.
inline bool WaitAfterUnregister(long long before_others, long long after_others) {
    return before_others < 0 || after_others < 0 || after_others < before_others;
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

// ---- (DPAPP-2j, R-A) the closed game pm keeps of a Download Play guest in the form M3c1, freed ------------------------------------------------
// Why it stays (Luma3DS_reference/sysmodules/pm/source): pm registers a process under its ACI's id (launch.c:57, process->titleId = the exheader's
// aci.local_caps.title_id - the key 00040000|low of the guest) and keeps the handle of an application that asked to be told of its end
// (process_monitor.c:93-113, PROCESSFLAG_NOTIFY_TERMINATION) until NS calls UnregisterProcess with ITS id (manager.c:71-92) - the id it launched,
// AM's 00040001|low: no node of pm has it, the handle is never closed, the closed game's memory stays until the console restarts (prova-2g:
// '3STARS' terminated, "1 reference besides ours"). 3DS Link makes the one call NS's would have made: pm:app command 12, UnregisterProcess(the key)
// (pmapp.c:108-112; it finds the FIRST node whose id, without its low byte, is the key's - process_data.c:32-43 - closes pm's handle, deletes the
// node, and answers 0 whatever it found).
// When it is safe (manager.c:71-92 never looks at the state: on a LIVE process pm would forget the running application - another one could start
// next to it - and never clean it up at its end, NS would wait for ever): only when every process whose id is the key's without the low byte is
// TERMINATED (svcWaitSynchronization(h, 0) == 0 exactly: the kernel's timeout 0x09401BFE is a positive result) with no thread left (GetProcessInfo
// type 7), none of them 3DS Link; only a key of the band the app registered (a record of ours), never 3DS Link's (0x0F83D1xx is IN the band); never on
// an emulator (pm is HLE there and keeps nothing); and only while 3DS Link is the running application (pm refuses any other application while one
// runs, launch.c:299 and :355: nothing with the key can start between the look and the call, and pm's cleanupProcess of the closed game already ran -
// it is what let 3DS Link start).
struct ProcFact {
    uint32_t pid = 0;
    uint64_t tid = 0;  ///< svcGetProcessInfo 0x10001 (Luma: the code set's id, the ACI's - the one pm registered)
    int signaled = -1; ///< WaitState: 1 terminated, 0 not yet, -1 not known
    int threads = -1;  ///< svcGetProcessInfo 7; -1 not known
};
enum class FreeVerdict : uint8_t {
    Free,     ///< every process of the key is terminated with no thread: `*calls` UnregisterProcess, one per node
    Nothing,  ///< no process of the key: nothing to free
    NotOurs,  ///< the key is not a registered key of the band, or is 3DS Link's: never touched
    Running,  ///< a process of the key is not terminated or has a thread: never touched (pm would forget a live application)
    Unsure,   ///< a process of the key whose state or threads did not read: never touched
    Self,     ///< 3DS Link's own process has the key: never touched
    Emulator, ///< pm of an emulator is HLE: never touched
};
constexpr int kMaxFreeCalls = 8;
/// pm's own comparison (process_data.c:37): the whole id but its low byte.
inline bool SameKeyAsPm(uint64_t tid, uint64_t key) {
    return (tid & ~0xFFull) == (key & ~0xFFull);
}
/// A key 3DS Link may ever ask pm to forget: 00040000 of the band, never 3DS Link's own (with or without its low byte).
inline bool FreeableKey(uint64_t key) {
    return azl::title_rules::IsPreparedId(key) && (key & ~0xFFull) != (azl::title_rules::kAppTid & ~0xFFull);
}
inline FreeVerdict Judge(const std::vector<ProcFact>& all, uint32_t self_pid, uint64_t key, bool registered, bool on_emulator, int* calls) {
    *calls = 0;
    if (on_emulator) {
        return FreeVerdict::Emulator;
    }
    if (!registered || !FreeableKey(key)) {
        return FreeVerdict::NotOurs;
    }
    int found = 0;
    for (const ProcFact& p : all) {
        if (!SameKeyAsPm(p.tid, key)) {
            continue;
        }
        if (p.pid == self_pid) {
            return FreeVerdict::Self;
        }
        if (p.signaled == 0 || p.threads > 0) {
            return FreeVerdict::Running;
        }
        if (p.signaled != 1 || p.threads != 0) {
            return FreeVerdict::Unsure;
        }
        ++found;
    }
    if (found == 0) {
        return FreeVerdict::Nothing;
    }
    *calls = found < kMaxFreeCalls ? found : kMaxFreeCalls;
    return FreeVerdict::Free;
}
inline const char* VerdictWord(FreeVerdict v) {
    switch (v) {
    case FreeVerdict::Free: return "terminated, no thread left";
    case FreeVerdict::Nothing: return "none";
    case FreeVerdict::NotOurs: return "not ours";
    case FreeVerdict::Running: return "running";
    case FreeVerdict::Unsure: return "state unknown";
    case FreeVerdict::Self: return "3DS Link";
    case FreeVerdict::Emulator: return "emulator";
    }
    return "?";
}
/// The command of pm:app that unregisters a process (pmapp.c:108-112): its header, two words of the id after it.
constexpr uint32_t kPmAppUnregisterHeader = 0x000C0080u; // IPC_MakeHeader(12, 2, 0)
static_assert(kPmAppUnregisterHeader == ((12u << 16) | (2u << 6) | 0u), "pm:app UnregisterProcess, command 12, two normal words");

inline std::string FreeLookLine(uint64_t key, FreeVerdict v, int processes, uint32_t ms) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "free: key %016llx: %d process(es) of the key, %s, %lu ms", static_cast<unsigned long long>(key), processes,
                  VerdictWord(v), static_cast<unsigned long>(ms));
    return buf;
}
inline std::string FreeCallLine(uint32_t pid, const char* name, uint64_t key, uint64_t am, uint32_t rc, uint32_t answer, uint32_t ms) {
    char buf[260];
    std::snprintf(buf, sizeof(buf), "free: pid %lu '%s' key %016llx (AM %016llx): terminated, 0 threads -> pm:app UnregisterProcess rc=0x%08lx answer=0x%08lx (%lu ms)",
                  static_cast<unsigned long>(pid), name, static_cast<unsigned long long>(key), static_cast<unsigned long long>(am),
                  static_cast<unsigned long>(rc), static_cast<unsigned long>(answer), static_cast<unsigned long>(ms));
    return buf;
}
inline std::string FreeAfterLine(uint32_t pid, bool gone, uint32_t ms, long long refs_others) {
    char buf[220];
    if (gone) {
        std::snprintf(buf, sizeof(buf), "free: pid %lu left memory after %lu ms", static_cast<unsigned long>(pid), static_cast<unsigned long>(ms));
    } else {
        std::snprintf(buf, sizeof(buf), "free: pid %lu still in memory after %lu ms, %lld reference(s) besides ours: not pm's (Rosalina or the kernel): restart the console",
                      static_cast<unsigned long>(pid), static_cast<unsigned long>(ms), refs_others);
    }
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
