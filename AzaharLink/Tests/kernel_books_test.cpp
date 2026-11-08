// Tests for the kernel's books (Includes/azl/books_math.hpp) on a made-up KResourceLimit laid out like the kernel's (KAutoObject: vtable,
// refcount; s32 limit[10]; s32 current[10]), with the kernel's own rules around it: what the game sees (limit - current), the reservation of
// svcControlMemory's ALLOC (refused above the limit, released again when the region has no pages), the release of its FREE (whoever charged
// the block), Luma3DS's svc 0xA3 that touches no books, and the end of the process (KProcess::Finalize gives back the sum of the private
// blocks; KResourceLimit::Release gives back NOTHING when the sum is above the books). The steps the console runs (RaiseThroughKernel,
// ChargeThroughKernel, ReleaseThroughKernel, and ChargeOrWrite / UnchargeOrWrite with their fallback that writes current[] when the kernel
// has no pages) run here as they are, over that made-up kernel; a game thread on the other core can act between any two of their calls.
//   kernel_books_test
// The console side (finding the live object, kernel mode, the svcs) is ctr/kernel_books.cpp and only a console runs it.
#include <cstddef>
#include <cstdio>
#include <functional>
#include <map>
#include <string_view>

#include "azl/books_math.hpp"

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

struct FakeResourceLimit {
    u32 vtable;
    u32 refcount;
    s32 limit[books::kSlots];
    s32 current[books::kSlots];
};
static_assert(offsetof(FakeResourceLimit, limit) == books::kLimitsOffset, "limit[] at 0x08");
static_assert(offsetof(FakeResourceLimit, current) == books::kCurrentOffset, "current[] at 0x30");

constexpr s32 kRefused = static_cast<s32>(0xC860180A); // Kernel11 ControlMemory: the reservation refused (13890)
constexpr s32 kNoPages = static_cast<s32>(0xD86007F3); // no physical page left in the region
constexpr s32 kBadFree = static_cast<s32>(0xE0A01BF5); // a FREE of an address with no block
// KPageTable::Operate (svc 0xA3's ALLOC): the address is not in a FREE or RESERVED block that covers the range, checked under the page
// table's mutex, nothing mapped (31902-31948)
constexpr s32 kBusyAddress = static_cast<s32>(0xD900060C);
static_assert(kNoPages == books::kNoPages, "the answer the fallback is for");
static_assert(kRefused == books::kAboveLimit, "the refusal the lift of a Charge keeps away from its own reservation");

/// A process, its region and the kernel's rules for its commit.
struct Process {
    FakeResourceLimit rl{};
    s32 private_blocks = 0; // what KProcess::Finalize gives back at the end
    s32 region_free = 0;    // the free pages of the process's region (APPLICATION)
    s32 system_free = 16 * 1024 * 1024; // ... of the SYSTEM region (svc 0xA3)
    s32 base_free = 4 * 1024 * 1024;    // ... of the BASE region (svc 0xA3, the last one ReleaseThroughKernel tries)
    /// `extra`: pages of the region beyond the budget (a New 3DS running an Old 3DS title: the region is bigger than the limit pm sets).
    explicit Process(s32 budget, s32 used, s32 extra = 0) {
        rl.limit[books::kCommit] = budget;
        rl.current[books::kCommit] = used;
        rl.limit[books::kThread] = 32;
        rl.limit[books::kEvent] = 64;
        rl.limit[books::kMutex] = 16;
        private_blocks = used;
        region_free = budget - used + extra;
    }
    s32 Limit() const { return rl.limit[books::kCommit]; }
    s32 Commit() const { return rl.current[books::kCommit]; }
    s64 Room() { return books::Room(rl.limit, rl.current); }
    /// KResourceLimit::Reserve (25211: refused when current + amount is above the limit).
    bool Reserve(s32 bytes) {
        if (static_cast<s64>(rl.current[books::kCommit]) + bytes > rl.limit[books::kCommit]) {
            return false;
        }
        rl.current[books::kCommit] += bytes;
        return true;
    }
    /// KResourceLimit::Release (32171: nothing when the amount is above current).
    void Release(s32 bytes) {
        if (bytes <= rl.current[books::kCommit]) {
            rl.current[books::kCommit] -= bytes;
        }
    }
    /// The game's own svcControlMemory(ALLOC): the reservation first, then the pages (released again without them).
    bool Allocate(s32 bytes) {
        if (!Reserve(bytes)) {
            return false;
        }
        if (region_free < bytes) {
            Release(bytes);
            return false;
        }
        region_free -= bytes;
        private_blocks += bytes;
        return true;
    }
    /// The game's svcControlMemory(FREE) of its own block.
    void Free(s32 bytes) {
        region_free += bytes;
        private_blocks -= bytes;
        Release(bytes);
    }
    /// svcControlMemoryUnsafe from the SYSTEM region: charged to nobody, a private block all the same.
    void AllocateOutside(s32 bytes) {
        system_free -= bytes;
        private_blocks += bytes;
    }
    /// svcControlMemoryUnsafe's FREE of it: the block goes, the books are not touched.
    void FreeOutside(s32 bytes) {
        system_free += bytes;
        private_blocks -= bytes;
    }
    /// svcControlMemory's FREE of it (Kernel11 ControlMemory 13876-13877: MEMOP_FREE releases `size` of the commit after the pages go,
    /// whoever charged them).
    void FreeOutsideAsCharged(s32 bytes) {
        FreeOutside(bytes);
        Release(bytes);
    }
    /// The end of the process: true when the books took the sum back (else the budget is lost until a reboot).
    bool Finalize() {
        if (private_blocks > rl.current[books::kCommit]) {
            return false; // KResourceLimit::Release: nothing given back
        }
        rl.current[books::kCommit] -= private_blocks;
        private_blocks = 0;
        return true;
    }
};

/// The kernel's calls of the steps (the console's are ctr/kernel_books.cpp, ConsoleKernel) on a made-up kernel, with the same rules:
/// svcControlMemory reserves (and releases again when no pages are had) and its FREE releases; svc 0xA3 touches no books.
struct FakeKernel {
    struct Block {
        u32 bytes;
        books::Region region; // where its pages came from (svcControlMemory's: the process's region, APPLICATION)
    };
    Process& p;
    std::map<u32, Block> blocks;
    u32 next_linear = 0x30000000;
    /// A game thread on the other core, between two calls: "lift", "reserve" (before AllocCharged), "free" (before FreeUncharged), "lower".
    std::function<void(const char*)> between;
    bool span = true;         // FreeSpan finds a place
    int fail_free_unsafe = 0; // that many svc 0xA3 FREEs fail
    int charged_frees_ok = -1; // that many svcControlMemory FREEs are made, then they fail (-1: all made)
    int reads = 0, lifts = 0, lowers = 0, reserves = 0, releases = 0;
    s32 lifted = 0, lowered = 0; // the sums Lift and Lower moved the limit by
    int writes = 0;        // WriteCurrent: current[] written in kernel mode (the fallback)
    s32 written = 0;       // ... the sum written
    int calls_before_write = -1; // the kernel's calls (reserves + AllocUncharged) made before the first write (-1: none written)
    int uncharged_allocs = 0;
    books::Region last_region = books::Region::System; // the region of the last svc 0xA3 ALLOC that was had

    explicit FakeKernel(Process& process) : p(process) {}
    void At(const char* step) {
        if (between) {
            between(step);
        }
    }
    void Read(s32* limit, s32* current) {
        At("read");
        ++reads;
        *limit = p.rl.limit[books::kCommit];
        *current = p.rl.current[books::kCommit];
    }
    void Lift(u32 bytes) {
        At("lift");
        ++lifts;
        lifted += static_cast<s32>(bytes);
        books::Lift(p.rl.limit, static_cast<s32>(bytes));
    }
    void Lower(u32 bytes) {
        At("lower");
        ++lowers;
        lowered += static_cast<s32>(bytes);
        books::Settle(p.rl.limit, static_cast<s32>(bytes));
    }
    s32 AllocCharged(u32 bytes, u32* address) {
        At("reserve");
        ++reserves;
        if (!p.Reserve(static_cast<s32>(bytes))) {
            return kRefused;
        }
        if (p.region_free < static_cast<s32>(bytes)) {
            p.Release(static_cast<s32>(bytes)); // (13882-13884)
            return kNoPages;
        }
        p.region_free -= static_cast<s32>(bytes);
        p.private_blocks += static_cast<s32>(bytes);
        *address = next_linear;
        blocks[next_linear] = Block{bytes, books::Region::Application};
        next_linear += bytes;
        return 0;
    }
    bool Unmap(u32 address, u32 bytes) {
        const auto it = blocks.find(address);
        if (it == blocks.end() || it->second.bytes != bytes) {
            return false;
        }
        Free(it->second.region) += static_cast<s32>(bytes);
        p.private_blocks -= static_cast<s32>(bytes);
        blocks.erase(it);
        return true;
    }
    s32 FreeUncharged(u32 address, u32 bytes) {
        At("free");
        if (fail_free_unsafe > 0) {
            --fail_free_unsafe;
            return kBadFree;
        }
        return Unmap(address, bytes) ? 0 : kBadFree;
    }
    s32& Free(books::Region region) {
        return region == books::Region::System ? p.system_free : region == books::Region::Application ? p.region_free : p.base_free;
    }
    s32 AllocUncharged(u32 address, u32 bytes, books::Region region) {
        At("alloc_uncharged");
        ++uncharged_allocs;
        if (blocks.count(address) != 0) {
            return kBusyAddress; // (the state is checked before any page is taken)
        }
        if (Free(region) < static_cast<s32>(bytes)) {
            return kNoPages;
        }
        Free(region) -= static_cast<s32>(bytes);
        p.private_blocks += static_cast<s32>(bytes);
        blocks[address] = Block{bytes, region};
        last_region = region;
        return 0;
    }
    s32 FreeCharged(u32 address, u32 bytes) {
        if (charged_frees_ok == 0) {
            return kBadFree;
        }
        if (charged_frees_ok > 0) {
            --charged_frees_ok;
        }
        if (!Unmap(address, bytes)) {
            return kBadFree;
        }
        ++releases;
        p.Release(static_cast<s32>(bytes)); // (13876-13877)
        return 0;
    }
    u32 FreeSpan(u32 bytes) {
        return span ? 0x0FF00000 - bytes : 0u;
    }
    void WriteCurrent(s32 bytes) {
        At("write");
        if (writes++ == 0) {
            calls_before_write = reserves + uncharged_allocs;
        }
        written += bytes;
        books::Charge(p.rl.current, bytes);
    }
};

constexpr s32 kPage = 2 * 0x1000; // the gate's page (two pages)

/// TakeGatePage: the page from svc 0xA3, then RaiseBooks; a Raise refused frees the page with svc 0xA3. True: raised.
bool TakePageAndRaise(Process& p, FakeKernel& k, s32* rc = nullptr) {
    p.AllocateOutside(kPage);
    const s32 r = books::RaiseThroughKernel(k, kPage);
    if (rc != nullptr) {
        *rc = r;
    }
    if (r != 0) {
        p.FreeOutside(kPage);
    }
    return r == 0;
}

bool TakePageAndRaise(Process& p) {
    FakeKernel k(p);
    return TakePageAndRaise(p, k);
}

// The gate's page: taken before the game sizes its heap, the room the game sees does not change; a game that sizes its heap on limit - current
// (libctru's way) takes ALL of it, "to the byte"; after the heap the limit goes back, current above it by the page; the end balances.
// (Monster Hunter XX does not size its heap on the limit: it reads APPMEMALLOC, TestChargeAfterTheGatePage.)
void TestRaiseThenSettle() {
    Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
    const s64 room = p.Room();
    FakeKernel k(p);
    CHECK(TakePageAndRaise(p, k));
    CHECK(k.lifts == 1 && k.lowers == 0 && k.reserves == 1 && k.releases == 0 && k.blocks.empty()); // (the block of the reservation gone)
    CHECK(p.Room() == room); // the game sees what it saw
    CHECK(p.Limit() == 80 * 1024 * 1024 + kPage);
    CHECK(p.Commit() == 3 * 1024 * 1024 + kPage);
    CHECK(p.region_free == 80 * 1024 * 1024 - 3 * 1024 * 1024); // (its pages back in the region)
    CHECK(p.Allocate(static_cast<s32>(room)));                    // its heap, to the byte
    CHECK(!p.Allocate(1));                                        // and not a byte more
    books::Settle(p.rl.limit, kPage);
    CHECK(p.Limit() == 80 * 1024 * 1024); // the limit as the system gave it
    CHECK(p.Room() == -kPage);            // (above the limit by the page: such a game had no budget left anyway)
    CHECK(p.Finalize());
    CHECK(p.Commit() == 0); // the next game finds the budget whole
    // the other slots were never touched
    CHECK(p.rl.limit[books::kThread] == 32 && p.rl.limit[books::kEvent] == 64 && p.rl.limit[books::kMutex] == 16);
    CHECK(p.rl.current[books::kThread] == 0 && p.rl.limit[0] == 0 && p.rl.current[0] == 0);
}

// A game that does not take all its room: the same.
void TestRaiseThenSettleWithRoomLeft() {
    Process p(64 * 1024 * 1024, 1024 * 1024);
    const s64 room = p.Room();
    CHECK(TakePageAndRaise(p));
    CHECK(p.Room() == room);
    CHECK(p.Allocate(40 * 1024 * 1024));
    books::Settle(p.rl.limit, kPage);
    CHECK(p.Room() == room - 40 * 1024 * 1024 - kPage);
    CHECK(p.Finalize() && p.Commit() == 0);
}

// The process dies before the settle with no EXIT event (killed while the plugin is swapped out, a crash): the end still balances, but the
// limit stays higher by the page, and on an Old 3DS (or a New 3DS in memtype 7) nobody puts it back (pm writes it only at boot): the next game
// would see room the region does not have.
void TestDeathBeforeSettle() {
    Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
    CHECK(TakePageAndRaise(p));
    CHECK(p.Finalize());
    CHECK(p.Commit() == 0);
    CHECK(p.Limit() == 80 * 1024 * 1024 + kPage); // (the category's object outlives the process)
}

// The same game closed before LateInit, WITH the EXIT event (it called svcExitProcess): the keep-alive thread settles what is owed, the
// limit goes under current by the page (no check of the kernel's looks at that: Release compares the amount with current alone), Finalize
// gives back the game AND the page, and the limit is the one of boot. LateInit, if it still comes, finds nothing owed.
void TestDeathBeforeSettleWithExit() {
    constexpr s32 kLimit = 80 * 1024 * 1024;
    Process p(kLimit, 3 * 1024 * 1024);
    u32 owed = 0;
    CHECK(TakePageAndRaise(p));
    owed = kPage; // (TakeGatePage: raised, and Settle owed)
    CHECK(p.Allocate(static_cast<s32>(p.Room()))); // a game that took everything ...
    const u32 at_exit = books::TakeOwed(&owed);     // ... and exits: the EXIT event
    CHECK(at_exit == static_cast<u32>(kPage));
    books::Settle(p.rl.limit, static_cast<s32>(at_exit));
    CHECK(p.Limit() == kLimit);
    CHECK(p.Room() == -kPage);          // (current above the limit by the page until the end)
    CHECK(books::TakeOwed(&owed) == 0); // LateInit after the EXIT: nothing to settle a second time
    CHECK(p.Finalize());
    CHECK(p.Commit() == 0 && p.Limit() == kLimit); // the next game: the budget whole, the limit of boot
    CHECK(books::Drift(p.Limit(), static_cast<u32>(kLimit)) == 0);
    // the other way round: LateInit first, then the EXIT finds nothing
    Process q(kLimit, 3 * 1024 * 1024);
    u32 owed_q = kPage;
    CHECK(TakePageAndRaise(q));
    books::Settle(q.rl.limit, static_cast<s32>(books::TakeOwed(&owed_q)));
    CHECK(books::TakeOwed(&owed_q) == 0);
    CHECK(q.Limit() == kLimit && q.Finalize() && q.Commit() == 0);
    // nothing owed (a New 3DS in memtype 6, or no Raise): the EXIT does nothing
    u32 none = 0;
    CHECK(books::TakeOwed(&none) == 0);
}

// An Old 3DS, an end with no EXIT event after a HOME (a process killed, a block that does not come back; a game closed from the HOME menu
// gets its EXIT once Luma3DS has mapped the plugin's block back, and that Settle is the EXIT's), and LateInit may not have come
// (the first seconds). SWAP_ENTER settles what is owed - but only once the game has its heap: after it, the limit is the one of boot at the
// end; before it, nothing is taken (a lower limit would take a page off the room the game sizes its heap with) and the bytes stay owed:
// a later SWAP_ENTER after the heap takes them, or, the game closed there, the next launch's drift does.
void TestOldClosedFromHome() {
    constexpr u32 kLimit = 64 * 1024 * 1024; // an Old 3DS in its 64 MB mode
    // HOME after the game sized its heap, closed from the HOME menu: settled at SWAP_ENTER, the end balances with the limit of boot
    {
        Process p(static_cast<s32>(kLimit), 2 * 1024 * 1024);
        u32 owed = 0;
        CHECK(TakePageAndRaise(p));
        owed = kPage;
        CHECK(p.Allocate(static_cast<s32>(p.Room()))); // its heap, to the byte
        const u32 at_swap = books::TakeOwedOnceHeapSeen(&owed, true);
        CHECK(at_swap == static_cast<u32>(kPage));
        books::Settle(p.rl.limit, static_cast<s32>(at_swap));
        CHECK(p.Limit() == static_cast<s32>(kLimit));
        CHECK(books::TakeOwedOnceHeapSeen(&owed, true) == 0); // the next HOME: nothing
        CHECK(books::TakeOwed(&owed) == 0);                   // LateInit or the EXIT, if they come: nothing
        CHECK(p.Finalize());                                  // an end with no EXIT
        CHECK(p.Commit() == 0 && p.Limit() == static_cast<s32>(kLimit));
        CHECK(books::Drift(p.Limit(), kLimit) == 0); // nothing left for the next launch
    }
    // HOME before the game's heap: nothing taken, the game keeps the room it sizes its heap with; closed there, the limit stays raised by
    // the page, and the next launch takes it back (Drift)
    {
        Process p(static_cast<s32>(kLimit), 2 * 1024 * 1024);
        u32 owed = 0;
        CHECK(TakePageAndRaise(p));
        owed = kPage;
        const s64 room = p.Room();
        CHECK(books::TakeOwedOnceHeapSeen(&owed, false) == 0);
        CHECK(owed == static_cast<u32>(kPage)); // still owed
        CHECK(p.Room() == room && p.Limit() == static_cast<s32>(kLimit) + kPage);
        CHECK(p.Finalize());
        CHECK(p.Limit() == static_cast<s32>(kLimit) + kPage);
        CHECK(books::Drift(p.Limit(), kLimit) == kPage); // SettleDrift's at the next launch
    }
    // HOME before the heap and back to the game, which sizes it; the next HOME settles, and the game closed then balances
    {
        Process p(static_cast<s32>(kLimit), 2 * 1024 * 1024);
        u32 owed = 0;
        CHECK(TakePageAndRaise(p));
        owed = kPage;
        CHECK(books::TakeOwedOnceHeapSeen(&owed, false) == 0);
        CHECK(p.Allocate(static_cast<s32>(p.Room()))); // the heap it sized after the first HOME: the whole room it saw
        books::Settle(p.rl.limit, static_cast<s32>(books::TakeOwedOnceHeapSeen(&owed, true)));
        CHECK(owed == 0 && p.Limit() == static_cast<s32>(kLimit));
        CHECK(p.Finalize() && p.Commit() == 0 && p.Limit() == static_cast<s32>(kLimit));
    }
}

// The KResourceLimit of the applications is ONE object since boot: the KProcess of the game before may still be finalized (on the other
// core) while the next one raises its books in EarlyInit. Its Release comes between any two of the Raise's calls: current[] is only ever
// changed by the kernel under its own mutex, so nothing is lost, in any order (the old Raise wrote current[] itself, ctr/kernel_books.cpp).
void TestRaiseWhileTheGameBeforeEnds() {
    constexpr s32 kLimit = 64 * 1024 * 1024;
    constexpr s32 kOldGame = 40 * 1024 * 1024;
    const char* const steps[] = {"lift", "reserve", "free", "never"};
    for (const char* when : steps) {
        Process p(kLimit, kOldGame); // the game before, its KProcess not finalized yet: its blocks still on the books, its pages still taken
        FakeKernel k(p);
        bool ended = false;
        const auto end_old_game = [&]() { // its KProcess::Finalize: KResourceLimit::Release of ITS private blocks, its pages back
            ended = true;
            p.Release(kOldGame);
            p.private_blocks -= kOldGame;
            p.region_free += kOldGame;
        };
        k.between = [&](const char* step) {
            if (!ended && std::string_view(step) == when) {
                end_old_game();
            }
        };
        CHECK(books::RaiseThroughKernel(k, kPage) == 0);
        if (!ended) {
            end_old_game();
        }
        CHECK(p.private_blocks == 0 && p.region_free == kLimit && k.blocks.empty());
        CHECK(p.Commit() == kPage && p.Room() == kLimit); // only the page on current: the new game sees the whole budget
        CHECK(p.Limit() == kLimit + kPage);
    }
}

// A Raise the kernel refuses: the limit goes back, nothing is owed, the page goes again with the svc that took it (the books untouched),
// the end balances; the result says why. A page kept without its Raise would lose the whole budget at the end.
void TestRaiseRefused() {
    // the region with no page for the reservation (the kernel reserves, finds no page, releases: 13882-13884)
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
        const s64 room = p.Room();
        p.region_free = kPage - 0x1000;
        FakeKernel k(p);
        s32 rc = 0;
        CHECK(!TakePageAndRaise(p, k, &rc));
        CHECK(rc == kNoPages);
        CHECK(k.lifts == 1 && k.lowers == 1);
        CHECK(p.Limit() == 80 * 1024 * 1024 && p.Commit() == 3 * 1024 * 1024 && p.Room() == room);
        p.region_free = static_cast<s32>(room);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        CHECK(p.Finalize() && p.Commit() == 0 && p.Limit() == 80 * 1024 * 1024);
    }
    // the lifted room taken by somebody else before the reservation: refused above the limit
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 4 * kPage);
        FakeKernel k(p);
        k.between = [&](const char* step) {
            if (std::string_view(step) == "reserve") {
                CHECK(p.Allocate(static_cast<s32>(p.Room())));
            }
        };
        s32 rc = 0;
        CHECK(!TakePageAndRaise(p, k, &rc));
        CHECK(rc == kRefused);
        CHECK(p.Limit() == 80 * 1024 * 1024);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // svc 0xA3 cannot free the block of the reservation: svcControlMemory frees it, and the reservation goes with it
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
        const s64 room = p.Room();
        FakeKernel k(p);
        k.fail_free_unsafe = 1;
        s32 rc = 0;
        CHECK(!TakePageAndRaise(p, k, &rc));
        CHECK(rc == kBadFree && k.releases == 1 && k.blocks.empty());
        CHECK(p.Limit() == 80 * 1024 * 1024 && p.Room() == room);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // ... and when neither frees it, the block stays, on the books like any of the game's: the end balances
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
        FakeKernel k(p);
        k.fail_free_unsafe = 1;
        k.charged_frees_ok = 0;
        CHECK(!TakePageAndRaise(p, k));
        CHECK(k.blocks.size() == 1 && p.Limit() == 80 * 1024 * 1024 && p.Room() == 80 * 1024 * 1024 - 3 * 1024 * 1024 - kPage);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // the page kept without its Raise: the end gives back nothing
    Process q(80 * 1024 * 1024, 3 * 1024 * 1024);
    q.AllocateOutside(kPage);
    CHECK(q.Allocate(static_cast<s32>(q.Room())));
    CHECK(!q.Finalize());
}

// The reference of the drift: the limit of boot. A New 3DS in memtype 7 (178 MB): Rosalina took the plugin's block (5 MiB) off APPMEMALLOC
// at the first launch with a plugin, and two launches died before Settle with no EXIT (8 KB each). Measured against APPMEMALLOC the drift is
// 5 MiB + 16 KB, above kMaxDrift: never taken back (the defect). Against the region's size it is the 16 KB.
void TestDriftReference() {
    constexpr u32 kRegion = 0xB200000; // a New 3DS in memtype 7 (Kernel11: applicationSize, and APPMEMALLOC at boot)
    constexpr u32 kBlock = 5 * 1024 * 1024;
    const u32 appmemalloc = kRegion - kBlock;
    const s32 limit = static_cast<s32>(kRegion) + 2 * kPage;
    CHECK(books::DriftReference(true, kRegion, appmemalloc) == kRegion);
    CHECK(books::Drift(limit, books::DriftReference(true, kRegion, appmemalloc)) == 2 * kPage);
    CHECK(books::Drift(limit, appmemalloc) == 0); // (what APPMEMALLOC alone gave)
    CHECK(books::Drift(limit, books::DriftReference(true, kRegion, kRegion - 2 * 1024 * 1024)) == 2 * kPage); // the 2 MiB block
    CHECK(books::Drift(static_cast<s32>(kRegion), books::DriftReference(true, kRegion, appmemalloc)) == 0);    // no drift
    // Luma3DS did not answer: APPMEMALLOC, as before
    CHECK(books::DriftReference(true, 0, appmemalloc) == appmemalloc);
    // an Old 3DS: APPMEMALLOC (the plugin's block is in the SYSTEM region, APPMEMALLOC untouched), whatever the region says
    constexpr u32 kOld = 64 * 1024 * 1024;
    CHECK(books::DriftReference(false, kOld, kOld) == kOld);
    CHECK(books::DriftReference(false, kOld + 0x1000, kOld) == kOld);
    CHECK(books::Drift(static_cast<s32>(kOld) + kPage, books::DriftReference(false, kOld, kOld)) == kPage);
    // the New 3DS in memtype 7 played through: the next launch takes the drift back, its own Raise and Settle balance
    Process p(limit, 0);
    p.region_free = static_cast<s32>(kRegion);
    const s32 drift = books::Drift(p.Limit(), books::DriftReference(true, kRegion, appmemalloc));
    books::Settle(p.rl.limit, drift);
    CHECK(p.Limit() == static_cast<s32>(kRegion));
    CHECK(TakePageAndRaise(p));
    CHECK(p.Room() == static_cast<s64>(kRegion)); // the room of the region, no more
    books::Settle(p.rl.limit, kPage);
    CHECK(p.Finalize() && p.Limit() == static_cast<s32>(kRegion));
}

// The drift on an Old 3DS: one KResourceLimit for every application since boot, its limit set once to APPMEMALLOC. Three launches die between
// Raise and Settle (pages of 1, 2 and 4 pages): the limit has drifted by their sum. The next launch takes the drift back before it raises the
// books: the game sees the room of the region, sizes its heap to the byte, and its own end balances with the limit as pm set it.
void TestDriftOfLaunchesThatDied() {
    constexpr u32 kAppMemAlloc = 64 * 1024 * 1024; // an Old 3DS in its 64 MB mode
    Process p(static_cast<s32>(kAppMemAlloc), 0);
    const s32 pages[] = {0x1000, 0x2000, 0x4000};
    for (const s32 page : pages) {
        CHECK(books::Drift(p.Limit(), kAppMemAlloc) == p.Limit() - static_cast<s32>(kAppMemAlloc));
        CHECK(p.Allocate(2 * 1024 * 1024)); // the game's code and stack
        p.AllocateOutside(page);
        FakeKernel k(p);
        CHECK(books::RaiseThroughKernel(k, static_cast<u32>(page)) == 0);
        CHECK(p.Finalize()); // closed from HOME before the plugin's main()
        p.region_free = static_cast<s32>(kAppMemAlloc);
        p.system_free = 16 * 1024 * 1024;
    }
    const s32 drift = 0x1000 + 0x2000 + 0x4000;
    CHECK(p.Limit() == static_cast<s32>(kAppMemAlloc) + drift);
    CHECK(p.Commit() == 0);
    CHECK(p.Room() == static_cast<s64>(kAppMemAlloc) + drift); // without the fix: room the region does not have
    // the next launch: the drift first, then the page as always
    CHECK(books::Drift(p.Limit(), kAppMemAlloc) == drift);
    books::Settle(p.rl.limit, books::Drift(p.Limit(), kAppMemAlloc));
    CHECK(p.Limit() == static_cast<s32>(kAppMemAlloc));
    CHECK(books::Drift(p.Limit(), kAppMemAlloc) == 0);
    CHECK(TakePageAndRaise(p));
    CHECK(p.Room() == static_cast<s64>(kAppMemAlloc)); // the room of the region, no more
    CHECK(p.Allocate(static_cast<s32>(p.Room())));
    books::Settle(p.rl.limit, kPage);
    CHECK(p.Finalize() && p.Commit() == 0 && p.Limit() == static_cast<s32>(kAppMemAlloc));
}

// What is not a drift of the gate's page is left alone: a limit at or below APPMEMALLOC, a part of a page, more than kMaxDrift (Luma3DS's
// MODE3 takes the plugin's block, 2 MiB at the least, off APPMEMALLOC and not off the limit).
void TestDriftBounds() {
    constexpr u32 kAppMemAlloc = 64 * 1024 * 1024;
    const s32 base = static_cast<s32>(kAppMemAlloc);
    CHECK(books::Drift(base, kAppMemAlloc) == 0);
    CHECK(books::Drift(base - 0x1000, kAppMemAlloc) == 0);
    CHECK(books::Drift(base + 0x1000, kAppMemAlloc) == 0x1000);
    CHECK(books::Drift(base + 0x800, kAppMemAlloc) == 0);
    CHECK(books::Drift(base + books::kMaxDrift, kAppMemAlloc) == books::kMaxDrift);
    CHECK(books::Drift(base + books::kMaxDrift + 0x1000, kAppMemAlloc) == 0);
    CHECK(books::Drift(base, kAppMemAlloc - 2 * 1024 * 1024) == 0); // MODE3 with the smallest block of the plugin
    CHECK(books::kMaxDrift < 2 * 1024 * 1024 && books::kMaxDrift >= 64 * 4 * 0x1000); // (64 launches of the biggest page, under MODE3's 2 MiB)
    CHECK(books::Drift(0x7FFFF000, 0) == 0);                        // (no overflow)
}

// Why: the page outside the budget and not on the books loses the whole budget at the end.
void TestWithoutTheBooks() {
    Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
    p.AllocateOutside(kPage);
    CHECK(p.Allocate(static_cast<s32>(p.Room())));
    CHECK(!p.Finalize()); // the kernel gives back nothing
}

constexpr s32 kBlock = 128 * 1024; // the socket block of a late title

// The socket block of a late title (ctr/soc.cpp): taken after the heap, the game's budget spent; charged: current += X with the limit lifted
// around the reservation only, the end balances. A New 3DS running an Old 3DS title: the region has pages beyond the limit pm set.
void TestCharge() {
    Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
    CHECK(p.Allocate(static_cast<s32>(p.Room()))); // the game took everything
    CHECK(p.Room() == 0);
    FakeKernel k(p);
    CHECK(books::ChargeThroughKernel(k, kBlock) == 0);
    CHECK(k.lifts == 1 && k.lowers == 1 && k.reserves == 1 && k.releases == 0 && k.blocks.empty());
    CHECK(p.Limit() == 80 * 1024 * 1024);                 // the limit as it was
    CHECK(p.Commit() == 80 * 1024 * 1024 + kBlock);       // the block on current
    CHECK(p.region_free == 40 * 1024 * 1024);             // the pages of the reservation back in the region
    CHECK(!p.Allocate(1));                                // the game: still nothing more
    p.AllocateOutside(kBlock);                            // svc 0xA3, the SYSTEM region
    CHECK(p.Finalize() && p.Commit() == 0);
}

// An Old 3DS whose game holds the whole region (the limit is the region's size, Monster Hunter XX: 80 MiB of 80): svcControlMemory finds no
// page of the region for the reservation, the kernel releases it again (kNoPages) and the kernel's way alone leaves the books as they were,
// with no socket block and no network (the regression of the first AV1 fix). ChargeOrWrite then writes current += X in kernel mode, as
// 1.0.13-1.1.1 did: after the kernel's try, and only for that answer. The block comes from the SYSTEM region and the end balances; a block
// refused after the charge is taken back, written too when no region has a page (an Old 3DS: the game's region full), and the end balances.
void TestChargeOnAFullRegion() {
    constexpr s32 kLimit = 80 * 1024 * 1024;
    // the kernel's way alone: refused for want of pages, the limit back, the books as they were
    {
        Process p(kLimit, 3 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        CHECK(p.region_free == 0);
        FakeKernel k(p);
        CHECK(books::ChargeThroughKernel(k, kBlock) == kNoPages);
        CHECK(p.Limit() == kLimit && p.Commit() == kLimit && k.lifts == 1 && k.lowers == 1);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // the fallback: the kernel first, then current written; the socket block from the SYSTEM region; the end balances
    {
        Process p(kLimit, 3 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        books::Way way = books::Way::Kernel;
        CHECK(books::ChargeOrWrite(k, kBlock, &way) == 0);
        CHECK(way == books::Way::Written);
        CHECK(k.writes == 1 && k.written == kBlock && k.calls_before_write == 1); // the kernel's reservation tried first, once
        CHECK(k.lifts == 1 && k.lowers == 1 && k.blocks.empty());
        CHECK(p.Limit() == kLimit && p.Commit() == kLimit + kBlock && p.region_free == 0);
        CHECK(!p.Allocate(1));     // the game: still nothing more
        p.AllocateOutside(kBlock); // svc 0xA3, the SYSTEM region: the socket block
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // the fallback, then the socket block refused (the SYSTEM region full too): the Uncharge finds no page in any region and writes it off
    {
        Process p(kLimit, 3 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        p.system_free = 0;
        p.base_free = 0;
        FakeKernel k(p);
        books::Way way = books::Way::Kernel;
        CHECK(books::ChargeOrWrite(k, kBlock, &way) == 0 && way == books::Way::Written);
        u32 released = 0;
        CHECK(books::UnchargeOrWrite(k, kBlock, &released, &way) == 0);
        CHECK(way == books::Way::Written && released == 0 && k.writes == 2 && k.written == 0);
        CHECK(p.Commit() == kLimit && p.Limit() == kLimit && k.blocks.empty());
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // without the fallback (what the kernel's way alone did): no charge, so no block outside the budget; one taken all the same would lose
    // the budget at the end
    {
        Process p(kLimit, 3 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        p.AllocateOutside(kBlock);
        CHECK(!p.Finalize());
    }
}

// The fallback is for want of pages only, and comes after the kernel's way: with pages the kernel charges and nothing is written; a
// reservation refused above the limit (a game thread took the lifted room first) or a FREE that failed stands, nothing written.
void TestChargeOrWriteKernelFirst() {
    constexpr s32 kLimit = 80 * 1024 * 1024;
    // a New 3DS running an Old 3DS title: the region has pages beyond the limit, the kernel charges
    {
        Process p(kLimit, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        books::Way way = books::Way::Written;
        CHECK(books::ChargeOrWrite(k, kBlock, &way) == 0);
        CHECK(way == books::Way::Kernel && k.writes == 0 && k.reserves == 1);
        CHECK(p.Commit() == kLimit + kBlock && p.Limit() == kLimit);
        p.AllocateOutside(kBlock);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // refused above the limit: nothing written
    {
        Process p(kLimit, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        k.between = [&](const char* step) {
            if (std::string_view(step) == "reserve") {
                CHECK(p.Allocate(kBlock));
            }
        };
        books::Way way = books::Way::Written;
        CHECK(books::ChargeOrWrite(k, kBlock, &way) == kRefused);
        CHECK(way == books::Way::Kernel && k.writes == 0);
        CHECK(p.Commit() == kLimit + kBlock && p.Limit() == kLimit); // (the game's block, not ours)
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // svc 0xA3 cannot free the block of the reservation: svcControlMemory frees it, the answer stands, nothing written
    {
        Process p(kLimit, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        k.fail_free_unsafe = 1;
        books::Way way = books::Way::Written;
        CHECK(books::ChargeOrWrite(k, kBlock, &way) == kBadFree);
        CHECK(way == books::Way::Kernel && k.writes == 0 && p.Commit() == kLimit && p.Limit() == kLimit);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // the Uncharge with pages: the kernel releases, nothing written; with no place in the heap area: the answer stands, nothing written
    {
        Process p(kLimit, kLimit + kBlock);
        FakeKernel k(p);
        u32 released = 0;
        books::Way way = books::Way::Written;
        CHECK(books::UnchargeOrWrite(k, kBlock, &released, &way) == 0);
        CHECK(way == books::Way::Kernel && k.writes == 0 && released == static_cast<u32>(kBlock) && p.Commit() == kLimit);
        Process q(kLimit, kLimit + kBlock);
        FakeKernel kq(q);
        kq.span = false;
        CHECK(books::UnchargeOrWrite(kq, kBlock, &released, &way) == books::kNoFreeSpan);
        CHECK(way == books::Way::Kernel && kq.writes == 0 && released == 0 && q.Commit() == kLimit + kBlock);
    }
}

// The whole launch of a late title (ctr/soc.cpp, gate_page.cpp): the gate's page raised before the game sizes its heap, the game's heap,
// the Settle of the page, the Charge of the socket block; the Settle before the Charge and after it. Two ways a game sizes its heap:
//   * Monster Hunter XX (analysis/mhxx-crash-2026-11-01/NOTA.md): heap = APPMEMALLOC - commit - 59 MiB, the 59 MiB taken too, so it ends
//     with current == APPMEMALLOC whatever the commit it started from; the limit of boot is APPMEMALLOC, so after the Settle current is at
//     the limit, and before it under it by the page;
//   * libctru's way (TestRaiseThenSettle): the whole of limit - current, so after the Settle current stands above the limit by the page.
// Either way the reservation of the Charge must pass the limit (the lift of ChargeLift) and meet the pages: on an Old 3DS (the region full)
// kNoPages and the fallback written; with pages beyond the limit (a New 3DS running an Old 3DS title) the kernel's way. The end balances
// and the limit comes back to the one of boot.
void TestChargeAfterTheGatePage() {
    constexpr s32 kAppMemAlloc = 80 * 1024 * 1024; // the limit of boot (an Old 3DS in its 80 MiB layout)
    constexpr s32 kStart = 13264 * 1024;           // the commit at the game's first instruction (a user's log, NOTA.md)
    constexpr s32 kFixed = 59 * 1024 * 1024;       // what Monster Hunter XX takes besides its heap
    // the lift: the block, and what current stands above the limit; never less than the block
    CHECK(books::ChargeLift(kAppMemAlloc, kAppMemAlloc, kBlock) == static_cast<u32>(kBlock));
    CHECK(books::ChargeLift(kAppMemAlloc, kAppMemAlloc + kPage, kBlock) == static_cast<u32>(kBlock + kPage));
    CHECK(books::ChargeLift(kAppMemAlloc + kPage, kAppMemAlloc, kBlock) == static_cast<u32>(kBlock));
    CHECK(books::ChargeLift(0, 0x7FFE0000, 0x10000) == 0x7FFF0000u); // (current + bytes: no overflow)
    for (int mhxx = 0; mhxx < 2; ++mhxx) {
        for (int pages_beyond = 0; pages_beyond < 2; ++pages_beyond) {
            for (int settle_first = 0; settle_first < 2; ++settle_first) {
                Process p(kAppMemAlloc, kStart, pages_beyond ? 40 * 1024 * 1024 : 0);
                FakeKernel k(p);
                CHECK(TakePageAndRaise(p, k)); // EarlyInit: limit and current up by the page
                const s32 heap = mhxx ? kAppMemAlloc - p.Commit() - kFixed : static_cast<s32>(p.Room()) - kFixed;
                CHECK(heap > 0);
                CHECK(p.Allocate(kFixed) && p.Allocate(heap)); // the game sizes and takes its memory
                CHECK(p.Commit() == (mhxx ? kAppMemAlloc : kAppMemAlloc + kPage));
                CHECK(p.region_free == (pages_beyond ? 40 * 1024 * 1024 : 0) + (mhxx ? kPage : 0)); // (the page came from the SYSTEM region)
                if (settle_first) {
                    books::Settle(p.rl.limit, kPage); // LateInit
                    CHECK(p.Room() == (mhxx ? 0 : -kPage));
                }
                k.lifts = k.lowers = k.reserves = k.reads = 0;
                k.lifted = k.lowered = 0;
                books::Way way = books::Way::Written;
                CHECK(books::ChargeOrWrite(k, kBlock, &way) == 0); // the first connection: never refused for the books alone
                CHECK(k.reads == 1 && k.lifts == 1 && k.lowers == 1 && k.lifted == k.lowered && k.reserves == 1);
                CHECK(k.lifted == kBlock + (settle_first && !mhxx ? kPage : 0));
                CHECK(way == (pages_beyond ? books::Way::Kernel : books::Way::Written));
                CHECK(k.writes == (pages_beyond ? 0 : 1) && k.blocks.empty());
                if (!settle_first) {
                    books::Settle(p.rl.limit, kPage); // LateInit after the connection
                }
                CHECK(p.Limit() == kAppMemAlloc);                                       // the limit of boot
                CHECK(p.Commit() == (mhxx ? kAppMemAlloc : kAppMemAlloc + kPage) + kBlock); // MHXX: limit + 128 KB
                CHECK(!p.Allocate(1));
                p.AllocateOutside(kBlock); // the socket block, from the SYSTEM region
                CHECK(p.Finalize() && p.Commit() == 0);
            }
        }
    }
    // the lift of the block alone (what ChargeThroughKernel did before): a libctru game after the Settle is refused above the limit, never
    // reaches the pages, and the fallback never comes
    {
        Process p(kAppMemAlloc, kStart);
        FakeKernel k(p);
        CHECK(TakePageAndRaise(p, k));
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        books::Settle(p.rl.limit, kPage);
        k.Lift(kBlock);
        CHECK(books::ReserveThroughKernel(k, kBlock) == kRefused);
        k.Lower(kBlock);
        CHECK(p.Limit() == kAppMemAlloc && p.Commit() == kAppMemAlloc + kPage);
    }
}

// A game thread on the other core takes the lifted room before the reservation (a game that allocates at its limit right then): its block
// is had, the reservation is refused; the limit comes back, the game's block is on the books like any other, the end balances. After the
// reservation the lifted room is ours: a game thread asking then is refused as it would have been.
void TestChargeRacedByTheGame() {
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        bool got = false;
        k.between = [&](const char* step) {
            if (std::string_view(step) == "reserve") {
                got = p.Allocate(kBlock);
            }
        };
        CHECK(books::ChargeThroughKernel(k, kBlock) == kRefused);
        CHECK(got);
        CHECK(p.Limit() == 80 * 1024 * 1024 && p.Commit() == 80 * 1024 * 1024 + kBlock);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        bool got = true;
        k.between = [&](const char* step) {
            if (std::string_view(step) == "free" || std::string_view(step) == "lower") {
                got = got && p.Allocate(0x1000);
            }
        };
        CHECK(books::ChargeThroughKernel(k, kBlock) == 0);
        CHECK(!got);
        CHECK(p.Limit() == 80 * 1024 * 1024 && p.Commit() == 80 * 1024 * 1024 + kBlock);
        p.AllocateOutside(kBlock);
        CHECK(p.Finalize() && p.Commit() == 0);
    }
}

// The order of the late socket block (ctr/soc.cpp, TryAt): the charge BEFORE the block, taken back when the block is not had.
void TestChargeBeforeTheBlock() {
    // the block refused: charged, then the charge taken back (Uncharge): the books as they were, the end balances
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        const s32 before = p.Commit();
        FakeKernel k(p);
        CHECK(books::ChargeThroughKernel(k, kBlock) == 0);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0 && released == static_cast<u32>(kBlock));
        CHECK(p.Commit() == before && p.Limit() == 80 * 1024 * 1024 && k.blocks.empty());
        CHECK(p.Finalize() && p.Commit() == 0);
    }
    // the process dies between the charge and the block: X too many on current; the end still gives the game's budget back, X is what stays
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        CHECK(books::ChargeThroughKernel(k, kBlock) == 0);
        CHECK(p.Finalize());
        CHECK(p.Commit() == kBlock);
    }
    // the old order: the block, then the process dies before the charge: the sum is above current and NOTHING comes back
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        p.AllocateOutside(kBlock);
        CHECK(!p.Finalize());
    }
    // a block that came at the wrong address is freed: with svcControlMemoryUnsafe and the charge taken back, the end balances; with
    // svcControlMemory (which releases the commit of a block the kernel never charged) and the charge taken back, current is X short
    {
        Process p(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(p.Allocate(static_cast<s32>(p.Room())));
        FakeKernel k(p);
        CHECK(books::ChargeThroughKernel(k, kBlock) == 0);
        p.AllocateOutside(kBlock);
        p.FreeOutside(kBlock);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);
        CHECK(p.Finalize() && p.Commit() == 0);
        Process q(80 * 1024 * 1024, 3 * 1024 * 1024, 40 * 1024 * 1024);
        CHECK(q.Allocate(static_cast<s32>(q.Room())));
        FakeKernel kq(q);
        CHECK(books::ChargeThroughKernel(kq, kBlock) == 0);
        q.AllocateOutside(kBlock);
        q.FreeOutsideAsCharged(kBlock);
        CHECK(books::ReleaseThroughKernel(kq, kBlock, &released) == 0);
        CHECK(!q.Finalize());
    }
}

// Uncharge (current -= X): svc 0xA3's block freed by svcControlMemory, whose FREE releases it. Whole; a page at a time when the SYSTEM region
// is short of the whole block (its own allocation just failed there); and what stays charged when it cannot be made.
void TestUncharge() {
    constexpr s32 kBefore = 80 * 1024 * 1024 + kBlock;
    // whole
    {
        Process p(80 * 1024 * 1024, kBefore);
        FakeKernel k(p);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);
        CHECK(released == static_cast<u32>(kBlock) && k.releases == 1 && p.Commit() == 80 * 1024 * 1024 && k.blocks.empty());
        CHECK(p.Limit() == 80 * 1024 * 1024 && p.system_free == 16 * 1024 * 1024);
    }
    // the SYSTEM region with a page free: a page at a time
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0x1000;
        FakeKernel k(p);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);
        CHECK(released == static_cast<u32>(kBlock) && k.releases == kBlock / 0x1000 && p.Commit() == 80 * 1024 * 1024);
        CHECK(p.system_free == 0x1000 && k.blocks.empty());
    }
    // no page at all in the SYSTEM region, pages in APPLICATION (a Charge made through the kernel has just found them there): a page at a
    // time from there, each back at once
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0;
        p.region_free = 0x1000;
        FakeKernel k(p);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);
        CHECK(released == static_cast<u32>(kBlock) && k.releases == kBlock / 0x1000 && p.Commit() == 80 * 1024 * 1024);
        CHECK(k.last_region == books::Region::Application && p.region_free == 0x1000 && p.system_free == 0 && k.blocks.empty());
    }
    // ... nor in APPLICATION: BASE
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0;
        p.region_free = 0;
        p.base_free = 0x1000;
        FakeKernel k(p);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);
        CHECK(released == static_cast<u32>(kBlock) && p.Commit() == 80 * 1024 * 1024);
        CHECK(k.last_region == books::Region::Base && p.base_free == 0x1000 && k.blocks.empty());
    }
    // no page in any region: nothing released, the result says why; UnchargeOrWrite then writes the charge off current
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0;
        p.region_free = 0;
        p.base_free = 0;
        FakeKernel k(p);
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == kNoPages);
        CHECK(released == 0 && p.Commit() == kBefore && k.writes == 0);
        books::Way way = books::Way::Kernel;
        CHECK(books::UnchargeOrWrite(k, kBlock, &released, &way) == 0);
        CHECK(way == books::Way::Written && released == 0 && k.writes == 1 && k.written == -kBlock);
        CHECK(k.calls_before_write > 3); // (the kernel's ALLOCs in the three regions first)
        CHECK(p.Commit() == 80 * 1024 * 1024 && k.blocks.empty());
    }
    // the last SYSTEM page taken by another process half-way, no other region with a page: what the kernel released is said, the rest written
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0x1000;
        p.region_free = 0;
        p.base_free = 0;
        FakeKernel k(p);
        int allocs = 0;
        k.between = [&](const char* step) {
            if (std::string_view(step) == "alloc_uncharged" && ++allocs == 4) { // (the whole block, two pages, then the third)
                p.system_free = 0;
            }
        };
        u32 released = 0;
        books::Way way = books::Way::Kernel;
        CHECK(books::UnchargeOrWrite(k, kBlock, &released, &way) == 0);
        CHECK(way == books::Way::Written && released == 2 * 0x1000 && k.written == -(kBlock - 2 * 0x1000));
        CHECK(p.Commit() == 80 * 1024 * 1024 && k.blocks.empty());
    }
    // no place in the heap area
    {
        Process p(80 * 1024 * 1024, kBefore);
        FakeKernel k(p);
        k.span = false;
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == books::kNoFreeSpan);
        CHECK(released == 0 && p.Commit() == kBefore);
    }
    // svcControlMemory's FREE fails half-way (a page at a time): what came off current is said, the block of the failed step is gone
    {
        Process p(80 * 1024 * 1024, kBefore);
        p.system_free = 0x1000;
        FakeKernel k(p);
        k.charged_frees_ok = 3;
        u32 released = 0;
        CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == kBadFree);
        CHECK(released == 3 * 0x1000 && p.Commit() == kBefore - 3 * 0x1000);
        CHECK(k.blocks.empty() && p.system_free == 0x1000);
    }
}

// Who owes the Settle: not a New 3DS in memtype 6 (pm puts the limit back at the end of every application); a New 3DS in memtype 7 and an
// Old 3DS, whatever its memtype, do (pm never writes the limit again there).
void TestSettleNeeded() {
    CHECK(!books::SettleNeeded(true, 6));
    CHECK(books::SettleNeeded(true, 7));
    CHECK(books::SettleNeeded(true, 0));
    for (u32 memtype = 0; memtype <= 7; ++memtype) {
        CHECK(books::SettleNeeded(false, memtype));
    }
}

// A New 3DS in memtype 6: Raise only. The game sizes its heap to the byte, frees a part and takes it back later (the room it saw stays
// whole for the whole game: a Settle would have refused that), the end balances, and pm's reset at the end of the application puts the
// limit back as the system gave it.
void TestNewMemtype6RaiseOnly() {
    constexpr s32 kDefault = 124 * 1024 * 1024; // (the limit pm gives the applications of a New 3DS in memtype 6)
    Process p(kDefault, 5 * 1024 * 1024);
    const s64 room = p.Room();
    CHECK(TakePageAndRaise(p));
    CHECK(!books::SettleNeeded(true, 6));
    CHECK(p.Room() == room);
    CHECK(p.Allocate(static_cast<s32>(room))); // its heap, to the byte
    p.Free(1024 * 1024);                       // a FREE of 1 MiB ...
    CHECK(p.Allocate(1024 * 1024));            // ... and the same MiB again, later in the game
    CHECK(p.Room() == 0);
    CHECK(p.Finalize() && p.Commit() == 0);
    CHECK(p.Limit() == kDefault + kPage); // (raised by the page until pm's reset)
    p.rl.limit[books::kCommit] = kDefault; // resetAppMemLimit at the end of the application
    // the next launch: no SettleDrift on a New 3DS in memtype 6 (SettleNeeded); and APPMEMALLOC there is the default minus the plugin's
    // block (Rosalina at the first launch with a plugin): measured against it, the limit pm put back would be no drift either
    constexpr u32 kAppMemAlloc = static_cast<u32>(kDefault) - 5 * 1024 * 1024;
    CHECK(books::Drift(p.Limit(), books::DriftReference(true, static_cast<u32>(kDefault), kAppMemAlloc)) == 0);
    CHECK(books::Drift(p.Limit(), kAppMemAlloc) == 0);
    // the same game with a Settle: the MiB taken back later is refused (the room it saw is a page short)
    Process q(kDefault, 5 * 1024 * 1024);
    CHECK(TakePageAndRaise(q));
    CHECK(q.Allocate(static_cast<s32>(q.Room())));
    books::Settle(q.rl.limit, kPage);
    q.Free(1024 * 1024);
    CHECK(!q.Allocate(1024 * 1024));
}

// The check of the offsets: the words read at the object must be what the kernel's calls say, all of them.
void TestSameAsTheCalls() {
    const s32 read[books::kChecked] = {80 << 20, 32, 64, 16};
    const s64 calls[books::kChecked] = {80 << 20, 32, 64, 16};
    CHECK(books::SameAsTheCalls(read, 3 << 20, calls, 3 << 20));
    CHECK(!books::SameAsTheCalls(read, 3 << 20, calls, 4 << 20));
    for (int i = 0; i < books::kChecked; ++i) {
        s64 other[books::kChecked] = {80 << 20, 32, 64, 16};
        other[i] += 1;
        CHECK(!books::SameAsTheCalls(read, 3 << 20, other, 3 << 20));
    }
    CHECK(books::kCheckedNames[0] == books::kCommit); // (ReadBooks takes the commit's limit from the first slot read)
}

} // namespace

int main() {
    TestRaiseThenSettle();
    TestRaiseThenSettleWithRoomLeft();
    TestDeathBeforeSettle();
    TestDeathBeforeSettleWithExit();
    TestOldClosedFromHome();
    TestRaiseWhileTheGameBeforeEnds();
    TestRaiseRefused();
    TestDriftOfLaunchesThatDied();
    TestDriftBounds();
    TestDriftReference();
    TestWithoutTheBooks();
    TestCharge();
    TestChargeOnAFullRegion();
    TestChargeOrWriteKernelFirst();
    TestChargeAfterTheGatePage();
    TestChargeRacedByTheGame();
    TestChargeBeforeTheBlock();
    TestUncharge();
    TestSettleNeeded();
    TestNewMemtype6RaiseOnly();
    TestSameAsTheCalls();
    std::printf("kernel_books_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
