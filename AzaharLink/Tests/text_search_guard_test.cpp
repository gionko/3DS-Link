// The guard against CTRPluginFramework's startup search running past the game's code (Sources/ctr/text_search_guard.cpp, and the Makefile's
// --wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv): the decision it takes, on made-up sizes and addresses shaped like the case that crashed in Kirby Triple Deluxe and Kirby Planet
// Robobot (the search that starts after a match that was not the right one is given the size of the WHOLE code and ends past it). The ARM stub that hands the guard r6 and lr
// (text_search_guard_entry.s) cannot run on a PC: it is read in the disassembly of the built .elf (Tests/text_search_guard_elf_check.sh).
// The second half is the Download Play application (0004001000022100, and since 2026-11-14 its other five regional titles: the list of text_search_guard.hpp): the one call of
// Services::GSP::Initialize that is answered 0x100009 for those titles when their code is under 1 MB + 9 bytes, and everything that must stay as it is (another title, another call,
// a code that passes the framework's test by itself, a kernel that does not answer).
//
//   text_search_guard_test      (needs nothing; built by Tests/run.sh)
#include <cstdio>
#include <initializer_list>

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

// The kernel's answer to "which title is this process" (svcGetProcessInfo, type 0x10001 of Luma3DS), as the test sets it.
static u64 g_title = 0;
static Result g_title_result = 0;
static int g_title_asked = 0;
static Handle g_asked_process = 0;
static u32 g_asked_type = 0;
extern "C" Result svcGetProcessInfo(s64* out, Handle process, u32 type) {
    ++g_title_asked;
    g_asked_process = process;
    g_asked_type = type;
    *out = static_cast<s64>(g_title); // (also when the call fails: a guard that did not look at the result would take what is left there for the title)
    return g_title_result;
}

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

    // ---- The Download Play application: the test of Services::GSP::Initialize before its search of the SDK wrapper ----
    // ldrd r2, [sp, #12] ; sub r3, r3, r2 ; sub r0, r0, r3, asr #2 ; cmp r0, r4 ; bcc <-1>      (what follows that call in libctrpf 0.8.0)
    const u32 gsp[5] = {0xe1cd20dc, 0xe0433002, 0xe0400143, 0xe1500004, 0x3a000006};
    const u32 gsp_far[5] = {0xe1cd20dc, 0xe0433002, 0xe0400143, 0xe1500004, 0x3a0000ff}; // the same test with the -1 further away
    const u64 kDownloadPlayEur = 0x0004001000022100ull;
    const u32 kDlplayCode = 0x93000; // its 0x93 pages of code
    const u32 trims_before = azl::ctr::TextSearchTrims();

    // the title, the call, a code under 1 MB + 9 bytes: the answer that lets the framework search
    g_title = kDownloadPlayEur;
    g_real = kDlplayCode;
    CHECK_EQ(azl_text_size_guard(0, gsp), 0x00100009); // the Download Play application gets through
    CHECK_EQ(g_asked_process, 0xFFFF8001); // the guard asked about its own process ...
    CHECK_EQ(g_asked_type, 0x10001);       // ... for its title id
    CHECK_EQ(azl_text_size_guard(0x00131438, gsp), 0x00100009); // (r6 is not the framework's "last match" at this call: not looked at)
    CHECK_EQ(azl_text_size_guard(0, gsp_far), 0x00100009);
    // ... which is what the framework's own test needs: answer - 9 is not below 0x100000 the first time round (r4 = 0x100000), and is below any later match (the framework
    // would search from the start again, find the same match and never leave the loop)
    CHECK_EQ(azl_text_size_guard(0, gsp) - 9 >= 0x00100000u, 1);
    CHECK_EQ(azl_text_size_guard(0, gsp) - 9 < 0x00100004u, 1);
    g_real = 0x00100008; // the last size that fails the framework's test
    CHECK_EQ(azl_text_size_guard(0, gsp), 0x00100009);
    g_real = 0x1000; // one page
    CHECK_EQ(azl_text_size_guard(0, gsp), 0x00100009);

    // a code that passes the framework's test by itself: the real size, and the kernel is not even asked
    const int asked = g_title_asked;
    for (const u32 size : {0x00100009u, 0x0010000Au, 0x00400000u, 0x03F00000u}) {
        g_real = size;
        CHECK_EQ(azl_text_size_guard(0, gsp), size); // a big code is left alone
    }
    g_real = 8; // (never a title: the framework's subtraction wraps and its test passes, the guard does the same sum)
    CHECK_EQ(azl_text_size_guard(0, gsp), 8);
    CHECK_EQ(g_title_asked, asked);

    // another title with the same small code (a game, the game's child, a neighbour of the id, the region digits that are no console's - 3, 4, 5, 9 - the id of the
    // application with the New 3DS bit, in the applets' category, with another variation): as today
    g_real = kDlplayCode;
    for (const u64 title : {0x0004001000023100ull, 0x0004001000024100ull, 0x0004001000025100ull, 0x0004001000029100ull, 0x000400100002F100ull, 0x0004001020022100ull,
                            0x0004003000022100ull, 0x0004001000021000ull, 0x0004001000020000ull, 0x0004001000021200ull,
                            0x0004001000022000ull, 0x0004001000022101ull, 0x0004000000022100ull, 0x00040000000F8200ull,
                            0x000400000F8F8200ull, 0x0000000000022100ull, 0x0004001000000000ull, 0x0ull}) {
        g_title = title;
        CHECK_EQ(azl::ctr::IsDownloadPlayTitle(title), 0);
        CHECK_EQ(azl_text_size_guard(0, gsp), kDlplayCode); // another title is left alone
    }
    // the kernel does not say which title this is: as today
    g_title = kDownloadPlayEur;
    g_title_result = static_cast<Result>(0xD8E007F7);
    CHECK_EQ(azl_text_size_guard(0, gsp), kDlplayCode);
    g_title_result = 0;

    // another call in that title (one word of the five is not the test's; the fifth becomes a bne): as today
    for (int changed = 0; changed < 5; ++changed) {
        u32 near_miss[5] = {gsp[0], gsp[1], gsp[2], gsp[3], gsp[4]};
        near_miss[changed] ^= changed == 4 ? 0x20000000u : 0x00000001u;
        CHECK_EQ(azl_text_size_guard(0, near_miss), kDlplayCode); // another call is left alone
    }
    // ... the SECOND call of GSP::Initialize among them, whose value is the size of the search (add r2, sp, #12 ; mov r1, r0 ; mov r0, #0x100000 ; bl Utils::Search):
    // the real size, so the search stays inside the title's code
    const u32 gsp_search[5] = {0xe28d200c, 0xe1a01000, 0xe3a00601, 0xebffbb43, 0xe2504000};
    CHECK_EQ(azl_text_size_guard(0, gsp_search), kDlplayCode);
    CHECK_EQ(azl_text_size_guard(0, other), kDlplayCode);
    // ... and KeepThreadMain's search in that title is cut as in every other
    CHECK_EQ(azl_text_size_guard(0x000FFFFC, site), kDlplayCode);
    CHECK_EQ(azl_text_size_guard(0x00120000, site), kDlplayCode - 0x20004);
    CHECK_EQ(azl::ctr::TextSearchTrims(), trims_before + 1); // (an answer to the GSP test is not a cut)
    std::printf("Download Play: for 0x%lx bytes of code the guard answers 0x%08lx to the GSP test\n", static_cast<unsigned long>(kDlplayCode),
                static_cast<unsigned long>(azl_text_size_guard(0, gsp)));

    // ---- Every region of the application (2026-11-14): the six titles of the list, written here once more by hand, in the list's order ----
    // Japan, USA, Europe (and Australia), China, Korea, Taiwan. Only the European code was read: the other five are answered the same because nothing of the answer is that
    // binary's (0x100009 for ANY code under 1 MB + 9 bytes), and everything else of the guard is for them what it is for the European title.
    static const u64 kRegions[] = {0x0004001000020100ull, 0x0004001000021100ull, 0x0004001000022100ull,
                                   0x0004001000026100ull, 0x0004001000027100ull, 0x0004001000028100ull};
    constexpr unsigned kListed = sizeof(azl::ctr::kDownloadPlayTitles) / sizeof(azl::ctr::kDownloadPlayTitles[0]);
    CHECK_EQ(kListed, 6); // the list is these six titles and no other
    for (unsigned i = 0; i < kListed && i < 6; ++i) {
        CHECK_EQ(azl::ctr::kDownloadPlayTitles[i] == kRegions[i], 1);
    }
    const u32 trims_regions = azl::ctr::TextSearchTrims();
    for (const u64 region : kRegions) {
        g_title = region;
        CHECK_EQ(azl::ctr::IsDownloadPlayTitle(region), 1);
        // the title, the call, a code under 1 MB + 9 bytes of whatever size (a regional build is not the European one: one page, another count of pages, the last size that fails)
        for (const u32 size : {0x1000u, kDlplayCode, 0x00094000u, 0x000F0000u, 0x00100008u}) {
            g_real = size;
            CHECK_EQ(azl_text_size_guard(0, gsp), 0x00100009); // every region of the Download Play application gets through
            // ... and the SECOND call of GSP::Initialize, the size of the search, gets the real size: the search stays inside the code of THAT build
            CHECK_EQ(azl_text_size_guard(0, gsp_search), size); // the search of a regional build has its own size
        }
        // a regional build that passes the framework's test by itself: the real size, and the kernel is not asked
        const int asked_region = g_title_asked;
        g_real = 0x00100009;
        CHECK_EQ(azl_text_size_guard(0, gsp), 0x00100009);
        g_real = 0x00180000;
        CHECK_EQ(azl_text_size_guard(0, gsp), 0x00180000);
        CHECK_EQ(g_title_asked, asked_region);
        // the kernel does not say which title this is: as before the guard (the abort, never a guess)
        g_real = kDlplayCode;
        g_title_result = static_cast<Result>(0xD8E007F7);
        CHECK_EQ(azl_text_size_guard(0, gsp), kDlplayCode);
        g_title_result = 0;
        // another call in that title, and KeepThreadMain's search, cut as in every other title
        CHECK_EQ(azl_text_size_guard(0, other), kDlplayCode);
        CHECK_EQ(azl_text_size_guard(0x000FFFFC, site), kDlplayCode);
        CHECK_EQ(azl_text_size_guard(0x00120000, site), kDlplayCode - 0x20004);
    }
    CHECK_EQ(azl::ctr::TextSearchTrims(), trims_regions + 6); // (one cut for each region: the search after a match)
    // a title that differs from one of the six in ONE bit is another title: not in the list, left alone
    g_real = kDlplayCode;
    for (const u64 region : kRegions) {
        for (int bit = 0; bit < 64; ++bit) {
            const u64 near = region ^ (1ull << bit);
            bool listed = false;
            for (const u64 r : kRegions) {
                listed = listed || r == near;
            }
            g_title = near;
            CHECK_EQ(azl::ctr::IsDownloadPlayTitle(near), listed ? 1 : 0);
            CHECK_EQ(azl_text_size_guard(0, gsp), listed ? 0x00100009u : kDlplayCode); // one bit away is another title
        }
    }
    std::printf("Download Play: the six regional titles are answered 0x00100009 under 1 MB + 9 bytes, every title one bit away is left alone\n");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
