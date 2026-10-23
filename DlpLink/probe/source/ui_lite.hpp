// Throwaway probe: the few widgets of AzaharLink/ConfigApp/source/ui.cpp that the installer child would need, re-typed
// without the App state (g.home_blocked, g.status ...) that ui.cpp depends on. Bodies are copied from ui.cpp (see line numbers there).
#pragma once
#include <string>
#include "gfx.hpp"

namespace ui {
namespace col = gfx::col;
constexpr float kBarH = 24.0f;
std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void DrawTitleBar(float width, const char* title, const std::string& right, bool home_off);
void DrawCard(float x, float y, float w, float h, u32 stripe);
void DrawProgressBar(float x, float y, float w, float h, float fraction);
void DrawButton(float x, float y, float w, float h, const char* key, const char* label, bool enabled);
void DrawFooterBackground();
void DrawFrameBorder(float width);
void DrawDotText(float x, float yc, u32 dot, u32 text_color, const std::string& text, float scale, float max_w);
} // namespace ui
