// The Download Play children 3DS Link prepared on this console (1.1.4: G2 and A3 of analysis/piano-1.1.4-2026-11-15/specifiche, one header for the
// two of them, contract K2-K4 of 7-CONTRATTO.md). The app (ConfigApp) installs, for every game turned on in Enable Titles that has a Download Play
// child, a hidden PREPARED CHILD (title_rules::PreparedOf of the game) and writes two kinds of files for the plugin of the system's Download Play
// application, which reads them; nothing else writes or reads them:
//   - the INDEX, kIndexPath: the line kIndexHeader, then one line per child, "<child>:<game>:<prepared>:<S|C>:<version>" (three ids of 16 hex
//     digits, S = the game on the SD card, C = on the game card - the prepared child is always on the SD -, the child's TMD version in decimal),
//     at most kRowBytes bytes, at most kMaxRows lines; prepared 0 = the game is there but its child is not prepared (yet). Lines that begin
//     with '#' are skipped (a tag of a later version). The app writes it again at every start (WriteTextFile: a .tmp and a .bak); the plugin
//     reads the main file only, once, on its own thread.
//   - a BLOCK per prepared child, kGuestsDir + BlockName(prepared): kBlockBytes bytes, little-endian units as the SMDH and the records of
//     dlp:CLNT hold them - the child's short and long names in the console's language and its big icon (from the SMDH of the meta block of the
//     child's CIA, dlp_real::SmdhTitleEntry) and the two sizes a host announces for it (frame 0 +0x38/+0x3C: EXACTLY dlp_real::FindChildPackage's
//     transfer_size = where the CIA's meta block starts, required_size = the CIA's length, or "another version" is said where it is not).
//     The plugin reads a block lazily: only the one of the child a host it heard announces, on the plugin's thread, never on a game's thread.
// What a host's child is on this console - ready, not prepared, another version... - is the Table's (VerdictFor), and the small line under the
// host's name in the Download Play application (DLPServerInfo node_info[0].username, 10 UTF-16 units) says it with a mark: MakeLabel, which
// shortens the name and never the mark.
// Pure header: no file, no log, no allocation, no lock; the readers never read past what they are given. The part G1's writers of dlp:CLNT call
// on the Download Play application's thread is marked as game-thread code (Tests/policy_check.sh).
#pragma once

#include <atomic>
#include <cstddef>
#include <initializer_list>

#include "azl/common.hpp"
#include "azl/dlp_title_block.hpp"  // (the SMDH: its title entries and its big icon)
#include "azl/dlp_types.hpp"        // (the offsets of the records of dlp:CLNT, kNameUnits)
#include "azl/title_rules.hpp"      // (PreparedOf: the prepared child of a game)

namespace azl {
namespace dlp_children {

// ---- the files ----------------------------------------------------------------------------------------------------------------------
/// The folder of the files the app writes for the plugin (CTRPF paths, as the plugin's kConfigPath; the app's are "sdmc:" + these). Not
/// /3ds/3DSLink/dlp/, which is 3DS Link Delivery's.
inline constexpr char kGuestsDir[] = "/3ds/3DSLink/dlp-guests/";
inline constexpr char kIndexPath[] = "/3ds/3DSLink/dlp-guests/index.txt";
inline constexpr char kIndexHeader[] = "#dlp-children 1";
/// What the plugin reads of the index at most (Plugin::ReadDataFile cuts at the last whole line).
inline constexpr std::size_t kIndexMaxBytes = 1024;
/// The children kept: the app's index, the plugin's table and G1's book hold as many.
inline constexpr std::size_t kMaxRows = 16;
/// A line of the index: three ids of 16 digits, S or C, a version of at most 5 digits, four ':' and the '\n'.
inline constexpr std::size_t kRowBytes = 3 * 16 + 1 + 5 + 4 + 1;
static_assert(kRowBytes == 59, "a line of the index is 59 bytes at most");
static_assert(sizeof(kIndexHeader) + kMaxRows * kRowBytes <= kIndexMaxBytes, "the header and every line fit in what the plugin reads");

// ---- a line of the index ------------------------------------------------------------------------------------------------------------
struct Row {
    u64 child;     ///< 00040001...: what the host announces and the Download Play application jumps to
    u64 game;      ///< 00040000...: the copy the child comes from (its content 2 holds the child's CIA)
    u64 prepared;  ///< title_rules::PreparedOf(game): the hidden title the app installed; 0 = not prepared (yet)
    u16 version;   ///< the child's TMD version
    char where;    ///< 'S': the game is on the SD card, 'C': on the game card
};
struct Index {
    Row rows[kMaxRows];
    u8 count;    ///< rows kept
    u8 skipped;  ///< lines refused (a wrong line, a child or a prepared title twice, one past kMaxRows, a line cut before its end)
};

/// Is the line one the plugin may believe? A child (00040001, low word not 0), a game (00040000, low word not 0), the prepared title 0 or
/// EXACTLY the game's PreparedOf (a row never names another title of the band: 3DS Link or a homebrew), S or C.
constexpr bool RowIsSound(const Row& r) {
    return static_cast<u32>(r.child >> 32) == title_rules::kTitleHighDlpChild && static_cast<u32>(r.child) != 0 &&
           static_cast<u32>(r.game >> 32) == title_rules::kTitleHighApplication && static_cast<u32>(r.game) != 0 &&
           (r.prepared == 0 || r.prepared == title_rules::PreparedOf(r.game)) && (r.where == 'S' || r.where == 'C');
}

/// The 16 hex digits of `v`, upper case, at p[0..15] (no NUL).
inline void PutHex16(char* p, u64 v) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i) {
        p[i] = kDigits[v & 0xF];
        v >>= 4;
    }
}

/// The line of `r` with its '\n' and a NUL after it; its length (without the NUL), or 0 (and "") for a row that is not sound.
inline std::size_t FormatRow(const Row& r, char (&out)[kRowBytes + 1]) {
    out[0] = '\0';
    if (!RowIsSound(r)) {
        return 0;
    }
    std::size_t at = 0;
    for (const u64 id : {r.child, r.game, r.prepared}) {
        PutHex16(out + at, id);
        at += 16;
        out[at++] = ':';
    }
    out[at++] = r.where;
    out[at++] = ':';
    char digits[5];
    std::size_t n = 0;
    u32 v = r.version;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        out[at++] = digits[--n];
    }
    out[at++] = '\n';
    out[at] = '\0';
    return at;
}

// ---- the block of a prepared child ----------------------------------------------------------------------------------------------------
inline constexpr std::size_t kBlockBytes = 5000;
inline constexpr std::size_t kBlockShortAt = 0;       // 64 units, little-endian
inline constexpr std::size_t kBlockLongAt = 128;      // 128 units, little-endian
inline constexpr std::size_t kBlockIconAt = 384;      // 48 x 48 RGB565, as the SMDH holds it
inline constexpr std::size_t kBlockTransferAt = 4992; // u32 LE: what a host announces at frame 0 +0x38 (where the CIA's meta block starts)
inline constexpr std::size_t kBlockRequiredAt = 4996; // u32 LE: ... at +0x3C (the CIA's length)
static_assert(kBlockShortAt + dlp_real::kTitleShortBytes == kBlockLongAt && kBlockLongAt + dlp_real::kTitleLongBytes == kBlockIconAt,
              "the names of a block are the 128 + 256 bytes of a title entry of the SMDH");
static_assert(kBlockIconAt + kTitleInfoIconSize == kBlockTransferAt && kBlockRequiredAt + 4 == kBlockBytes, "the icon, then the two sizes close the block");
/// "<prepared, 16 digits>.bin" and its NUL.
inline constexpr std::size_t kBlockNameBytes = 16 + 4 + 1;

/// The name of the block of the prepared child `prepared`, in kGuestsDir.
inline void BlockName(u64 prepared, char (&out)[kBlockNameBytes]) {
    PutHex16(out, prepared);
    std::memcpy(out + 16, ".bin", 5);
}

/// The block of a child (the app): the names of the console's language (or the SMDH's next best, dlp_real::SmdhTitleEntry) and the big icon of
/// `smdh` (`size` bytes: the SMDH of the meta block of the child's CIA), and the two sizes FindChildPackage gives for that CIA. false, and `out`
/// untouched, when it is not a whole SMDH; an SMDH with no name gives empty names.
inline bool MakeBlock(u8* out, const u8* smdh, std::size_t size, u8 language, u32 transfer, u32 required) {
    if (size < dlp_real::kSmdhBytes || std::memcmp(smdh, "SMDH", 4) != 0) {
        return false;
    }
    std::memset(out, 0, kBlockBytes);
    const unsigned entry = dlp_real::SmdhTitleEntry(smdh, language);
    if (entry < dlp_real::kSmdhTitles) {
        const u8* title = smdh + dlp_real::kSmdhTitlesAt + entry * dlp_real::kSmdhTitleBytes;
        std::memcpy(out + kBlockShortAt, title, dlp_real::kTitleShortBytes);
        std::memcpy(out + kBlockLongAt, title + dlp_real::kTitleShortBytes, dlp_real::kTitleLongBytes);
    }
    std::memcpy(out + kBlockIconAt, smdh + dlp_real::kSmdhBigIconAt, kTitleInfoIconSize);
    StoreLE32(out + kBlockTransferAt, transfer);
    StoreLE32(out + kBlockRequiredAt, required);
    return true;
}

// ---- what a host's child is on this console --------------------------------------------------------------------------------------------
/// The verdict on the child a host announces (seven values: G2). Only Ready lets the Download Play application join (MayJoin).
enum class Verdict : u8 {
    Unknown = 0,       ///< the plugin has no table (yet), or has not read the child's block yet: the host's name as it is, no mark, refused
    Ready = 1,         ///< its row, the game, the prepared child and its plugin: nothing to download
    NoRow = 2,         ///< no row for that child (no index, or the game is not turned on in 3DS Link)
    NoGame = 3,        ///< the row's game is not on the console
    NotPrepared = 4,   ///< the row says 0, the prepared title is not installed, or its block is not there (the app did not finish it)
    NoChildPlugin = 5, ///< no plugin in the prepared child's folder of /luma/plugins
    OtherVersion = 6,  ///< the host announces sizes that are not this child's
};

/// What a slot of Table::block holds once the plugin's thread looked for the row's block and found none whole (no file, a short one, no
/// memory): its address only, never read. A slot is then one of three: nullptr = not read yet, &kNoBlockTag = none, else the block.
inline constexpr u8 kNoBlockTag = 0;
enum class BlockState : u8 { NotRead, Read, None };

/// The plugin's table of the children: made once on the plugin's thread from the index and the checks of JudgeAll, then published (an atomic
/// pointer) and never changed, but for its blocks: a slot goes once from nullptr (not read) to a block of kBlockBytes the plugin's thread read
/// (PutBlock) or to &kNoBlockTag (NoBlock), and the threads that read it see it whole (release / acquire). Fill it with Clear first.
struct Table {
    Index index;
    u8 verdict[kMaxRows];  ///< Verdict of each row of `index`
    bool index_found;      ///< the file was there with the right header (for the log: a table without it says NoRow for every host)
    std::atomic<const u8*> block[kMaxRows];
};

/// An empty table: no row, no block.
inline void Clear(Table* t) {
    std::memset(&t->index, 0, sizeof(t->index));
    std::memset(t->verdict, 0, sizeof(t->verdict));
    t->index_found = false;
    for (std::atomic<const u8*>& b : t->block) {
        b.store(nullptr, std::memory_order_relaxed);
    }
}

/// The block of row `row`, once: false if the row is not there or has a block already (the one given is then not the table's).
inline bool PutBlock(Table* t, std::size_t row, const u8* block) {
    if (row >= t->index.count || block == nullptr) {
        return false;
    }
    const u8* none = nullptr;
    return t->block[row].compare_exchange_strong(none, block, std::memory_order_acq_rel);
}
/// Row `row` has no block (the plugin's thread looked and found none whole), once: false if the row is not there or its slot is not empty.
inline bool NoBlock(Table* t, std::size_t row) {
    return PutBlock(t, row, &kNoBlockTag);
}

// >>> game-thread code (Tests/policy_check.sh: what G1's writers of dlp:CLNT call on the Download Play application's thread)

/// The row of the child `child_tid`, or -1.
inline int FindRow(const Index& idx, u64 child_tid) {
    for (std::size_t r = 0; r < idx.count && r < kMaxRows; ++r) {
        if (idx.rows[r].child == child_tid) {
            return static_cast<int>(r);
        }
    }
    return -1;
}
/// The row whose prepared title is `prepared` (not 0), or -1: who a prepared child is (K8), never a formula read backwards.
inline int FindPrepared(const Index& idx, u64 prepared) {
    for (std::size_t r = 0; prepared != 0 && r < idx.count && r < kMaxRows; ++r) {
        if (idx.rows[r].prepared == prepared) {
            return static_cast<int>(r);
        }
    }
    return -1;
}

/// The block of row `row`, or nullptr (not read, none, or no such row).
inline const u8* BlockOf(const Table* t, std::size_t row) {
    const u8* b = (t == nullptr || row >= t->index.count || row >= kMaxRows) ? nullptr : t->block[row].load(std::memory_order_acquire);
    return b == &kNoBlockTag ? nullptr : b;
}
/// Whether row `row`'s block was read, found missing, or not looked for yet (what the plugin's thread is asked to read: NotRead). No such row: None.
inline BlockState BlockStateOf(const Table* t, std::size_t row) {
    if (t == nullptr || row >= t->index.count || row >= kMaxRows) {
        return BlockState::None;
    }
    const u8* b = t->block[row].load(std::memory_order_acquire);
    return b == nullptr ? BlockState::NotRead : (b == &kNoBlockTag ? BlockState::None : BlockState::Read);
}
inline u32 BlockTransfer(const u8* block) {
    return LoadLE32(block + kBlockTransferAt);
}
inline u32 BlockRequired(const u8* block) {
    return LoadLE32(block + kBlockRequiredAt);
}

/// The verdict on the child `child_tid` of a host that announced the sizes `ann_transfer` / `ann_required` (frame 0 +0x38/+0x3C, 0 = not
/// heard): no table -> Unknown; no row -> NoRow (a table without an index too: every host is shown not ready); the row's verdict; and a Ready
/// row is Ready only with its block read and the sizes matching: block not read yet -> Unknown (refused until the plugin's thread read it:
/// the sizes of another version cannot be told before), no block -> NotPrepared, a size that differs from the host's - both sides not 0 -
/// OtherVersion (equal, or either 0: Ready). A row JudgeAll did not judge says NotPrepared: refused, never Ready.
inline Verdict VerdictFor(const Table* t, u64 child_tid, u32 ann_transfer, u32 ann_required) {
    if (t == nullptr) {
        return Verdict::Unknown;
    }
    const int r = FindRow(t->index, child_tid);
    if (r < 0) {
        return Verdict::NoRow;
    }
    const u8 said = t->verdict[r];
    if (said != static_cast<u8>(Verdict::Ready)) {
        return said <= static_cast<u8>(Verdict::OtherVersion) && said != static_cast<u8>(Verdict::Unknown) ? static_cast<Verdict>(said) : Verdict::NotPrepared;
    }
    const u8* slot = t->block[r].load(std::memory_order_acquire);
    if (slot == nullptr || slot == &kNoBlockTag) {
        return slot == nullptr ? Verdict::Unknown : Verdict::NotPrepared; // (not read yet: never Ready before the sizes are known)
    }
    const u32 transfer = BlockTransfer(slot), required = BlockRequired(slot);
    if ((ann_transfer != 0 && transfer != 0 && ann_transfer != transfer) || (ann_required != 0 && required != 0 && ann_required != required)) {
        return Verdict::OtherVersion;
    }
    return Verdict::Ready;
}
/// Does the Download Play application join the host? Only when its child is Ready: anything else would download, and the child never travels
/// through the room.
constexpr bool MayJoin(Verdict v) {
    return v == Verdict::Ready;
}

// ---- the small line under the host's name: DLPServerInfo node_info[0].username (kServerInfoHostName), kNameUnits units -----------------
enum class MarkStyle : u8 { Glyph, Text };
/// G2.0 (analysis/piano-1.1.4-2026-11-15/g2-font/ESITO.md): the system font could not be read on the PC, so the marks are "OK" / "NO", which
/// every font has. Glyph (U+2713 / U+2717) only once a real cbf_std.bcfnt's CMAP or a photo of the line shows both.
inline constexpr MarkStyle kMarkStyle = MarkStyle::Text;
inline constexpr u16 kGlyphReady = 0x2713, kGlyphNotReady = 0x2717;

/// out = the host's name, shortened, then ' ' and the mark, zero-padded: "Gionko OK" / "Gionko NO" (Text), "Gionko ✓" (Glyph). The NAME is
/// shortened, never the mark: the name keeps kNameUnits - 1 - (the mark's units) units at most (8 with a glyph, 7 with OK/NO), the units before
/// its first 0, without the spaces and the high surrogates at its end (a high surrogate there is always alone). An empty name: the mark alone. `out` is not `host`.
inline void MakeLabel(u16 (&out)[kNameUnits], const u16 (&host)[kNameUnits], bool ready, MarkStyle style) {
    u16 mark[2] = {ready ? kGlyphReady : kGlyphNotReady, 0};
    u32 mark_units = 1;
    if (style == MarkStyle::Text) {
        mark[0] = ready ? u16('O') : u16('N');
        mark[1] = ready ? u16('K') : u16('O');
        mark_units = 2;
    }
    const u32 budget = kNameUnits - 1 - mark_units;
    u32 len = 0;
    while (len < kNameUnits && host[len] != 0) {
        ++len;
    }
    if (len > budget) {
        len = budget;
    }
    // (the spaces at the end, and a high surrogate there: the first half of a pair whose second half the cut took, or none had)
    while (len > 0 && (host[len - 1] == u16(' ') || (host[len - 1] >= 0xD800 && host[len - 1] <= 0xDBFF))) {
        --len;
    }
    u32 at = 0;
    for (; at < len; ++at) {
        out[at] = host[at];
    }
    if (len > 0) {
        out[at++] = u16(' ');
    }
    for (u32 m = 0; m < mark_units; ++m) {
        out[at++] = mark[m];
    }
    while (at < kNameUnits) {
        out[at++] = 0;
    }
}

/// Into a DLPServerInfo record (kServerInfoSize bytes, written by G1): the small line - the host's name as it is for Unknown, else MakeLabel with
/// ready = MayJoin(v). Exactly the kNameUnits units at kServerInfoHostName, little-endian, and nothing else.
inline void WriteLabel(u8* server_info, const u16 (&host)[kNameUnits], Verdict v, MarkStyle style = kMarkStyle) {
    u16 line[kNameUnits];
    if (v == Verdict::Unknown) {
        for (u32 i = 0; i < kNameUnits; ++i) {
            line[i] = host[i];
        }
    } else {
        MakeLabel(line, host, MayJoin(v), style);
    }
    for (u32 i = 0; i < kNameUnits; ++i) {
        StoreLE16(server_info + kServerInfoHostName + 2 * i, line[i]);
    }
}

/// What title_long says after the host's name, for each verdict (ASCII).
inline const char* TitleLongTail(Verdict v) {
    switch (v) {
    case Verdict::Ready: return " - join it from the 3DS Link app";  // (1.1.4, route B: the system's application never joins, spec 8 D-B5)
    case Verdict::NoRow: return " - turn this game on in 3DS Link";
    case Verdict::NoGame: return " - this game is not on this console";
    case Verdict::NotPrepared: return " - prepare this game in 3DS Link";
    case Verdict::NoChildPlugin: return " - turn this game on again in 3DS Link";
    case Verdict::OtherVersion: return " - another version of the game";
    case Verdict::Unknown: break;
    }
    return "";
}
/// The units of title_long (kTitleInfoTitleLong, 128 units).
inline constexpr u32 kTitleLongUnits = 128;

/// Into a DLPTitleInfo record (kTitleInfoSize bytes): title_long = "Hosted by " + the host's whole name (the units before its first 0) + the
/// verdict's tail, zero-padded to its 128 units. Nothing else of the record.
inline void WriteTitleLong(u8* title_info, const u16 (&host)[kNameUnits], Verdict v) {
    u8* p = title_info + kTitleInfoTitleLong;
    u32 at = 0;
    for (const char* s = "Hosted by "; *s != '\0'; ++s) {
        StoreLE16(p + 2 * at++, static_cast<u8>(*s));
    }
    for (u32 i = 0; i < kNameUnits && host[i] != 0; ++i) {
        StoreLE16(p + 2 * at++, host[i]);
    }
    for (const char* s = TitleLongTail(v); *s != '\0' && at + 1 < kTitleLongUnits; ++s) {
        StoreLE16(p + 2 * at++, static_cast<u8>(*s));
    }
    while (at < kTitleLongUnits) {
        StoreLE16(p + 2 * at++, 0);
    }
}

/// Into a DLPTitleInfo record, from a block: title_short (64 units at kTitleInfoTitleShort), the icon (kTitleInfoIcon) and the size field
/// (kTitleInfoSizeField) = the block's transfer size. title_long is WriteTitleLong's; the ids, the MAC, the version and the age ratings are G1's.
inline void WriteFromBlock(u8* title_info, const u8* block) {
    std::memcpy(title_info + kTitleInfoTitleShort, block + kBlockShortAt, dlp_real::kTitleShortBytes);
    std::memcpy(title_info + kTitleInfoIcon, block + kBlockIconAt, kTitleInfoIconSize);
    StoreLE32(title_info + kTitleInfoSizeField, BlockTransfer(block));
}

// <<< game-thread code

static_assert(kTitleInfoTitleShort + dlp_real::kTitleShortBytes == kTitleInfoTitleLong && kTitleInfoTitleLong + kTitleLongUnits * 2 == kTitleInfoIcon,
              "title_short and title_long are where WriteFromBlock and WriteTitleLong write them");
static_assert(sizeof("Hosted by ") - 1 + kNameUnits + sizeof(" - turn this game on again in 3DS Link") - 1 < kTitleLongUnits,
              "the longest title_long fits with its zero");

// ---- reading the index (the plugin's thread; the tests) --------------------------------------------------------------------------------
/// The value of 16 hex digits at p (either case); false for any other character.
inline bool ReadHex16(const char* p, u64* out) {
    u64 v = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = p[i];
        u32 d;
        if (c >= '0' && c <= '9') {
            d = static_cast<u32>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<u32>(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<u32>(c - 'a' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

/// One line of the index without its '\n' (and without a '\r' before it): false for anything but the five fields of a sound row.
inline bool ParseRow(const char* p, std::size_t n, Row* out) {
    // child:game:prepared:W:version - 3 x 17 + 2 + 1..5 digits
    if (n < 3 * 17 + 2 + 1 || n > kRowBytes - 1 || p[16] != ':' || p[33] != ':' || p[50] != ':' || p[52] != ':') {
        return false;
    }
    Row r{};
    if (!ReadHex16(p, &r.child) || !ReadHex16(p + 17, &r.game) || !ReadHex16(p + 34, &r.prepared)) {
        return false;
    }
    const char w = p[51];
    r.where = (w == 's' || w == 'S') ? 'S' : ((w == 'c' || w == 'C') ? 'C' : '\0');
    u32 version = 0;
    for (std::size_t i = 53; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        version = version * 10 + static_cast<u32>(p[i] - '0');
    }
    if (version > 0xFFFF) {
        return false;
    }
    r.version = static_cast<u16>(version);
    if (!RowIsSound(r)) {
        return false;
    }
    *out = r;
    return true;
}

/// The index from the `size` bytes of `text` (not NUL-terminated). false, and no row, when the first line is not exactly kIndexHeader (another
/// version: as if there were no file). Then every line that ends with its '\n' ('\r' before it allowed): an empty line or a '#' line is skipped;
/// a line that is not a sound row, a second line of a child or of a prepared title, a line past kMaxRows rows, and a last line the text cuts before
/// its '\n' are refused (counted in `skipped`). Never reads past `size`.
inline bool ParseIndex(const char* text, std::size_t size, Index* out) {
    std::memset(out, 0, sizeof(*out));
    std::size_t at = 0;
    bool first = true;
    while (at < size) {
        std::size_t end = at;
        while (end < size && text[end] != '\n') {
            ++end;
        }
        const bool whole = end < size;
        std::size_t n = end - at;
        if (whole && n > 0 && text[at + n - 1] == '\r') {
            --n;
        }
        const char* line = text + at;
        at = whole ? end + 1 : size;
        if (first) {
            first = false;
            if (!whole || n != sizeof(kIndexHeader) - 1 || std::memcmp(line, kIndexHeader, n) != 0) {
                return false;
            }
            continue;
        }
        if (whole && (n == 0 || line[0] == '#')) {
            continue;
        }
        Row r{};
        const bool kept = whole && out->count < kMaxRows && ParseRow(line, n, &r) && FindRow(*out, r.child) < 0 &&
                          (r.prepared == 0 || FindPrepared(*out, r.prepared) < 0);
        if (kept) {
            out->rows[out->count++] = r;
        } else if (out->skipped < 0xFF) {
            ++out->skipped;
        }
    }
    return !first;
}

// ---- judging the rows (the plugin's thread, where the file system is open) ---------------------------------------------------------------
/// What the plugin asks the console (the tests: fakes). media: 1 = the SD card, 2 = the game card (FS_MediaType).
struct Probe {
    void* ctx;
    bool (*title_present)(void* ctx, u64 title_id, u8 media, u32 content); // content 2 of the game, content 0 of the prepared title
    bool (*plugin_present)(void* ctx, u64 prepared);                       // /luma/plugins/<prepared>/AzaharLink.3gx: the one file of "ready"
};
/// How the titles are looked for: the file system, AM, or not at all (the row is believed): fixed by D-S1.
enum class PresenceBy : u8 { FileSystem, Am, Index };

/// The verdict of every row of `idx` into `verdict`: prepared 0 -> NotPrepared (nothing asked); the game absent -> NoGame; the prepared title
/// absent -> NotPrepared; its plugin absent -> NoChildPlugin; else Ready. With PresenceBy::Index the two titles are not asked. The cheapest
/// refusal first: plugin_present is asked only when all else passed.
inline void JudgeAll(const Index& idx, u8 (&verdict)[kMaxRows], const Probe& p, PresenceBy by) {
    for (std::size_t r = 0; r < kMaxRows; ++r) {
        verdict[r] = static_cast<u8>(Verdict::Unknown);
    }
    for (std::size_t r = 0; r < idx.count && r < kMaxRows; ++r) {
        const Row& row = idx.rows[r];
        Verdict v = Verdict::Ready;
        if (row.prepared == 0) {
            v = Verdict::NotPrepared;
        } else if (by != PresenceBy::Index && !p.title_present(p.ctx, row.game, row.where == 'C' ? 2 : 1, 2)) {
            v = Verdict::NoGame;
        } else if (by != PresenceBy::Index && !p.title_present(p.ctx, row.prepared, 1, 0)) {
            v = Verdict::NotPrepared;
        } else if (!p.plugin_present(p.ctx, row.prepared)) {
            v = Verdict::NoChildPlugin;
        }
        verdict[r] = static_cast<u8>(v);
    }
}

} // namespace dlp_children
} // namespace azl
