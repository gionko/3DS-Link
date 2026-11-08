// The look at a closed game still in memory: zombie_census.hpp says when it runs, zombie_plan.hpp what it decides and writes.
//
// What it opens. svcOpenProcess on each process of the list, as Rosalina's process list does, only to read its title id; a process that is not a
// game is closed at once, a game is described with that same handle and then closed (one open per process). The one danger of opening a process is
// the instant between its last reference going away and the kernel taking it off the list (KProcess::Destroy): an open in that instant would bring
// back an object being freed. Rosalina's list has the same exposure; a closed game kept in memory has someone else's reference for minutes (that is
// the bug), so the instant is not this one. The ticks after the start only read the process list.
//
// What it never calls: svcGetThreadList. The kernel's GetThreadList takes a reference on the process and never gives it back (kernel 11.14,
// "they forget to decref the process!"): on a closed game it would keep it in memory by itself. The thread count comes from svcGetProcessInfo
// type 7, which gives its reference back (Tests/policy_check.sh forbids svcGetThreadList in the app and in the plugin). Nothing is copied from
// another process's handle table and nothing is written to the kernel.
#include <3ds.h>

#include <cstring>
#include <ctime>
#include <vector>

#include "fileio.hpp"
#include "heap_alloc.hpp"
#include "heap_plan.hpp"
#include "paths.hpp"
#include "zombie_census.hpp"
#include "zombie_plan.hpp"

namespace zombie_census {
namespace {

constexpr std::size_t kLogLines = 200;
constexpr std::size_t kLogBytes = 48u << 10;
constexpr std::size_t kReadBytes = 96u << 10;
constexpr int kMaxPids = 64;

struct Tracked {
    bool on = false;
    u32 pid = 0;
    u64 start_ms = 0;
    u64 last_check_ms = 0;
    u64 still_written_s = 0;
};
Tracked g_tracked;
bool g_notice = false; // the main page shows the message once (TakeNotice)

u64 NowMs() {
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

void Append(const std::string& datadir, const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return;
    }
    char stamp[96];
    if (heap_plan::FormatStamp(static_cast<int64_t>(std::time(nullptr)), static_cast<uint32_t>(NowMs()), stamp, sizeof(stamp)) <= 0) {
        stamp[0] = '\0';
    }
    const std::string path = setup::JoinPath(datadir, "3DSLink-census.log");
    std::string text;
    if (setup::ReadTextFile(path, kReadBytes, text) == setup::LoadResult::Unreadable) {
        return; // (a file that could not be read is not written over)
    }
    for (const std::string& l : lines) {
        text = heap_plan::KeepLast(text, std::string(stamp) + l, kLogLines, kLogBytes);
    }
    setup::WriteTextFile(path, text);
}

bool OnEmulator() {
    s64 v = 0;
    svcGetSystemInfo(&v, 0x20000, 0); // (1 on Citra and Azahar; a console's Luma answers with 0 in it)
    return v == 1;
}

// False when the list could not be read (nothing is decided on a failed read).
bool ListPids(std::vector<u32>& out) {
    s32 n = 0;
    u32 pids[kMaxPids] = {};
    if (R_FAILED(svcGetProcessList(&n, pids, kMaxPids)) || n < 0) {
        return false;
    }
    out.assign(pids, pids + (n > kMaxPids ? kMaxPids : n));
    return true;
}

// The classes of the APPLICATION limit (3DS Link is in it too: the closed game's live objects are what a clean start does not have).
std::string ClassesLine() {
    Handle rl = 0;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        return "classes of the APPLICATION limit: not readable";
    }
    ResourceLimitType types[zombie_plan::kClasses];
    for (int i = 0; i < zombie_plan::kClasses; ++i) {
        types[i] = static_cast<ResourceLimitType>(i + 1); // (COMMIT 1 .. ADDRESSARBITER 8)
    }
    int64_t cur[zombie_plan::kClasses] = {};
    int64_t lim[zombie_plan::kClasses] = {};
    svcGetResourceLimitCurrentValues(cur, rl, types, zombie_plan::kClasses);
    svcGetResourceLimitLimitValues(lim, rl, types, zombie_plan::kClasses);
    svcCloseHandle(rl);
    return zombie_plan::ClassesLine(cur, lim);
}

// The closed game, through the handle the scan opened: state, threads, references, its own handles, the memory runs worth writing.
void Describe(const zombie_plan::Proc& p, Handle h, std::vector<std::string>& out) {
    const int signaled = zombie_plan::WaitState(static_cast<uint32_t>(svcWaitSynchronization(h, 0)));
    s64 v = -1;
    const int threads = R_SUCCEEDED(svcGetProcessInfo(&v, h, 7)) ? static_cast<int>(v) : -1; // (type 7: balanced, unlike GetThreadList)
    v = -1;
    const long long handles = R_SUCCEEDED(svcGetProcessInfo(&v, h, 4)) ? static_cast<long long>(v) : -1; // (its own handle table, in use)
    s64 refs = -1;
    const long long others = R_SUCCEEDED(svcGetHandleInfo(&refs, h, 1)) ? zombie_plan::OthersFromHandleInfo(refs) : -1;
    out.push_back(zombie_plan::ProcLine(p, signaled, threads, others) + ", " + std::to_string(handles) + " handle(s) in its own table");
    u32 addr = 0;
    int runs = 0;
    for (int guard = 0; guard < 4096 && runs < 48; ++guard) {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryProcessMemory(&mi, &pi, h, addr))) {
            break;
        }
        if (zombie_plan::WorthWriting(mi.base_addr, mi.size, mi.state)) {
            out.push_back(zombie_plan::RunLine(p.pid, mi.base_addr, mi.size, mi.state, mi.perm));
            ++runs;
        }
        if (!zombie_plan::NextAddress(mi.base_addr, mi.size, &addr)) {
            break;
        }
    }
}

} // namespace

void Run(const std::string& datadir) {
    static bool done = false; // (one look per start, whoever calls)
    if (done) {
        return;
    }
    done = true;
    std::vector<std::string> lines;
    lines.push_back(ClassesLine());
    if (!heap_alloc::ForeignAtStart() || OnEmulator()) {
        Append(datadir, lines);
        return;
    }
    u32 self = 0;
    svcGetProcessId(&self, CUR_PROCESS_HANDLE);
    std::vector<u32> pids;
    if (!ListPids(pids)) {
        lines.push_back("memory of another process at start, but the process list could not be read");
        Append(datadir, lines);
        return;
    }
    int read = 0;
    std::vector<zombie_plan::Proc> games;
    for (u32 pid : pids) {
        if (pid == self) {
            continue;
        }
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pid))) {
            continue;
        }
        ++read;
        zombie_plan::Proc p;
        p.pid = pid;
        s64 tid = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&tid, h, 0x10001))) { // (Luma: the title id of the process's code set)
            p.tid = static_cast<u64>(tid);
        }
        s64 name = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&name, h, 0x10000))) { // (Luma: the 8 letters of its name)
            std::memcpy(p.name, &name, 8);
            p.name[8] = '\0';
        }
        if (zombie_plan::IsGameTitle(p.tid)) {
            games.push_back(p);
            Describe(p, h, lines);
        }
        svcCloseHandle(h);
    }
    if (games.empty()) {
        lines.push_back("memory of another process at start, but no other game in the process list (" + std::to_string(read) + " of " +
                        std::to_string(pids.size()) + " processes read)");
    } else if (games.size() > 1) {
        lines.push_back(std::to_string(games.size()) + " other games in memory: the first one is followed");
    }
    Append(datadir, lines);
    if (!games.empty()) {
        g_tracked.on = true;
        g_tracked.pid = games.front().pid;
        g_tracked.start_ms = NowMs();
        g_tracked.last_check_ms = g_tracked.start_ms;
        g_notice = true;
    }
}

bool TakeNotice() {
    const bool n = g_notice;
    g_notice = false;
    return n;
}

void Tick(const std::string& datadir) {
    if (!g_tracked.on) {
        return;
    }
    const u64 now = NowMs();
    if (now - g_tracked.last_check_ms < 5000) {
        return;
    }
    g_tracked.last_check_ms = now;
    const u64 secs = (now - g_tracked.start_ms) / 1000;
    std::vector<u32> pids;
    if (!ListPids(pids)) {
        return; // (a failed read says nothing: the next tick looks again)
    }
    if (!zombie_plan::Listed(pids, g_tracked.pid)) {
        g_tracked.on = false;
        Append(datadir, {zombie_plan::GoneLine(g_tracked.pid, secs)});
        return;
    }
    if (zombie_plan::StillDue(g_tracked.still_written_s, secs)) {
        g_tracked.still_written_s = secs;
        Append(datadir, {zombie_plan::StillLine(g_tracked.pid, secs)});
    }
}

} // namespace zombie_census
