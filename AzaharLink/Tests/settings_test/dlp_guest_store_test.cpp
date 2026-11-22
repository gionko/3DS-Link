// The files, the registry and the decisions of the Download Play children 3DS Link prepares (1.1.4, piece APP-STORE: analysis/piano-1.1.4-2026-11-15/
// specifiche/6-APP-boot9.md 4.4-4.7 with its critic 6-APP-boot9-CRITICO.md 3-5 and K9 of 7-CONTRATTO.md): ConfigApp/source/dlp_guest_store.cpp,
// dlp_guest_decide.hpp, dlp_guest_exclude.hpp and games.cpp's DisableGuestFolder, the very files, built on a PC with a stand-in for <3ds.h>, against a
// folder called "sdmc:" under a temporary directory. What it holds: a .guest record written and read back (and every record that is not sound refused:
// a homebrew of the band, 3DS Link's id, a prepared id that is not the game's own), the records of a folder (a .bak alone is the record), which titles
// are ours (lock-free slots), children.cache, the index the plugin reads (its own parser reads it back; 16 rows at most), a block, the leftovers (a
// .bak dropped only when its file is whole, put back otherwise; the work files and the blocks of the titles that are not there), the start (a Preparing
// that died: folder, block, THAT pending title, the title, the record last; a run over an Installed child that AM kept goes back to it; the index written
// again; after the review: a run that is undone names the old child and is made again, AM that does not answer settles nothing, a title AM lists but
// cannot describe is deleted, a title that is not ours any more is disowned and never touched, the records past the registry's slots are left alone),
// every branch of Decide (composer, language, Probe, adoption, refusal, a title gone is prepared again, another product code, AM silent, 16 children at
// most), the games that may have a child (every
// sold id but 3DS Link's, never a homebrew of the band, never the Download Play application), the lobbies' key of a prepared id that is no sold
// game's, and the catalog of Titles Delivery without our titles. No key, no game byte: made-up SMDH and ids only.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "azl/dlp_children.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_decide.hpp"
#include "dlp_guest_exclude.hpp"
#include "../titleshare_host/recv_filter.h"
#include "dlp_guest_paths.hpp"
#include "dlp_guest_store.hpp"
#include "games.hpp"
#include "paths.hpp"
#include "public_lobbies.hpp"

namespace fs = std::filesystem;
namespace dc = azl::dlp_children;
using namespace dlpguest;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

static const u32 kSoldCatalog[] = {
#include "sold_catalog.inc"
};

static void Put(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

static std::string Get(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[1024];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

static bool Exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static ino_t Inode(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
}

static void Fresh() {
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/luma", 0777);
    mkdir("sdmc:/luma/plugins", 0777);
}

static const std::string kDir = "sdmc:/3ds/3DSLink/dlp-guests";
static const std::string kRec = kDir + "/" + std::string("0123456789abcdef") + "0123456789abcdef"; // (DPAPP-2j, R-B: this console's records)

static std::string Name16(u64 v) {
    char p[17];
    dc::PutHex16(p, v);
    p[16] = '\0';
    return p;
}

// A sound record: the game, its child (same low word, as Island Tour's) and its prepared title, Installed.
static Guest MakeGuest(u64 game, GuestState state = GuestState::Installed) {
    Guest g;
    g.game = game;
    g.child = (0x00040001ull << 32) | (game & 0xFFFFFFFFull);
    g.prepared = azl::title_rules::PreparedOf(game);
    g.where = 'S';
    g.game_version = 1040;
    g.update_version = 2064;
    g.child_version = 1024;
    for (int i = 0; i < 8; ++i) {
        g.child_hash8[i] = static_cast<u8>(0xA0 + i);
    }
    g.transfer = 0x01EE3A00;
    g.required = 0x01EEA8C0;
    std::strcpy(g.product, "CTR-P-ATSP");
    g.composer = kComposerVersion;
    g.language = 1;
    g.variant = 1;
    g.state = state;
    return g;
}

static bool SameGuest(const Guest& a, const Guest& b) {
    return a.game == b.game && a.child == b.child && a.prepared == b.prepared && a.where == b.where && a.game_version == b.game_version &&
           a.update_version == b.update_version && a.child_version == b.child_version && std::memcmp(a.child_hash8, b.child_hash8, 8) == 0 &&
           a.transfer == b.transfer && a.required == b.required && std::strcmp(a.product, b.product) == 0 && a.composer == b.composer &&
           a.language == b.language && a.variant == b.variant && a.state == b.state && a.prev_installed == b.prev_installed && a.prev_version == b.prev_version &&
           a.form == b.form && a.prev_other == b.prev_other && a.prev_other_version == b.prev_other_version;
}

static std::string Replace(std::string s, const std::string& from, const std::string& to) {
    const std::size_t at = s.find(from);
    if (at != std::string::npos) {
        s.replace(at, from.size(), to);
    }
    return s;
}

static const u64 kIslandEur = 0x00040000000F8200ull; // Mario Party: Island Tour (EUR): its prepared title is 000400000F8F8200
static const u64 kGameB = 0x0004000000086300ull;
static const u64 kGameC = 0x0004000000030700ull;
static const u64 kGameD = 0x00040000000A5F00ull;
static const u64 kGameE = 0x0004000000055D00ull;
static const u64 kGameF = 0x000400000F700000ull; // (the second band of the sold ids: its prepared title is 000400000FF00000)

// ---------------------------------------------------------------------------------------------

static void TestGuestText() {
    Guest g = MakeGuest(kIslandEur, GuestState::Preparing);
    g.prev_installed = true;
    g.prev_version = 1008;
    g.where = 'C';
    CHECK(g.prepared == 0x000400000F8F8200ull);
    const std::string text = GuestText(g);
    CHECK(text.compare(0, 13, "#dlp-guest 1\n") == 0);
    CHECK(text.find("prepared=000400000F8F8200\n") != std::string::npos);
    CHECK(text.find("child_hash8=A0A1A2A3A4A5A6A7\n") != std::string::npos);
    Guest back;
    CHECK(ParseGuest(text, &back) && SameGuest(back, g));
    // CRLF, a comment and a key of a later version are taken
    std::string crlf;
    for (const char c : text) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }
    CHECK(ParseGuest(crlf, &back) && SameGuest(back, g));
    CHECK(ParseGuest(text + "# a note\nlater_key=whatever\n", &back) && SameGuest(back, g));
    // every other record is refused, and the output is left as it was
    Guest untouched = MakeGuest(kGameB);
    const auto refused = [&](const std::string& t) {
        Guest out = untouched;
        return !ParseGuest(t, &out) && SameGuest(out, untouched);
    };
    CHECK(refused(""));
    CHECK(refused(Replace(text, "#dlp-guest 1", "#dlp-guest 2")));
    CHECK(refused("game=00040000000F8200\n" + text));                              // the header is not the first line
    CHECK(refused(Replace(text, "prepared=000400000F8F8200", "prepared=000400000F8F8300"))); // not the game's own prepared title
    CHECK(refused(Replace(text, "game=00040000000F8200", "game=000400000F8F8200")));  // a homebrew of the band: PreparedOf is 0
    CHECK(refused(Replace(Replace(text, "game=00040000000F8200", "game=000400000003D100"), "prepared=000400000F8F8200", "prepared=000400000F83D100"))); // 3DS Link's
    CHECK(refused(Replace(text, "game=00040000000F8200", "game=0004000E000F8200")));  // an update is no game
    CHECK(refused(Replace(text, "child=00040001000F8200", "child=00040000000F8200"))); // a child is 00040001
    CHECK(refused(Replace(text, "child=00040001000F8200", "child=0004000100000000")));
    CHECK(refused(Replace(text, "where=C", "where=X")));
    CHECK(refused(Replace(text, "state=preparing", "state=done")));
    CHECK(refused(Replace(text, "state=preparing\n", "")));
    CHECK(refused(Replace(text, "prev_state=installed", "prev_state=removing")));
    CHECK(refused(Replace(text, "child_version=1024", "child_version=65536")));
    CHECK(refused(Replace(text, "child_version=1024", "child_version=-1")));
    CHECK(refused(Replace(text, "child_hash8=A0A1A2A3A4A5A6A7", "child_hash8=A0A1A2A3A4A5A6A")));
    CHECK(refused(Replace(text, "product=CTR-P-ATSP", "product=CTR:P:ATSP")));
    CHECK(refused(Replace(text, "product=CTR-P-ATSP", "product=CTR-P-ATSP-0123456789")));
    CHECK(refused(Replace(text, "composer=" + std::to_string(kComposerVersion), "composer=256")));
    CHECK(refused(Replace(text, "game=00040000000F8200", "game=00040000000F82")));
    CHECK(refused(Replace(text, "transfer=", "transfer")));
    // the second band of the sold ids has its prepared titles too (the user's formula, no bound)
    const Guest f = MakeGuest(kGameF);
    CHECK(f.prepared == 0x000400000FF00000ull && ParseGuest(GuestText(f), &back) && SameGuest(back, f));
}

static void TestFolder() {
    Fresh();
    CHECK(LoadGuests().empty());
    CHECK(!Exists(kDir) && !Exists(kRec));
    const Guest a = MakeGuest(kIslandEur);
    CHECK(SaveGuest(a));
    CHECK(Exists(kRec + "/000400000F8F8200.guest"));
    CHECK(GuestPath(a.prepared) == kRec + "/000400000F8F8200.guest");
    CHECK(PartPath(a.prepared) == kRec + "/000400000F8F8200.part");
    std::vector<Guest> got = LoadGuests();
    CHECK(got.size() == 1 && SameGuest(got[0], a));
    // what is not a record of its own name is not read
    Put(kRec + "/readme.txt", "hello");
    Put(kRec + "/0123.guest", GuestText(MakeGuest(kGameB)));
    Put(kRec + "/" + Name16(MakeGuest(kGameB).prepared) + ".guest", GuestText(MakeGuest(kGameC)));      // a record under another title's name
    std::string lower = Name16(MakeGuest(kGameD).prepared) + ".guest";
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    Put(kRec + "/" + lower, GuestText(MakeGuest(kGameD)));                                               // a name the app never writes
    Put(kRec + "/" + Name16(MakeGuest(kGameE).prepared) + ".guest", "#dlp-guest 1\ngame=00040000\n");   // broken
    got = LoadGuests();
    CHECK(got.size() == 1 && got[0].prepared == a.prepared);
    // a write cut between its two renames leaves the old record as .bak alone: it is the record
    const Guest c = MakeGuest(kGameC);
    CHECK(SaveGuest(c));
    CHECK(std::rename(GuestPath(c.prepared).c_str(), (GuestPath(c.prepared) + ".bak").c_str()) == 0);
    got = LoadGuests();
    CHECK(got.size() == 2 && got[0].prepared == c.prepared && SameGuest(got[0], c) && got[1].prepared == a.prepared);
    CHECK(Exists(GuestPath(c.prepared)));
    // a record that is not sound is never written
    Guest bad = MakeGuest(kGameD);
    bad.prepared += 0x100;
    CHECK(!SaveGuest(bad) && !Exists(GuestPath(bad.prepared)));
    // dropped whole: the record, its .bak, its .tmp
    Put(GuestPath(a.prepared) + ".bak", "x");
    Put(GuestPath(a.prepared) + ".tmp", "x");
    CHECK(DropGuest(a.prepared));
    CHECK(!Exists(GuestPath(a.prepared)) && !Exists(GuestPath(a.prepared) + ".bak") && !Exists(GuestPath(a.prepared) + ".tmp"));
    CHECK(Exists(kRec + "/readme.txt"));
}

static void TestRegistry() {
    ClearRegistry();
    const u64 p = azl::title_rules::PreparedOf(kIslandEur);
    CHECK(!IsOwnedGuestTid(p) && !IsOwnedGuestTid(0));
    Register(p, kFormApplication);
    CHECK(IsOwnedGuestTid(p));
    Register(p, kFormApplication); // (once)
    Unregister(p);
    CHECK(!IsOwnedGuestTid(p));
    // only a title of the band, never 3DS Link, never a game
    CHECK(!Register(kIslandEur, kFormApplication) && !Register(azl::title_rules::kAppTid, kFormApplication) && !Register(0, kFormApplication));
    CHECK(!IsOwnedGuestTid(kIslandEur) && !IsOwnedGuestTid(azl::title_rules::kAppTid));
    // the slots: kRegistrySlots titles, no more, and a title past them is said so (it is not ours to this run)
    int registered = 0;
    for (u64 i = 0; i < kRegistrySlots + 4; ++i) {
        registered += Register(0x000400000F900000ull + (i << 8), kFormApplication) ? 1 : 0;
    }
    int owned = 0;
    for (u64 i = 0; i < kRegistrySlots + 4; ++i) {
        owned += IsOwnedGuestTid(0x000400000F900000ull + (i << 8)) ? 1 : 0;
    }
    CHECK(owned == static_cast<int>(kRegistrySlots));
    CHECK(registered == static_cast<int>(kRegistrySlots));
    CHECK(Register(0x000400000F900000ull, kFormApplication)); // (one that is there already: ours)
    // LoadRegistry: exactly the records of the folder
    Fresh();
    CHECK(SaveGuest(MakeGuest(kIslandEur)) && SaveGuest(MakeGuest(kGameB, GuestState::Preparing)) && SaveGuest(MakeGuest(kGameC, GuestState::Removing)));
    const std::vector<Guest> guests = LoadRegistry();
    CHECK(guests.size() == 3);
    CHECK(IsOwnedGuestTid(p) && IsOwnedGuestTid(MakeGuest(kGameB).prepared) && IsOwnedGuestTid(MakeGuest(kGameC).prepared));
    CHECK(!IsOwnedGuestTid(0x000400000F900000ull) && !IsOwnedGuestTid(MakeGuest(kGameD).prepared));
    CHECK(!IsOwnedGuestTid(kIslandEur) && !IsOwnedGuestTid(MakeGuest(kIslandEur).child)); // the game and the child are never "ours"
    // more records than slots (a card copied from another console): the ones past the slots are counted, and are not ours
    Fresh();
    const std::size_t many = kRegistrySlots + 3;
    for (std::size_t i = 0; i < many; ++i) {
        CHECK(SaveGuest(MakeGuest(0x0004000000100000ull + (static_cast<u64>(i) << 8))));
    }
    int unowned = -1;
    const std::vector<Guest> all = LoadRegistry(&unowned);
    int ours = 0;
    for (const Guest& g : all) {
        ours += IsOwnedGuestTid(g.prepared) ? 1 : 0;
    }
    CHECK(all.size() == many && unowned == 3 && ours == static_cast<int>(kRegistrySlots));
}

static void TestCache() {
    Fresh();
    std::vector<ChildSeen> seen(3);
    seen[0].game = kIslandEur;
    seen[0].game_version = 1040;
    seen[0].update_version = 2064;
    seen[0].has_child = true;
    seen[0].child = 0x00040001000F8200ull;
    seen[0].child_version = 1024;
    seen[0].child_hash8[7] = 0x7F;
    seen[0].transfer = 7;
    seen[0].required = 9;
    std::strcpy(seen[0].product, "CTR-P-ATSP");
    seen[1].game = kGameB;
    seen[1].game_version = 0;
    seen[1].update_version = 0;
    seen[2] = seen[0];
    seen[2].game_version = 1056; // (the same game at another version: a line of its own)
    CHECK(SaveChildrenCache(seen));
    CHECK(CachePath() == kDir + "/children.cache");
    std::vector<ChildSeen> back = LoadChildrenCache();
    CHECK(back.size() == 3 && back[0].has_child && back[0].child == seen[0].child && back[0].child_hash8[7] == 0x7F && back[0].transfer == 7 &&
          back[0].required == 9 && std::strcmp(back[0].product, "CTR-P-ATSP") == 0 && !back[1].has_child && back[2].game_version == 1056);
    CHECK(FindChildSeen(back, kIslandEur, 1040, 2064) == &back[0] && FindChildSeen(back, kIslandEur, 1056, 2064) == &back[2]);
    CHECK(FindChildSeen(back, kIslandEur, 1040, 2080) == nullptr && FindChildSeen(back, kGameC, 0, 0) == nullptr);
    // a broken line is skipped, another header is no cache
    std::vector<ChildSeen> out;
    CHECK(ParseCache("#dlp-children-cache 1\n00040000000F8200:1:2:none\nbroken\n000400000F8F8200:0:0:none\n0004000000086300:0:0:00040000000F8200:1:00:0:0:X\n", &out) &&
          out.size() == 1 && out[0].game == kIslandEur);
    CHECK(!ParseCache("#dlp-children-cache 2\n", &out));
    Put(CachePath(), "garbage");
    CHECK(LoadChildrenCache().empty());
    // (1.1.4, spec 9 point 5) a look that failed is remembered with its versions, its reason and boot9.bin's stamp: never as a fact (FindChildSeen),
    // only as a failure (FindChildFailure); a line of a failure an older app reads is skipped as broken, one of an older app is read as before
    std::vector<ChildSeen> mixed(2);
    mixed[0] = seen[0];
    mixed[1].game = kGameB;
    mixed[1].game_version = 3;
    mixed[1].update_version = 4;
    mixed[1].error = 309 /* kErrNcch */;
    mixed[1].keys = 0xDEADBEEFu;
    CHECK(SaveChildrenCache(mixed));
    back = LoadChildrenCache();
    CHECK(back.size() == 2 && back[1].error == 309 /* kErrNcch */ && back[1].keys == 0xDEADBEEFu && !back[1].has_child && back[0].error == 0);
    CHECK(FindChildSeen(back, kGameB, 3, 4) == nullptr && FindChildFailure(back, kGameB, 3, 4) == &back[1]);
    CHECK(FindChildFailure(back, kGameB, 3, 5) == nullptr && FindChildFailure(back, kIslandEur, 1040, 2064) == nullptr);
    CHECK(CacheText(mixed).find(":3:4:failed:309:3735928559\n") != std::string::npos);
    CHECK(ParseCache("#dlp-children-cache 1\n00040000000F8200:1:2:failed:0:7\n00040000000F8200:1:2:failed:x:7\n00040000000F8200:1:2:lost:309:7\n", &out) &&
          out.empty()); // (no reason, a reason that is not a number, another word: broken)
    CHECK(ParseCache("#dlp-children-cache 1\n00040000000F8200:1:2:failed:302:0\n00040000000F8200:1:2:none\n", &out) && out.size() == 1 &&
          out[0].error == 302 /* kErrNoSource */); // (one line per game and versions: the first stays)
}

static void TestIndex() {
    CHECK(std::string("sdmc:") + dc::kIndexPath == IndexPath());
    std::vector<Guest> guests = {MakeGuest(kIslandEur), MakeGuest(kGameB, GuestState::Preparing), MakeGuest(kGameC), MakeGuest(kGameD, GuestState::Removing)};
    guests[2].where = 'C';
    std::vector<Waiting> waiting(2);
    waiting[0].game = kGameE;
    waiting[0].child = 0x0004000100055D00ull;
    waiting[0].child_version = 3;
    waiting[1].game = kIslandEur; // (its child has a row already: not twice)
    waiting[1].child = MakeGuest(kIslandEur).child;
    int dropped = -1;
    const std::string text = IndexText(guests, waiting, &dropped);
    CHECK(dropped == 0);
    CHECK(text.compare(0, 16, "#dlp-children 1\n") == 0);
    CHECK(text.find("00040001000F8200:00040000000F8200:000400000F8F8200:S:1024\n") != std::string::npos);
    dc::Index idx;
    CHECK(dc::ParseIndex(text.data(), text.size(), &idx) && idx.count == 3 && idx.skipped == 0);
    CHECK(idx.rows[0].prepared == guests[0].prepared && idx.rows[1].prepared == guests[2].prepared && idx.rows[1].where == 'C');
    CHECK(idx.rows[2].game == kGameE && idx.rows[2].prepared == 0 && idx.rows[2].version == 3);
    CHECK(dc::FindPrepared(idx, MakeGuest(kGameB).prepared) < 0 && dc::FindPrepared(idx, MakeGuest(kGameD).prepared) < 0); // Preparing and Removing: no row
    CHECK(text.size() <= dc::kIndexMaxBytes);
    // at most kMaxRows rows: the rest is counted
    std::vector<Guest> many;
    for (u32 i = 0; i < 20; ++i) {
        many.push_back(MakeGuest(0x0004000000040000ull + (static_cast<u64>(i) << 8)));
    }
    const std::string full = IndexText(many, {}, &dropped);
    CHECK(dropped == 4 && dc::ParseIndex(full.data(), full.size(), &idx) && idx.count == dc::kMaxRows && idx.skipped == 0 && full.size() <= dc::kIndexMaxBytes);
    // written: always at the start (a new file), otherwise only when it differs
    Fresh();
    CHECK(WriteIndex(text, false) && Get(IndexPath()) == text);
    const ino_t first = Inode(IndexPath());
    CHECK(WriteIndex(text, false) && Inode(IndexPath()) == first);
    CHECK(WriteIndex(text, true) && Inode(IndexPath()) != first && Get(IndexPath()) == text);
    CHECK(WriteIndex(full, false) && Get(IndexPath()) == full && !Exists(IndexPath() + ".bak") && !Exists(IndexPath() + ".tmp"));
}

static std::vector<u8> MakeSmdh() {
    std::vector<u8> s(azl::dlp_real::kSmdhBytes, 0);
    std::memcpy(s.data(), "SMDH", 4);
    const char* name = "Island";
    for (unsigned e = 0; e < azl::dlp_real::kSmdhTitles; ++e) {
        u8* t = s.data() + azl::dlp_real::kSmdhTitlesAt + e * azl::dlp_real::kSmdhTitleBytes;
        for (unsigned i = 0; name[i] != '\0'; ++i) {
            t[2 * i] = static_cast<u8>(name[i]);
            t[azl::dlp_real::kTitleShortBytes + 2 * i] = static_cast<u8>(name[i] + 1);
        }
    }
    for (std::size_t i = 0; i < azl::dlp_real::kTitleIconBytes; ++i) {
        s[azl::dlp_real::kSmdhBigIconAt + i] = static_cast<u8>(i * 7);
    }
    return s;
}

static void TestBlock() {
    Fresh();
    const u64 p = azl::title_rules::PreparedOf(kIslandEur);
    const std::vector<u8> smdh = MakeSmdh();
    CHECK(WriteBlock(p, smdh.data(), smdh.size(), 1, 0x01EE3A00, 0x01EEA8C0));
    CHECK(BlockPath(p) == kDir + "/000400000F8F8200.bin");
    const std::string b = Get(BlockPath(p));
    CHECK(b.size() == dc::kBlockBytes);
    if (b.size() == dc::kBlockBytes) {
        const u8* u = reinterpret_cast<const u8*>(b.data());
        CHECK(u[dc::kBlockShortAt] == 'I' && u[dc::kBlockShortAt + 1] == 0 && u[dc::kBlockLongAt] == 'I' + 1);
        CHECK(std::memcmp(u + dc::kBlockIconAt, smdh.data() + azl::dlp_real::kSmdhBigIconAt, azl::dlp_real::kTitleIconBytes) == 0);
        CHECK(dc::BlockTransfer(u) == 0x01EE3A00 && dc::BlockRequired(u) == 0x01EEA8C0);
        CHECK(u[dc::kBlockTransferAt] == 0x00 && u[dc::kBlockTransferAt + 1] == 0x3A && u[dc::kBlockTransferAt + 3] == 0x01); // little-endian
    }
    std::vector<u8> no = smdh;
    no[0] = 'X';
    const u64 q = azl::title_rules::PreparedOf(kGameB);
    CHECK(!WriteBlock(q, no.data(), no.size(), 1, 1, 2) && !Exists(BlockPath(q)));
    CHECK(!WriteBlock(q, smdh.data(), smdh.size() - 1, 1, 1, 2) && !Exists(BlockPath(q)));
    CHECK(!WriteBlock(kGameB, smdh.data(), smdh.size(), 1, 1, 2) && !Exists(BlockPath(kGameB))); // a game's id is no prepared title
}

static void TestSweep() {
    Fresh();
    const Guest inst = MakeGuest(kIslandEur);                       // Installed
    const Guest prep = MakeGuest(kGameB, GuestState::Preparing);    // Preparing, nothing before
    Guest over = MakeGuest(kGameC, GuestState::Preparing);          // Preparing over an Installed child
    over.prev_installed = true;
    const std::vector<Guest> alive = {inst, prep, over};
    setup::MakeDirs(kDir);
    for (const Guest& g : alive) {
        CHECK(SaveGuest(g));
    }
    const u64 none = azl::title_rules::PreparedOf(kGameD);
    Put(PartPath(inst.prepared), "p");
    Put(PartPath(prep.prepared), "p");
    Put(PartPath(none), "p");
    Put(BlockPath(inst.prepared), "b");
    Put(BlockPath(prep.prepared), "b");
    Put(BlockPath(over.prepared), "b");
    Put(BlockPath(none), "b");
    Put(kDir + "/x.tmp", "t");
    Put(IndexPath() + ".tmp", "t");
    Put(IndexPath(), "#dlp-children 1\n");
    Put(IndexPath() + ".bak", "old");
    Put(CachePath() + ".bak", "#dlp-children-cache 1\n");          // alone: it IS the cache
    Put(kDir + "/readme.txt", "r");
    Put(kDir + "/foo.bin", "f");
    Put(kDir + "/foo.part", "f");
    std::string lower = Name16(none) + ".bin";
    lower[9] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[9])));
    Put(kDir + "/" + lower, "l");
    const Sweep s = SweepOrphans(alive);
    CHECK(s.temps == 2 && s.baks == 1 && s.restored == 1 && s.parts == 2 && s.blocks == 2);
    CHECK(!Exists(PartPath(inst.prepared)) && Exists(PartPath(prep.prepared)) && !Exists(PartPath(none)));
    CHECK(Exists(BlockPath(inst.prepared)) && !Exists(BlockPath(prep.prepared)) && Exists(BlockPath(over.prepared)) && !Exists(BlockPath(none)));
    CHECK(!Exists(kDir + "/x.tmp") && !Exists(IndexPath() + ".tmp") && !Exists(IndexPath() + ".bak") && Get(IndexPath()) == "#dlp-children 1\n");
    CHECK(Get(CachePath()) == "#dlp-children-cache 1\n" && !Exists(CachePath() + ".bak"));
    CHECK(Exists(kDir + "/readme.txt") && Exists(kDir + "/foo.bin") && Exists(kDir + "/foo.part") && Exists(kDir + "/" + lower));
    for (const Guest& g : alive) {
        CHECK(Exists(GuestPath(g.prepared))); // the records are never the sweep's
    }
    CHECK(SweepOrphans({}).blocks == 2); // (nobody alive: the two blocks left go)
}

static Seen GoodSeen(const Guest* g) {
    Seen s;
    s.game = kIslandEur;
    s.on = true;
    s.present = true;
    s.game_version = 1040;
    s.update_version = 2064;
    s.child = ChildFact::Has;
    s.child_tid = MakeGuest(kIslandEur).child;
    s.child_version = 1024;
    std::strcpy(s.product, "CTR-P-ATSP");
    s.guest = g;
    s.prepared_installed = true;
    s.prepared_version = 1024;
    std::strcpy(s.prepared_product, "CTR-P-ATSP");
    s.plugin_on = true;
    s.block_ok = true;
    s.language = 1;
    s.keys = true;
    return s;
}

static void TestDecide() {
    Guest g = MakeGuest(kIslandEur);
    Seen s = GoodSeen(&g);
    CHECK(Decide(s) == Act::Nothing);
    // switched off, gone, a card that is out
    s.on = false;
    CHECK(Decide(s) == Act::Remove);
    s.keys = false;
    CHECK(Decide(s) == Act::Remove); // (a removal needs no key)
    s = GoodSeen(&g);
    s.present = false;
    CHECK(Decide(s) == Act::Remove);
    g.where = 'C';
    CHECK(Decide(s) == Act::Nothing);
    g = MakeGuest(kIslandEur);
    // a game of the SD card the scan did not recognise, which AM still lists (or whose list could not be read): not gone, its child stays, with or
    // without boot9.bin; switched off, it goes all the same
    s = GoodSeen(&g);
    s.present = false;
    s.still_listed = true;
    CHECK(Decide(s) == Act::Nothing);
    s.keys = false;
    CHECK(Decide(s) == Act::Nothing); // (the review of APP-WORKER: the guest of a game the scan missed was removed with no question)
    s.on = false;
    CHECK(Decide(s) == Act::Remove);
    // the states of the record
    g.state = GuestState::Removing;
    s = GoodSeen(&g);
    CHECK(Decide(s) == Act::Remove);
    g.state = GuestState::Preparing;
    CHECK(Decide(s) == Act::Nothing);
    g = MakeGuest(kIslandEur);
    // no boot9.bin: what is there stays, nothing is made
    s = GoodSeen(&g);
    s.keys = false;
    s.prepared_installed = false;
    CHECK(Decide(s) == Act::Nothing);
    // the title of ours gone (the Installation check took it, or the user): prepared again, not an error; another version of it: the same
    s = GoodSeen(&g);
    s.prepared_installed = false;
    CHECK(Decide(s) == Act::Prepare);
    s = GoodSeen(&g);
    s.prepared_version = 1008;
    CHECK(Decide(s) == Act::Prepare);
    // an older composer
    g.composer = static_cast<u8>(kComposerVersion - 1);
    s = GoodSeen(&g);
    CHECK(Decide(s) == Act::Prepare);
    g = MakeGuest(kIslandEur);
    // another version of the game or of its update, a card not read again this session
    s = GoodSeen(&g);
    s.game_version = 1056;
    CHECK(Decide(s) == Act::Check);
    s = GoodSeen(&g);
    s.update_version = 2080;
    CHECK(Decide(s) == Act::Check);
    g.where = 'C';
    s = GoodSeen(&g);
    s.card = true;
    CHECK(Decide(s) == Act::Check);
    s.card_checked = true;
    CHECK(Decide(s) == Act::Nothing);
    // the game moved between the SD card and the card, the plugin's folder, the block, the language
    s.card = false;
    CHECK(Decide(s) == Act::Repair);
    g = MakeGuest(kIslandEur);
    s = GoodSeen(&g);
    s.plugin_on = false;
    CHECK(Decide(s) == Act::Repair);
    s = GoodSeen(&g);
    s.block_ok = false;
    CHECK(Decide(s) == Act::Repair);
    s = GoodSeen(&g);
    s.language = 2;
    CHECK(Decide(s) == Act::Repair);
    // no record
    s = GoodSeen(nullptr);
    s.prepared_installed = false;
    CHECK(Decide(s) == Act::Prepare);
    s.on = false;
    CHECK(Decide(s) == Act::Nothing);
    s.on = true;
    s.present = false;
    CHECK(Decide(s) == Act::Nothing);
    s.present = true;
    s.keys = false;
    CHECK(Decide(s) == Act::Nothing);
    s.keys = true;
    s.child = ChildFact::Unknown;
    CHECK(Decide(s) == Act::Probe); // a game never looked at
    s.child = ChildFact::None;
    CHECK(Decide(s) == Act::Nothing);
    // the id taken: our child without its record (same product code, same version) is adopted; anything else is refused, never deleted
    s = GoodSeen(nullptr);
    CHECK(Decide(s) == Act::Adopt);
    s.prepared_version = 1008;
    CHECK(Decide(s) == Act::Refuse);
    s = GoodSeen(nullptr);
    std::strcpy(s.prepared_product, "CTR-N-HBRW");
    CHECK(Decide(s) == Act::Refuse);
    s = GoodSeen(nullptr);
    s.product[0] = '\0';
    s.prepared_product[0] = '\0';
    CHECK(Decide(s) == Act::Refuse);
    // AM did not answer about the prepared id: nothing, whatever the record says
    for (const GuestState state : {GuestState::Installed, GuestState::Removing}) {
        g = MakeGuest(kIslandEur, state);
        s = GoodSeen(&g);
        s.am_known = false;
        s.on = false;
        CHECK(Decide(s) == Act::Nothing);
    }
    s = GoodSeen(nullptr);
    s.prepared_installed = false;
    s.am_known = false;
    CHECK(Decide(s) == Act::Nothing);
    // a record whose id now holds a title that is not ours (another product code): disowned, never installed over, never deleted
    g = MakeGuest(kIslandEur);
    s = GoodSeen(&g);
    std::strcpy(s.prepared_product, "CTR-N-HBRW");
    CHECK(Decide(s) == Act::Disown);
    s.prepared_version = 1;
    CHECK(Decide(s) == Act::Disown); // (not Prepare)
    s.on = false;
    CHECK(Decide(s) == Act::Disown); // (not Remove)
    s = GoodSeen(&g);
    s.present = false;
    std::strcpy(s.prepared_product, "CTR-N-HBRW");
    CHECK(Decide(s) == Act::Disown);
    g.state = GuestState::Removing;
    CHECK(Decide(s) == Act::Disown);
    g = MakeGuest(kIslandEur);
    // ... but a code that could not be read (AM's, or the record's) proves nothing: the record stays the proof
    s = GoodSeen(&g);
    s.prepared_product[0] = '\0';
    s.prepared_version = 1008;
    CHECK(Decide(s) == Act::Prepare);
    s.on = false;
    CHECK(Decide(s) == Act::Remove);
    g.product[0] = '\0';
    s = GoodSeen(&g);
    std::strcpy(s.prepared_product, "CTR-N-HBRW");
    s.on = false;
    CHECK(Decide(s) == Act::Remove);
    // AM gives 15 characters of a product code of 16: the same title
    g = MakeGuest(kIslandEur);
    std::strcpy(g.product, "CTR-P-ABCDEFGHIJ");
    s = GoodSeen(&g);
    std::strcpy(s.prepared_product, "CTR-P-ABCDEFGHI");
    CHECK(Decide(s) == Act::Nothing);
    s = GoodSeen(nullptr);
    std::strcpy(s.product, "CTR-P-ABCDEFGHIJ");
    std::strcpy(s.prepared_product, "CTR-P-ABCDEFGHI");
    CHECK(Decide(s) == Act::Adopt); // (our child, adopted: not refused for the character AM does not give)
    g = MakeGuest(kIslandEur);
    // kMaxGuests records at most: a game past them gets none (nor adopts one); a game that has its record keeps its child
    s = GoodSeen(nullptr);
    s.prepared_installed = false;
    s.guests = kMaxGuests - 1;
    CHECK(Decide(s) == Act::Prepare);
    s.guests = kMaxGuests;
    CHECK(Decide(s) == Act::Full);
    s = GoodSeen(nullptr);
    s.guests = kMaxGuests;
    CHECK(Decide(s) == Act::Full);
    s = GoodSeen(&g);
    s.guests = kMaxGuests;
    s.prepared_installed = false;
    CHECK(Decide(s) == Act::Prepare);
    // the start
    Guest st = MakeGuest(kIslandEur);
    const char* ours = "CTR-P-ATSP";
    CHECK(DecideAtStart(st, Presence::Present, 1024, ours) == StartAct::Keep && DecideAtStart(st, Presence::Absent, 0, "") == StartAct::Keep &&
          DecideAtStart(st, Presence::Unknown, 0, "") == StartAct::Keep);
    st.state = GuestState::Removing;
    CHECK(DecideAtStart(st, Presence::Present, 1024, ours) == StartAct::TakeAway);
    st.state = GuestState::Preparing;
    CHECK(DecideAtStart(st, Presence::Present, 1024, ours) == StartAct::TakeAway && DecideAtStart(st, Presence::Absent, 0, "") == StartAct::TakeAway);
    st.prev_installed = true;
    st.prev_version = 1008;
    CHECK(DecideAtStart(st, Presence::Present, 1008, ours) == StartAct::Revert);
    CHECK(DecideAtStart(st, Presence::Present, 1008, "") == StartAct::Revert); // (a code not read proves nothing)
    CHECK(DecideAtStart(st, Presence::Present, 1024, ours) == StartAct::TakeAway && DecideAtStart(st, Presence::Absent, 1008, "") == StartAct::TakeAway);
    CHECK(DecideAtStart(st, Presence::Broken, 1008, "") == StartAct::TakeAway); // (listed, not described: no going back to it)
    // AM did not answer: the record is left as it is, its title never taken for gone
    CHECK(DecideAtStart(st, Presence::Unknown, 0, "") == StartAct::Later);
    st.state = GuestState::Removing;
    CHECK(DecideAtStart(st, Presence::Unknown, 0, "") == StartAct::Later);
    // the id holds a title that is not ours any more: disowned, never deleted (a removal, a run, even a run whose old version AM "kept")
    CHECK(DecideAtStart(st, Presence::Present, 1024, "CTR-N-HBRW") == StartAct::Disown && DecideAtStart(st, Presence::Broken, 0, "CTR-N-HBRW") == StartAct::Disown);
    st.state = GuestState::Preparing;
    CHECK(DecideAtStart(st, Presence::Present, 1008, "CTR-N-HBRW") == StartAct::Disown);
    CHECK(DecideAtStart(st, Presence::Absent, 0, "CTR-N-HBRW") == StartAct::TakeAway); // (nothing there: nobody's title to spare)
    // (the final review of 1.1.4) a first run cut before its record had a product code: a title AM lists at the id is never deleted - a homebrew
    // of the same unique id installed meanwhile, or ours, which the next look adopts; nothing listed, or a run over our old child: as before
    Guest first = MakeGuest(kIslandEur, GuestState::Preparing);
    first.product[0] = '\0';
    CHECK(DecideAtStart(first, Presence::Present, 1024, "CTR-N-HBRW") == StartAct::Disown);
    CHECK(DecideAtStart(first, Presence::Present, 1024, ours) == StartAct::Disown);
    CHECK(DecideAtStart(first, Presence::Present, 1024, "") == StartAct::Disown);
    CHECK(DecideAtStart(first, Presence::Broken, 0, "") == StartAct::Disown); // (listed, not described: still nobody's to delete)
    CHECK(DecideAtStart(first, Presence::Absent, 0, "") == StartAct::TakeAway);
    CHECK(DecideAtStart(first, Presence::Unknown, 0, "") == StartAct::Later);
    first.prev_installed = true;
    first.prev_version = 1008;
    CHECK(DecideAtStart(first, Presence::Present, 1008, "") == StartAct::Revert);
}

static void TestGames() {
    std::set<u32> sold_keys;
    const std::size_t count = sizeof(kSoldCatalog) / sizeof(kSoldCatalog[0]);
    for (std::size_t i = 0; i < count; ++i) {
        sold_keys.insert(lobbies::TitleKey(0x0004000000000000ull | kSoldCatalog[i]));
    }
    int games = 0, without = 0, key_clash = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const u64 tid = 0x0004000000000000ull | kSoldCatalog[i];
        const bool guest = setup::IsGuestGame(tid, "");
        games += guest ? 1 : 0;
        without += guest ? 0 : 1;
        CHECK(guest == (PreparedFor(tid) != 0));
        if (guest) {
            // a room's member that declares a prepared id plays no game of the lobbies' list
            key_clash += sold_keys.count(lobbies::TitleKey(PreparedFor(tid))) != 0 ? 1 : 0;
            CHECK(!setup::IsSoldGameTid(PreparedFor(tid)) && !setup::IsGuestGame(PreparedFor(tid), ""));
        }
    }
    CHECK(games + without == static_cast<int>(count) && without <= 1); // (3DS Link's own id, 0x0003D100, would be 3DS Link: no child)
    CHECK(!setup::IsGuestGame(0x000400000003D100ull, ""));
    CHECK(key_clash == 0);
    // never a homebrew of the band, 3DS Link, the Download Play application, an impostor, an update or a child
    for (u64 low = 0x0F800000ull; low <= 0x0FFFFF00ull; low += 0x00012300ull) {
        CHECK(!setup::IsGuestGame(0x0004000000000000ull | low, ""));
        CHECK(PreparedFor(0x0004000000000000ull | low) != (0x0004000000000000ull | low));
    }
    CHECK(!setup::IsGuestGame(0x000400000F800100ull, "FBI") && !setup::IsGuestGame(azl::title_rules::kAppTid, "3DS Link"));
    for (const u64 dlp : setup::kDownloadPlayTids) {
        CHECK(!setup::IsGuestGame(dlp, "Download Play"));
    }
    CHECK(!setup::IsGuestGame(0x00040000000F9800ull, "Fast PlayCoin - 300 coins NOW") && !setup::IsGuestGame(0x0004000E000F8200ull, "") &&
          !setup::IsGuestGame(0x00040001000F8200ull, ""));
    CHECK(setup::IsGuestGame(kIslandEur, "Mario Party: Island Tour") && setup::IsGuestGame(kGameF, ""));
}

struct FakeGroup {
    char name[48];
    int members[48];
    int nMembers;
};

static void TestExclude() {
    ClearRegistry();
    const u64 ours = azl::title_rules::PreparedOf(kIslandEur);
    Register(ours, kFormApplication);
    // items: a game, its update, our prepared title, another game, a homebrew of the band that is NOT ours
    const u64 items[] = {kIslandEur, 0x0004000E000F8200ull, ours, kGameB, 0x000400000F800100ull};
    FakeGroup games[4] = {};
    games[0].members[0] = 0;
    games[0].members[1] = 1;
    games[0].nMembers = 2;
    std::strcpy(games[0].name, "Island");
    games[1].members[0] = 2;
    games[1].nMembers = 1;
    std::strcpy(games[1].name, "ours");
    games[2].members[0] = 3;
    games[2].members[1] = 2; // (a group that would hold ours and a game: the game stays)
    games[2].nMembers = 2;
    std::strcpy(games[2].name, "B");
    games[3].members[0] = 4;
    games[3].nMembers = 1;
    std::strcpy(games[3].name, "FBI");
    int n = 4;
    const int dropped = DropOwnedMembers(games, &n, [&items](int i) { return i >= 0 && i < 5 ? items[i] : 0; }, [](u64 tid) { return IsOwnedGuestTid(tid); });
    CHECK(dropped == 2 && n == 3);
    CHECK(std::strcmp(games[0].name, "Island") == 0 && games[0].nMembers == 2 && games[0].members[1] == 1);
    CHECK(std::strcmp(games[1].name, "B") == 0 && games[1].nMembers == 1 && games[1].members[0] == 3);
    CHECK(std::strcmp(games[2].name, "FBI") == 0 && games[2].nMembers == 1 && games[2].members[0] == 4);
    ClearRegistry();
    int m = 3;
    CHECK(DropOwnedMembers(games, &m, [&items](int i) { return items[i]; }, [](u64 tid) { return IsOwnedGuestTid(tid); }) == 0 && m == 3);
}

static void TestGuestFolderGuard() {
    Fresh();
    const std::string plugin = "3GX$ a plugin";
    const u64 p = azl::title_rules::PreparedOf(kIslandEur);
    for (const u64 tid : std::vector<u64>{kIslandEur, p, 0x000400000F800100ull, p + 0x100}) {
        mkdir(setup::GamePluginDir(tid).c_str(), 0777);
        Put(setup::GamePluginPath(tid), plugin);
    }
    CHECK(!setup::DisableGuestFolder(kIslandEur, kIslandEur));          // a game's own folder
    CHECK(!setup::DisableGuestFolder(0x000400000F800100ull, 0x000400000F800100ull)); // a homebrew of the band
    CHECK(!setup::DisableGuestFolder(p + 0x100, kIslandEur));           // another title of the band
    CHECK(!setup::DisableGuestFolder(p, 0x0004000E000F8200ull));         // an update is no game
    CHECK(!setup::DisableGuestFolder(0, 0));
    CHECK(Exists(setup::GamePluginPath(kIslandEur)) && Exists(setup::GamePluginPath(p)) && Exists(setup::GamePluginPath(0x000400000F800100ull)) &&
          Exists(setup::GamePluginPath(p + 0x100)));
    CHECK(setup::DisableGuestFolder(p, kIslandEur));
    CHECK(!Exists(setup::GamePluginPath(p)) && !Exists(setup::GamePluginDir(p)) && Exists(setup::GamePluginPath(kIslandEur)));
}

// The console of the start: AM's titles in a map, every call written down; the folders are the real ones (setup::DisableGuestFolder).
static bool Has(const std::vector<u64>& v, u64 x) {
    for (const u64 y : v) {
        if (y == x) {
            return true;
        }
    }
    return false;
}

struct FakeConsole final : StartConsole {
    std::vector<std::pair<u64, u16>> titles;              // listed and described (product code "CTR-P-ATSP", the records')
    std::vector<u64> broken;                              // listed, not described (product code unread)
    std::vector<std::pair<u64, std::string>> products;    // another product code for a title of the lists
    bool unknown = false;                                 // AM cannot list the titles
    std::vector<u64> pending, deleted, disabled, disabled_system, refuse_folder, refuse_delete;
    int record_missing_at_disable = 0;
    Presence Installed(u64 prepared_id, u8 form, u16* version) override {
        const u64 prepared = AmTitleFor(prepared_id, form); // (the build 2f: the fake lists the ids of AM)
        if (unknown) {
            return Presence::Unknown;
        }
        for (const u64 b : broken) {
            if (b == prepared) {
                return Presence::Broken;
            }
        }
        for (const auto& t : titles) {
            if (t.first == prepared) {
                *version = t.second;
                return Presence::Present;
            }
        }
        return Presence::Absent;
    }
    bool Product(u64 prepared_id, u8 form, char* out) override {
        const u64 prepared = AmTitleFor(prepared_id, form);
        std::memset(out, 0, kProductBytes);
        for (const auto& p : products) {
            if (p.first == prepared) {
                std::strncpy(out, p.second.c_str(), kProductBytes - 1);
                return out[0] != '\0';
            }
        }
        for (const auto& t : titles) {
            if (t.first == prepared) {
                std::strcpy(out, "CTR-P-ATSP");
                return true;
            }
        }
        return false;
    }
    void DeletePending(u64 prepared_id, u8 form) override {
        const u64 prepared = AmTitleFor(prepared_id, form);
        pending.push_back(prepared);
    }
    std::vector<u64> tickets_dropped; // (the build 2h) the ids whose ticket the start asked to delete (R7)
    std::vector<u64> tickets_at;      // (DPAPP-2h-sec, second pass) the ids the console holds a ticket at (TicketAt)
    bool tickets_unread = false;      // ... its lists not read
    std::vector<u64> delete_keeps_ticket; // ... a delete that takes the title and says false: its ticket not known gone (DeletePrepared's kTicketLeft)
    bool DeleteTitle(u64 prepared_id, u8 form, bool drop_ticket) override {
        const u64 prepared = AmTitleFor(prepared_id, form);
        deleted.push_back(prepared);
        if (drop_ticket) {
            tickets_dropped.push_back(prepared);
        }
        if (Has(refuse_delete, prepared)) {
            return false;
        }
        const bool keeps = Has(delete_keeps_ticket, prepared);
        if (drop_ticket && !keeps) {
            tickets_at.erase(std::remove(tickets_at.begin(), tickets_at.end(), prepared), tickets_at.end());
        }
        for (auto it = broken.begin(); it != broken.end(); ++it) {
            if (*it == prepared) {
                broken.erase(it);
                return !keeps;
            }
        }
        for (auto it = titles.begin(); it != titles.end(); ++it) {
            if (it->first == prepared) {
                titles.erase(it);
                return !keeps;
            }
        }
        return false;
    }
    Presence TicketAt(u64 prepared_id, u8 form) override {
        if (tickets_unread) {
            return Presence::Unknown;
        }
        return Has(tickets_at, AmTitleFor(prepared_id, form)) ? Presence::Present : Presence::Absent;
    }
    std::vector<u64> tickets_alone; // (the review of the build 2h, R1) the ids whose ticket ALONE the start asked to delete
    bool ticket_stays = false;      // ... and the guard refused it (or AM kept it)
    bool DropTicket(u64 prepared_id, u8 form) override {
        tickets_alone.push_back(AmTitleFor(prepared_id, form));
        if (!ticket_stays) {
            tickets_at.erase(std::remove(tickets_at.begin(), tickets_at.end(), AmTitleFor(prepared_id, form)), tickets_at.end());
        }
        return !ticket_stays;
    }
    bool DisableFolder(u64 prepared, u64 game) override {
        if (static_cast<u32>(prepared >> 32) == azl::title_rules::kTitleHighSystemApplication) {
            disabled_system.push_back(prepared); // (the build 2h: the folder of the system id, asked before the key's)
            return setup::DisableGuestFolder(prepared, game);
        }
        disabled.push_back(prepared);
        record_missing_at_disable += Exists(GuestPath(prepared)) ? 0 : 1; // (the record goes LAST)
        for (const u64 r : refuse_folder) {
            if (r == prepared) {
                return false;
            }
        }
        return setup::DisableGuestFolder(prepared, game);
    }
};

static void TestStart() {
    // nothing was ever prepared: no folder is made, nothing is asked
    Fresh();
    ClearRegistry();
    {
        FakeConsole c;
        const StartReport r = SweepAtStart(c);
        CHECK(r.guests == 0 && !r.index_written && !Exists(kDir) && c.pending.empty() && c.disabled.empty());
    }
    // every case at once
    Fresh();
    const Guest a = MakeGuest(kIslandEur);                              // Installed: kept
    const Guest b = MakeGuest(kGameB, GuestState::Preparing);           // a run that died after AM's Finish: taken away whole
    Guest c = MakeGuest(kGameC, GuestState::Preparing);                 // a run over an Installed child that AM kept: back to it (made again later)
    c.prev_installed = true;
    c.prev_version = 1008;
    Guest d = MakeGuest(kGameD, GuestState::Preparing);                 // ... whose old child is gone: taken away
    d.prev_installed = true;
    d.prev_version = 1008;
    const Guest e = MakeGuest(kGameE, GuestState::Removing);            // a removal that was cut: finished
    const Guest f = MakeGuest(kGameF, GuestState::Preparing);           // its folder cannot be taken away: kept, as Removing
    for (const Guest& g : {a, b, c, d, e, f}) {
        CHECK(SaveGuest(g));
        mkdir(setup::GamePluginDir(g.prepared).c_str(), 0777);
        Put(setup::GamePluginPath(g.prepared), "3GX$");
        Put(BlockPath(g.prepared), "b");
        Put(PartPath(g.prepared), "p");
    }
    mkdir(setup::GamePluginDir(kGameB).c_str(), 0777); // (the game's own folder: never the start's)
    Put(setup::GamePluginPath(kGameB), "3GX$");
    Put(IndexPath() + ".bak", "#dlp-children 1\n"); // the index cut between its two renames: the plugin sees none
    FakeConsole con;
    con.titles = {{a.prepared, 1024}, {b.prepared, 1024}, {c.prepared, 1008}, {e.prepared, 1024}, {f.prepared, 1024}, {0x000400000F800100ull, 1}};
    con.refuse_folder = {f.prepared};
    const StartReport r = SweepAtStart(con);
    CHECK(r.guests == 6 && r.kept == 1 && r.reverted == 1 && r.taken == 3 && r.failed == 1 && r.index_written);
    // the pending installations: of the titles taken away and of the run undone (THAT title's: the old child stays), of no other title (never all)
    CHECK(con.pending.size() == 5 && Has(con.pending, b.prepared) && Has(con.pending, c.prepared) && Has(con.pending, d.prepared) && Has(con.pending, e.prepared) && Has(con.pending, f.prepared));
    CHECK(con.deleted.size() == 3 && Has(con.deleted, b.prepared) && Has(con.deleted, e.prepared) && Has(con.deleted, f.prepared)); // (d's is not there)
    CHECK(con.disabled.size() == 4 && con.record_missing_at_disable == 0);
    CHECK(!Has(con.pending, a.prepared) && !Has(con.deleted, a.prepared) && !Has(con.deleted, c.prepared) && !Has(con.deleted, 0x000400000F800100ull));
    // what is left
    CHECK(Exists(GuestPath(a.prepared)) && Exists(BlockPath(a.prepared)) && Exists(setup::GamePluginPath(a.prepared)) && !Exists(PartPath(a.prepared)));
    for (const Guest& g : {b, d, e}) {
        CHECK(!Exists(GuestPath(g.prepared)) && !Exists(BlockPath(g.prepared)) && !Exists(PartPath(g.prepared)) && !Exists(setup::GamePluginDir(g.prepared)));
        CHECK(!IsOwnedGuestTid(g.prepared));
    }
    CHECK(Exists(setup::GamePluginPath(kGameB)));
    std::vector<Guest> now = LoadGuests();
    CHECK(now.size() == 3);
    for (const Guest& g : now) {
        if (g.prepared == c.prepared) {
            CHECK(g.state == GuestState::Installed && !g.prev_installed && Exists(BlockPath(c.prepared)) && !Exists(PartPath(c.prepared)));
            // the record names the child AM kept (its version), and composer 0: the next look makes it again
            CHECK(g.child_version == 1008 && g.composer == 0);
            Seen sc = GoodSeen(&g);
            sc.game = c.game;
            sc.child_version = 1008;
            sc.prepared_version = 1008;
            CHECK(Decide(sc) == Act::Prepare);
        } else if (g.prepared == f.prepared) {
            CHECK(g.state == GuestState::Removing && Exists(setup::GamePluginPath(f.prepared)) && !Exists(BlockPath(f.prepared)));
        } else {
            CHECK(g.prepared == a.prepared && g.state == GuestState::Installed);
        }
    }
    CHECK(IsOwnedGuestTid(a.prepared) && IsOwnedGuestTid(c.prepared) && IsOwnedGuestTid(f.prepared));
    // the index, written again: the two Installed children, nothing of a run or a removal; no .bak left
    const std::string idx_text = Get(IndexPath());
    dc::Index idx;
    CHECK(dc::ParseIndex(idx_text.data(), idx_text.size(), &idx) && idx.count == 2 && dc::FindPrepared(idx, a.prepared) >= 0 &&
          dc::FindPrepared(idx, c.prepared) >= 0 && !Exists(IndexPath() + ".bak"));
    const int ci = dc::FindPrepared(idx, c.prepared);
    CHECK(ci >= 0 && idx.rows[ci].version == 1008); // the row names the child AM kept, not the one the run was making
    // a second start: f is a removal now, and its folder still cannot go; nothing else is asked
    FakeConsole again;
    again.titles = {{a.prepared, 1024}, {c.prepared, 1008}, {f.prepared, 1024}};
    again.refuse_folder = {f.prepared};
    const StartReport r2 = SweepAtStart(again);
    CHECK(r2.kept == 2 && r2.failed == 1 && again.pending.size() == 1 && again.pending[0] == f.prepared);
    again.refuse_folder.clear();
    const StartReport r3 = SweepAtStart(again);
    CHECK(r3.kept == 2 && r3.taken == 1 && !IsOwnedGuestTid(f.prepared) && !Exists(setup::GamePluginDir(f.prepared)) && LoadGuests().size() == 2);

    // AM that does not answer: nothing is settled, nothing taken for gone (a removal, a run over an Installed child)
    Fresh();
    ClearRegistry();
    const Guest h = MakeGuest(kGameB, GuestState::Removing);
    Guest k = MakeGuest(kGameC, GuestState::Preparing);
    k.prev_installed = true;
    k.prev_version = 1008;
    for (const Guest& g : {h, k}) {
        CHECK(SaveGuest(g));
        mkdir(setup::GamePluginDir(g.prepared).c_str(), 0777);
        Put(setup::GamePluginPath(g.prepared), "3GX$");
        Put(PartPath(g.prepared), "p");
    }
    FakeConsole silent;
    silent.unknown = true;
    silent.titles = {{h.prepared, 1024}, {k.prepared, 1008}};
    const StartReport r4 = SweepAtStart(silent);
    CHECK(r4.later == 2 && r4.taken == 0 && r4.reverted == 0 && r4.failed == 0);
    CHECK(silent.deleted.empty() && silent.pending.empty() && silent.disabled.empty());
    CHECK(LoadGuests().size() == 2 && IsOwnedGuestTid(h.prepared) && IsOwnedGuestTid(k.prepared) && Exists(setup::GamePluginPath(h.prepared)) &&
          Exists(PartPath(k.prepared)));
    for (const Guest& g : LoadGuests()) {
        CHECK(g.state == (g.prepared == h.prepared ? GuestState::Removing : GuestState::Preparing));
    }
    // AM back: h's title is listed but cannot be described (Broken): deleted; k's id holds somebody else's title now: disowned, never touched
    FakeConsole back;
    back.broken = {h.prepared};
    back.titles = {{k.prepared, 1008}};
    back.products = {{k.prepared, "CTR-N-HBRW"}};
    const StartReport r5 = SweepAtStart(back);
    CHECK(r5.taken == 1 && r5.disowned == 1 && r5.reverted == 0 && r5.failed == 0);
    CHECK(Has(back.deleted, h.prepared) && !Has(back.deleted, k.prepared) && Has(back.pending, h.prepared) && !Has(back.pending, k.prepared));
    CHECK(Has(back.disabled, k.prepared) && back.titles.size() == 1 && back.titles[0].first == k.prepared);
    CHECK(LoadGuests().empty() && !IsOwnedGuestTid(h.prepared) && !IsOwnedGuestTid(k.prepared) && !Exists(setup::GamePluginDir(k.prepared)) &&
          !Exists(PartPath(k.prepared)));
    // a Broken title that AM does not delete: the record stays, as a removal
    Fresh();
    ClearRegistry();
    CHECK(SaveGuest(h));
    FakeConsole stuck;
    stuck.broken = {h.prepared};
    stuck.refuse_delete = {h.prepared};
    const StartReport r6 = SweepAtStart(stuck);
    CHECK(r6.failed == 1 && r6.taken == 0 && IsOwnedGuestTid(h.prepared) && LoadGuests().size() == 1 && LoadGuests()[0].state == GuestState::Removing);
    // a record to disown whose plugin's copy cannot go: it stays, and somebody else's title is still untouched
    Fresh();
    ClearRegistry();
    CHECK(SaveGuest(k));
    FakeConsole held;
    held.titles = {{k.prepared, 1}};
    held.products = {{k.prepared, "CTR-N-HBRW"}};
    held.refuse_folder = {k.prepared};
    const StartReport r7 = SweepAtStart(held);
    CHECK(r7.failed == 1 && r7.disowned == 0 && held.deleted.empty() && held.pending.empty() && IsOwnedGuestTid(k.prepared) && LoadGuests().size() == 1);
    // more records than registry slots: the ones past them are not ours to this run, and nothing of them is touched
    Fresh();
    ClearRegistry();
    for (std::size_t n = 0; n < kRegistrySlots + 2; ++n) {
        CHECK(SaveGuest(MakeGuest(0x0004000000100000ull + (static_cast<u64>(n) << 8), GuestState::Removing)));
    }
    FakeConsole over;
    const StartReport r8 = SweepAtStart(over);
    CHECK(r8.unowned == 2 && r8.taken == static_cast<int>(kRegistrySlots) && over.disabled.size() == kRegistrySlots && over.pending.size() == kRegistrySlots);
    CHECK(over.disabled_system.size() == kRegistrySlots); // (the build 2h: both folders of every record taken away)
    CHECK(LoadGuests().size() == 2);
}

// (the build 2f, 2026-11-19) the two ids of a prepared child: the record's form, the registry, Decide and the start with both forms
static void TestForms() {
    // the ids: M3c1's id of AM is 00040001|low, the system form's (the build 2h) 00040010|low for AM AND the process; SlotKeyOf takes the three
    // highs of the band back to the key, nothing else
    const u64 p = azl::title_rules::PreparedOf(kIslandEur);
    const u64 am = 0x000400010F8F8200ull;
    const u64 sys = 0x000400100F8F8200ull;
    CHECK(AmTitleFor(p, kFormDlpChild) == am && AmTitleFor(p, kFormApplication) == p && AmTitleFor(p, kFormSystem) == sys);
    CHECK(ProcessIdFor(p, kFormDlpChild) == p && ProcessIdFor(p, kFormApplication) == p && ProcessIdFor(p, kFormSystem) == sys);
    CHECK(SlotKeyOf(am) == p && SlotKeyOf(p) == p && SlotKeyOf(sys) == p && SlotKeyOf(0x00040001000F8200ull) == 0 && SlotKeyOf(0x000400120F8F8200ull) == 0);
    CHECK(SlotKeyOf(0x0004001000022100ull) == 0 && SlotKeyOf(azl::title_rules::kAppTid) == azl::title_rules::kAppTid); // (3DS Link is in the band: Register refuses it)
    // (DPAPP-2j) the form M3c1 is made again (the real AM refused the system form on the SD card, prova-2i); no fallback to HOME; the system form
    // and the old form are never installed; the jump goes to M3c1 and to the old form, never to the system form
    CHECK(kComposerVersion == 4 && kPreparedForm == kFormDlpChild && !kFallbackToApplicationForm && Installable(kFormDlpChild));
    CHECK(!Installable(kFormSystem) && !Installable(kFormApplication) && Jumpable(kFormDlpChild) && Jumpable(kFormApplication) && !Jumpable(kFormSystem));
    CHECK(kForms[0] == kFormDlpChild && kForms[1] == kFormApplication && kForms[2] == kFormSystem);
    // (the build 2h) the record of the system form, the old child's form and the ticket: written and read again; a record of the builds 2f/2g has
    // no prev_other_form - the other of its two forms; an old child in the run's own form, a ticket word this version does not know: refused
    Guest sg = MakeGuest(kIslandEur);
    sg.form = kFormSystem;
    sg.ticket_known = true;
    sg.ticket_ours = true;
    sg.prev_other = true;
    sg.prev_other_version = 1008;
    sg.prev_other_form = kFormDlpChild;
    const std::string stext = GuestText(sg);
    CHECK(stext.find("form=sys\n") != std::string::npos && stext.find("prev_other_form=dlp\n") != std::string::npos && stext.find("ticket=ours\n") != std::string::npos);
    Guest sback;
    CHECK(ParseGuest(stext, &sback) && sback.form == kFormSystem && sback.prev_other_form == kFormDlpChild && sback.ticket_known && sback.ticket_ours);
    CHECK(ParseGuest(Replace(stext, "ticket=ours", "ticket=other"), &sback) && sback.ticket_known && !sback.ticket_ours);
    CHECK(ParseGuest(Replace(stext, "ticket=ours\n", ""), &sback) && !sback.ticket_known && !sback.ticket_ours);
    CHECK(!ParseGuest(Replace(stext, "ticket=ours", "ticket=maybe"), &sback));
    CHECK(!ParseGuest(Replace(stext, "prev_other_form=dlp", "prev_other_form=sys"), &sback)); // (an old child in the run's own form)
    CHECK(!ParseGuest(Replace(stext, "prev_other_form=dlp", "prev_other_form=nand"), &sback));
    Guest legacy = MakeGuest(kIslandEur);
    legacy.form = kFormDlpChild;
    legacy.prev_other = true;
    CHECK(ParseGuest(Replace(GuestText(legacy), "prev_other_form=app\n", ""), &sback) && sback.prev_other_form == kFormApplication);
    legacy.form = kFormApplication;
    CHECK(ParseGuest(Replace(Replace(GuestText(legacy), "prev_other_form=app\n", ""), "prev_other=installed", "prev_other=none"), &sback) &&
          sback.prev_other_form == kFormDlpChild); // (an app record of 2f: its other form was dlp)
    // the composers before 3 are "to update", whatever their form
    for (const u8 composer : {1, 2}) {
        Guest older = MakeGuest(kIslandEur);
        older.composer = composer;
        older.form = composer == 2 ? kFormDlpChild : kFormApplication;
        Seen so = GoodSeen(&older);
        so.prepared_form = older.form;
        CHECK(Decide(so) == Act::Prepare);
    }
    // a key of 00040010 is never registered: the registry holds keys only
    ClearRegistry();
    CHECK(!Register(sys, kFormSystem) && !Register(am, kFormDlpChild) && !IsOwnedGuestTid(sys));
    // the record: form=dlp written and read again; a record without the key is of the old form; a value this version does not know: refused
    Guest g = MakeGuest(kIslandEur);
    g.form = kFormDlpChild;
    const std::string text = GuestText(g);
    CHECK(text.find("form=dlp\n") != std::string::npos);
    Guest back;
    CHECK(ParseGuest(text, &back) && back.form == kFormDlpChild && SameGuest(back, g));
    CHECK(ParseGuest(Replace(text, "form=dlp\n", ""), &back) && back.form == kFormApplication);
    CHECK(ParseGuest(Replace(text, "form=dlp", "form=app"), &back) && back.form == kFormApplication);
    CHECK(!ParseGuest(Replace(text, "form=dlp", "form=nand"), &back) && !ParseGuest(Replace(text, "form=dlp", "form="), &back));
    // (the review of the build 2f) the migration's old child: two keys written and read again; a record without them has none; a broken value: refused
    g.prev_other = true;
    g.prev_other_version = 1008;
    const std::string mig_text = GuestText(g);
    CHECK(mig_text.find("prev_other=installed\nprev_other_version=1008\n") != std::string::npos);
    CHECK(ParseGuest(mig_text, &back) && back.prev_other && back.prev_other_version == 1008 && SameGuest(back, g));
    CHECK(ParseGuest(Replace(Replace(mig_text, "prev_other=installed\n", ""), "prev_other_version=1008\n", ""), &back) && !back.prev_other && back.prev_other_version == 0);
    CHECK(!ParseGuest(Replace(mig_text, "prev_other=installed", "prev_other=yes"), &back) && !ParseGuest(Replace(mig_text, "prev_other_version=1008", "prev_other_version=x"), &back));
    g.prev_other = false;
    g.prev_other_version = 0;
    // the registry: either id of a registered title is ours; the form for the jump; nothing for another low word or another category
    ClearRegistry();
    CHECK(!IsOwnedGuestTid(am) && FormOf(p) == kFormApplication && AmTitleOf(p) == p);
    CHECK(Register(p, kFormDlpChild));
    CHECK(IsOwnedGuestTid(p) && IsOwnedGuestTid(am) && FormOf(p) == kFormDlpChild && AmTitleOf(p) == am);
    CHECK(!IsOwnedGuestTid(0x000400010F8F8300ull) && IsOwnedGuestTid(0x000400100F8F8200ull) && !IsOwnedGuestTid(0x000400120F8F8200ull));
    CHECK(!IsOwnedGuestTid(0x000400100F8F8300ull) && ProcessIdOf(p) == p); // (the build 2h: the system id of a registered key is ours too)
    SetForm(p, kFormSystem);
    CHECK(AmTitleOf(p) == sys && ProcessIdOf(p) == sys && IsOwnedGuestTid(am));
    SetForm(p, kFormDlpChild);
    CHECK(!IsOwnedGuestTid(0x00040001000F8200ull) && !IsOwnedGuestTid(kIslandEur)); // (the game's own child, the game: never)
    SetForm(p, kFormApplication);
    CHECK(FormOf(p) == kFormApplication && AmTitleOf(p) == p && IsOwnedGuestTid(am)); // (a migration: the other id stays ours)
    CHECK(Register(p, kFormDlpChild) && FormOf(p) == kFormDlpChild); // (registered already: its form set)
    SetForm(0x000400000F8F8300ull, kFormDlpChild);
    CHECK(FormOf(0x000400000F8F8300ull) == kFormApplication); // (nothing for a title that is not registered)
    Unregister(p);
    CHECK(!IsOwnedGuestTid(am) && FormOf(p) == kFormApplication);
    Fresh();
    CHECK(SaveGuest(g));
    LoadRegistry();
    CHECK(FormOf(p) == kFormDlpChild && AmTitleOf(p) == am);

    // Decide: a record of composer 1 (the old form) is "to update"; the other form's id of our child is a Repair; one that is not ours is left
    Guest old = MakeGuest(kIslandEur);
    old.composer = 1;
    Seen s = GoodSeen(&old);
    CHECK(Decide(s) == Act::Prepare);
    Guest hidden = MakeGuest(kIslandEur);
    hidden.form = kFormDlpChild;
    s = GoodSeen(&hidden);
    s.prepared_form = kFormDlpChild;
    CHECK(Decide(s) == Act::Nothing);
    s.other_form_present = true;
    CHECK(Decide(s) == Act::Nothing); // (not ours: never touched)
    s.other_form_ours = true;
    CHECK(Decide(s) == Act::Repair);
    Guest fallback = MakeGuest(kIslandEur); // (composer 2 in the old form: the fallback, never made again by itself)
    s = GoodSeen(&fallback);
    CHECK(Decide(s) == Act::Nothing);
    // no record: the child in either form is adopted; a title that is not the child on either id is refused
    s = GoodSeen(nullptr);
    s.prepared_form = kFormDlpChild;
    CHECK(Decide(s) == Act::Adopt);
    s.prepared_form = kFormApplication;
    CHECK(Decide(s) == Act::Adopt);
    s.other_form_present = true;
    CHECK(Decide(s) == Act::Refuse);
    s.other_form_ours = true;
    CHECK(Decide(s) == Act::Adopt);
    s = GoodSeen(nullptr);
    s.prepared_installed = false;
    CHECK(Decide(s) == Act::Prepare);
    s.other_form_present = true;
    CHECK(Decide(s) == Act::Refuse);
    // (the build 2h) an id of the slot that is somebody else's (the guard of the ids, from ids.cache): refused - never prepared, never adopted
    for (const int taken : {325 /* kErrIdSystem */, 326 /* kErrIdTicket */, 327 /* kErrIdRunning */, 312 /* kErrIdTaken */}) {
        s = GoodSeen(nullptr);
        s.prepared_installed = false;
        s.id_taken = taken;
        CHECK(Decide(s) == Act::Refuse);
        s = GoodSeen(nullptr);
        s.prepared_form = kFormSystem;
        s.id_taken = taken;
        CHECK(Decide(s) == Act::Refuse); // (our child there without its record: not adopted either)
    }

    // the start: a run of the hidden form that died: both ids go when they are ours (the same product code); one of another product is never
    // touched; Revert only in the form of the record
    Fresh();
    ClearRegistry();
    Guest died = MakeGuest(kIslandEur, GuestState::Preparing);
    died.form = kFormDlpChild;
    Guest other = MakeGuest(kGameB, GuestState::Preparing);
    other.form = kFormDlpChild;
    Guest back_to = MakeGuest(kGameC, GuestState::Preparing);
    back_to.form = kFormDlpChild;
    back_to.prev_installed = true;
    back_to.prev_version = 1008;
    Guest moved = MakeGuest(kGameD, GuestState::Preparing); // (prev_installed of the hidden form, but AM has only the old one: taken away)
    moved.form = kFormDlpChild;
    moved.prev_installed = true;
    moved.prev_version = 1008;
    for (const Guest& x : {died, other, back_to, moved}) {
        CHECK(SaveGuest(x));
    }
    FakeConsole con;
    con.titles = {{AmTitleFor(died.prepared, kFormDlpChild), 1024}, {died.prepared, 1024}, {AmTitleFor(other.prepared, kFormDlpChild), 1024},
                  {other.prepared, 1024}, {AmTitleFor(back_to.prepared, kFormDlpChild), 1008}, {moved.prepared, 1008}};
    con.products = {{other.prepared, "CTR-N-HBRW"}};
    const StartReport r = SweepAtStart(con);
    CHECK(r.taken == 3 && r.reverted == 1 && r.failed == 0);
    CHECK(Has(con.deleted, AmTitleFor(died.prepared, kFormDlpChild)) && Has(con.deleted, died.prepared));
    CHECK(Has(con.deleted, AmTitleFor(other.prepared, kFormDlpChild)) && !Has(con.deleted, other.prepared)); // (another product code: never)
    CHECK(!Has(con.deleted, AmTitleFor(back_to.prepared, kFormDlpChild)) && Has(con.pending, AmTitleFor(back_to.prepared, kFormDlpChild)));
    CHECK(Has(con.deleted, moved.prepared)); // (the old form's id, with the record's product code: the half of a migration that was cut)
    CHECK(!Has(con.pending, back_to.prepared) && !Has(con.pending, other.prepared));
    std::vector<Guest> left = LoadGuests();
    CHECK(left.size() == 1 && left[0].prepared == back_to.prepared && left[0].state == GuestState::Installed && left[0].form == kFormDlpChild);
    CHECK(FormOf(back_to.prepared) == kFormDlpChild && !IsOwnedGuestTid(AmTitleFor(died.prepared, kFormDlpChild)));

    // (the review of the build 2f) a migration cut in the middle (prev_other): its old child of the other form there whole, at its version and
    // ours - the record goes back to it (Installed, the old form, the composer it had), the new form's pending title and title go, the old one
    // NEVER; the old child gone or at another version: taken away as before; AM silent about the old one: left for later
    Fresh();
    ClearRegistry();
    Guest mig = MakeGuest(kIslandEur, GuestState::Preparing); // (a finished new form: AM lists both)
    mig.form = kFormDlpChild;
    mig.composer = 1;
    mig.prev_other = true;
    mig.prev_other_version = 1024;
    Guest cut = MakeGuest(kGameB, GuestState::Preparing); // (cut before the Finish: only the old one)
    cut.form = kFormDlpChild;
    cut.composer = 1;
    cut.prev_other = true;
    cut.prev_other_version = 1024;
    Guest moved_on = MakeGuest(kGameC, GuestState::Preparing); // (the old one at another version than the record says: taken away)
    moved_on.form = kFormDlpChild;
    moved_on.prev_other = true;
    moved_on.prev_other_version = 1024;
    for (const Guest& x : {mig, cut, moved_on}) {
        CHECK(SaveGuest(x));
        Put(BlockPath(x.prepared), std::string(azl::dlp_children::kBlockBytes, 'b'));
    }
    FakeConsole mc;
    mc.titles = {{AmTitleFor(mig.prepared, kFormDlpChild), 1024}, {mig.prepared, 1024}, {cut.prepared, 1024}, {moved_on.prepared, 1008}};
    const StartReport rm = SweepAtStart(mc);
    CHECK(rm.reverted == 2 && rm.taken == 1 && rm.failed == 0);
    // (DPAPP-2j) mig: the new hidden child (M3c1) is there whole and ours - kept (KeepNew: the old form, visible in HOME, is never installed again,
    // never gone back to); its pending part dropped, nothing deleted - the old form is a Repair of the next look
    CHECK(!Has(mc.deleted, AmTitleFor(mig.prepared, kFormDlpChild)) && !Has(mc.deleted, mig.prepared) && Has(mc.pending, AmTitleFor(mig.prepared, kFormDlpChild)));
    CHECK(!Has(mc.deleted, AmTitleFor(cut.prepared, kFormDlpChild)) && !Has(mc.deleted, cut.prepared) && Has(mc.pending, AmTitleFor(cut.prepared, kFormDlpChild)));
    CHECK(!Has(mc.pending, mig.prepared) && !Has(mc.pending, cut.prepared));
    CHECK(Has(mc.deleted, moved_on.prepared) && !Has(mc.disabled, mig.prepared) && !Has(mc.disabled, cut.prepared));
    left = LoadGuests();
    CHECK(left.size() == 2);
    for (const Guest& x : left) {
        const bool kept_new = x.prepared == mig.prepared; // (DPAPP-2j: KeepNew, the M3c1 child; cut: back to the old form, RevertOther)
        CHECK((x.prepared == mig.prepared || x.prepared == cut.prepared) && x.state == GuestState::Installed);
        CHECK(kept_new ? (x.form == kFormDlpChild && x.composer == 0 && FormOf(x.prepared) == kFormDlpChild)
                       : (x.form == kFormApplication && x.composer == 1 && FormOf(x.prepared) == kFormApplication));
        CHECK(!x.prev_other && x.prev_other_version == 0 && x.child_version == 1024 && IsOwnedGuestTid(x.prepared));
        CHECK(Exists(BlockPath(x.prepared))); // (the old child's block stays: it is the child that works)
    }
    CHECK(!Exists(BlockPath(moved_on.prepared)) && !IsOwnedGuestTid(moved_on.prepared));
    // the pure decision: the old one there whole -> RevertOther; AM silent of it -> Later; another product code there -> no revert (TakeAway)
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::RevertOther);
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Present, 1024, "") == StartAct::RevertOther); // (a code that does not read: the record is the proof)
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Unknown, 0, "") == StartAct::Later);
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Present, 1024, "CTR-N-HBRW") == StartAct::TakeAway);
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Broken, 0, "") == StartAct::TakeAway);
    CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Absent, 0, "") == StartAct::TakeAway);
    Guest plain = mig;
    plain.prev_other = false;
    CHECK(DecideAtStart(plain, Presence::Absent, 0, "", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::TakeAway); // (no migration said: as before)
    CHECK(DecideAtStart(mig, Presence::Present, 1024, "CTR-N-HBRW", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::Disown);

    // Decide (the review of the build 2f): a child of composer 1 is "to update", but a Repair it needs comes first (the child that works kept
    // right); composer 0 - a child to make again - never gets one (its block would be another child's)
    Guest c1 = MakeGuest(kIslandEur);
    c1.composer = 1;
    Seen sc = GoodSeen(&c1);
    CHECK(Decide(sc) == Act::Prepare);
    sc.plugin_on = false;
    CHECK(Decide(sc) == Act::Repair);
    sc.plugin_on = true;
    sc.language = 2;
    CHECK(Decide(sc) == Act::Repair);
    c1.composer = 0;
    CHECK(Decide(sc) == Act::Prepare);
    c1.composer = kComposerVersion;
    CHECK(Decide(sc) == Act::Repair);
}

// (the review of the build 2h) the start: R1 - a run cut after its Begin left the ticket its record says is ours, with no title: it goes with the
// record (the record stays as a removal when it does not go); never a ticket the record does not say is ours. R2 - a migration from M3c1 cut after
// AM's Finish keeps the new child (KeepNew), never goes back to M3c1. R3 - RevertOther never deletes a listed title whose code does not read.
static void TestStart2hRev() {
    // R1: TakeAway of a run that died with no title: the ticket alone, then the record
    for (int stays = 0; stays < 2; ++stays) {
        Fresh();
        ClearRegistry();
        Guest died = MakeGuest(kIslandEur, GuestState::Preparing);
        died.form = kFormSystem;
        SayTicketOurs(died);
        CHECK(SaveGuest(died));
        FakeConsole c;
        c.ticket_stays = stays == 1;
        const StartReport r = SweepAtStart(c);
        CHECK(c.tickets_alone.size() == 1 && c.tickets_alone[0] == AmTitleFor(died.prepared, kFormSystem) && c.deleted.empty());
        if (stays) {
            CHECK(r.failed == 1 && LoadGuests().size() == 1 && LoadGuests()[0].state == GuestState::Removing);
        } else {
            CHECK(r.taken == 1 && LoadGuests().empty());
        }
    }
    // ... never a ticket the record does not say is ours (a record of the builds before), never one of a title AM lists
    Fresh();
    ClearRegistry();
    {
        Guest old = MakeGuest(kIslandEur, GuestState::Preparing);
        old.form = kFormSystem;
        old.ticket_known = false;
        Guest listed = MakeGuest(kGameB, GuestState::Removing);
        listed.form = kFormSystem;
        SayTicketOurs(listed);
        CHECK(SaveGuest(old) && SaveGuest(listed));
        FakeConsole c;
        c.titles = {{AmTitleFor(listed.prepared, kFormSystem), 1024}};
        SweepAtStart(c);
        CHECK(c.tickets_alone.empty() && Has(c.deleted, AmTitleFor(listed.prepared, kFormSystem)));
    }
    // R2: a migration from a form never installed again (DPAPP-2j: the system form of the build 2h, prev_other_form sys) cut after AM's Finish: the
    // new child (M3c1) kept, the old title untouched (a Repair of the next look)
    Fresh();
    ClearRegistry();
    {
        Guest mig = MakeGuest(kIslandEur, GuestState::Preparing);
        mig.form = kFormDlpChild;
        mig.composer = 2;
        mig.prev_other = true;
        mig.prev_other_version = 1024;
        mig.prev_other_form = kFormSystem;
        SayTicketOurs(mig);
        CHECK(SaveGuest(mig));
        CHECK(DecideAtStart(mig, Presence::Present, 1040, "CTR-P-ATSP", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::KeepNew);
        CHECK(DecideAtStart(mig, Presence::Present, 1040, "", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::RevertOther); // (its code unread)
        CHECK(DecideAtStart(mig, Presence::Absent, 0, "", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::RevertOther);  // (cut before the Finish)
        Guest from_app = mig;
        from_app.prev_other_form = kFormApplication; // (DPAPP-2j: the old form shows in HOME and is never installed again - the new child is kept too)
        CHECK(DecideAtStart(from_app, Presence::Present, 1040, "CTR-P-ATSP", Presence::Present, 1024, "CTR-P-ATSP") == StartAct::KeepNew);
        FakeConsole c;
        c.titles = {{AmTitleFor(mig.prepared, kFormDlpChild), 1040}, {AmTitleFor(mig.prepared, kFormSystem), 1024}};
        const StartReport r = SweepAtStart(c);
        CHECK(r.reverted == 1 && c.deleted.empty() && c.tickets_alone.empty() && Has(c.pending, AmTitleFor(mig.prepared, kFormDlpChild)));
        const std::vector<Guest> left = LoadGuests();
        CHECK(left.size() == 1 && left[0].state == GuestState::Installed && left[0].form == kFormDlpChild && left[0].composer == 0 &&
              left[0].child_version == 1040 && !left[0].prev_other && FormOf(mig.prepared) == kFormDlpChild);
    }
    // R3: RevertOther with the new form's title listed and its code unread: never deleted; with no title and the ticket ours: the ticket alone
    Fresh();
    ClearRegistry();
    {
        Guest mig = MakeGuest(kIslandEur, GuestState::Preparing);
        mig.form = kFormSystem;
        mig.composer = 2;
        mig.prev_other = true;
        mig.prev_other_version = 1024;
        mig.prev_other_form = kFormApplication;
        SayTicketOurs(mig);
        Guest cut = mig;
        cut.game = kGameB;
        cut.prepared = azl::title_rules::PreparedOf(kGameB);
        CHECK(SaveGuest(mig) && SaveGuest(cut));
        FakeConsole c;
        c.titles = {{AmTitleFor(mig.prepared, kFormSystem), 1040}, {mig.prepared, 1024}, {cut.prepared, 1024}};
        c.products = {{AmTitleFor(mig.prepared, kFormSystem), ""}};
        const StartReport r = SweepAtStart(c);
        CHECK(r.reverted == 2 && c.deleted.empty());
        CHECK(c.tickets_alone.size() == 1 && c.tickets_alone[0] == AmTitleFor(cut.prepared, kFormSystem));
        for (const Guest& x : LoadGuests()) {
            CHECK(x.state == GuestState::Installed && x.form == kFormApplication);
        }
    }
}

// (DPAPP-2h-sec) the record's word on its ticket holds on the console it was written on only (console=): a card moved to another console never makes the
// start drop that console's ticket; Titles Delivery never sends an id of the band in a child's form, and a receiver installs only what recv_filter.h
// lets through (the module would delete the title and its ticket first, on the media ts_title_dest says)
static void TestSec2h() {
    // the key console=: written with the ticket word, read back; 0 or a value that does not read refuses the record
    Guest g = MakeGuest(kIslandEur, GuestState::Installed);
    g.form = kFormSystem;
    SayTicketOurs(g);
    const std::string text = GuestText(g);
    CHECK(text.find("console=" + std::to_string(ThisConsole()) + "\n") != std::string::npos);
    Guest back;
    CHECK(ParseGuest(text, &back) && back.console == ThisConsole() && TicketWordHere(back));
    CHECK(!ParseGuest(Replace(text, "console=" + std::to_string(ThisConsole()), "console=0"), &back));
    CHECK(!ParseGuest(Replace(text, "console=" + std::to_string(ThisConsole()), "console=x1"), &back));
    CHECK(ParseGuest(Replace(text, "console=" + std::to_string(ThisConsole()) + "\n", ""), &back) && back.console == 0 && !TicketWordHere(back));
    Guest plain = MakeGuest(kIslandEur, GuestState::Installed);
    CHECK(GuestText(plain).find("console=") == std::string::npos); // (no stamp: no key - an older record stays as it was)
    CHECK(ConsoleStampOf(0x1111) != ConsoleStampOf(0x2222) && ConsoleStampOf(0) == 0);

    // the start on ANOTHER console: a run cut after its Begin (TakeAway) and a migration cut (RevertOther), the title gone, the ticket said ours
    // there - this console's ticket at the id is never dropped; the record goes as before
    for (int way = 0; way < 2; ++way) {
        Fresh();
        ClearRegistry();
        Guest died = MakeGuest(kIslandEur, GuestState::Preparing);
        died.form = kFormSystem;
        SayTicketOurs(died);
        died.console = ConsoleStampOf(0x2222); // (written on the console of the card's owner)
        if (way == 1) {
            died.composer = 2;
            died.prev_other = true;
            died.prev_other_version = 1024;
            died.prev_other_form = kFormApplication;
        }
        CHECK(SaveGuest(died));
        FakeConsole c;
        if (way == 1) {
            c.titles = {{died.prepared, 1024}};
        }
        const StartReport r = SweepAtStart(c);
        CHECK(c.tickets_alone.empty() && c.deleted.empty());
        CHECK(way == 0 ? (r.taken == 1 && LoadGuests().empty()) : (r.reverted == 1 && LoadGuests().size() == 1));
        // ... the same record written on THIS console: the ticket alone goes (the review of the build 2h, R1)
        Fresh();
        ClearRegistry();
        died.console = ThisConsole();
        CHECK(SaveGuest(died));
        FakeConsole here;
        if (way == 1) {
            here.titles = {{died.prepared, 1024}};
        }
        SweepAtStart(here);
        CHECK(here.tickets_alone.size() == 1 && here.tickets_alone[0] == AmTitleFor(died.prepared, kFormSystem));
    }

    // Titles Delivery's sender: the band in a child's form never, ours or not; the band's applications (the homebrew) only when they are not ours
    Fresh();
    ClearRegistry();
    const u64 key = azl::title_rules::PreparedOf(kIslandEur);
    CHECK(NeverSent(azl::title_rules::PreparedSystemOf(key)) && NeverSent(AmTitleFor(key, kFormDlpChild)) && !IsOwnedGuestTid(azl::title_rules::PreparedSystemOf(key)));
    // (DPAPP-2j-rev, the decision of DPAPP-2h-sec) the key itself - the application form - with no record is a title of the SD card like a homebrew of
    // the band with the same id: sent (its record says a guest, never its id)
    CHECK(!NeverSent(key) && !NeverSent(0x000400000FF40000ull) && !NeverSent(kIslandEur) && !NeverSent(0x0004000E000F8200ull));
    CHECK(NeverSent(0x0004001000022100ull)); // (DPAPP-2j: the system's own Download Play - no title of the SD card - is never sent either)
    CHECK(Register(key, kFormSystem));
    CHECK(NeverSent(key) && NeverSent(azl::title_rules::PreparedSystemOf(key)));
    ClearRegistry();

    // Titles Delivery's receiver (titleshare_host/recv_filter.h): only an application, a demo, an update or a DLC of the SD card, never 3DS Link
    CHECK(dl_recv_refusal(kIslandEur) == DL_RECV_OK && dl_recv_refusal(0x0004000E000F8200ull) == DL_RECV_OK && dl_recv_refusal(0x0004008C000F8200ull) == DL_RECV_OK);
    CHECK(dl_recv_refusal(0x000400000FF40000ull) == DL_RECV_OK); // (a homebrew of the band: as any application; the app's registry says its guests)
    CHECK(dl_recv_refusal(0x0004013800000002ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x0004013000002E02ull) == DL_RECV_NOT_SD); // NATIVE_FIRM, a sysmodule
    CHECK(dl_recv_refusal(0x0004001000022000ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x0004003000008F02ull) == DL_RECV_NOT_SD);  // a system application, HOME
    CHECK(dl_recv_refusal(0x000400100F8F8200ull) == DL_RECV_GUEST_OTHER && dl_recv_refusal(0x000400010F8F8200ull) == DL_RECV_GUEST_OTHER);  // a guest, M3c1
    CHECK(dl_recv_refusal(0x000400100F800100ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x000400010FF40000ull) == DL_RECV_NOT_SD);  // the band, no sold game's
    CHECK(dl_recv_refusal(0x00040001000F8200ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x0004800542383841ull) == DL_RECV_NOT_SD);  // a child, DSiWare
    CHECK(dl_recv_refusal(0x0003000042383841ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x0004000000123402ull) == DL_RECV_NOT_SD);  // TWL, variation 02
    CHECK(dl_recv_refusal(azl::title_rules::kAppTid) == DL_RECV_SELF && dl_recv_refusal(azl::title_rules::kAppTid | 0x01) == DL_RECV_SELF);
    CHECK(dl_recv_refusal(0x0004000E0F83D100ull) == DL_RECV_OK); // (an update is not 3DS Link)
    // every id the filter lets through is one FBI's rule (the module's ts_title_dest) puts on the SD card
    u64 seed = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const u64 highs[] = {0x00040000ull, 0x0004000Eull, 0x0004008Cull, 0x00040010ull, 0x00040001ull, 0x00040002ull, 0x00040030ull, 0x00040138ull, 0x00048004ull, 0x00030000ull};
        const u64 tid = (highs[(seed >> 59) % 10] << 32) | (seed & 0xFFFFFFFFull);
        const u16 platform = static_cast<u16>(tid >> 48), category = static_cast<u16>(tid >> 32);
        const u8 variation = static_cast<u8>(tid);
        const bool nand = platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02)));
        if (dl_recv_refusal(tid) == DL_RECV_OK) {
            CHECK(!nand);
        }
    }
}

// (DPAPP-2h-sec, second pass) a record whose word on its ticket is not known to be this console's (no console=, or the stamp unknown): the start
// drops no ticket on it and never forgets the record while the console holds a ticket at the id; a delete that took the title and not its ticket
// (kTicketLeft) of another form is followed by the ticket alone; Titles Delivery receives a demo of the SD card
static void TestSec2hSecond() {
    for (int way = 0; way < 3; ++way) {
        // way 0: a record of the builds before (no console=); way 1: this console's stamp unknown; way 2: no ticket at the id (the record goes)
        Fresh();
        ClearRegistry();
        Guest died = MakeGuest(kIslandEur, GuestState::Preparing);
        died.form = kFormSystem;
        SayTicketOurs(died);
        died.console = way == 1 ? ConsoleStampOf(0x1111) : 0;
        CHECK(SaveGuest(died));
        const std::vector<Guest> saved = LoadGuests();
        CHECK(saved.size() == 1);
        const Guest back = saved.empty() ? Guest() : saved[0];
        CHECK(back.ticket_known && back.ticket_ours && back.console == died.console);
        if (way == 1) {
            SetThisConsole(0);
        }
        CHECK(TicketWordUnplaced(back) && !TicketWordHere(back));
        FakeConsole c;
        if (way != 2) {
            c.tickets_at = {AmTitleFor(died.prepared, kFormSystem)};
        }
        const StartReport r = SweepAtStart(c);
        SetThisConsole(ConsoleStampOf(0x1111));
        CHECK(c.tickets_alone.empty() && c.deleted.empty() && c.tickets_dropped.empty());
        if (way == 2) {
            CHECK(r.taken == 1 && LoadGuests().empty());
        } else {
            CHECK(r.failed == 1 && LoadGuests().size() == 1 && LoadGuests()[0].state == GuestState::Removing && LoadGuests()[0].ticket_ours &&
                  c.tickets_at.size() == 1);
        }
    }
    // ... the lists not read: the record stays too
    Fresh();
    ClearRegistry();
    {
        Guest died = MakeGuest(kIslandEur, GuestState::Removing);
        died.form = kFormSystem;
        SayTicketOurs(died);
        died.console = 0;
        CHECK(SaveGuest(died));
        FakeConsole c;
        c.tickets_unread = true;
        const StartReport r = SweepAtStart(c);
        CHECK(r.failed == 1 && LoadGuests().size() == 1 && c.tickets_alone.empty());
    }
    // ... a record of the builds before whose title is listed and ours: the title and its ticket go (R7, the legacy rule), then the record
    Fresh();
    ClearRegistry();
    {
        Guest gone = MakeGuest(kIslandEur, GuestState::Removing);
        gone.form = kFormSystem;
        SayTicketOurs(gone);
        gone.console = 0;
        CHECK(SaveGuest(gone));
        FakeConsole c;
        c.titles = {{AmTitleFor(gone.prepared, kFormSystem), 1024}};
        c.tickets_at = {AmTitleFor(gone.prepared, kFormSystem)};
        const StartReport r = SweepAtStart(c);
        CHECK(r.taken == 1 && LoadGuests().empty() && Has(c.tickets_dropped, AmTitleFor(gone.prepared, kFormSystem)) && c.tickets_at.empty());
    }
    // the other form of a cut migration, ours: its delete takes the title and not its ticket - the ticket alone at once, then the record
    for (int stays = 0; stays < 2; ++stays) {
        Fresh();
        ClearRegistry();
        Guest mig = MakeGuest(kIslandEur, GuestState::Removing);
        mig.form = kFormSystem;
        SayTicketOurs(mig);
        CHECK(SaveGuest(mig));
        FakeConsole c;
        c.titles = {{mig.prepared, 1024}}; // (the old form's title, the record's product code)
        c.delete_keeps_ticket = {mig.prepared};
        c.ticket_stays = stays == 1;
        const StartReport r = SweepAtStart(c);
        CHECK(Has(c.deleted, mig.prepared) && Has(c.tickets_alone, mig.prepared));
        if (stays) {
            CHECK(r.failed == 1 && LoadGuests().size() == 1 && LoadGuests()[0].state == GuestState::Removing);
        } else {
            CHECK(r.taken == 1 && LoadGuests().empty());
        }
    }
    // Titles Delivery's receiver: a demo of the SD card (00040002) as before the first review; never one the module would put in the NAND
    CHECK(dl_recv_refusal(0x0004000200123400ull) == DL_RECV_OK && dl_recv_refusal(0x0004000200123402ull) == DL_RECV_OK);
    CHECK(dl_recv_refusal(0x0004000300123400ull) == DL_RECV_NOT_SD && dl_recv_refusal(0x0004001200123400ull) == DL_RECV_NOT_SD);
}

// (DPAPP-2j, R-B) a card moved or copied to another console, as the system does it: every console's records in the folder of its ID0
// (FSUSER_GetSdmcCtrRootPath), another console's never read, decided on or deleted - its blocks kept; the records of the shared folder of the builds
// before moved only when this console wrote them, or (no console=) AM lists their title with their code
static const std::string kId0A = std::string("0123456789abcdef") + "0123456789abcdef"; // (in two pieces: never 32 hex digits written out)
static const std::string kId0B = std::string("fedcba9876543210") + "fedcba9876543210";
static const std::string kId1 = std::string("0011223344556677") + "8899aabbccddeeff";
static void TestOtherConsole2j() {
    // the ID0 as the system names it
    CHECK(Id0OfRoot("/Nintendo 3DS/" + std::string("0123456789ABCDEF") + "0123456789abcdef/" + kId1) == kId0A);
    CHECK(Id0OfRoot("/Nintendo 3DS/" + kId0A + "/" + kId1 + "/") == kId0A);
    CHECK(Id0OfRoot("").empty() && Id0OfRoot("/Nintendo 3DS/").empty() && Id0OfRoot("/Nintendo 3DS/0123/4567").empty());
    CHECK(Id0OfRoot("/Nintendo DS/" + kId0A + "/" + kId1).empty());
    CHECK(Id0OfRoot("/Nintendo 3DS/" + kId0A.substr(0, 31) + "g/" + kId1).empty());
    CHECK(Id0OfRoot("/Nintendo 3DS/" + kId0A + "/" + kId1 + "/x").empty());
    // (DPAPP-2j-rev) an emulator's card (Azahar does not answer FSUSER_GetSdmcCtrRootPath): its ONE ID0 folder; none or two - never a guess
    const std::string zeros = std::string(16, '0') + std::string(16, '0');
    CHECK(Id0OfFolders({zeros, "Private"}) == zeros && Id0OfFolders({"Private", kId0B}) == kId0B);
    CHECK(Id0OfFolders({std::string("0123456789ABCDEF") + "0123456789abcdef"}) == kId0A);
    CHECK(Id0OfFolders({kId0A, kId0B}).empty() && Id0OfFolders({"Private"}).empty() && Id0OfFolders({}).empty() && Id0OfFolders({kId0A.substr(1)}).empty());
    {
        const std::string card = "card2jrev/Nintendo 3DS";
        fs::create_directories(card + "/" + zeros + "/" + kId1);
        fs::create_directories(card + "/Private");
        CHECK(Id0OfSdCard(card) == zeros);
        fs::create_directories(card + "/" + kId0A);
        CHECK(Id0OfSdCard(card).empty());
        fs::remove_all("card2jrev");
        CHECK(Id0OfSdCard(card).empty());
    }
    // the ID0 not known: no record is read or written - so nothing of AM (every write of AM comes after its record)
    Fresh();
    ClearRegistry();
    SetThisId0("");
    CHECK(RecordsDir().empty() && GuestPath(PreparedFor(kIslandEur)).empty() && PartPath(PreparedFor(kIslandEur)).empty());
    CHECK(!SaveGuest(MakeGuest(kIslandEur)) && LoadGuests().empty() && !Exists(kDir + "/000400000F8F8200.guest"));
    SetThisId0("not an id0");
    CHECK(ThisId0().empty());
    // console A prepares Island Tour and game B
    SetThisId0(kId0A);
    CHECK(ThisId0() == kId0A && RecordsDir() == kDir + "/" + kId0A);
    Guest ia = MakeGuest(kIslandEur);
    SayTicketOurs(ia);
    Guest ba = MakeGuest(kGameB);
    SayTicketOurs(ba);
    CHECK(SaveGuest(ia) && SaveGuest(ba));
    Put(BlockPath(ia.prepared), std::string(azl::dlp_children::kBlockBytes, 'a'));
    Put(BlockPath(ba.prepared), std::string(azl::dlp_children::kBlockBytes, 'a'));
    const std::string ia_text = Get(GuestPath(ia.prepared)), ba_text = Get(GuestPath(ba.prepared));
    // the card in console B: nothing of A is read, decided, asked of AM, or deleted; A's blocks stay; the index holds B's children only
    SetThisId0(kId0B);
    SetThisConsole(ConsoleStampOf(0x2222));
    ClearRegistry();
    CHECK(LoadGuests().empty());
    {
        FakeConsole none; // (B's AM lists none of A's titles)
        const StartReport r = SweepAtStart(none);
        CHECK(r.guests == 0 && r.moved == 0 && none.deleted.empty() && none.pending.empty() && none.disabled.empty() && none.tickets_alone.empty());
    }
    CHECK(!IsOwnedGuestTid(ia.prepared) && !IsOwnedGuestTid(ba.prepared));
    CHECK(Get(kDir + "/" + kId0A + "/000400000F8F8200.guest") == ia_text && Get(kDir + "/" + kId0A + "/" + Name16(ba.prepared) + ".guest") == ba_text);
    CHECK(Exists(BlockPath(ia.prepared)) && Exists(BlockPath(ba.prepared))); // (another console's record names them: kept)
    CHECK(Get(IndexPath()).find("0F8F8200") == std::string::npos);
    const std::vector<u64> others = OtherConsolesKeys();
    CHECK(others.size() == 2 && std::binary_search(others.begin(), others.end(), ia.prepared) && std::binary_search(others.begin(), others.end(), ba.prepared));
    // B prepares Island Tour too: its own record, next to A's, never over it
    Guest ib = MakeGuest(kIslandEur);
    ib.game_version = 1056;
    SayTicketOurs(ib);
    CHECK(SaveGuest(ib) && GuestPath(ib.prepared) == kDir + "/" + kId0B + "/000400000F8F8200.guest");
    CHECK(Get(kDir + "/" + kId0A + "/000400000F8F8200.guest") == ia_text && LoadGuests().size() == 1 && LoadGuests()[0].game_version == 1056);
    // B removes its child: A's record, block untouched
    CHECK(DropGuest(ib.prepared) && Get(kDir + "/" + kId0A + "/000400000F8F8200.guest") == ia_text && Exists(BlockPath(ia.prepared)));
    // ... a block no console's record names goes
    const u64 lone = PreparedFor(kGameC);
    Put(BlockPath(lone), "b");
    SweepOrphans(LoadGuests());
    CHECK(!Exists(BlockPath(lone)) && Exists(BlockPath(ia.prepared)));
    // the card back in console A: its records are there, as they were (Keep)
    SetThisId0(kId0A);
    SetThisConsole(ConsoleStampOf(0x1111));
    ClearRegistry();
    {
        FakeConsole back;
        back.titles = {{AmTitleFor(ia.prepared, ia.form), 1024}, {AmTitleFor(ba.prepared, ba.form), 1024}};
        const StartReport r = SweepAtStart(back);
        CHECK(r.guests == 2 && r.kept == 2 && back.deleted.empty());
    }
    CHECK(IsOwnedGuestTid(ia.prepared) && IsOwnedGuestTid(ba.prepared) && Get(IndexPath()).find("000400000F8F8200") != std::string::npos);

    // the records of the shared folder (the builds before DPAPP-2j): this console's moved in, another's and the unproved ones left intact
    Fresh();
    ClearRegistry();
    SetThisId0(kId0A);
    Guest mine = MakeGuest(kIslandEur); // (written by this console: console= is ThisConsole)
    SayTicketOurs(mine);
    Guest other = MakeGuest(kGameB); // (written by another console)
    SayTicketOurs(other);
    other.console = ConsoleStampOf(0x9999);
    const Guest listed = MakeGuest(kGameC); // (no console=, the builds up to 2h: AM lists its title with its code - ours)
    const Guest absent = MakeGuest(kGameD); // (no console=, its title not listed: not proved - left)
    Guest coded = MakeGuest(kGameE);        // (no console=, listed with another product code: not ours - left)
    setup::MakeDirs(kDir);
    for (const Guest& x : {mine, other, listed, absent, coded}) {
        Put(kDir + "/" + Name16(x.prepared) + ".guest", GuestText(x));
    }
    Put(kDir + "/" + Name16(mine.prepared) + ".guest.bak", "old");
    {
        FakeConsole c;
        c.titles = {{AmTitleFor(listed.prepared, listed.form), 1024}, {AmTitleFor(coded.prepared, coded.form), 1024}};
        c.products = {{AmTitleFor(coded.prepared, coded.form), "CTR-N-HBRW"}};
        const StartReport r = SweepAtStart(c);
        CHECK(r.moved == 2 && r.other == 3);
        CHECK(c.deleted.empty() && c.tickets_alone.empty());
    }
    CHECK(Exists(kDir + "/" + kId0A + "/" + Name16(mine.prepared) + ".guest") && !Exists(kDir + "/" + Name16(mine.prepared) + ".guest") &&
          !Exists(kDir + "/" + Name16(mine.prepared) + ".guest.bak"));
    CHECK(Exists(kDir + "/" + kId0A + "/" + Name16(listed.prepared) + ".guest") && !Exists(kDir + "/" + Name16(listed.prepared) + ".guest"));
    for (const Guest& x : {other, absent, coded}) {
        CHECK(Get(kDir + "/" + Name16(x.prepared) + ".guest") == GuestText(x) && !Exists(kDir + "/" + kId0A + "/" + Name16(x.prepared) + ".guest"));
        CHECK(!IsOwnedGuestTid(x.prepared));
    }
    CHECK(IsOwnedGuestTid(mine.prepared) && IsOwnedGuestTid(listed.prepared));
    // a start cut between the write and the remove: the copy in this console's folder is the record; the shared one goes when it is the same text
    Put(kDir + "/" + Name16(mine.prepared) + ".guest", Get(kDir + "/" + kId0A + "/" + Name16(mine.prepared) + ".guest"));
    {
        FakeConsole c;
        c.titles = {{AmTitleFor(mine.prepared, mine.form), 1024}, {AmTitleFor(listed.prepared, listed.form), 1024}};
        ClearRegistry();
        SweepAtStart(c);
    }
    CHECK(!Exists(kDir + "/" + Name16(mine.prepared) + ".guest") && Exists(kDir + "/" + kId0A + "/" + Name16(mine.prepared) + ".guest"));
    SetThisId0(kId0A);
    ClearRegistry();
}

// (DPAPP-2j, R-E) Titles Delivery: the sender offers only what a receiver takes, and never a Download Play guest - its record lost, another
// console's, past the registry: every sold game's guest id under 00040001 and 00040010 is never sent, never received. (DPAPP-2j-rev) Under 00040000
// the id alone is no guest (a homebrew of the SD card may have it): the record says - with one, never sent; with none, a title of the SD card
static void TestDelivery2j() {
    ClearRegistry();
    int guests = 0;
    for (u32 unique = 0; unique < 0x100000u; ++unique) {
        const bool sold = (unique >= setup::kSoldIdFirst && unique <= setup::kSoldIdLast) || (unique >= setup::kSoldNewIdFirst && unique <= setup::kSoldNewIdLast);
        if (!sold) {
            continue;
        }
        const u64 game = 0x0004000000000000ull | (static_cast<u64>(unique) << 8);
        CHECK(dl_recv_refusal(game) == DL_RECV_OK && !NeverSent(game) && !dl_is_prepared_of_sold(game)); // (the game itself: as ever)
        const u64 key = azl::title_rules::PreparedOf(game);
        if (key == 0) {
            continue;
        }
        ++guests;
        for (const u8 form : kForms) {
            const u64 id = AmTitleFor(key, form);
            if (form == kFormApplication) {
                CHECK(id == key && !dl_is_prepared_of_sold(id) && !NeverSent(id) && dl_recv_refusal(id) == DL_RECV_OK);
            } else {
                CHECK(dl_is_prepared_of_sold(id) && NeverSent(id) && dl_recv_refusal(id) == DL_RECV_GUEST_OTHER);
            }
        }
    }
    CHECK(guests > 7000);
    // (DPAPP-2j-rev) the application form of a guest with its record: never sent (the registry), whatever its id
    {
        const u64 key = azl::title_rules::PreparedOf(kIslandEur);
        CHECK(!NeverSent(key) && Register(key, kFormApplication) && NeverSent(key) && NeverSent(AmTitleFor(key, kFormDlpChild)));
        ClearRegistry();
        CHECK(!NeverSent(key));
    }
    // (DPAPP-2j-rev) homebrew CIAs of the SD card whose ids are those of sold games' guests (0x0F830000-0x0F9FFFFF, 0x0FF00000-0x0FF1FFFF):
    // sent and received as any application of the SD card (before DPAPP-2j they were; DPAPP-2j refused a quarter of the band)
    for (const u64 hb : {0x000400000F850000ull, 0x000400000F900000ull, 0x000400000F9FFF00ull, 0x000400000FF15F00ull, 0x000400000F8F8200ull}) {
        CHECK(!dl_is_prepared_of_sold(hb) && !NeverSent(hb) && dl_recv_refusal(hb) == DL_RECV_OK);
    }
    int homebrew_sent = 0;
    for (u32 unique = 0xF8000u; unique < 0x100000u; ++unique) {
        const u64 hb = 0x0004000000000000ull | (static_cast<u64>(unique) << 8);
        homebrew_sent += !NeverSent(hb) && dl_recv_refusal(hb) == DL_RECV_OK ? 1 : 0;
    }
    CHECK(homebrew_sent == 0x8000 - 1); // (every unique id of the band but 3DS Link's)
    // the band's homebrew that is no sold game's guest stays sendable (FBI 0x0F8001xx); 3DS Link never
    CHECK(!NeverSent(0x000400000F800100ull) && dl_recv_refusal(0x000400000F800100ull) == DL_RECV_OK && !dl_is_prepared_of_sold(0x000400000F800100ull));
    CHECK(!NeverSent(0x000400000FF40000ull) && NeverSent(azl::title_rules::kAppTid) && !dl_is_prepared_of_sold(azl::title_rules::kAppTid));
    // only titles of the SD card: applications, demos, updates, DLC - never a system title, a DSiWare, a child of a game, a guest
    CHECK(!NeverSent(0x0004000E000F8200ull) && !NeverSent(0x0004008C000F8200ull) && !NeverSent(0x0004000200123400ull));
    CHECK(NeverSent(0x00040001000F8200ull) && NeverSent(0x0004800542383841ull) && NeverSent(0x0004001000022100ull) && NeverSent(0x0004013000002E02ull));
    CHECK(NeverSent(0x0004000000123402ull)); // (variation 0x02: FBI puts it in the NAND)
    // the receiver's reason for the guest of another console in a guest's form, and the sender's: the same predicate
    CHECK(dl_recv_refusal(0x000400010F8F8200ull) == DL_RECV_GUEST_OTHER && dl_recv_refusal(0x000400100FF15F00ull) == DL_RECV_GUEST_OTHER);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_dlpstore_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    SetThisConsole(ConsoleStampOf(0x1111)); // (DPAPP-2h-sec: the console the records are written on)
    SetThisId0(std::string("0123456789abcdef") + "0123456789abcdef"); // (DPAPP-2j, R-B: the folder of this console's records)
    TestGuestText();
    TestFolder();
    TestRegistry();
    TestCache();
    TestIndex();
    TestBlock();
    TestSweep();
    TestDecide();
    TestGames();
    TestExclude();
    TestGuestFolderGuard();
    TestStart();
    TestForms();
    TestStart2hRev();
    TestSec2h();
    TestSec2hSecond();
    TestOtherConsole2j();
    TestDelivery2j();
    if (chdir("/") != 0) {
        std::fprintf(stderr, "cannot leave %s\n", dir.string().c_str());
    }
    fs::remove_all(dir);
    std::printf("dlp_guest_store_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
