// The "How to get boot9.bin" guide without a console (ConfigApp/guide/guide_logic.hpp, the very file, plain C++), run by Tests/run.sh. What it pins: the steps
// (how many, that every one has a title and at most three lines of words that fit under the picture, that the pictures are in order and that every picture
// exists as a PNG within the size the top screen has room for, with the t3s that tex3ds turns into the t3x, and that the script that draws them uses nothing
// outside the folder), that the last step says where the dump ends up and that the place is where 3DS Link looks, what every combination of keys does to the
// step number (L / D-pad up back, R / D-pad down forward: the steps are a vertical list; the D-pad's left and right do nothing; both directions at once nothing, B
// closes, no wrap), what a tap on the footer does and what the footer's buttons say, and the look at the SD card for boot9.bin (the names and sizes that count, the
// two folders) against a made-up card.
//
// The test is run from AzaharLink/ (Tests/run.sh does that): `guide_test [guide folder]` (default ConfigApp/guide).
#include <dirent.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "guide_logic.hpp"

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

static std::string g_dir = "ConfigApp/guide";

static bool Exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::vector<std::string> ListDir(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (const struct dirent* e = readdir(d)) {
            names.push_back(e->d_name);
        }
        closedir(d);
    }
    return names;
}

static bool StartsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}
static bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
static bool Contains(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

// ---------------------------------------------------------------------------------------------

static void TestWords() {
    CHECK(guide::StepCount() == guide::kStepTotal);
    CHECK(guide::StepCount() >= 7 && guide::StepCount() <= 8); // the guide is meant to be short: 7 or 8 steps
    for (int i = 0; i < guide::StepCount(); ++i) {
        const guide::Step& step = guide::StepAt(i);
        CHECK(step.title && std::strlen(step.title) > 0 && static_cast<int>(std::strlen(step.title)) <= guide::kMaxTitleChars);
        CHECK(step.text[0] != nullptr); // at least one line
        bool gap = false;
        int lines = 0;
        for (int l = 0; l < 3; ++l) {
            if (!step.text[l]) {
                gap = true; // after the first empty one, no line may follow
                continue;
            }
            CHECK(!gap);
            ++lines;
            const std::string line = step.text[l];
            CHECK(!line.empty());
            CHECK(static_cast<int>(line.size()) <= guide::kMaxLineChars);
            CHECK(line.front() != ' ' && line.back() != ' ');
            for (char c : line) {
                CHECK(c >= 0x20 && c < 0x7F); // plain ASCII: the font and the quotes of the source are never in doubt
            }
        }
        CHECK(lines >= 1 && lines <= 3);
        for (char c : std::string(step.title)) {
            CHECK(c >= 0x20 && c < 0x7F);
        }
    }
    // an out of range index gives the nearest step, never memory that is not the table's
    CHECK(&guide::StepAt(-3) == &guide::StepAt(0));
    CHECK(&guide::StepAt(99) == &guide::StepAt(guide::StepCount() - 1));

    // what the steps say, step by step (DlpLink/docs/GUIDA-BOOT9.md)
    const guide::Step& first = guide::StepAt(0);
    CHECK(Contains(first.text[0], "off") || Contains(first.text[1], "off"));
    CHECK(Contains(first.text[1], "START"));
    CHECK(Contains(guide::StepAt(1).text[0], "HOME"));
    CHECK(Contains(guide::StepAt(2).title, "Scripts"));
    CHECK(Contains(guide::StepAt(3).title, "GM9Megascript"));
    CHECK(Contains(guide::StepAt(4).title, "Dump Options"));
    CHECK(Contains(guide::StepAt(5).title, "Boot9.bin") && Contains(guide::StepAt(5).title, "Boot11.bin"));
    CHECK(Contains(guide::StepAt(6).text[0], "0:/gm9/out"));

    // The last step says where the dump ends up AND that 3DS Link looks there. The place it names is the folder 3DS Link looks in: sdmc:/gm9/out
    // (paths.hpp kGodModeOutDir, which setup::Boot9Candidates and FindBoot9 search) is the folder this module's own look searches too.
    const guide::Step& last = guide::StepAt(guide::StepCount() - 1);
    CHECK(last.image == -1); // words only: the dump is in a folder, there is nothing of GodMode9 to show
    std::string all;
    for (int l = 0; l < 3; ++l) {
        if (last.text[l]) {
            all += std::string(last.text[l]) + "\n";
        }
    }
    CHECK(Contains(all, "gm9/out"));
    CHECK(Contains(all, "3DS Link"));
    const std::string god = guide::kBoot9GodModeDir;
    CHECK(god == "sdmc:/gm9/out");
    CHECK(EndsWith(god, "gm9/out") && StartsWith(god, "sdmc:/"));
    CHECK(Contains(guide::StepAt(6).text[0], god.substr(6).c_str())); // "0:/gm9/out" holds "gm9/out"
}

static void TestPictures() {
    // pictures are in order: step N's picture is image N - 1 ... (static_assert in the header; here the files)
    std::set<int> used;
    int next = 0;
    for (int i = 0; i < guide::StepCount(); ++i) {
        const int image = guide::StepAt(i).image;
        CHECK(image >= -1 && image < guide::kImageCount);
        if (image < 0) {
            continue;
        }
        CHECK(image == next); // in order, none twice
        ++next;
        used.insert(image);
    }
    CHECK(static_cast<int>(used.size()) == guide::kImageCount);

    size_t total_png = 0;
    for (int image = 0; image < guide::kImageCount; ++image) {
        char name[32];
        std::snprintf(name, sizeof(name), "step%d", image + 1);
        const std::string png = g_dir + "/gfx/" + name + ".png";
        const std::string t3s = g_dir + "/gfx/" + name + ".t3s";
        CHECK(Exists(png));
        CHECK(Exists(t3s));
        // the PNG: its signature and the size in its IHDR (big endian) within what the top screen has room for (400x240 less the bar and three lines)
        const std::string data = ReadFile(png);
        total_png += data.size();
        CHECK(data.size() > 33 && data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && data.compare(12, 4, "IHDR") == 0);
        if (data.size() > 33) {
            auto be32 = [&](size_t o) {
                return (static_cast<unsigned>(static_cast<unsigned char>(data[o])) << 24) | (static_cast<unsigned>(static_cast<unsigned char>(data[o + 1])) << 16) |
                       (static_cast<unsigned>(static_cast<unsigned char>(data[o + 2])) << 8) | static_cast<unsigned>(static_cast<unsigned char>(data[o + 3]));
            };
            const unsigned w = be32(16), h = be32(20);
            CHECK(w >= 64 && w <= static_cast<unsigned>(guide::kMaxPictureW));
            CHECK(h >= 32 && h <= static_cast<unsigned>(guide::kMaxPictureH));
            CHECK(w % 2 == 0 && h % 2 == 0); // made 2x: even sides
        }
        // the t3s tex3ds reads: RGB565 and compressed, then the picture, spelt the way the Makefile's rule finds it (beside the t3s)
        const std::string text = ReadFile(t3s);
        CHECK(text == std::string("-f rgb565 -z auto\n") + name + ".png\n");
    }
    CHECK(total_png > 0 && total_png < 64 * 1024); // the pictures are small: they travel in the installer over Download Play (their t3x are about 12 KB)

    // no picture of the folder is left over (a step taken out leaves a file that nothing shows and the Makefile would still link in)
    int pngs = 0, t3ss = 0;
    for (const std::string& name : ListDir(g_dir + "/gfx")) {
        if (EndsWith(name, ".png")) {
            ++pngs;
        }
        if (EndsWith(name, ".t3s")) {
            ++t3ss;
        }
    }
    CHECK(pngs == guide::kImageCount && t3ss == guide::kImageCount);

    // The script that draws them reads only inside the repository: the screenshots are copies in guide/src (the final repository must not depend on DlpTest/).
    const std::string script = ReadFile("../DlpLink/tools/make_guide.py");
    if (script.empty()) {
        std::printf("SKIP the script check: ../DlpLink/tools/make_guide.py is not here (the test is run from AzaharLink/ of the whole repository)\n");
    } else {
        CHECK(!Contains(script, "DlpTest"));
        size_t pos = 0;
        int shots = 0;
        while ((pos = script.find("snap_", pos)) != std::string::npos) {
            const size_t end = script.find(".png", pos);
            CHECK(end != std::string::npos && end - pos < 40);
            if (end != std::string::npos && script.compare(pos, 5, "snap_") == 0 && script.substr(pos, end - pos).find('*') == std::string::npos) {
                CHECK(Exists(g_dir + "/src/" + script.substr(pos, end + 4 - pos))); // (a name, not the glob the docstring writes)
                ++shots;
            }
            pos += 5;
        }
        CHECK(shots > 0);
    }
}

// ---------------------------------------------------------------------------------------------

static void TestMove() {
    using namespace guide;
    const int n = StepCount();
    bool closed = true;
    // libctru's bits of the D-pad's left and right, which the viewer does not use any more (the steps are a vertical list): they must do nothing
    const u32 kDRight = 1u << 4, kDLeft = 1u << 5;

    // the keys the viewer answers to are libctru's (guide.cpp has a static_assert for each against the real header)
    CHECK(kKeyB == (1u << 1) && kKeyDUp == (1u << 6) && kKeyDDown == (1u << 7) && kKeyR == (1u << 8) && kKeyL == (1u << 9));

    CHECK(Move(0, kKeyR, &closed) == 1 && !closed);
    CHECK(Move(1, kKeyL, &closed) == 0 && !closed);
    CHECK(Move(0, kKeyDDown, &closed) == 1 && !closed); // D-pad down = the next step
    CHECK(Move(1, kKeyDUp, &closed) == 0 && !closed);   // D-pad up = the step before
    CHECK(Move(3, kKeyDDown, &closed) == 4 && !closed);
    CHECK(Move(3, kKeyDUp, &closed) == 2 && !closed);
    CHECK(Move(3, 0, &closed) == 3 && !closed); // nothing pressed: nothing happens, and `closed` is written (false)

    // the D-pad's left and right do nothing in the viewer, from any step, and do not disturb a key that moves
    for (int step = 0; step < n; ++step) {
        CHECK(Move(step, kDLeft, &closed) == step && !closed);
        CHECK(Move(step, kDRight, &closed) == step && !closed);
        CHECK(Move(step, kDLeft | kDRight, &closed) == step && !closed);
    }
    CHECK(Move(3, kDLeft | kKeyDUp, &closed) == 2 && !closed);
    CHECK(Move(3, kDRight | kKeyDDown, &closed) == 4 && !closed);
    CHECK(Move(3, kDLeft | kKeyDDown, &closed) == 4 && !closed);
    CHECK(Move(3, kDRight | kKeyL, &closed) == 2 && !closed);
    CHECK(Move(3, kDLeft | kKeyR, &closed) == 4 && !closed);
    CHECK(Move(3, kDLeft | kDRight | kKeyDUp, &closed) == 2 && !closed);

    // the bounds: no wrap-around, with every key that moves
    CHECK(Move(0, kKeyL, &closed) == 0 && !closed);
    CHECK(Move(0, kKeyDUp, &closed) == 0 && !closed);
    CHECK(Move(n - 1, kKeyR, &closed) == n - 1 && !closed);
    CHECK(Move(n - 1, kKeyDDown, &closed) == n - 1 && !closed);
    CHECK(Move(0, kKeyDUp | kKeyL, &closed) == 0 && !closed);
    CHECK(Move(n - 1, kKeyDDown | kKeyR, &closed) == n - 1 && !closed);
    // ... and the first and the last step are reached one at a time, in both directions
    {
        int step = 0;
        for (int i = 0; i < n + 3; ++i) {
            const int next = Move(step, kKeyDDown, &closed);
            CHECK(next == (step < n - 1 ? step + 1 : step));
            step = next;
        }
        CHECK(step == n - 1);
        for (int i = 0; i < n + 3; ++i) {
            const int next = Move(step, kKeyDUp, &closed);
            CHECK(next == (step > 0 ? step - 1 : 0));
            step = next;
        }
        CHECK(step == 0);
    }

    // the Circle Pad moves along the list like the D-pad (libctru's KEY_UP / KEY_DOWN include it: every other list of the app answers to it)
    CHECK(kKeyCPadUp == (1u << 30) && kKeyCPadDown == (1u << 31));
    CHECK(Move(3, kKeyCPadUp, &closed) == 2 && !closed);
    CHECK(Move(3, kKeyCPadDown, &closed) == 4 && !closed);
    CHECK(Move(0, kKeyCPadUp, &closed) == 0 && !closed);
    CHECK(Move(n - 1, kKeyCPadDown, &closed) == n - 1 && !closed);
    CHECK(Move(3, kKeyCPadUp | kKeyCPadDown, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyCPadUp | kKeyDDown, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyCPadUp | kKeyDUp, &closed) == 2 && !closed);
    CHECK(Move(3, kKeyCPadDown | kKeyL, &closed) == 3 && !closed);
    CHECK(Move(2, kKeyB | kKeyCPadDown, &closed) == 2 && closed);

    // B closes, and the step is left as it was; with other keys too
    CHECK(Move(2, kKeyB, &closed) == 2 && closed);
    CHECK(Move(n - 1, kKeyB, &closed) == n - 1 && closed);
    CHECK(Move(2, kKeyB | kKeyR, &closed) == 2 && closed);
    CHECK(Move(2, kKeyB | kKeyL | kKeyDUp, &closed) == 2 && closed);
    CHECK(Move(2, kKeyB | kKeyDDown, &closed) == 2 && closed);

    // both directions at once: no move (L and R together, up and down of the D-pad together, and across)
    CHECK(Move(3, kKeyL | kKeyR, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyDUp | kKeyDDown, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyL | kKeyDDown, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyDUp | kKeyR, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyL | kKeyR | kKeyDUp | kKeyDDown, &closed) == 3 && !closed);
    CHECK(Move(3, kKeyL | kKeyR | kKeyDUp | kKeyDDown | kDLeft | kDRight, &closed) == 3 && !closed);

    // two keys of the same direction are one step
    CHECK(Move(3, kKeyL | kKeyDUp, &closed) == 2 && !closed);
    CHECK(Move(3, kKeyR | kKeyDDown, &closed) == 4 && !closed);

    // other buttons do nothing (A, SELECT, START, X, Y: bits 0, 2, 3, 10, 11; the D-pad's right and left are bits 4 and 5)
    for (u32 other : {1u << 0, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 10, 1u << 11}) {
        CHECK(Move(3, other, &closed) == 3 && !closed);
    }

    // a step out of range is brought in first
    CHECK(Move(-5, 0, &closed) == 0);
    CHECK(Move(99, 0, &closed) == n - 1);
    CHECK(Move(-5, kKeyR, &closed) == 1);
    CHECK(Move(-5, kKeyDDown, &closed) == 1);
    CHECK(Move(99, kKeyL, &closed) == n - 2);
    CHECK(Move(99, kKeyDUp, &closed) == n - 2);

    // no `closed` pointer: the same answers
    CHECK(Move(3, kKeyR, nullptr) == 4);
    CHECK(Move(3, kKeyDDown, nullptr) == 4);
    CHECK(Move(3, kKeyDUp, nullptr) == 2);
    CHECK(Move(3, kKeyB, nullptr) == 3);

    // every combination of the five keys that count and four that do not, from every step: the whole table against the rule stated plainly
    for (int step = 0; step < n; ++step) {
        for (u32 mask = 0; mask < 512; ++mask) {
            u32 keys = 0;
            keys |= (mask & 1) ? kKeyB : 0;
            keys |= (mask & 2) ? kKeyL : 0;
            keys |= (mask & 4) ? kKeyR : 0;
            keys |= (mask & 8) ? kKeyDUp : 0;
            keys |= (mask & 16) ? kKeyDDown : 0;
            keys |= (mask & 32) ? kDLeft : 0;
            keys |= (mask & 64) ? kDRight : 0;
            keys |= (mask & 128) ? (1u << 0) : 0;  // A
            keys |= (mask & 256) ? (1u << 11) : 0; // Y
            const bool back = (mask & (2 | 8)) != 0, forward = (mask & (4 | 16)) != 0;
            int expect = step;
            if (!(mask & 1) && back != forward) {
                expect = back ? (step > 0 ? step - 1 : 0) : (step < n - 1 ? step + 1 : step);
            }
            bool c = false;
            const int got = Move(step, keys, &c);
            CHECK(got == expect);
            CHECK(c == ((mask & 1) != 0));
            CHECK(got >= 0 && got < n);
        }
    }
}

static void TestTap() {
    using namespace guide;
    // the three buttons of the footer, and the places that are none
    CHECK(TapKeys(kCloseButton.x + 5, kCloseButton.y + 5) == kKeyB);
    CHECK(TapKeys(kBackButton.x + 5, kBackButton.y + 5) == kKeyL);
    CHECK(TapKeys(kNextButton.x + 5, kNextButton.y + 5) == kKeyR);
    CHECK(TapKeys(kCloseButton.x, kCloseButton.y) == kKeyB); // the corner pixels count
    CHECK(TapKeys(kNextButton.x + kNextButton.w - 1, kNextButton.y + kNextButton.h - 1) == kKeyR);
    CHECK(TapKeys(kNextButton.x + kNextButton.w, kNextButton.y) == 0); // one pixel past
    CHECK(TapKeys(160, 100) == 0); // the list of steps is not a button
    CHECK(TapKeys(-1, -1) == 0);   // no tap
    CHECK(TapKeys(10, -1) == 0);
    CHECK(TapKeys(-1, 220) == 0);
    // the buttons are inside the touch screen (320x240) and do not touch each other
    for (const Rect& r : {kCloseButton, kBackButton, kNextButton}) {
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 320 && r.y + r.h <= 240);
    }
    CHECK(kCloseButton.x + kCloseButton.w < kBackButton.x);
    CHECK(kBackButton.x + kBackButton.w < kNextButton.x);
    // what the footer says: every key that does what the button does is on its chip (the steps are a list: up and down, and L and R as they always were)
    CHECK(std::strcmp(kCloseKeyText, "B") == 0 && std::strcmp(kCloseLabel, "Close") == 0);
    CHECK(std::strcmp(kBackKeyText, "Up/L") == 0 && std::strcmp(kBackLabel, "Back") == 0);
    CHECK(std::strcmp(kNextKeyText, "Down/R") == 0 && std::strcmp(kNextLabel, "Next") == 0);
    CHECK(Contains(kBackKeyText, "Up") && !Contains(kBackKeyText, "Down") && !Contains(kBackKeyText, "Left") && !Contains(kBackKeyText, "Right"));
    CHECK(Contains(kNextKeyText, "Down") && !Contains(kNextKeyText, "Up") && !Contains(kNextKeyText, "Left") && !Contains(kNextKeyText, "Right"));
    // ... and the chip and the word fit the button even in a wide font (the drawing shrinks them when they do not, so this is a margin, not a promise): the chip is
    // 6 pixels a character plus 10 for its padding, the word 7 a character, 6 between them, 8 of margin
    const struct { const char* key; const char* label; Rect rect; } kFooter[] = {
        {kCloseKeyText, kCloseLabel, kCloseButton}, {kBackKeyText, kBackLabel, kBackButton}, {kNextKeyText, kNextLabel, kNextButton}};
    for (const auto& button : kFooter) {
        const int width = static_cast<int>(std::strlen(button.key)) * 6 + 10 + 6 + static_cast<int>(std::strlen(button.label)) * 7 + 8;
        CHECK(std::strlen(button.key) >= 1 && std::strlen(button.label) >= 1);
        CHECK(width <= button.rect.w);
    }
    // and every tap, put through Move, does what its key does
    bool closed = false;
    CHECK(Move(2, TapKeys(kBackButton.x + 3, kBackButton.y + 3), &closed) == 1 && !closed);
    CHECK(Move(2, TapKeys(kNextButton.x + 3, kNextButton.y + 3), &closed) == 3 && !closed);
    CHECK(Move(2, TapKeys(kCloseButton.x + 3, kCloseButton.y + 3), &closed) == 2 && closed);
}

// ---------------------------------------------------------------------------------------------

// A made-up SD card: full path -> size (a folder is a path that is a prefix of another, and is not in the map as a file).
struct FakeFs {
    std::map<std::string, std::uint64_t> files;
    int sizes_asked = 0;
    bool FileSize(const std::string& path, guide::u64* size) {
        ++sizes_asked;
        auto it = files.find(path);
        if (it == files.end()) {
            return false;
        }
        *size = it->second;
        return true;
    }
    void ListNames(const std::string& dir, std::vector<std::string>* out) {
        const std::string prefix = dir + "/";
        for (const auto& kv : files) {
            if (StartsWith(kv.first, prefix.c_str()) && kv.first.find('/', prefix.size()) == std::string::npos) {
                out->push_back(kv.first.substr(prefix.size()));
            }
        }
    }
};

static bool Found(const std::map<std::string, std::uint64_t>& files) {
    FakeFs fs;
    fs.files = files;
    return guide::Boot9FoundIn(fs);
}

static void TestBoot9() {
    using namespace guide;
    // the places and the rules, as paths.hpp / paths.cpp have them (Tests/policy_check.sh pins the two folders against paths.hpp)
    CHECK(std::strcmp(kBoot9AppDir, "sdmc:/3ds/3DSLink") == 0);
    CHECK(std::strcmp(kBoot9GodModeDir, "sdmc:/gm9/out") == 0);
    CHECK(std::strcmp(kBoot9FileName, "boot9.bin") == 0);
    CHECK(kBoot9DirCount == 2 && kBoot9Dirs[0] == kBoot9AppDir && kBoot9Dirs[1] == kBoot9GodModeDir);
    CHECK(Boot9SizeOk(0x10000) && Boot9SizeOk(0x8000));
    CHECK(!Boot9SizeOk(0) && !Boot9SizeOk(0x4000) && !Boot9SizeOk(0x8001) && !Boot9SizeOk(0x20000) && !Boot9SizeOk(0x10000 + 1));

    // names
    CHECK(IsBoot9Name("boot9.bin") && IsBoot9Name("BOOT9.BIN") && IsBoot9Name("Boot9.Bin"));
    CHECK(IsBoot9Name("ABC12345678_boot9_00.bin")); // what GodMode9 2.x makes of it
    CHECK(IsBoot9Name("boot9_prot.bin"));
    CHECK(!IsBoot9Name("boot11.bin") && !IsBoot9Name("ABC12345678_boot11_00.bin"));
    CHECK(!IsBoot9Name("boot9_boot11.bin")); // (both words: not a boot9)
    CHECK(!IsBoot9Name("boot9.bin.bak") && !IsBoot9Name("boot9.txt") && !IsBoot9Name("boot9") && !IsBoot9Name(".bin") && !IsBoot9Name("boot.bin"));
    CHECK(!IsBoot9Name("") && !IsBoot9Name("a.bin") && !IsBoot9Name("otp.bin"));

    const std::uint64_t full = 0x10000, half = 0x8000;
    CHECK(!Found({}));
    // boot9.bin in either folder
    CHECK(Found({{"sdmc:/gm9/out/boot9.bin", full}}));
    CHECK(Found({{"sdmc:/gm9/out/boot9.bin", half}}));
    CHECK(Found({{"sdmc:/3ds/3DSLink/boot9.bin", full}}));
    CHECK(Found({{"sdmc:/3ds/3DSLink/boot9.bin", half}, {"sdmc:/gm9/out/boot11.bin", full}}));
    // a size that is no boot9's does not count (a truncated copy, an empty file, something else called that)
    CHECK(!Found({{"sdmc:/gm9/out/boot9.bin", 0}}));
    CHECK(!Found({{"sdmc:/gm9/out/boot9.bin", 1234}}));
    CHECK(!Found({{"sdmc:/gm9/out/boot9.bin", full + 1}}));
    CHECK(!Found({{"sdmc:/3ds/3DSLink/boot9.bin", 0x4000}}));
    // a bad file in one folder does not hide a good one in the other
    CHECK(Found({{"sdmc:/3ds/3DSLink/boot9.bin", 0}, {"sdmc:/gm9/out/boot9.bin", full}}));
    CHECK(Found({{"sdmc:/gm9/out/boot9.bin", 0}, {"sdmc:/3ds/3DSLink/boot9.bin", full}}));
    // GodMode9 2.x names the dump after the console
    CHECK(Found({{"sdmc:/gm9/out/ABC12345678_boot9_00.bin", full}, {"sdmc:/gm9/out/ABC12345678_boot11_00.bin", full}}));
    CHECK(!Found({{"sdmc:/gm9/out/ABC12345678_boot11_00.bin", full}})); // the other bootrom has the same size: not a boot9
    CHECK(!Found({{"sdmc:/gm9/out/ABC12345678_boot9_00.bin", 7}}));
    CHECK(Found({{"sdmc:/gm9/out/BOOT9.BIN", half}}));
    CHECK(Found({{"sdmc:/3ds/3DSLink/old_boot9_copy.bin", full}}));
    // other places are not looked in (the folder of the .3dsx is the app's own lookup, not this one's), other names do not count
    CHECK(!Found({{"sdmc:/3ds/boot9.bin", full}}));
    CHECK(!Found({{"sdmc:/boot9.bin", full}}));
    CHECK(!Found({{"sdmc:/gm9/boot9.bin", full}}));
    CHECK(!Found({{"sdmc:/gm9/out/sub/boot9.bin", full}})); // (the sub-folders are not looked at)
    CHECK(!Found({{"sdmc:/gm9/out/boot9.txt", full}, {"sdmc:/gm9/out/boot9.bin.bak", full}, {"sdmc:/gm9/out/otp.bin", full}}));
    CHECK(!Found({{"sdmc:/luma/payloads/boot9.bin", full}}));

    // it is a quick look: a handful of file calls on a card with nothing, however many files the folders hold
    FakeFs empty;
    CHECK(!Boot9FoundIn(empty) && empty.sizes_asked == 2);
    FakeFs crowd;
    for (int i = 0; i < 300; ++i) {
        crowd.files["sdmc:/gm9/out/dump_" + std::to_string(i) + ".bin"] = full;
    }
    CHECK(!Boot9FoundIn(crowd) && crowd.sizes_asked == 2); // none of them is a boot9 by name: their sizes are never asked
}

// ---------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    if (!Exists(g_dir + "/guide_logic.hpp")) {
        std::printf("guide_test: %s/guide_logic.hpp is not here: run it from AzaharLink/ (or give the guide folder)\n", g_dir.c_str());
        return 2;
    }
    TestWords();
    TestPictures();
    TestMove();
    TestTap();
    TestBoot9();
    std::printf("guide_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
