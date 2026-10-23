#include "ui_lite.hpp"
#include <cstdarg>
#include <cstdio>
#include <cmath>

namespace ui {

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static void DrawBarBackground(float width) {
    const u32 bar_bottom = gfx::Mix(col::kBar, gfx::Rgba(168, 174, 188), 0.55f);
    gfx::FillGradient(0, 0, width, kBarH, col::kBar, col::kBar, bar_bottom, bar_bottom);
    gfx::FillRect(0, kBarH, width, 1, gfx::WithAlpha(col::kAccent, 200));
    gfx::FillRect(0, 0, 5, kBarH, col::kAccent);
}

static float DrawHomeOffBadge(float right_edge, float yc) {
    const float text_w = gfx::TextWidth("HOME off", 0.4f);
    const float w = 18.0f + text_w + 8.0f;
    const float x = right_edge - w;
    const u32 red = gfx::Rgba(190, 40, 40);
    gfx::FillRect(x, yc - 8.0f, w, 16.0f, gfx::Mix(col::kBar, red, 0.18f));
    gfx::StrokeRect(x, yc - 8.0f, w, 16.0f, 1.0f, red);
    gfx::Line(x + 3.0f, yc, x + 8.0f, yc - 4.5f, 1.5f, red);
    gfx::Line(x + 8.0f, yc - 4.5f, x + 13.0f, yc, 1.5f, red);
    gfx::StrokeRect(x + 4.5f, yc, 7.0f, 5.0f, 1.0f, red);
    gfx::Line(x + 2.0f, yc + 6.0f, x + 14.0f, yc - 6.0f, 1.5f, red);
    gfx::DrawText(x + 17.0f, yc, 0.4f, red, "HOME off");
    return x;
}

void DrawTitleBar(float width, const char* title, const std::string& right, bool home_off) {
    DrawBarBackground(width);
    gfx::DrawText(14, kBarH * 0.5f, 0.56f, col::kBarText, title);
    float edge = width - 10.0f;
    if (!right.empty()) {
        gfx::DrawText(edge, kBarH * 0.5f, 0.45f, gfx::Mix(col::kBarText, col::kBar, 0.35f), right, gfx::Align::Right);
        edge -= gfx::TextWidth(right, 0.45f) + 10.0f;
    }
    if (home_off && width < 330.0f) {
        DrawHomeOffBadge(edge, kBarH * 0.5f);
    }
}

void DrawCard(float x, float y, float w, float h, u32 stripe) {
    gfx::FillRect(x, y, w, h, col::kPanel);
    gfx::StrokeRect(x, y, w, h, 1.0f, col::kLine);
    gfx::FillRect(x, y, 3, h, stripe);
}

void DrawProgressBar(float x, float y, float w, float h, float fraction) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    gfx::FillRect(x, y, w, h, col::kTrack);
    const float filled = std::floor((w - 2.0f) * fraction);
    if (filled > 0.0f) {
        gfx::FillGradient(x + 1.0f, y + 1.0f, filled, h - 2.0f, gfx::Mix(col::kAccent, col::kText, 0.35f),
                          gfx::Mix(col::kAccent, col::kText, 0.35f), col::kAccent, col::kAccent);
    }
    gfx::StrokeRect(x, y, w, h, 1.0f, col::kBorder);
}

void DrawButton(float x, float y, float w, float h, const char* key, const char* label, bool enabled) {
    gfx::FillRect(x, y, w, h, enabled ? col::kPanelAlt : col::kPanel);
    gfx::StrokeRect(x, y, w, h, 1.0f, enabled ? col::kBorder : col::kLine);
    float key_scale = 0.34f, label_scale = 0.42f, pad = 10.0f, gap = 6.0f;
    float chip_w = gfx::TextWidth(key, key_scale) + pad;
    float total = chip_w + gap + gfx::TextWidth(label, label_scale);
    if (total > w - 8.0f) {
        // The system font of a console is wider than the stand-in of an emulator: shrink everything to fit the button (as the app's own DrawButton does).
        const float shrink = (w - 8.0f) / total;
        key_scale *= shrink;
        label_scale *= shrink;
        chip_w = gfx::TextWidth(key, key_scale) + pad;
        total = chip_w + gap + gfx::TextWidth(label, label_scale);
    }
    const float bx = x + (w - total) * 0.5f;
    const float yc = y + h * 0.5f;
    gfx::FillRect(bx, yc - 7.0f, chip_w, 14.0f, enabled ? col::kBar : col::kTrack);
    gfx::DrawText(bx + chip_w * 0.5f, yc, key_scale, col::kBarText, key, gfx::Align::Center);
    gfx::DrawText(bx + chip_w + gap, yc, label_scale, enabled ? col::kText : col::kDim, label);
}

void DrawFooterBackground() {
    gfx::FillRect(0, 214, gfx::kBottomW, 26, col::kBg);
    gfx::FillRect(0, 213, gfx::kBottomW, 1, col::kLine);
}

void DrawFrameBorder(float width) {
    gfx::StrokeRect(0, 0, width, static_cast<float>(gfx::kScreenH), 1.0f, gfx::WithAlpha(col::kBorder, 150));
}

void DrawDotText(float x, float yc, u32 dot, u32 text_color, const std::string& text, float scale, float max_w) {
    gfx::FillCircle(x + 4.0f, yc, 4.0f, dot);
    gfx::DrawText(x + 14.0f, yc, scale, text_color, gfx::Ellipsize(text, scale, max_w - 14.0f));
}

} // namespace ui
