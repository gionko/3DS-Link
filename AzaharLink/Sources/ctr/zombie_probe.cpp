// The console reads of ctr/zombie_probe.hpp: everything that needs Luma3DS's custom svcs lives here, so plugin.cpp,
// the chat and the mirror call one place and Tests/policy_check.sh can scope it. Nothing here runs unless the capture
// is on (g_debug_level>0), and nothing that needs a Luma svc unless Luma3DS is present (BackdoorOk(); ForcedResLine
// uses official svcs only); nothing changes the game or the system - only reads and log lines. The pure logic (mask
// classification, memory-map walk and merge, IPC decode, handle classes, owned objects, the startup decision) is in the
// header, tested on a PC. No newlib formatter: one Log() format per line (the keep-alive thread has 4 KB of stack).
#include "ctr/zombie_probe.hpp"

#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cstring>

#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "csvc.h"

// The mirror's counters (Sources/ctr/input_mirror.cpp): how many times the keyboard's hold of the game's threads was
// taken and let go, to catch an imbalance that would leave a thread held (0x20) at the exit. Read-only here.
extern "C" volatile u32 azl_mirror_freeze_count;
extern "C" volatile u32 azl_mirror_unfreeze_count;

namespace azl {
namespace zombie {

namespace {

// The resource-limit classes the res line walks, the same order and names as plugin.cpp's LogMemory.
const struct {
    ResourceLimitType type;
    const char* name;
} kLimits[] = {
    {RESLIMIT_COMMIT, "commit KB"},    {RESLIMIT_THREAD, "thread"},
    {RESLIMIT_EVENT, "event"},         {RESLIMIT_MUTEX, "mutex"},
    {RESLIMIT_SEMAPHORE, "semaphore"}, {RESLIMIT_TIMER, "timer"},
    {RESLIMIT_SHAREDMEMORY, "shmem"},  {RESLIMIT_ADDRESSARBITER, "arbiter"},
};
constexpr int kLimitCount = sizeof(kLimits) / sizeof(kLimits[0]);

// GET_ALL_HANDLES writes one word per handle in use (the class tokens counted are in the header: kHandleTokens). A
// fixed buffer, not on the caller's stack (the keep-alive thread's is 4 KB): one call at a time, at an event. A game's
// table has a few hundred slots at most; GameHandleCounts checks the count in use against it (with kHandleBufSpare for
// handles opened between the check and the call) before the kernel writes, since the kernel does not bound the copy.
constexpr u32 kHandleBufWords = 0x400;
constexpr u32 kHandleBufSpare = 0x80;
u32 g_handle_buf[kHandleBufWords];

constexpr int kMapLineCap = 128; // (the memory-map walk's lines at most)

int g_backdoor = -1; ///< -1 not looked at, 0 no, 1 yes

} // namespace

// >>> game-thread code (Tests/policy_check.sh): BackdoorOk is reachable from a hook game-thread entry (NoteIpcHandleXfer),
// so it keeps the printf family, Log() and the heap out - no Log() here (the callgraph check proves the whole game-thread
// path is formatter-free). The same test as the mirror's Supported() (input_mirror.cpp), cached once.
bool BackdoorOk() {
    if (g_backdoor < 0) {
        if (CTRPluginFramework::System::IsCitra()) {
            g_backdoor = 0; // an emulator: the Luma svc below is never issued there
        } else {
            // A check of a known mapped page: 0 where the svc is not answered.
            g_backdoor = svcConvertVAToPA(reinterpret_cast<const void*>(0x1FF80000), false) != 0 ? 1 : 0;
        }
    }
    return g_backdoor == 1;
}
// <<< game-thread code

namespace {

// The APPLICATION resource limit's current and maximum values per class (kLimits' order), -1 where the svc gave none.
// Official svcs only (an emulator answers them too).
void ReadLimits(s64* cur, s64* max) {
    ResourceLimitType names[kLimitCount];
    for (int i = 0; i < kLimitCount; ++i) {
        names[i] = kLimits[i].type;
        cur[i] = -1;
        if (max) {
            max[i] = -1;
        }
    }
    Handle limit = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
        svcGetResourceLimitCurrentValues(cur, limit, names, kLimitCount);
        if (max) {
            svcGetResourceLimitLimitValues(max, limit, names, kLimitCount);
        }
        svcCloseHandle(limit);
    }
}

} // namespace

void ForcedResLine(const char* when) {
    if (g_debug_level <= 0) {
        return; // (official svcs only: no backdoor gate, the line is a reference on Azahar too)
    }
    s64 cur[kLimitCount];
    s64 max[kLimitCount];
    ReadLimits(cur, max);
    const auto v = [&](const s64* a, int i) { return static_cast<long>(i == 0 ? a[i] / 1024 : a[i]); };
    static_assert(kLimitCount == 8, "the res line's format names the eight classes of kLimits");
    // One Log() format (printf_lite, a small frame): no newlib formatter on the keep-alive thread's 4 KB stack.
    Log("res %s: commit KB %ld/%ld thread %ld/%ld event %ld/%ld mutex %ld/%ld semaphore %ld/%ld timer %ld/%ld "
        "shmem %ld/%ld arbiter %ld/%ld | OSD notifies: chat %lu, plugin %lu",
        when, v(cur, 0), v(max, 0), v(cur, 1), v(max, 1), v(cur, 2), v(max, 2), v(cur, 3), v(max, 3), v(cur, 4),
        v(max, 4), v(cur, 5), v(max, 5), v(cur, 6), v(max, 6), v(cur, 7), v(max, 7), StatsChatOsdNotifies(),
        StatsPluginOsdNotifies());
}

void GameRefcount(const char* when) {
    if (g_debug_level <= 0 || !BackdoorOk()) {
        return;
    }
    Handle h = 0;
    if (R_FAILED(svcDuplicateHandle(&h, CUR_PROCESS_HANDLE))) {
        Log("ref %s: could not duplicate the process handle", when);
        return;
    }
    s64 v = 0;
    const Result rc = svcGetHandleInfo(&v, h, 1); // case 1: obj->refcount - 1 (the temp ref is cancelled inside the svc)
    svcCloseHandle(h);                            // (our one extra reference goes: every path closes it)
    // The objects the game owns at the same instant (each holds a reference to its owner): the limit's thread..arbiter.
    s64 cur[kLimitCount];
    ReadLimits(cur, nullptr);
    if (R_FAILED(rc)) {
        Log("ref %s: svcGetHandleInfo failed 0x%08lx", when, static_cast<unsigned long>(rc));
        return;
    }
    // v counts our dup handle; subtract it for the reference count without this probe.
    const s64 refs = v - 1;
    const s64 owned = OwnedObjects(cur, 1, kLimitCount);
    if (owned < 0) {
        Log("ref %s: game KProcess refcount %ld (owned objects not read: no residual)", when, static_cast<long>(refs));
        return;
    }
    Log("ref %s: game KProcess refcount %ld = owned objects %ld (thread %ld event %ld mutex %ld semaphore %ld timer %ld "
        "shmem %ld arbiter %ld) + residual %ld",
        when, static_cast<long>(refs), static_cast<long>(owned), static_cast<long>(cur[1]), static_cast<long>(cur[2]),
        static_cast<long>(cur[3]), static_cast<long>(cur[4]), static_cast<long>(cur[5]), static_cast<long>(cur[6]),
        static_cast<long>(cur[7]), static_cast<long>(refs - owned));
}

void ThreadSchedDump(const char* when) {
    if (g_debug_level <= 0 || !BackdoorOk()) {
        return;
    }
    // The count from GetProcessInfo type 7, which gives its process reference back. There is no list of thread ids any
    // more: the kernel's svcGetThreadList leaks one reference on the process at every call (the very bug of phase K), so
    // the scheduling masks of the threads are not read.
    s64 count = -1;
    if (svcGetProcessInfo(&count, CUR_PROCESS_HANDLE, 7) != 0) {
        count = -1;
    }
    // A hold is two Freeze passes (hidmirror::Switcher::Hold) and one let-go: balanced when passes == 2 x let-gos.
    const unsigned long passes = azl_mirror_freeze_count;
    const unsigned long letgos = azl_mirror_unfreeze_count;
    Log("threads %s: %ld thread(s) | mirror hold passes %lu, let-gos %lu (balanced when passes = 2 x let-gos: %s)", when,
        static_cast<long>(count), passes, letgos, passes == 2 * letgos ? "yes" : "NO");
}

void GameHandleCounts() {
    if (g_debug_level <= 0 || !BackdoorOk()) {
        return;
    }
    // GET_ALL_HANDLES writes one word per handle in use, with no bound on the buffer: the table's count in use first
    // (official GetProcessInfo type 4), with room to spare for handles opened between the two calls.
    s64 in_use = 0;
    if (R_FAILED(svcGetProcessInfo(&in_use, CUR_PROCESS_HANDLE, 4)) || in_use < 0 ||
        in_use > static_cast<s64>(kHandleBufWords - kHandleBufSpare)) {
        Log("handles at EXIT: not counted (%ld handle(s) in use, room for %lu)", static_cast<long>(in_use),
            static_cast<unsigned long>(kHandleBufWords - kHandleBufSpare));
        return;
    }
    // Token 0: no class filter, so the kernel only copies descriptors and never calls into an object of the table
    // (with a token it calls GetClassToken on every entry without the table's lock or a reference: a handle closed on
    // another core at that moment would be a freed object). Each handle is then classified by svcGetHandleInfo 0x10001,
    // which takes the lock and a reference, and fails cleanly on a handle closed meanwhile ("gone").
    const s32 got = svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_GET_ALL_HANDLES, reinterpret_cast<u32>(g_handle_buf), 0);
    if (got < 0) {
        Log("handles at EXIT: GET_ALL_HANDLES failed 0x%08lx", static_cast<unsigned long>(got));
        return;
    }
    const s32 total = got > static_cast<s32>(kHandleBufWords) ? static_cast<s32>(kHandleBufWords) : got;
    u32 counts[kHandleClasses + 1] = {};
    u32 gone = 0;
    for (s32 i = 0; i < total; ++i) {
        s64 token = 0;
        if (R_FAILED(svcGetHandleInfo(&token, static_cast<Handle>(g_handle_buf[i]), 0x10001))) {
            ++gone;
            continue;
        }
        ++counts[HandleClassIndex(static_cast<u32>(token))];
    }
    static_assert(kHandleClasses == 9, "the handles line's format names the nine classes of kHandleTokens");
    const auto c = [&](int i) { return static_cast<unsigned long>(counts[i]); };
    Log("handles at EXIT: %ld in all: %s %lu %s %lu %s %lu %s %lu %s %lu %s %lu %s %lu %s %lu %s %lu other %lu gone %lu",
        static_cast<long>(got), kHandleTokenNames[0], c(0), kHandleTokenNames[1], c(1), kHandleTokenNames[2], c(2),
        kHandleTokenNames[3], c(3), kHandleTokenNames[4], c(4), kHandleTokenNames[5], c(5), kHandleTokenNames[6], c(6),
        kHandleTokenNames[7], c(7), kHandleTokenNames[8], c(8), c(kHandleClasses), static_cast<unsigned long>(gone));
}

void MemMapWalk() {
    if (g_debug_level <= 0 || !BackdoorOk()) {
        return;
    }
    const auto query = [](u32 addr, MapBlock* b) {
        MemInfo info{};
        PageInfo page;
        if (R_FAILED(svcQueryMemory(&info, &page, addr))) {
            return false;
        }
        b->base = info.base_addr;
        b->size = info.size;
        b->state = info.state;
        b->perm = info.perm;
        return true;
    };
    const auto emit = [](const MergedRun& run) {
        Log("vm 0x%08lx + %lu KB state %lu perm %lu", static_cast<unsigned long>(run.base),
            static_cast<unsigned long>(run.size / 1024), static_cast<unsigned long>(run.state),
            static_cast<unsigned long>(run.perm));
    };
    const WalkEnd end = WalkMap(0x00100000, 0x14000000, kMapLineCap, query, emit);
    if (end.truncated) {
        Log("vm: walk truncated at 0x%08lx (%d runs logged, the cap)", static_cast<unsigned long>(end.at), end.runs);
    }
}

} // namespace zombie
} // namespace azl
