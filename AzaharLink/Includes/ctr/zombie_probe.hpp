// Debug-only probes for the "zombie" that a Monster Hunter XX chat session leaves behind: after HOME->Close the old
// process stays TERMINATED but never finalized, its pages still in the APPLICATION region until a reboot. Nothing here
// changes the game or the system; every entry point only reads kernel state and writes a log line, and only when the
// capture is on (g_debug_level>0) and Luma3DS's custom svcs are available (not an emulator). The reads tell WHICH class
// of object is still alive in the zombie and WHO holds a reference to it; the fix comes later, from what they show.
//
// The pure logic that a PC can test (the scheduling-mask classification, the memory-map merge, the IPC handle-transfer
// decode) is in the azl::zombie namespace below, header only (Tests/zombie_probe_test.cpp). The reads that need the
// console - svcGetResourceLimit, svcDuplicateHandle, svcGetHandleInfo, svcGetThreadList, svcOpenThread,
// svcTranslateHandle (0xB2), svcCustomBackdoor (0x80), svcControlProcess GET_ALL_HANDLES (0xB3), svcQueryMemory - are in
// Sources/ctr/zombie_probe.cpp.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace zombie {

// ---- pure logic (PC-testable) ----------------------------------------------------------------

// A thread's scheduling mask is worth a line when it carries a hold or an out-of-the-ordinary run state:
//   0x20  Luma3DS/ControlProcess hold (plgloader.c, ExitProcess.c clears it raw on exit);
//   0x40  a second scheduler bit;
//   low nibble not 0 (runnable), 1 (running) or 2 (terminating): a thread stuck in some other state.
// A thread in state 2 (terminating) is NOT reported as a hold: that is the normal state of the dying process's threads.
inline bool SchedMaskNotable(u8 mask) {
    const u8 low = static_cast<u8>(mask & 0xF);
    return (mask & 0x20) != 0 || (mask & 0x40) != 0 || (low != 0 && low != 1 && low != 2);
}

// A compact walk of the address space merges adjacent runs with the same state and permission (svcQueryMemory already
// returns the maximal contiguous run of identical attributes, but a caller that skips FREE gaps can still hand two
// non-adjacent runs of equal attributes, which must NOT merge). The caller feeds non-FREE blocks in rising address
// order; Add() returns a finished run to log when the new block does not extend the current one, and Flush() gives the
// last. The never-loop guard belongs to the caller (next<=addr -> stop), as in LogAddressMap.
struct MergedRun {
    u32 base = 0;
    u32 size = 0;
    u32 state = 0;
    u32 perm = 0;
};

class MapMerger {
public:
    /// Feeds one block. Returns true and fills `out` with a finished run when the block starts a new run (the finished
    /// run is the one before it); false when the block extended the current run (nothing to log yet).
    bool Add(u32 base, u32 size, u32 state, u32 perm, MergedRun* out) {
        if (have_ && cur_.base + cur_.size == base && cur_.state == state && cur_.perm == perm) {
            cur_.size += size; // adjacent and identical: extend
            return false;
        }
        const bool emit = have_;
        if (emit) {
            *out = cur_;
        }
        cur_ = MergedRun{base, size, state, perm};
        have_ = true;
        return emit;
    }
    /// The last run, if any (call once at the end).
    bool Flush(MergedRun* out) {
        if (!have_) {
            return false;
        }
        *out = cur_;
        have_ = false;
        return true;
    }

private:
    bool have_ = false;
    MergedRun cur_;
};

// The walk itself (MemMapWalk), over a query callback so a PC can drive it. `query(addr, &block)` answers the block that
// holds `addr` (false: stop); blocks of state 0 (FREE) are skipped, the rest go through a MapMerger and every finished
// run to `emit(run)`, at most `cap` of them. The walk stops at `end`, at a block of size 0, and when the next address
// does not go up (a block that ends at or below where it began, or one that wraps past 0xFFFFFFFF): it never loops.
// When the cap cuts it, the result says so and where (the run that did not fit), so the log can mark the cut instead of
// leaving a list whose tail looks like "nothing there".
struct MapBlock {
    u32 base = 0;
    u32 size = 0;
    u32 state = 0;
    u32 perm = 0;
};
constexpr u32 kMemStateFree = 0; // (libctru's MEMSTATE_FREE)

struct WalkEnd {
    bool truncated = false;
    u32 at = 0;    ///< when truncated: the base of the first run that was not emitted
    int runs = 0;  ///< the runs emitted
};

template <class Query, class Emit>
WalkEnd WalkMap(u32 begin, u32 end, int cap, Query&& query, Emit&& emit) {
    WalkEnd w;
    MapMerger merger;
    MergedRun run;
    u32 addr = begin;
    while (addr < end) {
        MapBlock b;
        if (!query(addr, &b) || b.size == 0) {
            break;
        }
        if (b.state != kMemStateFree && merger.Add(b.base, b.size, b.state, b.perm, &run)) {
            if (w.runs >= cap) {
                w.truncated = true;
                w.at = run.base;
                return w;
            }
            emit(run);
            ++w.runs;
        }
        const u32 next = b.base + b.size;
        if (next <= addr) {
            break; // (the same never-loop guard as LogAddressMap; a wrap past the top lands here too)
        }
        addr = next;
    }
    if (merger.Flush(&run)) {
        if (w.runs >= cap) {
            w.truncated = true;
            w.at = run.base;
        } else {
            emit(run);
            ++w.runs;
        }
    }
    return w;
}

// ---- the startup-occupancy decision (Plugin::CheckStartupOccupancy, the one probe at debug=0) ----------------------
// At the plugin's start, before the game's crt0, a fresh launch has:
//   books  = the APPLICATION resource limit's current COMMIT: this game's own commit (the limit carries one application);
//   own    = svcGetProcessInfo(CUR_PROCESS_HANDLE, 2): the game's PRIVATE blocks + its handle table + kernel allocations,
//            never less than its commit (the same figure as heap_plan's Facts.own, ConfigApp/source/heap_plan.hpp);
//   used   = the APPLICATION region's used pages: own + the plugin's block (shared: not in own) + the system's bookkeeping.
// A closed game that was never finalized (Monster Hunter XX after a chat session) keeps its commit in the books AND its
// pages in the region until KProcess::Finalize. So it shows up in BOTH, and only `own` (read from this process, not from
// the shared limit) is free of it: comparing `used` against the limit's current would cancel the zombie on both sides.
// Foreign in the books = books > own + margin (heap_plan's ForeignInBooks, which gave "foreign 1" in 3DSLink-heap.log);
// foreign in the region = used > own + plugin block + margin. `own` unknown (0, or the svc failed) = no decision.
constexpr s64 kStartupMargin = 4ll << 20; // 4 MB for the system's bookkeeping
constexpr u32 kStartupForeignBooks = 1;
constexpr u32 kStartupForeignRegion = 2;

inline u32 StartupForeign(s64 books, s64 used, s64 own, s64 plugin_block) {
    if (own <= 0) {
        return 0;
    }
    u32 f = 0;
    if (books > own + kStartupMargin) {
        f |= kStartupForeignBooks;
    }
    if (used > own + plugin_block + kStartupMargin) {
        f |= kStartupForeignRegion;
    }
    return f;
}

// ---- the game's handles by class (GameHandleCounts) -----------------------------------------------------------------
// The kernel's class tokens (Luma3DS k11 kernel.h) of the classes counted, in the order of the log line. A handle table
// never holds a KSession itself (0x70): a session is held through its KClientSession (0xA5) or KServerSession (0x95),
// and GetClassToken answers the most derived class. Anything else (KProcess, ports, resource limit, ...) is "other".
constexpr u32 kHandleTokens[] = {0x1F, 0xB0, 0x8D, 0x39, 0x2F, 0x35, 0x98, 0x95, 0xA5};
constexpr const char* kHandleTokenNames[] = {"event", "shmem", "thread", "mutex", "semaphore",
                                             "timer", "arbiter", "server-session", "client-session"};
constexpr int kHandleClasses = sizeof(kHandleTokens) / sizeof(kHandleTokens[0]);
static_assert(sizeof(kHandleTokenNames) / sizeof(kHandleTokenNames[0]) == kHandleClasses, "one name per token");

/// The column of a class token, or kHandleClasses for "other".
inline int HandleClassIndex(u32 token) {
    for (int i = 0; i < kHandleClasses; ++i) {
        if (kHandleTokens[i] == token) {
            return i;
        }
    }
    return kHandleClasses;
}

// ---- the game KProcess's references (GameRefcount) --------------------------------------------------------------
// Every object a process owns holds a reference to its owner: KThread, KEvent, KMutex, KSemaphore, KTimer,
// KAddressArbiter and KSharedMemory each IncrementRefcount(owner) in their Initialize (k11 11.14: KThread::Initialize,
// KEvent::Initialize @20329, KMutex::Initialize, KSemaphore::Initialize, KTimer::Initialize, KAddressArbiter::Initialize,
// KSharedMemory::Initialize). So the raw refcount moves with every one of them the game, CTRPF or the plugin creates or
// frees, and says nothing by itself. The resource limit counts the same objects (its current value per class; the
// APPLICATION limit carries only this game while it runs), so refcount - owned = the references held from OUTSIDE the
// objects the game owns (handles other processes keep, the kernel's own): that residual is the number to compare across
// events. `owned` = the sum of the limit's current thread..arbiter values (a value the svc did not give, < 0, is left out
// and the residual is then not trusted: OwnedObjects answers -1).
inline s64 OwnedObjects(const s64* current, int first, int count) {
    s64 sum = 0;
    for (int i = first; i < count; ++i) {
        if (current[i] < 0) {
            return -1;
        }
        sum += current[i];
    }
    return sum;
}

// An IPC reply's translate section may hand the caller a handle (the service the game just opened, an event a command
// returned). The header word counts the normal params in bits 6.. and the translate words in bits 0..5; the translate
// section begins right after the normal params. A handle descriptor has its low nibble 0 (0x00 copy, 0x10 move; bit 5
// is "replace by caller pid", still a handle descriptor); the handles follow it, (desc>>26)+1 of them. Other
// descriptors - static buffers (low bits 0b10) and map buffers (bit 3 set) - are each followed by one pointer word and
// are skipped. Decode returns the FIRST handle a descriptor carries.
struct HandleXfer {
    bool found = false;
    bool move = false; ///< move (closed in the sender) rather than copy
    u32 handle = 0;    ///< the first handle of the first handle descriptor
};

inline u32 IpcNormalCount(u32 header) {
    return (header >> 6) & 0xFF;
}
inline u32 IpcTranslateWords(u32 header) {
    return header & 0x3F;
}

/// `cmd` points at the reply (cmd[0] is the header); `words` is how many words are safe to read (the command buffer is
/// 64 words). Walks the translate section and fills `out` with the first handle descriptor found.
inline HandleXfer DecodeReplyHandleXfer(const u32* cmd, u32 words) {
    HandleXfer out;
    if (cmd == nullptr || words == 0) {
        return out;
    }
    const u32 header = cmd[0];
    const u32 normal = IpcNormalCount(header);
    const u32 translate = IpcTranslateWords(header);
    const u32 begin = 1 + normal;             // first translate word
    const u32 end = begin + translate;         // one past the last
    if (translate == 0 || end > words) {
        return out; // no translate section, or the header claims more than the buffer holds
    }
    u32 i = begin;
    while (i < end) {
        const u32 desc = cmd[i];
        if ((desc & 0xF) == 0x0) {             // handle descriptor (copy 0x00 / move 0x10)
            const u32 count = (desc >> 26) + 1;
            if (i + 1 + count > end || count == 0) {
                break; // malformed: the handles do not fit the section
            }
            out.found = true;
            out.move = (desc & 0x10) != 0;
            out.handle = cmd[i + 1];
            return out;
        }
        if ((desc & 0x3) == 0x2) {             // static buffer descriptor: one pointer word follows
            i += 2;
            continue;
        }
        if ((desc & 0x8) != 0) {               // map buffer descriptor: one pointer word follows
            i += 2;
            continue;
        }
        break; // an unknown descriptor: stop (do not guess a length)
    }
    return out;
}

// ---- the KThread layout this build reads (one layout for the Old and the New 3DS, Luma3DS k11 kernel.h) -------------
// The byte offsets inside Luma3DS's KThread. schedulingMask is the first member after the four base classes; coreId is
// 0x3C past it. Derived from the TLS words the mirror already pins and verifies (threadLocalStorage +0x94,
// threadLocalStorageKernel +0x98, Includes/azl/hid_mirror.hpp): threadLocalStorage sits 0x60 past schedulingMask, so
// schedulingMask is 0x94-0x60 = 0x34 and coreId is 0x34+0x3C = 0x70. The backdoor read VERIFIES coreId at run time
// against svcGetProcessorID() for the calling thread before any mask is trusted (ThreadSchedDump).
constexpr u32 kSchedMaskOff = 0x34;
constexpr u32 kCoreIdOff = 0x70;

// What the backdoor returns packed: the scheduling mask in the low byte, the coreId in the next byte.
inline u8 PackedMask(u32 packed) {
    return static_cast<u8>(packed & 0xFF);
}
inline u8 PackedCore(u32 packed) {
    return static_cast<u8>((packed >> 8) & 0xFF);
}

// ---- console reads (Sources/ctr/zombie_probe.cpp) --------------------------------------------
// All of these do nothing unless g_debug_level>0 AND the console has Luma3DS's custom svcs (BackdoorOk()), except
// ForcedResLine, which uses official svcs only and needs just the log on (BackdoorOk() itself never issues a Luma svc
// on an emulator: IsCitra() is tested first). None runs
// per frame: the caller invokes them only at the events named in the spec (keyboard open/close, mirror on/give-back,
// HOME_ENTER, HOME_EXIT, EXIT), never on the Old-3DS SWAP path.

/// True when the custom svcs may be used (not an emulator, svcConvertVAToPA answers). Computed once, then cached; on an
/// emulator the Luma svc is never issued (IsCitra() short-circuits it, as input_mirror's Supported()).
bool BackdoorOk();

/// "res <when>: ..." - the APPLICATION resource-limit current/maximum per class, the same shape as plugin.cpp's
/// LogMemory line, but forced AT the event (a clean close then has a reference to compare the stuck one against), with
/// the OSD-notify counts (the chat's own and the plugin's). Uses only official svcs (svcGetResourceLimit*), so it needs
/// no backdoor and runs on Azahar too (a reference for the per-class diff); still debug-gated.
void ForcedResLine(const char* when);

/// "ref <when>: game KProcess refcount N = owned objects O + residual R" - this process's KProcess reference count, read
/// without leaving a reference behind, next to the objects the game owns at the same instant (OwnedObjects: every one
/// of them holds a reference to its owner). Only a change of R between events says something outside grabbed (or let
/// go of) a reference; N alone moves with every thread, event or mutex created or freed.
void GameRefcount(const char* when);

/// "threads <when>: N thread(s) | mirror hold passes P, let-gos L (balanced ...)" - the process's thread count
/// (GetProcessInfo type 7: never svcGetThreadList, which leaks a reference on the process) and the mirror's hold passes
/// and let-gos (a hold is two passes: balanced when passes == 2 x let-gos). The per-thread scheduling masks are not read:
/// reading them needed the thread list.
void ThreadSchedDump(const char* when);

/// "handles at EXIT: event N shmem N ... other N" - how many objects of each class the game still holds a moment before
/// death. One GET_ALL_HANDLES with no class filter (only copies descriptors: no object of the table is touched), then
/// each handle classified by svcGetHandleInfo 0x10001, which takes the table's lock and a reference.
void GameHandleCounts();

/// "vm 0x.. + N KB state S perm P" - the merged non-free runs of 0x00100000..0x14000000, for the clean-vs-stuck diff;
/// "vm: walk truncated at 0x.." when the line cap cut it.
void MemMapWalk();

} // namespace zombie
} // namespace azl
