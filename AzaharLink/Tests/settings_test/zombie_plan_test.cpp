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

// (DPAPP-2j, R-A) the judgement before pm:app UnregisterProcess, and a model of pm (Luma3DS_reference/sysmodules/pm/source: process_data.c:32-43
// takes the FIRST node whose id without the low byte is the asked one's; manager.c:71-92 deletes it whatever its state): no verdict Free ever lets a
// live process, 3DS Link, a key that is not ours or an emulator's be touched.
namespace {
struct PmNode {
    uint32_t pid;
    uint64_t tid;
    bool alive;
};
// pm's UnregisterProcess on the model: the first node of the key goes; its pid, 0 when none
uint32_t PmUnregister(std::vector<PmNode>& nodes, uint64_t key) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if ((nodes[i].tid & ~0xFFull) == (key & ~0xFFull)) {
            const uint32_t pid = nodes[i].pid;
            nodes.erase(nodes.begin() + static_cast<long>(i));
            return pid;
        }
    }
    return 0;
}
} // namespace

static void TestFree2j() {
    using namespace zombie_plan;
    std::printf("zombie plan: freeing a closed guest of the form M3c1 (DPAPP-2j)\n");
    const uint64_t key = 0x000400000F8F8200ull; // (Island Tour's guest: the ACI's id pm registered)
    const uint32_t self = 40;
    int calls = -1;
    // T1: one closed game of the key: Free, one call
    std::vector<ProcFact> one = {{30, 0x0004013000002E02ull, 0, 3}, {41, key, 1, 0}};
    CHECK(Judge(one, self, key, true, false, &calls) == FreeVerdict::Free && calls == 1);
    // T2: a brother alive with another low byte (pm would take the FIRST node of the key): never touched
    std::vector<ProcFact> brother = {{41, key, 1, 0}, {42, key | 0x05, 0, 4}};
    CHECK(Judge(brother, self, key, true, false, &calls) == FreeVerdict::Running && calls == 0);
    // T3: 3DS Link's own key (with or without its low byte) is never one, though it is in the band
    CHECK(Judge(one, self, 0x000400000F83D100ull, true, false, &calls) == FreeVerdict::NotOurs && calls == 0);
    CHECK(Judge(one, self, 0x000400000F83D1FFull, true, false, &calls) == FreeVerdict::NotOurs);
    CHECK(!FreeableKey(0x000400000F83D100ull) && !FreeableKey(0x000400000F83D142ull) && FreeableKey(key));
    // T4: the other forms' ids of the band do not count (pm compares the whole high word); the key is always 00040000
    std::vector<ProcFact> forms = {{43, 0x000400010F8F8200ull, 0, 2}, {44, 0x000400100F8F8200ull, 0, 2}, {45, 0x000400020F8F8200ull, 0, 2}};
    CHECK(Judge(forms, self, key, true, false, &calls) == FreeVerdict::Nothing && calls == 0);
    CHECK(Judge(one, self, 0x000400010F8F8200ull, true, false, &calls) == FreeVerdict::NotOurs); // (an id of AM is never a key)
    CHECK(Judge(one, self, 0x000400100F8F8200ull, true, false, &calls) == FreeVerdict::NotOurs);
    // T5: state or threads not known, not terminated, a thread left
    for (const ProcFact& p : {ProcFact{41, key, -1, 0}, ProcFact{41, key, 1, -1}}) {
        CHECK(Judge({p}, self, key, true, false, &calls) == FreeVerdict::Unsure && calls == 0);
    }
    for (const ProcFact& p : {ProcFact{41, key, 0, 0}, ProcFact{41, key, 1, 1}, ProcFact{41, key, 0, -1}}) {
        CHECK(Judge({p}, self, key, true, false, &calls) == FreeVerdict::Running && calls == 0);
    }
    CHECK(WaitState(0x09401BFEu) == 0 && WaitState(0) == 1 && WaitState(0xD8E007F7u) == -1); // (the timeout is POSITIVE: not terminated)
    // T6: a key no record of ours names (a homebrew of the band, FBI's): never
    CHECK(Judge({{41, 0x000400000F800100ull, 1, 0}}, self, 0x000400000F800100ull, false, false, &calls) == FreeVerdict::NotOurs && calls == 0);
    // T7: our own process with the key: never
    CHECK(Judge({{self, key, 1, 0}}, self, key, true, false, &calls) == FreeVerdict::Self && calls == 0);
    // T8: an emulator: never (pm is HLE)
    CHECK(Judge(one, self, key, true, true, &calls) == FreeVerdict::Emulator && calls == 0);
    // T9: two closed games of the key: two calls; the bound of 8
    std::vector<ProcFact> two = {{41, key, 1, 0}, {46, key | 0x01, 1, 0}};
    CHECK(Judge(two, self, key, true, false, &calls) == FreeVerdict::Free && calls == 2);
    std::vector<ProcFact> many;
    for (uint32_t i = 0; i < 12; ++i) {
        many.push_back({50 + i, key, 1, 0});
    }
    CHECK(Judge(many, self, key, true, false, &calls) == FreeVerdict::Free && calls == kMaxFreeCalls && kMaxFreeCalls == 8);
    CHECK(Judge({}, self, key, true, false, &calls) == FreeVerdict::Nothing && calls == 0);
    // T10: on random lists, pm's model never loses a live node, 3DS Link's or another key's when the verdict is Free and the calls are made
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    int frees = 0;
    for (int round = 0; round < 20000; ++round) {
        std::vector<ProcFact> facts;
        std::vector<PmNode> pm;
        const int n = 1 + static_cast<int>((seed >> 60) & 7);
        for (int i = 0; i < n; ++i) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const uint64_t tids[] = {key, key | 0x03, 0x000400000F83D100ull, 0x000400010F8F8200ull, 0x000400000F812300ull, 0x0004013000002E02ull};
            ProcFact p;
            p.pid = 60 + static_cast<uint32_t>(i);
            p.tid = tids[(seed >> 33) % 6];
            p.signaled = static_cast<int>((seed >> 40) % 3) - 1;
            p.threads = static_cast<int>((seed >> 45) % 3) - 1;
            if ((seed >> 50) % 9 == 0) {
                p.pid = self;
            }
            facts.push_back(p);
            pm.push_back({p.pid, p.tid, !(p.signaled == 1 && p.threads == 0)});
        }
        const bool registered = (seed >> 55) % 4 != 0;
        const FreeVerdict v = Judge(facts, self, key, registered, false, &calls);
        if (v != FreeVerdict::Free) {
            CHECK(calls == 0);
            continue;
        }
        ++frees;
        for (int c = 0; c < calls; ++c) {
            const uint32_t gone = PmUnregister(pm, key);
            bool was_dead_of_key = false;
            for (const ProcFact& p : facts) {
                was_dead_of_key = was_dead_of_key || (p.pid == gone && SameKeyAsPm(p.tid, key) && p.signaled == 1 && p.threads == 0 && p.pid != self);
            }
            CHECK(gone != 0 && was_dead_of_key);
        }
        for (const PmNode& node : pm) {
            CHECK(!(SameKeyAsPm(node.tid, key)) || !node.alive); // (nothing alive of the key was ever there to be lost)
        }
    }
    CHECK(frees > 100); // (the random lists did reach the verdict Free)
    // T11: the command and the lines
    CHECK(kPmAppUnregisterHeader == 0x000C0080u);
    const std::string look = FreeLookLine(key, FreeVerdict::Free, 1, 3);
    CHECK(look == "free: key 000400000f8f8200: 1 process(es) of the key, terminated, no thread left, 3 ms");
    const std::string call = FreeCallLine(41, "3STARS", key, 0x000400010F8F8200ull, 0, 0, 1);
    CHECK(Has(call, "free: pid 41 '3STARS' key 000400000f8f8200 (AM 000400010f8f8200)") && Has(call, "UnregisterProcess rc=0x00000000 answer=0x00000000 (1 ms)"));
    CHECK(FreeAfterLine(41, true, 50, 0) == "free: pid 41 left memory after 50 ms");
    CHECK(Has(FreeAfterLine(41, false, 500, 1), "still in memory after 500 ms, 1 reference(s) besides ours: not pm's (Rosalina or the kernel): restart the console"));
    CHECK(std::string(VerdictWord(FreeVerdict::Running)) == "running" && std::string(VerdictWord(FreeVerdict::Emulator)) == "emulator");
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
    // (DPAPP-2h-sec) the Download Play guest of the build 2h: its process is 00040010 in the band (Island Tour's 000400100F8F8200), the same
    // answer as azl::title_rules::PreparedSlotOfProcess; never the system's own applications of 00040010
    CHECK(IsGameTitle(0x000400100F8F8200ull));
    CHECK(IsGameTitle(0x000400100F800000ull) && IsGameTitle(0x000400100FFFFF00ull));
    CHECK(!IsGameTitle(0x000400100F7FFF00ull) && !IsGameTitle(0x0004001010000000ull)); // (just out of the band)
    CHECK(!IsGameTitle(0x0004001000022100ull)); // the system's Download Play application (EUR)
    CHECK(!IsGameTitle(0x0004001000022000ull) && !IsGameTitle(0x0004001000020000ull)); // System Settings, the camera...
    CHECK(!IsGameTitle(0x000400120F8F8200ull)); // 00040012 is not a guest's
    for (uint64_t t : {0x000400100F8F8200ull, 0x000400000F8F8200ull, 0x000400100F7FFF00ull, 0x0004001000022100ull, 0x000400010F8F8200ull}) {
        const bool band = azl::title_rules::PreparedSlotOfProcess(t) != 0;
        CHECK(!band || IsGameTitle(t)); // every process PreparedSlotOfProcess calls a prepared child is a game here
    }
    {
        std::vector<Proc> two(2);
        two[0].pid = 41; two[0].tid = 0x000400000F83D100ull; // 3DS Link
        two[1].pid = 60; two[1].tid = 0x000400100F8F8200ull; // the guest left in memory
        CHECK(Candidates(two, 41).size() == 1 && Candidates(two, 41)[0].pid == 60);
    }

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

    TestFree2j();
    // (DPAPP-2j-rev) the gate's list: a process that could not be opened and is gone from the list read again left in between (the list whole);
    // one still listed is not known
    CHECK(zombie_plan::NoneListed({1, 2, 3}, {}) && zombie_plan::NoneListed({1, 2, 3}, {4, 5}) && !zombie_plan::NoneListed({1, 2, 3}, {4, 2}));
    CHECK(!zombie_plan::NoneListed({7}, {7}) && zombie_plan::NoneListed({}, {7}));
    // (DPAPP-2j-rev) the 500 ms of the freeing only when pm gave a reference back; no fall (a reference not pm's): one look, no wait
    CHECK(zombie_plan::WaitAfterUnregister(1, 0) && zombie_plan::WaitAfterUnregister(2, 1));
    CHECK(!zombie_plan::WaitAfterUnregister(1, 1) && !zombie_plan::WaitAfterUnregister(2, 2) && !zombie_plan::WaitAfterUnregister(0, 0));
    CHECK(zombie_plan::WaitAfterUnregister(-1, 1) && zombie_plan::WaitAfterUnregister(1, -1)); // (a count not read: as before)
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
