// The plan of the gate's page: where the persistent gate of the svc hook (Sources/ctr/gate.s) goes in the game's process, what its
// stubs are, and how a call site is turned into ONE word (see the top of gate.s for the gate itself).
//
// Every hooked call site of the game holds one word, `b stub_i`, written once before the game runs. Every stub is three words:
//     str lr, [sp, #-4]!  ;  bl azl_gate_32 (or azl_gate_23)  ;  b site_i + 4
// The stubs follow the gate's code in a page of the game's process just below 0x01E80000 (where CTRPF and Luma's cheats have
// their page): the swap of the plugin's block on an Old 3DS unmaps 0x07000000 and the plugin's heap, never this page. After
// WriteGate nothing in the game's text or in the page's code is written again; the swap only flips the gate's flag
// (azl/hook_swap.hpp: GateClose, GateOpen).
// Everything here is plain code over injected memory and an injected query of the address space, so that the host tests
// (Tests/gate_plan_test.cpp) run the very functions the plugin runs.
#pragma once

#include <cstddef>

#include "azl/common.hpp"

namespace azl {

// The gate's header, as laid out by Sources/ctr/gate.s (offsets from azl_gate_begin; Tests/hook_sim/gate_sim.py checks every
// one of them against the symbols of the assembled gate).
constexpr u32 kGateFlag = 0x00;        ///< 1 = the plugin is there (written by the plugin only); the gate reads it at in_flight - 4
constexpr u32 kGateInFlight = 0x04;    ///< game threads between the count going up and coming down
constexpr u32 kGatePre32 = 0x08;       ///< pre/post of svc 0x32 (SendSyncRequest) and svc 0x23 (CloseHandle), in the plugin's block
constexpr u32 kGatePost32 = 0x0C;
constexpr u32 kGatePre23 = 0x10;
constexpr u32 kGatePost23 = 0x14;
constexpr u32 kGateLastSite = 0x18;    ///< stub+8 of the last thread that went into the plugin (counted, flag 1: for the line of a close that times out)
constexpr u32 kGateLastHandle = 0x1C;  ///< ... and its handle
constexpr u32 kGatePostSkipped = 0x20; ///< posts not called because the flag was 0 after the svc
constexpr u32 kGateFakeHandles = 0x24; ///< 7 x {handle, result}: the fake sessions the gate answers itself with the flag 0 (gate.s)
constexpr u32 kGateFakeSlots = 7;
constexpr u32 kGateFakeHits = 0x5C;    ///< svc 0x32 on a fake handle answered by the gate with the flag 0
constexpr u32 kGateFakeClosed = 0x60;  ///< bit i: slot i's handle closed with the flag 0
constexpr u32 kGateCode = 0x64;        ///< azl_gate_32, the first instruction
constexpr u32 kGateEntry23 = 0x25C;    ///< azl_gate_23
constexpr u32 kGateBytes = 1048;       ///< azl_gate_end - azl_gate_begin

/// The header as a struct, for the plugin's side of the gate (azl/hook_swap.hpp). Its words are read and written atomically
/// by the game's threads in the gate (ldrex/strex) and by the plugin.
struct GateHeader {
    volatile u32 flag;
    volatile u32 in_flight;
    u32 pre32;
    u32 post32;
    u32 pre23;
    u32 post23;
    volatile u32 last_site;
    volatile u32 last_handle;
    volatile u32 post_skipped;
    struct FakeSlot {
        volatile u32 handle; ///< 0: free (the plugin writes the result first, then the handle; clears the handle to free it)
        volatile u32 result; ///< what the gate puts in cmdbuf[1] for a call on it with the flag 0
    } fake[kGateFakeSlots];
    volatile u32 fake_hits;
    volatile u32 fake_closed;
};
static_assert(offsetof(GateHeader, flag) == kGateFlag, "gate.s: flag");
static_assert(offsetof(GateHeader, in_flight) == kGateInFlight, "gate.s: in_flight (right after flag)");
static_assert(offsetof(GateHeader, pre32) == kGatePre32 && offsetof(GateHeader, post32) == kGatePost32, "gate.s: pre32/post32");
static_assert(offsetof(GateHeader, pre23) == kGatePre23 && offsetof(GateHeader, post23) == kGatePost23, "gate.s: pre23/post23");
static_assert(offsetof(GateHeader, last_site) == kGateLastSite && offsetof(GateHeader, last_handle) == kGateLastHandle, "gate.s: last_site/last_handle");
static_assert(offsetof(GateHeader, post_skipped) == kGatePostSkipped, "gate.s: post_skipped");
static_assert(offsetof(GateHeader, fake) == kGateFakeHandles && offsetof(GateHeader, fake_hits) == kGateFakeHits &&
                  offsetof(GateHeader, fake_closed) == kGateFakeClosed && sizeof(GateHeader) == kGateCode,
              "gate.s: the fake sessions' table, then the code");

constexpr u32 kGatePageBytes = 0x1000;
constexpr u32 kGateStubBytes = 12;
constexpr u32 kGateCeiling = 0x01E80000;    ///< CTRPF's and Luma's page (0x2000 bytes): the gate's page always ends at or below it
constexpr u32 kFrameworkPageBytes = 0x2000;
constexpr u32 kGateFloor = 0x00100000;      ///< the start of the game's image: the page never goes below
constexpr u32 kGateMemFree = 0;             ///< MEMSTATE_FREE of svcQueryMemory
constexpr u32 kGateMaxQueries = 256;        ///< ChooseAddress gives up after so many queries (a real map needs a handful)
constexpr u32 kNoGateSite = 0xFFFFFFFFu;

constexpr u32 kStubSaveLr = 0xE52DE004;    ///< str lr, [sp, #-4]!
constexpr u32 kGateSvc32Word = 0xEF000032;
constexpr u32 kGateSvc23Word = 0xEF000023;

/// `b` (or `bl`) at `from` to `to`, in ARM state. False when `to` is beyond the +-32 MB of the instruction or not on a word.
inline bool EncodeBranch(u32 from, u32 to, u32* word, bool link = false) {
    const s64 offset = static_cast<s64>(to) - (static_cast<s64>(from) + 8);
    if ((offset & 3) != 0 || offset < -0x2000000 || offset > 0x1FFFFFC) {
        return false;
    }
    *word = (link ? 0xEB000000u : 0xEA000000u) | (static_cast<u32>(offset >> 2) & 0x00FFFFFFu);
    return true;
}

/// The three words of the stub at `stub` for the site at `site`: `str lr, [sp, #-4]!`, `bl gate_entry`, `b site + 4`.
inline bool BuildStub(u32 site, u32 gate_entry, u32 stub, u32 words[3]) {
    words[0] = kStubSaveLr;
    return EncodeBranch(stub + 4, gate_entry, &words[1], true) && EncodeBranch(stub + 8, site + 4, &words[2]);
}

/// Pages for the gate's code and `sites` stubs.
inline u32 PageCount(u32 sites, u32 gate_bytes = kGateBytes) {
    return (gate_bytes + sites * kGateStubBytes + kGatePageBytes - 1) / kGatePageBytes;
}

inline u32 StubsBase(u32 page, u32 gate_bytes = kGateBytes) {
    return page + ((gate_bytes + 3) & ~3u);
}

/// The site whose stub called the gate: `stub_ret` is what the stub's `bl` left in lr (stub + 8). kNoGateSite for anything else.
inline u32 SiteIndex(u32 stub_ret, u32 stubs_base, u32 count) {
    if (stub_ret < stubs_base + 8) {
        return kNoGateSite;
    }
    const u32 offset = stub_ret - 8 - stubs_base;
    if (offset % kGateStubBytes != 0 || offset / kGateStubBytes >= count) {
        return kNoGateSite;
    }
    return offset / kGateStubBytes;
}

/// Whether every site in [lo_site, hi_site] and every stub in [base, base + size) reach each other (the site's `b` to its stub,
/// the stub's `b` back to site + 4). The extremes are enough: the offsets are linear in both.
inline bool GateInReach(u32 lo_site, u32 hi_site, u32 base, u32 size) {
    const u32 stubs[2] = {base, base + size - kGateStubBytes};
    const u32 sites[2] = {lo_site, hi_site};
    u32 word = 0;
    for (u32 stub : stubs) {
        for (u32 site : sites) {
            if (!EncodeBranch(site, stub, &word) || !EncodeBranch(stub + 8, site + 4, &word)) {
                return false;
            }
        }
    }
    return true;
}

/// One answer of the injected query (svcQueryMemory on the console): the span that holds the address asked, and its state.
struct GateSpan {
    u32 base;
    u32 size;
    u32 state;
};

/// How the gate's page is taken (Sources/ctr/gate_page.cpp):
///   * Alias: memory of the plugin's mapped there (svc 0xA0, as CTRPF maps its own page): Azahar, and a New 3DS with a Luma3DS older than
///     13.0 (as the build of 1.1.2 that ran on Friend-A's New 3DS XL: svc 0xA3 may not be there, and Luma3DS swaps the plugin's block only
///     on an Old 3DS). Not the way of choice on a console: the mapping (state 0x5806, not PRIVATE) is never counted by the kernel's books
///     of the process, and an alias left mapped OUTLIVES the game: after the game's process is gone its pages stay taken in the APPLICATION
///     region until a reboot, and the next start of a game fails to map the plugin's memory ("Cannot map plugin memory", 0xD86007F3) or
///     an application that asks for all the memory to the byte stops at its start (proved on console 63, New 2DS XL, Luma3DS 13.4.0,
///     2026-11-06: three starts in a row with the System way, the first one fails with the alias; AzaharLink/analysis/
///     friend_a-crash-uscita-gioco-2026-11-06/ANALISI.md, "Prove decisive"). The reading of the decompiled kernel that said the end of the
///     game's KProcess gives the pages back (IncrefPages at the map, KProcessPageTable::Finalize) was WRONG: it is no guarantee. CTRPF and
///     Luma3DS always unmap their aliases themselves; this one is not unmapped yet (open: the New 3DS below 13.0). Never a fallback for a
///     SYSTEM page that failed (an alias there would outlive every such launch; the launch goes without hooks instead).
///   * System: a block of its own in the SYSTEM region with Luma3DS's svc 0xA3, on the kernel's books (state 0xBB05, PRIVATE: the sum
///     KProcess::Finalize gives back counts it, and RaiseBooks has put it there), freed with the process: an Old 3DS and a New 3DS, from
///     Luma3DS 13.0. Where it fails TakeGatePage takes no other page: no hooks for that launch, on an Old and a New 3DS alike.
///   * None: an Old 3DS with an older Luma3DS (or none): svc 0xA3 may not be there, and an svc the kernel does not know is a kernel panic
///     (SvcStopPointOrInvalidId); an alias there is memory of the plugin's block, which the swap takes away under a thread asleep in the
///     gate (the very defect the gate is for). No page and no hooks for the launch.
enum class GateWay : u32 { Alias, System, None };

/// Luma3DS 13.0 (svcGetSystemInfo(0x10000, 0): major << 24 | minor << 16 | build << 8): its own plugin loader, which takes the plugin's block
/// of an Old 3DS with svc 0xA3 itself (sysmodules/rosalina/source/plugin/memoryblock.c), so a console that loads the plugin with it has it.
constexpr u32 kLumaWithControlMemoryUnsafe = 13u << 24;

/// Four ways out: Azahar -> Alias; from Luma3DS 13.0 -> System (Old and New); before it, a New 3DS -> Alias and an Old 3DS -> None.
/// The only ways to the alias: a System that fails has no fallback (Sources/ctr/gate_page.cpp: no hooks for that launch).

inline GateWay ChooseGateWay(bool citra, bool is_new, u32 luma_version, const char** reason) {
    if (citra) {
        *reason = "Azahar: no swap of the plugin's block";
        return GateWay::Alias;
    }
    if (luma_version < kLumaWithControlMemoryUnsafe) {
        if (is_new) {
            *reason = "a New 3DS with a Luma3DS older than 13.0: svc 0xA3 may not be there, and no swap of the plugin's block";
            return GateWay::Alias;
        }
        *reason = "an Old 3DS with a Luma3DS older than 13.0: svc 0xA3 may not be there (an unknown svc is a kernel panic)";
        return GateWay::None;
    }
    *reason = is_new ? "New 3DS: a page on the kernel's books, freed with the game (an alias stays out of the books until the game's end)"
                     : "Old 3DS: the swap takes the plugin's block away";
    return GateWay::System;
}

enum class GatePlace : u32 { Found, NoRoom, OutOfReach, QueryFailed, BadInput };

inline const char* GatePlaceName(GatePlace p) {
    switch (p) {
    case GatePlace::Found: return "found";
    case GatePlace::NoRoom: return "no free room below 0x01E80000";
    case GatePlace::OutOfReach: return "no free room within reach of the sites";
    case GatePlace::QueryFailed: return "the memory query failed";
    default: return "bad input";
    }
}

struct GatePlaceInfo {
    GatePlace result = GatePlace::BadInput;
    u32 queries = 0;
};

/// Where the gate's page of `size` bytes goes: the highest place, ending at or below 0x01E80000 (never in CTRPF's page above it),
/// that is all FREE and from which every site in [min_site, max_site] is within reach. `query(address, GateSpan*)` returns false when
/// the address space cannot be asked. Returns 0 when there is no such place (`info` says why).
template <class Query>
inline u32 ChooseAddress(Query query, u32 min_site, u32 max_site, u32 size, GatePlaceInfo* info = nullptr) {
    GatePlaceInfo local;
    GatePlaceInfo& out = info ? *info : local;
    out = GatePlaceInfo{};
    if (size == 0 || size % kGatePageBytes != 0 || min_site > max_site || (min_site & 3) != 0 || (max_site & 3) != 0) {
        out.result = GatePlace::BadInput;
        return 0;
    }
    bool out_of_reach = false;
    u32 top = kGateCeiling; // the highest end the page may have
    while (out.queries < kGateMaxQueries && top >= kGateFloor + size) {
        const u32 base = top - size;
        GateSpan span{};
        ++out.queries;
        if (!query(base, &span) || span.base > base || static_cast<u64>(span.base) + span.size <= base) {
            out.result = GatePlace::QueryFailed;
            return 0;
        }
        if (span.state != kGateMemFree) {
            top = span.base & ~(kGatePageBytes - 1); // below this mapping
            continue;
        }
        if (static_cast<u64>(span.base) + span.size < top) {
            top = (span.base + span.size) & ~(kGatePageBytes - 1); // the free span ends before the page would
            continue;
        }
        if (!GateInReach(min_site, max_site, base, size)) {
            out_of_reach = true;
            // lower is nearer the game's text: straight to the highest end the lowest site reaches, or one page down
            const u32 reach_top = (min_site + 0x2000000u) & ~(kGatePageBytes - 1);
            top = reach_top < top ? reach_top : top - kGatePageBytes;
            continue;
        }
        out.result = GatePlace::Found;
        return base;
    }
    out.result = out_of_reach ? GatePlace::OutOfReach : GatePlace::NoRoom;
    return 0;
}

/// One call site to hook: its address and the word that is there (svc 0x32 or svc 0x23).
struct GateSite {
    u32 address;
    u32 original;
};

/// The gate's code to copy (the plugin: azl_gate_begin .. azl_gate_end) and where its two entries are, from its start.
struct GateBlob {
    const u32* words;
    u32 bytes;
    u32 entry32;
    u32 entry23;
};

/// pre/post of the plugin (addresses in the plugin's block).
struct GatePointers {
    u32 pre32;
    u32 post32;
    u32 pre23;
    u32 post23;
};

struct GateWriteResult {
    bool ok = false;        ///< false: nothing was written to the game's text (bad page, bad blob)
    u32 hooked = 0;         ///< sites with a stub (the first `hooked` entries of the array, in the order of their stubs); all but
                            ///< `patch_failed` of them now hold `b stub`
    u32 out_of_reach = 0;   ///< sites left as they are: their stub is beyond the 32 MB of a `b`
    u32 foreign = 0;        ///< sites left as they are: the word there is not the svc that was found (somebody else changed it)
    u32 no_room = 0;        ///< sites left as they are: no room for their stub in the page
    u32 patch_failed = 0;   ///< sites whose word could not be written (their stub stays, unused)
};

/// Writes the gate into its page and hooks the sites, once, before the game runs:
///   1. the gate's code, its header at zero (flag 0, in_flight 0), the pointers to pre/post, one stub per site; the caches (Sync);
///   2. flag = 1;
///   3. ONE word per site, `b stub_i`, and the caches again.
/// The sites that are hooked are moved to the front of `sites`, in the order of their stubs (SiteIndex finds them there); the
/// others are counted and left as they are. Memory (`m`): Load(address) reads the game's text, StorePage(address, word) writes
/// the page, PatchText(address, word) writes ONE word of the game's text (false when it could not), Sync() cleans the data
/// cache and drops the instruction cache. Tests/gate_plan_test.cpp runs this very function.
template <class Mem>
inline GateWriteResult WriteGate(Mem& m, u32 page, u32 page_bytes, const GateBlob& blob, const GatePointers& p, GateSite* sites,
                                 u32 count) {
    GateWriteResult r;
    if (page == 0 || page % kGatePageBytes != 0 || page_bytes == 0 || page_bytes % kGatePageBytes != 0 || blob.words == nullptr ||
        blob.bytes % 4 != 0 || blob.bytes < kGateCode || blob.bytes > page_bytes || blob.entry32 < kGateCode ||
        blob.entry23 < kGateCode || blob.entry32 >= blob.bytes || blob.entry23 >= blob.bytes) {
        return r;
    }
    r.ok = true;
    for (u32 i = 0; i < blob.bytes / 4; ++i) {
        m.StorePage(page + 4 * i, i < kGateCode / 4 ? 0u : blob.words[i]); // (the header starts at zero: flag 0, nobody counted)
    }
    m.StorePage(page + kGatePre32, p.pre32);
    m.StorePage(page + kGatePost32, p.post32);
    m.StorePage(page + kGatePre23, p.pre23);
    m.StorePage(page + kGatePost23, p.post23);
    const u32 stubs = StubsBase(page, blob.bytes);
    const u32 room = (page + page_bytes - stubs) / kGateStubBytes;
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
        const GateSite site = sites[i];
        if (kept >= room) {
            ++r.no_room;
            continue;
        }
        const bool is32 = site.original == kGateSvc32Word;
        if ((!is32 && site.original != kGateSvc23Word) || m.Load(site.address) != site.original) {
            ++r.foreign;
            continue;
        }
        const u32 stub = stubs + kept * kGateStubBytes;
        u32 words[3];
        u32 jump = 0;
        if (!BuildStub(site.address, page + (is32 ? blob.entry32 : blob.entry23), stub, words) ||
            !EncodeBranch(site.address, stub, &jump)) {
            ++r.out_of_reach;
            continue;
        }
        m.StorePage(stub, words[0]);
        m.StorePage(stub + 4, words[1]);
        m.StorePage(stub + 8, words[2]);
        sites[kept++] = site;
    }
    m.Sync();
    m.StorePage(page + kGateFlag, 1u);
    m.Sync();
    for (u32 i = 0; i < kept; ++i) {
        u32 jump = 0;
        EncodeBranch(sites[i].address, stubs + i * kGateStubBytes, &jump); // (in reach: checked above)
        if (!m.PatchText(sites[i].address, jump)) {
            ++r.patch_failed;
        }
    }
    m.Sync();
    r.hooked = kept;
    return r;
}

} // namespace azl
