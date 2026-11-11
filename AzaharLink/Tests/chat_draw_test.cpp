// The chat's lines on the top screen (azl/chat_draw.hpp) and the drawing under them (azl/gfx_surface.hpp, azl/font_spleen.inc), run by
// Tests/run.sh with ASan/UBSan: fake framebuffers laid out as the framework hands them (rotated columns, guard bytes around them, and a
// column longer than the screen's 240 rows once), in the five formats of the GSP and with the 3D on. What it holds:
// - the pixel bytes of every format against an encoder of its own, and the fonts (95 glyphs each, the space empty, every other one drawn;
//   the 6x10 one the 6x12 one with its 13 glyphs that reach the top or the bottom row moved as Tests/make_font_spleen.py says);
// - rectangles and text of both fonts at rows that start a column off a word: exactly their pixels change, the right image too with the 3D;
// - the wrap of "Nick: text" (one line, two and three at a space, a word cut, a break at a space that would push the rest past the block cut
//   at the line's end instead, the longest message whole), the colour of a nick, when the lines and the notifications show;
// - the blocks in the 4 corners: where each one is (the numbers of STUDIO-CHAT section 6, written out here again), the bar, the paddings,
//   the glyphs of the nick in its colour and of the text in the text colour, nothing written outside the blocks (not a byte of the guards,
//   of the gap rows of a long column or of the right image with the 3D off), the bottom-right corner above the notifications, the blocks
//   that no longer fit dropped, the oldest first, and a stack that stops where the one in the opposite corner begins (the plugin's
//   notifications: a block with no nick, its bar in kNoticeBar);
// - the cost: every store is counted (AZL_GFX_COUNT_STORE): the strips of the chat go out as 32-bit words only, a frame of 5 blocks is held
//   to a budget, and the drawing allocates nothing (malloc is wrapped by the link).
// With a folder as its argument it writes the pictures there as BMP files (the 4 corners, every format, the right eye, the notifications,
// long and cut messages, the two fonts).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static unsigned long g_stores[5]; // [1], [2], [4]: stores of that many bytes
#define AZL_GFX_COUNT_STORE(bytes) (++g_stores[(bytes)])

#include "azl/chat_draw.hpp"
#include "azl/ini_values.hpp"

using namespace azl;
using azl::gfx::Rgb;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

// ---- every allocation of the process is counted (the link wraps malloc; operator new is replaced to go through it) ----
static std::atomic<unsigned> g_allocs{0};
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(n);
}
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t n) {
    return operator new(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

// The numbers of chat_pos are the corners' (the plugin hands the ini's value straight to DrawChatLines).
static_assert(int(chat::kTopLeft) == int(ini::kChatPosTopLeft) && int(chat::kTopRight) == int(ini::kChatPosTopRight), "corners");
static_assert(int(chat::kBottomLeft) == int(ini::kChatPosBottomLeft) && int(chat::kBottomRight) == int(ini::kChatPosBottomRight), "corners");

static const char* const kFormatNames[5] = {"rgba8", "bgr8", "rgb565", "rgb5a1", "rgba4"};
static const char* const kCornerNames[4] = {"top-left", "top-right", "bottom-left", "bottom-right"};

// ---------------------------------------------------------------------------------------------
// The test's own encoder and decoder (not gfx::Raw: the layer is checked against these)
// ---------------------------------------------------------------------------------------------

static unsigned Bpp(unsigned format) {
    return format == 0 ? 4 : format == 1 ? 3 : 2;
}

static void Encode(Rgb c, unsigned format, u8* out) {
    unsigned v = 0;
    switch (format) {
    case 0: // RGBA8: A B G R in memory
        out[0] = 0xFF, out[1] = c.b, out[2] = c.g, out[3] = c.r;
        return;
    case 1: // BGR8
        out[0] = c.b, out[1] = c.g, out[2] = c.r;
        return;
    case 2:
        v = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3);
        break;
    case 3:
        v = (c.r >> 3) << 11 | (c.g >> 3) << 6 | (c.b >> 3) << 1 | 1;
        break;
    default:
        v = (c.r >> 4) << 12 | (c.g >> 4) << 8 | (c.b >> 4) << 4 | 0xF;
        break;
    }
    out[0] = static_cast<u8>(v), out[1] = static_cast<u8>(v >> 8);
}

static Rgb Decode(const u8* p, unsigned format) {
    const unsigned v = p[0] | p[1] << 8;
    switch (format) {
    case 0:
        return {p[3], p[2], p[1]};
    case 1:
        return {p[2], p[1], p[0]};
    case 2:
        return {static_cast<u8>(((v >> 11) & 31) << 3), static_cast<u8>(((v >> 5) & 63) << 2), static_cast<u8>((v & 31) << 3)};
    case 3:
        return {static_cast<u8>(((v >> 11) & 31) << 3), static_cast<u8>(((v >> 6) & 31) << 3), static_cast<u8>(((v >> 1) & 31) << 3)};
    default:
        return {static_cast<u8>(((v >> 12) & 15) << 4), static_cast<u8>(((v >> 8) & 15) << 4), static_cast<u8>(((v >> 4) & 15) << 4)};
    }
}

// ---------------------------------------------------------------------------------------------
// A fake framebuffer: the top screen as the framework hands it, both eyes, guard bytes around each
// ---------------------------------------------------------------------------------------------

constexpr unsigned kGuard = 64;

struct Fb {
    unsigned format, bpp, rowsize, w, h;
    bool stereo;
    std::vector<u8> eye[2];  // guard + w columns of rowsize pixels + guard
    std::vector<u8> before[2];

    Fb(unsigned fmt, bool three_d, unsigned rows = 240, unsigned width = 400)
        : format(fmt), bpp(Bpp(fmt)), rowsize(rows), w(width), h(240), stereo(three_d) {
        for (auto& e : eye) {
            e.assign(kGuard * 2 + static_cast<std::size_t>(w) * rowsize * bpp, 0);
        }
    }
    u8* Base(int e) {
        return eye[e].data() + kGuard;
    }
    gfx::Surface Surface(bool usable = true) {
        gfx::Surface s;
        s.left = Base(0);
        s.right = stereo ? Base(1) : nullptr;
        s.stride = rowsize * bpp;
        s.bpp = static_cast<u8>(usable ? bpp : bpp + 1);
        s.format = static_cast<u8>(format);
        s.w = static_cast<u16>(w);
        s.h = static_cast<u16>(h);
        return s;
    }
    std::size_t Offset(unsigned x, unsigned y) const {
        return kGuard + (static_cast<std::size_t>(x) * rowsize + (h - 1 - y)) * bpp;
    }
    Rgb At(int e, unsigned x, unsigned y) const {
        return Decode(&eye[e][Offset(x, y)], format);
    }
    bool Is(int e, unsigned x, unsigned y, Rgb c) const {
        u8 want[4];
        Encode(c, format, want);
        return std::memcmp(&eye[e][Offset(x, y)], want, bpp) == 0;
    }
    // A "game": a sky, a ground and some stripes, in every byte of the visible pixels; the guards and the gap rows get a pattern of their own.
    void Paint() {
        for (int e = 0; e < 2; ++e) {
            for (std::size_t i = 0; i < eye[e].size(); ++i) {
                eye[e][i] = static_cast<u8>(0xA5 ^ (i * 7));
            }
            for (unsigned x = 0; x < w; ++x) {
                for (unsigned y = 0; y < h; ++y) {
                    Rgb c;
                    if (y < 150) {
                        c = {static_cast<u8>(40 + y / 3), static_cast<u8>(90 + y / 2), static_cast<u8>(200 - y / 4)};
                    } else {
                        c = {static_cast<u8>(60 + (x % 40)), static_cast<u8>(140 + (y - 150) / 2), 50};
                    }
                    if (((x + y) / 18) % 9 == 0 && y < 150) {
                        c = {250, 250, 240};
                    }
                    if (e == 1) {
                        c.r = static_cast<u8>(c.r ^ 0x20); // (the right image differs, so a copy of the left one would be seen)
                    }
                    Encode(c, format, &eye[e][Offset(x, y)]);
                }
            }
            before[e] = eye[e];
        }
    }
    bool Changed(int e, unsigned x, unsigned y) const {
        return std::memcmp(&eye[e][Offset(x, y)], &before[e][Offset(x, y)], bpp) != 0;
    }
};

struct Rect {
    int x, y, w, h;
    bool Has(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Every byte that changed belongs to a visible pixel inside one of `rects` (on the left image, and on the right one only with the 3D):
// not a byte of the guards, of the gap rows of a long column, or of the right image with the 3D off.
static bool OnlyInside(const Fb& fb, const std::vector<Rect>& rects) {
    bool ok = true;
    for (int e = 0; e < 2; ++e) {
        for (std::size_t i = 0; i < fb.eye[e].size(); ++i) {
            if (fb.eye[e][i] == fb.before[e][i]) {
                continue;
            }
            bool inside = false;
            if (i >= kGuard && i < fb.eye[e].size() - kGuard && (e == 0 || fb.stereo)) {
                const std::size_t px = (i - kGuard) / fb.bpp;
                const unsigned x = static_cast<unsigned>(px / fb.rowsize), r = static_cast<unsigned>(px % fb.rowsize);
                if (r < fb.h) {
                    const int y = static_cast<int>(fb.h - 1 - r);
                    for (const Rect& rc : rects) {
                        inside = inside || rc.Has(static_cast<int>(x), y);
                    }
                }
            }
            if (!inside) {
                std::fprintf(stderr, "  a byte written outside: eye %d, offset %zu\n", e, i);
                ok = false;
                break;
            }
        }
    }
    return ok;
}

static void ResetStores() {
    std::memset(g_stores, 0, sizeof(g_stores));
}
static unsigned long StoredBytes() {
    return g_stores[1] + g_stores[2] * 2 + g_stores[4] * 4;
}

// ---- BMP ----
static std::string g_out; // the folder of the pictures ("" = none)

static void WriteBmp(const Fb& fb, int e, const std::string& name) {
    if (g_out.empty()) {
        return;
    }
    const std::string path = g_out + "/" + name + ".bmp";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "  cannot write %s\n", path.c_str());
        ++g_failures;
        return;
    }
    const unsigned row = fb.w * 3, size = 54 + row * fb.h;
    u8 head[54] = {'B', 'M'};
    const auto put32 = [&](int at, unsigned v) {
        for (int i = 0; i < 4; ++i) {
            head[at + i] = static_cast<u8>(v >> (8 * i));
        }
    };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, fb.w);
    put32(22, fb.h);
    head[26] = 1;
    head[28] = 24;
    put32(34, row * fb.h);
    std::fwrite(head, 1, sizeof(head), f);
    std::vector<u8> line(row);
    for (int y = static_cast<int>(fb.h) - 1; y >= 0; --y) {
        for (unsigned x = 0; x < fb.w; ++x) {
            const Rgb c = fb.At(e, x, static_cast<unsigned>(y));
            line[x * 3] = c.b, line[x * 3 + 1] = c.g, line[x * 3 + 2] = c.r;
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------------------------
// The messages
// ---------------------------------------------------------------------------------------------

static chat::Line MakeLine(const char* nick, const char* text, u32 t_ms, bool own = false) {
    chat::Line l;
    l.nick_len = static_cast<u8>(std::strlen(nick));
    l.text_len = static_cast<u8>(std::strlen(text));
    std::memcpy(l.nick, nick, l.nick_len);
    std::memcpy(l.text, text, l.text_len);
    l.t_ms = t_ms;
    l.own = own ? 1 : 0;
    return l;
}

static const char* const kLong = "the bridge on the left is broken, go around by the lake and meet me at the gate";
// 128 characters, what the chat keeps at most: two lines of a block do not hold it with the nick, so it ends with "..."
static const char* const kMax =
    "I am writing the longest message the chat keeps, one hundred and twenty eight characters in all, to see where it is cut, ok bye!";

static chat::View FiveMessages() {
    chat::View v;
    v.lines[0] = MakeLine("Gionko", "hi all, ready for the next round?", 1000, true);
    v.lines[1] = MakeLine("Marta_92", "yes! picking my character now", 2000);
    v.lines[2] = MakeLine("Toad.Racer", kLong, 3000);
    v.lines[3] = MakeLine("LinkFan", kMax, 4000);
    v.lines[4] = MakeLine("Gionko", "ok, the match starts in 10 seconds", 5000, true);
    v.count = 5;
    v.seq = 2;
    return v;
}

static std::string Shown(const chat::Line& l, const chat::Wrap& w, u32 line) {
    std::string s;
    for (u32 c = 0; c < w.len[line]; ++c) {
        s += w.dots && line + 1 == w.lines && c + 3 >= w.len[line] ? '.' : chat::BlockChar(l, w.start[line] + c);
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

static void TestFormatsAndFonts() {
    const Rgb colours[] = {{0, 0, 0}, {255, 255, 255}, {13, 14, 17}, {70, 178, 255}, {255, 140, 56}, {1, 2, 3}};
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        CHECK(gfx::BytesPerPixel(f) == Bpp(f));
        for (const Rgb& c : colours) {
            u8 want[4] = {}, got[4] = {};
            Encode(c, f, want);
            const u32 raw = gfx::Raw(c, f);
            for (unsigned i = 0; i < Bpp(f); ++i) {
                got[i] = static_cast<u8>(raw >> (8 * i));
            }
            CHECK(std::memcmp(want, got, Bpp(f)) == 0);
            if (Bpp(f) < 4) {
                CHECK((raw >> (8 * Bpp(f))) == 0); // (no bits past the pixel: they would land on the next one)
            }
        }
    }
    for (unsigned f = gfx::kFormats; f < 8; ++f) {
        CHECK(gfx::BytesPerPixel(f) == 0);
    }
    // the fonts: 95 glyphs, the space empty, every other one drawn, nothing past the cell's rows, no two the same
    CHECK(gfx::kFontFirst == 0x20 && gfx::kFontGlyphs == 95);
    for (const gfx::Font* font : {&gfx::kFont6x12, &gfx::kFont8x16, &gfx::kFont6x10}) {
        std::vector<std::vector<u16>> seen;
        for (unsigned g = 0; g < 95; ++g) {
            std::vector<u16> cols(font->cols + g * font->w, font->cols + (g + 1) * font->w);
            unsigned any = 0;
            for (u16 c : cols) {
                any |= c;
                CHECK((c >> font->h) == 0);
            }
            CHECK(g == 0 ? any == 0 : any != 0);
            for (const auto& other : seen) {
                CHECK(other != cols);
            }
            seen.push_back(cols);
        }
    }
    // '|' and '_' tell up from down and left from right: the bar is one column of several rows; the underscore is low in every column
    // that draws it (bit 0 is the bottom row)
    unsigned bar_cols = 0;
    for (unsigned c = 0; c < 6; ++c) {
        const u16 v = gfx::kSpleen6x12['|' - 0x20][c];
        bar_cols += v != 0;
        if (v) {
            CHECK(__builtin_popcount(v) >= 8);
        }
        const u16 under = gfx::kSpleen6x12['_' - 0x20][c];
        CHECK(under == 0 || under < (1u << 4));
    }
    CHECK(bar_cols == 1);
    // the 6x10 font (the chat's lines) is the 6x12 one in a cell two rows shorter: rows 1-10 of the 82 glyphs that leave the top and the
    // bottom row empty; rows 2-11 of the 10 that reach the top row (drawn a row lower); p, q and y with their last row OR-ed into the one
    // above it. Nothing else of a glyph is lost.
    std::string top, bottom;
    for (unsigned g = 0; g < 95; ++g) {
        u16 any = 0;
        for (unsigned c = 0; c < 6; ++c) {
            any |= gfx::kSpleen6x12[g][c];
        }
        const bool tall = (any >> 11) & 1, low = any & 1;
        top += tall ? static_cast<char>(0x20 + g) : '\0';
        bottom += low ? static_cast<char>(0x20 + g) : '\0';
        for (unsigned c = 0; c < 6; ++c) {
            const u16 v = gfx::kSpleen6x12[g][c];
            const u16 want = tall ? static_cast<u16>(v >> 2) : static_cast<u16>(((v | (v & 1) << 1) >> 1) & 0x3FF);
            CHECK(gfx::kSpleen6x10[g][c] == want);
            if (tall) {
                CHECK((v & 3) == 0); // (the two bottom rows of a tall glyph are empty: nothing lost)
            }
        }
    }
    const auto chars = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c) {
                out += c;
            }
        }
        return out;
    };
    CHECK(chars(top) == std::string("$()/[") + '\\' + "]{|}");
    CHECK(chars(bottom) == "pqy");
    CHECK(gfx::kFont6x10.w == 6 && gfx::kFont6x10.h == 10);
    // a glyph that is not printable ASCII is drawn as '?'
    for (unsigned c = 0; c < 6; ++c) {
        CHECK(gfx::GlyphColumn(gfx::kFont6x12, '\x7F', c) == gfx::kSpleen6x12['?' - 0x20][c]);
        CHECK(gfx::GlyphColumn(gfx::kFont6x12, static_cast<char>(0xC3), c) == gfx::kSpleen6x12['?' - 0x20][c]);
    }
}

static void TestRectsAndText() {
    const Rgb fill = {200, 30, 90}, fg = {232, 234, 238}, bg = {13, 14, 17};
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        for (int stereo = 0; stereo < 2; ++stereo) {
            Fb fb(f, stereo != 0);
            fb.Paint();
            const gfx::Surface s = fb.Surface();
            CHECK(gfx::Usable(s));
            // rows that start a column off a word in every format (y + h - 1 = 238: the column starts at the pixel 1), and a height over 32
            ResetStores();
            gfx::FillRect(s, 13, 170, 7, 69, fill);
            CHECK(StoredBytes() == 7u * 69u * fb.bpp * (stereo ? 2u : 1u));
            // cut to the screen: nothing of the part outside
            gfx::FillRect(s, 395, -5, 20, 10, fill);
            CHECK(OnlyInside(fb, {{13, 170, 7, 69}, {395, 0, 5, 5}}));
            for (int e = 0; e <= stereo; ++e) {
                bool all = true;
                for (int x = 13; x < 20; ++x) {
                    for (int y = 170; y < 239; ++y) {
                        all = all && fb.Is(e, x, y, fill);
                    }
                }
                CHECK(all);
                CHECK(fb.Is(e, 399, 0, fill) && fb.Is(e, 395, 4, fill));
            }
            // text in both fonts, at rows off a word: each pixel of a cell is the glyph's bit
            fb.Paint();
            const char* text = "Ab?~";
            gfx::DrawText(s, 30, 41, text, 4, gfx::kFont6x12, fg, bg);
            gfx::DrawText(s, 30, 77, text, 4, gfx::kFont8x16, fg, bg);
            gfx::DrawText(s, 392, 100, text, 4, gfx::kFont8x16, fg, bg); // (cut at the right edge)
            gfx::DrawText(s, 30, 230, text, 4, gfx::kFont6x12, fg, bg);  // (past the bottom: not drawn)
            CHECK(OnlyInside(fb, {{30, 41, 24, 12}, {30, 77, 32, 16}, {392, 100, 8, 16}}));
            for (int e = 0; e <= stereo; ++e) {
                bool exact = true;
                for (int i = 0; i < 4; ++i) {
                    for (int c = 0; c < 6; ++c) {
                        for (int r = 0; r < 12; ++r) {
                            const bool on = (gfx::kSpleen6x12[text[i] - 0x20][c] >> (11 - r)) & 1;
                            exact = exact && fb.Is(e, 30 + i * 6 + c, 41 + r, on ? fg : bg);
                        }
                    }
                    for (int c = 0; c < 8; ++c) {
                        for (int r = 0; r < 16; ++r) {
                            const bool on = (gfx::kSpleen8x16[text[i] - 0x20][c] >> (15 - r)) & 1;
                            exact = exact && fb.Is(e, 30 + i * 8 + c, 77 + r, on ? fg : bg);
                        }
                    }
                }
                CHECK(exact);
            }
        }
    }
    // a surface the layer cannot draw on is left alone: an unknown format, a pixel size that is not the format's, no image, a short column
    Fb fb(1, false);
    fb.Paint();
    gfx::Surface s = fb.Surface(false);
    CHECK(!gfx::Usable(s));
    gfx::FillRect(s, 0, 0, 50, 50, fill);
    s = fb.Surface();
    s.format = 5;
    s.bpp = 2;
    CHECK(!gfx::Usable(s));
    gfx::FillRect(s, 0, 0, 50, 50, fill);
    s = fb.Surface();
    s.left = nullptr;
    gfx::FillRect(s, 0, 0, 50, 50, fill);
    s = fb.Surface();
    s.stride = 239 * 3;
    CHECK(!gfx::Usable(s));
    gfx::FillRect(s, 0, 0, 50, 50, fill);
    CHECK(OnlyInside(fb, {}));
}

static void TestWrapAndColours() {
    CHECK(std::strlen(kMax) == chat::kTextMax);
    // one line: "Nick: text" up to kCols characters
    chat::Line l = MakeLine("Gionko", "hi", 0);
    chat::Wrap w = chat::WrapLine(l);
    CHECK(w.lines == 1 && w.len[0] == 10 && !w.dots && Shown(l, w, 0) == "Gionko: hi");
    const std::string fits(chat::kCols - 8, 'x');
    l = MakeLine("Gionko", fits.c_str(), 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 1 && w.len[0] == chat::kCols);
    // two lines, broken at the last space of the first line's second half
    l = MakeLine("Toad.Racer", kLong, 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 2 && !w.dots);
    CHECK(Shown(l, w, 0) == "Toad.Racer: the bridge on the left is broken, go around by the");
    CHECK(Shown(l, w, 1) == "lake and meet me at the gate");
    CHECK(w.len[0] <= chat::kCols && w.len[1] <= chat::kCols);
    // a word with no space in it is cut at kCols, and the second line takes the rest
    const std::string word(70, 'w');
    l = MakeLine("Bob", word.c_str(), 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 2 && w.len[0] == chat::kCols && w.start[1] == chat::kCols && w.len[1] == 5 + 70 - chat::kCols && !w.dots);
    // a word that ends just where the line does: the break is the space after it, and the second line starts with the next word
    std::string spaced = std::string(chat::kCols - 5, 'a') + " " + std::string(10, 'b');
    spaced[10] = ' '; // (a space in the first half too: not the one taken)
    l = MakeLine("Bob", spaced.c_str(), 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 2 && w.len[0] == chat::kCols && w.start[1] == chat::kCols + 1 && chat::BlockChar(l, w.start[1]) == 'b');
    // the 128 characters the chat keeps: three lines, whole (the user's request, 2026-11-08: no "..." for a message the chat keeps)
    l = MakeLine("LinkFan", kMax, 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 3 && !w.dots);
    CHECK(Shown(l, w, 0) == "LinkFan: I am writing the longest message the chat keeps, one");
    CHECK(Shown(l, w, 1) == "hundred and twenty eight characters in all, to see where it is");
    CHECK(Shown(l, w, 2) == "cut, ok bye!");
    // a space that would leave the rest too long for the lines after it is not a break: the line is cut at its end (here the second, whose
    // last space is 40 cells in: broken there, the third line would need 77 cells and end with "...")
    const std::string nick(chat::kNickMax, 'N');
    const std::string spaced_far = std::string(9, 'a') + " " + std::string(40, 'b') + " " + std::string(77, 'c');
    l = MakeLine(nick.c_str(), spaced_far.c_str(), 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 3 && !w.dots);
    CHECK(w.start[0] == 0 && w.len[0] == 31 && w.start[1] == 32 && w.len[1] == chat::kCols && w.start[2] == 94 && w.len[2] == 56);
    // the most a message can be: 20 + 2 + 128 characters, whole, never past the line or its fields; with or without spaces, wherever they are
    const std::string text(chat::kTextMax, 't');
    l = MakeLine(nick.c_str(), text.c_str(), 0);
    w = chat::WrapLine(l);
    CHECK(w.lines == 3 && !w.dots && w.len[0] == chat::kCols && w.len[1] == chat::kCols && w.len[2] == 150 - 2 * chat::kCols);
    CHECK(w.start[2] + w.len[2] == chat::BlockChars(l));
    unsigned whole = 0;
    for (unsigned seed = 1; seed <= 2000; ++seed) {
        std::string t;
        unsigned r = seed * 2654435761u;
        for (u32 i = 0; i < chat::kTextMax; ++i) {
            r = r * 1103515245u + 12345u;
            const bool space = (r >> 16) % (2 + seed % 13) == 0 && i > 0 && i + 1 < chat::kTextMax && t.back() != ' ';
            t += space ? ' ' : static_cast<char>('a' + (r >> 20) % 26);
        }
        l = MakeLine(nick.c_str(), t.c_str(), 0);
        w = chat::WrapLine(l);
        u32 cells = 0;
        bool ok = !w.dots && w.lines <= chat::kBlockLines;
        for (u32 k = 0; k < w.lines; ++k) {
            ok = ok && w.len[k] <= chat::kCols && w.len[k] > 0 && chat::BlockChar(l, w.start[k]) != ' ';
            cells += w.len[k] + (k + 1 < w.lines ? w.start[k + 1] - w.start[k] - w.len[k] : 0);
            if (k + 1 < w.lines) {
                ok = ok && (w.start[k + 1] == w.start[k] + w.len[k] || chat::BlockChar(l, w.start[k] + w.len[k]) == ' ');
            }
        }
        ok = ok && cells == chat::BlockChars(l);
        whole += ok ? 1 : 0;
    }
    CHECK(whole == 2000);
    // a notification of the plugin (a line with no nick) is its text alone
    l = MakeLine("", "Joined room \"Test\"", 0);
    w = chat::WrapLine(l);
    CHECK(chat::BlockChars(l) == 18 && w.lines == 1 && w.len[0] == 18 && Shown(l, w, 0) == "Joined room \"Test\"");

    // the colour of a nick: stable, one of five, and the five all used by some nicks
    CHECK(chat::NickColour("Gionko", 6) == chat::NickColour("Gionko", 6));
    unsigned used = 0;
    for (int i = 0; i < 200; ++i) {
        char nick[12];
        const int n = std::snprintf(nick, sizeof(nick), "Player%d", i);
        const u32 c = chat::NickColour(nick, static_cast<u32>(n));
        CHECK(c < chat::kNickColours);
        used |= 1u << c;
    }
    CHECK(used == 31);

    // when the lines show: 10 s after the last one came (the clock may wrap), always with the keyboard open, never with none kept
    chat::View v;
    CHECK(chat::ShownLines(v, 0, true) == 0);
    v.lines[0] = MakeLine("A", "one", 0xFFFFF000u);
    v.lines[1] = MakeLine("B", "two", 0xFFFFFF00u);
    v.count = 2;
    CHECK(chat::ShownLines(v, 0xFFFFFF00u + chat::kShowMs - 1, false) == 2);
    CHECK(chat::ShownLines(v, 0xFFFFFF00u + chat::kShowMs, false) == 0);
    CHECK(chat::ShownLines(v, 0xFFFFFF00u + chat::kShowMs, true) == 2);
    // the user's "about 10 s after the last message", in numbers: the checks above move with the constant
    CHECK(chat::ShownLines(v, 0xFFFFFF00u + 9999u, false) == 2);
    CHECK(chat::ShownLines(v, 0xFFFFFF00u + 10000u, false) == 0);
    // a line stamped by the network thread a little after the game's thread read its clock is new, not 2^32 - 1 ms old; much later is not
    CHECK(chat::ShownLines(v, 0xFFFFFF00u - 1, false) == 2);
    CHECK(chat::ShownLines(v, 0xFFFFFF00u - chat::kAheadMs, false) == 2);
    CHECK(chat::ShownLines(v, 0xFFFFFF00u - chat::kAheadMs - 1, false) == 0);
    // a view that says more than it holds: what it holds, never a line past its array
    {
        chat::View odd = v;
        odd.count = 9;
        CHECK(chat::ShownLines(odd, 0, true) == chat::kLines);
        CHECK(chat::ShownLines(odd, odd.lines[chat::kLines - 1].t_ms, false) == chat::kLines);
    }

    // the plugin's own notifications: the newest ones younger than 5 s (in the order of their times), a clock that wraps
    {
        chat::View n;
        CHECK(chat::ShownNotices(n, 0) == 0);
        n.lines[0] = MakeLine("", "one", 0xFFFFF000u);
        n.lines[1] = MakeLine("", "two", 0xFFFFFF00u);
        n.lines[2] = MakeLine("", "three", 0x00000100u);
        n.count = 3;
        CHECK(chat::ShownNotices(n, 0x00000100u) == 3);
        CHECK(chat::ShownNotices(n, 0xFFFFF000u + 4999u) == 3);
        CHECK(chat::ShownNotices(n, 0xFFFFF000u + 5000u) == 2);
        CHECK(chat::ShownNotices(n, 0xFFFFFF00u + 5000u) == 1);
        CHECK(chat::ShownNotices(n, 0x00000100u + 5000u) == 0);
        n.count = 9; // (a view that says more than it holds: what it holds)
        CHECK(chat::ShownNotices(n, 0) <= chat::kLines);
    }
    // the framework's notifications on screen: the last 5.3 s of them, at most kNoticeSlots
    chat::NoticeClock clock;
    CHECK(clock.Young(1000) == 0);
    clock.Note(1000);
    clock.Note(3000);
    CHECK(clock.Young(3000) == 2);
    CHECK(clock.Young(1000 + chat::kNoticeMs) == 1);
    CHECK(clock.Young(3001 + chat::kNoticeMs) == 0);
    // a notification noted by the plugin's thread just after the game's thread read its clock is on screen already
    CHECK(clock.Young(2990) == 2);
    CHECK(clock.Young(1000 - chat::kAheadMs - 2) == 0);
    for (int i = 0; i < 20; ++i) {
        clock.Note(9000);
    }
    CHECK(clock.Young(9000) == chat::kNoticeSlots);
}

// Where the blocks of `v` go in `corner`: the numbers of STUDIO-CHAT section 6, written out again here (and of 2026-11-08: blocks of 12, 24
// and 32 rows, lines of 62 cells). `limit`: the row a stack may not pass (the other stack's edge), -1 none.
static std::vector<Rect> Expected(const chat::View& v, u32 shown, int corner, u32 notices, int limit = -1) {
    std::vector<Rect> out;
    const bool top = corner == chat::kTopLeft || corner == chat::kTopRight;
    const bool right = corner == chat::kTopRight || corner == chat::kBottomRight;
    int y = top ? 8 : corner == chat::kBottomRight && notices > 0 ? ((227 - 15 * static_cast<int>(notices)) / 4) * 4 : 236;
    u32 first_shown = v.count - shown;
    if (top) {
        // a top stack that does not fit drops its oldest lines: the newest ones that fit, counted back from the newest
        int room = (limit < 0 ? 240 : limit) - y;
        u32 fit = 0;
        while (fit < shown) {
            const chat::Wrap fw = chat::WrapLine(v.lines[v.count - 1 - fit]);
            const int need = (fw.lines == 1 ? 12 : fw.lines == 2 ? 24 : 32) + (fit != 0 ? chat::kGapY : 0);
            if (need > room) {
                break;
            }
            room -= need;
            ++fit;
        }
        first_shown = v.count - fit;
        shown = fit;
    }
    for (u32 k = 0; k < shown; ++k) {
        const chat::Line& l = v.lines[top ? first_shown + k : v.count - 1 - k];
        const chat::Wrap w = chat::WrapLine(l);
        const int h = w.lines == 1 ? 12 : w.lines == 2 ? 24 : 32;
        u32 cols = 0;
        for (u32 i = 0; i < w.lines; ++i) {
            cols = w.len[i] > cols ? w.len[i] : cols;
        }
        const int width = 2 + 4 + 6 * static_cast<int>(cols) + 4;
        Rect r;
        r.w = width;
        r.h = h;
        r.x = right ? 390 - width : 6;
        if (top) {
            r.y = y;
            if (r.y + h > (limit < 0 ? 240 : limit)) {
                break;
            }
            y += h + chat::kGapY;
        } else {
            r.y = y - h;
            if (r.y < (limit < 0 ? 0 : limit)) {
                break;
            }
            y = r.y - chat::kGapY;
        }
        out.push_back(r);
    }
    return out;
}

// The inside of a block: the bar, the paddings, every pixel one of the three colours, the glyphs of the nick in the nick's colour and of the
// rest in the text colour (each pixel of each cell against the font's bit).
static bool BlockIsRight(const Fb& fb, int e, const Rect& r, const chat::Line& l) {
    const Rgb nick = l.nick_len == 0 ? chat::kNoticeBar : chat::kNick[chat::NickColour(l.nick, l.nick_len)];
    const chat::Wrap w = chat::WrapLine(l);
    const int pad = w.lines == 2 ? 2 : 1; // (12, 24, 32 rows: 1 + 10 + 1, 2 + 20 + 2, 1 + 30 + 1)
    bool ok = true;
    for (int x = r.x; x < r.x + r.w; ++x) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            const int cx = x - r.x, cy = y - r.y;
            Rgb want = chat::kBg;
            if (cx < 2) {
                want = nick;
            } else if (cx >= 6 && cx < r.w - 4 && cy >= pad && cy < r.h - pad) {
                const u32 line = static_cast<u32>((cy - pad) / 10), row = static_cast<u32>((cy - pad) % 10);
                const u32 c = static_cast<u32>((cx - 6) / 6), g = static_cast<u32>((cx - 6) % 6);
                if (line < w.lines && c < w.len[line]) {
                    const u32 at = w.start[line] + c;
                    const char ch = w.dots && line + 1 == w.lines && c + 3 >= w.len[line] ? '.' : chat::BlockChar(l, at);
                    if ((gfx::kSpleen6x10[ch - 0x20][g] >> (9 - row)) & 1) {
                        want = at < l.nick_len ? nick : chat::kText;
                    }
                }
            }
            if (!fb.Is(e, static_cast<unsigned>(x), static_cast<unsigned>(y), want)) {
                ok = false;
            }
        }
    }
    return ok;
}

// Text without a background, with a shadow (the user's decision, 2026-11-11: no black strip behind the chat's lines - the game shows through everywhere - and a black shadow one pixel to
// the right and one down of every pixel of every letter): the bar is written in full, a glyph pixel is its colour, a shadow pixel is the background's black, and every other pixel of the block is
// the game's own, as it was (nothing is read: the pixel is simply not written).
static bool BlockIsRightShadow(const Fb& fb, int e, const Rect& r, const chat::Line& l) {
    const Rgb nick = l.nick_len == 0 ? chat::kNoticeBar : l.pad >= 1 && l.pad <= chat::kNodeColours ? chat::kNodeColour[l.pad - 1] : chat::kNick[chat::NickColour(l.nick, l.nick_len)];
    const chat::Wrap w = chat::WrapLine(l);
    const int pad = w.lines == 2 ? 2 : 1;
    // 0 = not a glyph pixel, 1 = a pixel in the nick's colour, 2 = in the text colour
    const auto glyph_at = [&](int cx, int cy) -> int {
        if (cx < 6 || cx >= r.w - 4 || cy < pad || cy >= r.h - pad) {
            return 0;
        }
        const u32 line = static_cast<u32>((cy - pad) / 10), row = static_cast<u32>((cy - pad) % 10);
        const u32 c = static_cast<u32>((cx - 6) / 6), g = static_cast<u32>((cx - 6) % 6);
        if (line >= w.lines || c >= w.len[line]) {
            return 0;
        }
        const u32 at = w.start[line] + c;
        const char ch = w.dots && line + 1 == w.lines && c + 3 >= w.len[line] ? '.' : chat::BlockChar(l, at);
        if (((gfx::kSpleen6x10[ch - 0x20][g] >> (9 - row)) & 1) == 0) {
            return 0;
        }
        return at < l.nick_len ? 1 : 2;
    };
    bool ok = true;
    for (int x = r.x; x < r.x + r.w; ++x) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            const int cx = x - r.x, cy = y - r.y;
            const unsigned ux = static_cast<unsigned>(x), uy = static_cast<unsigned>(y);
            const int own = glyph_at(cx, cy);
            if (cx < 2) {
                ok = ok && fb.Is(e, ux, uy, nick); // (the bar, written in full)
            } else if (own != 0) {
                ok = ok && fb.Is(e, ux, uy, own == 1 ? nick : chat::kText);
            } else if ([&] { // the shadow: a pixel within kShadowPx to the right and down of a pixel of a letter
                           for (int dx = 0; dx <= static_cast<int>(chat::kShadowPx); ++dx) {
                               for (int dy = 0; dy <= static_cast<int>(chat::kShadowPx); ++dy) {
                                   if ((dx != 0 || dy != 0) && glyph_at(cx - dx, cy - dy) != 0) {
                                       return true;
                                   }
                               }
                           }
                           return false;
                       }()) {
                ok = ok && fb.Is(e, ux, uy, chat::kBg);
            } else {
                ok = ok && !fb.Changed(e, ux, uy); // (the game's pixel, as it was)
            }
        }
    }
    return ok;
}

static void TestTranslucent() {
    const chat::View v = FiveMessages();
    unsigned long opaque_bytes = 0, shadow_bytes = 0;
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        for (int stereo = 0; stereo < 2; ++stereo) {
            for (int corner = 0; corner < 4; ++corner) {
                Fb fb(f, stereo != 0);
                fb.Paint();
                ResetStores();
                const u32 drawn = chat::DrawChatLines(fb.Surface(), v, 5, corner, 0, chat::kNoLimit, nullptr, true);
                const unsigned long bytes = StoredBytes();
                const std::vector<Rect> want = Expected(v, 5, corner, 0);
                CHECK(drawn == want.size() && drawn == 5);
                CHECK(OnlyInside(fb, want));
                for (u32 k = 0; k < want.size(); ++k) {
                    const chat::Line& l = v.lines[corner < 2 ? k : 4 - k];
                    for (int e = 0; e <= stereo; ++e) {
                        CHECK(BlockIsRightShadow(fb, e, want[k], l));
                    }
                }
                // the cost: far fewer stores than the opaque block (no background at all: the letters and their shadows, and the bar)
                Fb fo(f, stereo != 0);
                fo.Paint();
                ResetStores();
                chat::DrawChatLines(fo.Surface(), v, 5, corner, 0);
                CHECK(bytes < StoredBytes());
                opaque_bytes += StoredBytes();
                shadow_bytes += bytes;
            }
        }
    }
    std::printf("shadow text: %lu bytes stored against %lu for the opaque lines (%lu%%)\n", shadow_bytes, opaque_bytes, shadow_bytes * 100 / opaque_bytes);
}
static void TestChatLines() {
    const chat::View v = FiveMessages();
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        for (int stereo = 0; stereo < 2; ++stereo) {
            for (unsigned rows : {240u, 256u}) {
                for (int corner = 0; corner < 4; ++corner) {
                    for (u32 notices : {0u, 2u}) {
                        if (notices && corner != chat::kBottomRight) {
                            continue;
                        }
                        Fb fb(f, stereo != 0, rows);
                        fb.Paint();
                        const unsigned allocs = g_allocs.load();
                        ResetStores();
                        const u32 drawn = chat::DrawChatLines(fb.Surface(), v, 5, corner, notices);
                        CHECK(g_allocs.load() == allocs);
                        const std::vector<Rect> want = Expected(v, 5, corner, notices);
                        CHECK(drawn == want.size() && drawn == 5);
                        CHECK(OnlyInside(fb, want));
                        unsigned long area = 0;
                        for (u32 k = 0; k < want.size(); ++k) {
                            const chat::Line& l = v.lines[corner < 2 ? k : 4 - k];
                            area += static_cast<unsigned long>(want[k].w) * want[k].h;
                            for (int e = 0; e <= stereo; ++e) {
                                CHECK(BlockIsRight(fb, e, want[k], l));
                            }
                        }
                        // the cost: every pixel of the blocks written once, on each image, and only with words
                        const unsigned long eyes = stereo ? 2 : 1;
                        CHECK(StoredBytes() == area * fb.bpp * eyes);
                        CHECK(g_stores[1] == 0 && g_stores[2] == 0);
                        if (rows == 240 && !stereo) {
                            const std::string name = std::string("chat-") + kCornerNames[corner] + (notices ? "-notices" : "") + "-" + kFormatNames[f];
                            if (f == gfx::kBGR8 || corner == chat::kBottomLeft) {
                                if (notices) {
                                    // (the framework's notifications, as they would be: right-aligned at 390, 15 rows each, above y = 230)
                                    const char* const notes[2] = {"Joined the room", "Marta_92 joined the room"};
                                    for (u32 n = 0; n < notices; ++n) {
                                        const u32 len = static_cast<u32>(std::strlen(notes[n]));
                                        const int y = 230 - 15 * static_cast<int>(notices) + 15 * static_cast<int>(n);
                                        gfx::FillRect(fb.Surface(), 390 - static_cast<int>(len) * 6 - 2, y, static_cast<int>(len) * 6 + 2, 13, {0, 0, 0});
                                        gfx::DrawText(fb.Surface(), 390 - static_cast<int>(len) * 6 - 1, y, notes[n], len, gfx::kFont6x12, {255, 255, 255}, {0, 0, 0});
                                    }
                                }
                                WriteBmp(fb, 0, name);
                            }
                        }
                        if (rows == 240 && stereo && f == gfx::kBGR8 && corner == chat::kBottomLeft) {
                            WriteBmp(fb, 1, "chat-bottom-left-bgr8-right-eye");
                        }
                    }
                }
            }
        }
    }

    // fewer lines shown than kept: the newest ones; none: nothing at all
    for (int corner = 0; corner < 4; ++corner) {
        Fb fb(gfx::kBGR8, false);
        fb.Paint();
        CHECK(chat::DrawChatLines(fb.Surface(), v, 2, corner, 0) == 2);
        CHECK(OnlyInside(fb, Expected(v, 2, corner, 0)));
        const std::vector<Rect> want = Expected(v, 2, corner, 0);
        CHECK(BlockIsRight(fb, 0, want[0], v.lines[corner < 2 ? 3 : 4]));
        fb.Paint();
        CHECK(chat::DrawChatLines(fb.Surface(), v, 0, corner, 0) == 0);
        CHECK(OnlyInside(fb, {}));
    }
    // the bottom-right corner climbs with the notifications; the blocks that no longer fit are dropped, the oldest first
    for (u32 notices = 0; notices <= 15; ++notices) {
        Fb fb(gfx::kRGB565, false);
        fb.Paint();
        const std::vector<Rect> want = Expected(v, 5, chat::kBottomRight, notices);
        CHECK(chat::BottomRightY(notices) == (notices ? ((227 - 15 * static_cast<int>(notices)) / 4) * 4 : 236));
        const u32 drawn = chat::DrawChatLines(fb.Surface(), v, 5, chat::kBottomRight, notices);
        CHECK(drawn == want.size());
        CHECK(OnlyInside(fb, want));
        // (the five blocks are 88 rows, no gaps between them: up to 9 notifications they fit, with 10 the oldest no longer does)
        CHECK(notices > 9 || drawn == 5);
        CHECK(notices != 10 || drawn < 5); // (the oldest goes first)
        if (notices == 10) {
            WriteBmp(fb, 0, "chat-bottom-right-10-notices-rgb565");
        }
    }
    // a stack stops where the one in the opposite corner begins (the plugin's notifications): the edge it hands back is the row past it and
    // its gap, the other stack's limit; the oldest blocks of the other one go
    {
        chat::View notes;
        for (u32 i = 0; i < 5; ++i) {
            notes.lines[i] = MakeLine("", "Gionko joined the room (Old 3DS, 3DS Link 1.1.2)", 1000 + i);
        }
        notes.count = 5;
        chat::View tall; // (5 messages of three lines: 176 rows with their gaps, more than the 148 the notifications leave)
        for (u32 i = 0; i < 5; ++i) {
            tall.lines[i] = MakeLine("LinkFan", kMax, 2000 + i);
        }
        tall.count = 5;
        for (int corner = 0; corner < 4; ++corner) {
            const int across = chat::Opposite(corner);
            Fb fb(gfx::kBGR8, false);
            fb.Paint();
            int edge = 0;
            CHECK(chat::DrawChatLines(fb.Surface(), notes, 5, across, 0, chat::kNoLimit, &edge) == 5);
            const bool notes_top = across == chat::kTopLeft || across == chat::kTopRight;
            CHECK(edge == (notes_top ? 8 + 5 * (12 + chat::kGapY) : 236 - 5 * (12 + chat::kGapY))); // (5 blocks of one line, 12 rows, and their gaps)
            const std::vector<Rect> want_notes = Expected(notes, 5, across, 0);
            const std::vector<Rect> want_chat = Expected(tall, 5, corner, 0, edge);
            const u32 drawn = chat::DrawChatLines(fb.Surface(), tall, 5, corner, 0, edge);
            CHECK(drawn == want_chat.size() && drawn == 5); // (with no gaps between the blocks all five fit: 160 rows of the 168 the notifications leave)
            std::vector<Rect> all = want_notes;
            all.insert(all.end(), want_chat.begin(), want_chat.end());
            CHECK(OnlyInside(fb, all));
            for (const Rect& n : want_notes) {
                CHECK(BlockIsRight(fb, 0, n, notes.lines[0]));
                for (const Rect& c : want_chat) {
                    const bool apart = n.y + n.h + chat::kGapY <= c.y || c.y + c.h + chat::kGapY <= n.y;
                    CHECK(apart); // (never over the chat: a gap of 4 rows at least)
                }
            }
            for (u32 k = 0; k < want_chat.size(); ++k) {
                CHECK(BlockIsRight(fb, 0, want_chat[k], tall.lines[0]));
            }
            if (corner == chat::kBottomLeft) {
                WriteBmp(fb, 0, "chat-bottom-left-notices-top-right-bgr8");
            }
            if (corner == chat::kTopLeft) {
                WriteBmp(fb, 0, "chat-top-left-notices-bottom-right-bgr8");
            }
        }
    }
    // a block whose columns would pass the screen's edge is not drawn (the static_asserts keep kCols inside: this is the guard behind them)
    {
        Fb fb(gfx::kBGR8, false);
        fb.Paint();
        CHECK(chat::DrawChatLines(fb.Surface(), v, 5, chat::kTopLeft, 0, chat::kNoLimit) == 5);
        CHECK(chat::kLeftX + chat::BlockWidth(chat::kCols) <= 400 && chat::kRightEdge - chat::BlockWidth(chat::kCols) >= 0);
        CHECK(chat::BlockHeight(1) == 12 && chat::BlockHeight(2) == 24 && chat::BlockHeight(3) == 32);
    }
    // a view that says more than it holds draws what it holds
    {
        chat::View odd = v;
        odd.count = 9;
        Fb fb(gfx::kRGBA8, false);
        fb.Paint();
        CHECK(chat::DrawChatLines(fb.Surface(), odd, 9, chat::kTopLeft, 0) == 5);
    }
    // a surface that is not the top screen's, or one the layer refuses, gets nothing
    {
        Fb fb(gfx::kBGR8, false, 240, 320);
        fb.Paint();
        CHECK(chat::DrawChatLines(fb.Surface(), v, 5, chat::kTopLeft, 0) == 0);
        Fb fb2(gfx::kBGR8, false);
        fb2.Paint();
        CHECK(chat::DrawChatLines(fb2.Surface(false), v, 5, chat::kTopLeft, 0) == 0);
        CHECK(OnlyInside(fb, {}) && OnlyInside(fb2, {}));
    }
}

// The cost of the worst frame: 5 blocks of 3 lines as wide as a block gets, in every format, with the 3D on. The budget is the bytes of the
// pixels a word at a time (no byte or halfword at all), so a change that writes a pixel twice, or goes back to smaller stores, fails here.
static void TestCost() {
    chat::View v;
    const std::string nick(chat::kNickMax, 'W'), text(chat::kTextMax, 'm');
    for (u32 i = 0; i < chat::kLines; ++i) {
        v.lines[i] = MakeLine(nick.c_str(), text.c_str(), 1000);
    }
    v.count = chat::kLines;
    const unsigned long block = static_cast<unsigned long>(chat::BlockWidth(chat::kCols)) * chat::BlockHeight(3);
    CHECK(chat::BlockWidth(chat::kCols) == 382 && chat::BlockHeight(3) == 32);
    std::printf("  the worst frame (5 blocks of 382 x 32, both images):");
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        Fb fb(f, true);
        fb.Paint();
        ResetStores();
        CHECK(chat::DrawChatLines(fb.Surface(), v, 5, chat::kBottomLeft, 0) == 5);
        const unsigned long words = 5 * block * fb.bpp / 4 * 2;
        CHECK(g_stores[4] == words && g_stores[1] == 0 && g_stores[2] == 0);
        std::printf(" %s %lu words", kFormatNames[f], g_stores[4]);
    }
    std::printf("\n");
}

// The two fonts, every glyph, for the user to see.
static void FontSheet() {
    Fb fb(gfx::kBGR8, false);
    fb.Paint();
    gfx::FillRect(fb.Surface(), 0, 0, 400, 240, chat::kBg);
    char row[48];
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < 48; ++i) {
            row[i] = static_cast<char>(0x20 + r * 48 + i < 0x7F ? 0x20 + r * 48 + i : ' ');
        }
        gfx::DrawText(fb.Surface(), 8, 8 + r * 14, row, 48, gfx::kFont6x12, chat::kText, chat::kBg);
        gfx::DrawText(fb.Surface(), 8, 48 + r * 18, row, 48, gfx::kFont8x16, chat::kText, chat::kBg);
    }
    const char* sample = "Spleen 6x12 and 8x16 - BSD 2-Clause, Frederic Cambus";
    gfx::DrawText(fb.Surface(), 8, 100, sample, static_cast<u32>(std::strlen(sample)), gfx::kFont6x12, chat::kNick[0], chat::kBg);
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < 48; ++i) {
            row[i] = static_cast<char>(0x20 + r * 48 + i < 0x7F ? 0x20 + r * 48 + i : ' ');
        }
        gfx::DrawText(fb.Surface(), 8, 120 + r * 10, row, 48, gfx::kFont6x10, chat::kText, chat::kBg);
    }
    const char* sample10 = "6x10 (the chat): gpqy jQ ($[x]) {a|b} / 100%";
    gfx::DrawText(fb.Surface(), 8, 144, sample10, static_cast<u32>(std::strlen(sample10)), gfx::kFont6x10, chat::kNick[1], chat::kBg);
    WriteBmp(fb, 0, "fonts-spleen");
}

int main(int argc, char** argv) {
    if (argc > 1) {
        g_out = argv[1];
    }
    TestFormatsAndFonts();
    TestRectsAndText();
    TestWrapAndColours();
    TestChatLines();
    TestTranslucent();
    TestCost();
    FontSheet();
    std::printf("chat_draw_test: %d checks, %d failed%s%s\n", g_checks, g_failures, g_out.empty() ? "" : "; pictures in ", g_out.c_str());
    return g_failures == 0 ? 0 : 1;
}
