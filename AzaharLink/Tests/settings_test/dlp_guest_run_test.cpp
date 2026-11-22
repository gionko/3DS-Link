// The worker of the Download Play children 3DS Link prepares (1.1.4, piece APP-WORKER: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 4.7,
// its critic 6-APP-boot9-CRITICO.md 4.1-4.5 and 6 steps 5-6, K9 of 7-CONTRATTO.md): ConfigApp/source/dlp_guest_run.cpp (the jobs), dlp_guest_route.hpp
// (route K: a game's content 2 inside the module's virtual CIA, the container's NCCH layer; the facts of a child's CIA) and dlp_guest_reconcile.hpp
// (the quiet and the heavy part, 16 children at most, the time), the very files, built on a PC against a folder called "sdmc:" and a made-up console.
// What it holds:
//   - route K finds content 2 by the TMD's order and the header's bitmap, reads the container's RomFS through its layer at any offset and length, and
//     refuses a container of another method, with a seed, or a game with no content 2 (no child);
//   - a child's facts: id, version, the TMD's hash, the product code under the title key (a wrong KeyY is kErrTitleKey);
//   - Prepare: the record is Preparing when AM is first asked, nothing is ever deleted before the install, the work file goes on every way out, a failure
//     before AM's Finish leaves the record as it was (none, or the Installed child AM kept), the installed CIA is the composed one (the prepared id in its
//     ticket and TMD, the original length), then the folder, the block, the record (Installed, composer, product, language, hidden), the index row;
//     no room, AM silent, a title that is not ours, a stop, a version AM does not confirm, a folder that fails;
//   - Remove (the title only when it is still ours, the record last), Disown (AM never asked), Probe (children.cache, "no child"), Check (the same child
//     recorded, another made again), Adopt, Repair; a child named by the container's first file;
//   - the reconciliation's order, its cap of 16, the question not asked twice, the time of Island Tour's child (2 min 40 s / 3 min 25 s), the bar.
// Made-up children only (Tests/settings_test/dlp_guest_synth.hpp): no byte of Nintendo, no real key.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "azl/dlp_children.hpp"
#include "azl/dlp_title_block.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_paths.hpp"
#include "dlp_guest_reconcile.hpp"
#include "dlp_guest_route.hpp"
#include "dlp_guest_run.hpp"
#include "dlp_guest_store.hpp"
#include "dlp_guest_synth.hpp"
#include "games.hpp"

namespace fs = std::filesystem;
using namespace dlpguest;
using synth::Bytes;

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
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const long long va_ = static_cast<long long>(a), vb_ = static_cast<long long>(b);          \
        if (va_ != vb_) {                                                                          \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                          \
    } while (0)

constexpr u64 kGame = 0x00040000000F8200ull;
constexpr u64 kChild = 0x00040001000F8200ull;
const u64 kPrepared = azl::title_rules::PreparedOf(kGame);
// (DPAPP-2j) the id AM installs the hidden child by: M3c1, 000400010F8F8200 (its process is the key, kPrepared); the tests of the builds 2f and 2h
// hold for it with the two forms swapped - the system form of the build 2h (kM3c1 keeps its name: the OTHER hidden form, 000400100F8F8200) is the
// one read, migrated and removed, never installed
constexpr u8 kHiddenForm = kPreparedForm;
// (DPAPP-2j, R-D) AM's own refusal (module AM 32, description 37 "Invalid NCCH": FBI's error.c) - the one Result that is a refusal of the form
constexpr RunResult kAmRefusal = static_cast<RunResult>(0xC8A08025u);
static_assert(kHiddenForm == kFormDlpChild, "the tests below make the form M3c1");
const u64 kAm = AmTitleFor(kPrepared, kFormDlpChild);
const u64 kM3c1 = AmTitleFor(kPrepared, kFormSystem);

static Keys KeysOf(const synth::MadeKeys& k) {
    Keys out;
    std::memcpy(out.ncch_x, k.ncch_x, 16);
    std::memcpy(out.common_x, k.common_x, 16);
    std::memcpy(out.common_y0, k.common_y0, 16);
    std::memcpy(out.generator, k.generator, 16);
    return out;
}

static bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static u64 FileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<u64>(st.st_size) : 0;
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

static bool FindGuest(u64 prepared, Guest* out) {
    for (const Guest& g : LoadGuests()) {
        if (g.prepared == prepared) {
            *out = g;
            return true;
        }
    }
    return false;
}

static void Clean() {
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777); // (the card's root: the app's MakeDirs never makes it)
    ClearRegistry();
}

// ---- a made-up container: content 2 of a game, an NCCH whose RomFS holds the child's CIA --------------------------------------------

struct Container {
    Bytes ncch;     // the container as the game's content 2 is on the card (its layer on, unless plain)
    Bytes romfs;    // its RomFS, plain (what route F gives, and what route K must give)
};

static Container MakeContainer(const Bytes& child_cia, const std::string& name, const synth::MadeKeys& keys, bool plain, u8 method = 0, bool seed = false) {
    Container c;
    u64 at = 0;
    c.romfs = synth::InRomFs(child_cia, name, &at);
    c.romfs.resize(synth::Up(c.romfs.size(), 0x200), 0);
    c.ncch.assign(0x200 + c.romfs.size(), 0);
    synth::Rng r(77);
    r.Fill(c.ncch.data(), 0x100);
    std::memcpy(&c.ncch[0x100], "NCCH", 4);
    synth::Le32(c.ncch, 0x104, static_cast<u32>(c.ncch.size() / 0x200));
    const u64 partition = 0x00040000000F8299ull;
    synth::Le64(c.ncch, 0x108, partition);
    c.ncch[0x18B] = method;
    c.ncch[0x18F] = static_cast<u8>((plain ? 0x04 : 0x00) | (seed ? 0x20 : 0x00) | 0x02);
    synth::Le32(c.ncch, 0x1B0, 1);
    synth::Le32(c.ncch, 0x1B4, static_cast<u32>(c.romfs.size() / 0x200));
    std::memcpy(&c.ncch[0x200], c.romfs.data(), c.romfs.size());
    if (!plain) {
        u8 key[16];
        synth::KeyOf(keys.ncch_x, c.ncch.data(), keys.generator, key);
        synth::CtrRegion(key, partition, 3, &c.ncch[0x200], c.romfs.size());
    }
    return c;
}

// A virtual CIA of a game with contents 0, (1, not in the file) and 2.
static Bytes MakeVirtualCia(const Bytes& content2, bool with_content2 = true) {
    const u32 cert = 0xA00, ticket = 0x350;
    const u32 body = 4 + 0x13C;
    const u32 count = with_content2 ? 3 : 2;
    const u32 tmd = body + 0xC4 + 0x900 + count * 0x30;
    Bytes c0(0x340, 0x5A);
    const u64 content_at = synth::Up(0x2020, 64) + synth::Up(cert, 64) + synth::Up(ticket, 64) + synth::Up(tmd, 64);
    const u64 total_content = synth::Up(c0.size(), 64) + (with_content2 ? synth::Up(content2.size(), 64) : 0);
    Bytes cia(content_at + total_content, 0);
    synth::Le32(cia, 0x00, 0x2020);
    synth::Le32(cia, 0x08, cert);
    synth::Le32(cia, 0x0C, ticket);
    synth::Le32(cia, 0x10, tmd);
    synth::Le64(cia, 0x18, total_content);
    cia[0x20] = static_cast<u8>(0x80 | (with_content2 ? 0x20 : 0)); // the bitmap: content 0 and 2 (1 is not in the file)
    const u64 tmd_at = synth::Up(0x2020, 64) + synth::Up(cert, 64) + synth::Up(ticket, 64);
    synth::Be32(cia, tmd_at, 0x00010004);
    synth::Be16(cia, tmd_at + body + 0x9E, static_cast<u16>(count));
    const u64 chunks = tmd_at + body + 0xC4 + 0x900;
    const u64 sizes[3] = {c0.size(), 0x777, content2.size()};
    for (u32 i = 0; i < count; ++i) {
        synth::Be32(cia, chunks + i * 0x30, 0x100 + i);
        synth::Be16(cia, chunks + i * 0x30 + 4, static_cast<u16>(i));
        synth::Be16(cia, chunks + i * 0x30 + 6, 0);
        synth::Be64(cia, chunks + i * 0x30 + 8, sizes[i]);
    }
    std::memcpy(&cia[content_at], c0.data(), c0.size());
    if (with_content2) {
        std::memcpy(&cia[content_at + synth::Up(c0.size(), 64)], content2.data(), content2.size());
    }
    return cia;
}

static bool ReadBytes(void* ctx, u64 off, u8* buf, u32 len) {
    const Bytes* b = static_cast<const Bytes*>(ctx);
    if (off > b->size() || len > b->size() - off) {
        return false;
    }
    std::memcpy(buf, b->data() + off, len);
    return true;
}

static void TestRoute() {
    const synth::Made m = synth::Make(synth::Opts());
    const Keys keys = KeysOf(m.keys);
    for (int plain = 0; plain < 2; ++plain) {
        const Container c = MakeContainer(m.cia, "00040001000f8200.cia", m.keys, plain == 1);
        Bytes cia = MakeVirtualCia(c.ncch);
        std::vector<u8> scratch(64);
        KeyedRomfs k;
        CHECK_EQ(OpenKeyedRomfs(&ReadBytes, &cia, cia.size(), keys, scratch.data(), scratch.size(), &k), 0);
        CHECK_EQ(k.romfs_size, c.romfs.size());
        CHECK(k.plain == (plain == 1));
        // every offset and length gives the plain RomFS (the scratch is 64 bytes: many aligned pieces)
        bool all = true;
        const u64 offs[] = {0, 1, 15, 16, 17, 100, 0x3FF, 0x401, c.romfs.size() - 33};
        const u32 lens[] = {1, 15, 16, 17, 63, 64, 65, 200, 33};
        for (u64 off : offs) {
            for (u32 len : lens) {
                if (off + len > c.romfs.size()) {
                    continue;
                }
                std::vector<u8> got(len);
                all = all && ReadKeyedRomfs(&k, off, got.data(), len) && std::memcmp(got.data(), c.romfs.data() + off, len) == 0;
            }
        }
        CHECK(all);
        std::vector<u8> whole(c.romfs.size());
        CHECK(ReadKeyedRomfs(&k, 0, whole.data(), static_cast<u32>(whole.size())) && whole == c.romfs);
        u8 one = 0;
        CHECK(!ReadKeyedRomfs(&k, c.romfs.size(), &one, 1)); // (past the end: refused)
        // the child's package is found through it, as route K gives it to the jobs
        azl::dlp_real::ChildPackage pkg{};
        CHECK(azl::dlp_real::FindChildPackage(&ReadKeyedRomfs, &k, 0x000F8200u, &pkg) == azl::dlp_real::PackageWhy::Ok);
        CHECK_EQ(pkg.required_size, m.cia.size());
        CloseKeyedRomfs(&k);
        bool wiped = true;
        for (u32 w : k.rk) {
            wiped = wiped && w == 0;
        }
        CHECK(wiped);
    }
    // the content found by the TMD's order and the bitmap (content 1 is not in the file)
    {
        const Container c = MakeContainer(m.cia, "00040001000f8200.cia", m.keys, true);
        Bytes cia = MakeVirtualCia(c.ncch);
        ContentAt at;
        CHECK_EQ(FindContent(&ReadBytes, &cia, cia.size(), 2, &at), 0);
        CHECK_EQ(at.size, c.ncch.size());
        CHECK(std::memcmp(cia.data() + at.at + 0x100, "NCCH", 4) == 0);
        CHECK_EQ(FindContent(&ReadBytes, &cia, cia.size(), 1, &at), kErrNoChild);
        Bytes none = MakeVirtualCia(c.ncch, false);
        std::vector<u8> scratch(64);
        KeyedRomfs k;
        CHECK_EQ(OpenKeyedRomfs(&ReadBytes, &none, none.size(), keys, scratch.data(), scratch.size(), &k), kErrNoChild);
        Bytes cut(cia.begin(), cia.begin() + 0x2000);
        CHECK_EQ(FindContent(&ReadBytes, &cut, cut.size(), 2, &at), kErrNotCia);
        CHECK_EQ(OpenKeyedRomfs(&ReadBytes, &cia, cia.size(), keys, scratch.data(), 8, &k), kErrNoMemory);
    }
    // containers that boot9.bin cannot open
    for (int v = 0; v < 2; ++v) {
        const Container c = MakeContainer(m.cia, "00040001000f8200.cia", m.keys, false, v == 0 ? 1 : 0, v == 1);
        Bytes cia = MakeVirtualCia(c.ncch);
        std::vector<u8> scratch(64);
        KeyedRomfs k;
        CHECK_EQ(OpenKeyedRomfs(&ReadBytes, &cia, cia.size(), keys, scratch.data(), scratch.size(), &k), kErrContainerCrypto);
    }
}

static void TestChildFacts() {
    for (int encrypted = 0; encrypted < 2; ++encrypted) {
        synth::Opts o;
        o.title_key = encrypted == 1;
        const synth::Made m = synth::Make(o);
        Keys keys = KeysOf(m.keys);
        Bytes cia = m.cia;
        ChildFacts f;
        CHECK_EQ(ReadChildFacts(&ReadBytes, &cia, 0, cia.size(), keys, &f), 0);
        CHECK_EQ(f.child, kChild);
        CHECK_EQ(f.version, o.version);
        CHECK(std::memcmp(f.hash8, m.tmd.data() + 4 + 0x13C + 0xC4 + 0x900 + 0x10, 8) == 0);
        CHECK(std::memcmp(f.product, m.plain.data() + 0x150, 16) == 0 && f.product[16] == '\0');
        if (encrypted == 1) {
            keys.common_y0[3] ^= 1;
            CHECK_EQ(ReadChildFacts(&ReadBytes, &cia, 0, cia.size(), keys, &f), kErrTitleKey);
        }
        CHECK_EQ(ReadChildFacts(&ReadBytes, &cia, 0, cia.size() - 1, keys, &f), kErrNotCia);
    }
}

// ---- a made-up console ------------------------------------------------------------------------------------------------------------

struct Fake final : WorkConsole {
    Bytes romfs;              // content 2 (route F gives it as it is)
    int open_rc = 0;
    Route route = Route::System;
    bool open = false;
    std::map<u64, std::pair<u16, std::string>> titles; // AM: the SD card's titles (version, product)
    bool am_silent = false;
    u64 free = 1ull << 34;
    bool begin_fail = false, finish_fail = false, folder_fail = false, stop = false, delete_fail = false;
    long install_fail_at = -1; // the Install call that fails
    u16 lie_version = 0;      // AM says this version after the Finish (0 = the true one)
    bool finish_forgets = false; // AM takes the Finish but does not list the title after it
    bool refuse_lower = true; // AM refuses at the Finish a version lower than the one installed (as the console does)
    Bytes installed;
    bool installing = false;
    std::vector<std::string> calls;
    int record_state_at_begin = -1; // the record's state when AM was first asked to install
    std::string product_at_begin;   // ... and its product code then
    bool part_at_begin = false;
    bool prev_other_at_begin = false;       // (the review of the build 2f) the record says the migration's old child when AM is asked
    u8 form_of_at_begin = 0xFF;             // ... and the registry still says the form the console has (FormOf)

    std::vector<std::string> record_at_open; // (DPAPP-2h-sec) the record's text when the content was opened ("" = none)
    int OpenContent(u64, bool, Route* r) override {
        calls.push_back("open");
        {
            Guest g;
            record_at_open.push_back(FindGuest(kPrepared, &g) ? GuestText(g) : std::string());
        }
        *r = route;
        open = open_rc == 0;
        return open_rc;
    }
    bool ReadContent(u64 off, u8* buf, u32 len) override {
        if (!open || off > romfs.size() || len > romfs.size() - off) {
            return false;
        }
        std::memcpy(buf, romfs.data() + off, len);
        return true;
    }
    void CloseContent() override {
        open = false;
    }
    bool card_holds = true; // the game card in the slot is the job's game
    int card_reads = 0;
    int card_gone_after = -1; // (>= 0) the card is taken out after that many reads of the slot: in the middle of a look
    bool CardHolds(u64) override {
        ++card_reads;
        return card_holds && (card_gone_after < 0 || card_reads <= card_gone_after);
    }
    Presence Installed(u64 prepared, u8 form, u16* version) override {
        if (am_silent) {
            return Presence::Unknown;
        }
        const auto it = titles.find(AmTitleFor(prepared, form)); // (the build 2f: AM lists the ids of AM)
        if (it == titles.end()) {
            return Presence::Absent;
        }
        *version = it->second.first;
        return Presence::Present;
    }
    bool Product(u64 prepared, u8 form, char* out) override {
        std::memset(out, 0, kProductBytes);
        const auto it = titles.find(AmTitleFor(prepared, form));
        if (it == titles.end()) {
            return false;
        }
        std::strncpy(out, it->second.second.c_str(), 15);
        return out[0] != '\0';
    }
    bool FreeBytes(u64* f) override {
        *f = free;
        return true;
    }
    // (the build 2f) a system that does not take the hidden form: at the begin, a write, the Finish, or a Finish that leaves it off the SD card
    bool refuse_hidden_begin = false, refuse_hidden_write = false, refuse_hidden_finish = false, hidden_not_listed = false;
    u8 install_form = kFormApplication;
    std::vector<u8> begin_forms;
    std::vector<u64> deleted_ids;
    u64 install_am = 0;          // (the review of the build 2h) the id of the install under way
    bool ticket_at_write = false; // AM takes the ticket with the CIA's first bytes (as the console is believed to): CancelInstall leaves it
    long stop_at_install = -1;    // (>= 0) B is pressed once that many writes of the install went in
    bool nand_fails_after_finish = false; // the NAND's list is not read once AM took a Finish
    bool finished_once = false;
    int record_state_at_delete = -1; // the record's state when AM was last asked to delete a title
    u8 record_form_at_delete = 0xFF;
    RunResult BeginInstall(u64 prepared, u8 form, bool ticket_expected, bool title_expected) override {
        calls.push_back("begin");
        begin_forms.push_back(form);
        // (the build 2h) the second guard, as dlp_guest_am.cpp asks it: never the form M3c1, the console's lists read again and AmMayWrite
        IdCensus live;
        if (!Installable(form) || !ReadIds(&live) || !AmMayWrite(live, AmTitleFor(prepared, form), ProcessIdFor(prepared, form), true, ticket_expected, title_expected)) {
            ++guard_refusals;
            return -9;
        }
        install_form = form;
        if (form == kFormDlpChild && refuse_hidden_begin) {
            return -7;
        }
        Guest g;
        record_state_at_begin = FindGuest(prepared, &g) ? static_cast<int>(g.state) : -1;
        product_at_begin = record_state_at_begin >= 0 ? std::string(g.product) : std::string();
        prev_other_at_begin = record_state_at_begin >= 0 && g.prev_other;
        form_of_at_begin = FormOf(prepared);
        part_at_begin = IsFile(PartPath(prepared));
        if (begin_fail || !IsOwnedGuestTid(prepared)) {
            return -1;
        }
        installing = true;
        installed.clear();
        install_am = AmTitleFor(prepared, form);
        return 0;
    }
    long installs = 0;
    // (DPAPP-2j, R-D) the Result of a write that fails: AM's refusal by default (module AM, "Invalid NCCH"); a test sets an error of the SD card,
    // a full card, AM's state, or a number of no module
    RunResult install_fail_rc = kAmRefusal;
    RunResult finish_rc = 0; // (DPAPP-2j, R-D) the Result the Finish answers (0: as the console)
    RunResult Install(u64 off, const u8* buf, u32 len) override {
        if (!installing || off != installed.size() || installs++ == install_fail_at || (install_form == kFormDlpChild && refuse_hidden_write)) {
            if (installs - 1 == install_fail_at && on_install_fail) {
                on_install_fail(*this);
            }
            return install_fail_rc;
        }
        if (installed.empty() && ticket_at_write) {
            tickets.insert(install_am); // (the ticket goes in with the front: AM_CancelCIAInstall does not take it back)
        }
        installed.insert(installed.end(), buf, buf + len);
        if (stop_at_install >= 0 && installs >= stop_at_install) {
            stop = true;
        }
        return 0;
    }
    std::function<void(Fake&)> on_finish;        // (DPAPP-2h-sec) something that changes the console when AM is asked to finish
    std::function<void(Fake&)> on_install_fail;  // ... when a write is refused (install_fail_at)
    RunResult FinishInstall() override {
        calls.push_back("finish");
        if (on_finish) {
            on_finish(*this);
        }
        installing = false;
        if (finish_fail) {
            return -2;
        }
        if (finish_rc != 0) {
            return finish_rc; // (DPAPP-2j, R-D: a Result of AM or FS for the Finish)
        }
        // AM takes what was written: its TMD's id and version, its NCCH's product code
        u8 head[0x20];
        std::memcpy(head, installed.data(), 0x20);
        CiaLayout lay;
        TmdFacts tmd;
        if (!ReadLayout(head, installed.size(), &lay) || !ReadTmd(installed.data() + lay.tmd_at, lay.tmd, &tmd)) {
            return -3;
        }
        if (static_cast<u32>(tmd.tid >> 32) == azl::title_rules::kTitleHighDlpChild && refuse_hidden_finish) {
            return kAmRefusal; // (DPAPP-2j: AM's own refusal of the hidden form - module AM)
        }
        const auto there = titles.find(tmd.tid);
        if (refuse_lower && there != titles.end() && there->second.first > tmd.version) {
            return -5; // (a lower version over a higher one)
        }
        char product[17] = {};
        std::memcpy(product, installed.data() + lay.content_at + 0x150, 16);
        const bool hidden = static_cast<u32>(tmd.tid >> 32) == azl::title_rules::kTitleHighDlpChild; // (DPAPP-2j: the hidden form made, M3c1)
        if (!finish_forgets && !(hidden && hidden_not_listed)) {
            titles[tmd.tid] = {lie_version != 0 ? lie_version : tmd.version, product};
        }
        if (finish_to_nand) {
            titles.erase(tmd.tid);
            nand.insert(tmd.tid); // (a system that put it in the NAND)
        }
        tickets.insert(tmd.tid); // (AM writes the ticket of what it installs)
        finished_once = true;
        return 0;
    }
    std::vector<u8> cancel_forms;
    u64 free_back_at_cancel = 0; // (DPAPP-2h-sec, second pass) the room AM's pending title and the work file give back once cancelled and removed
    void CancelInstall(u64 prepared, u8 form) override {
        calls.push_back("cancel " + std::to_string(prepared == kPrepared));
        cancel_forms.push_back(form);
        installing = false;
        free += free_back_at_cancel;
        free_back_at_cancel = 0;
    }
    std::vector<u64> keep_ticket_once; // (DPAPP-2h-sec, second pass) a delete of that id takes the title, keeps its ticket, says so (kTicketLeft)
    RunResult DeleteTitle(u64 prepared, u8 form, bool drop_ticket) override {
        calls.push_back("delete");
        // (the build 2h) the second guard, as dlp_guest_am.cpp asks it: nothing at all is deleted when it refuses
        IdCensus live;
        if (!ReadIds(&live) || !AmMayWrite(live, AmTitleFor(prepared, form), ProcessIdFor(prepared, form), false, false, false)) {
            ++guard_refusals;
            return -9;
        }
        deleted_ids.push_back(AmTitleFor(prepared, form));
        {
            Guest g;
            record_state_at_delete = FindGuest(prepared, &g) ? static_cast<int>(g.state) : -1;
            record_form_at_delete = record_state_at_delete >= 0 ? g.form : 0xFF;
        }
        if (!delete_fail) {
            titles.erase(AmTitleFor(prepared, form));
        }
        bool kept = false;
        if (drop_ticket) {
            const auto k = std::find(keep_ticket_once.begin(), keep_ticket_once.end(), AmTitleFor(prepared, form));
            if (k != keep_ticket_once.end()) {
                keep_ticket_once.erase(k);
                kept = true;
            }
        }
        if (drop_ticket && !kept && titles.count(AmTitleFor(prepared, form)) == 0) {
            // (DPAPP-2h-sec, as dlp_guest_am.cpp DeletePrepared: the ticket only once AM no longer lists the title)
            tickets.erase(AmTitleFor(prepared, form));
            tickets_dropped.push_back(AmTitleFor(prepared, form));
        }
        if (delete_fail) {
            return -4;
        }
        return kept && titles.count(AmTitleFor(prepared, form)) == 0 ? -10 : 0; // (the second pass: the title gone, its ticket not - kTicketLeft)
    }
    std::vector<std::pair<u64, bool>> folders;
    bool Folder(u64 prepared, u64, bool on) override {
        folders.emplace_back(prepared, on);
        return !folder_fail;
    }
    // (the build 2h) the console's other lists of ids, for the guard: the NAND, the tickets, the card, the processes, the titles begun
    std::set<u64> nand, tickets, card, processes = {azl::title_rules::kAppTid}, pending_sd, pending_nand;
    std::vector<u64> tickets_dropped;
    bool finish_to_nand = false;
    int ids_fail_from = -1;  // (>= 0) the ReadIds from that one on say a list was not read
    int ids_reads = 0;
    int guard_refusals = 0;
    std::function<void(Fake&, int)> on_read; // something that changes the console between two reads (the read's number)
    bool ReadIds(IdCensus* out) override {
        const int n = ids_reads++;
        if (on_read) {
            on_read(*this, n);
        }
        struct Lists final : IdReader {
            Fake* f;
            bool fail;
            bool List(int source, std::vector<u64>* ids) override {
                ids->clear();
                switch (source) {
                case kSrcSd: for (const auto& t : f->titles) ids->push_back(t.first); break;
                case kSrcNand: if (f->nand_fails_after_finish && f->finished_once) return false; ids->assign(f->nand.begin(), f->nand.end()); break;
                case kSrcTicket: if (fail) return false; ids->assign(f->tickets.begin(), f->tickets.end()); break;
                case kSrcCard: ids->assign(f->card.begin(), f->card.end()); break;
                case kSrcProcess: ids->assign(f->processes.begin(), f->processes.end()); break;
                case kSrcPendingSd: ids->assign(f->pending_sd.begin(), f->pending_sd.end()); break;
                case kSrcPendingNand: ids->assign(f->pending_nand.begin(), f->pending_nand.end()); break;
                default: return false;
                }
                return true;
            }
            u64 NowMs() override {
                return 0;
            }
        } lists;
        lists.f = this;
        lists.fail = ids_fail_from >= 0 && n >= ids_fail_from;
        *out = IdCensus();
        return RefreshCensus(out, lists, nullptr);
    }
    bool Stop() override {
        return stop;
    }
    void Progress(int, u64, u64) override {}
    std::vector<std::string> steps;
    void Step(const char* what, u64, RunResult) override {
        steps.push_back(what);
    }
    std::vector<std::string> notes;    // (the build 2e slot25) the lines CryptoLine made
    std::vector<std::string> id_notes; // (the build 2h) the lines of the guard of the ids ("guest: ids ...")
    void Note(const char* line) override {
        if (std::strncmp(line, "guest: ids", 10) == 0) {
            id_notes.push_back(line);
            return;
        }
        notes.push_back(line);
        steps.push_back("note");
    }
    std::vector<u64> home_icons; // (the build 2e) the titles whose icon was read by {tid, SD}
    void HomeIcon(u64 prepared) override {
        home_icons.push_back(prepared);
        steps.push_back("home icon");
    }
    // (DPAPP-2j, R-D) a line of the log (Note) that holds `s`
    bool NoteHas(const std::string& s) const {
        for (const std::string& n : notes) {
            if (n.find(s) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    int StepAt(const std::string& what) const {
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i] == what) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    int Count(const std::string& c) const {
        int n = 0;
        for (const std::string& x : calls) {
            n += x == c ? 1 : 0;
        }
        return n;
    }
};

struct Bench {
    synth::Made made;
    Keys keys;
    std::vector<u8> buf, work;
    std::unique_ptr<Composed> composed;
    RunIn in;
    Fake con;
    u64 at = 0; // where the child's CIA is in the container's RomFS
    explicit Bench(const synth::Opts& o = synth::Opts(), const std::string& name = "00040001000f8200.cia") : made(synth::Make(o)), keys(KeysOf(made.keys)),
                                                                                                      buf(4096 + 16), work(kWorkBytes), composed(new Composed) {
        in.keys = &keys;
        in.language = 1;
        in.buf = buf.data();
        in.buf_size = buf.size();
        in.work = work.data();
        in.work_size = work.size();
        in.composed = composed.get();
        con.romfs = synth::InRomFs(made.cia, name, &at);
    }
};

// (DPAPP-2j) the child just made (M3c1) turned into one of another form: the console's title and ticket at that form's id, the record of that form
// at `composer` (the 2i's fallbacks shown in HOME: the old form; the build 2h's system form, which only Azahar took)
static void MakeOtherForm(Bench& b, u8 form, u8 composer) {
    const u64 to = AmTitleFor(kPrepared, form);
    b.con.titles[to] = b.con.titles[kAm];
    b.con.titles.erase(kAm);
    if (b.con.tickets.erase(kAm) != 0) {
        b.con.tickets.insert(to);
    }
    Guest g;
    CHECK(FindGuest(kPrepared, &g));
    g.form = form;
    g.composer = composer;
    CHECK(SaveGuest(g));
    CHECK(Register(kPrepared, form));
}

static Planned Job(Act act, u16 game_version = 0x10, u16 update_version = 0) {
    Planned p;
    p.game = kGame;
    p.act = act;
    p.game_version = game_version;
    p.update_version = update_version;
    return p;
}

static void TestPrepare() {
    Clean();
    {
        Bench b;
        JobNote note;
        const int first = RunJob(Job(Act::Prepare), b.in, b.con, &note);
        CHECK_EQ(first, 0);
        if (first != 0) {
            for (const std::string& s : b.con.steps) {
                std::fprintf(stderr, "    step: %s\n", s.c_str());
            }
        }
        CHECK_EQ(b.con.record_state_at_begin, static_cast<int>(GuestState::Preparing)); // the title was ours before AM was asked
        // ... with its product code (the final review of 1.1.4: a cut from here on leaves a record the start holds against the title at the id)
        CHECK(!b.con.product_at_begin.empty() && std::memcmp(b.con.product_at_begin.data(), b.made.plain.data() + 0x150, b.con.product_at_begin.size()) == 0);
        CHECK(b.con.part_at_begin);
        CHECK_EQ(b.con.Count("delete"), 0); // nothing deleted before (or after) the install
        CHECK_EQ(b.con.Count("begin"), 1);
        CHECK_EQ(b.con.Count("finish"), 1);
        CHECK(!IsFile(PartPath(kPrepared)));
        CHECK_EQ(b.con.installed.size(), b.made.cia.size()); // the length of the original
        CiaLayout lay;
        TmdFacts tmd;
        TicketFacts tik;
        CHECK(ReadLayout(b.con.installed.data(), b.con.installed.size(), &lay) && ReadTmd(b.con.installed.data() + lay.tmd_at, lay.tmd, &tmd) &&
              ReadTicket(b.con.installed.data() + lay.ticket_at, lay.ticket, &tik));
        CHECK_EQ(tmd.tid, kAm);
        CHECK_EQ(tik.tid, kAm);
        CHECK_EQ(tmd.type & 1, 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        CHECK(g.state == GuestState::Installed && g.game == kGame && g.child == kChild && g.child_version == synth::Opts().version);
        CHECK(g.composer == kComposerVersion && g.variant == kHide && g.language == 1 && g.where == 'S' && g.game_version == 0x10 && !g.prev_installed);
        CHECK(std::memcmp(g.product, b.made.plain.data() + 0x150, 15) == 0);
        CHECK(g.transfer != 0 && g.required == b.made.cia.size());
        CHECK(IsOwnedGuestTid(kPrepared));
        CHECK(b.con.folders.size() == 1 && b.con.folders[0].first == kPrepared && b.con.folders[0].second); // (DPAPP-2j, M3c1: the process's id, the key)
        CHECK_EQ(FileSize(BlockPath(kPrepared)), azl::dlp_children::kBlockBytes);
        azl::dlp_children::Index index;
        const std::string text = Get(IndexPath());
        CHECK(azl::dlp_children::ParseIndex(text.data(), text.size(), &index) && index.count == 1 && index.rows[0].prepared == kPrepared &&
              index.rows[0].child == kChild);
        const std::vector<ChildSeen> cache = LoadChildrenCache();
        CHECK(cache.size() == 1 && cache[0].has_child && cache[0].child == kChild && cache[0].game_version == 0x10);
        // installed hidden: the meta block's SMDH has bit 0 off
        CHECK_EQ(LoadLe32(b.con.installed.data() + lay.meta_at + 0x400 + 0x2028) & 1, 0);
        // (the build 2e) HOME's icon read once, for the prepared title, after AM's commit and its version, before the record and the index
        CHECK(b.con.home_icons.size() == 1 && b.con.home_icons[0] == kPrepared);
        CHECK(b.con.StepAt("title version") >= 0 && b.con.StepAt("title version") < b.con.StepAt("home icon"));
        CHECK(b.con.StepAt("home icon") < b.con.StepAt("plugin folder") && b.con.StepAt("home icon") < b.con.StepAt("record and index"));

        // again over the Installed child: AM keeps the old one until the Finish (never deleted first); a Begin that fails leaves the record as it was
        b.con.begin_fail = true;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrInstall);
        Guest after;
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.game_version == 0x10 && IsOwnedGuestTid(kPrepared));
        CHECK_EQ(b.con.Count("delete"), 0);
        CHECK(!IsFile(PartPath(kPrepared)));
        CHECK(b.con.titles.count(kAm) == 1);
        // ... an install write that fails: cancelled (THAT title), the record as it was
        b.con.begin_fail = false;
        b.con.install_fail_at = b.con.installs + 3;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrInstall);
        CHECK(b.con.Count("cancel 1") == 1 && b.con.Count("delete") == 0);
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.game_version == 0x10);
        CHECK(!IsFile(PartPath(kPrepared)));
        CHECK_EQ(b.con.home_icons.size(), 1); // (no install, no icon read: only after AM's commit)
        // ... a Finish that fails
        b.con.install_fail_at = -1;
        b.con.finish_fail = true;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrInstall);
        CHECK(b.con.Count("cancel 1") == 1 && b.con.Count("delete") == 0);
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.game_version == 0x10);
        CHECK_EQ(b.con.home_icons.size(), 1); // (a Finish that fails: no icon read)
        b.con.finish_fail = false;
        // ... the child made again is OLDER than ours installed (the card of 1.0 after the eShop copy of 1.1): AM refuses it at the Finish, and only
        // then our old child is deleted and the install made once more (critic 1.6). A delete that fails: the old child and the record as they were
        const u16 made_version = synth::Opts().version;
        const u16 higher = static_cast<u16>(made_version + 0x10);
        const std::string ours = b.con.titles[kAm].second;
        b.con.titles[kAm].first = higher;
        b.con.delete_fail = true;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrInstall);
        CHECK(b.con.Count("finish") == 1 && b.con.Count("delete") == 1 && b.con.Count("begin") == 1 && b.con.titles[kAm].first == higher);
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.game_version == 0x10 && !IsFile(PartPath(kPrepared)));
        b.con.delete_fail = false;
        // ... a Finish that fails again after the delete: no child left, the record of before (its title gone: the next look prepares it again)
        b.con.finish_fail = true;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrInstall);
        CHECK(b.con.Count("finish") == 2 && b.con.Count("delete") == 1 && b.con.Count("begin") == 2 && b.con.titles.count(kAm) == 0);
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.game_version == 0x10 && IsOwnedGuestTid(kPrepared));
        CHECK(!IsFile(PartPath(kPrepared)));
        b.con.finish_fail = false;
        // ... and when AM takes it: the older child, deleted only after the first Finish
        b.con.titles[kAm] = {higher, ours};
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), 0);
        CHECK(b.con.Count("finish") == 2 && b.con.Count("delete") == 1 && b.con.Count("begin") == 2 && b.con.titles[kAm].first == made_version);
        CHECK(b.con.calls.size() > 4 && b.con.calls[1] == "begin" && b.con.calls[2] == "finish" && b.con.calls[3] == "cancel 1" && b.con.calls[4] == "delete");
        CHECK(FindGuest(kPrepared, &after) && after.state == GuestState::Installed && after.child_version == made_version && after.game_version == 0x11);
        CHECK(!IsFile(PartPath(kPrepared)));
        // ... a title at the id with ANOTHER product code: never installed over
        b.con.titles[kAm].second = "CTR-P-OTHR";
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, &note), kErrIdTaken);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("open") == 0);
    }
    // a composition that fails (a damaged content): AM never asked, no work file, no record
    Clean();
    {
        Bench b;
        b.con.romfs[b.at + b.made.content_at + b.made.romfs_at + 0x10] ^= 0x40; // (a byte of the child's RomFS: only its SHA-256 can tell)
        JobNote note;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, &note), kErrSourceHash);
        CHECK_EQ(b.con.Count("begin"), 0);
        CHECK(!IsFile(PartPath(kPrepared)) && !IsFile(GuestPath(kPrepared)) && !IsOwnedGuestTid(kPrepared));
    }
    // a stop in the middle (the app ends): nothing left
    Clean();
    {
        Bench b;
        b.con.stop = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCancel);
        CHECK(!IsFile(PartPath(kPrepared)) && !IsFile(GuestPath(kPrepared)) && b.con.Count("begin") == 0);
    }
    // (the review of the build 2e) a Finish AM takes but does not list after it: the check of the install fails, and HOME's icon is never read.
    // (DPAPP-2j: the hidden form not on the SD card after a Finish AM answered 0 is no refusal of the form - kErrInstallCheck, no fallback)
    Clean();
    {
        Bench b;
        b.con.finish_forgets = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstallCheck);
        CHECK(b.con.Count("finish") == 1 && b.con.StepAt("hidden form not taken") < 0 && b.con.StepAt("not on the SD card: where AM put it is not looked at") >= 0);
        CHECK(b.con.home_icons.empty()); // (AM does not list the title: no icon read)
    }

    // no room, AM silent, a title that is not ours, no keys: nothing written
    Clean();
    {
        Bench b;
        b.con.free = 2 * b.made.cia.size();
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrNoSpace);
        CHECK(!IsFile(GuestPath(kPrepared)) && b.con.Count("begin") == 0);
        b.con.free = 1ull << 34;
        b.con.am_silent = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstall);
        CHECK(!IsFile(GuestPath(kPrepared)) && b.con.Count("open") == 1);
        b.con.am_silent = false;
        b.con.titles[kPrepared] = {7, "CTR-P-HOME"};
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrIdTaken);
        CHECK(!IsFile(GuestPath(kPrepared)) && b.con.Count("delete") == 0 && b.con.Count("begin") == 0);
        b.con.titles.clear();
        RunIn no_keys = b.in;
        no_keys.keys = nullptr;
        CHECK_EQ(RunJob(Job(Act::Prepare), no_keys, b.con, nullptr), kErrNoBoot9);
        CHECK(!IsFile(GuestPath(kPrepared)));
        b.con.open_rc = kErrNoMovable;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrNoMovable);
        CHECK(!IsFile(GuestPath(kPrepared)));
    }
    // AM does not confirm the version; the folder fails: the record is Installed all the same (the next look repairs or prepares again)
    Clean();
    {
        Bench b;
        b.con.lie_version = 3;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstallCheck);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed);
        Clean();
        Bench c;
        c.con.folder_fail = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), c.in, c.con, nullptr), kErrPlugin);
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && IsFile(BlockPath(kPrepared)));
    }
    // a child whose package is named after another id: found by the container's first file
    Clean();
    {
        synth::Opts o;
        o.child = 0x00040001000F8300ull;
        o.partition = 0x00040001000F8377ull;
        Bench b(o, "00040001000F8300.cia");
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.child == 0x00040001000F8300ull && g.prepared == kPrepared);
    }
}

// (the build 2e slot25) a child of method 1. Without boot.firm: 308, the line of the log with the case after "compose", the record taken away (Undo),
// nothing asked of AM, and NOTHING in children.cache (a Prepare that failed is not remembered: A on the row tries again, with boot.firm there now).
// With the made-up boot.firm: the child is made and the line says where the KeyX was (an offset). A child of method 0 writes no line.
struct FirmBytes {
    const Bytes* bytes;
    static bool Get(void* ctx, const u8** data, std::size_t* size) {
        const Bytes* b = static_cast<FirmBytes*>(ctx)->bytes;
        *data = b->data();
        *size = b->size();
        return true;
    }
};

static void TestSlot25() {
    synth::Opts o;
    o.method = 1;
    o.rng = 21;
    Clean();
    {
        Bench b(o);
        JobNote note;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, &note), kErrChildCrypto);
        CHECK(b.con.notes.size() == 1 && b.con.notes[0] == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 why Method "
                                                           "(0x18B=01: slot 0x25) step firm missing, 0 candidates, 0 ms");
        CHECK(b.con.StepAt("compose") >= 0 && b.con.StepAt("compose") + 1 == b.con.StepAt("note"));
        Guest g;
        CHECK(!FindGuest(kPrepared, &g));
        CHECK_EQ(b.con.Count("begin"), 0);
        CHECK(FindChildFailure(LoadChildrenCache(), kGame, 0x10, 0) == nullptr);
    }
    {
        Bench b(o);
        const synth::Firm firm = synth::MakeFirm(b.made.keys.slot25_x, synth::FirmOpts());
        FirmBytes fb{&firm.bytes};
        b.in.firm = &FirmBytes::Get;
        b.in.fctx = &fb;
        JobNote note;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, &note), 0);
        CHECK(b.con.notes.size() == 1 &&
              b.con.notes[0] == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 slot 0x25 key found at 0x00005200, 5121 candidates, 0 ms");
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && g.composer == kComposerVersion);
        CiaLayout lay;
        CHECK(ReadLayout(b.con.installed.data(), b.con.installed.size(), &lay) && b.con.installed[lay.content_at + 0x18B] == 0 &&
              (b.con.installed[lay.content_at + 0x18F] & 0x04) != 0);  // (NoCrypto, method 0: the form of the build 2e)
    }
    // (the review of DPAPP-2e slot25) the KeyX found, then the child fails for another reason (a byte of the RomFS past its superblock: 310) -
    // the line is written all the same: its offset, candidates and time are what a first console test needs
    Clean();
    {
        Bench b(o);
        const synth::Firm firm = synth::MakeFirm(b.made.keys.slot25_x, synth::FirmOpts());
        FirmBytes fb{&firm.bytes};
        b.in.firm = &FirmBytes::Get;
        b.in.fctx = &fb;
        b.con.romfs[b.at + b.made.content_at + b.made.romfs_at + 0x400] ^= 0x40;
        JobNote note;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, &note), kErrSourceHash);
        CHECK(b.con.notes.size() == 1 &&
              b.con.notes[0] == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 slot 0x25 key found at 0x00005200, 5121 candidates, 0 ms");
        CHECK_EQ(b.con.Count("begin"), 0);
    }
    Clean();
    {
        Bench b;
        JobNote note;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, &note), 0);
        CHECK(b.con.notes.empty());
    }
    Clean();
}

static void TestRemoveDisown() {
    Clean();
    Bench b;
    CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
    // a title that cannot be deleted: the record stays, as Removing
    b.con.delete_fail = true;
    b.con.folders.clear();
    CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), kErrInstall);
    Guest g;
    CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Removing && IsOwnedGuestTid(kPrepared));
    CHECK(b.con.folders.size() == 2 && !b.con.folders[0].second && !b.con.folders[1].second && b.con.folders[0].first == kM3c1 && b.con.folders[1].first == kPrepared);
    azl::dlp_children::Index index;
    std::string text = Get(IndexPath());
    CHECK(azl::dlp_children::ParseIndex(text.data(), text.size(), &index) && index.count == 0); // the row went first
    // then it goes: title, block, record last, registry
    b.con.delete_fail = false;
    b.con.calls.clear();
    CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
    CHECK_EQ(b.con.Count("delete"), 1);
    CHECK(b.con.titles.count(kAm) == 0 && !IsFile(GuestPath(kPrepared)) && !IsFile(BlockPath(kPrepared)) && !IsOwnedGuestTid(kPrepared));
    // a title that is not ours any more (another product code) is never deleted
    Clean();
    Bench c;
    CHECK_EQ(RunJob(Job(Act::Prepare), c.in, c.con, nullptr), 0);
    c.con.titles[kAm].second = "CTR-P-OTHR";
    c.con.calls.clear();
    CHECK_EQ(RunJob(Job(Act::Remove), c.in, c.con, nullptr), 0);
    CHECK(c.con.Count("delete") == 0 && c.con.titles.count(kAm) == 1 && !IsFile(GuestPath(kPrepared)));
    // Disown: AM is not asked anything
    Clean();
    Bench d;
    CHECK_EQ(RunJob(Job(Act::Prepare), d.in, d.con, nullptr), 0);
    d.con.calls.clear();
    d.con.am_silent = true; // (it would say nothing anyway: Disown does not ask)
    d.con.folders.clear();
    CHECK_EQ(RunJob(Job(Act::Disown), d.in, d.con, nullptr), 0);
    CHECK(d.con.calls.empty() && d.con.titles.count(kAm) == 1);
    CHECK(d.con.folders.size() == 2 && !d.con.folders[0].second && !d.con.folders[1].second); // (the build 2h: both folders)
    CHECK(!IsFile(GuestPath(kPrepared)) && !IsFile(BlockPath(kPrepared)) && !IsOwnedGuestTid(kPrepared));
    // ... with a folder that does not go: the record stays (Removing), the next look disowns again
    Clean();
    Bench e;
    CHECK_EQ(RunJob(Job(Act::Prepare), e.in, e.con, nullptr), 0);
    e.con.folder_fail = true;
    CHECK_EQ(RunJob(Job(Act::Disown), e.in, e.con, nullptr), kErrPlugin);
    CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Removing);
    // no record: nothing
    Clean();
    Bench f;
    CHECK_EQ(RunJob(Job(Act::Remove), f.in, f.con, nullptr), 0);
    CHECK(f.con.calls.empty());
}

static void TestLooks() {
    // Probe: the facts in children.cache; a game with no child is written as such
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Probe, 0x20, 0x30), b.in, b.con, nullptr), 0);
        std::vector<ChildSeen> cache = LoadChildrenCache();
        const ChildSeen* s = FindChildSeen(cache, kGame, 0x20, 0x30);
        CHECK(s != nullptr && s->has_child && s->child == kChild && s->required == b.made.cia.size() && std::memcmp(s->product, b.made.plain.data() + 0x150, 15) == 0);
        CHECK(!IsFile(GuestPath(kPrepared)) && b.con.Count("begin") == 0);
        Bench other(synth::Opts(), "notachildpackage.bin"); // (a content 2 with no package of a child in it)
        CHECK_EQ(RunJob(Job(Act::Probe, 0x21), other.in, other.con, nullptr), 0);
        cache = LoadChildrenCache();
        s = FindChildSeen(cache, kGame, 0x21, 0);
        CHECK(s != nullptr && !s->has_child && cache.size() == 1);
        // content 2 that is not a RomFS: from the system, a game with no child (written); through our keys, maybe the layer taken off wrong (never written)
        Bench junk;
        junk.con.romfs.assign(0x400, 0x5A);
        CHECK_EQ(RunJob(Job(Act::Probe, 0x24), junk.in, junk.con, nullptr), 0);
        cache = LoadChildrenCache();
        CHECK(FindChildSeen(cache, kGame, 0x24, 0) != nullptr && !FindChildSeen(cache, kGame, 0x24, 0)->has_child);
        junk.con.route = Route::Keyed;
        CHECK_EQ(RunJob(Job(Act::Probe, 0x25), junk.in, junk.con, nullptr), kErrNcch);
        cache = LoadChildrenCache();
        CHECK(FindChildSeen(cache, kGame, 0x25, 0) == nullptr);
        CHECK(FindChildFailure(cache, kGame, 0x25, 0) != nullptr && FindChildFailure(cache, kGame, 0x25, 0)->error == kErrNcch); // (spec 9: remembered as a failure)
        b.con.open_rc = kErrNoChild; // (route K: no content 2)
        CHECK_EQ(RunJob(Job(Act::Probe, 0x22), b.in, b.con, nullptr), 0);
        cache = LoadChildrenCache();
        CHECK(FindChildSeen(cache, kGame, 0x22, 0) != nullptr && !FindChildSeen(cache, kGame, 0x22, 0)->has_child);
        b.con.open_rc = kErrNoMovable; // needs movable.sed: nothing is written, the next look asks again
        CHECK_EQ(RunJob(Job(Act::Probe, 0x23), b.in, b.con, nullptr), kErrNoMovable);
        cache = LoadChildrenCache();
        CHECK(FindChildSeen(cache, kGame, 0x23, 0) == nullptr && FindChildSeen(cache, kGame, 0x22, 0) != nullptr);
        CHECK(FindChildFailure(cache, kGame, 0x23, 0) == nullptr); // (not the game's: not remembered as a failure either)
        // (1.1.4, spec 9 point 5) a failure that is the game's is remembered with the stamp of the run's boot9.bin: another boot9.bin reads it again
        b.in.keys_stamp = 0x1234u;
        b.con.open_rc = kErrNoSource; // (a game of the SD card whose content could not be read)
        CHECK_EQ(RunJob(Job(Act::Probe, 0x26), b.in, b.con, nullptr), kErrNoSource);
        cache = LoadChildrenCache();
        const ChildSeen* failed = FindChildFailure(cache, kGame, 0x26, 0);
        CHECK(failed != nullptr && failed->error == kErrNoSource && failed->keys == 0x1234u && FindChildSeen(cache, kGame, 0x26, 0) == nullptr);
        CHECK(cache.size() == 1); // (in place of what was known of the game)
        b.con.open_rc = kErrNoMemory; // (no memory: not the game's)
        CHECK_EQ(RunJob(Job(Act::Probe, 0x27), b.in, b.con, nullptr), kErrNoMemory);
        cache = LoadChildrenCache();
        CHECK(FindChildFailure(cache, kGame, 0x27, 0) == nullptr && FindChildFailure(cache, kGame, 0x26, 0) != nullptr);
        // without boot9.bin: route F alone. "No child" is still known; a child it finds waits for the keys (kErrNoBoot9: written as such with the
        // stamp of no boot9.bin, so that Games shows the game and the next start does not read it again - the review of spec 9; not a failure)
        RunIn no_keys = b.in;
        no_keys.keys = nullptr;
        no_keys.keys_stamp = 0; // (KeysStamp without boot9.bin)
        b.con.open_rc = kErrNoChild;
        CHECK_EQ(RunJob(Job(Act::Probe, 0x28), no_keys, b.con, nullptr), 0);
        cache = LoadChildrenCache();
        CHECK(FindChildSeen(cache, kGame, 0x28, 0) != nullptr && !FindChildSeen(cache, kGame, 0x28, 0)->has_child);
        b.con.open_rc = 0;
        CHECK_EQ(RunJob(Job(Act::Probe, 0x29), no_keys, b.con, nullptr), kErrNoBoot9);
        cache = LoadChildrenCache();
        const ChildSeen* waits = FindChildFailure(cache, kGame, 0x29, 0);
        CHECK(FindChildSeen(cache, kGame, 0x29, 0) == nullptr && waits != nullptr && waits->error == kErrNoBoot9 && waits->keys == 0u && !b.con.open);
        CHECK(waits != nullptr && !ToRead(true, false, nullptr, waits, 0) && ToRead(true, false, nullptr, waits, 0x1234u)); // (when boot9.bin comes, not at every start)
        b.con.open_rc = kErrNoBoot9; // (route F refused, route K needs the keys)
        CHECK_EQ(RunJob(Job(Act::Probe, 0x2A), no_keys, b.con, nullptr), kErrNoBoot9);
        CHECK(FindChildFailure(LoadChildrenCache(), kGame, 0x2A, 0) == nullptr);
    }
    // Check: the same child is only recorded at the new versions; another child is made again by the next look
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Planned j = Job(Act::Check, 0x40, 0x50);
        j.card = true;
        JobNote note;
        CHECK_EQ(RunJob(j, b.in, b.con, &note), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.game_version == 0x40 && g.update_version == 0x50 && g.where == 'C' && g.composer == kComposerVersion);
        CHECK(note.card_checked && !note.remade);
        synth::Opts o;
        o.version = 0x0420;
        Bench newer(o);
        CHECK_EQ(RunJob(Job(Act::Check, 0x41), newer.in, newer.con, &note), 0);
        CHECK(FindGuest(kPrepared, &g) && g.composer == 0 && g.game_version == 0x40 && note.remade);
        Seen s;
        s.game = kGame;
        s.on = s.present = s.keys = true;
        s.guest = &g;
        s.prepared_installed = true;
        s.prepared_version = g.child_version;
        s.game_version = 0x40;
        CHECK(Decide(s) == Act::Prepare); // (the next look prepares the new child)
        CHECK_EQ(newer.con.Count("begin"), 0);
    }
    // Adopt: the title at the id IS the child (product code, version): its record again, made again by the next look; otherwise never taken
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Probe), b.in, b.con, nullptr), 0);
        const ChildSeen s = LoadChildrenCache()[0];
        b.con.titles[kPrepared] = {s.child_version, std::string(s.product, 15)};
        b.con.titles[kPrepared].second = "CTR-P-NOPE";
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), kErrIdTaken);
        CHECK(!IsFile(GuestPath(kPrepared)));
        b.con.titles[kPrepared] = {static_cast<u16>(s.child_version + 1), std::string(s.product, 15)};
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), kErrIdTaken);
        b.con.titles[kPrepared] = {s.child_version, std::string(s.product, 15)};
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && g.composer == 0 && g.child == kChild && IsOwnedGuestTid(kPrepared));
        CHECK(b.con.Count("delete") == 0 && b.con.Count("begin") == 0);
    }
    // (the review of the final review's fixes of 1.1.4) a card swapped or pulled after the look planned its job: neither its "not found" nor its
    // child is written under the game - no source, tried again by the next look; the game's card still in the slot, or a game of the SD card: as before
    Clean();
    {
        Bench b;
        Planned j = Job(Act::Probe, 0, 0x30);
        j.card = true;
        b.con.open_rc = kErrNoChild; // (FS's "not found" on the card's route F)
        b.con.card_holds = false;
        CHECK_EQ(RunJob(j, b.in, b.con, nullptr), kErrNoSource);
        CHECK(LoadChildrenCache().empty());
        b.con.open_rc = 0; // (a content 2 FS opened: the card's that is in the slot now)
        CHECK_EQ(RunJob(j, b.in, b.con, nullptr), kErrNoSource);
        CHECK(LoadChildrenCache().empty() && !b.con.open);
        JobNote note;
        j.act = Act::Prepare;
        CHECK_EQ(RunJob(j, b.in, b.con, &note), kErrNoSource);
        CHECK(b.con.Count("begin") == 0 && !IsFile(GuestPath(kPrepared)) && !note.card_checked);
        CHECK_EQ(b.con.card_reads, 3);
        Planned sd = Job(Act::Probe, 0x20, 0x30); // (a game of the SD card: the slot is never asked)
        CHECK_EQ(RunJob(sd, b.in, b.con, nullptr), 0);
        CHECK_EQ(b.con.card_reads, 3);
        b.con.card_holds = true;
        b.con.open_rc = kErrNoChild;
        j.act = Act::Probe;
        CHECK_EQ(RunJob(j, b.in, b.con, nullptr), 0);
        const std::vector<ChildSeen> cache = LoadChildrenCache();
        const ChildSeen* none = FindChildSeen(cache, kGame, 0, 0x30);
        CHECK(none != nullptr && !none->has_child && cache.size() == 1);
        CHECK_EQ(b.con.card_reads, 4);
    }
    // (the review of spec 9) a card taken out WHILE its content is read: its "no child" or its child that does not read is no source, never written
    Clean();
    {
        Bench nochild(synth::Opts(), "notachildpackage.bin");
        Planned j = Job(Act::Probe, 0, 0x31);
        j.card = true;
        nochild.con.card_gone_after = 1; // (the slot holds the game at the open, not after)
        CHECK_EQ(RunJob(j, nochild.in, nochild.con, nullptr), kErrNoSource);
        CHECK(LoadChildrenCache().empty() && !nochild.con.open && nochild.con.card_reads == 2);
        synth::Opts o;
        o.title_key = true;
        Bench locked(o);
        locked.keys.common_y0[3] ^= 1; // (its child's facts do not read: kErrTitleKey, the game's while the card stays)
        locked.con.card_gone_after = 1;
        CHECK_EQ(RunJob(j, locked.in, locked.con, nullptr), kErrNoSource);
        CHECK(LoadChildrenCache().empty() && locked.con.card_reads == 2);
        locked.con.card_gone_after = -1;
        CHECK_EQ(RunJob(j, locked.in, locked.con, nullptr), kErrTitleKey);
        const std::vector<ChildSeen> cache = LoadChildrenCache();
        CHECK(FindChildFailure(cache, kGame, 0, 0x31) != nullptr && FindChildFailure(cache, kGame, 0, 0x31)->error == kErrTitleKey);
    }
    // Repair: the folder, the block, where the game is, the language
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        std::remove(BlockPath(kPrepared).c_str());
        b.con.folders.clear();
        b.con.calls.clear();
        RunIn other = b.in;
        other.language = 2;
        Planned j = Job(Act::Repair);
        j.card = true;
        CHECK_EQ(RunJob(j, other, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.language == 2 && g.where == 'C' && IsFile(BlockPath(kPrepared)));
        CHECK(b.con.folders.size() == 1 && b.con.folders[0].second);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("delete") == 0);
    }
    CHECK_EQ(ChildLowOfName("00040001000f8200.cia"), 0x000F8200u);
    CHECK_EQ(ChildLowOfName("00040001000F83AB.CIA"), 0x000F83ABu);
    CHECK_EQ(ChildLowOfName("00040000000f8200.cia"), 0u);
    CHECK_EQ(ChildLowOfName("00040001000f82.cia"), 0u);
    CHECK_EQ(ChildLowOfName("00040001000g8200.cia"), 0u);
    CHECK_EQ(ChildLowOfName(""), 0u);
}

// (the build 2f, 2026-11-19) the hidden form (M3c1): the two ids of the CIA, the migration of a child of the old form, the system that does not take
// the hidden form (D2: the fallback, kErrShownInHome, no loop), the memo of the session, the removal and the adoption of either form
static void TestForms() {
    // a child made now: the hidden form - the ticket and the TMD name the id of AM, the record says form dlp, the registry knows it
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild);
        CHECK(b.con.titles.count(kAm) == 1 && b.con.titles.count(kPrepared) == 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.composer == kComposerVersion && FormOf(kPrepared) == kFormDlpChild && AmTitleOf(kPrepared) == kAm);
        CHECK(b.con.StepAt("form") >= 0 && b.con.StepAt("media after the install: SD") > b.con.StepAt("install finish"));
        CHECK(b.con.StepAt("media after the install: SD") < b.con.StepAt("title version") && b.con.home_icons.size() == 1 && b.con.home_icons[0] == kPrepared);
        CHECK(b.con.StepAt("hidden form not taken") < 0 && b.con.Count("delete") == 0);
        // the CIA: AM's id in the ticket, the TMD, 0x108 and 0x118; the same id in 0x3C8, 0x400 (the one Luma and the plugin read) and 0x800
        CiaLayout lay;
        TmdFacts tmd;
        TicketFacts tik;
        CHECK(ReadLayout(b.con.installed.data(), b.con.installed.size(), &lay) && ReadTmd(b.con.installed.data() + lay.tmd_at, lay.tmd, &tmd) &&
              ReadTicket(b.con.installed.data() + lay.ticket_at, lay.ticket, &tik));
        const u8* n = b.con.installed.data() + lay.content_at;
        CHECK(tmd.tid == kAm && tik.tid == kAm && LoadLe64(n + 0x108) == kAm && LoadLe64(n + 0x118) == kAm);
        // (DPAPP-2j) M3c1: the exheader's ids are the process's, the key - the plugin's folder and the one pm registers (freed by zombie_free.cpp)
        CHECK(LoadLe64(n + 0x3C8) == kPrepared && LoadLe64(n + 0x400) == kPrepared && LoadLe64(n + 0x800) == kPrepared);
        CHECK(b.con.folders.size() == 1 && b.con.folders[0].first == kPrepared && b.con.folders[0].second);
    }
    // the migration: a child of the old form (composer 1, visible in HOME - the 2i's fallbacks) is made again hidden; the old title goes only AFTER the
    // new one is there
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormApplication, 1);
        Guest old;
        CHECK(FindGuest(kPrepared, &old) && old.form == kFormApplication && b.con.titles.count(kPrepared) == 1 && b.con.titles.count(kAm) == 0);
        b.con.calls.clear();
        b.con.steps.clear();
        b.con.begin_forms.clear();
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild);
        CHECK(b.con.titles.count(kAm) == 1 && b.con.titles.count(kPrepared) == 0 && b.con.deleted_ids.size() == 1 && b.con.deleted_ids[0] == kPrepared);
        // (the review of the build 2f) while AM works the record says the old child (a cut goes back to it) and the registry the form there is
        CHECK(b.con.prev_other_at_begin && b.con.form_of_at_begin == kFormApplication);
        CHECK(b.con.StepAt("old form removed (HOME showed it)") > b.con.StepAt("install finish"));
        CHECK(b.con.StepAt("old form removed (HOME showed it)") > b.con.StepAt("media after the install: SD"));
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.composer == kComposerVersion && g.state == GuestState::Installed && g.game_version == 0x11);
        CHECK(IsOwnedGuestTid(kAm) && AmTitleOf(kPrepared) == kAm);
    }
    // ... and a child of the system form of the build 2h (composer 3: Azahar took it) - the same, the old title's line says which
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormSystem, 3);
        b.con.steps.clear();
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.titles.count(kAm) == 1 && b.con.titles.count(kM3c1) == 0 && b.con.deleted_ids.size() == 1 && b.con.deleted_ids[0] == kM3c1);
        CHECK(b.con.StepAt("old form removed (the system form of the build 2h)") > b.con.StepAt("install finish"));
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.composer == kComposerVersion);
        // its plugin's folder (the system id's) taken away after the new one (the key's)
        CHECK(b.con.StepAt("old plugin folder removed") > b.con.StepAt("plugin folder"));
    }
    // ... the old title of another product code (somebody else's at the prepared id): never deleted - refused before anything is made (K9.1)
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles[kPrepared] = {7, "CTR-N-HBRW"};
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrIdTaken);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("delete") == 0 && b.con.titles.count(kPrepared) == 1);
    }
    // the begin that fails is not a refusal of the form (the review of the build 2f: AM_StartCiaInstall reads nothing of the title): kErrInstall,
    // no memo, no fallback, no record
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        b.con.refuse_hidden_begin = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstall);
        CHECK(!memo.load() && b.con.StepAt("hidden form not taken") < 0 && b.con.begin_forms.size() == 1 && b.con.Count("delete") == 0);
        Guest g;
        CHECK(!FindGuest(kPrepared, &g) && b.con.titles.empty());
    }
    // (DPAPP-2j) the system does not take the hidden form - AM refuses a write or the Finish: NO fallback (no child in HOME, the user's decision),
    // kErrCannotHide, no memo, nothing of the refused id left (its ticket dropped), no record; AM's own Result in the log. A Finish that leaves it off
    // the SD card is no refusal (AM said 0): kErrInstallCheck
    for (int way = 1; way < 4; ++way) {
        Clean();
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        b.con.refuse_hidden_write = way == 1;
        b.con.refuse_hidden_finish = way == 2;
        b.con.hidden_not_listed = way == 3;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), way == 3 ? kErrInstallCheck : kErrCannotHide);
        CHECK(!memo.load() && b.con.StepAt("fallback: made again in the old form") < 0 && b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild);
        CHECK(b.con.Count("open") == 1 && b.con.titles.empty());
        CHECK(b.con.cancel_forms.size() >= 1 && b.con.cancel_forms[0] == kFormDlpChild); // (THAT pending title of the hidden form cancelled)
        if (way != 3) {
            CHECK(b.con.StepAt("hidden form not taken") >= 0 && b.con.tickets.count(kAm) == 0);
            CHECK(b.con.NoteHas("Result 0xC8A08025 (module 32, summary 5, description 37)") && b.con.NoteHas("AM refused it"));
        } else {
            CHECK(b.con.StepAt("not on the SD card: where AM put it is not looked at") >= 0 && b.con.StepAt("hidden form not taken") < 0);
        }
        Guest g;
        CHECK(!FindGuest(kPrepared, &g) && !IsFile(PartPath(kPrepared)) && !IsFile(BlockPath(kPrepared)));
        CHECK(std::string(Phrase(kErrCannotHide)).find("not installed") != std::string::npos && !CanTryAgain(kErrCannotHide));
        // ... and the next child tries the hidden form again (no memo with the fallback off): never put in HOME by another game's refusal
        b.con.calls.clear();
        b.con.begin_forms.clear();
        b.con.steps.clear();
        RunJob(Job(Act::Prepare, 0x12), b.in, b.con, nullptr);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild && b.con.StepAt("hidden form skipped: refused earlier in this session") < 0);
    }
    // the memo of a session where the hidden form was refused does nothing with the fallback off: the hidden form is tried
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{true};
        b.in.hidden_refused = &memo;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild && b.con.StepAt("hidden form skipped: refused earlier in this session") < 0);
    }
    // the migration refused: the old child (visible in HOME) stays as it was - never deleted before the new one is there, no fallback made
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormApplication, 1);
        b.con.refuse_hidden_finish = true;
        b.con.calls.clear();
        b.con.steps.clear();
        b.con.deleted_ids.clear();
        // (DPAPP-2j-rev) said as it is: the old guest works and HOME shows it (kErrShownInHome) - never "not installed"
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrShownInHome);
        CHECK(std::string(Phrase(kErrShownInHome)).find("not installed") == std::string::npos && std::string(Phrase(kErrShownInHome)).find("HOME shows it") != std::string::npos);
        CHECK(b.con.StepAt("fallback: the old form kept") < 0 && b.con.Count("open") == 1 && b.con.Count("begin") == 1);
        CHECK(b.con.titles.count(kPrepared) == 1 && b.con.titles.count(kAm) == 0 && std::find(b.con.deleted_ids.begin(), b.con.deleted_ids.end(), kPrepared) == b.con.deleted_ids.end());
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormApplication && g.composer == 1 && g.state == GuestState::Installed && FormOf(kPrepared) == kFormApplication);
    }
    // the fallback is off (DPAPP-2j): a refusal leaves no child in HOME (kErrCannotHide)
    CHECK(!kFallbackToApplicationForm);
    // a hidden child that AM took before: a Finish that fails over it is a fault of the moment (kErrInstall), never the fallback
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.refuse_hidden_finish = true;
        b.con.begin_forms.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrInstall);
        CHECK(b.con.begin_forms.size() == 1 && b.con.titles.count(kAm) == 1);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.game_version == 0x10);
    }
    // ... and a hidden child whose title was taken away by hand (the review of the build 2f: the fallback looked at prev_installed only, the memo at
    // the record): a write refused over it is a fault of the moment too - kErrInstall, no memo, no fallback, the record of the hidden form kept
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles.clear();
        b.con.refuse_hidden_write = true;
        b.con.begin_forms.clear();
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstall);
        CHECK(!memo.load() && b.con.StepAt("hidden form not taken") < 0 && b.con.begin_forms.size() == 1 && b.con.deleted_ids.empty());
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.state == GuestState::Installed && FormOf(kPrepared) == kFormDlpChild);
    }
    // a migration whose old title's product code does not read: the build 2h's R6 - a LISTED title whose code does not read is never deleted on the
    // record alone (only a Broken one; the fake lists Present titles): kept and said ("old form unverified: kept"), the new one made
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormApplication, 1); // (DPAPP-2j: a child of the old form, as the 2i left them - the fallback is off: made by hand)
        Guest old;
        CHECK(FindGuest(kPrepared, &old));
        old.composer = 1;
        CHECK(SaveGuest(old));
        b.con.titles[kPrepared].second = ""; // (listed, its code unread)
        b.in.hidden_refused = nullptr;
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.deleted_ids.empty() && b.con.titles.count(kPrepared) == 1 && b.con.titles.count(kAm) == 1 && b.con.StepAt("old form unverified: kept") >= 0);
    }
    // a stop in the middle of a migration: the old child is untouched, the record as it was (the old form)
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormApplication, 1); // (DPAPP-2j: a child of the old form, as the 2i left them - the fallback is off: made by hand)
        Guest old;
        CHECK(FindGuest(kPrepared, &old));
        old.composer = 1;
        CHECK(SaveGuest(old));
        memo.store(false);
        b.con.stop = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCancel);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormApplication && g.composer == 1 && g.state == GuestState::Installed && FormOf(kPrepared) == kFormApplication);
        CHECK(b.con.titles.count(kPrepared) == 1 && b.con.titles.count(kAm) == 0 && b.con.Count("delete") == 0);
    }
    // Remove: the record's form, and the other form's id when it is ours (the same product code); never one of another product
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles[kPrepared] = b.con.titles[kAm]; // (the half of a migration that was cut)
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        CHECK(b.con.deleted_ids.size() == 2 && b.con.deleted_ids[0] == kAm && b.con.deleted_ids[1] == kPrepared && b.con.titles.empty());
        CHECK(!IsFile(GuestPath(kPrepared)) && !IsOwnedGuestTid(kAm) && !IsOwnedGuestTid(kPrepared));
        Clean();
        Bench c;
        CHECK_EQ(RunJob(Job(Act::Prepare), c.in, c.con, nullptr), 0);
        c.con.titles[kPrepared] = {7, "CTR-N-HBRW"};
        c.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Remove), c.in, c.con, nullptr), 0);
        CHECK(c.con.deleted_ids.size() == 1 && c.con.deleted_ids[0] == kAm && c.con.titles.count(kPrepared) == 1 && c.con.StepAt("other form not ours: kept") >= 0);
    }
    // Repair: the other form's id of our child goes; Adopt: the hidden form first
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles[kPrepared] = b.con.titles[kAm];
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Repair), b.in, b.con, nullptr), 0);
        CHECK(b.con.deleted_ids.size() == 1 && b.con.deleted_ids[0] == kPrepared && b.con.titles.count(kAm) == 1 && b.con.StepAt("repair: other form removed") >= 0);
    }
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Probe), b.in, b.con, nullptr), 0);
        const ChildSeen s = LoadChildrenCache()[0];
        b.con.titles[kAm] = {s.child_version, std::string(s.product, 15)};
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.composer == 0 && IsOwnedGuestTid(kAm) && FormOf(kPrepared) == kFormDlpChild);
    }
}

// The copy of the plugin in a prepared title's folder (games.cpp, called by the thread of the copies): only EXACTLY the game's prepared title.
static void TestGuestFolder() {
    Clean();
    mkdir("sdmc:/3ds", 0777);
    const std::string src = "sdmc:/3ds/AzaharLink.3gx";
    if (FILE* f = std::fopen(src.c_str(), "wb")) {
        std::fputs("a plugin", f);
        std::fclose(f);
    }
    CHECK(setup::EnableGuestFolder(kGame, kGame, src.c_str()) == setup::EnableResult::CopyFailed);                // the game's own folder: never
    CHECK(setup::EnableGuestFolder(0x000400000F800100ull, 0x000400000F800100ull, src.c_str()) == setup::EnableResult::CopyFailed); // a homebrew of the band
    CHECK(setup::EnableGuestFolder(kPrepared, 0x0004000E000F8200ull, src.c_str()) == setup::EnableResult::CopyFailed); // not an application
    CHECK(setup::EnableGuestFolder(kPrepared + 0x100, kGame, src.c_str()) == setup::EnableResult::CopyFailed);   // another prepared id
    CHECK(!IsFile(setup::GamePluginPath(kGame)) && !IsFile(setup::GamePluginPath(kPrepared + 0x100)));
    CHECK(setup::EnableGuestFolder(kPrepared, kGame, src.c_str()) == setup::EnableResult::Ok);
    CHECK(Get(setup::GamePluginPath(kPrepared)) == "a plugin");
    CHECK(setup::DisableGuestFolder(kPrepared, kGame) && !IsFile(setup::GamePluginPath(kPrepared)));
}

// (1.1.4, spec 9 points 4, 5 and 7) the reading of the background, the failures kept, what a row of Download Play > Games shows, and "on" = a record
static void TestGames(const Guest& installed) {
    ChildSeen fact, failure;
    failure.error = kErrNcch;
    failure.keys = 7;
    CHECK(ToRead(true, false, nullptr, nullptr, 7));      // a game never read
    CHECK(!ToRead(false, false, nullptr, nullptr, 7));    // not a game that may have one (a homebrew, the system's Download Play)
    CHECK(!ToRead(true, true, nullptr, nullptr, 7));      // a record: its child is known
    CHECK(!ToRead(true, false, &fact, nullptr, 7));       // known at these versions
    CHECK(!ToRead(true, false, nullptr, &failure, 7));    // failed with this boot9.bin: not again and again
    CHECK(ToRead(true, false, nullptr, &failure, 8));     // boot9.bin came or changed: read again
    CHECK(ToRead(true, false, nullptr, &failure, 0) == (failure.keys != 0));
    CHECK(!FailureKept(0, false) && !FailureKept(kErrNoChild, false) && !FailureKept(kErrNoBoot9, false) && !FailureKept(kErrBoot9NoKeys, false));
    CHECK(!FailureKept(kErrNoMovable, false) && !FailureKept(kErrNoMemory, false) && !FailureKept(kErrCancel, false));
    CHECK(!FailureKept(kErrNoSource, true) && FailureKept(kErrNoSource, false)); // (a card taken out is not the game's)
    CHECK(FailureKept(kErrNcch, false) && FailureKept(kErrNotCia, true) && FailureKept(kErrTitleKey, false) && FailureKept(kErrContainerCrypto, false));
    Seen s;
    s.game = 0x0004000000001500ull;
    s.present = s.keys = true;
    CHECK(ShownOf(s, nullptr, Act::Probe) == Shown::Hidden);   // never read: only counted ("Checking your games")
    CHECK(ShownOf(s, &failure, Act::Probe) == Shown::NotRead); // the last look failed: grey, "not read"
    ChildSeen waits;
    waits.error = kErrNoBoot9;                                 // (route F found its child without boot9.bin)
    CHECK(ShownOf(s, &waits, Act::Nothing) == Shown::Off);     // it has one: Off, and A says that boot9.bin is needed (the review of spec 9)
    s.child = ChildFact::None;
    CHECK(ShownOf(s, nullptr, Act::Nothing) == Shown::Hidden); // no child: not listed
    s.child = ChildFact::Has;
    CHECK(ShownOf(s, nullptr, Act::Prepare) == Shown::Off);    // a child, no record: Off (whatever Enable Titles says)
    CHECK(Decide(s) == Act::Nothing);                          // ... and nothing is made of it until the user switches it on (spec 9 point 7)
    s.on = true;
    CHECK(Decide(s) == Act::Prepare);                          // switched on in Games: its child is prepared
    s.on = false;
    s.present = false;
    CHECK(ShownOf(s, nullptr, Act::Nothing) == Shown::Hidden);
    Guest rec = installed;
    s.guest = &rec;
    s.present = true;
    CHECK(ShownOf(s, nullptr, Act::Nothing) == Shown::On);
    CHECK(ShownOf(s, nullptr, Act::Check) == Shown::On);       // (the check runs quietly when the page Download Play opens)
    CHECK(ShownOf(s, &failure, Act::Nothing) == Shown::NotRead); // (the review of spec 9) the look at the game's new version failed: not On for ever
    CHECK(ShownOf(s, &failure, Act::Prepare) == Shown::ToUpdate);
    CHECK(ShownOf(s, nullptr, Act::Prepare) == Shown::ToUpdate);
    {
        Seen c; // the game was updated: a Check, once; one that failed with this boot9.bin is not made again by itself
        c.game = rec.game;
        c.on = c.present = c.keys = c.plugin_on = c.block_ok = true;
        c.guest = &rec;
        c.prepared_installed = true;
        c.prepared_version = rec.child_version;
        c.game_version = static_cast<u16>(rec.game_version + 1);
        c.update_version = rec.update_version;
        c.language = rec.language;
        CHECK(Decide(c) == Act::Check);
        c.look_failed = true;
        CHECK(Decide(c) == Act::Nothing);
    }
    s.present = false;
    CHECK(ShownOf(s, nullptr, Act::Nothing) == Shown::On);     // (its card is out: it stays)
    CHECK(ShownOf(s, &failure, Act::Nothing) == Shown::On);
    rec.state = GuestState::Preparing;
    CHECK(ShownOf(s, nullptr, Act::Nothing) == Shown::ToUpdate);
    rec.state = GuestState::Removing;
    CHECK(ShownOf(s, nullptr, Act::Remove) == Shown::Hidden);
}

static void TestReconcile() {
    std::vector<Guest> records(3);
    std::vector<Look> looks;
    const auto add = [&](u64 game, bool on, ChildFact child, const Guest* guest, bool installed) {
        Look l;
        l.seen.game = game;
        l.seen.on = on;
        l.seen.present = true;
        l.seen.keys = true;
        l.seen.child = child;
        l.seen.guest = guest;
        l.seen.prepared_installed = installed;
        l.seen.plugin_on = l.seen.block_ok = true;
        l.seen.language = 1;
        l.bytes = 1000;
        looks.push_back(l);
    };
    Guest a;
    a.game = 0x0004000000030700ull;
    a.prepared = azl::title_rules::PreparedOf(a.game);
    a.state = GuestState::Installed;
    a.composer = kComposerVersion;
    a.language = 1;
    add(0x0004000000001100ull, true, ChildFact::Unknown, nullptr, false); // Probe
    add(0x0004000000001200ull, true, ChildFact::Has, nullptr, false);     // Prepare
    add(a.game, false, ChildFact::Has, &a, true);                          // Remove (no longer a game that can have one)
    add(0x0004000000001300ull, true, ChildFact::None, nullptr, false);    // Nothing
    add(0x0004000000001400ull, true, ChildFact::Has, nullptr, true);      // Refuse (another title, no product)
    Plan p = Reconcile(looks, 0);
    CHECK(p.quiet.size() == 2 && p.quiet[0].act == Act::Remove && p.quiet[1].act == Act::Probe); // removals first
    CHECK(p.heavy.size() == 1 && p.heavy[0].game == 0x0004000000001200ull && p.heavy[0].bytes == 1000);
    CHECK(p.said.size() == 1 && p.said[0].act == Act::Refuse);
    // a record whose game the scan did not list: removed only when AM's list of the SD card does not hold it either; a game AM still lists (or a
    // list AM did not give) is one the scan did not recognise: its child stays
    const std::vector<u64> sd = {0x0004000000001100ull, a.game};
    CHECK(StillOnSd(&sd, a.game) && StillOnSd(nullptr, a.game) && !StillOnSd(&sd, 0x0004000000009900ull));
    looks.clear();
    add(a.game, true, ChildFact::Has, &a, true);
    looks[0].seen.present = false;
    looks[0].seen.still_listed = StillOnSd(&sd, a.game);
    p = Reconcile(looks, 1);
    CHECK(p.quiet.empty() && p.heavy.empty() && p.said.empty());
    looks[0].seen.still_listed = StillOnSd(&sd, 0x0004000000009900ull);
    p = Reconcile(looks, 1);
    CHECK(p.quiet.size() == 1 && p.quiet[0].act == Act::Remove);
    // 16 at most: with 15 records one more is given, the next is Full
    looks.clear();
    add(0x0004000000002100ull, true, ChildFact::Has, nullptr, false);
    add(0x0004000000002200ull, true, ChildFact::Has, nullptr, false);
    p = Reconcile(looks, kMaxGuests - 1);
    CHECK(p.heavy.size() == 1 && p.heavy[0].game == 0x0004000000002100ull && p.said.size() == 1 && p.said[0].act == Act::Full);
    p = Reconcile(looks, kMaxGuests);
    CHECK(p.heavy.empty() && p.said.size() == 2);
    // the time: Island Tour's child (30.9 MiB) about 2 min 40 s by the system, 3 min 25 s by the keys
    const u64 island = static_cast<u64>(30.9 * 1048576.0);
    CHECK(SecondsToPrepare(island, false) >= 155 && SecondsToPrepare(island, false) <= 165);
    CHECK(SecondsToPrepare(island, true) >= 200 && SecondsToPrepare(island, true) <= 210);
    CHECK(SecondsToPrepare(0, false) > SecondsToPrepare(island, false)); // (not known: 32 MiB)
    std::vector<Planned> heavy(2);
    heavy[0].bytes = heavy[1].bytes = static_cast<u32>(island);
    heavy[1].card = true;
    CHECK_EQ(MinutesFor(heavy), (SecondsToPrepare(island, true) + SecondsToPrepare(island, false) + 59) / 60);
    CHECK_EQ(MinutesFor({}), 0u);
    // the bar: never back, never whole before the end
    double last = -1.0;
    bool rising = true;
    for (int st = kStageRead; st <= kStageFinish; ++st) {
        for (u64 d = 0; d <= 10; ++d) {
            const double f = JobFraction(st, d, 10);
            rising = rising && f >= last && f <= 1.0;
            last = f;
        }
    }
    CHECK(rising);
    CHECK(RunFraction(1, 2, 1.0) < 1.0 && RunFraction(0, 2, 0.5) == 0.25 && RunFraction(0, 0, 0.5) == 0.0);
    TestGames(a);
}

// (the build 2h) the guard of the ids in the jobs: an id of the slot that is somebody else's - on the SD card, in the NAND, a ticket, the card, a
// process, a title begun - stops an install before ANYTHING is written (no record, no Begin); a cache that is not what the console says stops it too
// (kErrIdStale, the cache taken again); a change between the first look and AM stops it; a removal never deletes an id that is not ours; the migration
// from M3c1 (00040001, the builds 2f/2g) to the system form never touches somebody else's title and never installs the form M3c1.
static void TestIds2h() {
    // every source of the slot that is somebody else's: refused before anything (no record, no Begin, no open)
    struct Case {
        const char* what;
        std::function<void(Fake&)> put;
        int err;
    };
    const Case cases[] = {
        {"SD, the system id", [](Fake& f) { f.titles[kAm] = {7, "CTR-N-HBRW"}; }, kErrIdTaken},
        {"SD, the key", [](Fake& f) { f.titles[kPrepared] = {7, "CTR-N-HBRW"}; }, kErrIdTaken},
        {"SD, M3c1's id", [](Fake& f) { f.titles[kM3c1] = {7, "CTR-N-HBRW"}; }, kErrIdTaken},
        {"NAND, the system id", [](Fake& f) { f.nand.insert(kAm); }, kErrIdSystem},
        {"NAND, the key", [](Fake& f) { f.nand.insert(kPrepared); }, kErrIdSystem},
        {"NAND, a title begun", [](Fake& f) { f.pending_nand.insert(kAm); }, kErrIdSystem},
        {"a ticket of another title", [](Fake& f) { f.tickets.insert(kAm); }, kErrIdTicket},
        {"the game card", [](Fake& f) { f.card.insert(kAm); }, kErrIdTaken},
        {"a closed game still in memory (the key)", [](Fake& f) { f.processes.insert(kPrepared); }, kErrIdRunning},
        {"a process of the system id, another low byte", [](Fake& f) { f.processes.insert(kAm | 0x3F); }, kErrIdRunning},
        {"a title begun by nobody of ours", [](Fake& f) { f.pending_sd.insert(kAm); }, kErrIdTaken},
        {"a list of AM not read", [](Fake& f) { f.ids_fail_from = 0; }, kErrInstall},
    };
    for (const Case& c : cases) {
        Clean();
        Bench b;
        c.put(b.con);
        const RunResult rc = RunJob(Job(Act::Prepare), b.in, b.con, nullptr);
        CHECK_EQ(rc, c.err);
        Guest g;
        const bool clean = b.con.Count("begin") == 0 && b.con.Count("open") == 0 && b.con.Count("delete") == 0 && !FindGuest(kPrepared, &g) &&
                           !IsOwnedGuestTid(kPrepared) && b.con.titles.count(kAm) == (c.err == kErrIdTaken && std::string(c.what) == "SD, the system id" ? 1u : 0u);
        CHECK(clean);
        if (rc != c.err || !clean) {
            std::fprintf(stderr, "    (case: %s)\n", c.what);
        }
    }
    // the cache said something else than the console: stopped, nothing written, the cache taken again - and the next try goes on
    Clean();
    {
        Bench b;
        IdCensus cache;
        b.con.titles[kAm] = {7, "CTR-N-HBRW"};
        b.con.ReadIds(&cache); // (the look saw a title at the id ...)
        b.con.titles.erase(kAm); // (... which is gone since)
        b.in.ids = &cache;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrIdStale);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("open") == 0 && !IsFile(GuestPath(kPrepared)));
        CHECK(SlotIn(cache, kPrepared).mask[kFormDlpChild] == 0); // (the cache took what the console says)
        bool said = false;
        for (const std::string& n : b.con.id_notes) {
            said = said || (n.find("the cache said dlp sd") != std::string::npos && n.find("the console says dlp -") != std::string::npos); // (DPAPP-2j: M3c1 first)
        }
        CHECK(said);
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        CHECK(b.con.titles.count(kAm) == 1 && SlotIn(cache, kPrepared).mask[kFormDlpChild] == (SrcBit(kSrcSd) | SrcBit(kSrcTicket)));
    }
    // a change between the first guard and AM (a ticket of somebody else's comes while the child is composed): stopped before the Begin, the record
    // undone, nothing of AM written
    for (int with_cache = 0; with_cache < 2; ++with_cache) {
        Clean();
        Bench b;
        IdCensus cache;
        b.con.ReadIds(&cache);
        b.con.ids_reads = 0;
        if (with_cache) {
            b.in.ids = &cache;
        }
        b.con.on_read = [](Fake& f, int n) {
            if (n == 1) {
                f.tickets.insert(kAm); // (the second read: the guard right before the Begin)
            }
        };
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), with_cache ? kErrIdStale : kErrIdTicket);
        Guest g;
        CHECK(b.con.Count("begin") == 0 && b.con.Count("delete") == 0 && !FindGuest(kPrepared, &g) && !IsOwnedGuestTid(kPrepared) &&
              !IsFile(PartPath(kPrepared)) && b.con.tickets.count(kAm) == 1);
    }
    // the second guard (dlp_guest_am.cpp): an id that comes into the NAND between the run's guard and AM's call - refused by it, nothing installed
    Clean();
    {
        Bench b;
        b.con.on_read = [](Fake& f, int n) {
            if (n == 2) {
                f.nand.insert(kAm); // (the third read: the one of BeginInstall's own guard)
            }
        };
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstall);
        CHECK(b.con.guard_refusals == 1 && b.con.installed.empty() && b.con.titles.count(kAm) == 0);
    }
    // the system put the child in the NAND: never deleted there, no fallback, the record undone, said (kErrPutInNand)
    Clean();
    {
        Bench b;
        b.con.finish_to_nand = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrPutInNand);
        Guest g;
        CHECK(b.con.Count("begin") == 1 && b.con.Count("delete") == 0 && b.con.nand.count(kAm) == 1 && !FindGuest(kPrepared, &g));
        CHECK(b.con.StepAt("NAND after the install: PRESENT") >= 0 && b.con.StepAt("hidden form not taken") < 0);
        // ... and from then on the slot is the NAND's: every install refused before anything (R2)
        b.con.finish_to_nand = false;
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrIdSystem);
        CHECK(b.con.Count("begin") == 0);
    }
    // the ticket of the refused hidden form goes only when it was not there before (R7): a refusal leaves no ticket of ours behind (DPAPP-2j: and no
    // child, no record - the fallback is off)
    Clean();
    {
        Bench b;
        b.con.refuse_hidden_finish = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCannotHide);
        CHECK(b.con.tickets_dropped.size() == 1 && b.con.tickets_dropped[0] == kAm && b.con.tickets.count(kAm) == 0);
        CHECK(b.con.begin_forms.size() == 1 && b.con.titles.empty());
        Guest g;
        CHECK(!FindGuest(kPrepared, &g) && !IsOwnedGuestTid(kPrepared));
    }
    // a removal never deletes an id that is not ours: another product code, the NAND, a closed game still in memory - the record stays Removing
    for (int way = 0; way < 3; ++way) {
        Clean();
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.deleted_ids.clear();
        if (way == 0) {
            b.con.titles[kAm].second = "CTR-N-HBRW";
        } else if (way == 1) {
            b.con.nand.insert(kAm);
        } else {
            b.con.processes.insert(kAm);
        }
        const RunResult rc = RunJob(Job(Act::Remove), b.in, b.con, nullptr);
        CHECK(b.con.deleted_ids.empty() && b.con.titles.count(kAm) == 1);
        Guest g;
        if (way == 0) {
            CHECK(rc == 0 && b.con.StepAt("remove: not ours any more") >= 0 && !FindGuest(kPrepared, &g)); // (not ours any more: our record goes, the title stays)
        } else {
            CHECK(rc == (way == 1 ? kErrIdSystem : kErrIdRunning) && FindGuest(kPrepared, &g) && g.state == GuestState::Removing);
        }
    }
    // a record of the builds before 2h whose ticket is unknown, its title's code unread: the title is not deleted on the record alone (R6), nor its ticket
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        MakeOtherForm(b, kFormApplication, 1); // (DPAPP-2j: a child of the old form, as the 2i left them - the fallback is off: made by hand)
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        b.con.titles[kPrepared].second = ""; // (listed, its code unread)
        b.con.deleted_ids.clear();
        RunJob(Job(Act::Remove), b.in, b.con, nullptr);
        CHECK(b.con.deleted_ids.empty() && b.con.titles.count(kPrepared) == 1 && b.con.StepAt("remove: title unverified: kept") >= 0);
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Removing);
    }
    // (DPAPP-2j) the migration of a child of the system form (the build 2h, composer 2-3: only Azahar took it): made again in M3c1; the system form's
    // title goes only after the new one is listed, with its ticket (ours: listed with the record's code); the new folder (the key's) before the old one
    // (the system id's) goes; the system form is never installed
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        // the console of the 2g test: the child installed as M3c1, its record of form dlp, composer 2, no ticket key
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        b.con.tickets.erase(kAm);
        b.con.tickets.insert(kM3c1);
        g.form = kFormSystem;
        g.composer = 2;
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        LoadRegistry();
        CHECK(FormOf(kPrepared) == kFormSystem);
        b.con.begin_forms.clear();
        b.con.deleted_ids.clear();
        b.con.tickets_dropped.clear();
        b.con.folders.clear();
        b.con.steps.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild);
        CHECK(b.con.titles.count(kAm) == 1 && b.con.titles.count(kM3c1) == 0 && b.con.deleted_ids.size() == 1 && b.con.deleted_ids[0] == kM3c1);
        CHECK(b.con.tickets_dropped.size() == 1 && b.con.tickets_dropped[0] == kM3c1);
        CHECK(b.con.StepAt("old form removed (the system form of the build 2h)") > b.con.StepAt("media after the install: SD"));
        CHECK(b.con.folders.size() == 2 && b.con.folders[0].first == kPrepared && b.con.folders[0].second && b.con.folders[1].first == kM3c1 &&
              !b.con.folders[1].second); // (the key's folder first, the system id's taken away after it)
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && g.composer == kComposerVersion && FormOf(kPrepared) == kFormDlpChild && AmTitleOf(kPrepared) == kAm);
    }
    // ... a closed game of M3c1 still in memory (the 2g zombie): refused before anything, "restart the console"
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        g.form = kFormSystem;
        g.composer = 2;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.processes.insert(kPrepared);
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrIdRunning);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("delete") == 0 && b.con.titles.count(kM3c1) == 1);
        CHECK(std::string(Advice(kErrIdRunning)).find("Restart the console") == 0 && CanTryAgain(kErrIdRunning));
    }
    // ... somebody else's title at the key (00040000|low) next to our M3c1 child: never touched, nothing installed next to it
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        b.con.titles[kPrepared] = {3, "CTR-N-HBRW"};
        g.form = kFormSystem;
        g.composer = 2;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.calls.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrIdTaken);
        CHECK(b.con.Count("begin") == 0 && b.con.Count("delete") == 0 && b.con.titles.count(kPrepared) == 1 && b.con.titles.count(kM3c1) == 1);
    }
    // ... M3c1's title listed but its code unread (not Broken): kept and said, never deleted on the record alone (R6); the new one is made
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = {b.con.titles[kAm].first, ""};
        b.con.titles.erase(kAm);
        b.con.tickets.erase(kAm); // (the console of the 2g test had no ticket of the system id)
        g.form = kFormSystem;
        g.composer = 2;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.deleted_ids.empty() && b.con.titles.count(kM3c1) == 1 && b.con.titles.count(kAm) == 1 && b.con.StepAt("old form unverified: kept") >= 0);
    }
    // (DPAPP-2j) the system form is never installed: a refusal of the migration of a child of the system form leaves it as it was, no fallback
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        b.con.tickets.erase(kAm);
        g.form = kFormSystem;
        g.composer = 2;
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.refuse_hidden_finish = true;
        b.con.begin_forms.clear();
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrCannotHide);
        CHECK(b.con.begin_forms.size() == 1 && b.con.begin_forms[0] == kFormDlpChild);
        CHECK(b.con.titles.count(kPrepared) == 0 && b.con.titles.count(kM3c1) == 1 && b.con.StepAt("fallback: the old form kept") < 0);
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormSystem && g.composer == 2 && g.state == GuestState::Installed);
        for (const u8 f : b.con.begin_forms) {
            CHECK(f != kFormSystem);
        }
    }
    // InstallOverHigher: a stop that comes after the old child was deleted does not cut the second write (the console is never left without one)
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        const u16 made = b.con.titles[kAm].first;
        b.con.titles[kAm].first = static_cast<u16>(made + 0x10);
        b.con.calls.clear();
        b.con.on_read = [](Fake& f, int) {
            if (f.Count("delete") == 1) {
                f.stop = true; // (B pressed right after the old child went)
            }
        };
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.stop && b.con.titles.count(kAm) == 1 && b.con.titles[kAm].first == made && b.con.Count("finish") == 2);
        Guest g;
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && g.child_version == made);
    }
    // Adopt: never with an id of the slot in the NAND, in a process, on the card
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Probe), b.in, b.con, nullptr), 0);
        const ChildSeen s = LoadChildrenCache()[0];
        b.con.titles[kAm] = {s.child_version, std::string(s.product, 15)};
        b.con.processes.insert(kAm);
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), kErrIdRunning);
        Guest g;
        CHECK(!FindGuest(kPrepared, &g));
        b.con.processes.erase(kAm);
        b.con.card.insert(kPrepared);
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), kErrIdTaken);
        b.con.card.clear();
        CHECK_EQ(RunJob(Job(Act::Adopt), b.in, b.con, nullptr), 0);
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormDlpChild && !g.ticket_known);
    }
}

// (the review of the build 2h) R1: an install given up after its Begin leaves no ticket of ours behind (AM takes it with the CIA's first bytes and
// AM_CancelCIAInstall does not take it back): B in the middle of the write, a Finish that fails, a migration from M3c1 stopped - and the next try is
// never refused for "a ticket that is not ours" (326); a removal whose title had gone takes the ticket the record says is ours. R2: the record is
// Installed before the old form of a migration goes. R3: InstallOverHigher never deletes a title whose code does not read. R5: a NAND not read after
// the Finish is never "absent".
static void TestIds2hRev() {
    // B in the middle of the write of a first child: nothing left, the ticket too; then the child is made
    Clean();
    {
        Bench b;
        b.con.ticket_at_write = true;
        b.con.stop_at_install = 1;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCancel);
        Guest g;
        CHECK(b.con.Count("begin") == 1 && b.con.Count("cancel 1") == 1 && b.con.tickets.count(kAm) == 0 && b.con.titles.count(kAm) == 0);
        CHECK(b.con.tickets_dropped.size() == 1 && b.con.tickets_dropped[0] == kAm && !FindGuest(kPrepared, &g) && !IsFile(PartPath(kPrepared)));
        CHECK(b.con.StepAt("install given up: the ticket it left dropped") >= 0);
        b.con.stop = false;
        b.con.stop_at_install = -1;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0); // (never 326)
        CHECK(b.con.titles.count(kAm) == 1 && FindGuest(kPrepared, &g) && g.state == GuestState::Installed);
    }
    // a Finish that fails over a first child: the ticket it left goes; the next try is made
    Clean();
    {
        Bench b;
        b.con.ticket_at_write = true;
        b.con.finish_fail = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstall);
        CHECK(b.con.tickets.count(kAm) == 0 && !b.con.tickets_dropped.empty() && b.con.tickets_dropped[0] == kAm && b.con.StepAt("install not finished: the ticket it left dropped") >= 0);
        CHECK(b.con.tickets.count(kPrepared) == 0); // (the fallback's install, refused too: its ticket goes as well)
        b.con.finish_fail = false;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
    }
    // a ticket that was there before the run (our child's: an update of it) is never dropped when the run is given up
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.ticket_at_write = true;
        b.con.stop_at_install = b.con.installs + 1;
        b.con.tickets_dropped.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrCancel);
        Guest g;
        CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty() && b.con.titles.count(kAm) == 1);
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && g.game_version == 0x10);
    }
    // the migration of a child of M3c1 (the 2g console) stopped by B in the middle of the write: the old child and its record as they were, the
    // system id's ticket gone; the next Update is made (never 326)
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        b.con.tickets.erase(kAm);
        b.con.tickets.insert(kM3c1);
        g.form = kFormSystem;
        g.composer = 2;
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.ticket_at_write = true;
        b.con.stop_at_install = b.con.installs + 1;
        b.con.tickets_dropped.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrCancel);
        CHECK(b.con.tickets.count(kAm) == 0 && b.con.tickets.count(kM3c1) == 1 && b.con.titles.count(kM3c1) == 1 && b.con.titles.count(kAm) == 0);
        CHECK(b.con.tickets_dropped.size() == 1 && b.con.tickets_dropped[0] == kAm);
        CHECK(FindGuest(kPrepared, &g) && g.form == kFormSystem && g.state == GuestState::Installed && FormOf(kPrepared) == kFormSystem);
        b.con.stop = false;
        b.con.stop_at_install = -1;
        b.con.record_state_at_delete = -1;
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
        CHECK(b.con.titles.count(kAm) == 1 && b.con.titles.count(kM3c1) == 0);
        // (R2) when M3c1's title was deleted the record was already the new child's: Installed, in the system form
        CHECK(b.con.record_state_at_delete == static_cast<int>(GuestState::Installed) && b.con.record_form_at_delete == kFormDlpChild);
    }
    // a removal whose title is gone: the ticket the record says is ours goes with it; with the tickets' list not read the record stays (Removing)
    for (int unread = 0; unread < 2; ++unread) {
        Clean();
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles.erase(kAm); // (deleted by hand: its ticket stays)
        b.con.tickets_dropped.clear();
        if (unread) {
            b.con.ids_fail_from = b.con.ids_reads;
        }
        RunJob(Job(Act::Remove), b.in, b.con, nullptr);
        Guest g;
        if (unread) {
            CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty() && FindGuest(kPrepared, &g) && g.state == GuestState::Removing);
        } else {
            CHECK(b.con.tickets.count(kAm) == 0 && b.con.tickets_dropped.size() == 1 && !FindGuest(kPrepared, &g));
        }
    }
    // ... and a ticket the record does not say is ours (a record of the builds before) is never dropped with no title
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        b.con.titles.erase(kAm);
        b.con.tickets_dropped.clear();
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty() && !FindGuest(kPrepared, &g));
    }
    // R3: AM refuses the older child over ours, whose code does not read: never deleted to make room (R6), the old child kept
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles[kAm].first = static_cast<u16>(b.con.titles[kAm].first + 0x10);
        b.con.titles[kAm].second = ""; // (listed, its code unread)
        b.con.deleted_ids.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrInstall);
        CHECK(b.con.deleted_ids.empty() && b.con.titles.count(kAm) == 1 && b.con.Count("finish") >= 1);
    }
    // R5: the NAND not read after the Finish and the title not on the SD card: no fallback, nothing deleted, the record as it was
    Clean();
    {
        Bench b;
        b.con.nand_fails_after_finish = true;
        b.con.hidden_not_listed = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstallCheck);
        Guest g;
        CHECK(b.con.Count("begin") == 1 && b.con.Count("delete") == 0 && !FindGuest(kPrepared, &g));
        CHECK(b.con.StepAt("NAND after the install: NOT READ") >= 0 && b.con.StepAt("not on the SD card, the NAND not read: nothing more done") >= 0);
        CHECK(b.con.StepAt("hidden form not taken") < 0);
    }
    // ... read and absent: AM answered 0 and does not list it - no refusal of the form, no fallback (DPAPP-2j): kErrInstallCheck, its ticket dropped
    Clean();
    {
        Bench b;
        b.con.hidden_not_listed = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrInstallCheck);
        CHECK(b.con.StepAt("NAND after the install: absent") >= 0 && b.con.StepAt("hidden form not taken") < 0 && b.con.tickets.count(kAm) == 0);
        Guest g;
        CHECK(!FindGuest(kPrepared, &g));
    }
}

// (DPAPP-2h-sec) the review of the build 2h, second round: F2 the fallback says nothing of the old form's ticket before ITS guard; F4 a run given up
// whose ticket could not be dropped keeps a record that says it (Removing), the next Remove drops it and the slot works again; F6 the old form of a
// migration stays when the new record was not written; F12 a write refused with the card full is no refusal of the form; F5 (the fake as the
// console's DeletePrepared) a delete that failed keeps the ticket; F3 a record of another console never drops this console's ticket
static void TestSec2h() {
    // F2 (DPAPP-2j: the fallback is off) the hidden form refused at the Finish: the content is opened once, no record and no ticket left after it
    Clean();
    {
        Bench b;
        b.con.refuse_hidden_finish = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCannotHide);
        CHECK(b.con.record_at_open.size() == 1 && b.con.record_at_open[0].empty());
        Guest g;
        CHECK(!FindGuest(kPrepared, &g) && b.con.tickets.count(kAm) == 0);
    }
    // F4: B in the middle of a first child's write and the tickets' list not read: the ticket the run left cannot be dropped - the record stays,
    // Removing in the system form, the ticket said ours; the next Remove drops the ticket and the record; the next Enable is made (never 326)
    Clean();
    {
        Bench b;
        b.con.ticket_at_write = true;
        b.con.stop_at_install = 1;
        b.con.on_read = [](Fake& f, int) {
            if (f.stop) {
                f.ids_fail_from = 0; // (from the stop on: no list of the tickets)
            }
        };
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrCancel);
        Guest g;
        CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty());
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Removing && g.form == kFormDlpChild && TicketWordHere(g) && g.ticket_ours &&
              IsOwnedGuestTid(kPrepared));
        b.con.on_read = nullptr;
        b.con.ids_fail_from = -1;
        b.con.stop = false;
        b.con.stop_at_install = -1;
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        CHECK(b.con.tickets.count(kAm) == 0 && b.con.tickets_dropped.size() == 1 && !FindGuest(kPrepared, &g));
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        CHECK(b.con.titles.count(kAm) == 1 && FindGuest(kPrepared, &g) && g.state == GuestState::Installed);
    }
    // F4 (had): a run given up over our child of the same form whose record said nothing of the ticket (an adoption): the ticket its guard judged
    // ours is said in the record kept - the next run is not refused for it
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        g.ticket_known = false;
        g.ticket_ours = false;
        g.console = 0;
        CHECK(SaveGuest(g));
        b.con.stop_at_install = b.con.installs + 1;
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), kErrCancel);
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Installed && TicketWordHere(g) && g.ticket_ours && b.con.tickets.count(kAm) == 1);
        b.con.titles.erase(kAm); // (the title gone, its ticket left: the record's word lets the next run over it)
        b.con.stop = false;
        b.con.stop_at_install = -1;
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), 0);
    }
    // F6: the migration from M3c1 whose new record cannot be written: M3c1's title is kept (the start goes back to it)
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.titles.erase(kAm);
        b.con.tickets.erase(kAm);
        b.con.tickets.insert(kM3c1);
        g.form = kFormSystem;
        g.composer = 2;
        g.ticket_known = false;
        CHECK(SaveGuest(g));
        LoadRegistry();
        b.con.deleted_ids.clear();
        b.con.on_finish = [](Fake&) { fs::create_directories(GuestPath(kPrepared) + ".tmp"); }; // (the record cannot be written from AM's Finish on)
        const RunResult rc = RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr);
        fs::remove_all(GuestPath(kPrepared) + ".tmp");
        CHECK(rc == kErrTemp && b.con.titles.count(kM3c1) == 1 && b.con.titles.count(kAm) == 1);
        CHECK(std::find(b.con.deleted_ids.begin(), b.con.deleted_ids.end(), kM3c1) == b.con.deleted_ids.end());
        CHECK(b.con.StepAt("old form removed (the system form of the build 2h)") < 0);
        // the record on the card is still the migration's: the start keeps the new child (KeepNew: the old form is never installed again, DPAPP-2j),
        // never takes it away alone
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Preparing && g.prev_other && g.prev_other_form == kFormSystem && g.form == kFormDlpChild);
        CHECK(DecideAtStart(g, Presence::Present, b.con.titles[kAm].first, b.con.titles[kAm].second.c_str(), Presence::Present, b.con.titles[kM3c1].first,
                            b.con.titles[kM3c1].second.c_str()) == StartAct::KeepNew);
    }
    // F12: a write refused with the SD card full: kErrNoSpace, no memo, no fallback, nothing left
    Clean();
    {
        Bench b;
        std::atomic<bool> memo{false};
        b.in.hidden_refused = &memo;
        b.con.install_fail_at = 1;
        b.con.on_install_fail = [](Fake& f) {
            f.free = 1024; // (the card full when AM refuses the write)
            f.free_back_at_cancel = 1ull << 34; // (DPAPP-2h-sec, second pass: and the room back once the run cancelled - read before it)
        };
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), kErrNoSpace);
        Guest g;
        CHECK(!memo.load() && b.con.begin_forms.size() == 1 && b.con.StepAt("hidden form not taken") < 0 && !FindGuest(kPrepared, &g));
        CHECK(b.con.StepAt("install write: the SD card is full") >= 0);
        // ... with room left the same refusal of the write is AM's: kErrCannotHide (DPAPP-2j: no memo, no fallback)
        Clean();
        Bench b2;
        std::atomic<bool> memo2{false};
        b2.in.hidden_refused = &memo2;
        b2.con.refuse_hidden_write = true;
        CHECK_EQ(RunJob(Job(Act::Prepare), b2.in, b2.con, nullptr), kErrCannotHide);
        CHECK(!memo2.load() && b2.con.begin_forms.size() == 1);
        // ... and a Finish refused with the card full: no memo, no fallback
        Clean();
        Bench b3;
        std::atomic<bool> memo3{false};
        b3.in.hidden_refused = &memo3;
        b3.con.refuse_hidden_finish = true;
        b3.con.on_finish = [](Fake& f) {
            f.free = 1024;
            f.free_back_at_cancel = 1ull << 34; // (DPAPP-2h-sec, second pass: read before the cancel)
        };
        CHECK_EQ(RunJob(Job(Act::Prepare), b3.in, b3.con, nullptr), kErrNoSpace);
        CHECK(!memo3.load() && b3.con.begin_forms.size() == 1 && b3.con.StepAt("install finish: the SD card is full") >= 0 &&
              b3.con.StepAt("hidden form not taken") < 0);
        // ... and a Finish refused with room at that moment but too little for the fallback after it: no memo, no fallback (the check after the cancel)
        Clean();
        Bench b4;
        std::atomic<bool> memo4{false};
        b4.in.hidden_refused = &memo4;
        b4.con.refuse_hidden_finish = true;
        b4.con.on_finish = [](Fake& f) { f.free = (4ull << 20) + 1; };
        CHECK_EQ(RunJob(Job(Act::Prepare), b4.in, b4.con, nullptr), kErrNoSpace);
        CHECK(!memo4.load() && b4.con.begin_forms.size() == 1 && b4.con.StepAt("install finish: the SD card is full") < 0 &&
              b4.con.StepAt("hidden form not taken: the SD card is full, no fallback") >= 0);
    }
    // F5 (the fake as the console): a delete AM did not do keeps the ticket of the title still listed; the record stays (Removing)
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.delete_fail = true;
        b.con.tickets_dropped.clear();
        RunJob(Job(Act::Remove), b.in, b.con, nullptr);
        Guest g;
        CHECK(b.con.titles.count(kAm) == 1 && b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty());
        CHECK(FindGuest(kPrepared, &g) && g.state == GuestState::Removing);
        b.con.delete_fail = false;
    }
    // F3: a removal on ANOTHER console (the card moved): the title not listed there, a ticket at the id - this console's - never dropped; the
    // record goes
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        g.console = ConsoleStampOf(0x2222); // (written by the card's own console)
        CHECK(SaveGuest(g));
        b.con.titles.erase(kAm);
        b.con.tickets_dropped.clear();
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty() && !FindGuest(kPrepared, &g));
    }
}

// (DPAPP-2h-sec, second pass) the record goes only once no ticket of ours is left at the id: a delete that took the title and not its ticket
// (DeletePrepared's kTicketLeft: AM's list not read, AM kept it) is followed by the ticket alone, in the record's form and in the other form; a
// record whose word on the ticket is not known to be this console's (no console=, the stamp unknown) never drops a ticket and stays while one is there
static void TestSec2hSecond() {
    // the record's form: the ticket kept by the delete goes right after it; kept twice, the record stays and the next Remove takes it
    for (int keeps = 1; keeps <= 2; ++keeps) {
        Clean();
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.keep_ticket_once.assign(static_cast<std::size_t>(keeps), kAm);
        const RunResult rc = RunJob(Job(Act::Remove), b.in, b.con, nullptr);
        Guest g;
        CHECK(b.con.titles.count(kAm) == 0);
        if (keeps == 1) {
            CHECK(rc == 0 && b.con.tickets.count(kAm) == 0 && !FindGuest(kPrepared, &g));
            CHECK(b.con.StepAt("remove: the ticket of the title deleted dropped") >= 0);
        } else {
            CHECK(rc != 0 && b.con.tickets.count(kAm) == 1 && FindGuest(kPrepared, &g) && g.state == GuestState::Removing && TicketWordHere(g));
            CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
            CHECK(b.con.tickets.count(kAm) == 0 && !FindGuest(kPrepared, &g));
        }
    }
    // the other form a migration left, ours: the ticket its delete kept goes right after it, then the record
    Clean();
    {
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        b.con.titles[kM3c1] = b.con.titles[kAm];
        b.con.tickets.insert(kM3c1);
        b.con.keep_ticket_once = {kM3c1};
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(b.con.titles.count(kM3c1) == 0 && b.con.tickets.count(kM3c1) == 0 && b.con.tickets.count(kAm) == 0 && !FindGuest(kPrepared, &g));
    }
    // a record of the builds before (no console=) or this console's stamp unknown, its title gone, a ticket at the id: nothing dropped, the record
    // stays (Removing) and the page says 326; once no ticket is there, the Remove takes the record
    for (int way = 0; way < 2; ++way) {
        Clean();
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        Guest g;
        CHECK(FindGuest(kPrepared, &g));
        if (way == 0) {
            g.console = 0;
            CHECK(SaveGuest(g));
        } else {
            SetThisConsole(0);
        }
        b.con.titles.erase(kAm);
        b.con.tickets_dropped.clear();
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), kErrIdTicket);
        CHECK(b.con.tickets.count(kAm) == 1 && b.con.tickets_dropped.empty() && FindGuest(kPrepared, &g) && g.state == GuestState::Removing &&
              TicketWordUnplaced(g));
        bool said = false;
        for (const std::string& n : b.con.notes) {
            said = said || n.find("a ticket is there") != std::string::npos;
        }
        CHECK(said);
        b.con.tickets.erase(kAm);
        CHECK_EQ(RunJob(Job(Act::Remove), b.in, b.con, nullptr), 0);
        CHECK(!FindGuest(kPrepared, &g));
        SetThisConsole(ConsoleStampOf(0x1111));
    }
}

// (DPAPP-2j, R-D) an error of the SD card in the middle of the install is no refusal of the form: the Result AM or FS gave is in the log, and it
// says which - FS/FSI/SDMC (and a short write) kErrSdIo, FS 210 kErrNoSpace, AM's state (4) and a number of no module kErrInstall; only AM's
// own refusal is kErrCannotHide. Never the memo, never a fallback; nothing of the run left (no record, no ticket, no work file)
static void TestSdErrors2j() {
    struct Case {
        RunResult rc;
        int err;
        const char* word;
    };
    const Case cases[] = {
        {static_cast<RunResult>(0xC8804478u), kErrSdIo, "an error of the SD card"},   // FS 120 (not found)
        {static_cast<RunResult>(0xC8A04549u), kErrSdIo, "an error of the SD card"},   // FS 329: a bus error of the card
        {static_cast<RunResult>(0xD8E0806Bu), kErrSdIo, "an error of the SD card"},   // the app's short write
        {static_cast<RunResult>(0xD8A0F7BFu), kErrSdIo, "an error of the SD card"},   // SDMC
        {static_cast<RunResult>(0xC86044D2u), kErrNoSpace, "the SD card is full"},    // FS 210 (partition full)
        {static_cast<RunResult>(0xC8A08004u), kErrInstall, "AM was busy"},            // AM 4 (its state)
        {static_cast<RunResult>(0x00000123u), kErrInstall, "not known"},              // no module (DPAPP-2j-rev: no code of 3DS Link's - 300-330 are its own)
        {kAmRefusal, kErrCannotHide, "AM refused it"},                                // AM's own refusal
    };
    for (const Case& k : cases) {
        for (int at_finish = 0; at_finish < 2; ++at_finish) {
            Clean();
            Bench b;
            std::atomic<bool> memo{false};
            b.in.hidden_refused = &memo;
            b.con.ticket_at_write = true;
            if (at_finish) {
                b.con.finish_rc = k.rc;
            } else {
                b.con.install_fail_at = 1;
                b.con.install_fail_rc = k.rc;
            }
            const RunResult rc = RunJob(Job(Act::Prepare), b.in, b.con, nullptr);
            CHECK_EQ(rc, k.err);
            CHECK(b.con.NoteHas("Result 0x" + HexDigits(static_cast<u32>(k.rc), 8)) && b.con.NoteHas(k.word));
            CHECK(b.con.NoteHas(at_finish ? "guest: install finish 000400010F8F8200" : "guest: install write 000400010F8F8200"));
            Guest g;
            CHECK(!memo.load() && b.con.begin_forms.size() == 1 && b.con.StepAt("fallback: made again in the old form") < 0 && !FindGuest(kPrepared, &g));
            CHECK(b.con.tickets.count(kAm) == 0 && !IsFile(PartPath(kPrepared)) && b.con.titles.empty());
            CHECK((k.err == kErrCannotHide) == (b.con.StepAt("hidden form not taken") >= 0)); // (only AM's refusal is the form's)
            if (rc != k.err) {
                for (const std::string& n : b.con.notes) {
                    std::fprintf(stderr, "    note: %s\n", n.c_str());
                }
            }
        }
    }
    // the classification itself (dlp_guest_logic.hpp), and the lines
    CHECK(ClassifyAm(0) == InstallFault::None && ClassifyAm(0xC8804478u) == InstallFault::SdIo && ClassifyAm(0xC86044D2u) == InstallFault::NoSpace);
    CHECK(ClassifyAm(0xC8A08025u) == InstallFault::FormRefused && ClassifyAm(0xC8A08004u) == InstallFault::Busy && ClassifyAm(0x13Bu) == InstallFault::Unknown);
    CHECK(ClassifyAm(kResultShortWrite) == InstallFault::SdIo && ClassifyAm(0xE0A0F7BFu) == InstallFault::SdIo && ClassifyAm(0xC8A06000u) == InstallFault::SdIo);
    CHECK(FaultLine("install write", 0x000400010F8F8200ull, 0xC8A08025u, 4096, 8192, InstallFault::FormRefused) ==
          "guest: install write 000400010F8F8200: Result 0xC8A08025 (module 32, summary 5, description 37) at 4096 of 8192 bytes: AM refused it");
    CHECK(HexDigits(0xD8E0806Bu, 8) == "D8E0806B" && HexDigits(0, 4) == "0000" && HexDigits(0x000400010F8F8200ull, 16) == "000400010F8F8200");
    CHECK(kErrSdIo == 330 && kErrLast == kErrSdIo && CanTryAgain(kErrSdIo) && std::string(Phrase(kErrSdIo)).find("SD card") != std::string::npos);
    // (DPAPP-2j-rev) the second write of InstallOverHigher (our child of a higher version: AM refused the older one at the Finish, ours deleted) fails
    // with an error of the SD card or a full card: 330 / 311 as for the first write, its Result said once, never 3DS Link's code said as a Result
    for (int k = 0; k < 2; ++k) {
        Clean();
        Bench b;
        CHECK_EQ(RunJob(Job(Act::Prepare), b.in, b.con, nullptr), 0);
        const u16 made = b.con.titles[kAm].first;
        b.con.titles[kAm].first = static_cast<u16>(made + 0x10);
        const u32 bad = k == 0 ? 0xC8804478u : 0xC86044D2u;
        b.con.install_fail_rc = static_cast<RunResult>(bad);
        b.con.on_finish = [](Fake& f) {
            if (f.install_fail_at < 0) {
                f.install_fail_at = f.installs; // (the first write after the refused Finish: the second write of InstallOverHigher)
            }
        };
        b.con.calls.clear();
        b.con.notes.clear();
        CHECK_EQ(RunJob(Job(Act::Prepare, 0x11), b.in, b.con, nullptr), k == 0 ? kErrSdIo : kErrNoSpace);
        CHECK(b.con.Count("finish") == 1 && b.con.Count("delete") == 1 && b.con.Count("begin") == 2);
        CHECK(b.con.NoteHas("guest: install write again 000400010F8F8200: Result 0x" + HexDigits(bad, 8)));
        CHECK(!b.con.NoteHas("Result 0x0000013B") && !b.con.NoteHas("Result 0x0000014A") && !b.con.NoteHas("Result 0x00000137"));
        CHECK(b.con.NoteHas("guest: install finish 000400010F8F8200: 3DS Link's own code " + std::to_string(k == 0 ? kErrSdIo : kErrNoSpace)));
    }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_dlprun_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    aes_init();
    SetThisConsole(ConsoleStampOf(0x1111)); // (DPAPP-2h-sec: the console the records are written on)
    SetThisId0(std::string("0123456789abcdef") + "0123456789abcdef"); // (DPAPP-2j, R-B: the folder of this console's records)
    TestSdErrors2j();
    TestRoute();
    TestChildFacts();
    TestPrepare();
    TestSlot25();
    TestRemoveDisown();
    TestLooks();
    TestForms();
    TestIds2h();
    TestIds2hRev();
    TestSec2h();
    TestSec2hSecond();
    TestGuestFolder();
    TestReconcile();
    if (chdir("/") != 0) {
        std::fprintf(stderr, "cannot leave %s\n", dir.string().c_str());
    }
    fs::remove_all(dir);
    std::printf("dlp_guest_run_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
