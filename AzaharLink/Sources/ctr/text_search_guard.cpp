#include "ctr/text_search_guard.hpp"

// See text_search_guard.hpp. The real function is reached through the linker's --wrap.
extern "C" u32 __real__ZN18CTRPluginFramework7Process11GetTextSizeEv(void);

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

volatile u32 g_trims = 0;

} // namespace

// Called from text_search_guard.s with r0 = the caller's r6 and r1 = the return address.
extern "C" u32 azl_text_size_guard(u32 caller_r6, const u32* return_address) {
    const u32 size = __real__ZN18CTRPluginFramework7Process11GetTextSizeEv();
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
