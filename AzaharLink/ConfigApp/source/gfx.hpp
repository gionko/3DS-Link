// Drawing layer of the 3DS Link app: citro3d/citro2d set-up, a few primitives, and text helpers
// (measuring, ellipsis, word wrap) built on the console's system font.
//
// Everything is drawn from scratch every frame into two render targets that citro3d swaps itself,
// so nothing is ever "cleared and redrawn" on the visible framebuffer: no flicker.
#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <string>
#include <vector>

namespace gfx {

constexpr int kTopW = 400;
constexpr int kBottomW = 320;
constexpr int kScreenH = 240;

constexpr u32 Rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return C2D_Color32(r, g, b, a);
}

// Same alpha, different opacity.
inline u32 WithAlpha(u32 color, u8 alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<u32>(alpha) << 24);
}

// Linear mix of two colours (alpha is taken from `a`).
inline u32 Mix(u32 a, u32 b, float t) {
    u32 out = a & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<u32>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

// The palette: dark near-black surfaces, a light title bar and one blue accent, like the in-game
// CTRPluginFramework menu.
namespace col {
constexpr u32 kBg = Rgba(13, 14, 17);
constexpr u32 kPanel = Rgba(24, 26, 31);
constexpr u32 kPanelAlt = Rgba(30, 33, 39);
constexpr u32 kLine = Rgba(46, 50, 58);
constexpr u32 kBorder = Rgba(176, 182, 194);
constexpr u32 kBar = Rgba(222, 225, 231);
constexpr u32 kBarText = Rgba(20, 22, 27);
constexpr u32 kText = Rgba(232, 234, 238);
constexpr u32 kDim = Rgba(138, 144, 156);
constexpr u32 kAccent = Rgba(70, 178, 255);
constexpr u32 kSel = Rgba(34, 56, 90);
constexpr u32 kSelPressed = Rgba(48, 78, 124);
constexpr u32 kOn = Rgba(74, 218, 128);
constexpr u32 kOff = Rgba(244, 100, 100);
constexpr u32 kWarn = Rgba(255, 198, 88);
constexpr u32 kAlert = Rgba(255, 140, 56); // a receiver that finished with a title that failed
constexpr u32 kTrack = Rgba(74, 80, 92);
constexpr u32 kKnob = Rgba(240, 242, 246);
// What a title is: a game (blue), an update (green), a DLC (red). The same three everywhere a title's kind is shown.
constexpr u32 kGame = Rgba(86, 170, 255);
constexpr u32 kUpdate = Rgba(74, 218, 128);
constexpr u32 kDlc = Rgba(244, 100, 100);
} // namespace col

// Starts citro3d/citro2d, the two screen targets, the font and the text buffer. Call after
// gfxInitDefault(). Returns false (and leaves nothing initialised) on failure.
bool Init();
// Why Init() returned false (a short sentence), for the text screen shown instead.
const char* InitError();
void Shutdown();

// One frame: BeginFrame (waits for the vertical blank), BeginTop + draw, BeginBottom + draw,
// EndFrame. BeginFrame returns false when the frame could not be started; skip drawing then. With `wait` false it never waits: when the
// GPU is still busy with the last frame it returns false at once (a transfer's screens use this: the radio loop must not stand still).
bool BeginFrame(bool wait = true);
void BeginTop();
void BeginBottom();
void EndFrame();

// Resets the GPU state citro2d relies on. Call after an applet (the software keyboard) returned.
void ResumeAfterApplet();

// Primitives. Coordinates are in pixels, origin at the top left of the current screen.
void FillRect(float x, float y, float w, float h, u32 color);
// Corner colours: top-left, top-right, bottom-left, bottom-right.
void FillGradient(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br);
void StrokeRect(float x, float y, float w, float h, float thickness, u32 color);
void FillCircle(float cx, float cy, float radius, u32 color);
void Line(float x0, float y0, float x1, float y1, float thickness, u32 color);

// A game's icon as its SMDH stores it (RGB565 in 8x8 tiles; `size` is 24 or 48), drawn with its top left corner at (x, y), at
// `scale` times its size. Its texture is made the first time it is drawn and kept, keyed by where `tiled` is: the data must stay
// where it is, unchanged, until ReleaseIcons() (the list of games does; a page that owns icon data calls it when it goes).
void DrawIcon(const u16* tiled, int size, float x, float y, float scale, u8 alpha = 255);
// Frees every icon texture (Shutdown does it too).
void ReleaseIcons();

enum class Align { Left, Center, Right };

// Draws one line of text vertically centred on `yc`. Control characters must not be present
// (see Clean()). Scale 1.0 is the native 30 px size of the system font.
void DrawText(float x, float yc, float scale, u32 color, const char* text, Align align = Align::Left);
inline void DrawText(float x, float yc, float scale, u32 color, const std::string& text,
                     Align align = Align::Left) {
    DrawText(x, yc, scale, color, text.c_str(), align);
}

float TextWidth(const char* text, float scale);
inline float TextWidth(const std::string& text, float scale) {
    return TextWidth(text.c_str(), scale);
}

// Draws text that must fit in `max_width`: first shrinks it (down to `min_scale`), then, if it is
// still too wide, shortens it with "...". Layouts use this for anything whose length depends on
// the font or on user/remote data.
void DrawFitted(float x, float yc, float scale, float min_scale, float max_width, u32 color,
                const std::string& text, Align align = Align::Left);

// Distance between two lines of text at this scale.
float LineHeight(float scale);

// Shortens `text` with "..." so that it fits in `max_width`.
std::string Ellipsize(const std::string& text, float scale, float max_width);

// A text too wide for `max_width` that is read in full by letting it run past: the part of `text` that is shown `elapsed_ms` after the
// selection came to it. It rests on its beginning for a second, moves one letter at a time up to its end, rests there, and starts again.
// A text that fits is returned as it is.
std::string MarqueeText(const std::string& text, float scale, float max_width, u64 elapsed_ms);

// Greedy word wrap on spaces and '\n'. The returned reference stays valid until the next WrapText
// call. Results are cached, so calling this every frame with the same arguments is cheap.
const std::vector<std::string>& WrapText(const std::string& text, float scale, float max_width);

// Draws wrapped text starting at `y_top` (centre of the first line is y_top + LineHeight/2),
// at most `max_lines` lines (the last one is ellipsized when there is more). Returns the height used.
float DrawWrapped(float x, float y_top, float max_width, float scale, u32 color,
                  const std::string& text, int max_lines);

// Replaces control characters (remote strings!) by spaces so they never reach the font parser.
std::string Clean(const std::string& text);

} // namespace gfx
