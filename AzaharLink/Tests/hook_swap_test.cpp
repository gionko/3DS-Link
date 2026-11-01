// Tests for the logic of the svc hook around the swap of the plugin's memory (Includes/azl/hook_swap.hpp), over an array that plays the game's text.
//   hook_swap_test
// What the console side adds (ctr/hook.cpp: the real stores, the cache, the wait for the threads in the hook, the counter, svc_entry.s) is held by
// Tests/hook_sim/entry_sim.py (the real assembly in an ARM emulator) and Tests/policy_check.sh (the text of the callers).
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "azl/hook_swap.hpp"

using namespace azl;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

namespace {

constexpr u32 kBase = 0x00100000;
constexpr u32 kSiteStride = 0x40;
constexpr u32 kTrampBase = 0x07000800;

/// The game's text as an array, with the order of every store recorded (the order matters to a thread that passes a site in between).
struct FakeMem {
    std::vector<u32> words;
    std::vector<u32> stores;  // the addresses stored to, in order
    bool record = true;
    std::atomic<u32> syncs{0};
    void (*on_store)(FakeMem&, u32 address) = nullptr;
    explicit FakeMem(u32 sites) : words(sites * kSiteStride / 4 + 16, 0x11111111u) {}
    u32& At(u32 address) { return words[(address - kBase) / 4]; }
    u32 Load(u32 address) { return At(address); }
    void StoreOriginal(u32 address, const u32 w[2]) {
        Put(address, w[0]);
        Put(address + 4, w[1]);
    }
    void StoreHook(u32 address, const u32 w[2]) {
        Put(address + 4, w[1]);
        Put(address, w[0]);
    }
    void Sync() { ++syncs; }
    void Put(u32 address, u32 value) {
        if (on_store) {
            on_store(*this, address);
        }
        if (record) {
            stores.push_back(address);
        }
        At(address) = value;
    }
};

struct World {
    FakeMem mem;
    std::vector<HookSite> sites;
    HandBackLedger ledger;
    volatile u32 state = kHooksActive;
    SwapStats stats;
    explicit World(u32 n) : mem(n) {
        for (u32 i = 0; i < n; ++i) {
            HookSite s;
            s.address = kBase + i * kSiteStride;
            s.original[0] = 0xEF000032;
            s.original[1] = 0xE1A00000 + i; // (a different "next instruction" each)
            s.tramp = kTrampBase + i * 32;
            sites.push_back(s);
            mem.At(s.address) = kSiteJumpWord;
            mem.At(s.address + 4) = s.tramp;
        }
    }
    u32 Count() const { return static_cast<u32>(sites.size()); }
    bool Hooked(u32 i) { return SiteIsOurs(mem, sites[i]); }
    bool Original(u32 i) { return SiteIsOriginal(mem, sites[i]); }
    u32 Return(u32 i) const { return sites[i].tramp + 8; }
    u32 HandBack(u32 i, u32 now = 1000) {
        return HandBackSite(mem, sites.data(), Count(), ledger, &state, Return(i), now);
    }
    u32 Suspend() { return SuspendSites(mem, sites.data(), Count(), ledger, &state, &stats); }
    u32 Resume() { return ResumeSites(mem, sites.data(), Count(), ledger, &state, &stats); }
    u32 Rearm(u32 now) { return RearmSites(mem, sites.data(), Count(), ledger, &state, now, &stats); }
};

void TestSuspendResumeEveryone() {
    World w(40);
    CHECK(w.Suspend() == 40);
    CHECK(w.state == kHooksSuspended);
    CHECK(w.stats.suspends == 1 && w.stats.restored == 40 && w.stats.not_ours == 0);
    CHECK(w.mem.syncs.load() == 1);
    for (u32 i = 0; i < 40; ++i) {
        CHECK(w.Original(i));
        CHECK(w.mem.At(w.sites[i].address) == 0xEF000032);
        CHECK(w.mem.At(w.sites[i].address + 4) == 0xE1A00000 + i);
    }
    CHECK(w.Resume() == 40);
    CHECK(w.state == kHooksActive);
    CHECK(w.stats.resumes == 1 && w.stats.hooked_again == 40);
    for (u32 i = 0; i < 40; ++i) {
        CHECK(w.Hooked(i));
    }
    // twice in a row: nothing to do the second time, nothing broken
    CHECK(w.Suspend() == 40);
    CHECK(w.Suspend() == 0);
    CHECK(w.stats.restored == 0);
    CHECK(w.Resume() == 40);
    CHECK(w.Resume() == 0);
    for (u32 i = 0; i < 40; ++i) {
        CHECK(w.Hooked(i));
    }
}

void TestForeignSitesAreLeftAlone() {
    World w(10);
    // site 3: the framework's hook changed the first word; site 6: somebody changed only the literal
    w.mem.At(w.sites[3].address) = 0xEA000010;
    w.mem.At(w.sites[6].address + 4) = 0x12345678;
    CHECK(w.Suspend() == 8);
    CHECK(w.stats.not_ours == 2);
    CHECK(w.mem.At(w.sites[3].address) == 0xEA000010);
    CHECK(w.mem.At(w.sites[6].address + 4) == 0x12345678);
    CHECK(w.mem.At(w.sites[6].address) == kSiteJumpWord);
    CHECK(w.Resume() == 8);
    CHECK(w.mem.At(w.sites[3].address) == 0xEA000010);
    CHECK(w.mem.At(w.sites[6].address + 4) == 0x12345678);
    for (u32 i = 0; i < 10; ++i) {
        if (i != 3 && i != 6) {
            CHECK(w.Hooked(i));
        }
    }
}

void TestStoreOrder() {
    // a thread that passes a site between its two stores must find (svc, literal) either way, never (ldr pc, next instruction)
    World w(3);
    w.mem.stores.clear();
    w.Suspend();
    CHECK(w.mem.stores.size() == 6);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(w.mem.stores[2 * i] == w.sites[i].address);
        CHECK(w.mem.stores[2 * i + 1] == w.sites[i].address + 4);
    }
    w.mem.stores.clear();
    w.Resume();
    CHECK(w.mem.stores.size() == 6);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(w.mem.stores[2 * i] == w.sites[i].address + 4);
        CHECK(w.mem.stores[2 * i + 1] == w.sites[i].address);
    }
    // the hand-back too
    w.mem.stores.clear();
    CHECK(w.HandBack(1) == w.sites[1].address);
    CHECK(w.mem.stores.size() == 2 && w.mem.stores[0] == w.sites[1].address && w.mem.stores[1] == w.sites[1].address + 4);
}

void TestFindSite() {
    World w(5);
    for (u32 i = 0; i < 5; ++i) {
        CHECK(FindSiteIndex(w.sites.data(), 5, w.Return(i)) == i);
    }
    CHECK(FindSiteIndex(w.sites.data(), 5, kTrampBase) == kNoSite);          // the trampoline itself is not what the bl returns to
    CHECK(FindSiteIndex(w.sites.data(), 5, kTrampBase + 8 + 4) == kNoSite);
    CHECK(FindSiteIndex(w.sites.data(), 5, 0) == kNoSite);
    w.sites[2].tramp = 0; // (a site with no trampoline is never found, whatever the argument)
    CHECK(FindSiteIndex(w.sites.data(), 5, 8) == kNoSite);
    CHECK(FindSiteIndex(w.sites.data(), 0, w.Return(0)) == kNoSite);
}

void TestHandBack() {
    World w(12);
    const u32 site = w.HandBack(4, 5000);
    CHECK(site == w.sites[4].address);
    CHECK(w.Original(4));
    CHECK(LedgerHas(w.ledger, 4) && LedgerAny(w.ledger));
    CHECK(w.ledger.last_ms == 5000);
    for (u32 i = 0; i < 12; ++i) {
        CHECK(i == 4 || w.Hooked(i)); // the others stay in the hook until the swap is announced
    }
    // a second thread through the same trampoline (it entered before the first restored the site): the site is original already, it goes there too
    CHECK(w.HandBack(4, 5001) == w.sites[4].address);
    CHECK(w.ledger.last_ms == 5001);
    // a return address that is no trampoline of ours
    CHECK(HandBackSite(w.mem, w.sites.data(), w.Count(), w.ledger, &w.state, 0x12345678, 5002) == 0);
    // a site somebody else changed since: the call stays with the hook
    w.mem.At(w.sites[5].address) = 0xEA000042;
    CHECK(w.HandBack(5) == 0);
    CHECK(!LedgerHas(w.ledger, 5));
    CHECK(w.mem.At(w.sites[5].address) == 0xEA000042);
}

void TestHandBackWhenSuspended() {
    World w(4);
    w.Suspend();
    CHECK(w.HandBack(1) == 0); // the hooks are out already: nothing to hand back, and the ledger must not grow
    CHECK(!LedgerAny(w.ledger));
    w.Resume();
    CHECK(w.HandBack(1) == w.sites[1].address);
}

void TestLedgerFull() {
    World w(kMaxHandedBack + 3);
    for (u32 i = 0; i < kMaxHandedBack; ++i) {
        CHECK(w.HandBack(i) == w.sites[i].address);
    }
    CHECK(w.HandBack(kMaxHandedBack) == 0);      // no room: that call stays with the hook, and its site is not touched
    CHECK(w.Hooked(kMaxHandedBack));
    CHECK(w.HandBack(2) == w.sites[2].address);   // one that is in the ledger already still goes
    CHECK(w.Rearm(1000 + kRearmDelayMs) == kMaxHandedBack);
    CHECK(!LedgerAny(w.ledger));
    CHECK(w.HandBack(kMaxHandedBack) == w.sites[kMaxHandedBack].address); // room again
}

static std::vector<bool>* g_ledger_seen;
static World* g_world;
static void NoteLedgerAtFirstStore(FakeMem&, u32) {
    g_ledger_seen->push_back(LedgerAny(g_world->ledger));
}

void TestLedgerBeforeTheSiteChanges() {
    // a re-hook (on another thread) that runs after the first store must already find the site in the ledger
    World w(3);
    std::vector<bool> seen;
    g_ledger_seen = &seen;
    g_world = &w;
    w.mem.on_store = NoteLedgerAtFirstStore;
    CHECK(w.HandBack(2) == w.sites[2].address);
    CHECK(seen.size() == 2 && seen[0] && seen[1]);
    w.mem.on_store = nullptr;
}

static std::vector<u32>* g_states_seen;
static void NoteStateAtStore(FakeMem&, u32) {
    g_states_seen->push_back(static_cast<u32>(g_world->state));
}

void TestStateOrder() {
    // the hooks are marked out BEFORE the first site goes back (a hand-back that reads the state in between must not start one), and the state is
    // active again only AFTER the last site is hooked (a hand-back before that would find the sites half done)
    World w(3);
    std::vector<u32> seen;
    g_states_seen = &seen;
    g_world = &w;
    w.mem.on_store = NoteStateAtStore;
    w.Suspend();
    CHECK(seen.size() == 6);
    for (u32 state : seen) {
        CHECK(state == kHooksSuspended);
    }
    seen.clear();
    w.Resume();
    CHECK(seen.size() == 6);
    for (u32 state : seen) {
        CHECK(state == kHooksSuspended);
    }
    CHECK(w.state == kHooksActive);
    w.mem.on_store = nullptr;
}

void TestRearm() {
    World w(6);
    CHECK(w.HandBack(1, 10000) != 0);
    CHECK(w.HandBack(3, 10500) != 0);
    CHECK(w.Rearm(10500 + kRearmDelayMs - 1) == 0); // not yet: the delay counts from the LAST hand-back
    CHECK(w.Original(1) && w.Original(3));
    CHECK(w.Rearm(10500 + kRearmDelayMs) == 2);
    CHECK(w.Hooked(1) && w.Hooked(3));
    CHECK(!LedgerAny(w.ledger));
    CHECK(w.stats.rearmed == 2);
    CHECK(w.Rearm(99999) == 0);
    // not while the hooks are out (the swap is on: the resume puts everything back)
    CHECK(w.HandBack(2, 20000) != 0);
    w.state = kHooksSuspended;
    CHECK(w.Rearm(30000) == 0);
    w.state = kHooksActive;
    // a site changed by somebody else in the meantime is not overwritten
    w.mem.At(w.sites[2].address) = 0xEA000099;
    CHECK(w.Rearm(30000) == 0);
    CHECK(w.mem.At(w.sites[2].address) == 0xEA000099);
    // the clock wraps (49 days of ms)
    World v(2);
    CHECK(v.HandBack(0, 0xFFFFFF00u) != 0);
    CHECK(v.Rearm(0xFFFFFF00u + kRearmDelayMs - 1) == 0);
    CHECK(v.Rearm(0xFFFFFF00u + kRearmDelayMs) == 1); // (modulo 2^32)
}

void TestWholeSwap() {
    World w(20);
    // HOME: the thread that raises the swap is handed back, then the framework says SWAP_ENTER
    CHECK(w.HandBack(7, 100) == w.sites[7].address);
    CHECK(w.Suspend() == 19); // (site 7 is the game's already)
    for (u32 i = 0; i < 20; ++i) {
        CHECK(w.Original(i)); // not one site jumps into the plugin's block when it goes
    }
    CHECK(!LedgerAny(w.ledger));
    CHECK(w.state == kHooksSuspended);
    // the block is back: SWAP_EXIT
    CHECK(w.Resume() == 20);
    for (u32 i = 0; i < 20; ++i) {
        CHECK(w.Hooked(i));
    }
    CHECK(w.state == kHooksActive);
    // the same again (a second HOME)
    CHECK(w.HandBack(2, 9000) == w.sites[2].address);
    CHECK(w.Suspend() == 19);
    CHECK(w.Resume() == 20);
    CHECK(w.Rearm(100000) == 0);
}

void TestConcurrentHandBacks() {
    // the ledger is claimed with a compare-and-swap: game threads that come at once, on different sites (a site's two words are racy by nature,
    // the ledger is not), never lose or double an entry
    for (int round = 0; round < 200; ++round) {
        World w(kMaxHandedBack);
        w.mem.record = false; // (the array of recorded stores is not for threads)
        std::atomic<u32> go{0};
        std::vector<std::thread> threads;
        std::vector<u32> got(kMaxHandedBack, 0);
        for (u32 t = 0; t < kMaxHandedBack; ++t) {
            threads.emplace_back([&, t] {
                while (!go.load()) {
                }
                got[t] = w.HandBack(t, 500 + t);
            });
        }
        go.store(1);
        for (auto& th : threads) {
            th.join();
        }
        for (u32 t = 0; t < kMaxHandedBack; ++t) {
            CHECK(got[t] == w.sites[t].address);
            CHECK(LedgerHas(w.ledger, t));
        }
        u32 slots = 0;
        for (u32 i = 0; i < kMaxHandedBack; ++i) {
            slots += w.ledger.slot[i] != 0;
        }
        CHECK(slots == kMaxHandedBack);
    }
}

} // namespace

int main() {
    TestSuspendResumeEveryone();
    TestForeignSitesAreLeftAlone();
    TestStoreOrder();
    TestFindSite();
    TestHandBack();
    TestHandBackWhenSuspended();
    TestLedgerFull();
    TestLedgerBeforeTheSiteChanges();
    TestStateOrder();
    TestRearm();
    TestWholeSwap();
    TestConcurrentHandBacks();
    std::printf("hook_swap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
