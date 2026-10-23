// guide_logic.hpp -- what the "How to get boot9.bin" guide decides without a console: the steps (their words and pictures), what the keys do, where the footer's
// buttons are, and where boot9.bin is looked for. Plain C++ with no libctru, citro2d or file call in it, so Tests/settings_test/guide_test.cpp builds this very
// file on a PC; guide.hpp includes it and guide.cpp (the viewer, the drawing, the real file calls) is the part that talks to the console.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace guide {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The keys of the console's pad that the guide uses: libctru's own bits (guide.cpp has a static_assert for each, so a PC test and the console agree).
constexpr u32 kKeyB = 1u << 1;
constexpr u32 kKeyDUp = 1u << 6;
constexpr u32 kKeyDDown = 1u << 7;
constexpr u32 kKeyCPadUp = 1u << 30;     // (the Circle Pad: libctru's KEY_UP / KEY_DOWN include it, as every other list of the app takes them)
constexpr u32 kKeyCPadDown = 1u << 31;
constexpr u32 kKeyR = 1u << 8;
constexpr u32 kKeyL = 1u << 9;

// ---------------------------------------------------------------------------------------------
// The steps. One picture each (a crop of GodMode9's screen with the arrows drawn on it, DlpLink/tools/make_guide.py) except the last one, which has none.
// The pictures are AzaharLink/ConfigApp/guide/gfx/step<image + 1>.png (and .t3s, the file tex3ds turns into the t3x that is linked in).
// ---------------------------------------------------------------------------------------------

struct Step {
    const char* title;   // "Start GodMode9"
    const char* text[3]; // up to three short lines (nullptr = none), English, under the picture
    int image;           // which picture (index of the step's t3x: step1 is image 0), -1 = text only
};

// How wide the words may be. The test counts characters (the console's font is proportional: 46 characters of 0.5 scale text are about 330 of the 376
// pixels the lines have, and the drawing shrinks a line that would not fit anyway).
constexpr int kMaxTitleChars = 30;
constexpr int kMaxLineChars = 46;
// The biggest picture: 392x164 pixels (a crop of at most 196x82 of the screenshot, made 2x). The top screen is 400x240: the title bar above, three lines of text under.
constexpr int kMaxPictureW = 392;
constexpr int kMaxPictureH = 164;
// How many pictures the guide has (the t3x symbols guide.cpp links in: step1_t3x ... step7_t3x).
constexpr int kImageCount = 7;

// What GodMode9 v2.1.1 shows (the screenshots DlpLink/docs/GUIDA-BOOT9.md lists) and what the app does with the result: the last step names the folder
// the dump ends up in (GodMode9's own default, "0:/gm9/out" = sdmc:/gm9/out) and says that 3DS Link looks there (setup::Boot9Candidates and FindBoot9 in
// ConfigApp/source/paths.cpp: next to the app, in its own folder, in sdmc:/gm9/out).
inline constexpr Step kSteps[] = {
    {"Start GodMode9",
     {"Turn the console off.", "Hold START and turn it on: GodMode9 opens.", "No GodMode9? github.com/d0k3/GodMode9"},
     0},
    {"Open the HOME menu",
     {"Press the HOME button of the console.", "GodMode9 shows the menu 'Select action'.", nullptr},
     1},
    {"Choose Scripts...",
     {"Press D-pad down 3 times, to Scripts...,", "then press A.", nullptr},
     2},
    {"Choose GM9Megascript",
     {"Select GM9Megascript and press A.", "A script by annson24. If it is missing, put", "it in gm9/scripts on the SD card."},
     3},
    {"Choose Dump Options",
     {"Press D-pad down to Dump Options,", "then press A.", nullptr},
     4},
    {"Dump Boot9.bin & Boot11.bin",
     {"Press D-pad down to the marked line,", "'Dump Boot9.bin & Boot11.bin', then press A.", nullptr},
     5},
    {"Confirm the dump",
     {"It asks to create the dumps in 0:/gm9/out", "(0: is the SD card). Press A for yes.", nullptr},
     6},
    {"Done",
     {"The dump is in gm9/out on the SD card.", "Reboot or power off from the HOME menu.", "3DS Link finds it there by itself."},
     -1},
};

constexpr int kStepTotal = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

// The pictures are the steps' own, in the order of the steps (image 0 is the first step that has one...): step<N>.png is image N - 1.
constexpr bool ImagesInOrder() {
    int next = 0;
    for (const Step& step : kSteps) {
        if (step.image >= 0) {
            if (step.image != next) {
                return false;
            }
            ++next;
        }
    }
    return next == kImageCount;
}
static_assert(ImagesInOrder(), "every picture belongs to one step, in order (and kImageCount counts them)");

inline int StepCount() {
    return kStepTotal;
}
// A step number out of range gives the nearest step (the viewer never asks for one).
inline const Step& StepAt(int index) {
    return kSteps[index < 0 ? 0 : (index >= kStepTotal ? kStepTotal - 1 : index)];
}

// ---------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------

// What the keys do to the step number, with no wrap-around. The steps are a VERTICAL list (the touch screen shows them one under the other), so the D-pad (and the Circle Pad) moves
// along it: up = back, down = forward; L = back and R = forward as well. The D-pad's left and right do nothing here. Both directions at once (L and R, or up and
// down of the D-pad, or L with D-pad down...) do nothing; two keys of the same direction are one step. `closed` (may be null) becomes true on B (the step is then
// left as it was). A step out of range is first brought in.
inline int Move(int step, u32 keys_down, bool* closed) {
    if (closed) {
        *closed = false;
    }
    if (step < 0) {
        step = 0;
    }
    if (step > kStepTotal - 1) {
        step = kStepTotal - 1;
    }
    if (keys_down & kKeyB) {
        if (closed) {
            *closed = true;
        }
        return step;
    }
    const bool back = (keys_down & (kKeyL | kKeyDUp | kKeyCPadUp)) != 0;
    const bool forward = (keys_down & (kKeyR | kKeyDDown | kKeyCPadDown)) != 0;
    if (back == forward) {
        return step;
    }
    if (back) {
        return step > 0 ? step - 1 : 0;
    }
    return step < kStepTotal - 1 ? step + 1 : step;
}

// ---------------------------------------------------------------------------------------------
// The footer of the touch screen: [B Close] [Up/L Back] [Down/R Next]. A tap on a button does what its key does (the tap stands for L and R).
// ---------------------------------------------------------------------------------------------

struct Rect {
    int x, y, w, h;
    bool Has(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};
constexpr Rect kCloseButton = {8, 216, 72, 20};
constexpr Rect kBackButton = {88, 216, 110, 20};
constexpr Rect kNextButton = {206, 216, 106, 20};
// What the three buttons say (guide.cpp draws them; the test counts the characters: the chip and the word must fit the button, and the font of a console is
// wider than a PC's). The key chip names every key that does what the button does, so the footer says the truth about the keys.
constexpr const char* kCloseKeyText = "B";
constexpr const char* kCloseLabel = "Close";
constexpr const char* kBackKeyText = "Up/L";
constexpr const char* kBackLabel = "Back";
constexpr const char* kNextKeyText = "Down/R";
constexpr const char* kNextLabel = "Next";

// The key a tap at (x, y) of the touch screen stands for (0 = it hit no button; a negative x or y = no tap).
inline u32 TapKeys(int x, int y) {
    if (x < 0 || y < 0) {
        return 0;
    }
    if (kCloseButton.Has(x, y)) {
        return kKeyB;
    }
    if (kBackButton.Has(x, y)) {
        return kKeyL;
    }
    if (kNextButton.Has(x, y)) {
        return kKeyR;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// boot9.bin: where it is looked for. The installer child (which has no setup::Paths) asks this: a quick look by name and size. The app has its own, exact
// lookup (setup::FindBoot9: the places below plus the folder the app was started from and the path the user chose, and the content of the file).
// ---------------------------------------------------------------------------------------------

// The two folders a 3DS Link installed from a CIA looks in. Tests/policy_check.sh pins that they say the same as paths.hpp (setup::kDefaultAppDir, the
// folder of an app with no folder of its own: there the app's folder and its data folder are one; and setup::kGodModeOutDir, where GodMode9 dumps to).
constexpr const char* kBoot9AppDir = "sdmc:/3ds/3DSLink";
constexpr const char* kBoot9GodModeDir = "sdmc:/gm9/out";
constexpr const char* kBoot9Dirs[] = {kBoot9AppDir, kBoot9GodModeDir};
constexpr int kBoot9DirCount = 2;
static_assert(sizeof(kBoot9Dirs) / sizeof(kBoot9Dirs[0]) == kBoot9DirCount, "the count is the list's");
constexpr const char* kBoot9FileName = "boot9.bin"; // the same name as setup::kBoot9FileName

// A boot9 dump is 64 KB (a whole dump) or 32 KB (the protected half): the same sizes as setup::Boot9SizeOk.
inline bool Boot9SizeOk(u64 size) {
    return size == 0x10000 || size == 0x8000;
}

// The NAME of a boot9 dump, as setup::IsBoot9Name has it: "boot9.bin", or what GodMode9 2.x makes of it ("ABC12345678_boot9_00.bin"): ends in ".bin" and has
// "boot9" in it (any case) but not "boot11" (the other bootrom, which has the same size).
inline bool IsBoot9Name(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".bin") != 0) {
        return false;
    }
    return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;
}

// Is there a boot9 in the folders above? `fs` is the real file system in guide.cpp and a made-up one in the test:
//   bool FileSize(const std::string& path, u64* size)                        a file's size; false when there is no such file (a folder does not count)
//   void ListNames(const std::string& dir, std::vector<std::string>* out)    the names in a folder (none when it is not there)
// boot9.bin itself first, then any name that IsBoot9Name; the size must be one a boot9 has.
template <class Fs>
bool Boot9FoundIn(Fs& fs) {
    for (int i = 0; i < kBoot9DirCount; ++i) {
        const std::string dir = kBoot9Dirs[i];
        u64 size = 0;
        if (fs.FileSize(dir + "/" + kBoot9FileName, &size) && Boot9SizeOk(size)) {
            return true;
        }
        std::vector<std::string> names;
        fs.ListNames(dir, &names);
        for (const std::string& name : names) {
            if (IsBoot9Name(name) && fs.FileSize(dir + "/" + name, &size) && Boot9SizeOk(size)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace guide
