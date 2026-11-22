// The composer of the Download Play child the app prepares with boot9.bin (1.1.4: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 3.3-3.6 and
// 4.3, with its critic 6-APP-boot9-CRITICO.md 4.5). Portable C++: every byte comes in and goes out through the caller's functions (the child's CIA in a
// game's content 2, the work file on the SD card, the AM sink), so it runs the same on the console and in Tests/settings_test/dlp_guest_test.cpp.
//   Compose: reads the CIA, checks it, takes the title key and the NCCH layer off the content while it streams it into the work file, with the changes
//            of dlp_guest_logic.hpp, and keeps the new front (header, certificates, ticket, TMD) and the meta block. Nothing is written but the work file,
//            and a content whose SHA-256 is not its TMD's gives kErrSourceHash: then the caller installs nothing.
//   Emit:    gives the AM sink the new CIA in its order: front, the work file, the padding, the meta block. Same length as the original.
// Composed is ~31 KB: it lives on the heap (new), never on the 64 KB stack of the worker's thread. The keys are wiped before Compose returns.
// A child of method 1 (the build 2e slot25): Compose asks the caller for boot.firm (FirmFn) and finds the KeyX of slot 0x25 in it (FindSlot25);
// Composed says how that went, and the caller writes the line of the log (CryptoLine) - never the key, the offset where it was.
#pragma once

#include <cstddef>
#include <cstdint>

#include "dlp_guest_logic.hpp"

namespace dlpguest {

using ReadFn = bool (*)(void* ctx, u64 off, u8* buf, u32 len);        // false = not all of it
using WriteFn = bool (*)(void* ctx, u64 off, const u8* buf, u32 len);  // the work file (by offset); the AM sink is sequential
using StepFn = bool (*)(void* ctx, int stage, u64 done, u64 total);    // false = cancel
/// boot.firm for a child of method 1: true = `data` and `size` given (the caller's, read once, read only, kept until its work ends and wiped then);
/// false = none (not on the SD card nor in the CTRNAND, or no memory: the caller's log says which). Asked only for a child of method 1.
using FirmFn = bool (*)(void* ctx, const u8** data, std::size_t* size);
using ClockFn = u64 (*)();                                              // milliseconds, for the line of the log (may be null)

enum Stage : int { kStageRead = 0, kStageDecrypt = 1, kStageInstall = 2, kStageFinish = 3 };

constexpr std::size_t kHashedMax = 0x4000;                               // the ExeFS bytes 0x1C0 may cover (a child has 0x200)
constexpr std::size_t kWorkBytes = (kHashedMax + 48) + (kSmdhSize + 48);  // the look before the stream: the ExeFS's hashed bytes and the icon

struct ComposeIn {
    ReadFn read = nullptr;  // the child's CIA: `cia_at` is where it starts in what `read` reads
    void* rctx = nullptr;
    u64 cia_at = 0, cia_size = 0;
    const Keys* keys = nullptr;
    u64 prepared = 0;                 // the prepared title (azl::title_rules::PreparedOf of the game): the exheader's ids, the id of the process
    u64 am_title = 0;                 // (the build 2f) the id AM installs it by: the ticket, the TMD, the NCCH's 0x108/0x118; 0 = `prepared`
    u8 variant = kDefaultVariant;     // kHide, always, on the console
    WriteFn temp = nullptr;           // the work file: the plain content, written once, in order
    void* tctx = nullptr;
    StepFn step = nullptr;            // may be null
    void* sctx = nullptr;
    u8* buf = nullptr;                // the stream's chunk: a multiple of 16 bytes
    std::size_t buf_size = 0;
    u8* work = nullptr;               // at least kWorkBytes
    std::size_t work_size = 0;
    FirmFn firm = nullptr;            // (method 1) boot.firm; null: a child of method 1 is kErrChildCrypto, step "firm missing"
    void* fctx = nullptr;
    ClockFn clock_ms = nullptr;       // may be null (the search's time is then 0)
};

struct Composed {
    CiaLayout lay;
    TicketFacts tik;  // as read (the title id of the child)
    TmdFacts tmd;     // as read
    u8 front[0x4000]; // [0, lay.content_at) of the new CIA
    u8 meta[kMetaMax];
    u8 new_hash[32];
    u32 icon_flags_before;
    bool hidden;
    // (the build 2e slot25) for the line of the log: the child's 0x188-0x18F (plain: flags, nothing secret), what ReadNcch said, and for method 1
    // the step of the search in boot.firm, where the KeyX was (an offset, never the key), how many candidates, how long
    u8 ncch_flags[8];
    NcchWhy why;
    Slot25Step slot25;
    u32 slot25_at, slot25_candidates, slot25_ms;
};

constexpr std::size_t kCryptoLineBytes = 192;  // (the console's LogLine holds 200)

/// The line of the log of the children for a child whose layer was not opened (kErrChildCrypto) or that needed the KeyX of slot 0x25 (the caller's,
/// after the step "compose"): "guest: child crypto <child> game <game> flags <0x188-0x18F> why <case> (...)" with the step of the search in
/// boot.firm, or "... slot 0x25 key found at 0x<offset>, <n> candidates, <ms> ms". Never a key. No printf: the composer writes nothing.
void CryptoLine(const Composed& c, u64 game, char* out, std::size_t n);

/// Fills `out` from boot9.bin (KeysFromBoot9) and the built-in values (the key generator's constant and the common KeyY 0, bk_get): 0, or
/// kErrBoot9NoKeys (then `out` is zeros). bk_get also readies aes.c's tables once (builtin_keys.c aes_ready), before any CTR of Compose.
int ComposeKeys(const u8* boot9, std::size_t size, Keys* out);

/// 0 or a kErr* of dlp_guest_logic.hpp.
int Compose(const ComposeIn& in, Composed* out);

/// 0, kErrTemp (the work file), kErrInstall (the sink) or kErrCancel. `buf`/`n`: a chunk of any size.
int Emit(const Composed& c, ReadFn temp_read, void* t, WriteFn sink, void* s, StepFn step, void* sc, u8* buf, std::size_t n);

} // namespace dlpguest
