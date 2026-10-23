// Relocation of a single ARM (32-bit) instruction so that it can be executed from a trampoline
// instead of its original place. Used by the svc hook: the game's `svc 0x32` is replaced together
// with the instruction after it, and that instruction has to live on in the trampoline.
//
// Only what is needed to stay correct is supported; anything position dependent that cannot be
// rewritten safely makes RelocateArm() fail, and the caller leaves that call site alone.
#pragma once

#include "azl/common.hpp"

namespace azl {

/// Reads a 32-bit word at a game address (used to copy literal-pool constants).
using ReadWordFn = u32 (*)(void* context, u32 address);

struct RelocResult {
    bool ok = false;
    const char* reason = ""; ///< why it failed (static string), when !ok
    u32 words[4] = {};       ///< position independent code, relative to its own first word
    int count = 0;           ///< 3 or 4 words
};

/// Builds code equivalent to `insn` executing at `address`, followed by a jump to `resume`.
///
/// Layout of the result: words[0] = the instruction (or its rewritten form), words[1] =
/// `ldr pc, [pc, #-4]`, words[2] = resume, words[3] = an optional literal used by words[0].
RelocResult RelocateArm(u32 insn, u32 address, u32 resume, ReadWordFn read, void* context);

} // namespace azl
