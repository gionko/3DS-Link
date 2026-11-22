// THE LIST of the games whose own thread on core 1 the kernel refuses while the framework's SysCore worker holds that core, on a New 3DS (pure data, no system calls: unit-tested on
// the PC, Tests/soc_plan_test.cpp). For these the framework's start-up threads go to core 0 on every console (ctr/thread_core.cpp), as they already do on an Old 3DS.
//
// The defect (kernel11 KThread::Initialize, the decompilation in the repository, the lines that return 0xE0A01839): for a thread of an APPLICATION process that asks for core 1 the kernel
//   (a) refuses when the process's CPU time limit is 0 and its kernel version is above 2.1 (Luma3DS's pm sets one for these titles, so it is not the case here), or
//   (b) in the scheduler's "single" application mode refuses when a thread of the same process already sits on core 1.
// libctrpf 0.8.0 starts one such worker (SysCore, core 1, priority 0x20) before the game's first instruction; the games below are the early 2011 ones whose NintendoWare sound system asks for its
// thread on core 1 a moment later (a `bl` to the SDK's thread-start function, whose error check accepts only success, info, status and out-of-resource: the rest ends in ErrDisp).
// Seen on a New 3DS XL (Luma3DS with "Redirect app. syscore threads to core2" OFF, so the games' own request really reaches the kernel) and on a New 2DS XL (console 63, Luma3DS 13.4.0,
// 2026-11-10): the same ErrDisp, the same address every time, a couple of seconds after the start, and the game plays with the title off in Enable Titles.
// A game with the same symptom needs its row in azl/title_rules.hpp with worker_off_core1 (every region of it) and nothing else (2026-11-15: the list that was
// here is that column now: Ridge Racer 3D, Pac-Man & Galaga Dimensions, Ocarina of Time 3D). Not a rule for every game: Super Street Fighter IV, Tekken and
// the rest start with the plugin on, and a worker moved off core 1 is one thing less for them to find out about.
#pragma once

#include <cstdint>

#include "azl/title_rules.hpp"

namespace azl {
namespace ctr {
namespace coreplan {

/// Whether the framework's start-up threads leave core 1 for this title on a New 3DS (an Old 3DS moves them for every title: ctr/thread_core.hpp).
/// The application's id only (the process's): a Download Play child or an update is no row's.
constexpr bool LeavesCore1ToTheGame(uint64_t title_id) {
    return title_rules::AppRulesFor(title_id).worker_off_core1;
}

} // namespace coreplan
} // namespace ctr
} // namespace azl
