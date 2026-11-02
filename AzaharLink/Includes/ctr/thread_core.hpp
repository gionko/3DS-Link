// Where the framework's own worker threads run on an Old 3DS.
//
// WHY. libctrpf 0.8.0 starts the workers of its Task scheduler (Scheduler::Initialize, before the game's first instruction): two on core 0 and one on the SYSCORE (core 1, priority
// 0x20). A thread on core 1 runs only when the game left that core some time, so this one may not have run at all when the game goes to the HOME menu. On an Old 3DS Luma3DS then stops the
// plugin's threads and unmaps the plugin's block, but it knows a thread of the plugin by a mark in its TLS, which libctru writes when the thread begins: a thread that has not begun
// is not stopped. When core 1 has time for it, the worker begins in memory that is gone (the Luma dump of issue 5: prefetch abort at the plugin's _thread_begin, core 1, every other register 0).
//
// WHAT. On an Old 3DS, a thread that the framework starts while it starts up never takes core 1: it goes to core 0, begins at once, and is stopped with the others. It is an idle worker that
// waits on an event; nothing in this plugin schedules a "SysCore" task, and a task that did would run on core 0 instead. Sources/ctr/thread_core.cpp applies the rule through two linker
// wraps (Makefile). The plugin's own threads (azl/platform.cpp) and the game's are never touched, and on a New 3DS nothing changes: there is no swap there.
#pragma once

namespace azl {
namespace ctr {
namespace threadcore {

/// The core a thread that the framework starts during its own start-up really gets: core 1 is the only value that changes.
constexpr int PlaceFrameworkStartThread(int requested_core) {
    return requested_core == 1 ? 0 : requested_core;
}

/// How many of the framework's start-up threads were moved off core 1 (0 on a New 3DS).
unsigned FrameworkThreadsMoved();

} // namespace threadcore
} // namespace ctr
} // namespace azl
