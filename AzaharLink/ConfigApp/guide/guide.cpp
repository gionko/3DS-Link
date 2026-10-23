// guide.cpp -- the viewer of "How to get boot9.bin" (guide.hpp) and the look at the SD card for boot9.bin. What needs no console (the steps, the keys, the list
// of places) is in guide_logic.hpp and is tested on a PC; this file draws with gfx:: (the same functions in the app and in the installer child, which is why it
// uses nothing of ui:: or ui_lite:: : their bars and buttons have other signatures in the two programs, so the few pieces it needs are drawn here with the
// same look) and keeps ONE picture in memory at a time.
#include "guide.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "gfx.hpp"

// The pictures: step<N>.png -> step<N>.t3s -> (tex3ds, the Makefile's GRAPHICS rule) step<N>.t3x -> (bin2o) the symbols below. The Makefile of the program that
// takes this module in lists the folder AzaharLink/ConfigApp/guide/gfx as GRAPHICS; a program without it does not link (a build that wants no pictures defines
// GUIDE_NO_PICTURES and Open() says no). The data is from the symbol to its _end (bin2s makes both; the size it also writes in the header is not a symbol).
// (Declared here, outside the namespace: they are plain C symbols.)
#ifndef GUIDE_NO_PICTURES
extern "C" {
extern const u8 step1_t3x[];
extern const u8 step1_t3x_end[];
extern const u8 step2_t3x[];
extern const u8 step2_t3x_end[];
extern const u8 step3_t3x[];
extern const u8 step3_t3x_end[];
extern const u8 step4_t3x[];
extern const u8 step4_t3x_end[];
extern const u8 step5_t3x[];
extern const u8 step5_t3x_end[];
extern const u8 step6_t3x[];
extern const u8 step6_t3x_end[];
extern const u8 step7_t3x[];
extern const u8 step7_t3x_end[];
}
#endif

namespace guide {

// The keys are libctru's: guide_logic.hpp has its own copy of the bits so that a PC can build it.
static_assert(kKeyB == KEY_B && kKeyDUp == KEY_DUP && kKeyDDown == KEY_DDOWN && kKeyR == KEY_R && kKeyL == KEY_L && kKeyCPadUp == KEY_CPAD_UP && kKeyCPadDown == KEY_CPAD_DOWN, "guide_logic.hpp's key bits are libctru's");

namespace col = gfx::col;

namespace {

#ifndef GUIDE_NO_PICTURES
struct Picture {
    const u8* begin;
    const u8* end;
};
const Picture kPictures[] = {
    {step1_t3x, step1_t3x_end}, {step2_t3x, step2_t3x_end}, {step3_t3x, step3_t3x_end}, {step4_t3x, step4_t3x_end},
    {step5_t3x, step5_t3x_end}, {step6_t3x, step6_t3x_end}, {step7_t3x, step7_t3x_end},
};
static_assert(sizeof(kPictures) / sizeof(kPictures[0]) == static_cast<std::size_t>(kImageCount), "one t3x symbol for each picture of the steps");
#endif

// ---------------------------------------------------------------------------------------------
// The look: the title bar, the buttons and the border of the app (ui.cpp's DrawBarBackground, DrawTitleBar, DrawButton, DrawFooterBackground, DrawFrameBorder),
// drawn here from gfx:: so that the same file serves the installer.
// ---------------------------------------------------------------------------------------------
constexpr float kBarH = 24.0f;
constexpr float kDepth = 0.5f;       // gfx.cpp's: everything on one plane, painter's order
constexpr float kTextScale = 0.5f;   // the three lines under the picture
constexpr float kRowH = 22.0f;       // a row of the list of steps (touch screen)

void DrawBar(float width, const char* title, const char* right) {
    const u32 bar_bottom = gfx::Mix(col::kBar, gfx::Rgba(168, 174, 188), 0.55f);
    gfx::FillGradient(0, 0, width, kBarH, col::kBar, col::kBar, bar_bottom, bar_bottom);
    gfx::FillRect(0, kBarH, width, 1, gfx::WithAlpha(col::kAccent, 200));
    gfx::FillRect(0, 0, 5, kBarH, col::kAccent);
    float edge = width - 10.0f;
    if (right && *right) {
        gfx::DrawText(edge, kBarH * 0.5f, 0.45f, gfx::Mix(col::kBarText, col::kBar, 0.35f), right, gfx::Align::Right);
        edge -= gfx::TextWidth(right, 0.45f) + 10.0f;
    }
    gfx::DrawFitted(14, kBarH * 0.5f, 0.56f, 0.4f, edge - 14.0f, col::kBarText, title);
}

void DrawFrameBorder(float width) {
    gfx::StrokeRect(0, 0, width, static_cast<float>(gfx::kScreenH), 1.0f, gfx::WithAlpha(col::kBorder, 150));
}

void DrawKeyButton(const Rect& r, const char* key, const char* label, bool enabled) {
    const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y), w = static_cast<float>(r.w), h = static_cast<float>(r.h);
    gfx::FillRect(x0, y0, w, h, enabled ? col::kPanelAlt : col::kPanel);
    gfx::StrokeRect(x0, y0, w, h, 1.0f, enabled ? col::kBorder : col::kLine);
    float key_scale = 0.38f, label_scale = 0.46f;
    float pad = 10.0f, gap = 6.0f; // the chip is the key's width plus `pad`; `gap` separates chip and label
    float chip_w = gfx::TextWidth(key, key_scale) + pad;
    float total = chip_w + gap + gfx::TextWidth(label, label_scale);
    if (total > w - 8.0f) { // the font of a console is wider than an emulator's stand-in: shrink everything to fit the button
        const float shrink = (w - 8.0f) / total;
        key_scale *= shrink;
        label_scale *= shrink;
        pad *= shrink;
        gap *= shrink;
        chip_w = gfx::TextWidth(key, key_scale) + pad;
        total = chip_w + gap + gfx::TextWidth(label, label_scale);
    }
    const float x = x0 + (w - total) * 0.5f;
    const float yc = y0 + h * 0.5f;
    gfx::FillRect(x, yc - 7.0f, chip_w, 14.0f, enabled ? col::kBar : col::kTrack);
    gfx::DrawText(x + chip_w * 0.5f, yc, key_scale, col::kBarText, key, gfx::Align::Center);
    gfx::DrawText(x + chip_w + gap, yc, label_scale, enabled ? col::kText : col::kDim, label);
}

// ---------------------------------------------------------------------------------------------
// The SD card, for Boot9Found (guide_logic.hpp's Boot9FoundIn asks these two questions)
// ---------------------------------------------------------------------------------------------
struct RealFs {
    bool FileSize(const std::string& path, u64* size) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            return false;
        }
        *size = static_cast<u64>(st.st_size);
        return true;
    }
    void ListNames(const std::string& dir, std::vector<std::string>* out) {
        DIR* d = opendir(dir.c_str());
        if (!d) {
            return;
        }
        while (const struct dirent* entry = readdir(d)) {
            out->push_back(entry->d_name);
            if (out->size() >= 4096) { // (a folder that big is not GodMode9's output folder: the look must stay short)
                break;
            }
        }
        closedir(d);
    }
};

} // namespace

bool Boot9Found() {
    RealFs fs;
    return Boot9FoundIn(fs);
}

// ---------------------------------------------------------------------------------------------
// The viewer
// ---------------------------------------------------------------------------------------------

void Viewer::FreePicture() {
    if (sheet_) {
        C3D_FrameSync(); // (the last frame may still be drawing with the texture)
        C2D_SpriteSheetFree(sheet_);
        sheet_ = nullptr;
    }
    sheet_image_ = -1;
}

bool Viewer::LoadPicture() {
    const int image = StepAt(step_).image;
    if (image == sheet_image_) { // (a picture that could not be made is not tried again on every frame: only when the step comes back)
        return sheet_ != nullptr || image < 0;
    }
    FreePicture();
    sheet_image_ = image;
    if (image < 0 || image >= kImageCount) {
        return true; // a step with no picture
    }
#ifdef GUIDE_NO_PICTURES
    return false;
#else
    sheet_ = C2D_SpriteSheetLoadFromMem(kPictures[image].begin, static_cast<std::size_t>(kPictures[image].end - kPictures[image].begin));
    if (!sheet_) {
        return false;
    }
    // The picture is drawn 1:1 (it was enlarged when it was made): no filter, so that no pixel of it is blurred.
    const C2D_Image img = C2D_SpriteSheetGetImage(sheet_, 0);
    C3D_TexSetFilter(img.tex, GPU_NEAREST, GPU_NEAREST);
    return true;
#endif
}

bool Viewer::Open() {
    Close();
    step_ = 0;
    if (!LoadPicture()) {
        FreePicture();
        return false;
    }
    open_ = true;
    return true;
}

void Viewer::Close() {
    open_ = false;
    FreePicture();
}

bool Viewer::Update(u32 keys_down, int tap_x, int tap_y) {
    if (!open_) {
        return false;
    }
    bool closed = false;
    step_ = Move(step_, keys_down | TapKeys(tap_x, tap_y), &closed);
    if (closed) {
        Close();
        return false;
    }
    LoadPicture(); // (false: no memory for this picture; the step shows its words alone, and the next step tries again)
    return true;
}

void Viewer::DrawTop() {
    const Step& step = StepAt(step_);
    char head[64];
    std::snprintf(head, sizeof(head), "%d. %s", step_ + 1, step.title);
    DrawBar(gfx::kTopW, head, nullptr);

    int lines = 0;
    while (lines < 3 && step.text[lines]) {
        ++lines;
    }
    const float screen_h = static_cast<float>(gfx::kScreenH);
    const float w = static_cast<float>(gfx::kTopW);

    if (step.image < 0) { // words only: a card with the lines in a bigger type
        const float card_y = kBarH + 8.0f, card_h = screen_h - card_y - 10.0f;
        gfx::FillRect(10, card_y, w - 20.0f, card_h, col::kPanel);
        gfx::StrokeRect(10, card_y, w - 20.0f, card_h, 1.0f, col::kLine);
        gfx::FillRect(10, card_y, 3, card_h, col::kAccent);
        const float lh = gfx::LineHeight(0.6f) + 12.0f;
        const float top = card_y + (card_h - lh * static_cast<float>(lines)) * 0.5f;
        for (int i = 0; i < lines; ++i) {
            gfx::DrawFitted(28, top + lh * (static_cast<float>(i) + 0.5f), 0.6f, 0.4f, w - 56.0f, col::kText, step.text[i]);
        }
        DrawFrameBorder(w);
        return;
    }

    // The words are at the bottom (three lines, as many as the step has, under the picture), the picture is centred above them.
    const float lh = gfx::LineHeight(kTextScale);
    const float text_top = screen_h - 5.0f - lh * 3.0f;
    const float area_top = kBarH + 3.0f, area_bottom = text_top - 2.0f;
    if (sheet_) {
        const C2D_Image img = C2D_SpriteSheetGetImage(sheet_, 0);
        const float pw = static_cast<float>(img.subtex->width), ph = static_cast<float>(img.subtex->height);
        const float px = std::floor((w - pw) * 0.5f);
        const float py = std::floor(area_top + (area_bottom - area_top - ph) * 0.5f);
        gfx::StrokeRect(px - 1.0f, py - 1.0f, pw + 2.0f, ph + 2.0f, 1.0f, col::kLine);
        C2D_DrawImageAt(img, px, py, kDepth, nullptr, 1.0f, 1.0f);
    } else { // no memory for the picture: the words still say it
        gfx::DrawText(w * 0.5f, (area_top + area_bottom) * 0.5f, 0.5f, col::kDim, "(the picture cannot be shown now)", gfx::Align::Center);
    }
    for (int i = 0; i < lines; ++i) {
        gfx::DrawFitted(14, text_top + lh * (static_cast<float>(i) + 0.5f), kTextScale, 0.34f, w - 28.0f, col::kText, step.text[i]);
    }
    DrawFrameBorder(w);
}

void Viewer::DrawBottom() {
    const int total = StepCount();
    char right[24];
    std::snprintf(right, sizeof(right), "Step %d of %d", step_ + 1, total);
    DrawBar(gfx::kBottomW, "How to get boot9.bin", right);

    // The whole way at a glance: the steps one under the other, the one on the top screen lit.
    const float list_top = kBarH + 6.0f, list_bottom = 210.0f;
    float row_h = kRowH;
    if (total > 0 && row_h * static_cast<float>(total) > list_bottom - list_top) {
        row_h = (list_bottom - list_top) / static_cast<float>(total);
    }
    const float rh = row_h - 2.0f;
    for (int i = 0; i < total; ++i) {
        const float y = list_top + row_h * static_cast<float>(i);
        gfx::FillRect(6, y, 308, rh, col::kPanel);
        const bool here = i == step_;
        if (here) {
            gfx::FillGradient(6, y, 308, rh, gfx::Mix(col::kSel, col::kAccent, 0.22f), col::kSel, gfx::Mix(col::kSel, col::kAccent, 0.22f), col::kSel);
            gfx::FillRect(6, y, 3, rh, col::kAccent);
        }
        char number[8];
        std::snprintf(number, sizeof(number), "%d", i + 1);
        const float yc = y + rh * 0.5f;
        gfx::DrawText(20, yc, 0.46f, here ? col::kAccent : col::kDim, number);
        gfx::DrawFitted(36, yc, 0.46f, 0.34f, 270.0f, here ? col::kText : col::kDim, StepAt(i).title);
    }

    gfx::FillRect(0, 214, gfx::kBottomW, 26, col::kBg);
    gfx::FillRect(0, 213, gfx::kBottomW, 1, col::kLine);
    DrawKeyButton(kCloseButton, kCloseKeyText, kCloseLabel, true);
    DrawKeyButton(kBackButton, kBackKeyText, kBackLabel, step_ > 0);
    DrawKeyButton(kNextButton, kNextKeyText, kNextLabel, step_ < total - 1);
    DrawFrameBorder(static_cast<float>(gfx::kBottomW));
}

} // namespace guide
