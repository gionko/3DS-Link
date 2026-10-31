// The guard against CTRPluginFramework's startup search running past the game's code (Sources/ctr/text_search_guard.cpp, and the Makefile's
// --wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv): the decision it takes, on made-up sizes and addresses shaped like the case that crashed in Kirby Triple Deluxe and Kirby Planet
// Robobot (the search that starts after a match that was not the right one is given the size of the WHOLE code and ends past it). The ARM stub that hands the guard r6 and lr
// (text_search_guard_entry.s) cannot run on a PC: it is read in the disassembly of the built .elf (Tests/text_search_guard_elf_check.sh).
//
//   text_search_guard_test      (needs nothing; built by Tests/run.sh)
#include <cstdio>

#include "ctr/text_search_guard.hpp"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK_EQ(got, want)                                                                                                         \
    do {                                                                                                                            \
        ++g_checks;                                                                                                                 \
        const unsigned long g_ = static_cast<unsigned long>(got);                                                                   \
        const unsigned long w_ = static_cast<unsigned long>(want);                                                                  \
        if (g_ != w_) {                                                                                                             \
            ++g_failures;                                                                                                           \
            std::printf("  FAIL %s:%d: %s is 0x%lx, expected 0x%lx\n", __FILE__, __LINE__, #got, g_, w_);                          \
        }                                                                                                                           \
    } while (0)

static u32 g_real = 0; // what Process::GetTextSize() would answer
extern "C" u32 __real__ZN18CTRPluginFramework7Process11GetTextSizeEv(void) {
    return g_real;
}
extern "C" u32 azl_text_size_guard(u32 caller_r6, const u32* return_address);

int main() {
    const u32 site[2] = {0xe1a07000, 0xe2866004};  // mov r7, r0 ; add r6, r6, #4  (the call in KeepThreadMain)
    const u32 other[2] = {0xe1a01000, 0xe3a00601}; // any other caller
    const u32 mov_only[2] = {0xe1a07000, 0xe3a00601}; // the first word alone is not the site
    const u32 add_only[2] = {0xe1a01000, 0xe2866004}; // nor is the second

    g_real = 0x400000; // a made-up code size: the code is 0x100000..0x500000
    CHECK_EQ(azl_text_size_guard(0x000FFFFC, site), 0x400000);              // the first search (from the start of the code): the size is already right
    CHECK_EQ(azl_text_size_guard(0x00200000, site), 0x400000 - 0x100004);   // the search that started after a match: the case of the crash
    CHECK_EQ(azl_text_size_guard(0x004FFFEC, site), 0x10);                  // 0x10 bytes left
    CHECK_EQ(azl_text_size_guard(0x004FFFF8, site), 4);                     // 4 bytes left: one pattern
    CHECK_EQ(azl_text_size_guard(0x004FFFFA, site), 0);                     // 2 left: shorter than the pattern, the search would wrap around
    CHECK_EQ(azl_text_size_guard(0x00700000, site), 0);                     // r6 beyond the end of the code
    CHECK_EQ(azl_text_size_guard(0x00200000, other), 0x400000);            // another caller: the real value
    CHECK_EQ(azl_text_size_guard(0x00200000, mov_only), 0x400000);
    CHECK_EQ(azl_text_size_guard(0x00200000, add_only), 0x400000);

    g_real = 0x500000; // a second made-up code size
    CHECK_EQ(azl_text_size_guard(0x00300000, site), 0x500000 - 0x200004);   // the same case with another code size

    g_real = 0x300000; // a game that does not crash
    CHECK_EQ(azl_text_size_guard(0x000FFFFC, site), 0x300000);

    // the trims it counted: the calls at the site that were given less than the whole size (the five of the first code size and the one of the second)
    CHECK_EQ(azl::ctr::TextSearchTrims(), 6);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
