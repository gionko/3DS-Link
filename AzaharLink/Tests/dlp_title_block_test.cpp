// THE TITLE IN THE ANNOUNCEMENT of the real Download Play (2026-11-14, stage H2 of analysis/island-tour-2026-11-13/plugin/PROGETTO-FINALE.md):
// azl/dlp_title_block.hpp (the block, the SMDH, the child's package, the list of the games), the builder of the frames with a title
// (azl/dlp_real_wire.hpp) and the adapter that sends them (azl/dlp_real_net.hpp). A console hosting Mario Party: Island Tour was listed
// by an Azahar's Download Play application with a black icon and no name (2026-11-14): the announcement carried zeros there.
//
//   S  an SMDH into a block: the names of the console's language and the big icon with every 16-bit unit turned to the wire's order, the
//      language that has no name (English, then the first that has one), what is not an SMDH (refused, the block untouched)
//   F  the five frames WITH a block against the capture of two Azahar that played the game (Tests/dlp_real_island_golden.inc): every byte
//      the capture kept is ours - the child's two sizes among them - and the ranges the capture's file does not carry (the game's name and
//      icon: only their sum is there) are exactly the block's; what Azahar's Download Play client reads out of the five frames
//      (dlp_clt_base.cpp:587-606, transcribed) is the SMDH's names and icon again; WITHOUT a block the frames are the ones of before, byte
//      for byte, for Luigi's Mansion, Mario Kart 7 and this game
//   P  the child's package: the RomFS of a child container made here (one CIA, more files, the whole IVFC container), the two sizes
//      and where the SMDH is; a package with the header numbers of the real child gives the capture's two sizes; every lie of an image
//      (lengths, offsets, a directory that loops, a short read at every step) is a refusal, and 20,000 damaged images never make it
//      read outside what it was given
//   N  the adapter on the wire (engines joined by the in-memory room, real time): no title given = the announcement of before; a title =
//      every frame of every group carries it; the title of another child = none of it; a title given while a group is under way =
//      that group ends as it began and the next one carries it; a lobby closed and another opened for another child = none of it
//   L  the list: the five title ids of Mario Party: Island Tour (3DS Link's catalogue, the family ATS) and no other; none of Luigi's
//      Mansion or Mario Kart 7; and against the plugin's own table (cut out of Sources/ctr/plugin.cpp): a listed game that has a row
//      has a row of the real Download Play
// The name and the icon here are MADE UP (nothing of a game is in this file): the same code against the real child's bytes and the
// capture's frames is run outside the repository (analysis, h2: byte for byte, checksum included).
//
//   python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -I"$OUT" -pthread -fsanitize=address,undefined
//       Tests/dlp_title_block_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp
//       dlph_wire.o <the core sources of Tests/run.sh>
#pragma GCC diagnostic ignored "-Wunused-function"  // (the rig's helpers and the plugin's functions this test does not use)

#include <algorithm>
#include <mutex>
#include <random>

#include "dlp_net_rig.hpp"

#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/dlp_title_block.hpp"
#include "azl/title_rules.hpp"
#include "ctr/text_search_guard.hpp"  // (1.1.4, G1: the plugin's slice - DlpModeOf - asks IsDownloadPlayTitle)
#include "dlp_real_luigi_golden.inc"
#include "dlp_real_mk7_golden.inc"
#include "dlp_real_island_golden.inc"

namespace azl {
namespace ctr {
namespace {
#include "plugin_dlp_slice.inc"
}  // namespace
}  // namespace ctr
}  // namespace azl

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

using dlp_real::ChildPackage;
using dlp_real::PackageWhy;
using dlp_real::TitleBlock;

static constexpr u64 kIslandChild = 0x00040001000F8200ULL;
static constexpr u32 kIslandLow = 0x000F8200u;
static constexpr u8 kIslandPlayers = 4;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;
static constexpr u64 kMk7Child = 0x0004000100030700ULL;
// The least checks a part makes: far above what is left if it returns early.
static constexpr int kLeastSmdh = 40000;
static constexpr int kLeastFrames = 2000;
static constexpr int kLeastPackage = 20000;
static constexpr int kLeastWire = 40;
static constexpr int kLeastList = 40;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void Ran(const char* part, int before, int least) {
    const int made = g_checks - before;
    std::printf("   (%s: %d checks, at least %d wanted)\n", part, made, least);
    CHECK(made >= least);
}

// ---------------------------------------------------------------------------------------------------------------------
// A made-up SMDH: every byte of a name and of the big icon says where it is, so that a slice moved or a pair not swapped shows.

static u8 Mix(u32 salt, u32 i) {
    return static_cast<u8>((i * 131u + (i >> 8) * 17u + salt * 29u + 7u) & 0xFFu);
}

/// `named`: the languages that have a name (bit = the entry). A name is 5 to 60 units, none of them zero.
static std::vector<u8> MakeSmdh(u32 salt, u32 named) {
    std::vector<u8> s(dlp_real::kSmdhBytes, 0);
    std::memcpy(s.data(), "SMDH", 4);
    for (unsigned lang = 0; lang < 16; ++lang) {
        u8* e = s.data() + 8 + lang * 0x200;
        if ((named & (1u << lang)) == 0) {
            continue;
        }
        const unsigned n_short = 5 + (salt + lang) % 56, n_long = 9 + (salt * 3 + lang) % 110;
        for (unsigned k = 0; k < n_short; ++k) {
            e[2 * k] = static_cast<u8>(0x21 + (k + lang + salt) % 90);  // low byte: never zero
            e[2 * k + 1] = static_cast<u8>(k % 3 == 0 ? 0x30 : 0);      // some units above Latin-1, as a Japanese name has
        }
        for (unsigned k = 0; k < n_long; ++k) {
            e[0x80 + 2 * k] = static_cast<u8>(0x21 + (k * 5 + lang + salt) % 90);
            e[0x80 + 2 * k + 1] = static_cast<u8>(k % 4 == 1 ? 0x4E : 0);
        }
        for (unsigned k = 0; k < 0x80; ++k) {
            e[0x180 + k] = 0xEE;  // the publisher: never announced
        }
    }
    for (u32 i = 0x2008; i < 0x24C0; ++i) {
        s[i] = 0xDD;  // the settings and the small icon: never announced
    }
    for (u32 i = 0; i < dlp_real::kTitleIconBytes; ++i) {
        s[dlp_real::kSmdhBigIconAt + i] = Mix(salt, i);
    }
    return s;
}

/// `wire` is `file` with every pair of bytes swapped.
static bool Swapped(const u8* wire, const u8* file, std::size_t count) {
    bool same = true;
    for (std::size_t i = 0; i + 1 < count; i += 2) {
        same = same && wire[i] == file[i + 1] && wire[i + 1] == file[i];
    }
    return same;
}

static TitleBlock Blank(u8 fill) {
    TitleBlock b;
    std::memset(&b, fill, sizeof(b));
    return b;
}

static void S_Smdh() {
    const int c = g_checks;
    std::printf("S: an SMDH into the block of the announcement\n");
    CHECK_EQ(sizeof(TitleBlock), 5004u);
    CHECK_EQ(dlp_real::kTitleShortBytes + dlp_real::kTitleLongBytes + dlp_real::kTitleIconBytes + 8u, 5000u);  // what the wire carries of it
    // every language that has a name gives ITS names; the icon is the big one; the rest of the block is not touched
    for (u32 salt = 1; salt <= 3; ++salt) {
        const std::vector<u8> smdh = MakeSmdh(salt, 0x0FFF);
        for (u8 lang = 0; lang < 12; ++lang) {
            TitleBlock b = Blank(0xA5);
            CHECK(dlp_real::TitleFromSmdh(&b, smdh.data(), smdh.size(), lang));
            const u8* e = smdh.data() + 8 + lang * 0x200;
            CHECK_EQ(dlp_real::SmdhTitleEntry(smdh.data(), lang), lang);
            CHECK(Swapped(b.title_short, e, 128));
            CHECK(Swapped(b.title_long, e + 0x80, 256));
            CHECK(Swapped(b.icon, smdh.data() + 0x24C0, 4608));
            for (std::size_t i = 0; i < 4608; i += 2) {
                CHECK_EQ(b.icon[i], smdh[0x24C0 + i + 1]);  // (one by one: where a wrong byte is)
                CHECK_EQ(b.icon[i + 1], smdh[0x24C0 + i]);
            }
            CHECK_EQ(b.child_tid_low, 0xA5A5A5A5u);
            CHECK_EQ(b.transfer_size, 0xA5A5A5A5u);
            CHECK_EQ(b.required_size, 0xA5A5A5A5u);
            bool publisher = false;
            for (std::size_t i = 0; i < sizeof(b.title_short); ++i) publisher |= b.title_short[i] == 0xEE;
            for (std::size_t i = 0; i < sizeof(b.title_long); ++i) publisher |= b.title_long[i] == 0xEE;
            CHECK(!publisher);
        }
    }
    // a language the game has no name in: English; no English: the first that has one (Japanese before the others); none: empty names
    {
        const std::vector<u8> eur = MakeSmdh(5, 0x073F);  // as a European game: no Chinese, no Korean, no Taiwanese
        for (u8 lang : {u8{6}, u8{7}, u8{11}, u8{12}, u8{15}, u8{16}, u8{0xFF}}) {
            TitleBlock b = Blank(0x5A);
            CHECK_EQ(dlp_real::SmdhTitleEntry(eur.data(), lang), 1u);
            CHECK(dlp_real::TitleFromSmdh(&b, eur.data(), eur.size(), lang));
            CHECK(Swapped(b.title_short, eur.data() + 8 + 0x200, 128));
            CHECK(Swapped(b.title_long, eur.data() + 8 + 0x200 + 0x80, 256));
        }
        const std::vector<u8> jpn = MakeSmdh(6, 0x0001);
        CHECK_EQ(dlp_real::SmdhTitleEntry(jpn.data(), 4), 0u);
        const std::vector<u8> kor = MakeSmdh(7, 0x0080);
        CHECK_EQ(dlp_real::SmdhTitleEntry(kor.data(), 4), 7u);
        CHECK_EQ(dlp_real::SmdhTitleEntry(kor.data(), 7), 7u);
        const std::vector<u8> both = MakeSmdh(8, 0x0081);
        CHECK_EQ(dlp_real::SmdhTitleEntry(both.data(), 4), 0u);
        const std::vector<u8> none = MakeSmdh(9, 0);
        CHECK_EQ(dlp_real::SmdhTitleEntry(none.data(), 4), dlp_real::kSmdhTitles);
        TitleBlock b = Blank(0x5A);
        CHECK(dlp_real::TitleFromSmdh(&b, none.data(), none.size(), 4));
        bool empty = true;
        for (std::size_t i = 0; i < sizeof(b.title_short); ++i) empty &= b.title_short[i] == 0;
        for (std::size_t i = 0; i < sizeof(b.title_long); ++i) empty &= b.title_long[i] == 0;
        CHECK(empty);
        CHECK(Swapped(b.icon, none.data() + 0x24C0, 4608));
    }
    // what is not a whole SMDH: refused, and the block is what it was
    {
        std::vector<u8> smdh = MakeSmdh(4, 0x0FFF);
        const TitleBlock before = Blank(0x3C);
        TitleBlock b = before;
        CHECK(!dlp_real::TitleFromSmdh(&b, smdh.data(), smdh.size() - 1, 1));
        CHECK(!dlp_real::TitleFromSmdh(&b, smdh.data(), 0x2008, 1));
        CHECK(!dlp_real::TitleFromSmdh(&b, smdh.data(), 0, 1));
        smdh[3] = 'X';
        CHECK(!dlp_real::TitleFromSmdh(&b, smdh.data(), smdh.size(), 1));
        CHECK(std::memcmp(&b, &before, sizeof(b)) == 0);
        smdh[3] = 'H';
        smdh.resize(smdh.size() + 64, 0x77);  // (a longer file: the SMDH is its beginning)
        CHECK(dlp_real::TitleFromSmdh(&b, smdh.data(), smdh.size(), 1));
        CHECK(Swapped(b.icon, smdh.data() + 0x24C0, 4608));
    }
    Ran("S", c, kLeastSmdh);
}

// ---------------------------------------------------------------------------------------------------------------------
// F: the frames

/// The block a console that hosts the capture's game would have: made-up names and icon, the capture's two sizes.
static TitleBlock IslandBlock(const std::vector<u8>& smdh, u8 language) {
    TitleBlock b;
    std::memset(&b, 0, sizeof(b));
    b.child_tid_low = kIslandLow;
    b.transfer_size = kDlpRealIslandTransferSize;
    b.required_size = kDlpRealIslandRequiredSize;
    CHECK(dlp_real::TitleFromSmdh(&b, smdh.data(), smdh.size(), language));
    return b;
}

/// What Azahar's Download Play client makes of the five frames (dlp_clt_base.cpp:587-606: DLPTitleInfo), in the console's byte order.
struct AzaharTitleInfo {
    u16 title_short[64];
    u16 title_long[128];
    u16 icon[0x900];
    u32 size;
    u32 unique_id;
};
static AzaharTitleInfo AzaharReads(const std::vector<u8> frames[5]) {
    AzaharTitleInfo t;
    std::memset(&t, 0, sizeof(t));
    const u8* f0 = frames[0].data();
    for (u32 k = 0; k < 64; ++k) t.title_short[k] = LoadBE16(f0 + 0x40 + 2 * k);
    for (u32 k = 0; k < 128; ++k) t.title_long[k] = LoadBE16(f0 + 0xC0 + 2 * k);
    t.unique_id = static_cast<u32>(LoadBE64(f0 + 0x10) & 0xFFFFFFFFu) >> 8;
    t.size = LoadBE32(f0 + 0x38);
    u32 at = 0;
    for (u32 k = 0; k < 0x9C; ++k) t.icon[at++] = LoadBE16(f0 + 0x1C0 + 2 * k);
    for (u32 f = 1; f <= 3; ++f) {
        for (u32 k = 0; k < 0x2CC; ++k) t.icon[at++] = LoadBE16(frames[f].data() + 0x10 + 2 * k);
    }
    CHECK_EQ(at, 0x900u);
    return t;
}
/// ... is the SMDH's own names and icon (entry `lang`), unit for unit.
static void CheckAzaharReads(const AzaharTitleInfo& t, const std::vector<u8>& smdh, unsigned lang) {
    const u8* e = smdh.data() + 8 + lang * 0x200;
    bool names = true, icon = true;
    for (u32 k = 0; k < 64; ++k) names &= t.title_short[k] == LoadLE16(e + 2 * k);
    for (u32 k = 0; k < 128; ++k) names &= t.title_long[k] == LoadLE16(e + 0x80 + 2 * k);
    for (u32 k = 0; k < 0x900; ++k) icon &= t.icon[k] == LoadLE16(smdh.data() + 0x24C0 + 2 * k);
    CHECK(names);
    CHECK(icon);
    CHECK(t.title_short[0] != 0 && t.title_long[0] != 0);
}

static void F_Frames() {
    const int c = g_checks;
    std::printf("F: the five frames with a title against the capture of Mario Party: Island Tour, and without one against the frames of before\n");
    const std::vector<u8> smdh = MakeSmdh(11, 0x073F);
    const TitleBlock block = IslandBlock(smdh, 4);  // an Italian console
    static const std::size_t kLen[5] = {768, 1448, 1448, 1448, 1464};
    std::vector<u8> frames[5];
    std::size_t carried = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const DlpRealGolden& g = kDlpRealIslandAnnounce[i];
        u8 buf[dlp_real::kBufferBytes + 16];
        std::memset(buf, 0xA5, sizeof(buf));
        const std::size_t n = dlp_real::AnnounceFrame(buf, i, kIslandChild, kIslandPlayers, kDlpRealIslandSeed, &block);
        CHECK_EQ(n, g.size);
        CHECK_EQ(n, kLen[i]);
        for (std::size_t k = n; k < sizeof(buf); ++k) {
            CHECK_EQ(buf[k], 0xA5u);  // nothing past the frame
        }
        const dlp_real::Packet p = dlp_real::ParseAnnounce(buf, n);
        CHECK(p.kind == dlp_real::Kind::Announce);
        CHECK_EQ(p.index, i);
        CHECK_EQ(LoadBE32(buf + 8), dlp_real::Checksum(kDlpRealIslandSeed, buf, n));  // sealed AFTER the title went in
        // every byte the capture's file kept is ours (the checksum apart: it is of the name and the icon too) ...
        const std::size_t r0 = g.redacted[0], r1 = g.redacted[1];
        for (std::size_t k = 0; k < n; ++k) {
            if ((k >= 8 && k < 12) || (k >= r0 && k < r1)) {
                continue;
            }
            CHECK_EQ(buf[k], g.bytes[k]);
        }
        // ... and the ranges it does not carry (the game's name and icon) are the block's, nothing more and nothing less
        if (i == 0) {
            CHECK_EQ(r0, 0x40u);
            CHECK_EQ(r1, 0x2F8u);
            CHECK(std::memcmp(buf + 0x40, block.title_short, 128) == 0);
            CHECK(std::memcmp(buf + 0xC0, block.title_long, 256) == 0);
            CHECK(std::memcmp(buf + 0x1C0, block.icon, 312) == 0);
            CHECK_EQ(LoadBE32(buf + 0x38), kDlpRealIslandTransferSize);
            CHECK_EQ(LoadBE32(buf + 0x3C), kDlpRealIslandRequiredSize);
            CHECK_EQ(dlp_real::AnnounceChildTid(buf), kIslandChild);
            CHECK_EQ(dlp_real::AnnounceMaxNodes(buf), kIslandPlayers);
        } else if (i <= 3) {
            CHECK_EQ(r0, 0x10u);
            CHECK_EQ(r1, n);
            CHECK(std::memcmp(buf + 0x10, block.icon + 312 + (i - 1) * 1432, 1432) == 0);
        } else {
            CHECK_EQ(r1, 0u);
            CHECK(std::memcmp(buf, g.bytes, n) == 0);  // frame 4: the capture's, checksum included
        }
        carried += r1 - r0;
        frames[i].assign(buf, buf + n);
    }
    CHECK_EQ(carried, dlp_real::kTitleShortBytes + dlp_real::kTitleLongBytes + dlp_real::kTitleIconBytes);
    // what Azahar's client hands its Download Play application: the SMDH's names and icon, and the size
    const AzaharTitleInfo read = AzaharReads(frames);
    CheckAzaharReads(read, smdh, 4);
    CHECK_EQ(read.size, kDlpRealIslandTransferSize);
    CHECK_EQ(read.unique_id, 0x000F82u);
    // sizes that were not read: zeros there, the names and the icon all the same
    {
        TitleBlock unsized = block;
        unsized.transfer_size = 0;
        unsized.required_size = 0;
        u8 buf[dlp_real::kBufferBytes];
        CHECK_EQ(dlp_real::AnnounceFrame(buf, 0, kIslandChild, kIslandPlayers, dlp_real::kTxSeed, &unsized), 768u);
        bool zero = true;
        for (std::size_t k = 0x38; k < 0x40; ++k) zero &= buf[k] == 0;
        CHECK(zero);
        CHECK(std::memcmp(buf + 0x40, block.title_short, 128) == 0);
    }
    // WITHOUT a title: the frames of before, byte for byte - the games that never have one, and this one until it is read
    struct Game {
        u64 child;
        u8 nodes;
        const u8* seed;
    };
    const Game games[] = {{kLuigiChild, 2, kDlpRealLuigiSeed}, {kLuigiChild, 2, dlp_real::kTxSeed}, {kMk7Child, 8, kDlpRealMk7Seed},
                          {kMk7Child, 8, dlp_real::kTxSeed},   {kIslandChild, 4, kDlpRealIslandSeed}, {kIslandChild, 4, dlp_real::kTxSeed}};
    for (const Game& game : games) {
        for (unsigned i = 0; i < 7; ++i) {
            u8 a[dlp_real::kBufferBytes], b[dlp_real::kBufferBytes];
            std::memset(a, 0x11, sizeof(a));
            std::memset(b, 0x11, sizeof(b));
            const std::size_t na = dlp_real::AnnounceFrame(a, i, game.child, game.nodes, game.seed);
            const std::size_t nb = dlp_real::AnnounceFrame(b, i, game.child, game.nodes, game.seed, nullptr);
            CHECK_EQ(na, nb);
            CHECK_EQ(na, i < 5 ? kLen[i] : 0u);
            CHECK(std::memcmp(a, b, sizeof(a)) == 0);
        }
    }
    {
        u8 buf[dlp_real::kBufferBytes];
        CHECK_EQ(dlp_real::AnnounceFrame(buf, 5, kIslandChild, kIslandPlayers, dlp_real::kTxSeed, &block), 0u);
        CHECK_EQ(dlp_real::AnnounceFrame(buf, 200, kIslandChild, kIslandPlayers, dlp_real::kTxSeed, &block), 0u);
    }
    // the frames of Luigi's Mansion and of Mario Kart 7 are still their captures' (the codec's own tests hold every byte: here the sizes)
    for (unsigned i = 0; i < 5; ++i) {
        u8 buf[dlp_real::kBufferBytes];
        CHECK_EQ(dlp_real::AnnounceFrame(buf, i, kLuigiChild, 2, kDlpRealLuigiSeed, nullptr), kDlpRealLuigiAnnounce[i].size);
        CHECK_EQ(dlp_real::AnnounceFrame(buf, i, kMk7Child, 8, kDlpRealMk7Seed, nullptr), kDlpRealMk7Announce[i].size);
    }
    Ran("F", c, kLeastFrames);
}

// ---------------------------------------------------------------------------------------------------------------------
// P: the child's package. An image is pieces at offsets (what is between them reads as zeros), so that a package of the real child's
// size - 32 MB - costs nothing.

struct Image {
    struct Piece {
        u64 at;
        std::vector<u8> bytes;
    };
    std::vector<Piece> pieces;
    u64 size = 0;
    u32 reads = 0;
    u32 fail_at = 0xFFFFFFFFu;  // the read (from 0) that gives less than asked
    u32 most = 0;               // the longest read asked
    bool outside = false;       // a read past the image was asked
    void Put(u64 at, const std::vector<u8>& bytes) {
        pieces.push_back({at, bytes});
        size = std::max<u64>(size, at + bytes.size());
    }
    void Patch(u64 at, const std::vector<u8>& bytes) {
        pieces.push_back({at, bytes});  // (a later piece wins)
    }
    static bool Read(void* ctx, u64 offset, u8* out, u32 n) {
        Image& im = *static_cast<Image*>(ctx);
        const u32 index = im.reads++;
        im.most = std::max(im.most, n);
        if (offset > im.size || n > im.size - offset) {
            im.outside = true;
            return false;
        }
        if (index == im.fail_at) {
            return false;
        }
        std::memset(out, 0, n);
        for (const Piece& p : im.pieces) {
            const u64 a = std::max(offset, p.at), b = std::min<u64>(offset + n, p.at + p.bytes.size());
            if (a < b) {
                std::memcpy(out + (a - offset), p.bytes.data() + (a - p.at), static_cast<std::size_t>(b - a));
            }
        }
        return true;
    }
};

static void W32(std::vector<u8>& v, std::size_t at, u32 x) {
    StoreLE32(v.data() + at, x);
}
static void W64(std::vector<u8>& v, std::size_t at, u64 x) {
    StoreLE64(v.data() + at, x);
}
static u64 Pad64(u64 x) {
    return (x + 63) & ~63ull;
}

struct CiaShape {
    u32 cert = 0xA00, ticket = 0x350, tmd = 0xB34, meta = 0x3AC0;
    u64 content = 0x3000;
    u64 MetaAt() const {
        return Pad64(0x2020) + Pad64(cert) + Pad64(ticket) + Pad64(tmd) + Pad64(content);
    }
    u64 Size() const {
        return MetaAt() + meta;
    }
};
struct RomFile {
    std::string name;
    CiaShape cia;
    std::vector<u8> smdh;  // put at the CIA's meta + 0x400
};

/// The RomFS (level 3) of a child container with these files in its root directory, at `base` of the image. Returns where each
/// file's data starts in the image.
static std::vector<u64> PutRomFs(Image& im, u64 base, const std::vector<RomFile>& files) {
    std::vector<u8> head(0x28, 0), dir(0x18, 0), meta;
    std::vector<u64> data_at;
    std::vector<u32> entry_at;
    u64 data = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t name_bytes = files[i].name.size() * 2, at = meta.size();
        entry_at.push_back(static_cast<u32>(at));
        meta.resize(at + 0x20 + ((name_bytes + 3) & ~3u), 0);
        W32(meta, at + 0x00, 0);            // the root directory
        W32(meta, at + 0x04, 0xFFFFFFFFu);  // the next file: set below
        W64(meta, at + 0x08, data);
        W64(meta, at + 0x10, files[i].cia.Size());
        W32(meta, at + 0x18, 0xFFFFFFFFu);
        W32(meta, at + 0x1C, static_cast<u32>(name_bytes));
        for (std::size_t k = 0; k < files[i].name.size(); ++k) {
            meta[at + 0x20 + 2 * k] = static_cast<u8>(files[i].name[k]);
        }
        data_at.push_back(data);
        data = (data + files[i].cia.Size() + 15) & ~15ull;
    }
    for (std::size_t i = 0; i + 1 < files.size(); ++i) {
        W32(meta, entry_at[i] + 0x04, entry_at[i + 1]);
    }
    const u32 dir_at = 0x28 + 4, file_hash_at = dir_at + 0x18, meta_at = file_hash_at + 4;
    const u32 file_data_at = (meta_at + static_cast<u32>(meta.size()) + 15) & ~15u;
    W32(head, 0x00, 0x28);
    W32(head, 0x04, 0x28);
    W32(head, 0x08, 4);
    W32(head, 0x0C, dir_at);
    W32(head, 0x10, 0x18);
    W32(head, 0x14, file_hash_at);
    W32(head, 0x18, 4);
    W32(head, 0x1C, meta_at);
    W32(head, 0x20, static_cast<u32>(meta.size()));
    W32(head, 0x24, file_data_at);
    W32(dir, 0x00, 0);
    W32(dir, 0x04, 0xFFFFFFFFu);
    W32(dir, 0x08, 0xFFFFFFFFu);
    W32(dir, 0x0C, files.empty() ? 0xFFFFFFFFu : 0);
    W32(dir, 0x10, 0xFFFFFFFFu);
    im.Put(base, head);
    im.Put(base + dir_at, dir);
    if (!meta.empty()) {
        im.Put(base + meta_at, meta);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const CiaShape& s = files[i].cia;
        std::vector<u8> cia(0x20, 0);
        W32(cia, 0x00, 0x2020);
        W32(cia, 0x08, s.cert);
        W32(cia, 0x0C, s.ticket);
        W32(cia, 0x10, s.tmd);
        W32(cia, 0x14, s.meta);
        W64(cia, 0x18, s.content);
        data_at[i] += base + file_data_at;
        im.Put(data_at[i], cia);
        if (!files[i].smdh.empty()) {
            im.Put(data_at[i] + s.MetaAt() + 0x400, files[i].smdh);
        }
        im.size = std::max<u64>(im.size, data_at[i] + s.Size());
    }
    return data_at;
}

static PackageWhy Find(Image& im, u32 child_low, ChildPackage* out) {
    im.reads = 0;
    im.outside = false;
    return dlp_real::FindChildPackage(&Image::Read, &im, child_low, out);
}

static void P_Package() {
    const int c = g_checks;
    std::printf("P: the child's package in the RomFS of the game's content 2\n");
    const std::vector<u8> smdh = MakeSmdh(21, 0x073F);
    const char* kName = "00040001000f8200.cia";
    // one CIA, as the game has it
    {
        Image im;
        RomFile f{kName, CiaShape(), smdh};
        const std::vector<u64> at = PutRomFs(im, 0, {f});
        ChildPackage p = {};
        CHECK(Find(im, kIslandLow, &p) == PackageWhy::Ok);
        CHECK_EQ(p.transfer_size, f.cia.MetaAt());
        CHECK_EQ(p.required_size, f.cia.Size());
        CHECK_EQ(p.required_size - p.transfer_size, 0x3AC0u);
        CHECK_EQ(p.smdh_at, at[0] + f.cia.MetaAt() + 0x400);
        CHECK_EQ(im.reads, 4u);  // the header, the root directory, the file's entry, the CIA's header
        CHECK(im.most <= 0x60);
        CHECK(!im.outside);
        std::vector<u8> got(dlp_real::kSmdhBytes);
        CHECK(Image::Read(&im, p.smdh_at, got.data(), static_cast<u32>(got.size())));
        CHECK(got == smdh);
        TitleBlock b = Blank(0);
        CHECK(dlp_real::TitleFromSmdh(&b, got.data(), got.size(), 4));
        CHECK(Swapped(b.icon, smdh.data() + 0x24C0, 4608));
        // another child of the same game is not this file
        CHECK(Find(im, kIslandLow | 1, &p) == PackageWhy::NoFile);
        CHECK_EQ(p.first_name_bytes, 40u);  // (for the log of a NoFile: what the file that IS there is called)
        CHECK(std::string(p.first_name) == kName);
        CHECK(Find(im, 0x000F8100u, &p) == PackageWhy::NoFile);
    }
    // a content that is NOT a multiple of 64 bytes: the meta block starts at the next multiple (Azahar's GetMetadataOffset), and so
    // does transfer_size; without that rounding the parts would not add up to the file
    for (u64 content : {0x3001ull, 0x303Full, 0x3040ull, 0x1EE2FC1ull}) {
        Image im;
        RomFile f{kName, CiaShape(), smdh};
        f.cia.content = content;
        PutRomFs(im, 0, {f});
        ChildPackage p = {};
        CHECK(Find(im, kIslandLow, &p) == PackageWhy::Ok);
        CHECK_EQ(p.transfer_size, f.cia.MetaAt());
        CHECK_EQ(p.transfer_size % 64, 0u);  // (the meta block never starts inside the content's padding)
        CHECK_EQ(p.transfer_size, 0x2040u + 0xA00u + 0x380u + 0xB40u + ((content + 63) & ~63ull));
        CHECK_EQ(p.required_size, p.transfer_size + 0x3AC0u);
        std::vector<u8> got(dlp_real::kSmdhBytes);
        CHECK(Image::Read(&im, p.smdh_at, got.data(), static_cast<u32>(got.size())));
        CHECK(got == smdh);  // (the SMDH is where the rounded offset says)
    }
    // the header numbers of the REAL child's package (sizes of its parts, read on a PC) give the two sizes of the capture
    {
        Image im;
        RomFile f{kName, CiaShape(), {}};
        f.cia.content = 0x1EE3000;
        PutRomFs(im, 0, {f});
        ChildPackage p = {};
        CHECK(Find(im, kIslandLow, &p) == PackageWhy::Ok);
        CHECK_EQ(p.transfer_size, kDlpRealIslandTransferSize);
        CHECK_EQ(p.required_size, kDlpRealIslandRequiredSize);
    }
    // the whole IVFC container in front of it (level 3 at its block boundary), a name in capitals, the second and the eighth file
    {
        Image im;
        std::vector<u8> ivfc(0x60, 0);
        std::memcpy(ivfc.data(), "IVFC", 4);
        W32(ivfc, 0x08, 0x20);
        W32(ivfc, 0x4C, 12);
        im.Put(0, ivfc);
        RomFile f{"00040001000F8200.cia", CiaShape(), smdh};
        const std::vector<u64> at = PutRomFs(im, 0x1000, {f});
        ChildPackage p = {};
        CHECK(Find(im, kIslandLow, &p) == PackageWhy::Ok);
        CHECK_EQ(p.smdh_at, at[0] + f.cia.MetaAt() + 0x400);
        CHECK(at[0] > 0x1000);
        CHECK_EQ(im.reads, 6u);
        Image bad = im;
        std::vector<u8> log2(4, 0);
        W32(log2, 0, 40);
        bad.Patch(0x4C, log2);
        CHECK(Find(bad, kIslandLow, &p) == PackageWhy::RomFs);
        W32(log2, 0, 0x7FFFFFFF);
        bad.Patch(0x08, log2);
        CHECK(Find(bad, kIslandLow, &p) == PackageWhy::RomFs);
    }
    CHECK_EQ(dlp_real::kPackageFiles, 8u);  // (the files below are counted against this: eight are looked at, a ninth is not)
    for (unsigned count : {2u, 8u, 9u}) {
        Image im;
        std::vector<RomFile> files;
        for (unsigned i = 0; i + 1 < count; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "00040001000f82%02x.cia", i + 1);
            CiaShape other;
            other.content = 0x1000 + 0x40 * i;
            files.push_back({name, other, {}});
        }
        CiaShape mine;
        mine.content = 0x5000;
        files.push_back({kName, mine, smdh});
        const std::vector<u64> at = PutRomFs(im, 0, files);
        ChildPackage p = {};
        if (count <= dlp_real::kPackageFiles) {
            CHECK(Find(im, kIslandLow, &p) == PackageWhy::Ok);
            CHECK_EQ(p.required_size, mine.Size());
            CHECK_EQ(p.smdh_at, at.back() + mine.MetaAt() + 0x400);
            CHECK(Find(im, kIslandLow | 1, &p) == PackageWhy::Ok);  // (the first file: the child 1)
            CHECK_EQ(p.required_size, files[0].cia.Size());
        } else {
            CHECK(Find(im, kIslandLow, &p) == PackageWhy::NoFile);  // (a ninth file is not looked at)
            CHECK_EQ(im.reads, 2u + dlp_real::kPackageFiles);
        }
    }
    // the lies: each is a refusal with its reason, with no read outside the image
    {
        RomFile f{kName, CiaShape(), smdh};
        const auto with = [&](u64 at, u32 value) {
            Image im;
            PutRomFs(im, 0, {f});
            std::vector<u8> v(4, 0);
            W32(v, 0, value);
            im.Patch(at, v);
            return im;
        };
        ChildPackage p = {};
        // (the RomFS made above: header 0x00, directory 0x2C, the file's entry 0x48, the CIA at 0x90)
        Image a = with(0x00, 0x29);  // not a level 3 header
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::RomFs);
        a = with(0x10, 0x17);  // a directory table too short for its root
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::RomFs);
        a = with(0x20, 0x1F);  // a file table too short for an entry
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::RomFs);
        a = with(0x2C + 0x0C, 0xFFFFFFFFu);  // no file in the root directory
        std::memset(&p, 0xA5, sizeof(p));
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK_EQ(a.reads, 2u);
        CHECK_EQ(p.first_name_bytes, 0u);  // (the log of a NoFile: no file, no name - never what was in the caller's memory)
        CHECK_EQ(p.first_name[0], '\0');
        a = with(0x2C + 0x0C, 0x30);  // the first file past the table
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::RomFs);
        a = with(0x2C + 0x0C, 0x4C);  // ... or where no whole entry fits
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::RomFs);
        a = with(0x48 + 0x04, 0);  // a directory whose file points at itself
        a.Patch(0x48 + 0x20, {'1'});
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK_EQ(a.reads, 2u + dlp_real::kPackageFiles);
        a = with(0x48 + 0x1C, 38);  // a shorter name
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK_EQ(p.first_name_bytes, 38u);
        CHECK(std::string(p.first_name) == "00040001000f8200.ci");  // (only the units the entry says are its name)
        a = with(0x48 + 0x1C, 0x7FFFFFF0);  // a name that says it is longer than anything: twenty units of it, no more
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK_EQ(p.first_name_bytes, 0x7FFFFFF0u);
        CHECK(std::string(p.first_name) == kName);  // (twenty units, whatever the entry says)
        a = with(0x48 + 0x20 + 2 * 16, 0x002F);  // "/cia"
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK(std::string(p.first_name) == "00040001000f8200/?ia");  // (the unit after it went to zero with the patch: not text)
        a = with(0x48 + 0x20 + 2 * 3, 0x003030B2);  // a unit that is not ASCII, and one that is a control character
        a.Patch(0x48 + 0x20 + 2 * 5, {0x07, 0x00});
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::NoFile);
        CHECK(std::string(p.first_name) == "000?0?01000f8200.cia");  // (what goes to the log is printable)
        a = with(0x48 + 0x14, 1);  // a file of more than 4 GB
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x48 + 0x10, 0x201F);  // a file shorter than a CIA's header
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x48 + 0x0C, 0x7FFFFFFF);  // its data far past the image
        CHECK(Find(a, kIslandLow, &p) != PackageWhy::Ok);
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x48 + 0x08, 0x00FFFFF0);  // its data past the image, by less
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Read);
        CHECK(a.outside);
        a = with(0x90 + 0x00, 0x2021);  // not a CIA
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x90 + 0x14, 0x3ABF);  // a meta block with no room for an SMDH
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x90 + 0x14, 0x3AC4);  // parts that do not add up to the file
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x90 + 0x08, 0xA40);
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        a = with(0x90 + 0x1C, 1);  // a content of more than 4 GB
        CHECK(Find(a, kIslandLow, &p) == PackageWhy::Cia);
        // a read that gives less than asked, at every step
        for (u32 k = 0; k < 4; ++k) {
            Image im;
            PutRomFs(im, 0, {f});
            im.fail_at = k;
            CHECK(Find(im, kIslandLow, &p) == PackageWhy::Read);
            CHECK_EQ(im.reads, k + 1);
        }
        Image empty;
        CHECK(Find(empty, kIslandLow, &p) == PackageWhy::Read);
        for (PackageWhy w : {PackageWhy::Ok, PackageWhy::Read, PackageWhy::RomFs, PackageWhy::NoFile, PackageWhy::Cia}) {
            CHECK(dlp_real::PackageWhyText(w) != nullptr && dlp_real::PackageWhyText(w)[0] != '\0');
        }
    }
    // damaged images: whatever it answers, it reads little, never outside what an honest reader gives, and an Ok adds up
    {
        Image good;
        RomFile f{kName, CiaShape(), {}};
        PutRomFs(good, 0, {f});
        std::vector<u8> flat(0xB0, 0);
        CHECK(Image::Read(&good, 0, flat.data(), static_cast<u32>(flat.size())));
        std::mt19937 rng(20261114);
        u32 ok = 0, refused = 0;
        for (u32 round = 0; round < 20000; ++round) {
            std::vector<u8> bytes = flat;
            const u32 changes = 1 + rng() % 4;
            for (u32 k = 0; k < changes; ++k) {
                const u32 at = rng() % bytes.size();
                switch (rng() % 4) {
                case 0: bytes[at] = static_cast<u8>(rng()); break;
                case 1: bytes[at] ^= static_cast<u8>(1u << (rng() % 8)); break;
                case 2: bytes[at] = 0xFF; break;
                default: bytes[at] = 0; break;
                }
            }
            Image im;
            im.Put(0, bytes);
            im.size = rng() % 3 == 0 ? rng() % (good.size + 1) : good.size;  // (sometimes cut short)
            im.pieces[0].bytes.resize(static_cast<std::size_t>(std::min<u64>(im.size, bytes.size())));
            ChildPackage p = {};
            const PackageWhy why = Find(im, kIslandLow, &p);
            CHECK(im.reads <= 4u + dlp_real::kPackageFiles + 2u);
            CHECK(im.most <= 0x60);
            bool printable = p.first_name[dlp_real::kCiaNameUnits] == '\0';  // (the name for the log: ended, and nothing but text)
            for (std::size_t k = 0; printable && p.first_name[k] != '\0'; ++k) {
                printable = p.first_name[k] >= 0x20 && p.first_name[k] < 0x7F;
            }
            CHECK(printable);
            if (why == PackageWhy::Ok) {
                ++ok;
                CHECK(p.required_size >= p.transfer_size + 0x400 + dlp_real::kSmdhBytes);
                CHECK(p.smdh_at >= p.transfer_size + 0x400u);
                CHECK(!im.outside);
            } else {
                ++refused;
                CHECK(why == PackageWhy::Read || why == PackageWhy::RomFs || why == PackageWhy::NoFile || why == PackageWhy::Cia);
            }
        }
        std::printf("   20000 damaged images: %u still a package, %u refused\n", ok, refused);
        CHECK(ok > 100 && refused > 5000);
    }
    Ran("P", c, kLeastPackage);
}

// ---------------------------------------------------------------------------------------------------------------------
// N: the adapter on the wire

/// The host: a console, the adapter, the lobby of Mario Party: Island Tour on it (four players, as its row of the plugin).
struct THost {
    THost(MockRoom& room, u8 last, u32 tid_low) : con(room, last), net(con.engine.get()), lobby(&net, Player::Cfg(tid_low, last, "Host")) {
        con.sniff = [this](const WifiPacket& w) {
            EtherType t;
            SecureDataHeader h;
            if (w.type != WifiPacket::Type::Data || !GetFrameEtherType(w.data, &t) || t != EtherType::SecureData ||
                !ParseSecureDataHeader(w.data, &h) || h.ActualDataSize() > w.data.size() || h.data_channel != 1) {
                return;
            }
            std::lock_guard<std::mutex> l(m);
            sent.emplace_back(w.data.end() - h.ActualDataSize(), w.data.end());
            dest_ok = dest_ok && h.dest_node_id == 0xFFFF;
        };
    }
    void Pump() {
        net.Pump();
        lobby.Pump();
    }
    void Open(u32 child_index = 0) {
        CHECK_EQ(lobby.ServerInitialize(3, child_index, nullptr), 0u);  // the game: three clients, no name
        CHECK_EQ(lobby.ServerStartAccepting(true, 0), 0u);              // by hand, channel 0
        CHECK(net.Holds());
    }
    std::vector<std::vector<u8>> Sent() {
        std::lock_guard<std::mutex> l(m);
        return sent;
    }
    Console con;
    RealDlpNet net;
    DlpLobby lobby;
    std::mutex m;
    std::vector<std::vector<u8>> sent;  // the payloads of channel 1, in order
    bool dest_ok = true;
};

template <typename F>
static bool PumpUntil(THost& h, u32 ms, F cond) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        h.Pump();
        if (cond()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        Sleep(2);
    }
}

/// A spectator comes in (as a searcher does) and the host is pumped until it has sent `groups` whole groups.
static std::vector<std::vector<u8>> Watch(THost& h, Console& spec, u32 groups) {
    u16 name[kNameUnits] = {'S', 'p'};
    CHECK_EQ(spec.engine->InitializeForLobby(0x5E, name), 0u);
    CHECK_EQ(spec.engine->ConnectBeginDlp(h.con.mac, 11, ConnectionType::Spectator), 0u);
    CHECK(PumpUntil(h, 3000, [&] { return spec.engine->ConnectPoll() == 1; }));
    CHECK(PumpUntil(h, 500 * groups + 1500, [&] { return h.net.counters().announce >= 5 * groups; }));
    return h.Sent();
}

/// Frame `k` of what the host sent is frame k % 5 of the announcement of `child`, with `title` or without, byte for byte.
static bool IsFrame(const std::vector<u8>& sent, std::size_t k, u64 child, const TitleBlock* title) {
    u8 want[dlp_real::kBufferBytes];
    const std::size_t n = dlp_real::AnnounceFrame(want, static_cast<unsigned>(k % 5), child, kIslandPlayers, dlp_real::kTxSeed, title);
    return sent.size() == n && std::memcmp(sent.data(), want, n) == 0;
}

static void N_Wire() {
    const int c = g_checks;
    std::printf("N: the adapter's announcement on the wire, with and without a title\n");
    const std::vector<u8> smdh = MakeSmdh(31, 0x073F);
    static TitleBlock block;  // (static: the adapter keeps the pointer for as long as it lives)
    block = IslandBlock(smdh, 4);
    // no title given: the announcement of before
    {
        g_rules.Reset();
        MockRoom room;
        THost h(room, 1, kIslandLow);
        Console spec(room, 3);
        Ticker ticker({h.con.engine.get(), spec.engine.get()});
        h.Open();
        CHECK(!h.net.titled());
        const std::vector<std::vector<u8>> sent = Watch(h, spec, 2);
        CHECK(sent.size() >= 10);
        bool all = true;
        for (std::size_t k = 0; k < sent.size(); ++k) {
            all = all && IsFrame(sent[k], k, kIslandChild, nullptr);
        }
        CHECK(all);
        CHECK(h.dest_ok);
        CHECK_RULES();
    }
    // a title: every frame of every group carries it, ONE frame per Pump as before, and Azahar's client reads the names and the icon
    {
        g_rules.Reset();
        MockRoom room;
        THost h(room, 1, kIslandLow);
        Console spec(room, 3);
        Ticker ticker({h.con.engine.get(), spec.engine.get()});
        h.net.SetTitleBlock(&block);  // (the plugin: at start, before the game opens its lobby)
        CHECK(h.net.titled());
        h.Open();
        u16 name[kNameUnits] = {'S', 'p'};
        CHECK_EQ(spec.engine->InitializeForLobby(0x5E, name), 0u);
        CHECK_EQ(spec.engine->ConnectBeginDlp(h.con.mac, 11, ConnectionType::Spectator), 0u);
        CHECK(PumpUntil(h, 3000, [&] { return spec.engine->ConnectPoll() == 1; }));
        u32 most_per_pump = 0;
        const u64 end = platform::NowMs() + 1300;
        while (platform::NowMs() < end) {
            const u32 before = h.net.counters().announce;
            h.Pump();
            most_per_pump = std::max<u32>(most_per_pump, h.net.counters().announce - before);
            Sleep(RealDlpNet::kPumpMs + 1);
        }
        CHECK_EQ(most_per_pump, 1u);
        CHECK(PumpUntil(h, 1500, [&] { return h.net.counters().announce % 5 == 0; }));
        const std::vector<std::vector<u8>> sent = h.Sent();
        CHECK(sent.size() >= 10 && sent.size() % 5 == 0);
        bool all = true;
        for (std::size_t k = 0; k < sent.size(); ++k) {
            all = all && IsFrame(sent[k], k, kIslandChild, &block);
            CHECK(dlp_real::ParseAnnounce(sent[k].data(), sent[k].size()).kind == dlp_real::Kind::Announce);
        }
        CHECK(all);
        CHECK(!IsFrame(sent[0], 0, kIslandChild, nullptr));
        if (sent.size() >= 5) {
            const std::vector<u8> group[5] = {sent[0], sent[1], sent[2], sent[3], sent[4]};
            const AzaharTitleInfo read = AzaharReads(group);
            CheckAzaharReads(read, smdh, 4);
            CHECK_EQ(read.size, kDlpRealIslandTransferSize);
        }
        CHECK(h.dest_ok);
        CHECK_RULES();
    }
    // the title of ANOTHER child (the game asked for the child 1; a game that is not this one): none of it on the wire
    for (int other = 0; other < 2; ++other) {
        g_rules.Reset();
        MockRoom room;
        THost h(room, 1, other == 0 ? kIslandLow : 0x001D1A00u);
        Console spec(room, 3);
        Ticker ticker({h.con.engine.get(), spec.engine.get()});
        h.net.SetTitleBlock(&block);
        h.Open(other == 0 ? 1 : 0);
        const u64 child = other == 0 ? (kIslandChild | 1) : kLuigiChild;
        const std::vector<std::vector<u8>> sent = Watch(h, spec, 1);
        CHECK(sent.size() >= 5);
        bool all = true;
        for (std::size_t k = 0; k < sent.size(); ++k) {
            all = all && IsFrame(sent[k], k, child, nullptr);
        }
        CHECK(all);
        CHECK_RULES();
    }
    // a title given (from another thread, as the plugin's) while a group is under way: that group ends as it began, the next carries it
    {
        g_rules.Reset();
        MockRoom room;
        THost h(room, 1, kIslandLow);
        Console spec(room, 3);
        Ticker ticker({h.con.engine.get(), spec.engine.get()});
        h.Open();
        u16 name[kNameUnits] = {'S', 'p'};
        CHECK_EQ(spec.engine->InitializeForLobby(0x5E, name), 0u);
        CHECK_EQ(spec.engine->ConnectBeginDlp(h.con.mac, 11, ConnectionType::Spectator), 0u);
        CHECK(PumpUntil(h, 3000, [&] { return spec.engine->ConnectPoll() == 1; }));
        CHECK(PumpUntil(h, 2500, [&] { return h.net.counters().announce % 5 == 2; }));  // (frames 0 and 1 of a group went)
        const u32 at = h.net.counters().announce;
        std::thread giver([&] { h.net.SetTitleBlock(&block); });
        giver.join();
        CHECK(PumpUntil(h, 3000, [&] { return h.net.counters().announce >= at + 3 + 10; }));
        const std::vector<std::vector<u8>> sent = h.Sent();
        CHECK(sent.size() >= at + 13);
        bool before = true, after = true;
        for (std::size_t k = 0; k < sent.size(); ++k) {
            if (k < at + 3u) {
                before = before && IsFrame(sent[k], k, kIslandChild, nullptr);
            } else {
                after = after && IsFrame(sent[k], k, kIslandChild, &block);
            }
        }
        CHECK(before);  // (the group under way: all five without)
        CHECK(after);   // (every later group: all five with)
        CHECK_RULES();
    }
    // the same host closes its lobby and opens another for the child 1: the title was the child 0's, so none of it from then on
    // (whether a group carries it is asked again at every group, never remembered from the lobby before)
    {
        g_rules.Reset();
        MockRoom room;
        THost h(room, 1, kIslandLow);
        Console spec(room, 3), spec2(room, 4);
        Ticker ticker({h.con.engine.get(), spec.engine.get(), spec2.engine.get()});
        h.net.SetTitleBlock(&block);
        h.Open(0);
        const std::vector<std::vector<u8>> first = Watch(h, spec, 1);
        CHECK(first.size() >= 5);
        bool titled = true;
        for (std::size_t k = 0; k < first.size(); ++k) {
            titled = titled && IsFrame(first[k], k, kIslandChild, &block);
        }
        CHECK(titled);
        CHECK_EQ(h.lobby.ServerFinalize(), 0u);
        CHECK(!h.net.Holds());
        const std::size_t before = h.Sent().size();
        h.Open(1);
        CHECK(h.net.titled());  // (the adapter still has the block: it is the child that is another)
        const u32 sent_before = h.net.counters().announce;
        u16 name[kNameUnits] = {'S', '2'};
        CHECK_EQ(spec2.engine->InitializeForLobby(0x5F, name), 0u);
        CHECK_EQ(spec2.engine->ConnectBeginDlp(h.con.mac, 11, ConnectionType::Spectator), 0u);
        CHECK(PumpUntil(h, 3000, [&] { return spec2.engine->ConnectPoll() == 1; }));
        CHECK(PumpUntil(h, 2500, [&] { return h.net.counters().announce >= sent_before + 5; }));
        const std::vector<std::vector<u8>> all = h.Sent();
        CHECK(all.size() >= before + 5);
        bool second = true;
        for (std::size_t k = before; k < all.size(); ++k) {
            second = second && IsFrame(all[k], k - before, kIslandChild | 1, nullptr);
        }
        CHECK(second);  // (the lobby of the child 1: all without)
        CHECK_RULES();
    }
    Ran("N", c, kLeastWire);
}

// ---------------------------------------------------------------------------------------------------------------------
// L: the list of the games

static void L_List() {
    const int c = g_checks;
    std::printf("L: every game whose host has the real Download Play announces its title (no list, 2026-11-15)\n");
    // Island Tour, Luigi's Mansion, Mario Kart 7: every region of each, as any application
    static const u64 kYes[] = {0x00040000000DD400ull, 0x00040000000F8100ull, 0x00040000000F8200ull, 0x0004000000119300ull, 0x0004000000119B00ull,
                               0x00040000001D1A00ull, 0x00040000001D1900ull, 0x00040000001D1800ull, 0x00040000001D4F00ull,
                               0x0004000000030700ull, 0x0004000000030800ull, 0x0004000000030600ull, 0x0004000000030A00ull, 0x000400000008B400ull};
    for (const u64 id : kYes) {
        CHECK(dlp_real::AnnouncesTitle(id));
        CHECK(!dlp_real::AnnouncesTitle(id | 0x0000000100000000ull));  // (the child's id is not the game's: a child never hosts)
        CHECK(!dlp_real::AnnouncesTitle((id & 0xFFFFFFFFull) | 0x0004000E00000000ull));  // (nor an update)
    }
    // never a system title (the Download Play application itself) or no title at all
    static const u64 kNot[] = {0x0004001000022100ull, 0x0004003000008F02ull, 0ull, 0x0004000000000000ull, ~0ull};
    for (const u64 id : kNot) {
        CHECK(!dlp_real::AnnouncesTitle(id));
    }
    // against the plugin's own table (the Download Play column of azl/title_rules.hpp, every application id of a row that has it): every id of
    // the real Download Play announces (they are the ids that get the adapter the title is given to), and the plugin's answer is the column's
    u32 rows = 0, real_rows = 0;
    for (std::size_t r = 0; r < title_rules::kRowCount; ++r) {
        const title_rules::Dlp& game = title_rules::kRows[r].dlp;  // (the name the lines below read)
        for (const u32 low : title_rules::kRows[r].low) {
            const u64 id = 0x0004000000000000ull | low;
            if (low == 0 || game.mode == DlpGameMode::None) {
                continue;
            }
            ++rows;
            CHECK(azl::ctr::DlpModeOf(id) == game.mode);
            if (game.mode == DlpGameMode::RealDlp) {
                ++real_rows;
                CHECK(dlp_real::AnnouncesTitle(id));
            }
        }
    }
    CHECK(rows >= 19);
    CHECK(real_rows >= 14); // (Island Tour 5, Luigi's Mansion 4, Mario Kart 7 5)
    CHECK(azl::ctr::DlpModeOf(0x00040000000F8200ull) == DlpGameMode::RealDlp);
    Ran("L", c, kLeastList);
}

int main() {
    std::printf("dlp_title_block_test: the title in the announcement of the real Download Play\n");
    S_Smdh();
    F_Frames();
    P_Package();
    N_Wire();
    L_List();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
