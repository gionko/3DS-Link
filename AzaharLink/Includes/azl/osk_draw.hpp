// The on-screen keyboard drawn (azl/osk.hpp's View) on the bottom screen, with the plugin's own drawing (azl/gfx_surface.hpp: columns
// written as words, never read) and Spleen 8x16. Plain C++, no heap. It draws the whole screen, every pixel once outside the text area and
// the bar (the keys: their dark line, their light top edge, the fill and the label of a column go out as one run), and it is not code of the
// game's thread: the paused menu draws with it straight on the framebuffer, the game running will draw into a copy of its own on the
// plugin's thread (the game's thread then only copies).
//
// The look (the user's decisions, 2026-11-04): the layout of the system's keyboard in a dark version in the colours of 3DS Link
// (ConfigApp/source/gfx.hpp): the keys in the grey of the app's panels with a light top edge and a dark line around them, as the system's
// keys have, the function keys (backspace, enter, shift, caps lock, space) in a dark green as the system's are green, the active tab and
// Send / OK in the accent, the key of the D-pad in the selection's blue with an accent outline, the key under the finger lit in the accent,
// Send off (grey) while there is nothing to send. The icons are drawn here as pixels (no font has them): the arrows of backspace and enter,
// the arrow of shift, the turning arrows of caps lock (a <-> A), the triangles of the scroll column.
#pragma once

#include "azl/gfx_surface.hpp"
#include "azl/osk.hpp"

namespace azl {
namespace osk {

/// The colours (3DS Link's: kBg, kPanel, kText, kDim, kAccent, kSel, kOn, kWarn, kOff, kBarText; the rest are steps between them).
constexpr gfx::Rgb kBg = {13, 14, 17};
constexpr gfx::Rgb kLineDark = {6, 7, 9};
constexpr gfx::Rgb kField = {24, 26, 31};
constexpr gfx::Rgb kDots = {62, 68, 80};
constexpr gfx::Rgb kBarBg = {17, 19, 23};
constexpr gfx::Rgb kInk = {232, 234, 238};
constexpr gfx::Rgb kDimInk = {138, 144, 156};
constexpr gfx::Rgb kAccent = {70, 178, 255};
constexpr gfx::Rgb kAccentEdge = {150, 212, 255};
constexpr gfx::Rgb kDarkInk = {20, 22, 27};
constexpr gfx::Rgb kSel = {34, 56, 90};
constexpr gfx::Rgb kGreen = {24, 82, 60};
constexpr gfx::Rgb kGreenEdge = {44, 124, 92};
constexpr gfx::Rgb kGreenInk = {226, 248, 236};
constexpr gfx::Rgb kOn = {74, 218, 128};
constexpr gfx::Rgb kWarn = {255, 198, 88};
constexpr gfx::Rgb kOff = {244, 100, 100};

/// How a key is painted: its fill, its top edge (or, `outline`, a 1-pixel frame inside its line), its label.
struct KeyLook {
    gfx::Rgb fill, edge, label;
    bool outline;
};
constexpr KeyLook kLookKey = {{40, 44, 52}, {66, 72, 84}, kInk, false};
constexpr KeyLook kLookFunction = {kGreen, kGreenEdge, kGreenInk, false};
constexpr KeyLook kLookFunctionOn = {kOn, {150, 240, 184}, kDarkInk, false};
constexpr KeyLook kLookOff = {{26, 28, 33}, {32, 35, 41}, {70, 76, 88}, false};
constexpr KeyLook kLookScroll = {{32, 36, 43}, {48, 53, 62}, kInk, false};
constexpr KeyLook kLookScrollOff = {{32, 36, 43}, {48, 53, 62}, {70, 76, 88}, false};
constexpr KeyLook kLookTab = {{50, 54, 63}, {72, 78, 90}, {196, 200, 210}, false};
constexpr KeyLook kLookTabOn = {kAccent, kAccentEdge, kDarkInk, false};
constexpr KeyLook kLookCancel = {{36, 40, 47}, {58, 63, 74}, kInk, false};
constexpr KeyLook kLookSubmit = {kAccent, kAccentEdge, kDarkInk, false};
constexpr KeyLook kLookSubmitOff = {{46, 50, 58}, {54, 58, 67}, {100, 106, 118}, false};
constexpr KeyLook kLookSelected = {kSel, kAccent, kInk, true};
constexpr KeyLook kLookPressed = {kAccent, kAccentEdge, kDarkInk, false};

/// A picture of pixels: `h` rows of `w` characters, '#' drawn.
struct Icon {
    const char* const* rows;
    u8 w, h;
};

namespace art {
constexpr const char* kBackspaceRows[] = {
    "     #            ",
    "    ##            ",
    "   ###            ",
    "  ################",
    " #################",
    "##################",
    " #################",
    "  ################",
    "   ###            ",
    "    ##            ",
    "     #            ",
};
constexpr const char* kEnterRows[] = {
    "            ####",
    "            ####",
    "     #      ####",
    "    ##      ####",
    "   ###      ####",
    "  ##############",
    " ###############",
    "  ##############",
    "   ###          ",
    "    ##          ",
    "     #          ",
};
constexpr const char* kShiftRows[] = {
    "      #      ",
    "     ###     ",
    "    #####    ",
    "   #######   ",
    "  #########  ",
    " ########### ",
    "#############",
    "    #####    ",
    "    #####    ",
    "    #####    ",
    "    #####    ",
    "    #####    ",
};
constexpr const char* kTurnRows[] = {
    "   #####   ",
    "  ##   ## #",
    " ##     ###",
    " #     ####",
    " #         ",
    "           ",
    "         # ",
    "####     # ",
    "###     ## ",
    "# ##   ##  ",
    "   #####   ",
};
constexpr const char* kUpRows[] = {
    "     #     ",
    "    ###    ",
    "   #####   ",
    "  #######  ",
    " ######### ",
    "###########",
};
constexpr const char* kDownRows[] = {
    "###########",
    " ######### ",
    "  #######  ",
    "   #####   ",
    "    ###    ",
    "     #     ",
};
} // namespace art

constexpr Icon kIconBackspace = {art::kBackspaceRows, 18, 11};
constexpr Icon kIconEnter = {art::kEnterRows, 16, 11};
constexpr Icon kIconShift = {art::kShiftRows, 13, 12};
constexpr Icon kIconTurn = {art::kTurnRows, 11, 11};
constexpr Icon kIconUp = {art::kUpRows, 11, 6};
constexpr Icon kIconDown = {art::kDownRows, 11, 6};

/// The column `col` of an icon as the font's columns are: bit 0 the bottom row.
inline u32 IconColumn(const Icon& icon, u32 col) {
    u32 bits = 0;
    for (u32 j = 0; j < icon.h; ++j) {
        if (icon.rows[j][col] == '#') {
            bits |= 1u << (icon.h - 1 - j);
        }
    }
    return bits;
}

/// One thing on a key: a glyph of Spleen 8x16 (`icon` nullptr) or an icon, its top-left pixel at (x, y) from the key's.
struct Mark {
    const Icon* icon;
    char ch;
    u8 x, y;
};
constexpr u32 kMaxMarks = 8;
struct Label {
    Mark marks[kMaxMarks];
    u32 n = 0;
    u32 width = 0; ///< from the left of the first to the right of the last
};

inline void AddGlyph(Label& l, char ch, u32 gap = 0) {
    if (l.n < kMaxMarks) {
        l.width += l.n ? gap : 0;
        l.marks[l.n++] = {nullptr, ch, static_cast<u8>(l.width), 0};
        l.width += 8;
    }
}
inline void AddIcon(Label& l, const Icon& icon, u32 gap = 0) {
    if (l.n < kMaxMarks) {
        l.width += l.n ? gap : 0;
        l.marks[l.n++] = {&icon, 0, static_cast<u8>(l.width), 0};
        l.width += icon.w;
    }
}
inline void AddText(Label& l, const char* text) {
    for (const char* p = text; *p; ++p) {
        AddGlyph(l, *p);
    }
}

/// The label centred on the inside of the key (the columns after its line, the rows under it).
inline void Centre(Label& l, const KeyDef& k) {
    const int left = 1 + (k.w - 1 - static_cast<int>(l.width)) / 2;
    for (u32 i = 0; i < l.n; ++i) {
        const int h = l.marks[i].icon ? l.marks[i].icon->h : 16;
        l.marks[i].x = static_cast<u8>(left + l.marks[i].x);
        l.marks[i].y = static_cast<u8>(1 + (k.h - 1 - h) / 2);
    }
}

/// `n` rows of one colour from y down, at x (runs of 32: what ColumnBoth takes).
inline void SolidColumn(const gfx::Surface& s, int x, int y, int n, u32 raw) {
    const gfx::Ink ink = {raw, raw, raw};
    for (int top = y; top < y + n; top += 32) {
        const int m = y + n - top < 32 ? y + n - top : 32;
        gfx::ColumnBoth(s, static_cast<u32>(x), static_cast<u32>(top), static_cast<u32>(m), 0, 0, ink);
    }
}

/// The bits of the rows [top, top + n) (bit 0 the bottom one) that `bits` (a mark's column, bit 0 its bottom row) covers when the mark's
/// top row is `mark_top` and it is `mark_h` rows tall.
inline u32 Place(u32 bits, int mark_top, int mark_h, int top, int n) {
    const int shift = top + n - (mark_top + mark_h);
    const u64 wide = shift >= 0 ? (shift >= 32 ? 0 : static_cast<u64>(bits) << shift) : (-shift >= 32 ? 0 : static_cast<u64>(bits) >> -shift);
    return static_cast<u32>(wide & ((u64{1} << n) - 1));
}

/// One key: its line at the left and at the top, then every column of its inside as runs of the edge (or outline), the label and the fill.
inline void PaintKey(const gfx::Surface& s, const KeyDef& k, const KeyLook& look, const Label& label) {
    const u32 line = gfx::Raw(kLineDark, s.format);
    const gfx::Ink ink = {gfx::Raw(look.edge, s.format), gfx::Raw(look.label, s.format), gfx::Raw(look.fill, s.format)};
    SolidColumn(s, k.x, k.y, k.h, line);
    const int in_top = k.y + 1, in_bottom = k.y + k.h - 1; // the inside's first and last row
    for (int c = 1; c < k.w; ++c) {
        const int x = k.x + c;
        SolidColumn(s, x, k.y, 1, line);
        u32 glyph[kMaxMarks];
        for (u32 i = 0; i < label.n; ++i) {
            const Mark& m = label.marks[i];
            const int w = m.icon ? m.icon->w : 8;
            glyph[i] = c >= m.x && c < m.x + w ? (m.icon ? IconColumn(*m.icon, static_cast<u32>(c - m.x))
                                                         : gfx::GlyphColumn(gfx::kFont8x16, m.ch, static_cast<u32>(c - m.x)))
                                               : 0;
        }
        const bool side = look.outline && (c == 1 || c == k.w - 1);
        for (int top = in_top; top <= in_bottom; top += 32) {
            const int n = in_bottom + 1 - top < 32 ? in_bottom + 1 - top : 32;
            u32 mb = 0;
            for (u32 i = 0; i < label.n; ++i) {
                if (glyph[i] != 0) {
                    const Mark& m = label.marks[i];
                    mb |= Place(glyph[i], k.y + m.y, m.icon ? m.icon->h : 16, top, n);
                }
            }
            u32 ma = side ? (n == 32 ? 0xFFFFFFFFu : (1u << n) - 1) : 0;
            if (top == in_top) {
                ma |= 1u << (n - 1); // the top edge (the outline's top)
            }
            if (look.outline && top + n - 1 == in_bottom) {
                ma |= 1u; // the outline's bottom
            }
            gfx::ColumnBoth(s, static_cast<u32>(x), static_cast<u32>(top), static_cast<u32>(n), ma & ~mb, mb, ink);
        }
    }
}

/// How key `i` looks in view `v`.
inline KeyLook LookOf(const View& v, u8 i) {
    const KeyDef& k = kKeys[i];
    const bool off = (k.kind == kSubmit && !v.submit_on) || (k.kind == kScrollUp && !v.can_up) || (k.kind == kScrollDown && !v.can_down) ||
                     !Live(k, v.page);
    if (v.pressed == i && !off) {
        return kLookPressed;
    }
    if (v.selected == i && !off) {
        return kLookSelected;
    }
    switch (k.kind) {
    case kChar:
        return kLookKey;
    case kBackspace:
    case kEnter:
    case kSpace:
        return kLookFunction;
    case kShift:
        return v.shift ? kLookFunctionOn : kLookFunction;
    case kCaps:
        return v.page != kAbc ? kLookOff : v.caps ? kLookFunctionOn : kLookFunction;
    case kTabAbc:
        return v.page == kAbc ? kLookTabOn : kLookTab;
    case kTabSymbol:
        return v.page == kSymbol ? kLookTabOn : kLookTab;
    case kCancel:
        return kLookCancel;
    case kSubmit:
        return v.submit_on ? kLookSubmit : kLookSubmitOff;
    case kScrollUp:
        return v.can_up ? kLookScroll : kLookScrollOff;
    case kScrollDown:
        return v.can_down ? kLookScroll : kLookScrollOff;
    default:
        return kLookOff;
    }
}

/// What is written on key `i` in view `v`.
inline Label LabelOf(const View& v, u8 i) {
    const KeyDef& k = kKeys[i];
    Label l;
    switch (k.kind) {
    case kChar: {
        char c = k.ch;
        if (c >= 'a' && c <= 'z' && (v.shift != v.caps)) {
            c = static_cast<char>(c - 'a' + 'A');
        }
        AddGlyph(l, c);
        break;
    }
    case kBackspace:
        AddIcon(l, kIconBackspace);
        break;
    case kEnter:
        AddIcon(l, kIconEnter);
        break;
    case kShift:
        AddIcon(l, kIconShift);
        break;
    case kCaps:
        AddGlyph(l, 'a');
        AddIcon(l, kIconTurn, 2);
        AddGlyph(l, 'A', 2);
        break;
    case kTabAbc:
        AddText(l, "ABC");
        break;
    case kTabSymbol:
        AddText(l, "Symbol");
        break;
    case kCancel:
        AddText(l, v.cancel);
        break;
    case kSubmit:
        AddText(l, v.submit);
        break;
    case kScrollUp:
        AddIcon(l, kIconUp);
        break;
    case kScrollDown:
        AddIcon(l, kIconDown);
        break;
    default:
        break;
    }
    Centre(l, k);
    return l;
}

/// Decimal digits of `n` into `out` (no printf: the plugin's text never goes through newlib's formatter where it does not have to).
inline u32 PutNumber(char* out, u32 n) {
    char rev[10];
    u32 k = 0;
    do {
        rev[k++] = static_cast<char>('0' + n % 10);
        n /= 10;
    } while (n != 0 && k < sizeof(rev));
    for (u32 i = 0; i < k; ++i) {
        out[i] = rev[k - 1 - i];
    }
    return k;
}

inline u32 Length(const char* s) {
    u32 n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

/// The cursor is drawn on line `r` of the text area.
inline bool CursorOn(const View& v, u32 r) {
    return v.cursor_row == r && v.cursor_on;
}

/// Line `r` of the text area: its cells (opaque), the rest of the line's cells in the field's colour (`blank_rest`: the line is drawn again
/// alone; the whole area was just filled with it otherwise), the cursor if it is on it. Its 16 rows hold nothing else (the dotted line is
/// under them, the keys of the pad further down).
inline void DrawTextLine(const gfx::Surface& s, const View& v, u32 r, bool blank_rest) {
    const int end = kTextX + v.text_len[r] * 8;
    gfx::DrawText(s, kTextX, kTextY[r], v.text[r], v.text_len[r], gfx::kFont8x16, kInk, kField);
    if (blank_rest) {
        gfx::FillRect(s, end, kTextY[r], kTextX + static_cast<int>(kCols) * 8 - end, 16, kField);
    }
    if (CursorOn(v, r)) {
        gfx::FillRect(s, kTextX + v.cursor_col * 8, kTextY[r], 2, 16, kAccent);
    }
}

/// The text area: the lines on their dotted lines, the cursor, and the keys of the pad in small type under them.
inline void DrawTextArea(const gfx::Surface& s, const View& v) {
    gfx::FillRect(s, 0, 0, kTextW, kTextH, kField);
    for (u32 r = 0; r < kRows; ++r) {
        for (int x = kTextX - 2; x + 3 <= kTextW - 3; x += 4) {
            gfx::FillRect(s, x, kDotY[r], 3, 1, kDots); // (dashes of 3, gaps of 1: the system's)
        }
        DrawTextLine(s, v, r, false);
    }
    // "L Shift  R Symbols  B Delete  START Send": the keys in the text's colour, what they do dim
    const char* words[4][2] = {{"L", "Shift"}, {"R", v.page == kAbc ? "Symbols" : "ABC"}, {"B", "Delete"}, {"START", v.submit}};
    int x = kTextX;
    for (u32 i = 0; i < 4; ++i) {
        if (i == 0 && v.page != kAbc) {
            continue; // (no shift on the Symbol page)
        }
        const u32 a = Length(words[i][0]), b = Length(words[i][1]);
        gfx::DrawText(s, x, kHintY, words[i][0], a, gfx::kFont6x12, kInk, kField);
        x += static_cast<int>(a + 1) * 6;
        gfx::DrawText(s, x, kHintY, words[i][1], b, gfx::kFont6x12, kDimInk, kField);
        x += static_cast<int>(b + 2) * 6;
    }
}

/// The bar: the title on a green chip at the left (where the system has its dictionary's button), the count at the right.
inline void DrawBar(const gfx::Surface& s, const View& v) {
    gfx::FillRect(s, 0, kBarY, kScreenW, kBarH, kBarBg);
    const u32 tl = Length(v.title);
    gfx::FillRect(s, 2, kBarY + 1, static_cast<int>(tl) * 8 + 12, kBarH - 2, kGreen);
    gfx::DrawText(s, 8, kBarY + 2, v.title, tl, gfx::kFont8x16, kGreenInk, kGreen);
    char count[16];
    u32 n = PutNumber(count, v.len);
    count[n++] = '/';
    n += PutNumber(count + n, v.max_len);
    const gfx::Rgb c = v.at_max ? kOff : v.len * 10 >= v.max_len * 9u ? kWarn : kDimInk;
    gfx::DrawText(s, kScreenW - 8 - static_cast<int>(n) * 8, kBarY + 2, count, n, gfx::kFont8x16, c, kBarBg);
}

/// The whole bottom screen. Nothing on a surface gfx::Usable refuses, or one smaller than 320 x 240.
inline void DrawOsk(const gfx::Surface& s, const View& v) {
    if (!gfx::Usable(s) || s.w < kScreenW || s.h < kScreenH) {
        return;
    }
    DrawTextArea(s, v);
    DrawBar(s, v);
    // What is not a key: the sides of the frame, the closing lines of the frame and of the tabs, the gaps between the groups.
    const int fx0 = kFrameX0[v.page], fx1 = kFrameX1[v.page];
    const u32 line = gfx::Raw(kLineDark, s.format);
    gfx::FillRect(s, 0, kKeysY, fx0, kFrameBottom + 1 - kKeysY, kBg);
    gfx::FillRect(s, fx1 + 1, kKeysY, kScreenW - fx1 - 1, kFrameBottom + 1 - kKeysY, kBg);
    SolidColumn(s, fx1, kKeysY, kFrameBottom - kKeysY, line);
    gfx::FillRect(s, fx0, kFrameBottom, fx1 + 1 - fx0, 1, kLineDark);
    gfx::FillRect(s, 0, kFrameBottom + 1, kScreenW, kTabsY - kFrameBottom - 1, kBg);
    gfx::FillRect(s, 0, kTabsY, 4, kTabsH + 1, kBg);
    gfx::FillRect(s, kTabsEnd + 1, kTabsY, kScreenW - kTabsEnd - 1, kTabsH + 1, kBg);
    SolidColumn(s, kTabsEnd, kTabsY, kTabsH, line);
    gfx::FillRect(s, 4, kTabsY + kTabsH, kTabsEnd + 1 - 4, 1, kLineDark);
    gfx::FillRect(s, 0, kTabsY + kTabsH + 1, kScreenW, kFootY - kTabsY - kTabsH - 1, kBg);
    for (u32 i = 0; i < kKeyCount; ++i) {
        if (OnPage(kKeys[i], v.page)) {
            const u8 key = static_cast<u8>(i);
            PaintKey(s, kKeys[i], LookOf(v, key), LabelOf(v, key));
        }
    }
}

static_assert(sizeof(KeyLook) == 10, "a key's look is its ten bytes, no padding: compared as bytes");
inline bool SameLook(const KeyLook& a, const KeyLook& b) {
    return std::memcmp(&a, &b, sizeof(KeyLook)) == 0;
}

/// The screen that shows `was` made to show `now`, every pixel as DrawOsk(s, now) draws it (Tests/osk_test.cpp holds it byte by byte,
/// in every format, along typing, the pad, the touch, shift, caps lock, scrolling and the blink, and over views changed at random), drawing
/// again only what differs: the lines of the text whose characters or cursor changed, the bar when the count did, the keys whose look or
/// label did; another page, title, submit or cancel label is the whole screen. For the chat over the running game, on the plugin's thread at
/// the game's own priority: the whole screen is 1.5-2 million instructions (5.6-7.4 ms of an Old 3DS at 268 MHz, a game's thread woken meanwhile
/// waits), the blink (one line) 62-105 K, a character typed (a line, the bar, a key) 166-254 K, a key of the D-pad moved 35-39 K
/// (Tests/chat_draw_cost.sh holds them to budgets). The caller knows the surface still shows `was` (the chat's Image: only this draws it);
/// a framebuffer the game may have swapped does not, and gets DrawOsk.
inline void DrawOskOver(const gfx::Surface& s, const View& now, const View& was) {
    if (now.page != was.page || std::memcmp(now.title, was.title, sizeof(now.title)) != 0 ||
        std::memcmp(now.submit, was.submit, sizeof(now.submit)) != 0 || std::memcmp(now.cancel, was.cancel, sizeof(now.cancel)) != 0) {
        DrawOsk(s, now); // (the frame, the keys of the pad under the text and every key are another)
        return;
    }
    if (!gfx::Usable(s) || s.w < kScreenW || s.h < kScreenH) {
        return;
    }
    for (u32 r = 0; r < kRows; ++r) {
        const bool cursor = CursorOn(now, r);
        if (now.text_len[r] != was.text_len[r] || std::memcmp(now.text[r], was.text[r], now.text_len[r]) != 0 ||
            cursor != CursorOn(was, r) || (cursor && now.cursor_col != was.cursor_col)) {
            DrawTextLine(s, now, r, true);
        }
    }
    if (now.len != was.len || now.max_len != was.max_len || now.at_max != was.at_max) {
        DrawBar(s, now);
    }
    // (a label changes with the view only through the case of a letter, shift XOR caps lock: LabelOf; Send's is the submit label)
    const bool recase = (now.shift != now.caps) != (was.shift != was.caps);
    for (u32 i = 0; i < kKeyCount; ++i) {
        const u8 key = static_cast<u8>(i);
        if (OnPage(kKeys[i], now.page)) {
            const KeyLook look = LookOf(now, key);
            if (!SameLook(look, LookOf(was, key)) || (recase && kKeys[i].kind == kChar && kKeys[i].ch >= 'a' && kKeys[i].ch <= 'z')) {
                PaintKey(s, kKeys[i], look, LabelOf(now, key));
            }
        }
    }
}

} // namespace osk
} // namespace azl
