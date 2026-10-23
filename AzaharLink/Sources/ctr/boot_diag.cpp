// The diagnostic build's early record (Includes/ctr/boot_diag.hpp). Everything is under AZL_BOOT_DIAG: the delivered plugin has an empty file here.
#include "ctr/boot_diag.hpp"

#ifdef AZL_BOOT_DIAG

#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "azl/platform.hpp"

namespace azl {
namespace ctr {
namespace bootdiag {

namespace {

using CTRPluginFramework::File;
using CTRPluginFramework::ThreadEx;

constexpr const char* kPath = "/3ds/3DSLink/AzaharLink.boot.log";
constexpr const char* kSwitchDir = "/3ds/3DSLink/diag-no-";
constexpr int kWatchSteps = 200;       // 200 x 50 ms = ten seconds
constexpr s64 kWatchStepNs = 50 * 1000 * 1000;
constexpr int kMaxThreads = 48;

File g_file;
bool g_open = false;
LightLock g_lock;
u64 g_t0_ms = 0;

// One line, written and flushed at once (a game that dies leaves the card as it was at the last line).
void Out(const char* fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    const unsigned long ms = static_cast<unsigned long>(platform::NowMs() - g_t0_ms);
    char line[330];
    const int n = std::snprintf(line, sizeof(line), "[%4lu.%03lu] %s\n", ms / 1000, ms % 1000, text);
    if (n <= 0) {
        return;
    }
    LightLock_Lock(&g_lock);
    if (g_open) {
        g_file.Write(line, static_cast<u32>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1));
        g_file.Flush();
    }
    LightLock_Unlock(&g_lock);
}

void LumaFacts() {
    struct Fact {
        s32 param;
        const char* name;
    };
    static const Fact kFacts[] = {{0, "version (major<<24|minor<<16|build<<8)"}, {1, "commit"},          {3, "config bits (bit 3 = redirect app syscore threads to core 2)"},
                                  {0x180, "plugin loader flags"}, {0x181, "thread redirection disabled"}, {0x201, "is New 3DS"},
                                  {0x80, "application memory region bytes"}};
    for (const Fact& f : kFacts) {
        s64 value = -1;
        const Result rc = svcGetSystemInfo(&value, 0x10000, f.param);
        Out("luma: %s = 0x%llx (rc 0x%08lx)", f.name, static_cast<unsigned long long>(value), static_cast<unsigned long>(rc));
    }
    u32 kernel = osGetKernelVersion();
    u32 firm = osGetFirmVersion();
    Out("os: kernel 0x%08lx, firm 0x%08lx, process core now %d", static_cast<unsigned long>(kernel), static_cast<unsigned long>(firm),
        static_cast<int>(svcGetProcessorID()));
}

const ResourceLimitType kLimitTypes[10] = {RESLIMIT_PRIORITY, RESLIMIT_COMMIT,   RESLIMIT_THREAD,      RESLIMIT_EVENT,         RESLIMIT_MUTEX,
                                           RESLIMIT_SEMAPHORE, RESLIMIT_TIMER,   RESLIMIT_SHAREDMEMORY, RESLIMIT_ADDRESSARBITER, RESLIMIT_CPUTIME};
const char* const kLimitNames[10] = {"priority", "commit",     "thread", "event",   "mutex",
                                     "semaphore", "timer",     "shmem",  "arbiter", "cputime"};

void Limits() {
    Handle limit = 0;
    Result rc = svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc)) {
        Out("limits: svcGetResourceLimit failed 0x%08lx", static_cast<unsigned long>(rc));
        return;
    }
    s64 cur[10];
    s64 max[10];
    for (int i = 0; i < 10; ++i) {
        cur[i] = max[i] = -1;
    }
    ResourceLimitType names[10];
    std::memcpy(names, kLimitTypes, sizeof(names));
    const Result rc_cur = svcGetResourceLimitCurrentValues(cur, limit, names, 10);
    const Result rc_max = svcGetResourceLimitLimitValues(max, limit, names, 10);
    svcCloseHandle(limit);
    char text[280];
    int n = 0;
    for (int i = 0; i < 10 && n < static_cast<int>(sizeof(text)) - 40; ++i) {
        n += std::snprintf(text + n, sizeof(text) - n, "%s %lld/%lld  ", kLimitNames[i], static_cast<long long>(cur[i]), static_cast<long long>(max[i]));
    }
    Out("limits (current/maximum, rc 0x%08lx 0x%08lx): %s", static_cast<unsigned long>(rc_cur), static_cast<unsigned long>(rc_max), text);
}

// The threads of the process: id, priority, ideal core, affinity mask. `known` receives the ids so that the watch can say which are new.
int ThreadList(u32* ids, int max_ids, Result* rc_out) {
    s32 count = 0;
    const Result rc = svcGetThreadList(&count, ids, max_ids, CUR_PROCESS_HANDLE);
    if (rc_out) {
        *rc_out = rc;
    }
    return R_SUCCEEDED(rc) ? static_cast<int>(count) : -1;
}

void OneThread(u32 id) {
    Handle h = 0;
    const Result rc = svcOpenThread(&h, CUR_PROCESS_HANDLE, id);
    if (R_FAILED(rc)) {
        Out("  thread 0x%lx: svcOpenThread failed 0x%08lx", static_cast<unsigned long>(id), static_cast<unsigned long>(rc));
        return;
    }
    s32 prio = -99;
    s32 core = -99;
    u8 mask = 0;
    const Result r1 = svcGetThreadPriority(&prio, h);
    const Result r2 = svcGetThreadIdealProcessor(&core, h);
    const Result r3 = svcGetThreadAffinityMask(&mask, h, 4);
    svcCloseHandle(h);
    Out("  thread 0x%lx: priority 0x%x (rc 0x%lx), ideal core %d (rc 0x%lx), affinity mask 0x%x (rc 0x%lx)", static_cast<unsigned long>(id),
        static_cast<unsigned>(prio), static_cast<unsigned long>(r1), static_cast<int>(core), static_cast<unsigned long>(r2), static_cast<unsigned>(mask),
        static_cast<unsigned long>(r3));
}

void Threads() {
    u32 ids[kMaxThreads];
    Result rc = 0;
    const int n = ThreadList(ids, kMaxThreads, &rc);
    if (n < 0) {
        Out("threads: svcGetThreadList failed 0x%08lx (the process may not call it)", static_cast<unsigned long>(rc));
        return;
    }
    Out("threads: %d", n);
    for (int i = 0; i < n && i < kMaxThreads; ++i) {
        OneThread(ids[i]);
    }
}

void State(const char* what) {
    Out("---- %s ----", what);
    Limits();
    Threads();
}

// The watch: ten seconds, every 50 ms; a line when the number of threads, the commit, the CPU time or the events change, and one a second.
void WatchMain(void*) {
    Handle limit = 0;
    if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
        Out("watch: no resource limit handle");
        return;
    }
    static const ResourceLimitType kWatched[4] = {RESLIMIT_THREAD, RESLIMIT_COMMIT, RESLIMIT_CPUTIME, RESLIMIT_EVENT};
    ResourceLimitType names[4];
    std::memcpy(names, kWatched, sizeof(names));
    s64 last[4] = {-2, -2, -2, -2};
    u32 known[kMaxThreads];
    int known_count = 0;
    Out("watch: running on core %d", static_cast<int>(svcGetProcessorID()));
    for (int step = 0; step < kWatchSteps; ++step) {
        s64 cur[4] = {-1, -1, -1, -1};
        svcGetResourceLimitCurrentValues(cur, limit, names, 4);
        bool changed = false;
        for (int i = 0; i < 4; ++i) {
            changed = changed || cur[i] != last[i];
            last[i] = cur[i];
        }
        u32 ids[kMaxThreads];
        Result rc = 0;
        const int n = ThreadList(ids, kMaxThreads, &rc);
        if (changed || step % 20 == 0) {
            Out("watch %3d: thread %lld commit %lld KB cputime %lld event %lld, threads listed %d", step, static_cast<long long>(cur[0]),
                static_cast<long long>(cur[1] / 1024), static_cast<long long>(cur[2]), static_cast<long long>(cur[3]), n);
        }
        // the threads that were not there at the last look (the game's own start here, one by one)
        for (int i = 0; i < n && i < kMaxThreads; ++i) {
            bool seen = false;
            for (int k = 0; k < known_count; ++k) {
                seen = seen || known[k] == ids[i];
            }
            if (!seen) {
                if (known_count < kMaxThreads) {
                    known[known_count++] = ids[i];
                }
                Out("watch %3d: new thread", step);
                OneThread(ids[i]);
            }
        }
        svcSleepThread(kWatchStepNs);
    }
    svcCloseHandle(limit);
    Out("watch: ended");
}

} // namespace

void Begin() {
    LightLock_Init(&g_lock);
    g_t0_ms = platform::NowMs();
    if (File::Open(g_file, kPath, File::RWC | File::TRUNCATE) == File::SUCCESS) {
        g_open = true;
    }
    Out("==== AzaharLink DIAGNOSTIC build (built %s %s): the state of the process, written at once ====", __DATE__, __TIME__);
    LumaFacts();
    State("before the plugin does anything (the state the game will find, without our socket buffer and hooks)");
}

void Mark(const char* what) {
    State(what);
}

bool Skip(const char* what) {
    const std::string path = std::string(kSwitchDir) + what;
    const bool skip = File::Exists(path) == 1;
    if (skip) {
        Out("switch: %s is there: skipping %s", path.c_str(), what);
    }
    return skip;
}

void StartWatch() {
    static ThreadEx* watch = new ThreadEx(&WatchMain, 0x4000, 0x30, 0); // (leaked on purpose: it ends by itself, the process may end first)
    const Result rc = watch->Start(nullptr);
    Out("watch: thread %s (0x%08lx)", R_SUCCEEDED(rc) ? "started" : "NOT started", static_cast<unsigned long>(rc));
}

} // namespace bootdiag
} // namespace ctr
} // namespace azl

#endif // AZL_BOOT_DIAG
