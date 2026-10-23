// The decisions of the 3DS Link Installer (DlpLink/child/source/installer_logic.hpp, the very file) against real CIAs.
//   child_logic_test <fixture.cia> <tid-hex> <version-u16> [<cia> <tid-hex> <version-u16>]...
// What it pins: a CIA is read the way tools/verify_cia.py reads it (title id, version, content count, the size the header adds up to); damaged
// and cut-off input never crashes the reader (ASan/UBSan: run it with -fsanitize); the install policy (nothing there: install; older: update;
// the same: leave the app alone and bring the plugin up to date; newer: touch nothing); the SD space the policy needs; which paths leave the copy of the
// app's CIA for the app (dlp/3dslink.cia), what it costs on the card and when nothing is written and nothing is asked; what the progress bar counts;
// when the finished screens speak of boot9.bin (per outcome, with boot9.bin there or not, on which screens) and what every combination of buttons does
// there (the guide's key is the only one that does not leave, and only alone); the lockout across the guide; the manifest tools/make_dlp_child.sh writes;
// the plugin magic; what each result code means on the screen; the version text. And safe_write.hpp, the code that puts the plugin and the copy of the CIA on the card,
// run on a temp folder: written whole through a .part and a rename, the old file kept on any failure (unwritable .part, a full card, a close request), an
// interrupted swap put back, the read-back that tells a file is the right one, the copy's own steps (folder made, source missing, nothing started when closing).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../child/source/installer_logic.hpp"
#include "../child/source/safe_write.hpp"

#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static std::vector<inst::u8> ReadAll(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<inst::u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestRealCia(const char* path, unsigned long long tid, unsigned version) {
    const std::vector<inst::u8> file = ReadAll(path);
    CHECK(file.size() > 0x4000);
    std::printf("  %s: %zu bytes\n", path, file.size());
    // exactly what the installer reads: the first 0x4000 bytes and the size of the file
    const size_t head = file.size() < 0x4000 ? file.size() : 0x4000;
    inst::CiaInfo info;
    CHECK(inst::ParseCia(file.data(), head, file.size(), &info));
    CHECK(info.tid == tid);
    CHECK(info.version == version);
    CHECK(info.content_count >= 1);
    CHECK(info.total_size <= file.size() + 0x40 && info.total_size + 0x40 >= file.size()); // the header adds up to the file (padding at most)
    CHECK(inst::TmdOffset(file.data(), head) == info.tmd_offset);

    // too short to show the TMD: false, never a read past the end
    for (size_t cut : {size_t(0), size_t(1), size_t(0x1F), size_t(0x20), size_t(0x2000), size_t(info.tmd_offset), size_t(info.tmd_offset + 4),
                       size_t(info.tmd_offset + 0x3FF)}) {
        inst::CiaInfo none;
        CHECK(!inst::ParseCia(file.data(), cut, file.size(), &none));
        std::vector<inst::u8> exact(file.begin(), file.begin() + static_cast<long>(cut)); // (a buffer of exactly `cut` bytes: ASan sees any read past it)
        CHECK(!inst::ParseCia(exact.data(), exact.size(), file.size(), &none));
    }
    // a file shorter than what the header needs
    inst::CiaInfo none;
    CHECK(!inst::ParseCia(file.data(), head, info.tmd_offset + 0x200, &none));
    // damaged headers: no crash, and the wrong ones are refused
    std::vector<inst::u8> bad(file.begin(), file.begin() + static_cast<long>(head));
    for (size_t i = 0; i < 0x20; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<inst::u8> m = bad;
            m[i] ^= static_cast<inst::u8>(1u << bit);
            inst::CiaInfo r;
            (void)inst::ParseCia(m.data(), m.size(), file.size(), &r); // must not crash; the result of a flipped size field may be either
        }
    }
    {   // header size not 0x2020
        std::vector<inst::u8> m = bad;
        m[0] ^= 1;
        inst::CiaInfo r;
        CHECK(!inst::ParseCia(m.data(), m.size(), file.size(), &r));
    }
    {   // a signature type that is not one of the six
        std::vector<inst::u8> m = bad;
        m[info.tmd_offset + 3] = 0x77;
        inst::CiaInfo r;
        CHECK(!inst::ParseCia(m.data(), m.size(), file.size(), &r));
    }
    {   // no contents
        std::vector<inst::u8> m = bad;
        const inst::u32 sig = inst::Be32(m.data() + info.tmd_offset);
        const size_t sig_size = (sig == 0x10000 || sig == 0x10003) ? 0x200 : (sig == 0x10001 || sig == 0x10004) ? 0x100 : 0x3C;
        const size_t body = info.tmd_offset + 4 + sig_size + ((sig == 0x10002 || sig == 0x10005) ? 0x40 : 0x3C);
        m[body + 0x9E] = 0;
        m[body + 0x9F] = 0;
        inst::CiaInfo r;
        CHECK(!inst::ParseCia(m.data(), m.size(), file.size(), &r));
    }
}

// A CIA header and TMD written by hand for each of the six signature types, with the sizes of makerom's CIAs: the parser must find the title id, the
// version and the content count at the offsets that type puts them (verify_cia.py's table: 0x240 / 0x140 / 0x80 from the TMD start).
static void TestSignatureTypes() {
    struct Sig {
        inst::u32 type;
        size_t size, pad;
    };
    const Sig sigs[] = {{0x10000, 0x200, 0x3C}, {0x10001, 0x100, 0x3C}, {0x10002, 0x3C, 0x40},
                        {0x10003, 0x200, 0x3C}, {0x10004, 0x100, 0x3C}, {0x10005, 0x3C, 0x40}};
    for (const Sig& sig : sigs) {
        std::vector<inst::u8> f(0x4000, 0);
        const inst::u32 hdr = 0x2020, cert = 0xA00, tik = 0x350, tmd = 0xB34, meta = 0x3AC0;
        std::memcpy(&f[0], &hdr, 4); // (the host is little endian, like the 3DS)
        std::memcpy(&f[8], &cert, 4);
        std::memcpy(&f[12], &tik, 4);
        std::memcpy(&f[16], &tmd, 4);
        std::memcpy(&f[20], &meta, 4);
        const inst::u64 content = 0x9E800;
        std::memcpy(&f[0x18], &content, 8);
        const size_t at = inst::TmdOffset(f.data(), f.size());
        CHECK(at == 0x2DC0);
        f[at] = static_cast<inst::u8>(sig.type >> 24);
        f[at + 1] = static_cast<inst::u8>(sig.type >> 16);
        f[at + 2] = static_cast<inst::u8>(sig.type >> 8);
        f[at + 3] = static_cast<inst::u8>(sig.type);
        const size_t body = at + 4 + sig.size + sig.pad;
        const inst::u64 tid = 0x000400000F83D100ull;
        for (int i = 0; i < 8; ++i) {
            f[body + 0x4C + static_cast<size_t>(i)] = static_cast<inst::u8>(tid >> (56 - 8 * i));
        }
        f[body + 0x9C] = 0x04; // version 1024
        f[body + 0x9D] = 0x00;
        f[body + 0x9E] = 0x00; // one content
        f[body + 0x9F] = 0x01;
        const inst::u64 total = at + inst::Align64(tmd) + inst::Align64(content) + meta;
        inst::CiaInfo info;
        CHECK(inst::ParseCia(f.data(), f.size(), total, &info));
        CHECK(info.tid == tid && info.version == 1024 && info.content_count == 1 && info.total_size == total);
    }
}

static void TestPolicy() {
    using inst::Action;
    CHECK(inst::Decide(false, 0, 1090) == Action::Install);
    CHECK(inst::Decide(false, 5000, 1090) == Action::Install); // (a version of a title that is not there means nothing)
    CHECK(inst::Decide(true, 1024, 1090) == Action::Update);
    CHECK(inst::Decide(true, 0, 1090) == Action::Update);
    CHECK(inst::Decide(true, 1090, 1090) == Action::Keep);
    CHECK(inst::Decide(true, 1091, 1090) == Action::Skip);
    CHECK(inst::Decide(true, 0xFFFF, 1090) == Action::Skip);
    // the space: the CIA, the plugin and 16 MiB for AM; the plugin alone with a megabyte; nothing when nothing is touched
    CHECK(inst::NeedBytes(1000000, 1000000, Action::Install, false) == 2000000 + (16ull << 20));
    CHECK(inst::NeedBytes(1000000, 1000000, Action::Update, false) == 2000000 + (16ull << 20));
    CHECK(inst::NeedBytes(1000000, 1000000, Action::Keep, false) == 1000000 + (1ull << 20));
    CHECK(inst::NeedBytes(1000000, 1000000, Action::Skip, false) == 0);
}

// The copy of the app's CIA the installer leaves at sdmc:/3ds/3DSLink/dlp/3dslink.cia: when it is written, what it costs on the card, when nothing is written and
// nothing is asked.
static void TestAppCopy() {
    using inst::Action;
    // the copy takes the size of the CIA on top of what the path needed before (the CIA of the package is copied byte for byte; the numbers differ per input so a
    // wrong term shows)
    for (const unsigned long long cia : {1ull, 635328ull, 1000000ull, 3ull << 20}) {
        for (const unsigned long long plugin : {1ull, 998365ull, 2ull << 20}) {
            CHECK(inst::NeedBytes(cia, plugin, Action::Install, true) == cia + plugin + cia + (16ull << 20));
            CHECK(inst::NeedBytes(cia, plugin, Action::Update, true) == cia + plugin + cia + (16ull << 20));
            CHECK(inst::NeedBytes(cia, plugin, Action::Keep, true) == plugin + cia + (1ull << 20));
            CHECK(inst::NeedBytes(cia, plugin, Action::Install, true) == inst::NeedBytes(cia, plugin, Action::Install, false) + cia);
            CHECK(inst::NeedBytes(cia, plugin, Action::Update, true) == inst::NeedBytes(cia, plugin, Action::Update, false) + cia);
            CHECK(inst::NeedBytes(cia, plugin, Action::Keep, true) == inst::NeedBytes(cia, plugin, Action::Keep, false) + cia);
            CHECK(inst::NeedBytes(cia, plugin, Action::Skip, true) == 0 && inst::NeedBytes(cia, plugin, Action::Skip, false) == 0); // touches nothing, copy included
        }
    }
    // main.cpp fails when free < NeedBytes: a card with exactly the room the app and the plugin need is NOT enough once the copy is written
    CHECK(inst::NeedBytes(635328, 998365, Action::Install, true) > inst::NeedBytes(635328, 998365, Action::Install, false));
    // which path writes the copy: every one that installs or keeps the app, unless the file there is already the right one; never when a newer app stays
    for (const Action a : {Action::Install, Action::Update, Action::Keep}) {
        CHECK(inst::WritesCopy(a, false));
        CHECK(!inst::WritesCopy(a, true));
    }
    CHECK(!inst::WritesCopy(Action::Skip, false) && !inst::WritesCopy(Action::Skip, true));
    // "nothing to do": the same version, the right plugin AND the right copy; a copy alone that is missing is work (and no question), a plugin alone is work
    CHECK(inst::NothingToWrite(Action::Keep, true, true));
    CHECK(!inst::NothingToWrite(Action::Keep, true, false));
    CHECK(!inst::NothingToWrite(Action::Keep, false, true));
    CHECK(!inst::NothingToWrite(Action::Keep, false, false));
    for (const Action a : {Action::Install, Action::Update, Action::Skip}) {
        for (bool plugin_same : {false, true}) {
            for (bool copy_same : {false, true}) {
                CHECK(!inst::NothingToWrite(a, plugin_same, copy_same)); // (the app itself is written, or a newer one is left to the question)
            }
        }
    }
    // the one question about files already there is the plugin's; the copy never asks (nothing to do asks nothing)
    CHECK(inst::AsksRepair(Action::Keep, false));
    CHECK(!inst::AsksRepair(Action::Keep, true));
    for (const Action a : {Action::Install, Action::Update, Action::Skip}) {
        CHECK(!inst::AsksRepair(a, false) && !inst::AsksRepair(a, true));
    }
    // the cases of the job, from the three facts it compares once: nothing there / an older app / the same / a newer one, each with and without the files in place
    struct Case {
        Action action;
        bool plugin_same, copy_same;
        bool asks, writes_copy, nothing;
    };
    const Case cases[] = {
        {Action::Install, false, false, false, true, false},  // a first install: the plugin and the copy are written, nothing is asked
        {Action::Install, true, true, false, false, false},   // (app gone, files left over from an earlier one: only the app is written)
        {Action::Update, false, false, false, true, false},   // an update that the user said yes to
        {Action::Update, true, false, false, true, false},
        {Action::Keep, true, true, false, false, true},       // the same version, everything right: nothing written, nothing asked
        {Action::Keep, true, false, false, true, false},      // the same version, only the copy missing: written, not asked
        {Action::Keep, false, true, true, false, false},      // the same version, wrong plugin: asked; the copy is right
        {Action::Keep, false, false, true, true, false},      // the same version, wrong plugin and no copy: one question, both written
    };
    for (const Case& c : cases) {
        CHECK(inst::AsksRepair(c.action, c.plugin_same) == c.asks);
        CHECK(inst::WritesCopy(c.action, c.copy_same) == c.writes_copy);
        CHECK(inst::NothingToWrite(c.action, c.plugin_same, c.copy_same) == c.nothing);
        // what is written is what the space check counts: with nothing to write the job never reaches the check; otherwise the check counts the copy exactly when it is written
        if (!c.nothing) {
            const unsigned long long with = inst::NeedBytes(635328, 998365, c.action, inst::WritesCopy(c.action, c.copy_same));
            const unsigned long long without = inst::NeedBytes(635328, 998365, c.action, false);
            CHECK(with == without + (c.writes_copy ? 635328ull : 0ull));
        }
    }
}

// The progress bar's total counts what the job writes: the plugin always, the CIA through AM when the app is installed, the copy when it is written. The worker
// advances over every one of them (Advance), so the sum of the parts must be exactly the total or the bar would stop short of 100% (or pass it).
static void TestWorkBytes() {
    using inst::Action;
    CHECK(inst::InstallsApp(Action::Install) && inst::InstallsApp(Action::Update) && !inst::InstallsApp(Action::Keep) && !inst::InstallsApp(Action::Skip));
    const unsigned long long cia = 635328, plugin = 998365;
    CHECK(inst::WorkBytes(cia, plugin, Action::Install, true) == plugin + cia + cia);
    CHECK(inst::WorkBytes(cia, plugin, Action::Install, false) == plugin + cia);
    CHECK(inst::WorkBytes(cia, plugin, Action::Update, true) == plugin + cia + cia);
    CHECK(inst::WorkBytes(cia, plugin, Action::Keep, true) == plugin + cia);   // (the app is not written: the copy is the only CIA that moves)
    CHECK(inst::WorkBytes(cia, plugin, Action::Keep, false) == plugin);
    for (const Action a : {Action::Install, Action::Update, Action::Keep}) {
        for (bool copy_same : {false, true}) {
            const bool write_copy = inst::WritesCopy(a, copy_same);
            const unsigned long long parts = plugin + (a == Action::Keep ? 0 : cia) + (write_copy ? cia : 0);
            CHECK(inst::WorkBytes(cia, plugin, a, write_copy) == parts);
        }
    }
}

// When the finished screens speak of boot9.bin, on which screens, and what the buttons do there. The user's rules (2026-10-21): a notice, never a wall; only when
// the job went well; with boot9.bin on the card the installer says nothing; the guide's key is not one that leaves.
static void TestBoot9Notice() {
    const int outcomes[] = {inst::OUT_NONE,   inst::OUT_INSTALLED,  inst::OUT_UPDATED,  inst::OUT_CURRENT,
                            inst::OUT_NEWER,  inst::OUT_LEFT,       inst::OUT_DOWNGRADED, inst::OUT_REPAIRED};
    // the five outcomes that leave a working 3DS Link because of this run (the user's list); a newer one that stays, "left as it is" and "none" do not
    for (const int o : outcomes) {
        const bool good = o == inst::OUT_INSTALLED || o == inst::OUT_UPDATED || o == inst::OUT_DOWNGRADED || o == inst::OUT_REPAIRED || o == inst::OUT_CURRENT;
        CHECK(inst::LeavesWorkingApp(o) == good);
        // not on the finished screen (the failure screen, a question, the work in progress): never, boot9.bin or not
        for (bool found : {false, true}) {
            CHECK(!inst::Boot9NoticeFor(false, o, found).any());
        }
        // boot9.bin on the card: nothing, on any finished screen
        CHECK(!inst::Boot9NoticeFor(true, o, true).any());
        // boot9.bin missing: the good outcomes show it on both screens and offer the key; the others stay quiet
        const inst::Boot9Notice n = inst::Boot9NoticeFor(true, o, false);
        CHECK(n.top == good && n.bottom == good && n.key == good && n.any() == good);
    }
    CHECK(!inst::LeavesWorkingApp(-1) && !inst::LeavesWorkingApp(99));   // (a number that is no outcome is not one)
    CHECK(!inst::Boot9NoticeFor(true, 99, false).any());
    // the outcome numbers are main.cpp's of old (the job record is a plain int): none = 0, installed = 1, ...
    CHECK(inst::OUT_NONE == 0 && inst::OUT_INSTALLED == 1 && inst::OUT_UPDATED == 2 && inst::OUT_CURRENT == 3 && inst::OUT_NEWER == 4 && inst::OUT_LEFT == 5 &&
          inst::OUT_DOWNGRADED == 6 && inst::OUT_REPAIRED == 7);
}

// What a press does on the screens that wait for the user without asking anything. Up to 1.0.1 every button of the pad left ("press any button"; on the
// finished screen A too) and a user who pressed X for Y was thrown to the HOME menu: now only A (finished screen) and B leave, the guide's key opens the guide
// when it is on offer, and everything else does nothing.
static void TestResultKeys() {
    using inst::ResultKey;
    // the button bits are libctru's (main.cpp static_asserts the same): A B SELECT START RIGHT LEFT UP DOWN R L X Y
    CHECK(inst::kPadA == 1u && inst::kPadB == 2u && inst::kPadSelect == 4u && inst::kPadStart == 8u && inst::kPadRight == 16u && inst::kPadLeft == 32u &&
          inst::kPadUp == 64u && inst::kPadDown == 128u && inst::kPadR == 256u && inst::kPadL == 512u && inst::kPadX == 1024u && inst::kPadY == 2048u);
    CHECK(inst::kGuideKey == inst::kPadY);
    CHECK(inst::kLeavePads == (inst::kPadA | inst::kPadB));
    bool finished_ok = true, failure_ok = true, guide_ok = true;
    for (inst::u32 down = 0; down < (1u << 16); ++down) {   // every combination of the sixteen low bits (the twelve buttons, ZL, ZR and two more)
        // the finished screen, no guide on offer: A or B leave, nothing else does anything (Y included)
        finished_ok = finished_ok && inst::ResultKeyFor(down, true, false) == ((down & (inst::kPadA | inst::kPadB)) ? ResultKey::Leave : ResultKey::None);
        // the failure screen that cannot be tried again (the guide is never there): only B leaves
        failure_ok = failure_ok && inst::ResultKeyFor(down, false, false) == ((down & inst::kPadB) ? ResultKey::Leave : ResultKey::None) &&
                     inst::ResultKeyFor(down, false, true) == ((down & inst::kPadB) ? ResultKey::Leave : ResultKey::None);
        // the finished screen with the guide on offer: Y opens it whatever else is down; without Y, A or B leave and the rest does nothing
        const ResultKey k = inst::ResultKeyFor(down, true, true);
        guide_ok = guide_ok && k == ((down & inst::kPadY) ? ResultKey::Guide : ((down & (inst::kPadA | inst::kPadB)) ? ResultKey::Leave : ResultKey::None));
    }
    CHECK(finished_ok);
    CHECK(failure_ok);
    CHECK(guide_ok);
    // a few of them by name (so a wrong rule above shows too)
    CHECK(inst::ResultKeyFor(inst::kPadY, true, true) == ResultKey::Guide);
    CHECK(inst::ResultKeyFor(inst::kPadY | inst::kPadA, true, true) == ResultKey::Guide);   // a console that reports a second button with the Y: the guide opens, B closes it
    CHECK(inst::ResultKeyFor(inst::kPadY | inst::kPadX, true, true) == ResultKey::Guide);
    CHECK(inst::ResultKeyFor(inst::kPadY, true, false) == ResultKey::None);                 // no notice, no guide: Y does nothing (it is not on the footer)
    CHECK(inst::ResultKeyFor(inst::kPadY, false, true) == ResultKey::None);                 // the failure screen has no guide key
    CHECK(inst::ResultKeyFor(inst::kPadA, true, true) == ResultKey::Leave);                 // "Go to HOME" on the right of the footer
    CHECK(inst::ResultKeyFor(inst::kPadB, true, true) == ResultKey::Leave);
    CHECK(inst::ResultKeyFor(inst::kPadX, true, true) == ResultKey::None);                  // the console test: X must NOT leave
    CHECK(inst::ResultKeyFor(inst::kPadX, true, false) == ResultKey::None);
    CHECK(inst::ResultKeyFor(inst::kPadL | inst::kPadR | inst::kPadStart | inst::kPadSelect | inst::kPadUp | inst::kPadDown | inst::kPadLeft | inst::kPadRight, true, true) ==
          ResultKey::None);
    CHECK(inst::ResultKeyFor(inst::kPadA, false, false) == ResultKey::None);                // a failure screen that cannot retry: A is not a leave key there
    CHECK(inst::ResultKeyFor(inst::kPadB, false, false) == ResultKey::Leave);
    CHECK(inst::ResultKeyFor(0, true, true) == ResultKey::None);
    CHECK(inst::ResultKeyFor(1u << 14, true, true) == ResultKey::None);                     // ZL
}

// The guide is a screen for the key lockout: opening and closing it starts the 800 ms count of the result screen again, so the B that closes the guide cannot
// also leave the app.
static void TestGuideLockout() {
    inst::Lockout lock;
    CHECK(!lock.Open(1, 1000) && lock.changed);                    // the finished screen appears
    CHECK(lock.Open(1, 1800) && !lock.changed);                    // its keys count after 800 ms
    CHECK(!lock.Open(inst::kScreenGuide, 2000) && lock.changed);   // the guide opens (the viewer's own keys do not wait for this: main.cpp does not gate them)
    CHECK(lock.Open(inst::kScreenGuide, 2900) && !lock.changed);
    CHECK(!lock.Open(1, 3000) && lock.changed);                    // closed with B: the finished screen is new again ...
    CHECK(!lock.Open(1, 3799) && !lock.changed);                   // ... and nothing leaves for 800 ms
    CHECK(lock.Open(1, 3800));
    CHECK(inst::kScreenGuide != 0 && inst::kScreenGuide != 1 && inst::kScreenGuide != 2 && inst::kScreenGuide != 3);   // not a phase of the job
}

// ---------------------------------------------------------------------------------------------
// safe_write.hpp on a temp folder
// ---------------------------------------------------------------------------------------------
namespace {

void WriteFile(const std::string& path, const std::string& bytes);

struct TestHooks : inst::Hooks {
    std::vector<std::string> log;
    unsigned long long advanced = 0;
    int blocks = 0;
    bool cancel = false;
    int cancel_after_blocks = -1;   // the system "closes the app" once this many blocks have been written (-1 = never)
    std::string vanish;             // a file that disappears when `vanish_at` bytes have been written (a card that loses the .part before the rename)
    unsigned long long vanish_at = 0;
    std::string rewrite_path, rewrite_bytes;   // a file that is rewritten when `rewrite_at` bytes have been written (what the card holds is then not what was read)
    unsigned long long rewrite_at = 0;
    void Log(const char* line) override { log.push_back(line); }
    void Advance(inst::u64 bytes) override {
        advanced += bytes;
        if (cancel_after_blocks >= 0 && ++blocks >= cancel_after_blocks) {
            cancel = true;
        }
        if (!vanish.empty() && advanced == vanish_at) {
            remove(vanish.c_str());
        }
        if (!rewrite_path.empty() && advanced == rewrite_at) {
            WriteFile(rewrite_path, rewrite_bytes);
        }
    }
    bool Cancelled() override { return cancel; }
    bool Logged(const char* needle) const {
        for (const std::string& l : log) {
            if (l.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

std::string MakeTempDir() {
    char tmpl[] = "/tmp/dlplink_safe_write_XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}
void WriteFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
bool Exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}
std::string Slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string Pattern(size_t n, unsigned seed) {   // not a repeating block: a shifted or truncated copy shows
    std::string s(n, 0);
    unsigned x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>(x >> 24);
    }
    return s;
}

} // namespace

static void TestSafeWrite() {
    CHECK(inst::kBlock == 256u * 1024u);
    const std::string dir = MakeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::string src_path = dir + "/src.bin", path = dir + "/out.bin", tmp = path + ".part", bak = path + ".old";
    const inst::Target t = {"copy", path.c_str(), tmp.c_str(), bak.c_str(), true};
    const inst::Target plugin_like = {"plugin", path.c_str(), tmp.c_str(), bak.c_str(), false};
    const std::string data = Pattern(100000, 1), other = Pattern(50000, 2);
    WriteFile(src_path, data);
    std::vector<inst::u8> buf(4096);   // (a small block: the loop runs 25 times; main.cpp lends 256 KiB)
    std::vector<inst::u8> big(inst::kBlock);

    const auto open_src = [&]() { return fopen(src_path.c_str(), "rb"); };

    // 1. nothing there: written whole, the progress adds up to the size, no .part and no .old left
    {
        TestHooks h;
        FILE* in = open_src();
        CHECK(in != nullptr);
        CHECK(inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(Slurp(path) == data);
        CHECK(!Exists(tmp) && !Exists(bak));
        CHECK(h.advanced == data.size() && h.log.empty());
    }
    // 2. a different file there: replaced by the new one, the old one gone, nothing left; and with the production block size
    {
        WriteFile(path, other);
        TestHooks h;
        FILE* in = open_src();
        CHECK(inst::WriteWhole(in, data.size(), t, big.data(), big.size(), h));
        fclose(in);
        CHECK(Slurp(path) == data && !Exists(tmp) && !Exists(bak));
        CHECK(h.advanced == data.size());
    }
    // 3. the .part cannot be opened (a folder with something in it stands there: remove() cannot take it away): false, the old file is as it was, the log says why
    {
        WriteFile(path, other);
        mkdir(tmp.c_str(), 0777);
        WriteFile(tmp + "/x", "x");
        TestHooks h;
        FILE* in = open_src();
        CHECK(!inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(Slurp(path) == other && !Exists(bak));
        CHECK(h.Logged("fopen(") && h.Logged("errno") && h.advanced == 0);
        // 3b. an interrupted swap of an earlier run (only .old is there): put back before anything else, even when this write then fails
        remove(path.c_str());
        WriteFile(bak, other);
        TestHooks h2;
        in = open_src();
        CHECK(!inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h2));
        fclose(in);
        CHECK(Slurp(path) == other && !Exists(bak));
        remove((tmp + "/x").c_str());
        rmdir(tmp.c_str());
    }
    // 4. a close request between two blocks: the copy stops, the old file stays, no .part; the plugin (stop_on_cancel false) is part of the promised work and goes on
    {
        WriteFile(path, other);
        TestHooks h;
        h.cancel_after_blocks = 3;
        FILE* in = open_src();
        CHECK(!inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(Slurp(path) == other && !Exists(tmp) && !Exists(bak));
        CHECK(h.Logged("stopped at offset 12288") && h.advanced == 3u * 4096u);
        TestHooks h2;
        h2.cancel_after_blocks = 3;
        in = open_src();
        CHECK(inst::WriteWhole(in, data.size(), plugin_like, buf.data(), buf.size(), h2));
        fclose(in);
        CHECK(Slurp(path) == data && !Exists(tmp) && !Exists(bak) && h2.advanced == data.size());
    }
    // 5. the source ends before `size` (a RomFS that ends early): false, the old file stays, no .part
    {
        WriteFile(path, other);
        TestHooks h;
        FILE* in = open_src();
        CHECK(!inst::WriteWhole(in, data.size() + 5000, t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(Slurp(path) == other && !Exists(tmp) && !Exists(bak) && h.Logged("read "));
    }
    // 5b. the swap fails after the old file was put aside (the .part is gone when the rename comes): the old file goes BACK, nothing is left, the log says so
    {
        WriteFile(path, other);
        TestHooks h;
        h.vanish = tmp;
        h.vanish_at = data.size();
        FILE* in = open_src();
        CHECK(!inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(Slurp(path) == other && !Exists(tmp) && !Exists(bak));
        CHECK(h.Logged("rename(") && h.Logged("errno"));
    }
    // 6. a card that is full: no more than 10000 bytes may be written to a file (RLIMIT_FSIZE; the write error is the same kind of thing, a failed write or flush
    // with an errno): false, the old file stays, the .part is gone
    {
        WriteFile(path, other);
        struct rlimit old_limit;
        CHECK(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
        struct rlimit limit = old_limit;
        limit.rlim_cur = 10000;
        void (*old_handler)(int) = std::signal(SIGXFSZ, SIG_IGN);   // (the default action of exceeding the limit is to kill the process)
        CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        TestHooks h;
        FILE* in = open_src();
        CHECK(!inst::WriteWhole(in, data.size(), t, buf.data(), buf.size(), h));
        fclose(in);
        CHECK(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
        std::signal(SIGXFSZ, old_handler);
        CHECK(Slurp(path) == other && !Exists(tmp) && !Exists(bak));
        CHECK(h.Logged("errno"));
        CHECK(h.advanced < data.size());
    }

    // 7. SameFileAs: the same bytes, one byte off at the start / middle / end, a shorter and a longer file, none; the reference may be anywhere in its stream
    {
        WriteFile(path, data);
        FILE* in = open_src();
        fseek(in, 777, SEEK_SET);
        CHECK(inst::SameFileAs(in, path.c_str(), data.size(), buf.data(), buf.size()));
        CHECK(inst::SameFileAs(in, path.c_str(), data.size(), big.data(), big.size()));
        for (const size_t at : {size_t(0), data.size() / 2, data.size() - 1, size_t(4095), size_t(4096), size_t(2047), size_t(2048)}) {
            std::string off = data;
            off[at] = static_cast<char>(off[at] ^ 1);
            WriteFile(path, off);
            CHECK(!inst::SameFileAs(in, path.c_str(), data.size(), buf.data(), buf.size()));
            CHECK(!inst::SameFileAs(in, path.c_str(), data.size(), big.data(), big.size()));
        }
        WriteFile(path, data.substr(0, data.size() - 1));
        CHECK(!inst::SameFileAs(in, path.c_str(), data.size(), buf.data(), buf.size()));
        WriteFile(path, data + "x");
        CHECK(!inst::SameFileAs(in, path.c_str(), data.size(), buf.data(), buf.size()));
        remove(path.c_str());
        CHECK(!inst::SameFileAs(in, path.c_str(), data.size(), buf.data(), buf.size()));
        fclose(in);
    }

    // 8. SaveCopy (the copy of the app's CIA): the folder is made, the file lands whole, a second run over the same file leaves the same file and nothing else
    {
        const std::string sub = dir + "/dlp", copy = sub + "/3dslink.cia", copy_tmp = copy + ".part", copy_bak = copy + ".old";
        const inst::Target ct = {"app copy", copy.c_str(), copy_tmp.c_str(), copy_bak.c_str(), true};
        TestHooks h;
        CHECK(!Exists(sub));
        CHECK(inst::SaveCopy(src_path.c_str(), sub.c_str(), data.size(), ct, buf.data(), buf.size(), h));
        CHECK(Slurp(copy) == data && !Exists(copy_tmp) && !Exists(copy_bak) && h.advanced == data.size());
        TestHooks h2;
        CHECK(inst::SaveCopy(src_path.c_str(), sub.c_str(), data.size(), ct, buf.data(), buf.size(), h2));
        CHECK(Slurp(copy) == data && !Exists(copy_tmp) && !Exists(copy_bak));
        // a stale copy of another version is replaced
        WriteFile(copy, other);
        TestHooks h3;
        CHECK(inst::SaveCopy(src_path.c_str(), sub.c_str(), data.size(), ct, buf.data(), buf.size(), h3));
        CHECK(Slurp(copy) == data);
        // the source cannot be opened: false, the copy that was there is as it was
        WriteFile(copy, other);
        TestHooks h4;
        CHECK(!inst::SaveCopy((dir + "/missing.cia").c_str(), sub.c_str(), data.size(), ct, buf.data(), buf.size(), h4));
        CHECK(Slurp(copy) == other && !Exists(copy_tmp) && !Exists(copy_bak) && h4.Logged("fopen failed") && h4.Logged("errno"));
        // the system is closing: nothing is started (not even the folder)
        const std::string sub2 = dir + "/dlp2", copy2 = sub2 + "/3dslink.cia", copy2_tmp = copy2 + ".part", copy2_bak = copy2 + ".old";
        const inst::Target ct2 = {"app copy", copy2.c_str(), copy2_tmp.c_str(), copy2_bak.c_str(), true};
        TestHooks h5;
        h5.cancel = true;
        CHECK(!inst::SaveCopy(src_path.c_str(), sub2.c_str(), data.size(), ct2, buf.data(), buf.size(), h5));
        CHECK(!Exists(sub2) && h5.Logged("not started") && h5.advanced == 0);
        // the folder cannot be made (a file stands where it should be): false, the log says so with the errno
        WriteFile(dir + "/dlp3", "a file");
        TestHooks h6;
        CHECK(!inst::SaveCopy(src_path.c_str(), (dir + "/dlp3").c_str(), data.size(), ct2, buf.data(), buf.size(), h6));
        CHECK(h6.Logged("cannot be made") && h6.Logged("errno") && h6.advanced == 0);
        // the source ends before the size the manifest named: false, no copy and no .part left behind
        remove(copy.c_str());
        TestHooks h7;
        CHECK(!inst::SaveCopy(src_path.c_str(), sub.c_str(), data.size() + 100, ct, buf.data(), buf.size(), h7));
        CHECK(!Exists(copy) && !Exists(copy_tmp) && !Exists(copy_bak));
        // the read-back: what the card holds is not what was read (here the source changes under the copy as its last block is written): the copy is taken away,
        // not left for the app to refuse (the file that was there is replaced by nothing: a missing file is written again next time)
        WriteFile(copy, other);
        TestHooks h8;
        h8.rewrite_path = src_path;
        h8.rewrite_bytes = Pattern(data.size(), 99);
        h8.rewrite_at = data.size();
        CHECK(!inst::SaveCopy(src_path.c_str(), sub.c_str(), data.size(), ct, buf.data(), buf.size(), h8));
        CHECK(!Exists(copy) && !Exists(copy_tmp) && !Exists(copy_bak) && h8.Logged("does not match"));
        WriteFile(src_path, data);   // (the source as it was, for the rest)
    }

    const std::string rm = "rm -rf '" + dir + "'";
    CHECK(std::system(rm.c_str()) == 0);
}

static void TestChange() {
    const auto v = [](unsigned major, unsigned minor, unsigned micro) { return (major << 10) | (minor << 4) | micro; };
    using inst::Change;
    CHECK(inst::Compare(v(1, 0, 0), v(1, 0, 0)) == Change::Same);
    CHECK(inst::Compare(v(1, 0, 0), v(1, 0, 1)) == Change::Patch);
    CHECK(inst::Compare(v(1, 0, 15), v(1, 1, 0)) == Change::Minor);
    CHECK(inst::Compare(v(1, 5, 3), v(1, 6, 0)) == Change::Minor);
    CHECK(inst::Compare(v(1, 9, 9), v(2, 0, 0)) == Change::Major);
    CHECK(inst::Compare(v(1, 0, 2), v(1, 0, 1)) == Change::Older);
    CHECK(inst::Compare(v(1, 1, 0), v(1, 0, 15)) == Change::Older);
    CHECK(inst::Compare(v(2, 0, 0), v(1, 63, 15)) == Change::Older);
    // the order of the numbers is the order of the versions: Decide and Compare never disagree
    for (unsigned a : {v(0, 9, 0), v(1, 0, 0), v(1, 0, 1), v(1, 1, 0), v(2, 0, 0)}) {
        for (unsigned b : {v(0, 9, 0), v(1, 0, 0), v(1, 0, 1), v(1, 1, 0), v(2, 0, 0)}) {
            const Change c = inst::Compare(a, b);
            const inst::Action act = inst::Decide(true, a, b);
            CHECK((c == Change::Same) == (act == inst::Action::Keep));
            CHECK((c == Change::Older) == (act == inst::Action::Skip));
            CHECK((c == Change::Patch || c == Change::Minor || c == Change::Major) == (act == inst::Action::Update));
        }
    }
    CHECK(std::string(inst::ChangeText(Change::Patch)) == "a patch" && std::string(inst::ChangeText(Change::Older)) == "an older release");
    CHECK(std::string(inst::ChangeText(Change::Minor)) == "a feature update" && std::string(inst::ChangeText(Change::Major)) == "a major update");
}

// every package version (all 65536 of the TMD) against the boundaries of the installed one: Compare and Decide never disagree and "older" is exactly "smaller"
static void TestChangeAll() {
    const auto v = [](unsigned major, unsigned minor, unsigned micro) { return (major << 10) | (minor << 4) | micro; };
    using inst::Change;
    bool consistent = true;
    for (unsigned a : {v(0, 0, 0), v(0, 9, 0), v(1, 0, 0), v(1, 0, 15), v(1, 63, 15), v(63, 63, 15)}) {
        for (unsigned b = 0; b < 65536; ++b) {
            const Change c = inst::Compare(a, b);
            const inst::Action act = inst::Decide(true, a, b);
            consistent = consistent && (c == Change::Same) == (b == a) && (c == Change::Older) == (b < a) && (c == Change::Same) == (act == inst::Action::Keep) &&
                         (c == Change::Older) == (act == inst::Action::Skip);
        }
    }
    CHECK(consistent);
}

// the keys of the screens (the same code main.cpp runs)
static void TestKeys() {
    using inst::NextDecision;
    CHECK(NextDecision(0, false, false) == 0);
    CHECK(NextDecision(0, true, false) == 2);
    CHECK(NextDecision(0, false, true) == 1);
    CHECK(NextDecision(0, true, true) == 2); // both in the same frame: leaving wins
    // the first answer stands whatever comes next: B, then X one frame later (the worker has not woken yet) must not turn Leave into Install older
    for (int first : {1, 2}) {
        for (bool leave : {false, true}) {
            for (bool yes : {false, true}) {
                CHECK(NextDecision(first, leave, yes) == first);
            }
        }
    }
    inst::Lockout lock;
    CHECK(!lock.Open(0, 1000) && lock.changed);   // the work
    CHECK(!lock.Open(3, 2000) && lock.changed);   // a question appears: its keys are closed ...
    CHECK(!lock.Open(3, 2799) && !lock.changed);
    CHECK(lock.Open(3, 2800) && !lock.changed);   // ... for 800 ms
    // B answers; the worker goes to the result in microseconds and the screens never see the work in between: the result is a NEW screen, closed again
    CHECK(!lock.Open(1, 2820) && lock.changed);
    CHECK(!lock.Open(1, 3619));
    CHECK(lock.Open(1, 3620));
    // "Try again": even the same screen as before counts as new
    lock.Forget();
    CHECK(!lock.Open(2, 5000) && lock.changed);
    CHECK(!lock.Open(2, 5799));
    CHECK(lock.Open(2, 5800));
}

static void TestManifest() {
    const inst::Manifest m = inst::ParseManifest(
        "version=1.4.2\nversion16=1090\ntid=000400000F83D100\n3dslink.cia=635328 3f7c0e1e20a1cf8e83cf57b9c6e8b8c6d6dc57b4f2a3ae1d8a1c1d4f5b1ab0c2\n"
        "AzaharLink.3gx=998365 6c8ab1aa8d44ca4b2bd2a8a9a27d5b9a1a0ef5e0cfb5c7e1e5d5a7a1d3c5d7e1\n");
    CHECK(m.version == "1.4.2" && m.version16 == 1090 && m.cia_size == 635328 && m.plugin_size == 998365 && m.complete());
    CHECK(m.tid == 0x000400000F83D100ull && m.has_tid);
    // no title named, or a zero one: not a package (the installer installs the title the manifest names and nothing else)
    CHECK(!inst::ParseManifest("version=1.0.0\nversion16=1024\n3dslink.cia=10 x\nAzaharLink.3gx=20 y\n").complete());
    CHECK(!inst::ParseManifest("version=1.0.0\nversion16=1024\ntid=0\n3dslink.cia=10 x\nAzaharLink.3gx=20 y\n").complete());
    // windows line ends, missing lines, nonsense
    const inst::Manifest w = inst::ParseManifest("version=1.0.0\r\nversion16=1024\r\ntid=000400000F83D100\r\n3dslink.cia=10 x\r\nAzaharLink.3gx=20 y\r\n");
    CHECK(w.complete() && w.version16 == 1024 && w.cia_size == 10 && w.plugin_size == 20);
    // The installer installs 3DS Link and nothing else. The job deletes the title the manifest names (and its ticket) before it installs, so a manifest that is complete
    // but names ANOTHER title (a game, a system title, the Download Play child itself, the app of a neighbouring unique id) is refused whole: complete() alone lets it through.
    CHECK(inst::kAppTid == 0x000400000F83D100ull);
    CHECK(inst::NamesApp(m) && inst::NamesApp(w));
    CHECK(inst::NamesApp(inst::ParseManifest("version=1.0.0\nversion16=1024\ntid=000400000f83d100\n3dslink.cia=10 x\nAzaharLink.3gx=20 y\n")));   // (the case of the digits is not the title)
    for (const char* other : {"000400000F83D200", "000400000F83D101", "000400000F83D000", "000400010F83D100", "0004000E0F83D100", "0004000000169600", "00040000001AAA00",
                              "0004008C00169600", "0004000000033C00", "0004013000002C02", "0005000010000000", "F83D100", "00040000F83D100", "000400000F83D1000"}) {
        const inst::Manifest o = inst::ParseManifest(std::string("version=1.0.0\nversion16=1024\ntid=") + other + "\n3dslink.cia=10 x\nAzaharLink.3gx=20 y\n");
        CHECK(o.complete() && !inst::NamesApp(o));
    }
    CHECK(!inst::NamesApp(inst::Manifest()));   // (no manifest read: no title either)
    CHECK(!inst::ParseManifest("").complete());
    CHECK(!inst::ParseManifest("version=1.0.0\nversion16=1024\ntid=000400000F83D100\n3dslink.cia=10 x\n").complete());           // no plugin line
    CHECK(!inst::ParseManifest("version16=1024\ntid=000400000F83D100\n3dslink.cia=0 x\nAzaharLink.3gx=20 y\n").complete());      // a zero size is no package
    CHECK(!inst::ParseManifest("garbage without equals\n\n=\n").complete());
    CHECK(inst::ParseManifest("version16=\n").has_version16 == false);
}

static void TestTexts() {
    CHECK(inst::VersionText(1024) == "1.0.0");
    CHECK(inst::VersionText(1090) == "1.4.2");
    CHECK(inst::VersionText((17u << 10) | 3) == "17.0.3");
    CHECK(inst::VersionText(0) == "0.0.0");
    CHECK(inst::PluginHeaderOk(reinterpret_cast<const inst::u8*>("3GX$\0\0\0\0"), 8));
    CHECK(!inst::PluginHeaderOk(reinterpret_cast<const inst::u8*>("3GX!"), 4));
    CHECK(!inst::PluginHeaderOk(reinterpret_cast<const inst::u8*>("3GX"), 3));
    // result codes: our own, then FS and AM by module and description, then anything else
    CHECK(inst::ErrorText(inst::kErrSdFull).find("Not enough space") == 0);
    CHECK(inst::ErrorText(inst::kErrPayload).find("damaged") != std::string::npos);
    CHECK(inst::ErrorText(inst::kErrNoPlugin).find("damaged") != std::string::npos);
    CHECK(inst::ErrorText(inst::kErrPluginWrite).find("SD card") != std::string::npos);
    CHECK(inst::ErrorText(inst::kErrCancelled).find("interrupted") != std::string::npos);
    CHECK(inst::ErrorText(inst::kErrNotVerified).find("verified") != std::string::npos);
    CHECK(inst::ErrorText(inst::kErrNoMemory) == "Not enough memory.");
    for (const inst::u32 own : {inst::kErrNoCia, inst::kErrNotCia, inst::kErrShortRead, inst::kErrNotThis}) {
        CHECK(inst::ErrorText(own).find("damaged") != std::string::npos);
    }
    CHECK(inst::ErrorText(inst::kErrShortWrite).find("SD card") != std::string::npos);
    // our own codes decode as module 19 (RM_HID): they are told from the system's codes by their exact value, never by the module
    for (const inst::u32 own : {inst::kErrPayload, inst::kErrNoCia, inst::kErrNotCia, inst::kErrShortRead, inst::kErrShortWrite, inst::kErrCancelled,
                                inst::kErrNotVerified, inst::kErrNoPlugin, inst::kErrPluginWrite, inst::kErrNotThis, inst::kErrNoMemory}) {
        CHECK(inst::RcModule(own) == 19 && inst::ErrorText(own) != "Something went wrong.");
    }
    // AM_GetTitleInfo on a title that is not installed: that one answer is "not found"; every other failure is "AM could not say"
    CHECK(inst::IsTitleNotFound(0xD8A083FAu));
    CHECK(!inst::IsTitleNotFound(0xD8A083FBu) && !inst::IsTitleNotFound(0xC8804478u) && !inst::IsTitleNotFound(0));
    CHECK(inst::IsTitleNotFound((0x1Bu << 27) | (5u << 21) | (32u << 10) | 1018u));
    CHECK(inst::IsTitleNotFound(0xC8A083FAu));  // (module 32, description 1018 under another level: the second clause, not the exact value)
    CHECK(!inst::IsTitleNotFound((0x1Bu << 27) | (5u << 21) | (32u << 10) | 1019u) && !inst::IsTitleNotFound((0x1Bu << 27) | (5u << 21) | (31u << 10) | 1018u));
    CHECK(inst::ErrorText(0xD8E06406u).find("refused access") != std::string::npos);
    CHECK(inst::ErrorText(0xD0406401u).find("refused access") != std::string::npos);
    // FS (module 17): 200-229 the card is full, 130-179 it is not there
    const auto rc = [](unsigned module, unsigned desc) { return 0xC8000000u | (module << 10) | desc; };
    CHECK(inst::ErrorText(rc(17, 205)).find("Not enough space") == 0);
    CHECK(inst::ErrorText(rc(17, 130)) == "The SD card is not available.");
    CHECK(inst::ErrorText(rc(17, 179)) == "The SD card is not available.");
    CHECK(inst::ErrorText(rc(17, 229)).find("Not enough space") == 0);
    CHECK(inst::ErrorText(rc(17, 230)) == "Something went wrong.");
    CHECK(inst::ErrorText(rc(17, 180)) == "Something went wrong.");
    CHECK(inst::ErrorText(rc(17, 129)) == "Something went wrong.");
    // AM (module 32): the title database, a damaged package
    CHECK(inst::ErrorText(rc(32, 43)).find("title database") != std::string::npos);
    CHECK(inst::ErrorText(rc(32, 393)).find("title database") != std::string::npos);
    CHECK(inst::ErrorText(rc(32, 106)).find("damaged") != std::string::npos);
    CHECK(inst::ErrorText(rc(32, 39)).find("damaged") != std::string::npos);
    // what a title that is not installed answers (not an error to the user: the installer ignores it) is not described as anything special
    CHECK(inst::ErrorText(0xD8A083FAu) == "Something went wrong.");
    CHECK(inst::RcModule(0xD8A083FAu) == 32 && inst::RcDescription(0xD8A083FAu) == 1018);
    CHECK(inst::ErrorText(0) == "Something went wrong.");
}

int main(int argc, char** argv) {
    if (argc < 4 || (argc - 1) % 3 != 0) {
        std::printf("usage: %s <cia> <tid-hex> <version-u16> [<cia> <tid-hex> <version-u16>]...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i + 2 < argc + 0; i += 3) {
        TestRealCia(argv[i], std::strtoull(argv[i + 1], nullptr, 16), static_cast<unsigned>(std::strtoul(argv[i + 2], nullptr, 10)));
    }
    TestSignatureTypes();
    TestPolicy();
    TestAppCopy();
    TestWorkBytes();
    TestBoot9Notice();
    TestResultKeys();
    TestGuideLockout();
    TestSafeWrite();
    TestChange();
    TestChangeAll();
    TestKeys();
    TestManifest();
    TestTexts();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
