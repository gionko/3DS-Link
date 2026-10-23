// Construction of the per-call-site trampoline used by the svc hook (see ctr/hook.cpp).
// Portable on purpose: the host tests build the very same words and run them in an emulator.
#pragma once

#include "azl/arm_reloc.hpp"
#include "azl/common.hpp"

namespace azl {

constexpr u32 kWordsPerTrampoline = 8;

/// Words that replace `svc` + the following instruction at the call site: a jump to the trampoline.
constexpr u32 kSiteJumpWord = 0xE51FF004; // ldr pc, [pc, #-4]  (the next word is the target)

/// Builds the trampoline for the `svc` found at `site`.
///   next            the instruction that follows the svc (it is executed from the trampoline)
///   tramp_address   where the returned words will live
///   entry           address of the assembly entry point (svc_entry.s)
/// Layout: push {lr}; bl entry; pop {lr}; <next, relocated>; ldr pc, [pc, #-4]; .word site + 8
/// Returns false (with `reason`) if the instruction cannot be relocated or `entry` is out of range
/// for a `bl`.
bool BuildSvcTrampoline(u32 site, u32 next, u32 tramp_address, u32 entry, ReadWordFn read,
                        void* context, u32 words[kWordsPerTrampoline], const char** reason);

} // namespace azl
