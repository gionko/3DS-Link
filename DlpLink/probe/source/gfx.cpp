#include "gfx.hpp"

#include <cmath>


namespace gfx {

namespace {

constexpr std::size_t kTextGlyphs = 4096;   // glyphs of text per frame (both screens)
constexpr std::size_t kMaxObjects = 8192;   // quads per frame: glyphs, rectangles, circles, lines
// citro2d checks its vertex buffer when it draws a rectangle, circle or line, but NOT when it draws
// text. Text can therefore never use more quads than the text buffer holds, and the rest of the
// frame must always fit in what is left.
static_assert(kTextGlyphs + 2048 <= kMaxObjects, "the shapes of a frame must fit next to its text");
constexpr float kDepth = 0.5f;              // everything on one plane, painter's order
// The vertical centre of lower-case text is a bit above the middle of the 30 px glyph cell; this
// moves the baseline so that text looks centred on the y that DrawText is given.
constexpr float kBaselineBias = 10.0f;
// The system font has generous letter spacing; drawing it a little narrower than it is tall makes
// small text denser and lets more fit on a line. Every width below includes this factor.
constexpr float kCondense = 0.9f;

C3D_RenderTarget* g_top = nullptr;
C3D_RenderTarget* g_bottom = nullptr;
C2D_TextBuf g_text_buf = nullptr;
C2D_Font g_font = nullptr;  // nullptr = the console's own system font
bool g_c3d_ready = false;
bool g_c2d_ready = false;
bool g_in_frame = false;

float g_unit = 1.0f;               // font units -> pixels at scale 1.0 (30 / glyph cell height)
float g_line_feed = 30.0f;         // distance between lines at scale 1.0, in pixels
float g_ascii_advance[128] = {};   // pixel advance of each ASCII glyph at scale 1.0

float Advance(uint32_t cp) {
    if (cp < 128) {
        return g_ascii_advance[cp];
    }
    const int index = C2D_FontGlyphIndexFromCodePoint(g_font, cp);
    const charWidthInfo_s* info = C2D_FontGetCharWidthInfo(g_font, index);
    return info ? static_cast<float>(info->charWidth) * g_unit * kCondense : 0.0f;
}

// Decodes the UTF-8 character at `p` (NUL terminated). Invalid bytes decode as U+FFFD, one at a time.
int NextChar(const char* p, uint32_t* cp) {
    const ssize_t n = decode_utf8(cp, reinterpret_cast<const uint8_t*>(p));
    if (n <= 0) {
        *cp = 0xFFFD;
        return 1;
    }
    return static_cast<int>(n);
}

// Longest prefix of `s` (on a character boundary) that is at most `max_width` wide.
std::string FitPrefix(const std::string& s, float scale, float max_width) {
    float width = 0.0f;
    std::size_t i = 0;
    while (i < s.size()) {
        uint32_t cp;
        const int n = NextChar(s.c_str() + i, &cp);
        const float advance = Advance(cp) * scale;
        if (width + advance > max_width) {
            break;
        }
        width += advance;
        i += static_cast<std::size_t>(n);
    }
    return s.substr(0, i);
}

void WrapParagraph(const std::string& para, float scale, float max_width, std::vector<std::string>& out) {
    const float space = Advance(' ') * scale;
    std::string line;
    float line_width = 0.0f;
    std::size_t i = 0;
    for (;;) {
        std::size_t j = para.find(' ', i);
        const bool last = (j == std::string::npos);
        if (last) {
            j = para.size();
        }
        const std::string word = para.substr(i, j - i);
        if (!word.empty()) {
            const float w = TextWidth(word.c_str(), scale);
            if (!line.empty() && line_width + space + w <= max_width) {
                line += ' ';
                line += word;
                line_width += space + w;
            } else {
                if (!line.empty()) {
                    out.push_back(line);
                    line.clear();
                    line_width = 0.0f;
                }
                if (w <= max_width) {
                    line = word;
                    line_width = w;
                } else {
                    // A word wider than the whole line (a long address, say): break it anywhere.
                    std::size_t k = 0;
                    float chunk_width = 0.0f;
                    while (k < word.size()) {
                        uint32_t cp;
                        const int n = NextChar(word.c_str() + k, &cp);
                        const float advance = Advance(cp) * scale;
                        if (chunk_width + advance > max_width && !line.empty()) {
                            out.push_back(line);
                            line.clear();
                            chunk_width = 0.0f;
                        }
                        line.append(word, k, static_cast<std::size_t>(n));
                        chunk_width += advance;
                        k += static_cast<std::size_t>(n);
                    }
                    line_width = chunk_width;
                }
            }
        }
        if (last) {
            break;
        }
        i = j + 1;
    }
    if (!line.empty()) {
        out.push_back(line);
    }
}

struct WrapEntry {
    bool used = false;
    float scale = 0.0f;
    float width = 0.0f;
    std::string text;
    std::vector<std::string> lines;
};
WrapEntry g_wrap_cache[10];
int g_wrap_next = 0;

} // namespace

static const char* g_init_error = "";

const char* InitError() {
    return g_init_error;
}

bool Init() {
    // The system font must be mapped before citro2d's text code touches it (it panics otherwise).
    if (R_FAILED(fontEnsureMapped())) {
        g_init_error = "the system font could not be mapped";
        return false;
    }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        g_init_error = "citro3d init failed (command buffer: no linear memory)";
        return false;
    }
    g_c3d_ready = true;
    if (!C2D_Init(kMaxObjects)) {
        g_init_error = "citro2d init failed (vertex buffers: no linear memory)";
        Shutdown();
        return false;
    }
    g_c2d_ready = true;
    C2D_Prepare();

    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!g_top || !g_bottom) {
        g_init_error = "the screen render targets could not be created (VRAM)";
        Shutdown();
        return false;
    }

    // Returns nullptr both on failure and when the console's own font is the requested one; in
    // both cases the system font (nullptr) is what we want. cfgu must be initialised by the caller.
    g_font = C2D_FontLoadSystem(CFG_REGION_USA);
    g_text_buf = C2D_TextBufNew(kTextGlyphs);
    if (!g_text_buf) {
        g_init_error = "the text buffer could not be allocated";
        Shutdown();
        return false;
    }

    const FINF_s* info = C2D_FontGetInfo(g_font);
    if (!info || !info->tglp || info->tglp->cellHeight == 0) {
        g_init_error = "the system font has no usable glyph data";
        Shutdown();
        return false;
    }
    g_unit = 30.0f / static_cast<float>(info->tglp->cellHeight);
    g_line_feed = std::ceil(g_unit * static_cast<float>(info->lineFeed));
    for (uint32_t cp = 0; cp < 128; ++cp) {
        g_ascii_advance[cp] = 0.0f;
        if (cp >= 32 && cp < 127) {
            const int index = C2D_FontGlyphIndexFromCodePoint(g_font, cp);
            const charWidthInfo_s* width = C2D_FontGetCharWidthInfo(g_font, index);
            g_ascii_advance[cp] = width ? static_cast<float>(width->charWidth) * g_unit * kCondense : 0.0f;
        }
    }
    return true;
}

void ReleaseIcons() {}

void Shutdown() {
    g_in_frame = false;
    ReleaseIcons(); // (before citro3d goes: the textures are its)
    if (g_text_buf) {
        C2D_TextBufDelete(g_text_buf);
        g_text_buf = nullptr;
    }
    if (g_font) {
        C2D_FontFree(g_font);
        g_font = nullptr;
    }
    if (g_c2d_ready) {
        C2D_Fini();
        g_c2d_ready = false;
    }
    if (g_c3d_ready) {
        C3D_Fini(); // also destroys the render targets
        g_c3d_ready = false;
    }
    g_top = nullptr;
    g_bottom = nullptr;
}

bool BeginFrame(bool wait) {
    if (!g_c3d_ready || !g_top || !g_bottom) {
        return false;
    }
    if (!C3D_FrameBegin(wait ? C3D_FRAME_SYNCDRAW : C3D_FRAME_NONBLOCK)) {
        return false;
    }
    g_in_frame = true;
    C2D_TextBufClear(g_text_buf);
    return true;
}

void BeginTop() {
    C2D_TargetClear(g_top, col::kBg);
    C2D_SceneBegin(g_top);
}

void BeginBottom() {
    C2D_TargetClear(g_bottom, col::kBg);
    C2D_SceneBegin(g_bottom);
}

void EndFrame() {
    if (g_in_frame) {
        g_in_frame = false;
        C3D_FrameEnd(0);
    }
}

void ResumeAfterApplet() {
    if (g_c2d_ready) {
        C2D_Prepare();
    }
}

void FillRect(float x, float y, float w, float h, u32 color) {
    if (w <= 0.0f || h <= 0.0f || (color >> 24) == 0) {
        return;
    }
    C2D_DrawRectSolid(x, y, kDepth, w, h, color);
}

void FillGradient(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    C2D_DrawRectangle(x, y, kDepth, w, h, tl, tr, bl, br);
}

void StrokeRect(float x, float y, float w, float h, float t, u32 color) {
    FillRect(x, y, w, t, color);
    FillRect(x, y + h - t, w, t, color);
    FillRect(x, y + t, t, h - 2 * t, color);
    FillRect(x + w - t, y + t, t, h - 2 * t, color);
}

void FillCircle(float cx, float cy, float radius, u32 color) {
    if (radius <= 0.0f || (color >> 24) == 0) {
        return;
    }
    C2D_DrawCircleSolid(cx, cy, kDepth, radius, color);
}

void Line(float x0, float y0, float x1, float y1, float thickness, u32 color) {
    C2D_DrawLine(x0, y0, color, x1, y1, color, thickness, kDepth);
}

float TextWidth(const char* text, float scale) {
    if (!text) {
        return 0.0f;
    }
    float width = 0.0f;
    const char* p = text;
    while (*p) {
        uint32_t cp;
        p += NextChar(p, &cp);
        width += Advance(cp);
    }
    return width * scale;
}

float LineHeight(float scale) {
    return std::ceil(g_line_feed * scale);
}

void DrawText(float x, float yc, float scale, u32 color, const char* text, Align align) {
    if (!text || !*text || !g_text_buf || (color >> 24) == 0) {
        return;
    }
    if (align != Align::Left) {
        const float width = TextWidth(text, scale);
        x -= (align == Align::Right) ? width : width * 0.5f;
    }
    x = std::floor(x + 0.5f);
    const float baseline = std::floor(yc + kBaselineBias * scale + 0.5f);
    C2D_Text t;
    C2D_TextFontParse(&t, g_font, g_text_buf, text);
    C2D_DrawText(&t, C2D_AtBaseline | C2D_WithColor, x, baseline, kDepth, scale * kCondense, scale, color);
}

void DrawFitted(float x, float yc, float scale, float min_scale, float max_width, u32 color,
                const std::string& text, Align align) {
    const float width = TextWidth(text.c_str(), scale);
    if (width > max_width && width > 0.0f) {
        scale = scale * max_width / width;
        if (scale < min_scale) {
            scale = min_scale;
        }
    }
    DrawText(x, yc, scale, color, Ellipsize(text, scale, max_width), align);
}

std::string Ellipsize(const std::string& text, float scale, float max_width) {
    if (TextWidth(text.c_str(), scale) <= max_width) {
        return text;
    }
    const float dots = TextWidth("...", scale);
    return FitPrefix(text, scale, max_width - dots) + "...";
}

std::string MarqueeText(const std::string& text, float scale, float max_width, u64 elapsed_ms) {
    if (TextWidth(text.c_str(), scale) <= max_width) {
        return text;
    }
    // The start of every character, and from the end the first one whose tail still fits: that is how far the text has to run.
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size();) {
        uint32_t cp;
        starts.push_back(i);
        i += static_cast<std::size_t>(NextChar(text.c_str() + i, &cp));
    }
    std::size_t last = 0;
    float tail = 0.0f;
    for (std::size_t k = starts.size(); k-- > 0;) {
        uint32_t cp;
        NextChar(text.c_str() + starts[k], &cp);
        tail += Advance(cp) * scale;
        if (tail > max_width) {
            last = k + 1;
            break;
        }
    }
    constexpr u64 kRestMs = 1000;
    constexpr u64 kStepMs = 150;
    const u64 steps = last;
    const u64 cycle = kRestMs + steps * kStepMs + kRestMs;
    const u64 t = elapsed_ms % cycle;
    std::size_t at = 0;
    if (t >= kRestMs) {
        at = t < kRestMs + steps * kStepMs ? static_cast<std::size_t>((t - kRestMs) / kStepMs) : last;
    }
    if (at >= starts.size()) {
        at = starts.size() - 1;
    }
    return FitPrefix(text.substr(starts[at]), scale, max_width);
}

const std::vector<std::string>& WrapText(const std::string& text, float scale, float max_width) {
    for (WrapEntry& e : g_wrap_cache) {
        if (e.used && e.scale == scale && e.width == max_width && e.text == text) {
            return e.lines;
        }
    }
    WrapEntry& e = g_wrap_cache[g_wrap_next];
    g_wrap_next = (g_wrap_next + 1) % static_cast<int>(sizeof(g_wrap_cache) / sizeof(g_wrap_cache[0]));
    e.used = true;
    e.scale = scale;
    e.width = max_width;
    e.text = text;
    e.lines.clear();
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        const std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (para.empty()) {
            e.lines.push_back(std::string());
        } else {
            WrapParagraph(para, scale, max_width, e.lines);
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return e.lines;
}

float DrawWrapped(float x, float y_top, float max_width, float scale, u32 color,
                  const std::string& text, int max_lines) {
    const std::vector<std::string>& lines = WrapText(text, scale, max_width);
    const float lh = LineHeight(scale);
    const int total = static_cast<int>(lines.size());
    const int shown = total < max_lines ? total : max_lines;
    for (int i = 0; i < shown; ++i) {
        if (i == shown - 1 && total > shown) {
            const float dots = TextWidth("...", scale);
            DrawText(x, y_top + lh * (static_cast<float>(i) + 0.5f), scale, color,
                     FitPrefix(lines[i], scale, max_width - dots) + "...");
        } else {
            DrawText(x, y_top + lh * (static_cast<float>(i) + 0.5f), scale, color, lines[i]);
        }
    }
    return lh * static_cast<float>(shown);
}

std::string Clean(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            c = ' ';
        }
    }
    return out;
}

} // namespace gfx
