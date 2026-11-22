#include "ctr/text_search_guard.hpp"

// See text_search_guard.hpp. The real function is reached through the linker's --wrap.
extern "C" u32 __real__ZN18CTRPluginFramework7Process11GetTextSizeEv(void);
// libctru's (on a PC, the test's). The guard asks the kernel for its own title: it runs inside the framework's start, and must not depend on
// what the plugin's EarlyInit or the framework's ProcessImpl have already read.
extern "C" Result svcGetProcessInfo(s64* out, Handle process, u32 type);

namespace {

// libctrpf starts the game's code at this address (it is a literal in its own search calls).
constexpr u32 kTextBase = 0x00100000;
// Utils::Search with a window shorter than the pattern (4 bytes) would wrap around, so it gets 0.
constexpr u32 kPatternBytes = 4;

// KeepThreadMain, after the call:   bl GetTextSize ; mov r7, r0 ; add r6, r6, #4
// and before the call to Utils::Search(r6, size, pattern). r6 is "the last match", so the search
// that follows starts at r6 + 4. The call sites of the other users of GetTextSize do not have
// these two words after them.
constexpr u32 kMovR7R0 = 0xe1a07000;
constexpr u32 kAddR6R6Imm4 = 0xe2866004;

// Services::GSP::Initialize, when the libctru wrapper of GSPGPU::RegisterInterruptRelayQueue is not in the title, loops on the Nintendo SDK one:
//     bl GetTextSize ; ldrd r2, [sp, #12] ; sub r3, r3, r2 ; sub r0, r0, r3, asr #2 ; cmp r0, r4 ; bcc <return -1>
//     bl GetTextSize ; add r2, sp, #12 ; mov r1, r0 ; mov r0, #0x100000 ; bl Utils::Search        (a SECOND call: the size of the search is not the first one's)
// r0 is the size of the code less the words of the pattern (9), but r4 is an ADDRESS: 0x100000 the first time round, the last match after it. A title
// whose code is shorter than 1 MB + 9 bytes fails the test before any search, GSP::Initialize answers -1 and KeepThreadMain calls abort(): the Download
// Play application (0x93000 bytes of code; "Fatal plugin error" on a console, 2026-11-13).
// For that application only, and only when the test would fail, the first call is answered kTextBase + 9: the smallest value that passes the first time round,
// and one that fails for every later match (r4 above 0x100000), where the framework would search again from 0x100000, find the same match and never
// leave the loop: a wrapper it does not know ends in the abort() of today, not in a black screen. The search is not touched: its size comes from the
// second call, which gets the real value like every other caller, so it reads the title's own code and nothing after it.
// That is why the answer is given to all six regional titles of the application (2026-11-14) though only the European code was read: nothing in it is
// an address or a size of that binary. In a regional build the guard has never seen, the framework's one search either finds the SDK wrapper with its
// command header (the hook every game gets), or finds nothing, or finds nine words that are not the wrapper - and those two end where they ended before
// the guard answered, in the -1 and the abort(). (The one match that would pass the test again is one at 0x100000 itself, the first word of the title's
// code, where a 3DS application has its start code and not a wrapper of GSP.)
constexpr u32 kLdrdR2R3Sp12 = 0xe1cd20dc;
constexpr u32 kSubR3R3R2 = 0xe0433002;
constexpr u32 kSubR0R0R3Asr2 = 0xe0400143;
constexpr u32 kCmpR0R4 = 0xe1500004;
constexpr u32 kBccMask = 0xff000000; // (how far the branch goes is the framework's business)
constexpr u32 kBcc = 0x3a000000;
constexpr u32 kSdkPatternWords = 9;
// Who is answered: the system's Download Play application, in its six regional titles (azl::ctr::kDownloadPlayTitles, text_search_guard.hpp: the list is
// there, once, for this file and for the plugin's start line). One application, on purpose: opening this to every title under 1 MB is a decision of its own.
constexpr Handle kOwnProcess = 0xFFFF8001; // CUR_PROCESS_HANDLE
constexpr u32 kTitleIdInfo = 0x10001;      // Luma3DS: the title id of the process (the framework's own ProcessImpl::Initialize asks the same)

volatile u32 g_trims = 0;

bool IsGspSdkTest(const u32* return_address) {
    return return_address[0] == kLdrdR2R3Sp12 && return_address[1] == kSubR3R3R2 && return_address[2] == kSubR0R0R3Asr2 &&
           return_address[3] == kCmpR0R4 && (return_address[4] & kBccMask) == kBcc;
}

bool IsDownloadPlay() {
    s64 title = 0;
    return svcGetProcessInfo(&title, kOwnProcess, kTitleIdInfo) == 0 && azl::ctr::IsDownloadPlayTitle(static_cast<u64>(title));
}

} // namespace

// Called from text_search_guard.s with r0 = the caller's r6 and r1 = the return address.
extern "C" u32 azl_text_size_guard(u32 caller_r6, const u32* return_address) {
    const u32 size = __real__ZN18CTRPluginFramework7Process11GetTextSizeEv();
    if (IsGspSdkTest(return_address)) {
        // (the framework's own sum, unsigned: a title of 1 MB + 9 bytes or more passes by itself and is never asked who it is)
        if (size - kSdkPatternWords >= kTextBase || !IsDownloadPlay()) {
            return size;
        }
        return kTextBase + kSdkPatternWords;
    }
    if (return_address[0] != kMovR7R0 || return_address[1] != kAddR6R6Imm4) {
        return size;
    }
    const u32 start = caller_r6 + 4;
    if (start <= kTextBase) {
        return size; // the search from the beginning of the code: the size is already right
    }
    const u32 used = start - kTextBase;
    const u32 left = used < size ? size - used : 0;
    const u32 cut = left < kPatternBytes ? 0 : left;
    if (cut != size) {
        ++g_trims;
    }
    return cut;
}

namespace azl {
namespace ctr {

u32 TextSearchTrims() {
    return g_trims;
}

} // namespace ctr
} // namespace azl
