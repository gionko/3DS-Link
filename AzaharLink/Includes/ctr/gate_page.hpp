// The gate's page of the svc hook (Sources/ctr/gate.s, azl/gate_plan.hpp): taken once, before the game runs, in the game's process just
// below CTRPF's page at 0x01E80000, where the swap of the plugin's block on an Old 3DS never reaches (Luma3DS unmaps 0x07000000 and the
// plugin's heap only). See Sources/ctr/gate_page.cpp for the ways it is taken, and why a SYSTEM page that fails has no fallback.
#pragma once

#include <3ds/types.h>

namespace azl {
namespace ctr {

enum class GatePageKind : u32 {
    None,   ///< no page: the hooks stay out for this launch
    System, ///< a block of its own (Luma3DS's svc 0xA3, SYSTEM region, RWX), put on the kernel's books of the process
    Alias,  ///< an alias of memory of the plugin's (svcMapProcessMemoryEx, as CTRPF maps its own page): Azahar, and a New 3DS with a Luma3DS
            ///< older than 13.0; never where the above failed (an alias outlives the game: no hooks for that launch instead)
};

struct GatePage {
    u32 address = 0;
    u32 bytes = 0;
    GatePageKind kind = GatePageKind::None;
    bool books_raised = false; ///< RaiseBooks(bytes) was made: SettleGateBooks owes the limit its bytes back once the game has its heap
    s32 commit_before = 0;     ///< the books of the process before RaiseBooks (for the line of SettleGateBooks)
    s32 limit_before = 0;
};

/// EarlyInit (InstallSvcHooks), before the game's first instruction: the page of `bytes` for the call sites in [min_site, max_site].
/// Logs one line ("gate: page at ..." or "gate: no hooks: ..."). With no page, `why` says why (for the notice on the screen).
GatePage TakeGatePage(u32 min_site, u32 max_site, u32 bytes, const char** why);

/// LateInit, on the plugin's main thread: once the game has its heap, the limit of the books goes back down by the page (the room the game
/// saw while it sized its heap was the room it would have had without the page). Logs "gate: books ...". Nothing for an alias, and nothing on
/// a New 3DS in memtype 6, where pm puts the limit back itself at the end of the application (books::SettleNeeded).
void SettleGateBooks(const GatePage& page);

/// The EXIT event (the framework's keep-alive thread, small stack): the Settle that LateInit has not made yet, once (they all share one
/// claim). Never waits for the lock of the books. No line: LogGateBooksAtEvents says it later.
void SettleGateBooksAtExit();
/// SWAP_ENTER (an Old 3DS, HOME; the keep-alive thread): the same, but only once the game has its heap. A game closed from the HOME menu
/// gets its EXIT event too (Luma3DS maps the plugin's block back first): this is a net for an end with no EXIT event. No line.
void SettleGateBooksAtSwap();
/// What the last of those two did with the books, in one line, once (the writer when it reports SWAP_ENTER, the shutdown).
void LogGateBooksAtEvents();

} // namespace ctr
} // namespace azl
