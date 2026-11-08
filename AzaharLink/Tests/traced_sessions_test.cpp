// Tests for the table of the game's real sessions the hook follows (Includes/azl/traced_sessions.hpp).
//   traced_sessions_test
// The case it is for: Pokemon Alpha Sapphire on an Old 3DS (issue 5, 1.1.1). The SDK opens an APT:A session for every APT call, about 13 at each
// HOME, and some closes happen while the game runs its own svc around the swap. With the old table the 16 slots leaked one by one and from the 13th
// HOME on the game's InquireNotification went by unseen (the debug log: `ipc h=... cmd=0x000b0040` instead of `apt InquireNotification`).
// The interleavings of two game threads are forced through the header's two test points (AZL_TRACED_AFTER_CLAIM, AZL_TRACED_BEFORE_EVICT).
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

static thread_local std::function<void()> g_after_claim, g_before_evict;
#define AZL_TRACED_AFTER_CLAIM()                                                                   \
    do {                                                                                           \
        if (g_after_claim) {                                                                       \
            auto f = g_after_claim;                                                                \
            g_after_claim = nullptr;                                                               \
            f();                                                                                   \
        }                                                                                          \
    } while (0)
#define AZL_TRACED_BEFORE_EVICT()                                                                  \
    do {                                                                                           \
        if (g_before_evict) {                                                                      \
            auto f = g_before_evict;                                                               \
            g_before_evict = nullptr;                                                              \
            f();                                                                                   \
        }                                                                                          \
    } while (0)

#include "azl/traced_sessions.hpp"

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

constexpr u32 kSlots = 16;
using Table = TracedTable<kSlots>;

u32 H(u32 counter, u32 index) {
    return (counter << 15) | index;
}

/// The console kernel's handle table as far as the test needs it (KHandleTable): a LIFO list of free indexes, a counter 1..0x7FFF that goes round.
struct Kernel {
    std::vector<u32> free_list;
    u32 next_id;
    explicit Kernel(u32 first_id = 1) : next_id(first_id) {
        for (u32 i = 0x1FF; i >= 0x20; --i) {
            free_list.push_back(i);
        }
    }
    u32 Open() {
        const u32 index = free_list.back();
        free_list.pop_back();
        const u32 h = H(next_id, index);
        next_id = next_id + 1 >= 0x8000 ? 1 : next_id + 1;
        return h;
    }
    void Close(u32 handle) { free_list.push_back(handle & kHandleIndexMask); }
};

/// The game opens a session through srv:GetServiceHandle (as PassThroughGetService does: Opened for every session, Add for the followed ones).
u32 Open(Kernel& k, Table& t, const char* name, bool apt, bool followed = true) {
    const u32 h = k.Open();
    t.Opened(h);
    if (followed) {
        t.Add(h, name, apt);
    }
    return h;
}

void Close(Kernel& k, Table& t, u32 h, bool seen) {
    if (seen) {
        t.Closed(h);
    }
    k.Close(h);
}

bool Followed(Table& t, u32 h, const char* name) {
    const TracedSession* s = t.Find(h);
    return s && std::strcmp(s->name, name) == 0 && s->apt == (std::strncmp(name, "APT:", 4) == 0);
}

bool AptFollowed(Table& t, u32 h) {
    return Followed(t, h, "APT:A");
}

void TestBasics() {
    Table t;
    Kernel k;
    CHECK(t.Find(0) == nullptr);
    CHECK(t.Find(kTracedClaiming) == nullptr);
    CHECK(!t.Add(0, "APT:A", true));
    CHECK(!t.Add(kTracedClaiming, "APT:A", true));
    const u32 cfg = Open(k, t, "cfg:u", false);
    CHECK(Followed(t, cfg, "cfg:u"));
    const u32 apt = Open(k, t, "APT:A", true);
    CHECK(AptFollowed(t, apt));
    Close(k, t, apt, true);
    CHECK(t.Find(apt) == nullptr);
    CHECK(t.Find(cfg) != nullptr);
    CHECK(t.stats.stale == 0 && t.stats.evicted == 0 && t.stats.dropped == 0);
}

/// The log of issue 5: five sessions for the whole launch, then HOME after HOME, each with 13 APT:A sessions opened and closed one after the other,
/// and one of the closes not seen. Every session the game opens is followed, after any number of HOMEs.
void TestPokemonHomeLoop() {
    Table t;
    Kernel k;
    const char* fixed[] = {"cfg:u", "ptm:u", "frd:u", "boss:U", "cecd:u"};
    u32 fixed_h[5];
    for (int i = 0; i < 5; ++i) {
        fixed_h[i] = Open(k, t, fixed[i], false);
    }
    for (int i = 0; i < 6; ++i) {
        Open(k, t, "other", false, false); // (fs:USER, hid:USER, gsp::Gpu ...: not followed)
    }
    bool all_followed = true;
    for (int home = 1; home <= 50; ++home) {
        for (int call = 0; call < 13; ++call) {
            const u32 h = Open(k, t, "APT:A", true);
            all_followed = all_followed && AptFollowed(t, h);
            Close(k, t, h, call != 7); // (one close per HOME out of the hook's sight)
        }
    }
    CHECK(all_followed);
    for (int i = 0; i < 5; ++i) {
        CHECK(Followed(t, fixed_h[i], fixed[i])); // (the sessions of the whole launch are never pushed out)
    }
    CHECK(t.stats.stale > 0);
    CHECK(t.stats.dropped == 0);
}

/// The index of a lost close goes to an object the hook never sees (an event, a thread): the full table gives the APT slot used least recently to the new
/// APT session, never a slot of another service, never an APT session the game keeps and uses; a full table of non-APT sessions drops the newcomer.
void TestEvictionWhenFull() {
    Table t;
    Kernel k;
    std::vector<u32> fixed;
    for (u32 i = 0; i < 5; ++i) {
        fixed.push_back(Open(k, t, "ndm:u", false));
    }
    const u32 kept = Open(k, t, "APT:A", true); // (opened first, used all the time)
    std::vector<u32> leaked;
    for (u32 i = 0; i < kSlots - 6; ++i) {
        leaked.push_back(Open(k, t, "APT:A", true)); // closed unseen; its index stays taken by an object nobody sees
        t.Touch(*t.Find(kept));
    }
    const u32 first = Open(k, t, "APT:A", true);
    CHECK(AptFollowed(t, first));
    CHECK(t.stats.evicted == 1);
    CHECK(t.Find(leaked[0]) == nullptr); // (the one used least recently went)
    CHECK(t.Find(leaked[1]) != nullptr);
    CHECK(AptFollowed(t, kept));
    for (u32 h : fixed) {
        CHECK(Followed(t, h, "ndm:u"));
    }
    const u32 second = Open(k, t, "APT:A", true);
    CHECK(AptFollowed(t, second) && AptFollowed(t, first) && AptFollowed(t, kept));
    CHECK(t.Find(leaked[1]) == nullptr);
    const u32 ptm = Open(k, t, "ptm:u", false);
    CHECK(t.Find(ptm) == nullptr);
    CHECK(t.stats.dropped == 1);
    // a table with no APT entry at all drops an APT newcomer too
    Table u;
    Kernel k2;
    for (u32 i = 0; i < kSlots; ++i) {
        Open(k2, u, "nwm::UDS", false);
    }
    const u32 apt = Open(k2, u, "APT:A", true);
    CHECK(u.Find(apt) == nullptr);
    CHECK(u.stats.dropped == 1 && u.stats.evicted == 0);
}

/// A new handle frees the entries with its index, whatever the counter above it.
void TestStaleRule() {
    Table t;
    t.Add(H(5, 0x53), "APT:A", true);
    t.Add(H(6, 0x54), "cfg:u", false);
    t.Opened(H(7, 0x53));
    CHECK(t.Find(H(5, 0x53)) == nullptr);
    CHECK(t.Find(H(6, 0x54)) != nullptr);
    CHECK(t.stats.stale == 1);
    t.Opened(0);
    t.Opened(kTracedClaiming);
    CHECK(t.stats.stale == 1);
    // the counter can differ in any bit, its lowest one too
    t.Add(H(5, 0x60), "APT:A", true);
    t.Opened(H(6, 0x60));
    CHECK(t.Find(H(5, 0x60)) == nullptr);
    CHECK(t.stats.stale == 2);
    // all 15 bits of the index count: 0x153 and 0x013 are two live handles
    t.Add(H(8, 0x153), "cfg:u", false);
    t.Opened(H(9, 0x013));
    t.Opened(H(9, 0x4153));
    CHECK(t.Find(H(8, 0x153)) != nullptr);
    CHECK(t.stats.stale == 2);
}

/// The counter went round (0x7FFF handles later): the kernel gives the very value of an entry whose close was lost. That entry is dead, and the new
/// session is found with its own service.
void TestCounterWrap() {
    Table t;
    Kernel k(0x7FFE);
    const u32 cfg = Open(k, t, "cfg:u", false);
    Close(k, t, cfg, false); // (lost)
    for (u32 i = 0; i < 0x7FFE; ++i) {
        k.Close(k.Open()); // (objects the hook never sees: the index goes back to the head of the list each time)
    }
    const u32 apt = k.Open();
    CHECK(apt == cfg);
    t.Opened(apt);
    t.Add(apt, "APT:A", true);
    CHECK(AptFollowed(t, apt));
    CHECK(t.stats.stale == 1);
}

/// Review 2026-11-04 (b): thread X claims the last free slot for cfg:u, which held a closed APT session; before X fills it, thread Y adds an APT session
/// to the full table. Y must not take X's slot (it showed the old tenant's APT flag and age), and both end up as they are.
void TestClaimNotStolen() {
    Table t;
    for (u32 i = 0; i < 15; ++i) {
        t.Add(H(1, 0x30 + i), "ndm:u", false);
    }
    t.Add(H(2, 0x50), "APT:A", true);
    t.Closed(H(2, 0x50)); // (the slot is free, with apt=true and an old age left in it)
    bool claiming_found = true;
    g_after_claim = [&] {
        claiming_found = t.Find(kTracedClaiming) != nullptr; // (a slot being filled is nobody's)
        t.Opened(H(5, 0x71));
        t.Add(H(5, 0x71), "APT:A", true);
    };
    t.Opened(H(4, 0x60));
    t.Add(H(4, 0x60), "cfg:u", false);
    CHECK(!claiming_found);
    CHECK(Followed(t, H(4, 0x60), "cfg:u"));
    CHECK(t.Find(H(5, 0x71)) == nullptr); // (the table had no APT entry to give: dropped, as before)
    CHECK(t.stats.dropped == 1 && t.stats.evicted == 0);
}

/// Review 2026-11-04 (a): thread Y picks the APT entry to evict; before its compare-and-swap that session is closed and cfg:u takes the slot. Y must not
/// evict cfg:u.
void TestEvictSeesTheHandleItChose() {
    Table t;
    for (u32 i = 0; i < 15; ++i) {
        t.Add(H(1, 0x30 + i), "ndm:u", false);
    }
    t.Add(H(2, 0x50), "APT:A", true);
    g_before_evict = [&] {
        t.Closed(H(2, 0x50));
        t.Opened(H(3, 0x60));
        t.Add(H(3, 0x60), "cfg:u", false);
    };
    t.Opened(H(4, 0x70));
    t.Add(H(4, 0x70), "APT:A", true);
    CHECK(Followed(t, H(3, 0x60), "cfg:u"));
    CHECK(t.Find(H(4, 0x70)) == nullptr);
    CHECK(t.stats.evicted == 0 && t.stats.dropped == 1);
}

/// Game threads open, follow and close at once: every handle a thread holds open is followed (16 slots, 8 threads, at most 8 open at a time); then
/// the table nearly full, so that the claims and the evictions race, and every slot is checked when the threads are done (ThreadSanitizer build too).
void TestThreads() {
    {
        Table t;
        std::atomic<u32> counter{0};
        std::atomic<int> lost{0};
        std::vector<std::thread> threads;
        for (int n = 0; n < 8; ++n) {
            threads.emplace_back([&, n] {
                for (int i = 0; i < 20000; ++i) {
                    const u32 h = H(counter.fetch_add(1) % 0x7FFF + 1, static_cast<u32>(0x40 + n)); // (each thread its own index: the kernel never gives one twice)
                    t.Opened(h);
                    t.Add(h, "APT:A", true);
                    if (!t.Find(h)) {
                        lost.fetch_add(1);
                    }
                    if (i % 3 != 0) {
                        t.Closed(h);
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        CHECK(lost.load() == 0);
        CHECK(t.stats.dropped == 0);
    }
    {
        Table t;
        for (u32 i = 0; i < 12; ++i) {
            t.Add(H(1, 0x100 + i), "cfg:u", false);
        }
        std::atomic<u32> counter{0};
        std::vector<std::thread> threads;
        for (int n = 0; n < 6; ++n) {
            threads.emplace_back([&, n] {
                for (int i = 0; i < 20000; ++i) {
                    const bool apt = (i + n) % 2 == 0;
                    const u32 h = H(counter.fetch_add(1) % 0x7FFE + 2, static_cast<u32>(0x40 + n));
                    t.Opened(h);
                    t.Add(h, apt ? "APT:A" : "ptm:u", apt);
                    if (TracedSession* s = t.Find(h)) {
                        t.Touch(*s);
                    }
                    if (i % 4 == 0) {
                        t.Closed(h);
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        for (u32 i = 0; i < 12; ++i) {
            CHECK(Followed(t, H(1, 0x100 + i), "cfg:u")); // (no thread ever takes a slot of another service)
        }
        int odd = 0;
        for (const TracedSession& s : t.slots) {
            if (s.handle == kTracedClaiming || (s.handle != 0 && s.apt != (std::strcmp(s.name, "APT:A") == 0))) {
                ++odd;
            }
        }
        CHECK(odd == 0); // (no slot left half filled, no flag of another tenant)
        CHECK(t.stats.evicted + t.stats.dropped > 0); // (the full table was reached)
    }
}

} // namespace

int main() {
    TestBasics();
    TestPokemonHomeLoop();
    TestEvictionWhenFull();
    TestStaleRule();
    TestCounterWrap();
    TestClaimNotStolen();
    TestEvictSeesTheHandleItChose();
    TestThreads();
    std::printf("traced_sessions_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
