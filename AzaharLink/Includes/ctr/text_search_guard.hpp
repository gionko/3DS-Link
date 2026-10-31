// Protection against CTRPluginFramework's startup search running past the game's code.
//
// libctrpf (0.8.0), in KeepThreadMain, looks in the game's .text for the `svc 0x1F` that maps the
// GSP shared memory (the one preceded by `mov r3, #0x10000000`). When a match is not the right one
// (Kirby Triple Deluxe and Kirby Planet Robobot have only the generic svc stub, which is not) it
// searches again from "match + 4" but with the size of the WHOLE .text, so the window ends
// "match - 0x100000" bytes past the end of the code. When the memory after the code is shorter than
// that, the search reads an unmapped page and the process dies with a data abort inside
// Utils::Search before the plugin has run a single line (Luma crash dump, PC in Search<unsigned long>).
//
// Process::GetTextSize() is called again for every search, from one place in KeepThreadMain: the
// guard (needs -Wl,--wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv, see the Makefile) cuts the
// size it returns to what is left of the .text from where that search starts. Every other caller
// gets the real value.
#pragma once

#include "types.h"

namespace azl {
namespace ctr {

/// How many times the framework's startup search had its size cut to the game's code.
u32 TextSearchTrims();

} // namespace ctr
} // namespace azl
