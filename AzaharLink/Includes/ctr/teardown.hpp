// Protection against CTRPluginFramework's exit sequence.
//
// When the game closes, libctrpf (0.8.0) runs the static destructors of its scheduler and hook
// manager, which free() the stacks and bookkeeping of worker threads that were never joined (and
// may not even have started yet: a thread on the syscore is only scheduled when the game left it
// CPU time). A late-starting worker then executes freed memory. On Tri Force Heroes this ended
// in a crash dump with the worker jumping into a stale OSD hook wrapper (see the 0.1.3 post-mortem
// in the project notes).
//
// Once the process is going away nothing needs to be given back, so free() is turned into a no-op:
// every block stays valid and the late worker simply sees "exit" and leaves. Needs the linker
// flag -Wl,--wrap=free (see the Makefile).
#pragma once

namespace azl {
namespace ctr {

/// From now on free() does nothing. Only a flag store: safe from any thread and any stack.
void BeginTeardown();
bool InTeardown();

} // namespace ctr
} // namespace azl
