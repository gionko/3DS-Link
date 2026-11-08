// The plugin's own drawing on a framebuffer of the 3DS: rectangles and the fonts of Spleen (azl/font_spleen.inc), written a word at a
// time and never read. Plain C++ and no heap: the in-game chat draws with it on the game's thread, inside the framework's OSD callback
// (the callback runs before the game's own present, so its time is the game's frame), and the PC tests draw with it into fake framebuffers
// (Tests/chat_draw_test.cpp, which also writes the pictures).
//
// Why not the framework's drawing (CTRPluginFramework 0.8.0): Screen::DrawRect writes a byte at a time, DrawSysfont reads the framebuffer,
// blends in floating point and fills its glyph cache with new; and both write only the left image of the 3D. The framebuffers are
// strongly-ordered, uncached memory (a read costs more than a write): here a pixel is only ever written, the runs of a column go out as
// 32-bit words, and the right image gets the same writes when the 3D is on.
//
// The layout is the framework's (Screen::GetFramebuffer, OSD.cpp): the screen is stored rotated, a column of the screen is one run of
// memory from its bottom row up, and the pixel (x, y) is at ((h - 1 - y) + x * rowsize) * bpp, where rowsize = stride / bpp (the framework
// writes it as rowsize - 1 - (y + rowsize - 240): the same thing, the rows past 240 of a longer column are the gap after the visible ones).
// The five formats are the GSP's (format & 7): RGBA8, BGR8, RGB565, RGB5A1, RGBA4; anything else is not drawn on.
#pragma once

#include <cstddef>
#include <cstdint>

#include "azl/common.hpp"

// The test counts the stores (the cost of a frame): it defines this before the include. The plugin's is nothing.
#ifndef AZL_GFX_COUNT_STORE
#define AZL_GFX_COUNT_STORE(bytes) ((void)0)
#endif

namespace azl {
namespace gfx {

#include "azl/font_spleen.inc"

/// The GSP's framebuffer formats (GSPGPU_FramebufferFormat, the low 3 bits of the screen's format).
enum Format : u32 { kRGBA8 = 0, kBGR8 = 1, kRGB565 = 2, kRGB5A1 = 3, kRGBA4 = 4 };
constexpr u32 kFormats = 5;

/// Bytes of a pixel of `format` (0: not a format this layer draws on).
constexpr u32 BytesPerPixel(u32 format) {
    return format == kRGBA8 ? 4 : format == kBGR8 ? 3 : format <= kRGBA4 ? 2 : 0;
}

/// One screen as the framework hands it (CTRPluginFramework::Screen): `left` is the image drawn on, `right` the right eye's (only the top
/// screen with the 3D on; nullptr otherwise), `stride` the bytes of one column of the screen, `w` x `h` the visible pixels (400 or 320 x 240).
struct Surface {
    u8* left = nullptr;
    u8* right = nullptr;
    u32 stride = 0;
    u8 bpp = 0;
    u8 format = 0;
    u16 w = 0;
    u16 h = 0;
};

/// A surface this layer may write: a known format whose pixel is `bpp` bytes, columns that hold the visible rows, words that are words.
inline bool Usable(const Surface& s) {
    return s.left != nullptr && s.format < kFormats && s.bpp == BytesPerPixel(s.format) && s.w != 0 && s.h != 0 && s.stride % s.bpp == 0 &&
           s.stride / s.bpp >= s.h && s.stride % 4 == 0 && (reinterpret_cast<std::uintptr_t>(s.left) & 3) == 0 &&
           (reinterpret_cast<std::uintptr_t>(s.right) & 3) == 0;
}

/// A screen of the framework (CTRPluginFramework::Screen) as this layer takes it. The pixel is the low 3 bits of the format only: the
/// framework hands the game's format byte as the game set it (0x40 the top screen in 2D, 0x20 the 3D: 0x42 is RGB565 on the top screen), and
/// its own Screen::BytesPerPixel is worked out of that byte unmasked (3 for any value it does not know: RGBA8 and the 16-bit formats with a
/// high bit would never be Usable, the chat not shown). The bytes of a pixel come from the masked format, never from the framework's.
inline Surface ScreenSurface(u8* left, u8* right, u32 stride, u32 raw_format, u16 w, u16 h) {
    Surface s;
    s.left = left;
    s.right = right;
    s.stride = stride;
    s.format = static_cast<u8>(raw_format & 7);
    s.bpp = static_cast<u8>(BytesPerPixel(s.format));
    s.w = w;
    s.h = h;
    return s;
}

struct Rgb {
    u8 r, g, b;
};

/// The bytes of one pixel of `c` in `format`, as a little-endian number (what a store of the pixel's bpp bytes writes). Opaque: every
/// alpha that the format has is full. RGBA8 is A, B, G, R in memory; BGR8 is B, G, R.
constexpr u32 Raw(Rgb c, u32 format) {
    return format == kRGBA8 ? (static_cast<u32>(c.r) << 24 | static_cast<u32>(c.g) << 16 | static_cast<u32>(c.b) << 8 | 0xFFu)
         : format == kBGR8 ? (static_cast<u32>(c.r) << 16 | static_cast<u32>(c.g) << 8 | c.b)
         : format == kRGB565 ? (static_cast<u32>(c.r >> 3) << 11 | static_cast<u32>(c.g >> 2) << 5 | static_cast<u32>(c.b >> 3))
         : format == kRGB5A1 ? (static_cast<u32>(c.r >> 3) << 11 | static_cast<u32>(c.g >> 3) << 6 | static_cast<u32>(c.b >> 3) << 1 | 1u)
                             : (static_cast<u32>(c.r >> 4) << 12 | static_cast<u32>(c.g >> 4) << 8 | static_cast<u32>(c.b >> 4) << 4 | 0xFu);
}

/// A font of the tables above: `cols` = the first glyph's first column; a glyph is `w` columns of `h` bits, bit 0 the bottom row.
struct Font {
    const u16* cols;
    u8 w, h;
};
constexpr Font kFont6x12 = {&kSpleen6x12[0][0], 6, 12};
constexpr Font kFont6x10 = {&kSpleen6x10[0][0], 6, 10}; ///< the 6x12 glyphs in a cell two rows shorter (the chat's lines: azl/chat_draw.hpp)
constexpr Font kFont8x16 = {&kSpleen8x16[0][0], 8, 16};

/// The three colours of a run, already in the surface's format: `a` where the first mask has a bit, `b` where the second has, `bg` elsewhere.
struct Ink {
    u32 a, b, bg;
};

// >>> game-thread code (Tests/policy_check.sh: the chat draws on the game's thread: no heap, no wait, no printf, no big buffer)

// (stores of the pixel's own width that may alias the framebuffer's bytes; the plugin is built with -fno-strict-aliasing as well)
typedef u32 __attribute__((may_alias, aligned(4))) Word;
typedef u16 __attribute__((may_alias, aligned(2))) Half;

inline void Put32(u8* p, u32 v) {
    AZL_GFX_COUNT_STORE(4);
    *reinterpret_cast<Word*>(p) = v;
}
inline void Put16(u8* p, u32 v) {
    AZL_GFX_COUNT_STORE(2);
    *reinterpret_cast<Half*>(p) = static_cast<u16>(v);
}
inline void Put8(u8* p, u32 v) {
    AZL_GFX_COUNT_STORE(1);
    *p = static_cast<u8>(v);
}

/// The colour of the pixel whose bit is `bit` in the masks: `a` where the first mask has it, else `b` where the second has it, else `bg`.
/// Always in line, and the colours handed as values, not read through the Ink: this is the inner loop of every column (out of line it
/// was a call a pixel, and the Ink read through a reference is read again after every store, which may alias it).
__attribute__((always_inline)) inline u32 PickBit(u32 ma, u32 mb, u32 bit, u32 a, u32 b, u32 bg) {
    u32 v = bg;
    if ((mb & bit) != 0) {
        v = b;
    }
    if ((ma & bit) != 0) {
        v = a; // (the first mask wins)
    }
    return v;
}

/// One pixel of `bpp` bytes (the pixels before a column's first word and after its last whole group).
inline void PutPixel(u8* p, u32 v, u32 bpp) {
    if (bpp == 4) {
        Put32(p, v);
    } else if (bpp == 2) {
        Put16(p, v);
    } else {
        Put8(p, v);
        Put8(p + 1, v >> 8);
        Put8(p + 2, v >> 16);
    }
}

/// The same word on the left image and, with the 3D, on the right one (laid out alike: the same offset is the same pixel).
__attribute__((always_inline)) inline void Put32Both(u8* l, u8* r, u32 off, u32 v) {
    Put32(l + off, v);
    if (r != nullptr) {
        Put32(r + off, v);
    }
}

// A whole group of four pixels (4, 3 or 2 words from a word-aligned address) on the left image and, with the 3D, on the right one. On the
// console the group goes out as ONE stm a image, the words in r4-r7: the framebuffer is strongly ordered and uncached, every store is a
// transaction of its own on the bus that the core waits for, and one store a word was 83 440 of them in the worst frame of the chat's lines
// (RGBA8, the 3D on); a block store is one burst. On the PC (and in the cost probe's reference build, AZL_GFX_NO_BURST) the same words go
// out one at a time, so that Tests/chat_draw_test.cpp counts them and checks every pixel; Tests/chat_draw_cost.py checks the burst's
// framebuffers byte by byte against the reference build's and counts the bus writes of a frame.
#if defined(__arm__) && !defined(__thumb__) && !defined(AZL_GFX_NO_BURST)
__attribute__((always_inline)) inline void Put4Both(u8* l, u8* r, u32 off, u32 w0, u32 w1, u32 w2, u32 w3) {
    register u32 x0 __asm__("r4") = w0;
    register u32 x1 __asm__("r5") = w1;
    register u32 x2 __asm__("r6") = w2;
    register u32 x3 __asm__("r7") = w3;
    __asm__ volatile("stm %0, {r4-r7}" : : "r"(l + off), "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    if (r != nullptr) {
        __asm__ volatile("stm %0, {r4-r7}" : : "r"(r + off), "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    }
}
__attribute__((always_inline)) inline void Put3Both(u8* l, u8* r, u32 off, u32 w0, u32 w1, u32 w2) {
    register u32 x0 __asm__("r4") = w0;
    register u32 x1 __asm__("r5") = w1;
    register u32 x2 __asm__("r6") = w2;
    __asm__ volatile("stm %0, {r4-r6}" : : "r"(l + off), "r"(x0), "r"(x1), "r"(x2) : "memory");
    if (r != nullptr) {
        __asm__ volatile("stm %0, {r4-r6}" : : "r"(r + off), "r"(x0), "r"(x1), "r"(x2) : "memory");
    }
}
__attribute__((always_inline)) inline void Put2Both(u8* l, u8* r, u32 off, u32 w0, u32 w1) {
    register u32 x0 __asm__("r4") = w0;
    register u32 x1 __asm__("r5") = w1;
    __asm__ volatile("stm %0, {r4-r5}" : : "r"(l + off), "r"(x0), "r"(x1) : "memory");
    if (r != nullptr) {
        __asm__ volatile("stm %0, {r4-r5}" : : "r"(r + off), "r"(x0), "r"(x1) : "memory");
    }
}
#else
__attribute__((always_inline)) inline void Put4Both(u8* l, u8* r, u32 off, u32 w0, u32 w1, u32 w2, u32 w3) {
    Put32Both(l, r, off, w0);
    Put32Both(l, r, off + 4, w1);
    Put32Both(l, r, off + 8, w2);
    Put32Both(l, r, off + 12, w3);
}
__attribute__((always_inline)) inline void Put3Both(u8* l, u8* r, u32 off, u32 w0, u32 w1, u32 w2) {
    Put32Both(l, r, off, w0);
    Put32Both(l, r, off + 4, w1);
    Put32Both(l, r, off + 8, w2);
}
__attribute__((always_inline)) inline void Put2Both(u8* l, u8* r, u32 off, u32 w0, u32 w1) {
    Put32Both(l, r, off, w0);
    Put32Both(l, r, off + 4, w1);
}
#endif

/// Writes the `n` (at most 32) pixels of one column that start at `l` (and at `r`, the right image's, or nullptr) and go up the screen
/// (pixel i at l + i * bpp, which is the row i above the run's bottom one), each in the colour its bits pick (PickBit), worked out once for
/// both images. Four pixels a turn, as whole words: 4 (RGBA8), 3 (BGR8) or 2 (16-bit) from a word-aligned start; a group of four in which
/// neither mask has a bit (most of a chat strip: the paddings, the rows and the columns between the glyphs) is the background's words,
/// nothing worked out. Only the pixels before the first aligned one and after the last whole group go out one by one, as bytes or
/// halfwords (the chat's strips start and end on rows that make none). This runs on the game's thread for every pixel of the chat's
/// lines, for 10 s after a message: Tests/chat_draw_cost.sh holds its instructions on the plugin's own compiler and flags.
inline void RunColumnBoth(u8* l, u8* r, u32 n, u32 ma, u32 mb, const Ink& ink, u32 bpp) {
    const u32 a = ink.a, b = ink.b, bg = ink.bg;
    u32 off = 0;
    for (; n != 0 && (reinterpret_cast<std::uintptr_t>(l + off) & 3) != 0; --n, off += bpp) {
        const u32 v = PickBit(ma, mb, 1, a, b, bg);
        PutPixel(l + off, v, bpp);
        if (r != nullptr) {
            PutPixel(r + off, v, bpp);
        }
        ma >>= 1;
        mb >>= 1;
    }
    const u32 end = off + (n & ~3u) * bpp;
    if (bpp == 4) {
        for (; off != end; off += 16, ma >>= 4, mb >>= 4) {
            if (((ma | mb) & 15) == 0) {
                Put4Both(l, r, off, bg, bg, bg, bg);
            } else {
                Put4Both(l, r, off, PickBit(ma, mb, 1, a, b, bg), PickBit(ma, mb, 2, a, b, bg), PickBit(ma, mb, 4, a, b, bg),
                         PickBit(ma, mb, 8, a, b, bg));
            }
        }
    } else if (bpp == 2) {
        const u32 bg2 = bg | bg << 16;
        for (; off != end; off += 8, ma >>= 4, mb >>= 4) {
            if (((ma | mb) & 15) == 0) {
                Put2Both(l, r, off, bg2, bg2);
            } else {
                Put2Both(l, r, off, PickBit(ma, mb, 1, a, b, bg) | PickBit(ma, mb, 2, a, b, bg) << 16,
                         PickBit(ma, mb, 4, a, b, bg) | PickBit(ma, mb, 8, a, b, bg) << 16);
            }
        }
    } else {
        // (BGR8: 4 pixels are 12 bytes, three words)
        for (; off != end; off += 12, ma >>= 4, mb >>= 4) {
            u32 c0 = bg, c1 = bg, c2 = bg, c3 = bg;
            if (((ma | mb) & 15) != 0) {
                c0 = PickBit(ma, mb, 1, a, b, bg);
                c1 = PickBit(ma, mb, 2, a, b, bg);
                c2 = PickBit(ma, mb, 4, a, b, bg);
                c3 = PickBit(ma, mb, 8, a, b, bg);
            }
            Put3Both(l, r, off, c0 | c1 << 24, c1 >> 8 | c2 << 16, c2 >> 16 | c3 << 8);
        }
    }
    for (n &= 3; n != 0; --n, off += bpp) {
        const u32 v = PickBit(ma, mb, 1, a, b, bg);
        PutPixel(l + off, v, bpp);
        if (r != nullptr) {
            PutPixel(r + off, v, bpp);
        }
        ma >>= 1;
        mb >>= 1;
    }
}

/// The offset of the pixel (x, y) from the start of an image (the caller keeps x < w and y < h). (x * rowsize + (h - 1 - y)) * bpp with
/// rowsize = stride / bpp is x * stride + (h - 1 - y) * bpp, and no division: the ARM11 has none, and it was a call to the library's.
inline u32 PixelOffset(const Surface& s, u32 x, u32 y) {
    return x * s.stride + (s.h - 1u - y) * s.bpp;
}

/// One column of the rows [y, y + n) at x (n at most 32; bit i of the masks is the row y + n - 1 - i), on the left image and, with the 3D,
/// on the right one.
inline void ColumnBoth(const Surface& s, u32 x, u32 y, u32 n, u32 ma, u32 mb, const Ink& ink) {
    const u32 off = PixelOffset(s, x, y + n - 1);
    RunColumnBoth(s.left + off, s.right != nullptr ? s.right + off : nullptr, n, ma, mb, ink, s.bpp);
}

/// A rectangle in one colour, cut to the screen.
inline void FillRect(const Surface& s, int x, int y, int w, int h, Rgb c) {
    if (!Usable(s)) {
        return;
    }
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > s.w ? s.w : x + w, y1 = y + h > s.h ? s.h : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    const u32 raw = Raw(c, s.format);
    const Ink ink = {raw, raw, raw};
    for (int cx = x0; cx < x1; ++cx) {
        // (from the top of the rectangle down, 32 rows a run; a run of 32 keeps the alignment of the next one)
        for (int top = y0; top < y1; top += 32) {
            const u32 n = static_cast<u32>(y1 - top < 32 ? y1 - top : 32);
            ColumnBoth(s, static_cast<u32>(cx), static_cast<u32>(top), n, 0, 0, ink);
        }
    }
}

/// The glyph of `c` in the fonts' tables (not printable ASCII: the glyph of '?').
inline u32 GlyphIndex(char c) {
    const u32 code = static_cast<u8>(c);
    return code >= kFontFirst && code < kFontFirst + kFontGlyphs ? code - kFontFirst : static_cast<u32>('?') - kFontFirst;
}

/// The column `col` of the glyph of `c`.
inline u32 GlyphColumn(const Font& f, char c, u32 col) {
    return f.cols[GlyphIndex(c) * f.w + col];
}

/// `len` characters of `text` in cells of the font, opaque (`fg` on `bg`), the cell's top row at y. The columns past the right edge are not
/// drawn; a line whose cells are not all inside the screen's rows is not drawn at all.
inline void DrawText(const Surface& s, int x, int y, const char* text, u32 len, const Font& f, Rgb fg, Rgb bg) {
    if (!Usable(s) || y < 0 || y + f.h > s.h) {
        return;
    }
    const Ink ink = {Raw(fg, s.format), 0, Raw(bg, s.format)};
    for (u32 i = 0; i < len; ++i) {
        for (u32 col = 0; col < f.w; ++col) {
            const int cx = x + static_cast<int>(i * f.w + col);
            if (cx >= 0 && cx < s.w) {
                ColumnBoth(s, static_cast<u32>(cx), static_cast<u32>(y), f.h, GlyphColumn(f, text[i], col), 0, ink);
            }
        }
    }
}
// <<< game-thread code

} // namespace gfx
} // namespace azl
