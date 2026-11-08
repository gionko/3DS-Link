// How much memory 3DS Link asks the kernel for before main(), and how it splits it between the heap and the linear heap: the decisions of
// heap_alloc.cpp (the app's own __system_allocateHeaps), with no libctru, so that Tests/settings_test/heap_plan_test.cpp builds this very file on a PC.
//
// libctru 2.7.0 reads the books of the APPLICATION resource limit (limit - current) and asks for ALL of it, down to the byte: the heap first, then the
// linear heap. When even one page of the APPLICATION region is held outside those books (a page that a process which is still alive keeps mapped after
// its owner freed it, a block that Rosalina has not given back yet), or when the region has no single free block as big as the linear heap (the linear
// heap is one contiguous block: KPageHeap::AllocateContiguous, first fit from the bottom of the region), the last request is refused and the app stops
// in svcBreak before main().
// What changes here:
//   - the total is the smaller of the books and the memory the region really has free (its size minus what the kernel says is used in it), minus a
//     margin of 256 KiB; the region counts only when the app's process lives in the APPLICATION region (a .3dsx run as an applet does not);
//   - memory that is not the app's (a game whose process is still alive: the books carry far more than the app's own process; pages of the region
//     outside the books: Rosalina's plugin block until it frees it) makes the app WAIT, 100 ms at a time, for about 3 s, before it asks: the
//     occupant usually goes away (Rosalina waits up to 50 x 50 ms for the game's process to end before it frees the plugin's block; the 2026-11-06
//     dump of a New 3DS has 82048 KB of the game still in the books). When it does not go away, the app takes what there is;
//   - the linear heap is asked for FIRST: a refused linear request costs nothing (the kernel zeroes the pages only after it found the block), and
//     it gets the contiguous block before the heap's pages are taken from the top. A refused linear heap makes the next request ask for less linear
//     heap (3/4, down to kMinLinear) with the same total: the heap is not cut for a refusal that is about contiguity;
//   - a refused heap is not the end: everything is given back and the same request is not made again until what the kernel says has changed (or
//     1 s has passed: each try zeroes the pages it got), for about 3 s of real time; then the total is cut to 3/4 at every refusal, down to
//     kMinTotal, then on down to kFloorTotal;
//   - less than kMinTotal available after the 3 s is not a stop: the app asks for what there is, as libctru (and so 3DS Link 1.1.1) did; only a
//     refusal at kFloorTotal stops the app, and then the numbers are in the registers of the crash dump (PackBreak);
//   - the numbers are kept (Record) and the app adds them as one line (FormatStamp + FormatLine) to a log in its folder that keeps the last
//     kLogMaxLines starts (KeepLast).
// The split itself is libctru's: half and half, the heap at most 24 MiB while the linear heap is under 32 MiB, and the heap never past the 96 MiB of
// the heap area (that last cap is in libctru's sources after 2.7.0: 2.7.0 itself has no such cap and would fail on a New 3DS in memory mode 7).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace heap_plan {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kHeapSplitCap = 24u << 20;                 // HEAP_SPLIT_SIZE_CAP of libctru
constexpr uint32_t kLinearCap = 32u << 20;                    // LINEAR_HEAP_SIZE_CAP of libctru
constexpr uint32_t kHeapAreaSize = 0x0E000000u - 0x08000000u; // OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN: 96 MiB (heap_alloc.cpp checks it)
constexpr uint32_t kMargin = 256u << 10;                      // left free for what the books and the region's count do not show
// What the app uses: the linear heap holds the frame buffers, citro3d's command buffer, citro2d's 8192 quads, the icons (64x64 RGB565 textures)
// and the guide's pictures, about 3.3-3.5 MiB (the TitleDelivery logs say 28.7-29.1 MiB free of 32); the heap holds 2.1 MiB when the Titles
// Delivery starts on a New 2DS XL (1.1.1, 88.0 total, 85.9 free), 5.2 MiB in another log (88.0 total, 82.8 free), the soc buffer's 1 MiB
// included; the receiver needs at least 3072 blocks (4.4 MiB) at 75% of what is free; libcurl and mbedTLS take some more during an update.
// kMinTotal is where the cuts after a refusal stop first (12 MiB of heap and 12 of linear heap after the split: room for all of the above, the
// Titles Delivery included). It is NOT a floor on the memory there is: less than it available is asked for as it is (libctru did the same).
constexpr uint32_t kMinTotal = 24u << 20;
// The floor: below it the app does not start (its refusal stops it with the numbers in the crash dump). An ESTIMATE from the numbers above:
// libctru's split gives half to the linear heap, and under about 6.6 MiB the 3.3 MiB of linear heap the app is seen to use do not fit; 8 MiB
// leaves 4 MiB on each side (the heap's 2.1 MiB of the 1.1.1 log fit, the menus run; the Titles Delivery's receiver may not). A start under
// it would fail later anyway (gfx/citro2d cannot get their linear memory), in a place with no numbers.
constexpr uint32_t kFloorTotal = 8u << 20;
constexpr uint32_t kMinLinear = 12u << 20;     // the linear heap is not cut below this (half of kMinTotal)
constexpr uint32_t kForeignMargin = 1u << 20;  // the books may carry this much more than the app's own process before it counts as a game's
constexpr uint32_t kWaitStepMs = 100;          // one wait
constexpr uint32_t kWaitBudgetMs = 3000;       // how long (real time, the tries included) the app waits before it asks for less
constexpr uint32_t kRetrySameMs = 1000;        // a refused request is made again with the same counts only after this long
constexpr uint32_t kMaxAttempts = 64;          // never reached (a few tries in 3 s, then at most ~10 cuts): a guard against a loop
constexpr uint32_t kAppRegion = 0x100;         // MEMOP_REGION_APP: the memory type in a process's kernel flags (svcGetProcessInfo type 19)
constexpr uint32_t kUnknown = 0xFFFFFFFFu;
constexpr std::size_t kLogMaxLines = 50;          // 3DSLink-heap.log keeps the lines of the last 50 starts...
constexpr std::size_t kLogMaxBytes = 32u << 10;   // ...and never more than 32 KiB (a line is about 0.5 KiB)
constexpr std::size_t kLogReadBytes = 64u << 10;  // what is read of the old file (more than it can hold)

// What the kernel says, read at one moment.
struct Facts {
    int64_t limit;   // the limit of the commit of the process's resource limit (svcGetResourceLimitLimitValues)
    int64_t current; // what is committed now (svcGetResourceLimitCurrentValues)
    uint32_t region; // the size of the APPLICATION region (0 = not known or not the app's region: the books alone count)
    uint32_t used;   // what is used in it (svcGetSystemInfo type 0, parameter 1: the region's size minus its free pages)
    uint32_t own;    // the app's own process (svcGetProcessInfo type 2), before any allocation: its code, data and stack (0 = not known)
};

// The kernel's answers as they come (heap_alloc.cpp fills it; the test's kernel too), and what they become (FromRaw).
//   used:        svcGetSystemInfo(0, 1). Kernel 11.14 (k11 GetSystemInfo): applicationHeap.regionSize - free pages x 4 KiB, so EVERY page of the
//                region that is not free is in it, the books or not.
//   luma_region: Luma3DS's svcGetSystemInfo(0x10000, 0x80): the size of the APPLICATION region as the kernel's page heap has it (k11_extension
//                GetSystemInfo.c; Rosalina's memoryblock.c uses it for the same sum). A stock kernel answers an unknown type with 0 and success.
//   appmemalloc: APPMEMALLOC of the configuration page (osGetMemRegionSize). NOT the region's size on a New 3DS with Luma: Rosalina takes the
//                plugin's block off it (memoryblock.c:53-54) and only pm writes it again.
//   own:         svcGetProcessInfo(CUR_PROCESS_HANDLE, 2). Kernel 11.14 (k11 GetProcessInfo, case 2): the sum of the process's memory blocks with
//                the PRIVATE flag (0x2000) + its handle table + what the kernel allocated for it (shared memory it made). The commit the process
//                carries in its resource limit is GetTotalCommittedMemory: the same PRIVATE blocks, those in 0x1C000000-0x20000000 left out
//                (k11 KProcess::SetResourceLimits). So type 2 is never less than the app's own commit, and a few KiB more at most before main().
//   memtype:     svcGetProcessInfo(CUR_PROCESS_HANDLE, 19) = kernel flags & 0xF00: the region the process allocates from (k11 ControlMemory takes
//                the region from the same field). 0x100 = APPLICATION.
// An *_rc that is not 0 means the call failed (an emulator that does not know the type, a kernel without Luma).
struct Raw {
    int64_t limit = 0;
    int64_t current = 0;
    int32_t used_rc = -1;
    int64_t used = 0;
    int32_t luma_rc = -1;
    int64_t luma_region = 0;
    uint32_t appmemalloc = 0;
    int32_t own_rc = -1;
    int64_t own = 0;
    int32_t memtype_rc = -1;
    int64_t memtype = 0;
};

struct Reading {
    Facts facts;
    uint32_t region_luma; // 1: the region's size came from Luma; 0: from APPMEMALLOC
    uint32_t memtype;     // the process's memory type (0x100 APPLICATION, 0x200 SYSTEM, 0x300 BASE), kUnknown when the call failed
};

inline bool Plausible(int64_t v) {
    return v > 0 && v <= 0x10000000;
}

inline Reading FromRaw(const Raw& r) {
    Reading out{};
    out.facts.limit = r.limit;
    out.facts.current = r.current;
    out.memtype = r.memtype_rc == 0 ? static_cast<uint32_t>(r.memtype) & 0xF00u : kUnknown;
    // a process that does not live in the APPLICATION region (a .3dsx run as an applet): that region and the books of the APPLICATION resource
    // limit are not its own; its books alone count. Unknown: taken as APPLICATION (the CIA always is).
    const bool app = out.memtype == kUnknown || out.memtype == kAppRegion;
    const uint32_t used = r.used_rc == 0 && Plausible(r.used) ? static_cast<uint32_t>(r.used) : 0;
    uint32_t region = 0;
    if (r.luma_rc == 0 && Plausible(r.luma_region)) {
        region = static_cast<uint32_t>(r.luma_region);
        out.region_luma = 1;
    } else {
        region = r.appmemalloc;
        out.region_luma = 0;
    }
    if (used == 0 || !app) {
        region = 0; // (no count of the region: the books alone)
    }
    out.facts.region = region;
    out.facts.used = region != 0 ? used : 0;
    out.facts.own = app && r.own_rc == 0 && Plausible(r.own) ? static_cast<uint32_t>(r.own) : 0;
    return out;
}

inline uint32_t PageDown(uint64_t v) {
    if (v > 0xFFFFFFFFull) {
        v = 0xFFFFFFFFull;
    }
    return static_cast<uint32_t>(v) & ~(kPage - 1);
}

// What libctru 2.7.0 calls "remaining": the books, page aligned (0 when the books are over their limit).
inline uint32_t BooksFree(const Facts& f) {
    return f.limit > f.current ? PageDown(static_cast<uint64_t>(f.limit - f.current)) : 0;
}

// The region's really free memory; 0xFFFFFFFF when the region's size is not known.
inline uint32_t RegionFree(const Facts& f) {
    if (f.region == 0) {
        return 0xFFFFFFFFu;
    }
    return f.region > f.used ? PageDown(f.region - f.used) : 0;
}

// The total the app may ask for now: the smaller of the two, minus the margin.
inline uint32_t Available(const Facts& f) {
    const uint32_t books = BooksFree(f);
    const uint32_t region = RegionFree(f);
    const uint32_t m = books < region ? books : region;
    return m > kMargin ? m - kMargin : 0;
}

// Memory that is not the app's, which usually goes away by itself.
// In the books: the APPLICATION resource limit carries one application, so before any allocation `current` is the app's own commit; far more
// than that is a game whose KProcess has not been finalized yet (KProcess::Finalize gives its commit back).
inline bool ForeignInBooks(const Facts& f) {
    return f.own != 0 && f.current > static_cast<int64_t>(f.own) + kForeignMargin;
}
// In the region, outside the books: used pages that no commit carries (Rosalina's plugin block of a New 3DS, svcControlMemoryUnsafe). The two pages
// of the plugin's alias stay under the margin and do not make the app wait.
inline bool ForeignInRegion(const Facts& f) {
    if (f.region == 0) {
        return false;
    }
    const uint32_t books = BooksFree(f);
    const uint32_t region = RegionFree(f);
    return books > region && books - region > kMargin;
}
inline uint32_t Foreign(const Facts& f) {
    return (ForeignInBooks(f) ? 1u : 0u) | (ForeignInRegion(f) ? 2u : 0u);
}

inline bool SameFacts(const Facts& a, const Facts& b) {
    return a.limit == b.limit && a.current == b.current && a.region == b.region && a.used == b.used && a.own == b.own;
}

struct Split {
    uint32_t heap;
    uint32_t linear;
    bool ok; // false: the fixed sizes do not fit in the total (libctru stops there)
};

// libctru's split of `total` (its "remaining"). fixed_heap / fixed_linear are __ctru_heap_size / __ctru_linear_heap_size as the program set them
// (0 = worked out here).
inline Split SplitTotal(uint32_t total, uint32_t fixed_heap, uint32_t fixed_linear) {
    Split s{fixed_heap, fixed_linear, true};
    if (static_cast<uint64_t>(fixed_heap) + fixed_linear > total) {
        s.ok = false;
        return s;
    }
    const bool auto_heap = fixed_heap == 0;
    const bool auto_linear = fixed_linear == 0;
    if (auto_heap && auto_linear) {
        // half and half, rounded in favour of the heap
        s.linear = (total / 2) & ~(kPage - 1);
        s.heap = total - s.linear;
        // a heap past its cap gives the rest to the linear heap...
        if (s.heap > kHeapSplitCap) {
            s.heap = kHeapSplitCap;
            s.linear = total - s.heap;
            // ...unless the linear heap goes past its own cap: then the heap takes it
            if (s.linear > kLinearCap) {
                s.linear = kLinearCap;
                s.heap = total - s.linear;
            }
        }
    } else if (auto_heap) {
        s.heap = total - s.linear;
    } else if (auto_linear) {
        s.linear = total - s.heap;
    }
    if (s.heap > kHeapAreaSize) {
        if (auto_linear) {
            s.linear += s.heap - kHeapAreaSize;
        }
        s.heap = kHeapAreaSize;
    }
    return s;
}

// libctru's split with the linear heap held under linear_cap (after a refused linear heap): what the linear heap gives up goes to the heap, which
// stays inside the heap area (past it, the total is smaller). A linear size the program fixed is never changed.
inline Split SplitCapped(uint32_t total, uint32_t fixed_heap, uint32_t fixed_linear, uint32_t linear_cap) {
    Split s = SplitTotal(total, fixed_heap, fixed_linear);
    if (!s.ok || fixed_linear != 0 || s.linear <= linear_cap) {
        return s;
    }
    const uint32_t extra = s.linear - linear_cap;
    s.linear = linear_cap;
    if (fixed_heap == 0) {
        const uint64_t heap = static_cast<uint64_t>(s.heap) + extra;
        s.heap = heap > kHeapAreaSize ? kHeapAreaSize : static_cast<uint32_t>(heap);
    }
    return s;
}

// The sequence of the requests.
enum class Action { Try, Wait, Panic };

// Which allocation the kernel refused (heap_alloc.cpp asks for the linear heap first, then the heap).
enum class Part : uint32_t { None = 0, Heap = 1, Linear = 2 };

struct Step {
    Action action;
    uint32_t total;   // Try: the total to ask for
    uint32_t wait_ms; // Wait: how long
};

struct State {
    uint32_t attempts;     // requests made
    uint32_t waited_ms;    // time slept
    uint32_t elapsed_ms;   // real time since the first reading (the tries included): the budget counts this
    uint32_t shrinks;      // cuts of the total to 3/4
    uint32_t linear_cuts;  // cuts of the linear heap to 3/4
    uint32_t failed_total; // the total of the last refused request of the waiting phase (0 = none)
    uint32_t shrink_cap;   // the shrinking phase: the most the next request asks for
    uint32_t linear_cap;   // the most the linear heap may be (kUnknown = libctru's split as it is)
    uint32_t refused_ms;   // elapsed_ms of the reading the last refused request was made on
    Facts refused_at;      // that reading
    bool refused;          // a request of the waiting phase was refused (refused_at / refused_ms are set)
    bool must_wait;        // a request was refused and no wait came after it yet
    bool shrinking;        // the waiting time is over: every refusal cuts the total
    bool dead;             // a request at the minimum was refused
};

inline State Start() {
    State s{};
    s.linear_cap = kUnknown;
    return s;
}

inline uint32_t Shrink(uint32_t total, uint32_t min_total) {
    const uint32_t cut = PageDown(static_cast<uint64_t>(total) * 3 / 4);
    return cut > min_total ? cut : min_total;
}

// The smallest request, the one whose refusal stops the app: kFloorTotal, or the sizes the program fixed when they are more than kMinTotal
// (libctru stops when they do not fit: there is nothing smaller to ask for).
inline uint32_t Floor(uint32_t min_total) {
    return min_total > kMinTotal ? min_total : kFloorTotal;
}

// The cut after a refusal of `total`: 3/4, landing on min_total first; under min_total on down to the floor.
inline uint32_t Cut(uint32_t total, uint32_t min_total) {
    return Shrink(total, total > min_total ? min_total : Floor(min_total));
}

// What to do next, with `f` read just now and s.elapsed_ms set. min_total is kMinTotal, or more when the program fixed bigger sizes.
inline Step Next(State& s, const Facts& f, uint32_t min_total) {
    if (s.dead || s.attempts >= kMaxAttempts) {
        return Step{Action::Panic, 0, 0};
    }
    const uint32_t available = Available(f);
    if (!s.shrinking) {
        if (s.elapsed_ms < kWaitBudgetMs) {
            // the same request on the same counts is refused again: it is made again only after kRetrySameMs
            const bool same = s.refused && SameFacts(f, s.refused_at) && s.elapsed_ms - s.refused_ms < kRetrySameMs;
            if (s.must_wait || same || available < min_total || Foreign(f) != 0) {
                return Step{Action::Wait, 0, kWaitStepMs};
            }
            ++s.attempts;
            return Step{Action::Try, available, 0};
        }
        s.shrinking = true;
        s.shrink_cap = s.failed_total != 0 ? Cut(s.failed_total, min_total) : available;
        if (s.failed_total != 0) {
            ++s.shrinks;
        }
    }
    // what there is, even under min_total (libctru asked for all of the books, whatever they were); never under the floor
    uint32_t total = s.shrink_cap < available ? s.shrink_cap : available;
    const uint32_t floor = Floor(min_total);
    if (total < floor) {
        total = floor; // (the counts may be wrong: the floor is asked for once, its refusal stops the app)
    }
    ++s.attempts;
    return Step{Action::Try, total, 0};
}

// A request of `total` (`linear` of it the linear heap) was refused, on the reading `f`. linear_auto: the program did not fix the linear size.
inline void Failed(State& s, uint32_t total, uint32_t linear, Part part, bool linear_auto, const Facts& f, uint32_t min_total) {
    if (part == Part::Linear && linear_auto && linear > kMinLinear) {
        // no single free block that big: less linear heap, the same total (the heap takes the rest), at once (the refusal cost nothing)
        const uint32_t cut = PageDown(static_cast<uint64_t>(linear) * 3 / 4);
        s.linear_cap = cut > kMinLinear ? cut : kMinLinear;
        ++s.linear_cuts;
        return;
    }
    if (s.shrinking) {
        if (total <= Floor(min_total)) {
            s.dead = true;
        } else {
            s.shrink_cap = Cut(total, min_total);
            ++s.shrinks;
        }
    } else {
        s.failed_total = total;
        s.must_wait = true;
        s.refused = true;
        s.refused_at = f;
        s.refused_ms = s.elapsed_ms;
    }
}

inline void Waited(State& s, uint32_t ms) {
    s.waited_ms += ms;
    s.must_wait = false;
    if (!s.shrinking) {
        s.linear_cap = kUnknown; // (after a wait the region may have a bigger block again: the cuts of the linear heap start over, they cost nothing)
    }
}

// What happened, for the log line. Plain data: heap_alloc.cpp keeps one in .bss, filled before main().
struct Record {
    Facts first;             // the first reading
    Facts last;              // the reading of the last request
    uint32_t appmemalloc;    // APPMEMALLOC of the configuration page at the last reading (what osGetMemRegionSize says)
    uint32_t region_luma;    // 1: the region's size came from Luma's svcGetSystemInfo(0x10000, 0x80); 0: from APPMEMALLOC
    uint32_t memtype;        // the process's memory type (kUnknown: the call failed)
    uint32_t foreign_first;  // Foreign() of the first reading: 1 the books, 2 the region
    uint32_t foreign_last;   // Foreign() of the last reading
    uint32_t total;          // the total given (heap + linear)
    uint32_t heap;
    uint32_t linear;
    uint32_t attempts;
    uint32_t waited_ms;
    uint32_t elapsed_ms;
    uint32_t shrinks;
    uint32_t linear_cuts;
    int32_t last_error;      // the result of the last refused request (0 = none)
    uint32_t refused_part;   // which allocation that was (Part)
    uint32_t refused_heap;   // and its sizes
    uint32_t refused_linear;
    uint32_t ok;             // 1: done (0: the app stops)
    uint32_t start_ms;       // env.NowMs() at the start: the time since the console started (svcGetSystemTick), ms
};

// The loop itself, with the kernel behind `env`:
//   Raw env.Read();
//   Part env.Try(uint32_t heap, uint32_t linear, int32_t* error);  (the linear heap first, then the heap; on a refusal nothing stays allocated)
//   void env.Sleep(uint32_t ms);
//   uint32_t env.NowMs();                                           (a clock in ms, wrapping is fine)
// True when the memory was given (rec says how); false means: stop the app.
template <class Env>
bool Run(Env& env, uint32_t fixed_heap, uint32_t fixed_linear, Record& rec) {
    const uint64_t fixed = static_cast<uint64_t>(fixed_heap) + fixed_linear;
    const uint32_t min_total = fixed > kMinTotal ? static_cast<uint32_t>(fixed > 0xFFFFFFFFull ? 0xFFFFFFFFull : fixed) : kMinTotal;
    State s = Start();
    const uint32_t t0 = env.NowMs();
    rec.start_ms = t0;
    bool first = true;
    for (;;) {
        const Raw raw = env.Read();
        const Reading rd = FromRaw(raw);
        const Facts& f = rd.facts;
        s.elapsed_ms = env.NowMs() - t0;
        if (first) {
            rec.first = f;
            rec.foreign_first = Foreign(f);
            first = false;
        }
        rec.last = f;
        rec.foreign_last = Foreign(f);
        rec.appmemalloc = raw.appmemalloc;
        rec.region_luma = rd.region_luma;
        rec.memtype = rd.memtype;
        const Step step = Next(s, f, min_total);
        rec.attempts = s.attempts;
        rec.waited_ms = s.waited_ms;
        rec.elapsed_ms = s.elapsed_ms;
        rec.shrinks = s.shrinks;
        rec.linear_cuts = s.linear_cuts;
        if (step.action == Action::Panic) {
            rec.ok = 0;
            return false;
        }
        if (step.action == Action::Wait) {
            env.Sleep(step.wait_ms);
            Waited(s, step.wait_ms);
            rec.waited_ms = s.waited_ms;
            continue;
        }
        const Split split = SplitCapped(step.total, fixed_heap, fixed_linear, s.linear_cap);
        Part part = Part::Heap; // (fixed sizes that do not fit: refused without asking, as libctru)
        int32_t error = 0;
        if (split.ok) {
            part = env.Try(split.heap, split.linear, &error);
            if (part == Part::None) {
                rec.total = split.heap + split.linear;
                rec.heap = split.heap;
                rec.linear = split.linear;
                rec.ok = 1;
                return true;
            }
        }
        rec.last_error = error;
        rec.refused_part = static_cast<uint32_t>(part);
        rec.refused_heap = split.heap;
        rec.refused_linear = split.linear;
        Failed(s, step.total, split.linear, part, fixed_linear == 0, f, min_total);
        rec.shrinks = s.shrinks;
        rec.linear_cuts = s.linear_cuts;
    }
}

// The numbers left in the crash dump when the app stops (svcBreak in heap_alloc.cpp). Luma3DS's dump of a svcBreak has the registers r0-r12 as
// they were at the svc instruction (k11_extension fatalExceptionHandlers.s, prefetchAbortHandler: it reloads them from the svc frame at the top of
// the thread's kernel stack, the frame KDebug::GetThreadContextInSvc reads) and the stack from sp to the end of its page.
//   r0  = 0 (USERBREAK_PANIC, as libctru)
//   r1  = w[0]  = 0x50414548, the bytes "HEAP": this break and not libctru's
//   r2  = w[1]  = the last error (Result of the refused svcControlMemory)
//   r3  = w[2]  = attempts | cuts of the total << 8 | cuts of the linear heap << 16 | flags << 24
//                 (flags: 1 the books carried a game at the first reading, 2 the region had pages outside the books at the first reading,
//                  4 and 8 the same at the last reading, 16 the region's size came from Luma, 32 the refused part was the linear heap)
//   r4  = w[3]  = real time spent, ms
//   r5  = w[4]  = first reading: limit, KiB         r6  = w[5]  = first reading: current, KiB
//   r7  = w[6]  = first reading: region, KiB        r8  = w[7]  = first reading: used, KiB
//   r9  = w[8]  = first reading: own process, KiB   r10 = w[9]  = last reading: current, KiB
//   r12 = w[10] = last reading: used, KiB
//   only on the stack (the 16 words start with the bytes "HEAP"): w[11] refused heap KiB, w[12] refused linear KiB, w[13] memory type,
//   w[14] APPMEMALLOC KiB, w[15] last reading: region KiB.
constexpr uint32_t kBreakMagic = 0x50414548u; // "HEAP" in memory (little endian)
struct BreakWords {
    uint32_t w[16];
};

inline uint32_t KiBOf(int64_t v) {
    return v <= 0 ? 0 : static_cast<uint32_t>(v / 1024);
}

inline BreakWords PackBreak(const Record& r) {
    BreakWords b{};
    const auto low8 = [](uint32_t v) { return v > 0xFF ? 0xFFu : v; };
    uint32_t flags = 0;
    flags |= (r.foreign_first & 1u) ? 1u : 0u;
    flags |= (r.foreign_first & 2u) ? 2u : 0u;
    flags |= (r.foreign_last & 1u) ? 4u : 0u;
    flags |= (r.foreign_last & 2u) ? 8u : 0u;
    flags |= r.region_luma ? 16u : 0u;
    flags |= r.refused_part == static_cast<uint32_t>(Part::Linear) ? 32u : 0u;
    b.w[0] = kBreakMagic;
    b.w[1] = static_cast<uint32_t>(r.last_error);
    b.w[2] = low8(r.attempts) | low8(r.shrinks) << 8 | low8(r.linear_cuts) << 16 | flags << 24;
    b.w[3] = r.elapsed_ms;
    b.w[4] = KiBOf(r.first.limit);
    b.w[5] = KiBOf(r.first.current);
    b.w[6] = r.first.region / 1024;
    b.w[7] = r.first.used / 1024;
    b.w[8] = r.first.own / 1024;
    b.w[9] = KiBOf(r.last.current);
    b.w[10] = r.last.used / 1024;
    b.w[11] = r.refused_heap / 1024;
    b.w[12] = r.refused_linear / 1024;
    b.w[13] = r.memtype;
    b.w[14] = r.appmemalloc / 1024;
    b.w[15] = r.last.region / 1024;
    return b;
}

// The line the app writes (KiB; no '\n' at the end of `out`, the caller adds it). Returns what snprintf returns.
inline int FormatLine(const Record& r, char* out, std::size_t size) {
    auto kib = [](int64_t v) { return static_cast<long long>(v / 1024); };
    auto ul = [](uint32_t v) { return static_cast<unsigned long>(v); };
    const char* part = r.refused_part == static_cast<uint32_t>(Part::Linear) ? "linear" : r.refused_part == static_cast<uint32_t>(Part::Heap) ? "heap" : "none";
    return std::snprintf(out, size,
                         "heap at start: %s total %lu KiB = heap %lu + linear %lu, attempts %lu, waited %lu ms, elapsed %lu ms, cuts %lu, "
                         "linear cuts %lu, last error 0x%08lX (%s: heap %lu linear %lu) | "
                         "first: limit %lld current %lld region %lu used %lu own %lu KiB, foreign %lu | "
                         "last: limit %lld current %lld region %lu used %lu own %lu KiB, foreign %lu | "
                         "region from %s, APPMEMALLOC %lu KiB, memtype 0x%lX",
                         !r.ok ? "FAILED," : r.total < kMinTotal ? "ok (reduced: under the minimum)," : "ok,", ul(r.total / 1024), ul(r.heap / 1024), ul(r.linear / 1024), ul(r.attempts), ul(r.waited_ms),
                         ul(r.elapsed_ms), ul(r.shrinks), ul(r.linear_cuts), ul(static_cast<uint32_t>(r.last_error)), part, ul(r.refused_heap / 1024),
                         ul(r.refused_linear / 1024), kib(r.first.limit), kib(r.first.current), ul(r.first.region / 1024), ul(r.first.used / 1024),
                         ul(r.first.own / 1024), ul(r.foreign_first), kib(r.last.limit), kib(r.last.current), ul(r.last.region / 1024),
                         ul(r.last.used / 1024), ul(r.last.own / 1024), ul(r.foreign_last), r.region_luma ? "Luma" : "APPMEMALLOC",
                         ul(r.appmemalloc / 1024), ul(r.memtype));
}

// The start of a line of the log: the console's date and time as its clock has them (std::time: the clock the user set, which may be years off:
// some test consoles keep 2011 on purpose) and the time since the console started (Record::start_ms: it orders the starts of one session even
// when the clock is wrong). "2026-11-06 21:04:05, boot +123.456 s | ". Returns what snprintf returns.
inline int FormatStamp(int64_t unix_seconds, uint32_t boot_ms, char* out, std::size_t size) {
    int64_t days = unix_seconds / 86400;
    int64_t secs = unix_seconds % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    // civil date from days since 1970-01-01 (the proleptic Gregorian calendar, H. Hinnant's civil_from_days)
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    return std::snprintf(out, size, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld, boot +%lu.%03lu s | ", static_cast<long long>(year),
                         static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(secs / 3600),
                         static_cast<long long>(secs / 60 % 60), static_cast<long long>(secs % 60), static_cast<unsigned long>(boot_ms / 1000),
                         static_cast<unsigned long>(boot_ms % 1000));
}

// The log's new text: the old text with `line` added at the end, then the oldest whole lines dropped until at most max_lines lines and max_bytes
// bytes are left (the newest line always stays). An old text cut in the middle of a line gets its '\n' first.
inline std::string KeepLast(const std::string& old, const std::string& line, std::size_t max_lines, std::size_t max_bytes) {
    std::string all = old;
    if (!all.empty() && all.back() != '\n') {
        all += '\n';
    }
    all += line;
    all += '\n';
    std::size_t lines = 0;
    for (char c : all) {
        lines += c == '\n' ? 1 : 0;
    }
    std::size_t start = 0;
    while (lines > 1 && (lines > max_lines || all.size() - start > max_bytes)) {
        start = all.find('\n', start) + 1;
        --lines;
    }
    return all.substr(start);
}

} // namespace heap_plan
