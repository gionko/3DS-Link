// Socket bring-up inside the game process.
//
// Most local-wireless games do not list "soc:U" in their service access control list, so the usual
// srvGetServiceHandle() fails for them. Luma3DS offers svcControlService(STEAL_CLIENT_SESSION)
// which hands out a session to any registered service regardless of the ACL. libctru's socInit()
// asks srvGetServiceHandle() for the session, so the linker flag
//     -Wl,--wrap=srvGetServiceHandle
// routes that single call here (see the Makefile).
//
// The buffer socInit() hands to the soc service must be ordinary heap pages (state 0xBB05: the kernel
// refuses a Luma SHARED plugin block). It is taken with svcControlMemory(ALLOC) in PatchProcess, before
// the game starts, so it is charged to the game's commit when games still size their heap from what is
// left; see Includes/azl/soc_plan.hpp for where it goes and why it is as small as it is.
#include "ctr/soc.hpp"

#include <3ds.h>

#include <cstdio>
#include <cstring>

#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/soc_plan.hpp"
#include "azl/stats.hpp"
#include "csvc.h"

extern "C" Result __real_srvGetServiceHandle(Handle* out, const char* name);
extern "C" int azl_enet_rcvbuf_bytes; // Sources/enet/enet_3ds.c: the SO_RCVBUF ENet asks for

extern "C" Result __wrap_srvGetServiceHandle(Handle* out, const char* name) {
    const Result rc = __real_srvGetServiceHandle(out, name);
    // soc:U for the sockets, cfg:u to read the console's profile name.
    if (R_SUCCEEDED(rc) || (std::strcmp(name, "soc:U") != 0 && std::strcmp(name, "cfg:u") != 0)) {
        return rc;
    }
    azl::Log("srv: %s denied (0x%08lx), stealing a session through Luma", name,
             static_cast<unsigned long>(rc));
    Handle stolen = 0;
    const Result steal = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &stolen, name);
    if (R_FAILED(steal)) {
        azl::Log("srv: svcControlService for %s failed (0x%08lx)", name,
                 static_cast<unsigned long>(steal));
        return rc;
    }
    *out = stolen;
    return 0;
}

namespace azl {
namespace ctr {

namespace {
bool g_soc_ready = false;
bool g_reserve_tried = false;
u32 g_soc_buffer = 0;
u32 g_soc_size = 0;
char g_init_failure[96] = "";

bool ReadCommit(u32* limit, u32* used) {
    Handle h = 0;
    s64 lim = 0;
    s64 cur = 0;
    ResourceLimitType what = RESLIMIT_COMMIT;
    bool ok = false;
    if (R_SUCCEEDED(svcGetResourceLimit(&h, CUR_PROCESS_HANDLE))) {
        ok = R_SUCCEEDED(svcGetResourceLimitLimitValues(&lim, h, &what, 1)) &&
             R_SUCCEEDED(svcGetResourceLimitCurrentValues(&cur, h, &what, 1));
        svcCloseHandle(h);
    }
    if (!ok || lim <= 0 || lim > 0x20000000 || cur < 0 || cur > lim) {
        return false;
    }
    *limit = static_cast<u32>(lim);
    *used = static_cast<u32>(cur);
    return true;
}

// End of the FREE span that contains `addr` (clamped to the end of the kernel's heap region), 0 if
// `addr` is not free. For 0x0E000000 this is where the main thread's stack starts.
u32 FreeSpanEnd(u32 addr) {
    MemInfo info{};
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info, &page, addr)) || info.state != MEMSTATE_FREE) {
        return 0;
    }
    const u64 end = static_cast<u64>(info.base_addr) + info.size;
    return end > socplan::kRegionEnd ? socplan::kRegionEnd : static_cast<u32>(end);
}

enum class Try { Allocated, Next, Stop };

// svcQueryMemory says where there is room, the kernel has the last word.
Try TryAt(u32 base, u32 size, u32* address) {
    MemInfo info{};
    PageInfo page;
    const Result query = svcQueryMemory(&info, &page, base);
    if (R_FAILED(query) || info.state != MEMSTATE_FREE ||
        static_cast<u64>(info.base_addr) + info.size < static_cast<u64>(base) + size) {
        // The first few refusals are logged: on a console nobody can test, the log must say why.
        static int logged = 0;
        if (logged < 3) {
            ++logged;
            Log("soc: 0x%08lx skipped: query 0x%08lx, state 0x%lx, span 0x%08lx + %lu KB",
                static_cast<unsigned long>(base), static_cast<unsigned long>(query),
                static_cast<unsigned long>(info.state), static_cast<unsigned long>(info.base_addr),
                static_cast<unsigned long>(info.size / 1024));
        }
        return Try::Next;
    }
    u32 out = 0;
    const Result rc = svcControlMemory(&out, base, 0, size, MEMOP_ALLOC,
                                       static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
    if (R_SUCCEEDED(rc)) {
        if (out != base) { // never seen; do not keep a block somewhere we did not plan
            u32 ignored = 0;
            svcControlMemory(&ignored, out, 0, size, MEMOP_FREE, static_cast<MemPerm>(0));
            Log("soc: heap alloc at 0x%08lx returned 0x%08lx, released", static_cast<unsigned long>(base),
                static_cast<unsigned long>(out));
            return Try::Next;
        }
        *address = out;
        return Try::Allocated;
    }
    Log("soc: heap alloc of 0x%lx bytes at 0x%08lx failed (0x%08lx)", static_cast<unsigned long>(size),
        static_cast<unsigned long>(base), static_cast<unsigned long>(rc));
    // 0xC860180A = the commit limit, 0xD86007F3 = no physical page left in the region: neither depends
    // on the address, trying other places is pointless. A refused address (0xE0E01BF5, 0xE0E01BFD,
    // 0xE0A01BF5) is another summary and moves on to the next candidate.
    return R_SUMMARY(rc) == RS_OUTOFRESOURCE ? Try::Stop : Try::Next;
}

struct Placement {
    u32 address;
    socplan::Window window;
    socplan::PlanInfo plan;
};

bool AllocateBlock(u32 size, Placement* result) {
    // Static: this runs on the framework's 4 KB start-up stack.
    static socplan::Candidate list[socplan::kMaxCandidates];
    socplan::Inputs in{};
    s64 region = 0;
    svcGetSystemInfo(&region, 0x10000, 0x80); // Luma: size of the application memory region
    in.region_bytes = region > 0 && region < 0x40000000 ? static_cast<u32>(region) : 0;
    if (!ReadCommit(&in.commit_limit, &in.commit_used) && in.region_bytes != 0) {
        in.commit_limit = in.region_bytes; // no commit numbers: assume the whole region is the budget
        in.commit_used = 0;
    }
    in.free_end = FreeSpanEnd(socplan::kHeapAreaEnd);
    socplan::PlanInfo plan{};
    const int count = socplan::Build(in, size, list, socplan::kMaxCandidates, &plan);
    Log("soc: placement of 0x%lx bytes: commit %lu of %lu KB, region %lu KB, %s%s, stack at 0x%08lx, %d candidates",
        static_cast<unsigned long>(size), static_cast<unsigned long>(in.commit_used / 1024),
        static_cast<unsigned long>(in.commit_limit / 1024), static_cast<unsigned long>(in.region_bytes / 1024),
        plan.known ? "the game's heap can reach " : "heap reach unknown",
        plan.known ? (plan.provable ? "a lower address (provable)" : "the top of the 96 MiB heap area") : "",
        static_cast<unsigned long>(in.free_end), count);
    for (int i = 0; i < count; ++i) {
        u32 address = 0;
        const Try t = TryAt(list[i].base, size, &address);
        if (t == Try::Allocated) {
            result->address = address;
            result->window = list[i].window;
            result->plan = plan;
            return true;
        }
        if (t == Try::Stop) {
            return false;
        }
    }
    return false;
}
} // namespace

bool ReserveSocketMemory() {
    if (g_soc_buffer) {
        return true;
    }
    if (g_reserve_tried) {
        return false; // a retry cannot do better: the game is running and has no commit left
    }
    g_reserve_tried = true;
    for (u32 size : socplan::kBlockSizes) {
        Placement p{};
        if (!AllocateBlock(size, &p)) {
            continue;
        }
        g_soc_buffer = p.address;
        g_soc_size = size;
        // The service never checks a buffer size against the block (a 256 KB SO_RCVBUF in a 128 KB block
        // made it write past the end), so the request is kept to the ring the block holds, and never
        // above 64 KB whatever the ini says (see Includes/azl/rcvbuf.h).
        azl_enet_rcvbuf_bytes = azl_rcvbuf_limit(azl_enet_rcvbuf_bytes, size);
        MemInfo info{};
        PageInfo page;
        svcQueryMemory(&info, &page, p.address);
        Log("soc: reserved a %lu KB buffer at 0x%08lx, %s (state 0x%lx, region 0x%08lx + %lu KB%s, SO_RCVBUF "
            "limited to %d KB)",
            static_cast<unsigned long>(size / 1024), static_cast<unsigned long>(p.address),
            socplan::WindowName(p.window), static_cast<unsigned long>(info.state),
            static_cast<unsigned long>(info.base_addr), static_cast<unsigned long>(info.size / 1024),
            p.plan.known ? (p.address >= p.plan.heap_reach ? ", above the game's heap reach" : ", INSIDE the game's heap reach")
                         : "",
            azl_enet_rcvbuf_bytes / 1024);
        if (size < socplan::kSocBlockSize) {
            Log("soc: WARNING only a %lu KB block could be reserved: the receive ring is held to %d KB (about half a"
                " second of a 4-player session)",
                static_cast<unsigned long>(size / 1024), azl_enet_rcvbuf_bytes / 1024);
        }
        return true;
    }
    Log("soc: could not reserve memory for the socket layer");
    return false;
}

unsigned int SocketBufferAddress() {
    return g_soc_buffer;
}

unsigned int SocketBufferSize() {
    return g_soc_size;
}

const char* SocketInitFailure() {
    return g_init_failure;
}

bool InitSockets() {
    if (g_soc_ready) {
        return true;
    }
    // Reserved at start-up (EarlyInit). Without it nothing can be done now: the game is running and a
    // late attempt only works when it still has commit left.
    if (!ReserveSocketMemory()) {
        std::snprintf(g_init_failure, sizeof(g_init_failure), "No memory for the network layer");
        return false;
    }
    const Result rc = socInit(reinterpret_cast<u32*>(g_soc_buffer), g_soc_size);
    if (R_FAILED(rc)) {
        const char* name = socplan::InitFailureName(static_cast<u32>(rc));
        Log("soc: socInit failed (0x%08lx) with a 0x%lx byte buffer at 0x%08lx%s%s", static_cast<unsigned long>(rc),
            static_cast<unsigned long>(g_soc_size), static_cast<unsigned long>(g_soc_buffer),
            name ? ": " : "", name ? name : "");
        // (the text for the screen: short, with the code)
        const char* shown = rc == static_cast<Result>(0xC8A0700B) ? "The game already uses soc:U (0x%08lx)"
                            : rc == static_cast<Result>(0xC8A07004) ? "soc:U session already started (0x%08lx)"
                            : g_debug_level > 0 ? "Could not start the network layer (0x%08lx)" // (the log has the details)
                                                : "Could not start the network layer (0x%08lx): set debug=1 for details";
        std::snprintf(g_init_failure, sizeof(g_init_failure), shown, static_cast<unsigned long>(rc));
        return false;
    }
    g_soc_ready = true;
    g_init_failure[0] = '\0';
    // While the service holds the block the kernel shows it as locked (state 11), the creator's
    // permissions are none.
    MemInfo info{};
    PageInfo page;
    svcQueryMemory(&info, &page, g_soc_buffer);
    Log("soc: sockets ready (%lu KB buffer at 0x%08lx, now state %lu perm %lu)",
        static_cast<unsigned long>(g_soc_size / 1024), static_cast<unsigned long>(g_soc_buffer),
        static_cast<unsigned long>(info.state), static_cast<unsigned long>(info.perm));
    return true;
}

void ShutdownSockets() {
    if (!g_soc_ready) {
        return;
    }
    // (the end of the sockets is the last thing the log says about them: how long soc:U took to let go, and what it answered)
    const u64 started_ms = platform::NowMs();
    Log("soc: socExit begins (the block at 0x%08lx stays mapped: the process is about to exit)",
        static_cast<unsigned long>(g_soc_buffer));
    const Result rc = socExit();
    Log("soc: socExit done in %lu ms -> 0x%08lx", static_cast<unsigned long>(platform::NowMs() - started_ms),
        static_cast<unsigned long>(rc));
    g_soc_ready = false; // the buffer stays mapped: the process is about to exit
}

} // namespace ctr
} // namespace azl
