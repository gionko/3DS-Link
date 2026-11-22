// On an Old 3DS, the framework's start-up threads off core 1 (Includes/ctr/thread_core.hpp says why). Two linker wraps, both in the Makefile:
//   -Wl,--wrap=svcCreateThread                                   every svcCreateThread of libctrpf (thread.o's __startThread, pluginInit.o's __entrypoint) ends in __wrap_svcCreateThread
//   -Wl,--wrap=_ZN18CTRPluginFramework9Scheduler10InitializeEv    CTRPluginFramework::Scheduler::Initialize(), called once by KeepThreadMain
// Without the second wrap the first one would not know who is asking; without the first the second does nothing.
#include "ctr/thread_core.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include "azl/core_plan.hpp"

namespace {

volatile bool g_moving = false; // only the framework's own thread runs between the two wraps (the game's main thread waits for it): no lock needed
unsigned g_moved = 0;
bool g_listed = false; // the title is on azl::ctr::coreplan::kCore1WorkerTitles (a New 3DS moves the worker for it)

} // namespace

extern "C" {

Result __real_svcCreateThread(Handle* thread, ThreadFunc entrypoint, u32 arg, u32* stack_top, s32 thread_priority, s32 processor_id);

Result __wrap_svcCreateThread(Handle* thread, ThreadFunc entrypoint, u32 arg, u32* stack_top, s32 thread_priority, s32 processor_id) {
    s32 placed = processor_id;
    if (g_moving) {
        placed = azl::ctr::threadcore::PlaceFrameworkStartThread(processor_id);
        if (placed != processor_id) {
            ++g_moved;
        }
    }
    return __real_svcCreateThread(thread, entrypoint, arg, stack_top, thread_priority, placed);
}

void __real__ZN18CTRPluginFramework9Scheduler10InitializeEv();

void __wrap__ZN18CTRPluginFramework9Scheduler10InitializeEv() {
    // The very test of Luma3DS's swap (rosalina's isN3DS): the console, not the game's mode.
    s64 is_new = 0;
    const bool old_console = svcGetSystemInfo(&is_new, 0x10001, 0) != 0;
    // The framework's ProcessImpl::Initialize (KeepThreadMain calls it just before this) has read the title id: a title of the list keeps core 1 free for itself on a New 3DS too.
    g_listed = !old_console && azl::ctr::coreplan::LeavesCore1ToTheGame(CTRPluginFramework::Process::GetTitleID());
    g_moving = old_console || g_listed;
    __real__ZN18CTRPluginFramework9Scheduler10InitializeEv();
    g_moving = false;
}

} // extern "C"

namespace azl {
namespace ctr {
namespace threadcore {

unsigned FrameworkThreadsMoved() {
    return g_moved;
}

bool MovedForListedTitle() {
    return g_listed;
}

} // namespace threadcore
} // namespace ctr
} // namespace azl
