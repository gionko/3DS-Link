// The memory 3DS Link asks for before main() (ConfigApp/source/heap_plan.hpp, the very file, plain C++), run by Tests/run.sh, against a made-up
// kernel: the books of the resource limit, the region's real pages, pages held outside the books (seen by the region's count or not, given back at
// a moment or never), a game whose process is still alive, the biggest free block of the region (the linear heap is one block), the time a try
// takes, the 96 MiB of the heap area, a process that does not live in the APPLICATION region. What it pins: the kernel's raw answers become the
// right numbers (FromRaw); the split is libctru's (the sources after 2.7.0, and 2.7.0 itself where they agree); clean books give what libctru gave
// minus the margin, at once; 8 KB outside the books do not make the app wait (libctru 2.7.0 stops on them); memory that is not the app's (a game
// still alive, Rosalina's block) makes it wait up to 3 s and then take what there is; a refused linear heap cuts the linear heap, not the heap; a
// refused request is not made again on the same counts before 1 s, and the 3 s are real time; then the total is cut to 3/4 down to the minimum
// and on to the floor; less than the minimum available is asked for as it is (wherever libctru 2.7.0, the allocator of 3DS Link 1.1.1, starts,
// down to the floor); the New (124 MiB), the Old (64 MiB) and memory mode 7 (178 MiB); the log line, its stamp and the log that keeps the last
// starts; the numbers left for the crash dump.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "heap_plan.hpp"

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

using heap_plan::Facts;
using heap_plan::Part;
using heap_plan::Raw;
using heap_plan::Record;
using heap_plan::Split;

constexpr uint32_t KiB = 1024;
constexpr uint32_t MiB = 1024 * 1024;
constexpr uint32_t kNever = 0xFFFFFFFFu;
constexpr uint32_t kBase = 5276 * KiB;                                // 3DS Link's own process (0x527000 in the dumps)
constexpr uint32_t kFull = 124 * MiB - kBase - heap_plan::kMargin;    // what clean books on a New give: 121444 KiB

// libctru's split as its sources have it after 2.7.0 (allocateHeaps.c), transcribed: the reference of SplitTotal.
static Split LibctruSplit(uint32_t remaining, uint32_t heap_size, uint32_t linear_size) {
    Split s{heap_size, linear_size, true};
    if (heap_size + linear_size > remaining) {
        s.ok = false;
        return s;
    }
    const bool auto_app_sz = heap_size == 0;
    const bool auto_linear_sz = linear_size == 0;
    if (auto_app_sz && auto_linear_sz) {
        s.linear = (remaining / 2) & ~0xFFFu;
        s.heap = remaining - s.linear;
        if (s.heap > (24u << 20)) {
            s.heap = 24u << 20;
            s.linear = remaining - s.heap;
            if (s.linear > (32u << 20)) {
                s.linear = 32u << 20;
                s.heap = remaining - s.linear;
            }
        }
    } else if (auto_app_sz) {
        s.heap = remaining - s.linear;
    } else if (auto_linear_sz) {
        s.linear = remaining - s.heap;
    }
    if (s.heap > 0x06000000u) {
        if (auto_linear_sz) {
            s.linear += s.heap - 0x06000000u;
        }
        s.heap = 0x06000000u;
    }
    return s;
}

// The same as the disassembly of libctru 2.7.0 (no cap at the heap area).
static Split Libctru270Split(uint32_t remaining) {
    Split s{0, 0, true};
    s.linear = (remaining >> 1) & 0xFFFFF000u;
    s.heap = remaining - s.linear;
    if (s.heap > 0x1800000u) {
        s.heap = 0x1800000u;
        s.linear = remaining - 0x1800000u;
        if (s.linear > 0x2000000u) {
            s.linear = 0x2000000u;
            s.heap = remaining - 0x2000000u;
        }
    }
    return s;
}

// A made-up kernel. The app's own process holds `base` (its code, its stack: in the books and in the region). A game whose process is still alive
// holds `game` (in the books and in the region) until game_until_ms. `hidden` is held in the region outside the books until hidden_until_ms;
// hidden_seen says whether the region's count (svcGetSystemInfo 0, 1) shows it: on kernel 11.14 it always does (the count is the region's size
// minus its free pages), so hidden_seen = false stands for counts that lie. `contiguous` is the biggest free block of the region until
// contiguous_until_ms (kNever: the free memory is one block). A try costs try_cost_ms once the linear heap was found (the kernel zeroes the
// pages). The kernel refuses a linear heap past the books or the biggest block, then a heap past the books, the region's free pages or the heap
// area. memtype is the process's memory type; app_books = false: the books are not the APPLICATION region's (an applet), the region does not
// limit the allocations.
struct FakeKernel {
    int64_t limit = 124 * MiB;
    uint32_t region = 124 * MiB;
    bool region_known = true;
    bool luma = true;
    uint32_t base = kBase;
    uint32_t game = 0;
    uint32_t game_until_ms = kNever;
    uint32_t hidden = 0;
    bool hidden_seen = true;
    uint32_t hidden_until_ms = kNever;
    uint32_t contiguous = kNever;
    uint32_t contiguous_until_ms = kNever;
    uint32_t try_cost_ms = 0;
    uint32_t memtype = 0x100;
    bool app_books = true;

    uint32_t now_ms = 0;
    std::vector<uint32_t> tries;     // the totals asked for (heap + linear)
    std::vector<uint32_t> linears;   // the linear heap of each
    std::vector<uint32_t> try_times; // when
    uint32_t sleeps = 0;
    uint32_t zeroings = 0;           // tries that got as far as zeroing pages

    uint32_t Game() const { return now_ms < game_until_ms ? game : 0; }
    uint32_t Hidden() const { return now_ms < hidden_until_ms ? hidden : 0; }
    int64_t Current() const { return static_cast<int64_t>(base) + (app_books ? Game() : 0); }
    uint32_t RealUsed() const { return base + Game() + Hidden(); }
    uint64_t BooksLeft() const { return limit > Current() ? static_cast<uint64_t>(limit - Current()) : 0; }
    uint64_t PhysFree() const {
        if (!app_books) {
            return 0xFFFFFFFFull;
        }
        return region > RealUsed() ? region - RealUsed() : 0;
    }
    uint64_t Largest() const {
        const uint64_t phys = PhysFree();
        return now_ms < contiguous_until_ms && contiguous < phys ? contiguous : phys;
    }

    Raw Read() {
        Raw r;
        r.limit = limit;
        r.current = Current();
        r.used_rc = region_known ? 0 : -1;
        r.used = base + Game() + (hidden_seen ? Hidden() : 0);
        r.luma_rc = luma ? 0 : -1;
        r.luma_region = luma ? region : 0;
        r.appmemalloc = region;
        r.own_rc = 0;
        r.own = base;
        r.memtype_rc = 0;
        r.memtype = memtype;
        return r;
    }
    bool Fits(uint64_t heap, uint64_t linear) const {
        return linear <= Largest() && heap <= heap_plan::kHeapAreaSize && heap + linear <= BooksLeft() && heap + linear <= PhysFree();
    }
    Part Try(uint32_t heap, uint32_t linear, int32_t* error) {
        tries.push_back(heap + linear);
        linears.push_back(linear);
        try_times.push_back(now_ms);
        if (linear > BooksLeft() || linear > Largest()) {
            *error = static_cast<int32_t>(0xD86003F3u);
            return Part::Linear;
        }
        ++zeroings;
        now_ms += try_cost_ms;
        if (heap > heap_plan::kHeapAreaSize || static_cast<uint64_t>(heap) + linear > BooksLeft() ||
            static_cast<uint64_t>(heap) + linear > PhysFree()) {
            *error = static_cast<int32_t>(0xC860180Au);
            return Part::Heap;
        }
        return Part::None;
    }
    void Sleep(uint32_t ms) {
        now_ms += ms;
        ++sleeps;
    }
    uint32_t NowMs() const { return now_ms; }
    // What libctru 2.7.0 would do on this kernel now: one request of all the books, split its way.
    bool Libctru270Starts() {
        const uint32_t remaining = heap_plan::BooksFree(heap_plan::FromRaw(Read()).facts);
        const Split s = Libctru270Split(remaining);
        return Fits(s.heap, s.linear);
    }
};

static bool RunOn(FakeKernel& k, Record& r) {
    r = Record{};
    return heap_plan::Run(k, 0, 0, r);
}

// The kernel's raw answers (heap_alloc.cpp, KernelEnv::Read) and what they become.
static void TestFromRaw() {
    Raw r;
    r.limit = 124 * MiB;
    r.current = kBase;
    r.used_rc = 0;
    r.used = kBase;
    r.luma_rc = 0;
    r.luma_region = 124 * MiB;
    r.appmemalloc = 119 * MiB; // a New 3DS with Luma after a game with a plugin: Rosalina took the plugin's 5 MiB off APPMEMALLOC
    r.own_rc = 0;
    r.own = kBase + 8 * KiB;
    r.memtype_rc = 0;
    r.memtype = 0x100;
    heap_plan::Reading d = heap_plan::FromRaw(r);
    CHECK(d.facts.region == 124 * MiB && d.region_luma == 1); // Luma's size, not APPMEMALLOC
    CHECK(d.facts.used == kBase && d.facts.own == kBase + 8 * KiB && d.memtype == 0x100);
    CHECK(heap_plan::Available(d.facts) == kFull);
    CHECK(heap_plan::Foreign(d.facts) == 0);

    // Luma's call fails, or answers 0 with success (a stock kernel), or something past 256 MiB: APPMEMALLOC
    Raw a = r;
    a.luma_rc = static_cast<int32_t>(0xD8E007EDu);
    CHECK(heap_plan::FromRaw(a).facts.region == 119 * MiB && heap_plan::FromRaw(a).region_luma == 0);
    a = r;
    a.luma_region = 0;
    CHECK(heap_plan::FromRaw(a).facts.region == 119 * MiB && heap_plan::FromRaw(a).region_luma == 0);
    a = r;
    a.luma_region = 0x10001000;
    CHECK(heap_plan::FromRaw(a).facts.region == 119 * MiB);
    // no count of the region: the books alone
    a = r;
    a.used_rc = -1;
    CHECK(heap_plan::FromRaw(a).facts.region == 0 && heap_plan::FromRaw(a).facts.used == 0);
    a = r;
    a.used = 0;
    CHECK(heap_plan::FromRaw(a).facts.region == 0);
    // the memory type: other bits of the kernel flags do not count; SYSTEM (an applet): no region and no own process, the books alone
    a = r;
    a.memtype = 0x2100;
    CHECK(heap_plan::FromRaw(a).memtype == 0x100 && heap_plan::FromRaw(a).facts.region == 124 * MiB);
    a = r;
    a.memtype = 0x200;
    d = heap_plan::FromRaw(a);
    CHECK(d.memtype == 0x200 && d.facts.region == 0 && d.facts.own == 0);
    a = r;
    a.memtype_rc = -1;
    d = heap_plan::FromRaw(a);
    CHECK(d.memtype == heap_plan::kUnknown && d.facts.region == 124 * MiB && d.facts.own == kBase + 8 * KiB); // (unknown: APPLICATION)
    // the own process not known: the books never say "a game"
    a = r;
    a.own_rc = -1;
    a.current = kBase + 82048 * KiB;
    d = heap_plan::FromRaw(a);
    CHECK(d.facts.own == 0 && !heap_plan::ForeignInBooks(d.facts));
}

static void TestPieces() {
    Facts f{};
    f.limit = 124 * MiB;
    f.current = kBase;
    f.region = 124 * MiB;
    f.used = kBase;
    f.own = kBase;
    CHECK(heap_plan::BooksFree(f) == 124 * MiB - kBase);
    CHECK(heap_plan::RegionFree(f) == 124 * MiB - kBase);
    CHECK(heap_plan::Available(f) == kFull);
    // books over their limit, a region more used than its size: nothing, not a wrapped number
    Facts over = f;
    over.current = over.limit + 4096;
    CHECK(heap_plan::BooksFree(over) == 0);
    CHECK(heap_plan::Available(over) == 0);
    Facts full = f;
    full.used = full.region + 4096;
    CHECK(heap_plan::RegionFree(full) == 0);
    CHECK(heap_plan::Available(full) == 0);
    // a region not known: the books alone
    Facts unknown = f;
    unknown.region = 0;
    unknown.used = 100 * MiB;
    CHECK(heap_plan::Available(unknown) == heap_plan::BooksFree(f) - heap_plan::kMargin);
    // pages: a count that is not page aligned goes down
    Facts odd = f;
    odd.current += 123;
    CHECK((heap_plan::BooksFree(odd) & 0xFFF) == 0);
    CHECK(heap_plan::BooksFree(odd) == heap_plan::BooksFree(f) - 4096);
    // less than the margin: nothing
    Facts tiny = f;
    tiny.current = tiny.limit - 128 * KiB;
    CHECK(heap_plan::Available(tiny) == 0);

    // memory that is not the app's: in the books past the own process + 1 MiB, in the region past the books + the margin
    CHECK(heap_plan::Foreign(f) == 0);
    Facts game = f;
    game.current = kBase + heap_plan::kForeignMargin;
    game.used = static_cast<uint32_t>(game.current);
    CHECK(heap_plan::Foreign(game) == 0);
    game.current += 4096;
    game.used += 4096;
    CHECK(heap_plan::Foreign(game) == 1);
    Facts block = f;
    block.used = kBase + heap_plan::kMargin;
    CHECK(heap_plan::Foreign(block) == 0); // (the alias's 8 KB and up to the margin)
    block.used += 4096;
    CHECK(heap_plan::Foreign(block) == 2);
    block.current = game.current;
    block.used = static_cast<uint32_t>(block.current) + 5 * MiB;
    CHECK(heap_plan::Foreign(block) == 3);
    Facts nobooks = block;
    nobooks.region = 0;
    nobooks.own = 0;
    CHECK(heap_plan::Foreign(nobooks) == 0);
    // a limit above the region (more books than pages) is not a block outside the books when it is within the margin
    Facts drift = f;
    drift.limit += 8 * KiB;
    CHECK(heap_plan::Foreign(drift) == 0);

    // the split is libctru's, for every total from 4 KiB to 256 MiB in uneven steps, with and without fixed sizes
    int mismatches = 0;
    int mismatches270 = 0;
    for (uint64_t t = 4096; t <= 256ull * MiB; t += 4096ull * 37) {
        const uint32_t total = static_cast<uint32_t>(t);
        const uint32_t fixed[][2] = {{0, 0}, {16 * MiB, 0}, {0, 8 * MiB}, {20 * MiB, 10 * MiB}, {100 * MiB, 0}};
        for (const auto& fx : fixed) {
            const Split a = heap_plan::SplitTotal(total, fx[0], fx[1]);
            const Split b = LibctruSplit(total, fx[0], fx[1]);
            if (a.ok != b.ok || (a.ok && (a.heap != b.heap || a.linear != b.linear))) {
                ++mismatches;
            }
            if (a.ok && a.heap + a.linear > total) {
                ++mismatches;
            }
            // with no cap on the linear heap, SplitCapped is SplitTotal
            const Split c = heap_plan::SplitCapped(total, fx[0], fx[1], heap_plan::kUnknown);
            if (c.ok != a.ok || c.heap != a.heap || c.linear != a.linear) {
                ++mismatches;
            }
        }
        // 2.7.0 agrees wherever the heap stays inside the heap area
        const Split a = heap_plan::SplitTotal(total, 0, 0);
        const Split c = Libctru270Split(total);
        if (c.heap <= heap_plan::kHeapAreaSize && (a.heap != c.heap || a.linear != c.linear)) {
            ++mismatches270;
        }
    }
    CHECK(mismatches == 0);
    CHECK(mismatches270 == 0);
    CHECK(!heap_plan::SplitTotal(10 * MiB, 8 * MiB, 4 * MiB).ok);

    // the linear heap held under a cap: the heap takes the rest, never past the heap area; a fixed linear size is not touched
    const Split capped = heap_plan::SplitCapped(kFull, 0, 0, 24 * MiB);
    CHECK(capped.linear == 24 * MiB && capped.heap == kFull - 24 * MiB);
    const Split area = heap_plan::SplitCapped(kFull, 0, 0, 12 * MiB);
    CHECK(area.linear == 12 * MiB && area.heap == heap_plan::kHeapAreaSize);
    const Split fixed_linear = heap_plan::SplitCapped(kFull, 0, 20 * MiB, 12 * MiB);
    CHECK(fixed_linear.linear == 20 * MiB && fixed_linear.heap == heap_plan::SplitTotal(kFull, 0, 20 * MiB).heap);
    const Split fixed_heap = heap_plan::SplitCapped(64 * MiB, 30 * MiB, 0, 16 * MiB);
    CHECK(fixed_heap.heap == 30 * MiB && fixed_heap.linear == 16 * MiB);

    // the minimum gives 12 MiB to each side
    const Split m = heap_plan::SplitTotal(heap_plan::kMinTotal, 0, 0);
    CHECK(m.heap == 12 * MiB && m.linear == 12 * MiB);
    CHECK(heap_plan::kMinLinear == m.linear);
    // a cut is 3/4, page aligned, never under the minimum
    CHECK(heap_plan::Shrink(100 * MiB, heap_plan::kMinTotal) == 75 * MiB);
    CHECK((heap_plan::Shrink(100 * MiB + 4096, heap_plan::kMinTotal) & 0xFFF) == 0);
    CHECK(heap_plan::Shrink(30 * MiB, heap_plan::kMinTotal) == heap_plan::kMinTotal);
    // the floor: kFloorTotal, or fixed sizes past the minimum; a cut lands on the minimum first, then goes on to the floor
    CHECK(heap_plan::kFloorTotal == 8 * MiB && heap_plan::kFloorTotal < heap_plan::kMinTotal);
    CHECK(heap_plan::Floor(heap_plan::kMinTotal) == heap_plan::kFloorTotal);
    CHECK(heap_plan::Floor(30 * MiB) == 30 * MiB);
    CHECK(heap_plan::Cut(30 * MiB, heap_plan::kMinTotal) == heap_plan::kMinTotal);
    CHECK(heap_plan::Cut(heap_plan::kMinTotal, heap_plan::kMinTotal) == 18 * MiB);
    CHECK(heap_plan::Cut(10 * MiB, heap_plan::kMinTotal) == heap_plan::kFloorTotal);
    CHECK(heap_plan::Cut(40 * MiB, 30 * MiB) == 30 * MiB && heap_plan::Cut(30 * MiB, 30 * MiB) == 30 * MiB);
}

// Clean books on a New 3DS: one request, at once, what libctru gave minus the margin.
static void TestCleanNew() {
    FakeKernel k;
    k.now_ms = 12345; // (the console started 12.345 s ago)
    Record r;
    CHECK(k.Libctru270Starts());
    CHECK(RunOn(k, r));
    const uint32_t books = 124 * MiB - kBase;
    CHECK(r.ok == 1);
    CHECK(r.attempts == 1 && r.waited_ms == 0 && r.elapsed_ms == 0 && r.shrinks == 0 && r.linear_cuts == 0 && k.sleeps == 0);
    CHECK(r.total == books - heap_plan::kMargin);
    CHECK(r.linear == 32 * MiB);
    CHECK(r.heap == r.total - 32 * MiB);
    CHECK(r.heap == Libctru270Split(books).heap - heap_plan::kMargin); // the margin comes off the heap
    CHECK(r.first.limit == 124 * MiB && r.first.current == kBase && r.first.own == kBase);
    CHECK(r.foreign_first == 0 && r.foreign_last == 0 && r.memtype == 0x100 && r.region_luma == 1);
    CHECK(k.zeroings == 1);
    CHECK(r.start_ms == 12345);
}

// 8 KB outside the books (the two pages of the plugin's alias, kept by a game's process that is still alive): libctru 2.7.0 stops; the app starts
// at once, 8 KB + the margin under the books, without waiting (under the margin it is not "memory that is not the app's").
static void TestEightKiB() {
    FakeKernel k;
    k.hidden = 8 * KiB;
    Record r;
    CHECK(!k.Libctru270Starts());
    CHECK(RunOn(k, r));
    CHECK(r.attempts == 1 && r.waited_ms == 0 && r.foreign_first == 0);
    CHECK(r.total == kFull - 8 * KiB);
    CHECK(r.linear == 32 * MiB);

    // the same pages with no count of the region (the books alone): the margin alone covers them, one request
    FakeKernel u;
    u.hidden = 8 * KiB;
    u.region_known = false;
    Record ru;
    CHECK(RunOn(u, ru));
    CHECK(ru.attempts == 1 && ru.waited_ms == 0);
    CHECK(ru.total == kFull);

    // more than the margin, with no count of the region, given back after 600 ms: refused at 0, the same counts are not asked again before 1 s,
    // then the whole of the books
    FakeKernel v;
    v.hidden = 1 * MiB;
    v.region_known = false;
    v.hidden_until_ms = 600;
    Record rv;
    CHECK(RunOn(v, rv));
    CHECK(rv.attempts == 2);
    CHECK(v.try_times.size() == 2 && v.try_times[0] == 0 && v.try_times[1] == heap_plan::kRetrySameMs);
    CHECK(rv.waited_ms == heap_plan::kRetrySameMs);
    CHECK(rv.shrinks == 0);
    CHECK(rv.total == kFull);
    CHECK(rv.last_error == static_cast<int32_t>(0xC860180Au) && rv.refused_part == static_cast<uint32_t>(Part::Heap)); // (the first refusal)
}

// 5 MiB outside the books (Rosalina's plugin block on a New 3DS, until it frees it).
static void TestFiveMiB() {
    // seen by the region's count, never given back: 3 s of waiting, then one request, 5 MiB less
    FakeKernel k;
    k.hidden = 5 * MiB;
    Record r;
    CHECK(!k.Libctru270Starts());
    CHECK(RunOn(k, r));
    CHECK(r.attempts == 1 && r.waited_ms == heap_plan::kWaitBudgetMs && r.elapsed_ms == heap_plan::kWaitBudgetMs && r.shrinks == 0);
    CHECK(r.total == kFull - 5 * MiB);
    CHECK(r.foreign_first == 2 && r.foreign_last == 2);
    CHECK(k.try_times.size() == 1 && k.try_times[0] == heap_plan::kWaitBudgetMs);

    // seen, given back after 300 ms: the app waits 300 ms and gets everything
    FakeKernel s;
    s.hidden = 5 * MiB;
    s.hidden_until_ms = 300;
    Record rs;
    CHECK(RunOn(s, rs));
    CHECK(rs.attempts == 1 && rs.waited_ms == 300 && rs.total == kFull);
    CHECK(rs.foreign_first == 2 && rs.foreign_last == 0);

    // not seen (counts that lie), given back after 1 s: refused at 0, asked again at 1 s, everything
    FakeKernel w;
    w.hidden = 5 * MiB;
    w.hidden_seen = false;
    w.hidden_until_ms = 1000;
    Record rw;
    CHECK(RunOn(w, rw));
    CHECK(rw.attempts == 2 && rw.waited_ms == 1000 && rw.shrinks == 0);
    CHECK(rw.total == kFull);

    // not seen, never given back: one request a second for 3 s, then one cut to 3/4 is enough
    FakeKernel p;
    p.hidden = 5 * MiB;
    p.hidden_seen = false;
    Record rp;
    CHECK(RunOn(p, rp));
    CHECK(rp.waited_ms == heap_plan::kWaitBudgetMs);
    CHECK(rp.attempts == 4);
    CHECK(rp.shrinks == 1);
    CHECK(rp.total == heap_plan::Shrink(kFull, heap_plan::kMinTotal));
    CHECK(p.tries.size() == 4 && p.tries[0] == kFull && p.tries[2] == kFull && p.try_times[1] == 1000 && p.try_times[2] == 2000 &&
          p.try_times[3] == heap_plan::kWaitBudgetMs);
}

// A game whose process is still alive: the books and the region's count both carry it. The dump of 2026-11-06 (Friend-A, New 3DS XL, after MHXX):
// currentCommit 0x5547000 = 82048 KB of the game + 5276 KB of the app.
static void TestGameAlive() {
    // the game's process ends after 1.5 s: the app waits for it and takes everything
    FakeKernel k;
    k.game = 82048 * KiB;
    k.game_until_ms = 1500;
    Record r;
    CHECK(RunOn(k, r));
    CHECK(r.attempts == 1 && r.waited_ms == 1500 && r.elapsed_ms == 1500 && r.shrinks == 0);
    CHECK(r.total == kFull);
    CHECK(r.linear == 32 * MiB);
    CHECK(r.first.current == static_cast<int64_t>(5276 + 82048) * KiB);
    CHECK(r.last.current == kBase);
    CHECK(r.foreign_first == 1 && r.foreign_last == 0);

    // it never ends: 3 s, then what there is (126976 - 87324 - 256 = 39396 KiB, half and half), with no cut
    FakeKernel n;
    n.game = 82048 * KiB;
    Record rn;
    CHECK(RunOn(n, rn));
    const uint32_t avail = 124 * MiB - kBase - 82048 * KiB - heap_plan::kMargin;
    CHECK(avail == 39396 * KiB);
    CHECK(rn.attempts == 1 && rn.waited_ms == heap_plan::kWaitBudgetMs && rn.shrinks == 0);
    CHECK(rn.total == avail && rn.heap + rn.linear == avail);
    CHECK(rn.heap >= 19 * MiB && rn.linear >= 19 * MiB);
    CHECK(rn.foreign_first == 1 && rn.foreign_last == 1);

    // the same game with the 8 KB of the alias on top, ending after 1.5 s: everything but the 8 KB
    FakeKernel a;
    a.game = 82048 * KiB;
    a.game_until_ms = 1500;
    a.hidden = 8 * KiB;
    Record ra;
    CHECK(!a.Libctru270Starts());
    CHECK(RunOn(a, ra));
    CHECK(ra.attempts == 1 && ra.waited_ms == 1500 && ra.total == kFull - 8 * KiB);

    // a bigger game that leaves less than the minimum and ends after 1.5 s: the same
    FakeKernel b;
    b.game = 110 * MiB;
    b.game_until_ms = 1500;
    Record rb;
    CHECK(RunOn(b, rb));
    CHECK(rb.attempts == 1);
    CHECK(rb.waited_ms == 1500);
    CHECK(rb.total == kFull);
    CHECK(rb.first.current == static_cast<int64_t>(kBase) + 110 * MiB);
    CHECK(rb.last.current == kBase);
}

// The linear heap refused for want of one free block that big (the counts are right): the linear heap is cut, the heap keeps the rest.
static void TestLinearContiguity() {
    // the biggest free block is 20 MiB: 32 and 24 refused, 18 given, at once; the heap is not cut (it takes the rest up to the heap area)
    FakeKernel k;
    k.contiguous = 20 * MiB;
    Record r;
    CHECK(!k.Libctru270Starts());
    CHECK(RunOn(k, r));
    CHECK(r.attempts == 3 && r.linear_cuts == 2 && r.waited_ms == 0 && r.shrinks == 0);
    CHECK(k.linears.size() == 3 && k.linears[0] == 32 * MiB && k.linears[1] == 24 * MiB && k.linears[2] == 18 * MiB);
    CHECK(r.linear == 18 * MiB);
    CHECK(r.heap == heap_plan::kHeapAreaSize);                         // (more than the 88676 KiB of a clean start)
    CHECK(r.heap >= heap_plan::SplitTotal(kFull, 0, 0).heap);
    CHECK(k.zeroings == 1);                                            // the refused linear heaps cost nothing
    CHECK(r.refused_part == static_cast<uint32_t>(Part::Linear));

    // the biggest block is 10 MiB (under the 12 MiB floor) for 1.5 s: 32, 24, 18, 13.5, 12 refused at 0 and at 1 s, everything at 2 s
    FakeKernel t;
    t.contiguous = 10 * MiB;
    t.contiguous_until_ms = 1500;
    Record rt;
    CHECK(RunOn(t, rt));
    CHECK(rt.attempts == 11 && rt.linear_cuts == 8 && rt.waited_ms == 2000 && rt.shrinks == 0);
    CHECK(rt.total == kFull && rt.linear == 32 * MiB); // (the cuts start over after a wait)
    CHECK(t.zeroings == 1);
    bool floor_ok = true;
    for (uint32_t l : t.linears) {
        if (l < heap_plan::kMinLinear) {
            floor_ok = false;
        }
    }
    CHECK(floor_ok);

    // the biggest block stays at 10 MiB: the total is cut too, the minimum's 12 MiB of linear heap is refused, and the cut under the minimum
    // (18 MiB: 9 of linear heap) fits; one zeroing in all (libctru 2.7.0 stops on its 32 MiB of linear heap)
    FakeKernel e;
    e.contiguous = 10 * MiB;
    Record re;
    CHECK(!e.Libctru270Starts());
    CHECK(RunOn(e, re));
    CHECK(re.total == 18 * MiB && re.linear == 9 * MiB && re.heap == 9 * MiB);
    CHECK(e.zeroings == 1);
    CHECK(e.tries.size() >= 2 && e.tries[e.tries.size() - 2] == heap_plan::kMinTotal && e.linears[e.tries.size() - 2] == heap_plan::kMinLinear);
    CHECK(re.elapsed_ms == heap_plan::kWaitBudgetMs);
    floor_ok = true;
    for (std::size_t i = 0; i < e.tries.size(); ++i) {
        if ((e.tries[i] >= heap_plan::kMinTotal && e.linears[i] < heap_plan::kMinLinear) || e.tries[i] < heap_plan::kFloorTotal) {
            floor_ok = false;
        }
    }
    CHECK(floor_ok);

    // the biggest block stays at 3 MiB: not even the floor's 4 MiB of linear heap fit; the app stops at the floor, with no zeroing at all
    FakeKernel d;
    d.contiguous = 3 * MiB;
    Record rd;
    CHECK(!RunOn(d, rd));
    CHECK(rd.ok == 0);
    CHECK(d.tries.back() == heap_plan::kFloorTotal && d.linears.back() == heap_plan::kFloorTotal / 2);
    CHECK(d.zeroings == 0);
    CHECK(rd.attempts == d.tries.size() && rd.attempts < heap_plan::kMaxAttempts);
    CHECK(rd.elapsed_ms == heap_plan::kWaitBudgetMs);
    CHECK(rd.refused_part == static_cast<uint32_t>(Part::Linear) && rd.refused_linear == heap_plan::kFloorTotal / 2);
    floor_ok = true;
    for (std::size_t i = 0; i < d.tries.size(); ++i) {
        if ((d.tries[i] >= heap_plan::kMinTotal && d.linears[i] < heap_plan::kMinLinear) || d.tries[i] < heap_plan::kFloorTotal) {
            floor_ok = false;
        }
    }
    CHECK(floor_ok);

    // the numbers left in the crash dump for that stop
    const heap_plan::BreakWords b = heap_plan::PackBreak(rd);
    char magic[5] = {};
    std::memcpy(magic, &b.w[0], 4);
    CHECK(std::string(magic) == "HEAP");
    CHECK(b.w[1] == 0xD86003F3u);
    CHECK((b.w[2] & 0xFF) == rd.attempts && ((b.w[2] >> 8) & 0xFF) == rd.shrinks && ((b.w[2] >> 16) & 0xFF) == rd.linear_cuts);
    CHECK(((b.w[2] >> 24) & 32u) != 0 && ((b.w[2] >> 24) & 16u) != 0 && ((b.w[2] >> 24) & 15u) == 0);
    CHECK(b.w[3] == rd.elapsed_ms);
    CHECK(b.w[4] == 126976 && b.w[5] == 5276 && b.w[6] == 126976 && b.w[7] == 5276 && b.w[8] == 5276);
    CHECK(b.w[9] == 5276 && b.w[10] == 5276 && b.w[11] == 4096 && b.w[12] == 4096 && b.w[13] == 0x100 && b.w[14] == 126976 && b.w[15] == 126976);
}

// A try takes time (the kernel zeroes every page it gives): the 3 s are real time, and a request refused on the same counts is not made again before
// 1 s, so the waiting phase costs a few zeroings, not thirty.
static void TestTryCost() {
    FakeKernel k;
    k.hidden = 5 * MiB;
    k.hidden_seen = false;
    k.try_cost_ms = 700;
    Record r;
    CHECK(RunOn(k, r));
    std::size_t waiting = 0;
    for (uint32_t t : k.try_times) {
        if (t < heap_plan::kWaitBudgetMs) {
            ++waiting;
        }
    }
    CHECK(waiting == 3);
    CHECK(k.now_ms <= heap_plan::kWaitBudgetMs + 2 * k.try_cost_ms);
    CHECK(r.elapsed_ms >= heap_plan::kWaitBudgetMs && r.waited_ms < r.elapsed_ms);
    CHECK(r.total == heap_plan::Shrink(kFull, heap_plan::kMinTotal));
}

// The New (124 MiB), the Old (64 MiB), memory mode 7 of the New (178 MiB).
static void TestModels() {
    FakeKernel n;
    Record rn;
    CHECK(RunOn(n, rn));
    CHECK(rn.heap == kFull - 32 * MiB && rn.linear == 32 * MiB);

    FakeKernel o;
    o.limit = 64 * MiB;
    o.region = 64 * MiB;
    o.base = 4 * MiB;
    Record ro;
    CHECK(o.Libctru270Starts());
    CHECK(RunOn(o, ro));
    CHECK(ro.attempts == 1 && ro.waited_ms == 0);
    CHECK(ro.total == 60 * MiB - heap_plan::kMargin);
    CHECK(ro.linear == 32 * MiB);
    CHECK(ro.heap == 28 * MiB - heap_plan::kMargin); // (the TitleDelivery logs of a 64 MiB start say "heap totale 28.0 MiB, lineare totale 32.0")
    // the Old with 8 KB outside the books
    o.hidden = 8 * KiB;
    CHECK(!o.Libctru270Starts());
    CHECK(RunOn(o, ro));
    CHECK(ro.attempts == 1 && ro.waited_ms == 0 && ro.total == 60 * MiB - 8 * KiB - heap_plan::kMargin);

    FakeKernel s;
    s.limit = 0xB200000;
    s.region = 0xB200000;
    Record rs;
    CHECK(!s.Libctru270Starts()); // 2.7.0 asks for a heap past the 96 MiB of the heap area
    CHECK(RunOn(s, rs));
    CHECK(rs.attempts == 1);
    CHECK(rs.heap == heap_plan::kHeapAreaSize);
    CHECK(rs.heap + rs.linear == 0xB200000u - kBase - heap_plan::kMargin);
}

// 3DSLink.3dsx run as an applet: its process allocates from the SYSTEM region with the applet's books; a game suspended in the APPLICATION
// region does not concern it. The books alone count, at once.
static void TestApplet() {
    FakeKernel k;
    k.memtype = 0x200;
    k.app_books = false;
    k.limit = 40 * MiB;
    k.hidden = 110 * MiB; // the suspended game, in the APPLICATION region's count
    Record r;
    CHECK(RunOn(k, r));
    CHECK(r.attempts == 1 && r.waited_ms == 0);
    CHECK(r.total == 40 * MiB - kBase - heap_plan::kMargin);
    CHECK(r.memtype == 0x200 && r.first.region == 0 && r.first.own == 0 && r.foreign_first == 0);
}

// The minimum and the floor. Wherever libctru 2.7.0 (3DS Link 1.1.1) starts with at least the floor, the app starts too.
static void TestMinimum() {
    // less than the minimum for good (a game of 100 MiB whose process never ends): 3 s of waiting, then what there is, as libctru: one request
    FakeKernel k;
    k.game = 100 * MiB;
    Record r;
    CHECK(k.Libctru270Starts());
    CHECK(RunOn(k, r));
    const uint32_t avail = 124 * MiB - kBase - 100 * MiB - heap_plan::kMargin;
    CHECK(avail == 19044 * KiB);
    CHECK(r.ok == 1 && r.total == avail && r.heap + r.linear == avail);
    CHECK(r.waited_ms == heap_plan::kWaitBudgetMs);
    CHECK(k.tries.size() == 1 && k.tries[0] == avail);
    CHECK(r.attempts == 1 && r.shrinks == 0);
    char line[1024];
    heap_plan::FormatLine(r, line, sizeof(line));
    CHECK(std::string(line).rfind("heap at start: ok (reduced: under the minimum), total 19044 KiB", 0) == 0);

    // a game of 110 MiB: 8804 KiB, still above the floor: the app starts (4.3 MiB on each side)
    FakeKernel g;
    g.game = 110 * MiB;
    Record rg;
    CHECK(g.Libctru270Starts());
    CHECK(RunOn(g, rg));
    CHECK(rg.total == 8804 * KiB && rg.attempts == 1 && g.tries.size() == 1);

    // under the floor (a game of 112 MiB: 6756 KiB): the floor is asked for once (the counts may be wrong), refused: the app stops with its
    // numbers. libctru would start here with 3.4 MiB of linear heap, under the 3.3-3.5 MiB the app is seen to use plus its slack (ESTIMATE:
    // gfx/citro2d would fail later, with no numbers)
    FakeKernel u;
    u.game = 112 * MiB;
    Record ru;
    CHECK(!RunOn(u, ru));
    CHECK(ru.ok == 0 && ru.waited_ms == heap_plan::kWaitBudgetMs);
    CHECK(u.tries.size() == 1 && u.tries[0] == heap_plan::kFloorTotal && ru.attempts == 1);
    CHECK(ru.refused_part == static_cast<uint32_t>(Part::Heap) && ru.last_error == static_cast<int32_t>(0xC860180Au));
    const heap_plan::BreakWords bu = heap_plan::PackBreak(ru);
    CHECK(bu.w[0] == heap_plan::kBreakMagic && bu.w[1] == 0xC860180Au && (bu.w[2] & 0xFF) == 1 && ((bu.w[2] >> 24) & 1u) != 0);
    CHECK(bu.w[5] == 5276 + 112 * 1024 && bu.w[11] == 4096 && bu.w[12] == 4096);

    // 3DSLink.3dsx run as an applet with small books (20 MiB): 3 s of waiting (less than the minimum), then all of it
    FakeKernel a;
    a.memtype = 0x200;
    a.app_books = false;
    a.limit = kBase + 20 * MiB;
    Record ra;
    CHECK(a.Libctru270Starts());
    CHECK(RunOn(a, ra));
    CHECK(ra.total == 20 * MiB - heap_plan::kMargin && ra.attempts == 1 && ra.waited_ms == heap_plan::kWaitBudgetMs);

    // the counts lie by a lot (100 MiB held, not seen: 18.8 MiB really free): the waiting phase asks only at 0, 1 and 2 s (the linear heap cut
    // where it does not fit); then the total is cut 3/4 at a time down to the minimum, and on under it: 18 MiB fit (libctru 2.7.0 stops here)
    FakeKernel l;
    l.hidden = 100 * MiB;
    l.hidden_seen = false;
    Record rl;
    CHECK(!l.Libctru270Starts());
    CHECK(RunOn(l, rl));
    CHECK(!l.tries.empty());
    CHECK(l.tries.back() == 18 * MiB && rl.total == 18 * MiB);
    CHECK(l.tries.size() >= 2 && l.tries[l.tries.size() - 2] == heap_plan::kMinTotal);
    bool steps_ok = true;
    std::vector<uint32_t> totals; // the distinct totals after the waiting phase
    for (std::size_t i = 0; i < l.tries.size(); ++i) {
        if (l.tries[i] < heap_plan::kFloorTotal || (l.tries[i] >= heap_plan::kMinTotal && l.linears[i] < heap_plan::kMinLinear)) {
            steps_ok = false;
        }
        if (l.try_times[i] < heap_plan::kWaitBudgetMs) {
            if (l.try_times[i] % heap_plan::kRetrySameMs != 0 || l.tries[i] > kFull) {
                steps_ok = false;
            }
        } else if (totals.empty() || totals.back() != l.tries[i]) {
            totals.push_back(l.tries[i]);
        }
    }
    CHECK(steps_ok);
    bool cuts_ok = !totals.empty() && totals[0] == heap_plan::Shrink(kFull, heap_plan::kMinTotal);
    for (std::size_t i = 1; i < totals.size(); ++i) {
        if (totals[i] != heap_plan::Cut(totals[i - 1], heap_plan::kMinTotal)) {
            cuts_ok = false;
        }
    }
    CHECK(cuts_ok);
    CHECK(rl.attempts == l.tries.size());
    CHECK(rl.shrinks == totals.size());
    CHECK(rl.attempts < heap_plan::kMaxAttempts);
    CHECK(rl.elapsed_ms == heap_plan::kWaitBudgetMs);

    // the same, with 25 MiB really free: the cuts stop at the first total that fits, at least the minimum
    FakeKernel f;
    f.hidden = 124 * MiB - kBase - 25 * MiB;
    f.hidden_seen = false;
    Record rf;
    CHECK(RunOn(f, rf));
    CHECK(rf.total >= heap_plan::kMinTotal && rf.total <= 25 * MiB);
    CHECK(rf.heap >= 12 * MiB && rf.linear >= 12 * MiB);

    // fixed sizes bigger than the minimum raise it
    FakeKernel x;
    x.game = 90 * MiB; // 28.8 MiB free
    Record rx;
    CHECK(!heap_plan::Run(x, 20 * MiB, 10 * MiB, rx)); // 30 MiB fixed: more than there is
    CHECK(x.tries.size() == 1 && x.tries[0] == 30 * MiB && x.linears[0] == 10 * MiB);
}

static void TestLine() {
    FakeKernel k;
    k.hidden = 8 * KiB;
    Record r;
    CHECK(RunOn(k, r));
    char line[1024];
    const int n = heap_plan::FormatLine(r, line, sizeof(line));
    CHECK(n > 0 && n < static_cast<int>(sizeof(line)));
    const std::string s(line);
    CHECK(s.rfind("heap at start: ok, total ", 0) == 0);
    CHECK(s.find("linear 32768,") != std::string::npos);
    CHECK(s.find("attempts 1, waited 0 ms, elapsed 0 ms, cuts 0, linear cuts 0, last error 0x00000000 (none: heap 0 linear 0)") != std::string::npos);
    CHECK(s.find("first: limit 126976 current 5276 region 126976 used 5284 own 5276 KiB, foreign 0") != std::string::npos);
    CHECK(s.find("region from Luma, APPMEMALLOC 126976 KiB, memtype 0x100") != std::string::npos);
    CHECK(s.find('\n') == std::string::npos);

    // the game's process still alive for 1.5 s: the line says what the first reading saw
    FakeKernel g;
    g.game = 82048 * KiB;
    g.game_until_ms = 1500;
    Record rg;
    CHECK(RunOn(g, rg));
    heap_plan::FormatLine(rg, line, sizeof(line));
    const std::string sg(line);
    CHECK(sg.find("waited 1500 ms, elapsed 1500 ms") != std::string::npos);
    CHECK(sg.find("first: limit 126976 current 87324 region 126976 used 87324 own 5276 KiB, foreign 1") != std::string::npos);
    CHECK(sg.find("last: limit 126976 current 5276 region 126976 used 5276 own 5276 KiB, foreign 0") != std::string::npos);

    // the longest line (every number at its widest) still fits
    Record w{};
    w.first.limit = w.first.current = w.last.limit = w.last.current = -0x7FFFFFFFFFFFFFFFll;
    w.first.region = w.first.used = w.first.own = w.last.region = w.last.used = w.last.own = 0xFFFFFFFFu;
    w.total = w.heap = w.linear = w.attempts = w.waited_ms = w.elapsed_ms = w.shrinks = w.linear_cuts = w.appmemalloc = 0xFFFFFFFFu;
    w.refused_heap = w.refused_linear = w.memtype = w.foreign_first = w.foreign_last = 0xFFFFFFFFu;
    w.refused_part = static_cast<uint32_t>(Part::Linear);
    w.last_error = -1;
    const int nw = heap_plan::FormatLine(w, line, sizeof(line));
    CHECK(nw > 0 && nw < static_cast<int>(sizeof(line)));
    CHECK(std::string(line).rfind("heap at start: FAILED,", 0) == 0);
    w.ok = 1;
    w.total = 0;
    const int nr = heap_plan::FormatLine(w, line, sizeof(line));
    CHECK(nr > nw && nr < static_cast<int>(sizeof(line)));
    CHECK(std::string(line).rfind("heap at start: ok (reduced: under the minimum), total 0 KiB", 0) == 0);
}

// The log keeps a line per start: the console's date and time and the time since boot, then the line; the last kLogMaxLines starts, at most
// kLogMaxBytes.
static void TestLog() {
    char stamp[96];
    CHECK(heap_plan::FormatStamp(1700000000, 123456, stamp, sizeof(stamp)) > 0);
    CHECK(std::string(stamp) == "2023-11-14 22:13:20, boot +123.456 s | ");
    heap_plan::FormatStamp(1293840000, 7, stamp, sizeof(stamp)); // (a console with its clock in 2011)
    CHECK(std::string(stamp) == "2011-01-01 00:00:00, boot +0.007 s | ");
    heap_plan::FormatStamp(1709164800 + 86399, 0, stamp, sizeof(stamp));
    CHECK(std::string(stamp) == "2024-02-29 23:59:59, boot +0.000 s | ");
    heap_plan::FormatStamp(951782400, 1000, stamp, sizeof(stamp));
    CHECK(std::string(stamp) == "2000-02-29 00:00:00, boot +1.000 s | ");
    heap_plan::FormatStamp(-1, 0xFFFFFFFFu, stamp, sizeof(stamp));
    CHECK(std::string(stamp) == "1969-12-31 23:59:59, boot +4294967.295 s | ");

    // a new file, an old one cut in the middle of a line, a line added to the old ones (nothing written over)
    CHECK(heap_plan::KeepLast("", "a", 50, 1000) == "a\n");
    CHECK(heap_plan::KeepLast("x\ny", "a", 50, 1000) == "x\ny\na\n");
    CHECK(heap_plan::KeepLast("x\ny\n", "a", 50, 1000) == "x\ny\na\n");
    // the last 50 lines: the oldest go first
    std::string log;
    for (int i = 1; i <= 60; ++i) {
        log = heap_plan::KeepLast(log, "start " + std::to_string(i), heap_plan::kLogMaxLines, heap_plan::kLogMaxBytes);
    }
    std::size_t lines = 0;
    for (char c : log) {
        lines += c == '\n' ? 1 : 0;
    }
    CHECK(lines == 50);
    CHECK(log.rfind("start 11\n", 0) == 0);
    CHECK(log.size() >= 9 && log.compare(log.size() - 9, 9, "start 60\n") == 0);
    // the bytes: whole lines only, the newest always there, even alone and too long
    CHECK(heap_plan::KeepLast("aaaa\nbbbb\n", "cccc", 50, 10) == "bbbb\ncccc\n");
    CHECK(heap_plan::KeepLast("aaaa\nbbbb\n", "cccccccccccc", 50, 10) == "cccccccccccc\n");
    // 60 starts with the widest line: within the limits, the lines whole
    Record w{};
    w.first.limit = w.first.current = w.last.limit = w.last.current = -0x7FFFFFFFFFFFFFFFll;
    w.first.region = w.first.used = w.first.own = w.last.region = w.last.used = w.last.own = 0xFFFFFFFFu;
    w.heap = w.linear = w.attempts = w.waited_ms = w.elapsed_ms = w.shrinks = w.linear_cuts = w.appmemalloc = 0xFFFFFFFFu;
    w.refused_heap = w.refused_linear = w.memtype = w.foreign_first = w.foreign_last = 0xFFFFFFFFu;
    w.ok = 1;
    char line[1024];
    log.clear();
    for (int i = 0; i < 60; ++i) {
        heap_plan::FormatStamp(-0x7FFFFFFFll * 1000, 0xFFFFFFFFu, stamp, sizeof(stamp));
        heap_plan::FormatLine(w, line, sizeof(line));
        log = heap_plan::KeepLast(log, std::string(stamp) + line, heap_plan::kLogMaxLines, heap_plan::kLogMaxBytes);
    }
    lines = 0;
    for (char c : log) {
        lines += c == '\n' ? 1 : 0;
    }
    CHECK(log.size() <= heap_plan::kLogMaxBytes && lines >= 40 && lines <= heap_plan::kLogMaxLines);
    CHECK(log.back() == '\n' && log.find(" | heap at start: ok (reduced") != std::string::npos);
    CHECK(heap_plan::kLogReadBytes > heap_plan::kLogMaxBytes);
}

int main() {
    TestFromRaw();
    TestPieces();
    TestCleanNew();
    TestEightKiB();
    TestFiveMiB();
    TestGameAlive();
    TestLinearContiguity();
    TestTryCost();
    TestModels();
    TestApplet();
    TestMinimum();
    TestLine();
    TestLog();
    std::printf
("heap_plan_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
