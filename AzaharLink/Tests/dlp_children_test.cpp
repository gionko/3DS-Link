// The children 3DS Link prepares for the system's Download Play application (1.1.4, piece BASE: contract K1-K4 of
// analysis/piano-1.1.4-2026-11-15/specifiche/7-CONTRATTO.md), on a PC, pure: the prepared child's id of every sold game of the catalogue
// (title_rules::PreparedOf, IsPreparedId), the index the app writes and the plugin reads (FormatRow / ParseIndex: the example of
// analysis/island-tour-2026-11-13/plugin/app/esempio-dlp-children.txt, round trips, sixteen rows, every kind of bad line, a text cut in a line),
// the block of 5,000 bytes (MakeBlock from an SMDH, its name), the verdicts (JudgeAll with fakes that count what they are asked, VerdictFor x
// MayJoin, a block of another version), the small line under a host's name in ten UTF-16 units with the name shortened and never the mark (both
// styles, ten thousand names at random), the writers into the records of dlp:CLNT (byte for byte, everything else untouched), and that the app's
// paths (ConfigApp/source/dlp_guest_paths.cpp, built here as it is) are the plugin's.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "azl/dlp_children.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_paths.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

namespace dc = azl::dlp_children;
namespace tr = azl::title_rules;
using azl::u16;
using azl::u32;
using azl::u64;
using azl::u8;
using dc::Verdict;

constexpr u64 kIslandEur = 0x00040000000F8200ull, kIslandEurChild = 0x00040001000F8200ull, kIslandEurPrepared = 0x000400000F8F8200ull;
constexpr u64 kIslandUsa = 0x00040000000F8100ull, kIslandUsaChild = 0x00040001000F8100ull;

// ---- K1: the prepared child of every sold game ----------------------------------------------------------------------------------------
struct CatalogTitle {
    u32 low;
    u32 family;
};
const CatalogTitle kCatalog[] = {
#include "title_families.inc"
};

void TestPreparedIds() {
    std::printf("K1: the prepared child of a game, over the whole catalogue of sold games\n");
    CHECK(tr::PreparedOf(kIslandEur) == kIslandEurPrepared);
    CHECK(tr::IsPreparedId(kIslandEurPrepared));
    CHECK(tr::kAppTid == 0x000400000F83D100ull);
    CHECK(tr::PreparedOf(kIslandEurChild) == 0);                 // a child
    CHECK(tr::PreparedOf(0x0004000E000F8200ull) == 0);           // an update
    CHECK(tr::PreparedOf(0x0004001000022100ull) == 0);           // a system application (the Download Play application)
    CHECK(tr::PreparedOf(kIslandEurPrepared) == 0);              // a prepared child itself (the game itself would be its id)
    CHECK(tr::PreparedOf(tr::kAppTid) == 0);                     // 3DS Link
    CHECK(tr::PreparedOf(0x000400000003D100ull) == 0);           // the game whose prepared child would be 3DS Link
    CHECK(tr::PreparedOf(0x00040000007FFF00ull) == 0x000400000FFFFF00ull); // the last of the 23 bits
    CHECK(tr::PreparedOf(0x0004000000800000ull) == 0x000400000F800000ull); // past them: the 23 low bits, no bound (the user's decision)
    CHECK(tr::PreparedOf(0x000400000F700000ull) == 0x000400000FF00000ull); // the band 0x0F7xxxxx of the catalogue
    CHECK(tr::PreparedOf(0x000400000F715F00ull) == 0x000400000FF15F00ull);
    CHECK(tr::PreparedOf(0x0004000000000000ull) == 0);
    CHECK(tr::IsPreparedId(0x000400000F800000ull) && tr::IsPreparedId(0x000400000FFFFFFFull));
    CHECK(!tr::IsPreparedId(0x000400000F7FFFFFull) && !tr::IsPreparedId(0x0004000010000000ull));
    CHECK(!tr::IsPreparedId(0x000400010F8F8200ull) && !tr::IsPreparedId(0x0004000E0F8F8200ull) && !tr::IsPreparedId(0));

    const std::size_t n = sizeof(kCatalog) / sizeof(kCatalog[0]);
    CHECK(n == 5217);
    std::map<u64, u32> seen; // prepared id -> the family of the game that has it
    std::size_t with = 0, without = 0, beyond = 0, to_itself = 0, to_app = 0, same_family = 0, other_family = 0, not_users = 0;
    std::size_t sys_wrong = 0;           // (the build 2h) a system id that is not the key's under 00040010
    std::map<u64, u32> sys_seen;         // ... without its low byte (pm's search): how many prepared ids share one
    for (const CatalogTitle& t : kCatalog) {
        const u64 game = 0x0004000000000000ull | t.low;
        const u64 p = tr::PreparedOf(game);
        // the user's formula (REGOLE-IMPLEMENTAZIONE.md), written here again on purpose: every sold id gets exactly it
        not_users += p != (0x0004000000000000ull | (0x0F800000u | (t.low & 0x007FFFFFu))) ? 1 : 0;
        if (p == 0) {
            ++without;
        } else {
            ++with;
            beyond += t.low > tr::kPreparedLowMask ? 1 : 0;
            CHECK(tr::IsPreparedId(p));
            to_itself += p == game ? 1 : 0;
            to_app += p == tr::kAppTid ? 1 : 0;
            // (the build 2h) its system id, the ONE id the console sees: 00040010 with the same low word, in the band, back to the key, never a
            // system application's (their low words are 0x0002xxxx and 0x2002xxxx: Azahar's system_titles.cpp), never 3DS Link's without its low byte
            const u64 sys = tr::PreparedSystemOf(p);
            sys_wrong += static_cast<u32>(sys >> 32) != tr::kTitleHighSystemApplication || static_cast<u32>(sys) != static_cast<u32>(p) ||
                                 tr::PreparedSlotOfProcess(sys) != p || (static_cast<u32>(sys) & 0xFFFF0000u) == 0x00020000u ||
                                 (static_cast<u32>(sys) & 0xFFFF0000u) == 0x20020000u
                             ? 1
                             : 0;
            sys_seen[sys & ~0xFFull] += 1;
            const auto it = seen.find(p);
            if (it == seen.end()) {
                seen[p] = t.family;
            } else if (it->second == t.family) {
                ++same_family;
            } else {
                ++other_family;
                std::printf("  two families have the prepared id %016llX\n", static_cast<unsigned long long>(p));
            }
        }
    }
    std::printf("  %zu sold ids: %zu with a prepared child (%zu of them above the 23 bits: the band 0x0F7xxxxx of the catalogue), %zu without\n", n, with,
                beyond, without);
    CHECK(to_itself == 0);
    CHECK(to_app == 0);
    CHECK(same_family == 0);
    CHECK(other_family == 0);
    CHECK(without == 0);              // (the user's decision: a prepared child for every sold game, the band 0x0F700000..0x0F715F00 too)
    CHECK(beyond == 257);             // (that band: 257 sold ids, whose children are 0x0FF00000..0x0FF15F00)
    CHECK(with + without == n);
    CHECK(not_users == 0);
    CHECK(sys_wrong == 0);
    std::size_t sys_shared = 0;
    for (const auto& kv : sys_seen) {
        sys_shared += kv.second > 1 ? kv.second - 1 : 0; // (two keys of one family whose low bytes differ: a variation of the same game - never two games)
    }
    std::printf("  (the build 2h) system ids 00040010|band of the catalogue: %zu ids, %zu sharing a key without its low byte\n", sys_seen.size(), sys_shared);
    CHECK(sys_seen.size() + sys_shared == with);
    CHECK(tr::PreparedSystemOf(tr::kAppTid) != 0 && (tr::PreparedSystemOf(tr::kAppTid) & ~0xFFull) != (tr::kAppTid & ~0xFFull));
}

// ---- the index ---------------------------------------------------------------------------------------------------------------------
dc::Row R(u64 child, u64 game, u64 prepared, char where, u16 version) {
    dc::Row r{};
    r.child = child;
    r.game = game;
    r.prepared = prepared;
    r.where = where;
    r.version = version;
    return r;
}

// Parses `text` from a buffer of exactly its size (AddressSanitizer catches a read past it).
bool Parse(const std::string& text, dc::Index* out) {
    std::vector<char> buf(text.begin(), text.end());
    return dc::ParseIndex(buf.empty() ? nullptr : buf.data(), buf.size(), out);
}

std::string Line(const dc::Row& r) {
    char out[dc::kRowBytes + 1];
    const std::size_t n = dc::FormatRow(r, out);
    return std::string(out, n);
}

bool SameRow(const dc::Row& a, const dc::Row& b) {
    return a.child == b.child && a.game == b.game && a.prepared == b.prepared && a.where == b.where && a.version == b.version;
}

void TestIndex() {
    std::printf("the index: the example, round trips, sixteen rows, bad lines, a text cut in a line\n");
    const std::string example =
        "#dlp-children 1\n"
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:0\n"
        "00040001000F8100:00040000000F8100:0000000000000000:C:0\n";
    dc::Index idx;
    CHECK(Parse(example, &idx));
    CHECK(idx.count == 2 && idx.skipped == 0);
    CHECK(SameRow(idx.rows[0], R(kIslandEurChild, kIslandEur, kIslandEurPrepared, 'S', 0)));
    CHECK(SameRow(idx.rows[1], R(kIslandUsaChild, kIslandUsa, 0, 'C', 0)));
    CHECK(dc::FindRow(idx, kIslandEurChild) == 0 && dc::FindRow(idx, kIslandUsaChild) == 1 && dc::FindRow(idx, 0x00040001000F8000ull) == -1);
    CHECK(dc::FindPrepared(idx, kIslandEurPrepared) == 0 && dc::FindPrepared(idx, 0) == -1 && dc::FindPrepared(idx, tr::kAppTid) == -1);
    // the writer gives the example back, byte for byte
    CHECK(std::string("#dlp-children 1\n") + Line(idx.rows[0]) + Line(idx.rows[1]) == example);
    // the longest line is kRowBytes, and it reads back
    const dc::Row longest = R(0x00040001007FFF00ull, 0x00040000007FFF00ull, 0x000400000FFFFF00ull, 'C', 65535);
    CHECK(Line(longest).size() == dc::kRowBytes);
    CHECK(Parse("#dlp-children 1\n" + Line(longest), &idx) && idx.count == 1 && SameRow(idx.rows[0], longest));
    // a row that is not sound is not written
    CHECK(Line(R(kIslandEurChild, kIslandEur, tr::kAppTid, 'S', 0)).empty());
    CHECK(Line(R(kIslandEur, kIslandEur, 0, 'S', 0)).empty());
    CHECK(Line(R(kIslandEurChild, kIslandEur, 0, 'X', 0)).empty());
    // another header: as if there were no file
    CHECK(!Parse("#dlp-children 2\n" + example.substr(16), &idx) && idx.count == 0);
    CHECK(!Parse("", &idx) && !Parse("#dlp-children 1", &idx) && !Parse(" #dlp-children 1\n", &idx) && !Parse("#dlp-children 10\n", &idx));
    CHECK(Parse("#dlp-children 1\n", &idx) && idx.count == 0 && idx.skipped == 0);
    // CRLF, '#' lines and empty lines; lower-case digits and where
    CHECK(Parse("#dlp-children 1\r\n\r\n# a tag of a later version\r\n\n00040001000f8200:00040000000f8200:000400000f8f8200:s:3073\r\n", &idx));
    CHECK(idx.count == 1 && idx.skipped == 0 && SameRow(idx.rows[0], R(kIslandEurChild, kIslandEur, kIslandEurPrepared, 'S', 3073)));
    // every kind of bad line is skipped and counted; the good line after them is kept
    const char* bad[] = {
        "00040001000F8200:00040000000F8200:000400000F8F8200:S",          // 4 fields
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:0:1",      // 6 fields
        "0040001000F8200:00040000000F8200:000400000F8F8200:S:0",         // 15 digits
        "000040001000F8200:00040000000F8200:000400000F8F8200:S:0",       // 17 digits
        "00040001000F820G:00040000000F8200:000400000F8F8200:S:0",        // not hex
        "00040001000F8200:00040000000F8200:000400000F8F8200:X:0",        // where
        "00040000000F8200:00040000000F8200:000400000F8F8200:S:0",        // the child is an application
        "00040001000F8200:00040001000F8200:000400000F8F8200:S:0",        // the game is a child
        "00040001000F8200:00040000000F8200:000400000F83D100:S:0",        // the prepared title is 3DS Link
        "00040001000F8200:00040000000F8200:000400000F8F8100:S:0",        // the prepared title of another game
        "00040001000F8200:00040000000F8200:000400010F8F8200:S:0",        // the prepared title is a child
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:70000",    // the version
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:",         // no version
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:-1",       // not a number
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:000001",   // six digits
        "00040001000F8200 00040000000F8200 000400000F8F8200 S 0",        // spaces
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:0 ",       // a space at the end
        "00040001000F8200:00040000000F8200:000400000F8F8200:S:0#",
    };
    std::string text = "#dlp-children 1\n";
    for (const char* b : bad) {
        text += b;
        text += "\n";
    }
    text += example.substr(16);
    CHECK(Parse(text, &idx));
    CHECK(idx.count == 2 && idx.skipped == sizeof(bad) / sizeof(bad[0]));
    CHECK(SameRow(idx.rows[0], R(kIslandEurChild, kIslandEur, kIslandEurPrepared, 'S', 0)));
    // a child twice, a prepared title twice: the first kept
    CHECK(Parse(example + "00040001000F8200:00040000000F8100:0000000000000000:C:5\n" + "00040001000F8000:00040000000F8200:000400000F8F8200:S:0\n", &idx));
    CHECK(idx.count == 2 && idx.skipped == 2 && idx.rows[0].game == kIslandEur);
    // eighteen sound rows: sixteen kept, two skipped; within the plugin's 1024 bytes
    std::string many = "#dlp-children 1\n";
    for (u32 i = 0; i < 18; ++i) {
        const u64 game = 0x0004000000100000ull + (static_cast<u64>(i) << 8);
        many += Line(R(game | 0x0000000100000000ull, game, tr::PreparedOf(game), i % 2 ? 'C' : 'S', static_cast<u16>(i * 1000)));
    }
    CHECK(Parse(many, &idx));
    CHECK(idx.count == dc::kMaxRows && idx.skipped == 2);
    CHECK(idx.rows[15].game == 0x0004000000100F00ull && idx.rows[15].version == 15000 && idx.rows[15].where == 'C');
    CHECK(16 + 16 * Line(longest).size() <= dc::kIndexMaxBytes);
    // a text cut inside a line (what ReadDataFile would give without its cut, or a file being written): every cut, nothing read past it,
    // and the cut line is never kept (its version could be the beginning of another)
    for (std::size_t cut = 0; cut <= example.size(); ++cut) {
        const bool ok = Parse(example.substr(0, cut), &idx);
        CHECK(ok == (cut >= 16));
        const std::size_t whole = cut >= example.size() ? 2 : (cut >= 16 + 55 ? 1 : 0);
        CHECK(idx.count == whole);
        CHECK(idx.skipped == ((cut > 16 && cut != 16 + 55 && cut != example.size()) ? 1 : 0));
    }
    // a line without its '\n' at the very end of the file is not kept either
    CHECK(Parse(example + "00040001000F8000:00040000000F8000:000400000F8F8000:S:1", &idx) && idx.count == 2 && idx.skipped == 1);
}

// ---- the block ---------------------------------------------------------------------------------------------------------------------
std::vector<u8> Smdh() {
    std::vector<u8> s(azl::dlp_real::kSmdhBytes, 0);
    std::memcpy(s.data(), "SMDH", 4);
    for (unsigned e = 0; e < azl::dlp_real::kSmdhTitles; ++e) {
        u8* t = s.data() + azl::dlp_real::kSmdhTitlesAt + e * azl::dlp_real::kSmdhTitleBytes;
        if (e == 3) {
            continue; // (French: no name, the next best is taken)
        }
        for (unsigned i = 0; i < 0x200; i += 2) {
            azl::StoreLE16(t + i, static_cast<u16>(0x4100 + e * 0x100 + i / 2)); // (a pattern of the entry and the place)
        }
    }
    for (std::size_t i = 0; i < azl::kTitleInfoIconSize; ++i) {
        s[azl::dlp_real::kSmdhBigIconAt + i] = static_cast<u8>(i * 7 + 1);
    }
    return s;
}

void TestBlock() {
    std::printf("the block: from an SMDH, its layout, its name\n");
    const std::vector<u8> smdh = Smdh();
    std::vector<u8> block(dc::kBlockBytes, 0xCC);
    CHECK(dc::MakeBlock(block.data(), smdh.data(), smdh.size(), 2 /* German */, 0x0000ABCDu, 0x01EE8DC0u));
    const u8* de = smdh.data() + azl::dlp_real::kSmdhTitlesAt + 2 * azl::dlp_real::kSmdhTitleBytes;
    CHECK(std::memcmp(block.data() + dc::kBlockShortAt, de, 128) == 0);
    CHECK(std::memcmp(block.data() + dc::kBlockLongAt, de + 128, 256) == 0);
    CHECK(std::memcmp(block.data() + dc::kBlockIconAt, smdh.data() + azl::dlp_real::kSmdhBigIconAt, azl::kTitleInfoIconSize) == 0);
    CHECK(dc::BlockTransfer(block.data()) == 0x0000ABCDu && dc::BlockRequired(block.data()) == 0x01EE8DC0u);
    CHECK(block[4992] == 0xCD && block[4993] == 0xAB && block[4999] == 0x01); // (little-endian)
    // a language with no name: English
    CHECK(dc::MakeBlock(block.data(), smdh.data(), smdh.size(), 3, 1, 2));
    CHECK(std::memcmp(block.data(), smdh.data() + azl::dlp_real::kSmdhTitlesAt + 1 * azl::dlp_real::kSmdhTitleBytes, 128) == 0);
    // not an SMDH, or a short one: refused, the block untouched
    std::vector<u8> before = block;
    std::vector<u8> not_smdh = smdh;
    not_smdh[0] = 'X';
    CHECK(!dc::MakeBlock(block.data(), not_smdh.data(), not_smdh.size(), 1, 9, 9));
    CHECK(!dc::MakeBlock(block.data(), smdh.data(), smdh.size() - 1, 1, 9, 9));
    CHECK(block == before);
    // no name at all: empty names, the icon
    std::vector<u8> nameless(azl::dlp_real::kSmdhBytes, 0);
    std::memcpy(nameless.data(), "SMDH", 4);
    nameless[azl::dlp_real::kSmdhBigIconAt] = 0x5A;
    CHECK(dc::MakeBlock(block.data(), nameless.data(), nameless.size(), 1, 0, 0));
    CHECK(std::all_of(block.begin(), block.begin() + dc::kBlockIconAt, [](u8 b) { return b == 0; }) && block[dc::kBlockIconAt] == 0x5A);
    char name[dc::kBlockNameBytes];
    dc::BlockName(kIslandEurPrepared, name);
    CHECK(std::string(name) == "000400000F8F8200.bin");
}

// ---- the verdicts ------------------------------------------------------------------------------------------------------------------
struct Fake {
    std::set<u64> titles;        // present (any media)
    std::set<u64> plugins;       // prepared ids with a plugin
    std::vector<std::string> asked;
};
bool FakeTitle(void* ctx, u64 tid, u8 media, u32 content) {
    Fake* f = static_cast<Fake*>(ctx);
    char line[64];
    std::snprintf(line, sizeof(line), "title %016llX media %u content %u", static_cast<unsigned long long>(tid), media, content);
    f->asked.push_back(line);
    return f->titles.count(tid) != 0;
}
bool FakePlugin(void* ctx, u64 prepared) {
    Fake* f = static_cast<Fake*>(ctx);
    char line[64];
    std::snprintf(line, sizeof(line), "plugin %016llX", static_cast<unsigned long long>(prepared));
    f->asked.push_back(line);
    return f->plugins.count(prepared) != 0;
}

void TestVerdicts() {
    std::printf("the verdicts: JudgeAll asks the cheapest first, VerdictFor x MayJoin, another version\n");
    // five games: ready, no game, not prepared (0), prepared title missing, no plugin
    const u64 g[5] = {0x0004000000100000ull, 0x0004000000100100ull, 0x0004000000100200ull, 0x0004000000100300ull, 0x0004000000100400ull};
    std::string text = "#dlp-children 1\n";
    for (int i = 0; i < 5; ++i) {
        text += Line(R(g[i] | 0x0000000100000000ull, g[i], i == 2 ? 0 : tr::PreparedOf(g[i]), i == 1 ? 'C' : 'S', 1));
    }
    dc::Table* t = new dc::Table;
    dc::Clear(t);
    t->index_found = Parse(text, &t->index);
    CHECK(t->index_found && t->index.count == 5);
    Fake f;
    f.titles = {g[0], tr::PreparedOf(g[0]), g[2], g[3], g[4], tr::PreparedOf(g[4])};
    f.plugins = {tr::PreparedOf(g[0]), tr::PreparedOf(g[3])};
    const dc::Probe probe{&f, FakeTitle, FakePlugin};
    dc::JudgeAll(t->index, t->verdict, probe, dc::PresenceBy::FileSystem);
    const Verdict want[5] = {Verdict::Ready, Verdict::NoGame, Verdict::NotPrepared, Verdict::NotPrepared, Verdict::NoChildPlugin};
    for (int i = 0; i < 5; ++i) {
        CHECK(t->verdict[i] == static_cast<u8>(want[i]));
    }
    for (std::size_t i = 5; i < dc::kMaxRows; ++i) {
        CHECK(t->verdict[i] == static_cast<u8>(Verdict::Unknown));
    }
    const std::vector<std::string> asked = {
        "title 0004000000100000 media 1 content 2", "title 000400000F900000 media 1 content 0", "plugin 000400000F900000", // ready: all three
        "title 0004000000100100 media 2 content 2",                                                                       // no game (on the card): one
        // (prepared 0: nothing asked)
        "title 0004000000100300 media 1 content 2", "title 000400000F900300 media 1 content 0",                           // prepared title missing
        "title 0004000000100400 media 1 content 2", "title 000400000F900400 media 1 content 0", "plugin 000400000F900400", // no plugin
    };
    CHECK(f.asked == asked);
    // by the index: the titles are not asked, the plugin is
    f.asked.clear();
    u8 by_index[dc::kMaxRows];
    dc::JudgeAll(t->index, by_index, probe, dc::PresenceBy::Index);
    CHECK(f.asked.size() == 4);
    CHECK(by_index[0] == static_cast<u8>(Verdict::Ready) && by_index[1] == static_cast<u8>(Verdict::NoChildPlugin) &&
          by_index[2] == static_cast<u8>(Verdict::NotPrepared) && by_index[3] == static_cast<u8>(Verdict::Ready));

    // VerdictFor
    const u64 child0 = g[0] | 0x0000000100000000ull;
    CHECK(dc::VerdictFor(nullptr, child0, 0, 0) == Verdict::Unknown);
    CHECK(dc::VerdictFor(t, 0x00040001000F8200ull, 0, 0) == Verdict::NoRow);
    for (int i = 0; i < 5; ++i) { // (a Ready row whose block is not read yet: Unknown, refused)
        CHECK(dc::VerdictFor(t, g[i] | 0x0000000100000000ull, 0, 0) == (want[i] == Verdict::Ready ? Verdict::Unknown : want[i]));
    }
    // a table without an index: every host NoRow (shown not ready)
    dc::Table* empty = new dc::Table;
    dc::Clear(empty);
    CHECK(dc::VerdictFor(empty, child0, 0, 0) == Verdict::NoRow);
    // a row nobody judged: refused
    dc::Table* unjudged = new dc::Table;
    dc::Clear(unjudged);
    CHECK(Parse(text, &unjudged->index));
    CHECK(dc::VerdictFor(unjudged, child0, 0, 0) == Verdict::NotPrepared);
    // the sizes: the block not read yet -> Unknown (refused: another version cannot be told yet); with the block, equal or 0 -> Ready,
    // different -> OtherVersion
    CHECK(dc::VerdictFor(t, child0, 111, 222) == Verdict::Unknown);
    CHECK(dc::VerdictFor(t, child0, 112, 223) == Verdict::Unknown && !dc::MayJoin(dc::VerdictFor(t, child0, 112, 223)));
    CHECK(dc::VerdictFor(t, child0, 0, 0) == Verdict::Unknown);
    CHECK(dc::BlockStateOf(t, 0) == dc::BlockState::NotRead && dc::BlockStateOf(t, 5) == dc::BlockState::None &&
          dc::BlockStateOf(nullptr, 0) == dc::BlockState::None);
    std::vector<u8> block(dc::kBlockBytes, 0);
    const std::vector<u8> smdh = Smdh();
    CHECK(dc::MakeBlock(block.data(), smdh.data(), smdh.size(), 1, 111, 222));
    CHECK(dc::BlockOf(t, 0) == nullptr);
    CHECK(dc::PutBlock(t, 0, block.data()));
    CHECK(!dc::PutBlock(t, 0, smdh.data()));      // once
    CHECK(!dc::PutBlock(t, 5, block.data()));     // no such row
    CHECK(!dc::PutBlock(t, 1, nullptr));
    CHECK(dc::BlockOf(t, 0) == block.data() && dc::BlockOf(t, 1) == nullptr && dc::BlockOf(nullptr, 0) == nullptr);
    CHECK(dc::BlockStateOf(t, 0) == dc::BlockState::Read && dc::BlockStateOf(t, 1) == dc::BlockState::NotRead);
    CHECK(!dc::NoBlock(t, 0)); // (a block read stays)
    CHECK(dc::VerdictFor(t, child0, 111, 222) == Verdict::Ready);
    CHECK(dc::VerdictFor(t, child0, 0, 0) == Verdict::Ready);
    CHECK(dc::VerdictFor(t, child0, 111, 0) == Verdict::Ready);
    CHECK(dc::VerdictFor(t, child0, 0, 222) == Verdict::Ready);
    CHECK(dc::VerdictFor(t, child0, 112, 222) == Verdict::OtherVersion);
    CHECK(dc::VerdictFor(t, child0, 111, 223) == Verdict::OtherVersion);
    CHECK(dc::VerdictFor(t, child0, 0, 223) == Verdict::OtherVersion);
    std::vector<u8> zero_sizes = block;
    azl::StoreLE32(zero_sizes.data() + dc::kBlockTransferAt, 0);
    azl::StoreLE32(zero_sizes.data() + dc::kBlockRequiredAt, 0);
    CHECK(dc::PutBlock(t, 4, zero_sizes.data()));
    CHECK(dc::VerdictFor(t, g[4] | 0x0000000100000000ull, 5, 6) == Verdict::NoChildPlugin); // (a block never makes a refused row Ready)
    // no block (the app did not write it, or it is short): a Ready row is NotPrepared for good, whatever the host announces
    dc::Table* blockless = new dc::Table;
    dc::Clear(blockless);
    CHECK(Parse(text, &blockless->index));
    std::memcpy(blockless->verdict, t->verdict, sizeof(t->verdict));
    CHECK(dc::NoBlock(blockless, 0));
    CHECK(!dc::NoBlock(blockless, 0) && !dc::PutBlock(blockless, 0, block.data()) && !dc::NoBlock(blockless, 5)); // (once; no such row)
    CHECK(dc::BlockOf(blockless, 0) == nullptr && dc::BlockStateOf(blockless, 0) == dc::BlockState::None);
    CHECK(dc::VerdictFor(blockless, child0, 0, 0) == Verdict::NotPrepared && dc::VerdictFor(blockless, child0, 111, 222) == Verdict::NotPrepared);
    CHECK(!dc::MayJoin(dc::VerdictFor(blockless, child0, 112, 223)));
    CHECK(dc::NoBlock(blockless, 4) && dc::VerdictFor(blockless, g[4] | 0x0000000100000000ull, 0, 0) == Verdict::NoChildPlugin);
    // MayJoin: Ready only
    for (u8 v = 0; v <= 6; ++v) {
        CHECK(dc::MayJoin(static_cast<Verdict>(v)) == (v == static_cast<u8>(Verdict::Ready)));
    }
    delete t;
    delete empty;
    delete unjudged;
    delete blockless;
}

// ---- the small line ----------------------------------------------------------------------------------------------------------------
void Name(u16 (&out)[azl::kNameUnits], const char* ascii) {
    std::memset(out, 0, sizeof(out));
    for (u32 i = 0; i < azl::kNameUnits && ascii[i] != '\0'; ++i) {
        out[i] = static_cast<u8>(ascii[i]);
    }
}
std::u16string Units(const u16 (&line)[azl::kNameUnits]) {
    std::u16string s;
    for (u32 i = 0; i < azl::kNameUnits && line[i] != 0; ++i) {
        s.push_back(static_cast<char16_t>(line[i]));
    }
    return s;
}
std::u16string U(const char* ascii) {
    std::u16string s;
    for (; *ascii != '\0'; ++ascii) {
        s.push_back(static_cast<char16_t>(*ascii));
    }
    return s;
}

void TestLabel() {
    std::printf("the small line: ten units, the name shortened and never the mark, both styles\n");
    CHECK(dc::kMarkStyle == dc::MarkStyle::Text); // (G2.0: the system font could not be read on the PC)
    struct Case {
        const char* name;
        const char* text_ready;    // Text, ready
        const char* text_not;      // Text, not ready
        const char* glyph_name;    // Glyph: the name part
    };
    const Case cases[] = {
        {"", "OK", "NO", ""},
        {"G", "G OK", "G NO", "G"},
        {"Gio", "Gio OK", "Gio NO", "Gio"},
        {"Gionko", "Gionko OK", "Gionko NO", "Gionko"},
        {"Gionko7", "Gionko7 OK", "Gionko7 NO", "Gionko7"},
        {"Friend-A", "Kamisam OK", "Kamisam NO", "Friend-A"},
        {"FriendA9", "Kamisam OK", "Kamisam NO", "Friend-A"},
        {"FriendA10", "Kamisam OK", "Kamisam NO", "Friend-A"},
        {"Ab   ", "Ab OK", "Ab NO", "Ab"},
        {"Abcdef  xy", "Abcdef OK", "Abcdef NO", "Abcdef"}, // (the cut leaves spaces at the end: dropped too)
        {"          ", "OK", "NO", ""},
    };
    for (const Case& c : cases) {
        u16 host[azl::kNameUnits], out[azl::kNameUnits];
        Name(host, c.name);
        dc::MakeLabel(out, host, true, dc::MarkStyle::Text);
        CHECK(Units(out) == U(c.text_ready));
        dc::MakeLabel(out, host, false, dc::MarkStyle::Text);
        CHECK(Units(out) == U(c.text_not));
        dc::MakeLabel(out, host, true, dc::MarkStyle::Glyph);
        std::u16string want = U(c.glyph_name);
        if (!want.empty()) {
            want.push_back(u' ');
        }
        CHECK(Units(out) == want + u'✓');
        dc::MakeLabel(out, host, false, dc::MarkStyle::Glyph);
        CHECK(Units(out) == want.substr(0, want.size()) + u'✗');
    }
    // a pair the cut splits: its first half goes too; a pair that fits stays
    {
        u16 host[azl::kNameUnits] = {'A', 'B', 'C', 'D', 'E', 'F', 0xD83D, 0xDE00, 'x', 0};
        u16 out[azl::kNameUnits];
        dc::MakeLabel(out, host, true, dc::MarkStyle::Text); // budget 7: ABCDEF + the high half -> ABCDEF
        CHECK(Units(out) == U("ABCDEF OK"));
        dc::MakeLabel(out, host, true, dc::MarkStyle::Glyph); // budget 8: the pair fits
        CHECK(out[6] == 0xD83D && out[7] == 0xDE00 && out[8] == u' ' && out[9] == 0x2713);
    }
    // ten thousand names at random (any units, zeros anywhere): ten units, the mark last and whole, the name a prefix of the host's, zeros after
    u32 seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    };
    for (int k = 0; k < 10000; ++k) {
        u16 host[azl::kNameUnits], out[azl::kNameUnits];
        for (u16& u : host) {
            const u32 r = next() % 16;
            u = r == 0 ? 0 : (r == 1 ? u16(' ') : (r == 2 ? static_cast<u16>(0xD800 + next() % 0x800) : static_cast<u16>(1 + next() % 0xFFFF)));
        }
        const bool ready = (next() & 1) != 0;
        const dc::MarkStyle style = (next() & 1) != 0 ? dc::MarkStyle::Glyph : dc::MarkStyle::Text;
        for (u16& u : out) {
            u = 0xCCCC;
        }
        dc::MakeLabel(out, host, ready, style);
        u32 used = 0;
        while (used < azl::kNameUnits && out[used] != 0) {
            ++used;
        }
        for (u32 i = used; i < azl::kNameUnits; ++i) {
            CHECK(out[i] == 0);
        }
        const u32 mark_units = style == dc::MarkStyle::Glyph ? 1 : 2;
        CHECK(used >= mark_units && used <= azl::kNameUnits);
        if (style == dc::MarkStyle::Glyph) {
            CHECK(out[used - 1] == (ready ? 0x2713 : 0x2717));
        } else {
            CHECK(out[used - 2] == (ready ? u16('O') : u16('N')) && out[used - 1] == (ready ? u16('K') : u16('O')));
        }
        const u32 name_units = used > mark_units ? used - mark_units - 1 : 0;
        CHECK(name_units <= azl::kNameUnits - 1 - mark_units);
        if (used > mark_units) {
            CHECK(out[name_units] == u16(' ') && out[name_units - 1] != u16(' '));
            CHECK(!(out[name_units - 1] >= 0xD800 && out[name_units - 1] <= 0xDBFF));
        }
        for (u32 i = 0; i < name_units; ++i) {
            CHECK(out[i] == host[i] && host[i] != 0);
        }
    }
}

void TestWriters() {
    std::printf("the writers: the small line, title_long and the block into the records, byte for byte, nothing else touched\n");
    u16 host[azl::kNameUnits];
    Name(host, "FriendA10");
    // WriteLabel: only 0x1C..0x2F change
    for (u8 v = 0; v <= 6; ++v) {
        std::vector<u8> rec(azl::kServerInfoSize, 0xCC);
        dc::WriteLabel(rec.data(), host, static_cast<Verdict>(v));
        for (std::size_t i = 0; i < rec.size(); ++i) {
            if (i < 0x1C || i >= 0x30) {
                CHECK(rec[i] == 0xCC);
            }
        }
        u16 line[azl::kNameUnits];
        for (u32 i = 0; i < azl::kNameUnits; ++i) {
            line[i] = azl::LoadLE16(rec.data() + 0x1C + 2 * i);
        }
        if (v == 0) {
            CHECK(Units(line) == U("FriendA10")); // Unknown: the name as it is
        } else {
            CHECK(Units(line) == U(v == 1 ? "Kamisam OK" : "Kamisam NO"));
        }
    }
    {
        std::vector<u8> rec(azl::kServerInfoSize, 0xCC);
        dc::WriteLabel(rec.data(), host, Verdict::NoRow, dc::MarkStyle::Glyph);
        CHECK(azl::LoadLE16(rec.data() + 0x1C + 2 * 9) == 0x2717 && azl::LoadLE16(rec.data() + 0x1C + 2 * 8) == u16(' '));
    }
    // WriteTitleLong: the exact text of each verdict, zero-padded, only 0xA0..0x19F change
    const char* tails[7] = {"", " - join it from the 3DS Link app", " - turn this game on in 3DS Link", " - this game is not on this console",
                            " - prepare this game in 3DS Link", " - turn this game on again in 3DS Link", " - another version of the game"};
    for (u8 v = 0; v <= 6; ++v) {
        std::vector<u8> rec(azl::kTitleInfoSize, 0xCC);
        dc::WriteTitleLong(rec.data(), host, static_cast<Verdict>(v));
        const std::u16string want = U("Hosted by FriendA10") + U(tails[v]);
        std::u16string got;
        for (u32 i = 0; i < dc::kTitleLongUnits; ++i) {
            const u16 u = azl::LoadLE16(rec.data() + azl::kTitleInfoTitleLong + 2 * i);
            if (i < want.size()) {
                got.push_back(static_cast<char16_t>(u));
            } else {
                CHECK(u == 0);
            }
        }
        CHECK(got == want);
        for (std::size_t i = 0; i < rec.size(); ++i) {
            if (i < azl::kTitleInfoTitleLong || i >= azl::kTitleInfoIcon) {
                CHECK(rec[i] == 0xCC);
            }
        }
    }
    {
        u16 empty[azl::kNameUnits] = {};
        std::vector<u8> rec(azl::kTitleInfoSize, 0xCC);
        dc::WriteTitleLong(rec.data(), empty, Verdict::Unknown);
        CHECK(azl::LoadLE16(rec.data() + azl::kTitleInfoTitleLong) == u16('H') && azl::LoadLE16(rec.data() + azl::kTitleInfoTitleLong + 2 * 9) == u16(' ') &&
              azl::LoadLE16(rec.data() + azl::kTitleInfoTitleLong + 2 * 10) == 0);
    }
    // WriteFromBlock: title_short, the icon, the size; everything else untouched (5032 bytes, field by field)
    const std::vector<u8> smdh = Smdh();
    std::vector<u8> block(dc::kBlockBytes, 0);
    CHECK(dc::MakeBlock(block.data(), smdh.data(), smdh.size(), 1, 0x00012345u, 0x01EE8DC0u));
    std::vector<u8> rec(azl::kTitleInfoSize, 0xCC);
    dc::WriteFromBlock(rec.data(), block.data());
    for (std::size_t i = 0; i < rec.size(); ++i) {
        if (i >= azl::kTitleInfoTitleShort && i < azl::kTitleInfoTitleLong) {
            CHECK(rec[i] == block[dc::kBlockShortAt + (i - azl::kTitleInfoTitleShort)]);
        } else if (i >= azl::kTitleInfoIcon && i < azl::kTitleInfoSizeField) {
            CHECK(rec[i] == block[dc::kBlockIconAt + (i - azl::kTitleInfoIcon)]);
        } else if (i >= azl::kTitleInfoSizeField && i < azl::kTitleInfoSizeField + 4) {
            continue;
        } else {
            CHECK(rec[i] == 0xCC);
        }
    }
    CHECK(azl::LoadLE32(rec.data() + azl::kTitleInfoSizeField) == 0x00012345u);
}

// ---- the app's paths are the plugin's ------------------------------------------------------------------------------------------------
void TestAppPaths() {
    std::printf("the app's paths (ConfigApp/source/dlp_guest_paths.cpp) are the plugin's\n");
    CHECK(std::string("sdmc:") + dc::kIndexPath == dlpguest::IndexPath());
    CHECK(std::string("sdmc:") + dc::kGuestsDir == dlpguest::GuestsDir() + "/");
    CHECK(dlpguest::GuestsDir() == "sdmc:/3ds/3DSLink/dlp-guests");
    CHECK(dlpguest::BlockPath(kIslandEurPrepared) == "sdmc:/3ds/3DSLink/dlp-guests/000400000F8F8200.bin");
    CHECK(dlpguest::PreparedFor(kIslandEur) == kIslandEurPrepared && dlpguest::PreparedFor(kIslandEurChild) == 0);
    CHECK(std::string(dc::kIndexPath).rfind(dc::kGuestsDir, 0) == 0);
    CHECK(std::string(dc::kGuestsDir).rfind("/3ds/3DSLink/", 0) == 0);    // (the plugin's data folder, Sources/ctr/plugin.cpp kDataDir)
    CHECK(std::string(dc::kGuestsDir).find("/dlp/") == std::string::npos); // (3DS Link Delivery's folder is another)
}

} // namespace

int main() {
    TestPreparedIds();
    TestIndex();
    TestBlock();
    TestVerdicts();
    TestLabel();
    TestWriters();
    TestAppPaths();
    if (g_failures != 0) {
        std::printf("dlp_children_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("dlp_children_test: all %d checks passed\n", g_checks);
    return 0;
}
