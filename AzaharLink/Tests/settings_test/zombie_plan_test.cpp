// zombie_plan_test: the pure part of 3DS Link's look at a closed game still in memory (ConfigApp/source/zombie_plan.hpp): which processes can be
// that game, the memory runs worth writing (shared, locked, the plugin's socket block), the lines, and when "still there" is written again.
#include <cstdio>
#include <string>
#include <vector>

#include "zombie_plan.hpp"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                       \
    } while (0)

static bool Has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

int main() {
    using namespace zombie_plan;
    std::printf("zombie plan: titles\n");
    CHECK(IsGameTitle(0x0004000000197100ull));  // MHXX
    CHECK(IsGameTitle(0x0004000200123400ull));  // a demo
    CHECK(IsGameTitle(0x0004000100123400ull));  // a Download Play child (Kirby's, 3DS Link Delivery's)
    CHECK(!IsGameTitle(0));                     // a title id that could not be read
    CHECK(!IsGameTitle(0x0004000E00197100ull)); // an update: never a process
    CHECK(!IsGameTitle(0x0004013000002E02ull)); // the socket sysmodule
    CHECK(!IsGameTitle(0x0004003000008F02ull)); // HOME menu
    CHECK(IsGameTitle(0x000400000F83D100ull)); // 3DS Link is an application title too: excluded by its pid, not by its title

    std::printf("zombie plan: candidates\n");
    std::vector<Proc> all(4);
    all[0].pid = 3;  all[0].tid = 0x0004013000002E02ull;
    all[1].pid = 40; all[1].tid = 0x0004000000197100ull;
    all[2].pid = 41; all[2].tid = 0x000400000F83D100ull; // 3DS Link
    all[3].pid = 9;  all[3].tid = 0x0004003000008F02ull;
    std::vector<Proc> c = Candidates(all, 41);
    CHECK(c.size() == 1 && c[0].pid == 40);
    CHECK(Candidates(all, 40).size() == 1 && Candidates(all, 40)[0].pid == 41);
    CHECK(Candidates({}, 1).empty());
    std::vector<Proc> unread(1);
    unread[0].pid = 50; // (GetProcessInfo 0x10001 failed: tid 0)
    CHECK(Candidates(unread, 1).empty());

    std::printf("zombie plan: listed\n");
    CHECK(Listed({1, 5, 40}, 40));
    CHECK(!Listed({1, 5}, 40));
    CHECK(!Listed({}, 0));

    std::printf("zombie plan: runs worth writing\n");
    CHECK(WorthWriting(0x10000000, 0x1000, kStateShared));
    CHECK(WorthWriting(0x0dfe0000, 0x20000, kStateLocked));
    CHECK(WorthWriting(0x0dfe0000, 0x20000, 5));          // the socket block, whatever its state (5 = PRIVATE: no memory block made of it any more)
    CHECK(WorthWriting(0x0d000000, 0x1000000, 5));        // a run that contains it
    CHECK(!WorthWriting(0x0d000000, 0xfe0000, 5));        // ends exactly where it starts: not containing
    CHECK(!WorthWriting(0x0e000000, 0x1000, 5));
    CHECK(!WorthWriting(0x08000000, 0x800000, 5));        // the heap: not written
    CHECK(!WorthWriting(0x0dfe0001, 0x10, 5));

    std::printf("zombie plan: lines\n");
    int64_t cur[kClasses] = {87340LL * 1024, 49, 30, 2, 3, 0, 6, 2};
    int64_t lim[kClasses] = {126976LL * 1024, 40, 32, 32, 8, 8, 16, 2};
    const std::string cl = ClassesLine(cur, lim);
    CHECK(Has(cl, "commit KiB 87340/126976"));
    CHECK(Has(cl, ", thread 49/40"));
    CHECK(Has(cl, ", shmem 6/16"));
    CHECK(Has(cl, ", arbiter 2/2"));
    Proc p;
    p.pid = 40;
    p.tid = 0x0004000000197100ull;
    std::snprintf(p.name, sizeof(p.name), "Rabbit_R");
    const std::string pl = ProcLine(p, 1, 0, 2);
    CHECK(Has(pl, "pid 40 'Rabbit_R' title 0004000000197100"));
    CHECK(Has(pl, "terminated (handle signaled), 0 thread(s) left, 2 reference(s) besides ours"));
    CHECK(Has(ProcLine(p, 0, 3, 5), "not terminated yet"));
    CHECK(Has(ProcLine(p, -1, -1, -1), "state unknown"));
    const std::string rl = RunLine(40, 0x0dfe0000, 0x20000, kStateLocked, 0);
    CHECK(Has(rl, "0x0dfe0000 + 0x20000: state 11 (locked: a memory block's source, a stack or TLS) perm 0x0 <- the plugin's socket block"));
    CHECK(Has(RunLine(40, 0x10000000, 0x1000, kStateShared, 1), "state 6 (shared) perm 0x1"));
    CHECK(!Has(RunLine(40, 0x10000000, 0x1000, kStateShared, 1), "socket block"));
    CHECK(Has(GoneLine(40, 125), "pid 40 left memory 125 s after this start"));
    CHECK(Has(StillLine(40, 60), "pid 40 still in memory 60 s after this start"));

    std::printf("zombie plan: class names\n");
    CHECK(std::string(ClassName(0)) == "commit KiB");
    CHECK(std::string(ClassName(6)) == "shmem");
    CHECK(std::string(ClassName(-1)) == "?");
    CHECK(std::string(ClassName(kClasses)) == "?");

    std::printf("zombie plan: the wait's answer\n");
    CHECK(WaitState(0) == 1);             // signaled: TERMINATED
    CHECK(WaitState(0x09401BFEu) == 0);   // the kernel's timeout, a positive result: not terminated yet
    CHECK(WaitState(0xD8E007F7u) == -1);  // an invalid handle: unknown

    std::printf("zombie plan: the references of others\n");
    CHECK(OthersFromHandleInfo(3) == 2);  // (GetHandleInfo 1 counts our handle too)
    CHECK(OthersFromHandleInfo(1) == 0);  // only ours
    CHECK(OthersFromHandleInfo(-1) == -2); // (a failed read is not passed here: the caller writes -1)

    std::printf("zombie plan: the walk of the address space\n");
    uint32_t next = 0;
    CHECK(NextAddress(0x00100000, 0x1000, &next) && next == 0x00101000);
    CHECK(!NextAddress(0x00100000, 0, &next));                 // an empty run: over
    CHECK(!NextAddress(0x3FFFF000, 0x1000, &next));            // the end of the user space
    CHECK(!NextAddress(0xFFFFF000, 0x2000, &next));            // a wrap
    CHECK(NextAddress(0x0dfe0000, 0x20000, &next) && next == 0x0e000000);

    std::printf("zombie plan: when 'still there' is written\n");
    CHECK(!StillDue(0, 59));
    CHECK(StillDue(0, 60));
    CHECK(!StillDue(60, 65));
    CHECK(StillDue(60, 300));
    CHECK(StillDue(300, 900));
    CHECK(!StillDue(900, 1799));
    CHECK(StillDue(900, 1800));
    CHECK(!StillDue(1800, 3599));
    CHECK(StillDue(1800, 3600));
    CHECK(StillDue(0, 4000)); // (a long pause of the app: written once)
    // two hours of ticks every 5 s: written exactly at 60, 300, 900, 1800, 3600 and 5400 s
    std::vector<uint64_t> written;
    uint64_t last = 0;
    for (uint64_t s = 5; s <= 7200; s += 5) {
        if (StillDue(last, s)) {
            last = s;
            written.push_back(s);
        }
    }
    CHECK((written == std::vector<uint64_t>{60, 300, 900, 1800, 3600, 5400, 7200}));

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
