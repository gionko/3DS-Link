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
// size it returns to what is left of the .text from where that search starts.
//
// One more call is answered, for ONE application (the system's Download Play, in each of its six regional titles - kDownloadPlayTitles below;
// 0004001000022100, Europe, is the one seen: 2026-11-13): Services::GSP::Initialize, before it
// looks for the Nintendo SDK wrapper of GSPGPU::RegisterInterruptRelayQueue, tests "size of the code - 9" against the ADDRESS 0x100000, so a title
// with less than 1 MB of code gets -1 without a search and KeepThreadMain calls abort(). For those titles, and only when the test would fail, that
// call is answered 0x100009 (text_search_guard.cpp says why that value); the search that follows takes its size from another call, which is not
// touched. Every other caller, and every other title, gets the real value.
#pragma once

#include "types.h"

namespace azl {
namespace ctr {

// THE LIST of the titles of the system's Download Play application, one for each region of the console (2026-11-14, the rule "every region of
// what we work on": the European title is the one that is tried, an American console must not be left with the abort). The ids are the system
// title list's (the same six in Azahar's src/core/system_titles.cpp; Australia has the European one). Only the European application was read
// (version 3073: 0x93000 bytes of code, the SDK wrapper at 0x00131438) and seen on a console; the other five are NOT seen and their code was
// not read. What makes that safe is that the answer does not lean on the binary: for a code under 1 MB + 9 bytes it lets the framework do ONE
// search of the title's own code with the real size - the wrapper is found and hooked as in every game, or it is not and the start ends in the
// abort() it ended in before (the guard's source says why never in a loop); a code of 1 MB + 9 bytes or more is never asked who it is.
// Two users, one list: the guard of GetTextSize (Sources/ctr/text_search_guard.cpp) and the line EarlyInit writes at the start of these titles
// (Sources/ctr/plugin.cpp). Pure data and a comparison: no system call, nothing of the framework (it is read inside the framework's own start).
constexpr u64 kDownloadPlayTitles[] = {
    0x0004001000020100ull, // the Download Play application, Japan (the system title list; not seen)
    0x0004001000021100ull, // the Download Play application, USA (the system title list; not seen)
    0x0004001000022100ull, // the Download Play application, Europe and Australia (seen on a console, 2026-11-13; its code read, version 3073)
    0x0004001000026100ull, // the Download Play application, China (the system title list; not seen)
    0x0004001000027100ull, // the Download Play application, Korea (the system title list; not seen)
    0x0004001000028100ull, // the Download Play application, Taiwan (the system title list; not seen)
};

/// Is this title the system's Download Play application, of any region?
constexpr bool IsDownloadPlayTitle(u64 title_id) {
    for (const u64 title : kDownloadPlayTitles) {
        if (title == title_id) {
            return true;
        }
    }
    return false;
}

/// How many times the framework's startup search had its size cut to the game's code.
u32 TextSearchTrims();

} // namespace ctr
} // namespace azl
