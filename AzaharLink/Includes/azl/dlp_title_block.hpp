// The title a host of the REAL Download Play announces (2026-11-14, stage H2 of analysis/island-tour-2026-11-13/plugin/PROGETTO-FINALE.md):
// the short and the long name of the child, its icon and its two sizes, which a real host puts in the five frames of its announcement
// (azl/dlp_real_wire.hpp) and which ours left at zero. Zero is right for the games whose guest is the game itself (Luigi's Mansion, Mario
// Kart 7: nobody shows them, and their frames are held byte for byte to their captures); it is wrong where the guest is the SYSTEM's
// Download Play application, which lists the host with that name and that icon: seen on 2026-11-14, a console hosting Mario Party:
// Island Tour was listed by an Azahar's Download Play with a black icon and an empty title.
//
// WHAT A REAL HOST ANNOUNCES (Azahar's dlp_srvr.cpp, CacheContentFileInMemory, and the capture of two Azahar of 2026-11-13): the child's
// package is a CIA in the RomFS of the game's content 2; from its meta block's SMDH the big icon (48 x 48, 16 bits) and the short and the
// long description of the console's language; transfer_size = where the meta block starts in the CIA, required_size = the CIA's length.
// On the wire every 16-bit unit is big-endian: the bytes of the SMDH swapped in pairs.
//
// WHERE THE PLUGIN TAKES IT FROM (Sources/ctr/plugin.cpp, on the plugin's own thread at start, never on a game thread): from the title
// that is RUNNING, whatever its region - no table of names. First the child's package itself, as a real host does (content 2 of the
// running title through the file system, FindChildPackage below: that archive is read from a game's process on consoles, the RomFS of
// a content 2 has never been seen read there); if that cannot be read, the game's own icon file (the "self NCCH" archive: the read
// every game already makes for the room's game name, seen on consoles), which for this game is the same name and the same icon
// (compared on a PC, Europe: analysis, not here) and leaves the two sizes at zero; if that fails too, nothing is given and the
// announcement is what it was.
//
// Pure: no state, no allocation, no log, no file. The readers take the caller's buffer and never read past what they were given;
// FindChildPackage reads through the caller's function (a file on the console, an image in memory in Tests/dlp_title_block_test.cpp).
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace dlp_real {

constexpr std::size_t kTitleShortBytes = 128;  // 64 UTF-16 units: frame 0, +0x40
constexpr std::size_t kTitleLongBytes = 256;   // 128 units: frame 0, +0xC0
constexpr std::size_t kTitleIconBytes = 4608;  // 48 x 48 pixels of 16 bits: frame 0 from +0x1C0, then frames 1..3

/// What the announcement carries of the child, ALREADY in the wire's byte order (the builder of the frames only copies slices).
struct TitleBlock {
    u32 child_tid_low;                 // the child this is the title of (the low word of its title id): another child gets zeros
    u32 transfer_size;                 // frame 0, +0x38 (0 = not read)
    u32 required_size;                 // frame 0, +0x3C (0 = not read)
    u8 title_short[kTitleShortBytes];  // big-endian units
    u8 title_long[kTitleLongBytes];
    u8 icon[kTitleIconBytes];
};
static_assert(sizeof(TitleBlock) == 5004, "the title of an announcement: 5,000 bytes of the wire and the child's id");

/// Which games' hosts announce the title of their child: EVERY game whose host has the real Download Play (the plugin asks only where there is the
/// hub of a row of the real Download Play, the Download Play column of azl/title_rules.hpp), as the system's own Download Play service does - it reads the
/// names, the icon and the sizes from the child in the game's content 2, whatever the game (2026-11-15, the user's rule: no list; before, only Mario
/// Party: Island Tour, and Luigi's Mansion and Mario Kart 7 announced zeros, so the system's Download Play application listed them with no name and no
/// icon). An APPLICATION id: a Download Play child or an update never hosts.
constexpr bool AnnouncesTitle(u64 title_id) {
    return static_cast<u32>(title_id >> 32) == 0x00040000u && static_cast<u32>(title_id) != 0;
}

// ---- the SMDH (the icon file of a title, and the one in the meta block of a CIA) ---------------------------------------------------

constexpr std::size_t kSmdhBytes = 0x36C0;
constexpr std::size_t kSmdhTitlesAt = 8;          // 16 entries of 0x200 bytes: short description 0x80, long 0x100, publisher 0x80
constexpr std::size_t kSmdhTitleBytes = 0x200;
constexpr unsigned kSmdhTitles = 16;
constexpr std::size_t kSmdhBigIconAt = 0x24C0;    // 0x1200 bytes
static_assert(kSmdhBigIconAt + kTitleIconBytes == kSmdhBytes, "the big icon is the end of an SMDH");

/// The entry of the titles to announce: the console's language (CFG's number is the SMDH's index), or - where the game has no name in
/// it - English, Japanese, then the first one that has a name. (Azahar's host takes the console's language and nothing else: an empty
/// name where the game has none in it. A name is better than none.) kSmdhTitles = the SMDH has no name at all.
inline unsigned SmdhTitleEntry(const u8* smdh, u8 language) {
    const auto named = [smdh](unsigned entry) { return LoadLE16(smdh + kSmdhTitlesAt + entry * kSmdhTitleBytes) != 0; };
    if (language < kSmdhTitles && named(language)) {
        return language;
    }
    if (named(1)) {
        return 1;
    }
    for (unsigned entry = 0; entry < kSmdhTitles; ++entry) {
        if (named(entry)) {
            return entry;
        }
    }
    return kSmdhTitles;
}

/// `count` bytes of 16-bit units from the file's order (little-endian) to the wire's (big-endian). `count` is even.
inline void SwapUnits(u8* out, const u8* in, std::size_t count) {
    for (std::size_t i = 0; i + 1 < count; i += 2) {
        out[i] = in[i + 1];
        out[i + 1] = in[i];
    }
}

/// The names and the icon of `out` from an SMDH of `size` bytes (the sizes and the child's id are not touched). false, and `out` as it
/// was, when it is not a whole SMDH. An SMDH with no name in any language gives the icon and empty names.
inline bool TitleFromSmdh(TitleBlock* out, const u8* smdh, std::size_t size, u8 language) {
    if (size < kSmdhBytes || std::memcmp(smdh, "SMDH", 4) != 0) {
        return false;
    }
    std::memset(out->title_short, 0, sizeof(out->title_short));
    std::memset(out->title_long, 0, sizeof(out->title_long));
    const unsigned entry = SmdhTitleEntry(smdh, language);
    if (entry < kSmdhTitles) {
        const u8* title = smdh + kSmdhTitlesAt + entry * kSmdhTitleBytes;
        SwapUnits(out->title_short, title, kTitleShortBytes);
        SwapUnits(out->title_long, title + kTitleShortBytes, kTitleLongBytes);
    }
    SwapUnits(out->icon, smdh + kSmdhBigIconAt, kTitleIconBytes);
    return true;
}

// ---- the child's package: a CIA, the one file of the RomFS of the game's content 2 ------------------------------------------------

/// Reads `size` bytes at `offset` of the RomFS image into `out`; false = not all of them.
using ReadAt = bool (*)(void* ctx, u64 offset, u8* out, u32 size);

constexpr std::size_t kCiaNameUnits = 20;  // "00040001000f8200.cia"

struct ChildPackage {
    u32 transfer_size;  // where the CIA's meta block starts
    u32 required_size;  // the CIA's length
    u64 smdh_at;        // where the SMDH of its meta block is in the RomFS image (kSmdhBytes from there)
    // For the log of a NoFile only (a region whose container names its file another way is told by one line of one session):
    u32 first_name_bytes;                // the length of the name of the root directory's first file, as the image gives it (0 = no file)
    char first_name[kCiaNameUnits + 1];  // its first units as text ('?' = not a printable ASCII character), "" = not read
};

enum class PackageWhy : u8 {
    Ok = 0,
    Read,      // the caller's function gave less than asked
    RomFs,     // not the header of a RomFS (level 3), nor an IVFC container that holds one
    NoFile,    // no file named <the child's title id>.cia in its root directory
    Cia,       // the file is not a CIA with a meta block that holds an SMDH, or its sizes do not add up to the file's
};

constexpr unsigned kPackageFiles = 8;      // files of the root directory looked at (a child container has one per child)
constexpr u32 kCiaHeaderBytes = 0x2020;
constexpr u32 kCiaMetaSmdhAt = 0x400;

/// Finds <0004000100000000 | child_tid_low>.cia in the root directory of the RomFS that `read` gives and reads its header. Every
/// offset and every size is checked before it is used: an image that lies gets a refusal, never a read the caller did not bound.
inline PackageWhy FindChildPackage(ReadAt read, void* ctx, u32 child_tid_low, ChildPackage* out) {
    u8 b[0x60];
    u64 base = 0;
    if (!read(ctx, 0, b, 0x28)) {
        return PackageWhy::Read;
    }
    if (std::memcmp(b, "IVFC", 4) == 0) {
        // (the whole container instead of its level 3: the header, the master hash, then level 3 at its block boundary)
        if (!read(ctx, 0, b, 0x60)) {
            return PackageWhy::Read;
        }
        const u32 master = LoadLE32(b + 0x08);
        const u32 block_log2 = LoadLE32(b + 0x4C);
        if (master > 0x100000 || block_log2 < 9 || block_log2 > 16) {
            return PackageWhy::RomFs;
        }
        const u64 block = 1ull << block_log2;
        base = (0x60 + static_cast<u64>(master) + block - 1) & ~(block - 1);
        if (!read(ctx, base, b, 0x28)) {
            return PackageWhy::Read;
        }
    }
    const u32 dir_meta_at = LoadLE32(b + 0x0C), dir_meta_len = LoadLE32(b + 0x10);
    const u32 file_meta_at = LoadLE32(b + 0x1C), file_meta_len = LoadLE32(b + 0x20);
    const u32 file_data_at = LoadLE32(b + 0x24);
    if (LoadLE32(b) != 0x28 || dir_meta_len < 0x18 || file_meta_len < 0x20) {
        return PackageWhy::RomFs;
    }
    if (!read(ctx, base + dir_meta_at, b, 0x18)) {
        return PackageWhy::Read;
    }
    u32 at = LoadLE32(b + 0x0C);  // the root directory's first file
    char want[kCiaNameUnits + 1] = "00040001";
    for (unsigned i = 0; i < 8; ++i) {
        want[8 + i] = "0123456789abcdef"[(child_tid_low >> (28 - 4 * i)) & 0xF];
    }
    std::memcpy(want + 16, ".cia", 5);
    u64 data_at = 0, data_size = 0;
    bool found = false;
    out->first_name_bytes = 0;
    out->first_name[0] = '\0';
    for (unsigned n = 0; n < kPackageFiles && !found && at != 0xFFFFFFFFu; ++n) {
        const u32 entry_bytes = 0x20 + 2 * kCiaNameUnits;
        if (at > file_meta_len || file_meta_len - at < 0x20) {
            return PackageWhy::RomFs;
        }
        const bool whole = file_meta_len - at >= entry_bytes;
        if (!read(ctx, base + file_meta_at + at, b, whole ? entry_bytes : 0x20)) {
            return PackageWhy::Read;
        }
        if (n == 0) {
            // (what the first file is called, for the log: only the units that were read and that the entry says are its name)
            out->first_name_bytes = LoadLE32(b + 0x1C);
            const u32 units = !whole ? 0 : out->first_name_bytes / 2 < kCiaNameUnits ? out->first_name_bytes / 2 : kCiaNameUnits;
            for (u32 i = 0; i < units; ++i) {
                const u16 unit = LoadLE16(b + 0x20 + 2 * i);
                out->first_name[i] = unit >= 0x20 && unit < 0x7F ? static_cast<char>(unit) : '?';
            }
            out->first_name[units] = '\0';
        }
        found = whole && LoadLE32(b + 0x1C) == 2 * kCiaNameUnits;
        for (unsigned i = 0; found && i < kCiaNameUnits; ++i) {
            const u16 unit = LoadLE16(b + 0x20 + 2 * i);
            found = unit == static_cast<u8>(want[i]) || (unit >= 'A' && unit <= 'F' && unit + 0x20 == static_cast<u8>(want[i]));
        }
        data_at = LoadLE64(b + 0x08);
        data_size = LoadLE64(b + 0x10);
        at = LoadLE32(b + 0x04);  // the next file of the directory
    }
    if (!found) {
        return PackageWhy::NoFile;
    }
    if (data_size > 0xFFFFFFFFull || data_size < kCiaHeaderBytes || data_at > 0xFFFFFFFFFFull) {
        return PackageWhy::Cia;
    }
    const u64 cia_at = base + file_data_at + data_at;
    if (!read(ctx, cia_at, b, 0x20)) {
        return PackageWhy::Read;
    }
    // The CIA's header: its own size, the certificate chain, the ticket, the TMD and the meta block (32 bits each), the content (64);
    // each part starts at a multiple of 64 bytes, the meta block last.
    const auto pad = [](u64 size) { return (size + 63) & ~63ull; };
    const u64 content = LoadLE64(b + 0x18);
    const u32 meta = LoadLE32(b + 0x14);
    if (LoadLE32(b) != kCiaHeaderBytes || content > 0xFFFFFFFFull) {
        return PackageWhy::Cia;
    }
    const u64 meta_at = pad(kCiaHeaderBytes) + pad(LoadLE32(b + 0x08)) + pad(LoadLE32(b + 0x0C)) + pad(LoadLE32(b + 0x10)) + pad(content);
    if (meta < kCiaMetaSmdhAt + kSmdhBytes || meta_at + meta != data_size) {
        return PackageWhy::Cia;
    }
    out->transfer_size = static_cast<u32>(meta_at);
    out->required_size = static_cast<u32>(data_size);
    out->smdh_at = cia_at + meta_at + kCiaMetaSmdhAt;
    return PackageWhy::Ok;
}

inline const char* PackageWhyText(PackageWhy why) {
    switch (why) {
    case PackageWhy::Ok: return "read";
    case PackageWhy::Read: return "a read gave less than asked";
    case PackageWhy::RomFs: return "not a RomFS";
    case PackageWhy::NoFile: return "no package of this child in it";
    default: return "its package is not a CIA with a title";
    }
}

}  // namespace dlp_real
}  // namespace azl
