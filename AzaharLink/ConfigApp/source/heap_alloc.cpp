// The app's own __system_allocateHeaps. libctru's (allocateHeaps.o of libctru 2.7.0) is weak; this file defines it and the four variables of that
// object (__ctru_heap, __ctru_linear_heap, __ctru_heap_size, __ctru_linear_heap_size), so the linker never takes libctru's object at all.
// What it does differently, and why, is in heap_plan.hpp. It runs from __libctru_init, before main() and before the static constructors and
// newlib: only system calls here, no allocation, no printf.
#include <3ds.h>
#include <3ds/allocator/mappable.h>

#include <cstdio>
#include <ctime>

#include "fileio.hpp"
#include "heap_alloc.hpp"
#include "heap_plan.hpp"
#include "paths.hpp"

static_assert(heap_plan::kHeapAreaSize == OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN, "the heap area of heap_plan.hpp is libctru's");
static_assert(heap_plan::kAppRegion == MEMOP_REGION_APP, "the APPLICATION memory type of heap_plan.hpp is libctru's");

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

u32 __ctru_heap;
u32 __ctru_linear_heap;
// Weak as in libctru: a program may fix either size (0 = worked out at start).
__attribute__((weak)) u32 __ctru_heap_size = 0;
__attribute__((weak)) u32 __ctru_linear_heap_size = 0;

void __system_allocateHeaps(void);
}

namespace {

// Filled before main() (zeroed .bss: crt0 clears it before __libctru_init).
heap_plan::Record g_record;

// What each value is, and what the kernel answers, is in heap_plan.hpp (Raw).
struct KernelEnv {
    heap_plan::Raw Read() {
        heap_plan::Raw r;
        Handle reslimit = 0;
        if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
            svcBreak(USERBREAK_PANIC); // (as libctru: a process without a resource limit cannot go on)
        }
        s64 limit = 0;
        s64 current = 0;
        ResourceLimitType type = RESLIMIT_COMMIT;
        svcGetResourceLimitLimitValues(&limit, reslimit, &type, 1);
        svcGetResourceLimitCurrentValues(&current, reslimit, &type, 1);
        svcCloseHandle(reslimit);
        r.limit = limit;
        r.current = current;

        s64 v = 0;
        r.used_rc = svcGetSystemInfo(&v, 0, 1);
        r.used = v;
        v = 0;
        r.luma_rc = svcGetSystemInfo(&v, 0x10000, 0x80);
        r.luma_region = v;
        r.appmemalloc = osGetMemRegionSize(MEMREGION_APPLICATION);
        v = 0;
        r.own_rc = svcGetProcessInfo(&v, CUR_PROCESS_HANDLE, 2);
        r.own = v;
        v = 0;
        r.memtype_rc = svcGetProcessInfo(&v, CUR_PROCESS_HANDLE, 19);
        r.memtype = v;
        return r;
    }

    // The linear heap first: KProcessPageTable::ControlMemory (kernel 11.14) zeroes the linear block only after KMemoryManager::AllocateContiguous
    // found it, so a refused linear heap costs nothing; the heap's pages are zeroed as they are taken (KPageTable::Operate).
    heap_plan::Part Try(u32 heap, u32 linear, s32* error) {
        u32 linear_addr = 0;
        Result rc = svcControlMemory(&linear_addr, 0x0, 0x0, linear, MEMOP_ALLOC_LINEAR, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
        if (R_FAILED(rc)) {
            *error = rc;
            return heap_plan::Part::Linear;
        }
        u32 heap_addr = 0;
        rc = svcControlMemory(&heap_addr, OS_HEAP_AREA_BEGIN, 0x0, heap, MEMOP_ALLOC, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
        if (R_FAILED(rc)) {
            *error = rc;
            u32 unused = 0;
            svcControlMemory(&unused, linear_addr, 0x0, linear, MEMOP_FREE, static_cast<MemPerm>(0)); // (the next request starts from nothing)
            return heap_plan::Part::Heap;
        }
        __ctru_heap = heap_addr;
        __ctru_linear_heap = linear_addr;
        return heap_plan::Part::None;
    }

    void Sleep(u32 ms) {
        svcSleepThread(static_cast<s64>(ms) * 1000000LL);
    }

    u32 NowMs() {
        return static_cast<u32>(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));
    }
};

// The end, with the numbers where Luma's crash dump keeps them (heap_plan.hpp, PackBreak): in r1-r10 and r12, and 16 words on this function's
// stack (between sp and the end of its page: the stack dump), starting with the bytes "HEAP". r11 is left alone (the frame pointer).
[[noreturn]] __attribute__((noinline)) void BreakWithNumbers(const heap_plan::Record& rec) {
    const heap_plan::BreakWords b = heap_plan::PackBreak(rec);
    volatile u32 on_stack[16];
    for (int i = 0; i < 16; ++i) {
        on_stack[i] = b.w[i];
    }
    __asm__ __volatile__("" : : "r"(on_stack) : "memory"); // (the words are used: they stay on the stack)
    register u32 r0 asm("r0") = USERBREAK_PANIC;
    register u32 r1 asm("r1") = b.w[0];
    register u32 r2 asm("r2") = b.w[1];
    register u32 r3 asm("r3") = b.w[2];
    register u32 r4 asm("r4") = b.w[3];
    register u32 r5 asm("r5") = b.w[4];
    register u32 r6 asm("r6") = b.w[5];
    register u32 r7 asm("r7") = b.w[6];
    register u32 r8 asm("r8") = b.w[7];
    register u32 r9 asm("r9") = b.w[8];
    register u32 r10 asm("r10") = b.w[9];
    register u32 r12 asm("r12") = b.w[10];
    for (;;) {
        __asm__ __volatile__("svc 0x3C"
                             :
                             : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8), "r"(r9), "r"(r10), "r"(r12)
                             : "memory"); // (the volatile stores to on_stack come before it)
    }
}

} // namespace

extern "C" void __system_allocateHeaps(void) {
    KernelEnv env;
    if (!heap_plan::Run(env, __ctru_heap_size, __ctru_linear_heap_size, g_record)) {
        BreakWithNumbers(g_record);
    }
    __ctru_heap_size = g_record.heap;
    __ctru_linear_heap_size = g_record.linear;

    // Mappable allocator
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

    // newlib's heap
    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

namespace heap_alloc {

void WriteStartLog(const std::string& datadir) {
    static bool written = false; // (one line per start, whoever calls)
    if (written) {
        return;
    }
    written = true;
    char stamp[96];
    char line[1024];
    const int s = heap_plan::FormatStamp(static_cast<int64_t>(std::time(nullptr)), g_record.start_ms, stamp, sizeof(stamp));
    const int n = heap_plan::FormatLine(g_record, line, sizeof(line));
    if (s <= 0 || n <= 0) {
        return;
    }
    const std::string path = setup::JoinPath(datadir, "3DSLink-heap.log");
    std::string old;
    if (setup::ReadTextFile(path, heap_plan::kLogReadBytes, old) == setup::LoadResult::Unreadable) {
        return; // (a file that could not be read is not written over: the starts it holds stay)
    }
    setup::WriteTextFile(path, heap_plan::KeepLast(old, std::string(stamp) + line, heap_plan::kLogMaxLines, heap_plan::kLogMaxBytes));
}

bool ForeignAtStart() {
    return g_record.foreign_first != 0 || g_record.foreign_last != 0;
}

} // namespace heap_alloc
