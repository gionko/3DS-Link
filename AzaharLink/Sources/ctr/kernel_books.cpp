// The kernel's books of the game's process (see Includes/ctr/kernel_books.hpp and Includes/azl/books_math.hpp).
//
// A block outside the game's budget (a late title on a console where it has left nothing: Monster Hunter XX on an Old 3DS holds 80 MiB of an 80 MiB region,
// budget included; the gate's page of the svc hook) is charged to nobody, but it is a private block all the same: when the process ends the kernel adds up its
// private blocks and gives the sum back to the budget, and a sum above the books gives back NOTHING: the next game
// would find no budget until a reboot. Freeing the block at the end would not do: after HOME on an Old 3DS the plugin is swapped out, and a game closed there
// gets its EXIT only once Luma has mapped the plugin's 5 MiB back (PluginLoader__HandleKernelEvent), and nothing runs when a process is killed. So the books are put right when the block is
// taken, and any end balances by itself. They may then stand above the limit by the block: such a game has no budget left anyway.
//
// KResourceLimit (Luma's kernel.h): KAutoObject (vtable, refcount), s32 limit[10], s32 current[10]. The offsets are checked on the live object against
// what the kernel's own calls say: anything else and nothing is written.
//
// How (line numbers: the Kernel11 11.14 decompilation, k11_latest_n3ds.cpp; Luma3DS's sources):
//   * The kernel's mutex is never taken here. The kernel changes current[] only under the KResourceLimit's own KLightMutex
//     (KResourceLimit::Reserve 25186-25228, ::Release 32146-32188), and a mutex taken from outside cannot be let go the kernel's way: a
//     waiter on the other core (KLightMutex::LockImpl 32610, its wait 32640-32662) is taken off its CPU by the scheduler (35780-35880) until
//     KLightMutex::UnlockImpl (32688-32705) sends that core the scheduler's interrupt, which a plain store of 0 does not. So current[]
//     moves through the kernel's own calls first:
//       - current += X: svcControlMemory MEMOP_ALLOC of X (Luma3DS hooks svc 0x01 only to pass isLoader, k11_extension svc.c:68 and
//         svc/ControlMemory.c:28-32): for an ALLOC of a process that is not the loader the kernel calls KProcess::ReserveResource(COMMIT, X)
//         (13863; KProcess::ReserveResource 22984-22993 is KResourceLimit::Reserve on the process's own object, the APPLICATION category's,
//         the one FindBooks finds) BEFORE KProcessPageTable::ControlMemory (13866), refuses with 0xC860180A when current + X is above the limit
//         (13890; Reserve 25211), and releases X itself when the pages are not had (13882-13884). The block is then freed with Luma3DS's svc
//         0xA3 MEMOP_FREE, which unmaps it with doControlMemory and nothing else (k11_extension svc/ControlMemoryUnsafe.c:42-46): no Release,
//         the X stays on current (books::ReserveThroughKernel).
//       - current -= X: a block of X taken with svc 0xA3 (ControlMemoryUnsafe.c:47-73: pages, no Reserve) and freed with svcControlMemory
//         MEMOP_FREE, after which the kernel calls KProcess::ReleaseResource(COMMIT, X) whoever charged it (13876-13877, "bug here");
//         KResourceLimit::Release does nothing when X is above current (32171), never the case here: X was charged first
//         (books::ReleaseThroughKernel).
//     KProcessPageTable::ControlMemory (20566-20735) and KPageTable::Operate (31699) touch no books (the commit is reserved or released
//     only at 13863, 13877, 13884, 17524, 17583, 18492, 18511, 41499 and 45145): svc 0xA3's FREE (doControlMemory, the 8 arguments of
//     KPageTable::Operate) and the kernel's FREE (QueryInfo of the deallocatable state, then KPageTable::Operate, 20606-20629, 20717) unmap
//     and give the pages back the same way; only the Release around them differs. Rosalina frees its own blocks, linear ones
//     included, with svc 0xA3's FREE (sysmodules/rosalina/source/plugin/memoryblock.c:69, 114).
//     The block of the reservation is MEMOP_ALLOC_LINEAR at an address the kernel chooses (addr0 = 0; KProcessPageTable::ControlMemory 20631-20660:
//     contiguous pages of the process's region, the address from the physical one): nothing to look for, and no address of the game's
//     address space is taken even for a moment (a block of the heap area at an address of our choosing could be the very place a game thread
//     asks for at that moment, and its svcControlMemory would fail). The pages are back a few svcs later; the game's heap and linear heap, if
//     they are not there yet, find the region as it was. The cost: the pages must be contiguous, and they come from the process's own region
//     (svcControlMemory takes no other for an application, 13859): a game that holds the whole region leaves none (Monster Hunter XX on an
//     Old 3DS: limit = region = 80 MiB, all of it the game's), and the kernel answers 0xD86007F3 (books::kNoPages). The block of the Release
//     (svc 0xA3: the SYSTEM region whole, then a page at a time in SYSTEM, APPLICATION, BASE, books::kReleaseRegions; svcControlMemory's FREE
//     gives pages back by their physical address whatever the region, KMemoryManager::FreeContiguousLocked ~33304) needs an address that
//     svcControlMemory's FREE takes: the heap area 0x08000000-0x10000000 (13928-13937), at the top of its highest free span (FreeHeapSpan),
//     away from where the game's heap grows. A game thread that maps the same address between svcQueryMemory and svc 0xA3 cannot end up
//     under our block, nor ours under its: svc 0xA3's ALLOC maps with doControlMemory (ControlMemoryUnsafe.c:70, updateFlags 0), which is
//     KPageTable::Operate (books_math.hpp, kNoPages, says how Luma3DS finds it), and Operate with updateFlags 0 takes the page table's
//     KLightMutex (31839-31848) and, under it, looks up the block that holds the address: unless it is FREE or RESERVED and covers the whole
//     range, it answers 0xD900060C and maps nothing (31902-31948), before it takes a page (31975-32025). One of the two ALLOCs fails
//     cleanly. What it costs: if ours loses that race at the step of one page, ReleaseThroughKernel answers 0xD900060C (not kNoPages: no
//     region change, no fallback) and the Uncharge leaves what it did not release on current, X too many until a reboot, never too few.
//     It needs a game that asks for an explicit address at the top of the highest free span of its heap area, right then, twice; the steps
//     run only when a late block was not had (Uncharge).
//   * current[kCommit] is written here only when the kernel had no pages for its own call (books::ChargeOrWrite, books::UnchargeOrWrite:
//     kNoPages from the ALLOC, nothing else): KernelChargeBooks, a load, an add and a store in kernel mode with the interrupts off, outside
//     the mutex, the way 1.0.13-1.1.1 charged every late block (there with the interrupts on, Sources/ctr/soc.cpp of v1.1.1). Without it
//     an Old 3DS running Monster Hunter XX would have no socket block, and no network (the issues 2 and 3 are that game on that console).
//     The window it leaves: a Reserve or a Release of the other core between the load and the store (one of the two lost: a few instructions,
//     once per game, on an Old 3DS whose game holds the whole region).
//   * limits[kCommit] is written here, in kernel mode (svcCustomBackdoor; Luma3DS's svcHandler turns the interrupts on before the backdoor's
//     function, k11_extension/source/svcHandler.s `cpsie i`, so they go off around the store), with no mutex: the kernel writes limits[] only
//     in KResourceLimit::SetLimitValue (33403, under the mutex), from svcSetResourceLimitValues (16170) and for the CPU time (24644, 39247:
//     another slot, a word of its own), and pm writes the applications' commit limit only at boot and on a New 3DS in memtype 6 (the launch of
//     a title in Old 3DS mode, before its process exists, and the end of every application: Luma3DS pm launch.c:231, process_monitor.c:33),
//     never while a game runs. KResourceLimit::Reserve reads it once, under its mutex (25211): it sees the word before or after our store,
//     whole. No other writer while we write it, and nothing ever waits on it.
//   * Raise (the gate's page, EarlyInit, before the game's first instruction): limit += X, then current += X through the kernel; refused, the
//     limit goes back. Charge (the late socket block, the game running with its budget spent): the books read (KernelReadBooks), limit += X
//     and what current stands above the limit (books::ChargeLift: the reservation is never refused for the books alone), current += X
//     through the kernel, the limit back down by the same; a game thread that takes the lifted room first makes the reservation fail
//     (0xC860180A, the books as they were, with the game's block on them: no fallback, a line of its own); the kernel with no pages for
//     the reservation: current += X written (KernelChargeBooks). Uncharge: current -= X through the kernel;
//     no region with a page: written. Settle: limit -= X. All of them under BooksGuard (one of the plugin's threads at a time);
//     the keep-alive thread only tries it (TrySettleOwedBooks).
//   Policy pins RaiseBooks to TakeGatePage and ChargeBooks / UnchargeBooks to the one place in soc.cpp.
#include "ctr/kernel_books.hpp"

#include <3ds.h>

#include <cstring>

#include "azl/books_math.hpp"
#include "azl/log.hpp"
#include "azl/soc_plan.hpp"
#include "csvc.h"

namespace azl {
namespace ctr {

namespace {

static_assert(books::kCommit == RESLIMIT_COMMIT && books::kThread == RESLIMIT_THREAD && books::kEvent == RESLIMIT_EVENT &&
                  books::kMutex == RESLIMIT_MUTEX,
              "the slots of libctru's ResourceLimitType");
static_assert(books::kCheckedNames[0] == books::kCommit, "ReadBooks takes the commit's limit from the first slot read");
static_assert(books::kPageBytes == 0x1000, "the kernel's page");

struct KernelBooks {
    volatile s32* limits;
    volatile s32* current;
    s32 read_limits[books::kChecked];
    s32 read_commit;
    s32 add;
};
KernelBooks g_books;
bool g_books_found = false;
LightLock g_books_lock; // (the amount for the kernel-mode functions is in g_books.add: one caller at a time)

struct BooksGuard {
    BooksGuard() {
        static bool ready = false;
        if (!ready) { // (the first call is the gate's page, in EarlyInit, before any other thread of the plugin)
            LightLock_Init(&g_books_lock);
            ready = true;
        }
        LightLock_Lock(&g_books_lock);
    }
    ~BooksGuard() {
        LightLock_Unlock(&g_books_lock);
    }
};

__attribute__((always_inline)) inline u32 InterruptsOff() {
    u32 cpsr;
    asm volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr)::"memory");
    return cpsr;
}

__attribute__((always_inline)) inline void InterruptsBack(u32 cpsr) {
    asm volatile("msr cpsr_c, %0" ::"r"(cpsr) : "memory");
}

// In kernel mode (svcCustomBackdoor): these touch the words above and call nothing (books_math.hpp's arithmetic is always inlined).
__attribute__((noinline)) s32 KernelReadBooks() {
    for (int i = 0; i < books::kChecked; ++i) {
        g_books.read_limits[i] = g_books.limits[books::kCheckedNames[i]];
    }
    g_books.read_commit = g_books.current[books::kCommit];
    return 0;
}

__attribute__((noinline)) s32 KernelLiftBooks() {
    const u32 cpsr = InterruptsOff();
    books::Lift(g_books.limits, g_books.add);
    InterruptsBack(cpsr);
    return 0;
}

__attribute__((noinline)) s32 KernelSettleBooks() {
    const u32 cpsr = InterruptsOff();
    books::Settle(g_books.limits, g_books.add);
    InterruptsBack(cpsr);
    return 0;
}

// current[kCommit] += add (add < 0: -=), only from ConsoleKernel::WriteCurrent, the fallback of books::ChargeOrWrite / UnchargeOrWrite.
__attribute__((noinline)) s32 KernelChargeBooks() {
    const u32 cpsr = InterruptsOff();
    books::Charge(g_books.current, g_books.add);
    InterruptsBack(cpsr);
    return 0;
}

// The top of the highest free span of the heap area that holds `bytes` (0: none), where svc 0xA3 takes a block that svcControlMemory's
// FREE then frees (the kernel takes a FREE only there or in the linear area).
u32 FreeHeapSpan(u32 bytes) {
    u32 best = 0;
    u32 address = socplan::kRegionBase;
    for (int i = 0; i < 64 && address < socplan::kRegionEnd; ++i) {
        MemInfo info{};
        PageInfo page;
        if (R_FAILED(svcQueryMemory(&info, &page, address)) || info.size == 0) {
            break;
        }
        const u64 end64 = static_cast<u64>(info.base_addr) + info.size;
        const u32 end = end64 > socplan::kRegionEnd ? socplan::kRegionEnd : static_cast<u32>(end64);
        const u32 start = info.base_addr < socplan::kRegionBase ? socplan::kRegionBase : info.base_addr;
        if (info.state == MEMSTATE_FREE && end > start && end - start >= bytes) {
            best = end - bytes;
        }
        address = end;
    }
    return best;
}

// The kernel's calls of books::ReserveThroughKernel and the steps after it (Includes/azl/books_math.hpp). Under BooksGuard.
struct ConsoleKernel {
    void Read(s32* limit, s32* current) {
        svcCustomBackdoor(reinterpret_cast<void*>(KernelReadBooks));
        *limit = g_books.read_limits[0]; // (kCheckedNames[0] is the commit)
        *current = g_books.read_commit;
    }
    static void MoveLimit(s32 (*write)(), u32 bytes) {
        g_books.add = static_cast<s32>(bytes);
        svcCustomBackdoor(reinterpret_cast<void*>(write));
    }
    void Lift(u32 bytes) {
        MoveLimit(KernelLiftBooks, bytes);
    }
    void Lower(u32 bytes) {
        MoveLimit(KernelSettleBooks, bytes);
    }
    s32 AllocCharged(u32 bytes, u32* address) {
        return svcControlMemory(address, 0, 0, bytes, MEMOP_ALLOC_LINEAR, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
    }
    s32 FreeUncharged(u32 address, u32 bytes) {
        u32 ignored = 0;
        return svcControlMemoryUnsafe(&ignored, address, bytes, MEMOP_FREE, static_cast<MemPerm>(0));
    }
    s32 AllocUncharged(u32 address, u32 bytes, books::Region region) {
        const u32 from = region == books::Region::System        ? MEMOP_REGION_SYSTEM
                         : region == books::Region::Application ? MEMOP_REGION_APP
                                                                : MEMOP_REGION_BASE;
        u32 out = 0;
        return svcControlMemoryUnsafe(&out, address, bytes, static_cast<MemOp>(MEMOP_ALLOC | from),
                                      static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
    }
    s32 FreeCharged(u32 address, u32 bytes) {
        u32 ignored = 0;
        return svcControlMemory(&ignored, address, 0, bytes, MEMOP_FREE, static_cast<MemPerm>(0));
    }
    u32 FreeSpan(u32 bytes) {
        return FreeHeapSpan(bytes);
    }
    void WriteCurrent(s32 bytes) {
        g_books.add = bytes;
        svcCustomBackdoor(reinterpret_cast<void*>(KernelChargeBooks));
    }
};

} // namespace

bool FindBooks() {
    const BooksGuard guard;
    Handle h = 0;
    if (R_FAILED(svcGetResourceLimit(&h, CUR_PROCESS_HANDLE))) {
        return false;
    }
    u32 object = 0;
    char name[32] = "";
    bool ok = R_SUCCEEDED(svcTranslateHandle(&object, name, h)) && std::strcmp(name, "KResourceLimit") == 0 && object >= 0x80000000;
    s64 limits[books::kChecked] = {};
    s64 commit = -1;
    if (ok) {
        ResourceLimitType names[books::kChecked];
        for (int i = 0; i < books::kChecked; ++i) {
            names[i] = static_cast<ResourceLimitType>(books::kCheckedNames[i]);
        }
        ok = R_SUCCEEDED(svcGetResourceLimitLimitValues(limits, h, names, books::kChecked)) &&
             R_SUCCEEDED(svcGetResourceLimitCurrentValues(&commit, h, names, 1));
    }
    if (ok) {
        g_books.limits = reinterpret_cast<volatile s32*>(object + books::kLimitsOffset);
        g_books.current = reinterpret_cast<volatile s32*>(object + books::kCurrentOffset);
        svcCustomBackdoor(reinterpret_cast<void*>(KernelReadBooks));
        ok = books::SameAsTheCalls(g_books.read_limits, g_books.read_commit, limits, commit);
    }
    svcCloseHandle(h);
    g_books_found = g_books_found || ok;
    Log("books: the kernel's books of this process: %s (object 0x%08lx, class '%s', commit %ld KB)", ok ? "found" : "NOT where they should be",
        static_cast<unsigned long>(object), name, static_cast<long>(commit / 1024));
    return ok;
}

bool ChargeBooks(u32 size) {
    const BooksGuard guard;
    if (!g_books_found) {
        return false;
    }
    ConsoleKernel kernel;
    books::Way way = books::Way::Kernel;
    const s32 rc = books::ChargeOrWrite(kernel, size, &way);
    if (way == books::Way::Written) {
        Log("books: charge of 0x%lx bytes WRITTEN in kernel mode: the kernel had no pages for its reservation (0x%08lx), the way of 1.0.13-1.1.1",
            static_cast<unsigned long>(size), static_cast<unsigned long>(books::kNoPages));
    } else if (rc == books::kAboveLimit) {
        s32 limit = 0;
        s32 current = 0;
        kernel.Read(&limit, &current);
        Log("books: the kernel refused 0x%lx bytes above the lifted limit (0x%08lx; now limit %ld KB, current %ld KB): a game thread took the "
            "room first, no charge and no fallback",
            static_cast<unsigned long>(size), static_cast<unsigned long>(rc), static_cast<long>(limit / 1024), static_cast<long>(current / 1024));
    } else if (rc != 0) {
        Log("books: the kernel did not reserve 0x%lx bytes (0x%08lx): no charge", static_cast<unsigned long>(size), static_cast<unsigned long>(rc));
    } else {
        Log("books: charge of 0x%lx bytes made through the kernel's reservation", static_cast<unsigned long>(size));
    }
    return rc == 0;
}

bool UnchargeBooks(u32 size) {
    const BooksGuard guard;
    if (!g_books_found) {
        return false;
    }
    ConsoleKernel kernel;
    u32 released = 0;
    books::Way way = books::Way::Kernel;
    const s32 rc = books::UnchargeOrWrite(kernel, size, &released, &way);
    if (way == books::Way::Written) {
        Log("books: uncharge of 0x%lx bytes: 0x%lx released by the kernel, 0x%lx WRITTEN off in kernel mode (no region had a page, 0x%08lx)",
            static_cast<unsigned long>(size), static_cast<unsigned long>(released), static_cast<unsigned long>(size - released),
            static_cast<unsigned long>(books::kNoPages));
    } else if (rc != 0) {
        Log("books: the kernel released 0x%lx of 0x%lx bytes (0x%08lx)", static_cast<unsigned long>(released), static_cast<unsigned long>(size),
            static_cast<unsigned long>(rc));
    } else {
        Log("books: uncharge of 0x%lx bytes made through the kernel's release", static_cast<unsigned long>(size));
    }
    return rc == 0;
}

bool RaiseBooks(u32 size, Result* rc) {
    const BooksGuard guard;
    if (!g_books_found) {
        return false;
    }
    ConsoleKernel kernel;
    *rc = books::RaiseThroughKernel(kernel, size);
    return *rc == 0;
}

bool SettleBooks(u32 size) {
    const BooksGuard guard;
    if (!g_books_found) {
        return false;
    }
    g_books.add = static_cast<s32>(size);
    svcCustomBackdoor(reinterpret_cast<void*>(KernelSettleBooks));
    return true;
}

OwedSettle TrySettleOwedBooks(u32* owed, bool heap_seen, u32* bytes) {
    *bytes = 0;
    if (__atomic_load_n(owed, __ATOMIC_SEQ_CST) == 0) {
        return OwedSettle::Nothing; // (nothing owed: FindBooks may never have run, the lock is not even looked at)
    }
    // Never waited for: whoever holds the lock (FindBooks writing a line, a Charge or an Uncharge of the late socket block and its svcs) holds
    // it for as long as it takes, and the keep-alive thread answers Luma3DS. The bytes are taken under the lock only, so a busy lock leaves
    // them owed, whole.
    if (LightLock_TryLock(&g_books_lock) != 0) {
        return OwedSettle::Busy;
    }
    const u32 take = books::TakeOwedOnceHeapSeen(owed, heap_seen);
    if (take != 0) {
        g_books.add = static_cast<s32>(take);
        svcCustomBackdoor(reinterpret_cast<void*>(KernelSettleBooks));
    }
    LightLock_Unlock(&g_books_lock);
    *bytes = take;
    return take != 0 ? OwedSettle::Settled : heap_seen ? OwedSettle::Nothing : OwedSettle::NoHeap;
}

bool ReadBooks(s32* limit, s32* current) {
    const BooksGuard guard;
    if (!g_books_found) {
        return false;
    }
    ConsoleKernel kernel;
    kernel.Read(limit, current);
    return true;
}

bool GameHeapSeen() {
    MemInfo info{};
    PageInfo page;
    return R_SUCCEEDED(svcQueryMemory(&info, &page, socplan::kRegionBase)) && info.state != MEMSTATE_FREE;
}

// A late block must not be there while the game sizes its heap, at its entry point. That is long past when the sockets start (the plugin's
// main() comes seconds later): this is the proof, cheap and bounded, not a wait anybody should see. Menu thread.
bool WaitForGameHeap() {
    for (int i = 0; i < 40; ++i) {
        if (GameHeapSeen()) {
            return true;
        }
        svcSleepThread(50 * 1000 * 1000);
    }
    Log("books: the game has no heap after 2 s, going on");
    return false;
}

} // namespace ctr
} // namespace azl
