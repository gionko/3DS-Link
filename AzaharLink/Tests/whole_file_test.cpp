// whole_file_test (F7, 2026-11-21): AzaharLink.ini written whole or not at all (Includes/azl/whole_file.hpp, the order of the app's WriteTextFile
// that Plugin::SaveConfig now takes, and the reader's choice of LoadConfig). The card is the model of Tests/whole_file_model.hpp: the power goes at
// every step of a save, a write it meets is cut at every prefix, in the three ways a rename onto a file can go (the console refuses, Azahar on a
// Linux host replaces, libctru deletes and renames). What it holds, for every state a save may meet (none, the old file, a stale .bak, half a .tmp,
// the .bak alone, an empty file of 1.1.3, the empty .new boxes of older builds):
//   - after any cut the plugin's reader and the app's reader both read the old whole file or the new whole one: never a part, never a stale .bak,
//     never "missing" when a whole file was there;
//   - a save that ends leaves the new file, no .tmp, no .bak - F7-rev: also the one a cut save left alone, that this save found instead of a file
//     (only a cut at the very last step leaves a stale .bak next to the new file: never read while the file answers);
//   - two saves cut one after the other (with the app opened between them or not) leave one of the three whole texts;
//   - one command that fails with the power on: the save says false and the old file is whole at its place, or it says true and the file is new;
//   - the cost: six calls of the card on a file that exists, four on a card without it (F7-rev: the .bak dropped all the same); the .new boxes are
//     never touched;
//   - F7-rev: the reader looks at the file once more before it takes the .bak (CTRPF's File::Exists says 0 for a passing error of the card too).
//   whole_file_test
#include <cstdio>
#include <set>

#include "whole_file_model.hpp"

using namespace wfm;
using azl::whole::BakOf;
using azl::whole::Source;
using azl::whole::TmpOf;

static int g_checks = 0, g_failures = 0;
static int g_shown = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            if (g_shown++ < 40) {                                                                  \
                std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                    \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#define CHECK_AT(cond, st, mode, what)                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            if (g_shown++ < 40) {                                                                  \
                std::fprintf(stderr, "  FAIL line %d: %s (%s, %s, %s)\n", __LINE__, #cond, (st), ModeName(mode), (what)); \
            }                                                                                      \
        }                                                                                          \
    } while (0)

static Read Whole(const std::string& t) {
    Read r;
    r.present = true;
    r.text = t;
    return r;
}

// what the readers read before the save: the file, else its .bak, else nothing
static Read Before(const Files& f) { return PluginRead(f, kPath); }

static bool BoxesKept(const Files& before, const Files& after) {
    for (const std::string& box : {kBoxTitles, kBoxIni}) {
        if (before.count(box) != after.count(box) || (before.count(box) && before.at(box) != after.at(box))) {
            return false;
        }
    }
    return true;
}

static void TestOneCut() {
    std::printf("one save cut at every step, every prefix of its write, three ways of renaming\n");
    long cases = 0;
    ForEachCut([&](const State& st, RenameMode mode, const Run& r) {
        ++cases;
        const Read old = Before(st.files);
        const Read got = PluginRead(r.files, kPath);
        CHECK_AT(got == old || got == Whole(kNew), st.name, mode, "the plugin reads the old or the new whole file");
        CHECK_AT(!old.present || got.present, st.name, mode, "never missing after a whole file");
        Files app = r.files;
        const Read app_got = AppRead(app, kPath);
        CHECK_AT(app_got == got, st.name, mode, "the app reads what the plugin reads");
        if (PluginRead(r.files, kPath).present && !r.files.count(kPath)) { // (the plugin took the .bak: the app puts it back)
            CHECK_AT(app.count(kPath) && !app.count(BakOf(kPath)), st.name, mode, "the app puts the .bak back at its place");
        }
        CHECK_AT(BoxesKept(st.files, r.files), st.name, mode, "the .new boxes untouched");
        if (!r.cut) {
            CHECK_AT(r.ok, st.name, mode, "a save the power let end");
        }
        if (r.ok) {
            CHECK_AT(r.files.count(kPath) && r.files.at(kPath) == kNew, st.name, mode, "an ended save leaves the new file");
            CHECK_AT(!r.files.count(TmpOf(kPath)), st.name, mode, "...and no .tmp");
            // (the power that goes at the last step, the .bak's Remove, leaves it next to the new file: stale, never read - checked above)
            CHECK_AT(r.cut || !r.files.count(BakOf(kPath)), st.name, mode, "...and no .bak, the one a cut save left alone too (F7-rev)");
        }
    });
    std::printf("  %ld cut saves\n", cases);
}

static void TestTwoCuts() {
    std::printf("two saves cut one after the other, the app opened between them or not\n");
    long cases = 0;
    for (const State& st : States()) {
        const Read old = Before(st.files);
        for (RenameMode mode : kModes) {
            const long t1 = Save(st.files, mode, -1, 0, kNew).steps;
            for (long k1 = 0; k1 <= t1; ++k1) {
                for (std::size_t p1 : {std::size_t(0), kNew.size() / 2, kNew.size()}) {
                    const Run first = Save(st.files, mode, k1, p1, kNew);
                    for (int app_between = 0; app_between < 2; ++app_between) {
                        Files mid = first.files;
                        if (app_between) {
                            AppRead(mid, kPath);
                        }
                        const long t2 = Save(mid, mode, -1, 0, kNew2).steps;
                        for (long k2 = 0; k2 <= t2; ++k2) {
                            for (std::size_t p2 : {std::size_t(0), kNew2.size() / 2, kNew2.size()}) {
                                ++cases;
                                const Run second = Save(mid, mode, k2, p2, kNew2);
                                const Read got = PluginRead(second.files, kPath);
                                CHECK_AT(got == old || got == Whole(kNew) || got == Whole(kNew2), st.name, mode, "one of the three whole texts");
                                CHECK_AT(!old.present || got.present, st.name, mode, "never missing after a whole file");
                                Files app = second.files;
                                CHECK_AT(AppRead(app, kPath) == got, st.name, mode, "the app reads what the plugin reads");
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("  %ld pairs of cut saves\n", cases);
}

static void TestOneFailure() {
    std::printf("one command of the card fails, the power on\n");
    long cases = 0;
    for (const State& st : States()) {
        const Read old = Before(st.files);
        const bool old_at_place = st.files.count(kPath) != 0;
        for (RenameMode mode : kModes) {
            const long total = Save(st.files, mode, -1, 0, kNew).steps;
            for (long f = 0; f < total; ++f) {
                ++cases;
                const Run r = Save(st.files, mode, -1, 0, kNew, f);
                const Read got = PluginRead(r.files, kPath);
                if (r.ok) {
                    CHECK_AT(got == Whole(kNew), st.name, mode, "a save that says true left the new file");
                } else {
                    CHECK_AT(got == old, st.name, mode, "a save that says false left the old file whole");
                    if (old_at_place) {
                        CHECK_AT(r.files.count(kPath) && r.files.at(kPath) == st.files.at(kPath), st.name, mode, "...at its place");
                    }
                    CHECK_AT(!r.files.count(TmpOf(kPath)) || st.files.count(TmpOf(kPath)), st.name, mode, "...and its .tmp is gone (or one left by an older cut)");
                }
            }
        }
    }
    std::printf("  %ld saves with one failing command\n", cases);
}

static void TestCost() {
    std::printf("the cost of a save\n");
    for (RenameMode mode : kModes) {
        const Run with_file = Save({{kPath, kOld}}, mode, -1, 0, kNew);
        CHECK(with_file.ok && with_file.calls == 6);
        CHECK(with_file.steps == 10); // (in every mode: no rename of the order meets a file, so libctru's second road never runs)
        const Run without = Save({}, mode, -1, 0, kNew);
        CHECK(without.ok && without.calls == 4 && without.steps == 8); // (F7-rev: the Remove of a .bak that may be there)
        std::printf("  %s: %d calls (%ld card commands) with a file, %d (%ld) without\n", ModeName(mode), with_file.calls, with_file.steps, without.calls,
                    without.steps);
    }
    // the reader: one look when the file is there (as LoadConfig before F7), two when it is not
    Card c;
    c.files = {{kPath, kOld}};
    CHECK(azl::whole::Choose(c, kPath) == Source::Path && c.calls == 1);
    Card d;
    d.files = {{BakOf(kPath), kBakOld}};
    CHECK(azl::whole::Choose(d, kPath) == Source::Bak && d.calls == 3); // (F7-rev: the file looked at once more before the .bak)
    Card e;
    CHECK(azl::whole::Choose(e, kPath) == Source::None && e.calls == 2);
    // the file wins over a stale .bak; an empty file is the file (as ReadTextFile)
    Card s;
    s.files = {{kPath, kOld}, {BakOf(kPath), kStale}};
    CHECK(azl::whole::Choose(s, kPath) == Source::Path);
    Card z;
    z.files = {{kPath, ""}, {BakOf(kPath), kStale}};
    CHECK(azl::whole::Choose(z, kPath) == Source::Path);
    // F7-rev: a first look at the file that fails (a passing error: CTRPF's File::Exists says 0) does not hand the reader a stale .bak
    Card f;
    f.files = {{kPath, kOld}, {BakOf(kPath), kStale}};
    f.fail_step = 0;
    CHECK(azl::whole::Choose(f, kPath) == Source::Path && f.calls == 3);
    Card g;
    g.files = {{BakOf(kPath), kBakOld}};
    g.fail_step = 0;
    CHECK(azl::whole::Choose(g, kPath) == Source::Bak); // (the .bak alone: still read)
    // F7-rev: a save that finds only the .bak of a cut save leaves the new file and nothing else, with the power on to its end
    for (RenameMode mode : kModes) {
        const Run r = Save({{BakOf(kPath), kBakOld}}, mode, -1, 0, kNew);
        CHECK(r.ok && r.files.size() == 1 && r.files.count(kPath) && r.files.at(kPath) == kNew);
    }
}

static void TestNames() {
    std::printf("the names are the app's\n");
    CHECK(TmpOf(kPath) == kPath + ".tmp");
    CHECK(BakOf(kPath) == kPath + ".bak");
    CHECK(BakOf("/3ds/3DSLink/AzaharLink.ini") == "/3ds/3DSLink/AzaharLink.ini.bak"); // (the plugin's kConfigBakPath)
}

int main() {
    TestNames();
    TestCost();
    TestOneCut();
    TestTwoCuts();
    TestOneFailure();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
