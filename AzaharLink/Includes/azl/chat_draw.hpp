// The chat's lines on the top screen: the kept messages (azl/chat_core.hpp) drawn as "Nick: text" blocks in the corner of chat_pos, with
// the drawing layer of azl/gfx_surface.hpp. Plain C++ and no heap; it runs on the game's thread inside the framework's OSD callback, so it
// reads only the view the ChatReader copied and the numbers it is handed, and it never waits.
//
// What a block looks like (the user's decisions, 2026-11-04; STUDIO-CHAT section 6; smaller, tighter and the whole width, 2026-11-08): an
// OPAQUE strip in the background of 3DS Link (a transparent one would have to read the framebuffer, which is uncached and costs more than a
// write, and text on a bare game is lost), a 2-pixel bar at its left in the colour of the nick, the nick in that colour and the rest in the
// text colour of the app (ConfigApp/source/gfx.hpp), in Spleen 6x12 drawn in a cell of 10 rows (gfx::kFont6x10: Tests/make_font_spleen.py
// says which 13 glyphs move a row to fit it). A message is wrapped on at most 3 lines of kCols characters, as wide as the top screen holds,
// at a space when there is one in the second half of the line and the rest still fits the lines left (else the line is cut at kCols): the
// longest nick and 128 characters always show whole; past the block (never, by the room's rules) the last line ends with "...". At most 5
// blocks (all the chat keeps), the newest at the bottom of the stack in every corner. The corners:
//   top-left      x = 6,                 from y = 8 down
//   top-right     right edge at x = 390, from y = 8 down
//   bottom-left   x = 6,                 bottom at y = 236, up
//   bottom-right  right edge at x = 390, bottom at y = 236 with no notification of the framework on screen; with n of them, above them:
//                 230 - 15 n - 3, rounded down to a multiple of 4. The framework draws its notifications right-aligned at x = 390 from
//                 y = 230 - 15 n down (OSDImpl::Draw), each for 5 s: the chat and the notifications make one column. NoticeClock counts
//                 them (the plugin calls Note next to every OSD::Notify).
// The plugin's own notifications are drawn the same way (a block with no nick, its bar in kNoticeBar), in the corner OPPOSITE the chat
// (Opposite: bottom-left <-> top-right, top-left <-> bottom-right), so that they never cover it (azl/chat_host.hpp, NoticeBoard); the
// chat's stack stops where theirs begins. Blocks that do not fit above the top of the screen (or before the other stack) are not drawn, the
// oldest first. Every edge of a block is on a row that is a multiple of 4 and every block is 12, 24 or 32 rows: a column of a block starts
// on a word in all five formats, so the whole strip goes out as words (Tests/chat_draw_test.cpp counts them), and it is at most 32 rows,
// one run of gfx::RunColumnBoth (its masks are 32 bits).
#pragma once

#include <atomic>

#include "azl/chat_core.hpp"
#include "azl/gfx_surface.hpp"

namespace azl {
namespace chat {

/// The corners, in the order and with the numbers of the ini's chat_pos (ini::kChatPos*; Tests/chat_draw_test.cpp holds them equal).
enum Corner : int { kTopLeft = 0, kTopRight = 1, kBottomLeft = 2, kBottomRight = 3 };

/// The corner across the screen from `corner` (the plugin's notifications go there: never over the chat).
constexpr int Opposite(int corner) {
    return 3 - corner; // (top-left <-> bottom-right, top-right <-> bottom-left)
}
static_assert(Opposite(kBottomLeft) == kTopRight && Opposite(kTopLeft) == kBottomRight && Opposite(kBottomRight) == kTopLeft &&
                  Opposite(kTopRight) == kBottomLeft,
              "the notifications go to the corner opposite the chat");

constexpr u32 kShowMs = 10000;   ///< the keyboard closed: the lines stay this long after the last message came
constexpr u32 kCols = 62;        ///< characters on a line of a block ("Nick: " included): the whole width of the top screen
constexpr u32 kBlockLines = 3;   ///< lines of a block at most (the longest nick and 128 characters: 150 of the 186 cells)
constexpr int kGlyphW = 6, kLineH = 10; ///< Spleen 6x12 in a cell of 10 rows (gfx::kFont6x10)
constexpr int kBarW = 2;         ///< the bar in the nick's colour
constexpr int kPadL = 4, kPadR = 4; ///< background between the bar and the text, and after the text
constexpr int kGapY = 4;         ///< rows of the game between two blocks
constexpr int kLeftX = 6;        ///< the left edge of the left corners
constexpr int kRightEdge = 390;  ///< the right edge of the right corners (the framework's XEND of its notifications)
constexpr int kTopY = 8;         ///< the first row of the top corners
constexpr int kBottomY = 236;    ///< the row under the bottom corners
constexpr int kNoLimit = -1;     ///< DrawChatLines: no other stack in the way, the screen's own edge
constexpr u32 kNoticeMs = 5300;  ///< a notification of the framework stays 5 s (and a frame or two before its clock starts)
constexpr u32 kNoticeSlots = 8;  ///< notifications counted at most
constexpr u32 kNoticeShowMs = 5000; ///< one of the plugin's own notifications stays this long (the framework's own time)

/// The colours of 3DS Link (ConfigApp/source/gfx.hpp: kBg, kText; the nicks: kAccent, kOn, kWarn, kAlert and a violet).
constexpr gfx::Rgb kBg = {13, 14, 17};
constexpr gfx::Rgb kText = {232, 234, 238};
constexpr u32 kNickColours = 5;
constexpr gfx::Rgb kNick[kNickColours] = {{70, 178, 255}, {74, 218, 128}, {255, 198, 88}, {255, 140, 56}, {186, 140, 255}};
constexpr gfx::Rgb kNoticeBar = {255, 255, 255}; ///< the bar of a notification of the plugin (a line with no nick): no nick's colour

/// Rows of background above and below the text of a block of `lines` lines: 1, and 2 for two lines (10 x 2 + 2 + 2 = 24), so that every
/// block is a whole number of 4 rows: 12, 24, 32.
constexpr int PadY(u32 lines) {
    return lines == 2 ? 2 : 1;
}

/// How a message sits on the lines of its block: line l shows the characters start[l] .. start[l] + len[l] - 1 of "Nick: text", and the
/// last line ends with "..." (its last 3 cells) when `dots`.
struct Wrap {
    u8 start[kBlockLines];
    u8 len[kBlockLines];
    u8 lines;
    u8 dots;
};

/// The characters of "Nick: text" without building it (it is up to 150 characters: no buffer of that size on a game thread). A line with
/// no nick (a notification of the plugin) is its text alone.
inline u32 BlockChars(const Line& l) {
    return l.nick_len == 0 ? l.text_len : static_cast<u32>(l.nick_len) + 2 + l.text_len;
}
inline char BlockChar(const Line& l, u32 i) {
    if (l.nick_len == 0) {
        return l.text[i];
    }
    return i < l.nick_len ? l.nick[i] : i == l.nick_len ? ':' : i == l.nick_len + 1u ? ' ' : l.text[i - l.nick_len - 2];
}

constexpr u32 kAheadMs = 1000; ///< a time stamped by another thread may be this much ahead of the reader's clock (see Recent)

/// "`t_ms` is less than `span_ms` before `now_ms`" on a clock that wraps. A time a little AHEAD of now (up to kAheadMs) is recent too: the
/// network thread stamps a line, and the plugin's thread a notification, with their own reading of the clock, which may come after the one
/// the game's thread took for its frame; read as an age, that time would be 2^32 - 1 ms old and the chat would vanish for a frame.
inline bool Recent(u32 now_ms, u32 t_ms, u32 span_ms) {
    const u32 age = now_ms - t_ms;
    return age < span_ms || age >= 0u - kAheadMs;
}

/// "The newest kept line is younger than kShowMs" (or the keyboard is open): how many of the view's lines to draw. A view that says more
/// than kLines (it never should) counts as kLines, as in DrawChatLines.
inline u32 ShownLines(const View& v, u32 now_ms, bool keyboard_open) {
    const u32 count = v.count < kLines ? v.count : kLines;
    if (count == 0) {
        return 0;
    }
    if (keyboard_open) {
        return count;
    }
    return Recent(now_ms, v.lines[count - 1].t_ms, kShowMs) ? count : 0;
}

/// The plugin's own notifications to draw: the newest ones younger than kNoticeShowMs (they come in the order of their times).
inline u32 ShownNotices(const View& v, u32 now_ms) {
    const u32 count = v.count < kLines ? v.count : kLines;
    u32 n = 0;
    while (n < count && Recent(now_ms, v.lines[count - 1 - n].t_ms, kNoticeShowMs)) {
        ++n;
    }
    return n;
}

/// The framework's notifications on screen (OSD::Notify), for the bottom-right corner. Note() is called by the plugin's thread next to each
/// OSD::Notify; the game's thread asks Young() every frame. Relaxed atomics, a ring of kNoticeSlots times: a time torn by a race is one
/// wrong frame.
class NoticeClock {
public:
    void Note(u32 now_ms) {
        const u32 at = next_.load(std::memory_order_relaxed);
        times_[at % kNoticeSlots].store(now_ms | 1u, std::memory_order_relaxed); // (| 1: a slot never noted is 0)
        next_.store(at + 1, std::memory_order_relaxed);
    }
    // >>> game-thread code (Tests/policy_check.sh)
    u32 Young(u32 now_ms) const {
        u32 n = 0;
        for (u32 i = 0; i < kNoticeSlots; ++i) {
            const u32 t = times_[i].load(std::memory_order_relaxed);
            if (t != 0 && Recent(now_ms | 1u, t, kNoticeMs)) { // (both odd: a Note of this very millisecond is 0 old, not 2^32 - 1)
                ++n;
            }
        }
        return n;
    }

private:
    std::atomic<u32> times_[kNoticeSlots] = {};
    std::atomic<u32> next_{0};
};

/// The colour of a nick: FNV-1a of its characters, one of kNickColours (the same nick is the same colour on every console).
inline u32 NickColour(const char* nick, u32 len) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < len; ++i) {
        h = (h ^ static_cast<u8>(nick[i])) * 16777619u;
    }
    return h % kNickColours;
}

/// Where "Nick: text" breaks (see Wrap). A line breaks at the last space of its second half (the character just past the line included: a
/// line that ends where a word does) when what follows the space still fits the lines left; otherwise it is cut at kCols (a word that long
/// has no better place, and a message that fits the block shows whole: 3 lines broken at spaces alone could hold as little as 93 cells).
inline Wrap WrapLine(const Line& l) {
    Wrap w = {};
    const u32 total = BlockChars(l);
    u32 at = 0;
    for (u32 line = 0; line < kBlockLines; ++line) {
        w.lines = static_cast<u8>(line + 1);
        w.start[line] = static_cast<u8>(at);
        const u32 rest = total - at;
        if (rest <= kCols) {
            w.len[line] = static_cast<u8>(rest);
            return w;
        }
        if (line + 1 == kBlockLines) {
            // too long for the block: the last line ends with "...", after its last character that is not a space
            u32 keep = kCols - 3;
            while (keep > 0 && BlockChar(l, at + keep - 1) == ' ') {
                --keep;
            }
            w.len[line] = static_cast<u8>(keep + 3);
            w.dots = 1;
            return w;
        }
        const u32 room = kCols * (kBlockLines - 1 - line); // (the cells of the lines after this one)
        u32 len = kCols;
        u32 next = at + kCols;
        for (u32 i = kCols; i >= kCols / 2; --i) {
            if (BlockChar(l, at + i) == ' ') {
                if (total - (at + i + 1) <= room) {
                    len = i;
                    next = at + i + 1;
                }
                break;
            }
        }
        w.len[line] = static_cast<u8>(len);
        at = next;
        if (at >= total) {
            return w; // (nothing after the break: the cleaning leaves no space at the end, so this is only a guard)
        }
    }
    return w;
}

/// The widest line of a wrap, in characters.
inline u32 WrapCols(const Wrap& w) {
    u32 cols = 0;
    for (u32 line = 0; line < kBlockLines; ++line) {
        cols = w.len[line] > cols ? w.len[line] : cols;
    }
    return cols;
}

/// The rows of a block of `lines` lines.
constexpr int BlockHeight(u32 lines) {
    return PadY(lines) * 2 + kLineH * static_cast<int>(lines);
}
/// The columns of a block whose longest line has `cols` characters.
constexpr int BlockWidth(u32 cols) {
    return kBarW + kPadL + kGlyphW * static_cast<int>(cols) + kPadR;
}
/// The row under the bottom-right stack with `notices` notifications of the framework on screen.
constexpr int BottomRightY(u32 notices) {
    return notices == 0 ? kBottomY : ((230 - 15 * static_cast<int>(notices) - 3) & ~3);
}

// The geometry, held where it is made: the strips go out as words (every height and every edge a multiple of 4 rows), a column of a block
// is one run of RunColumnBoth (32 rows at most: its masks are a u32), the widest block is inside the top screen in every corner (no column
// past 399: DrawBlock writes where it is told, on the game's own framebuffer), and the longest message fits.
static_assert(BlockHeight(1) % 4 == 0 && BlockHeight(2) % 4 == 0 && BlockHeight(3) % 4 == 0, "every block is a whole number of 4 rows");
static_assert(kGapY % 4 == 0 && kTopY % 4 == 0 && kBottomY % 4 == 0, "every edge of a block is on a row that is a multiple of 4");
static_assert(BlockHeight(kBlockLines) <= 32, "a column of a block is one run of RunColumnBoth (32-bit masks)");
static_assert(kLeftX + BlockWidth(kCols) <= 400, "a block in a left corner ends inside the top screen");
static_assert(kRightEdge <= 400 && kRightEdge - BlockWidth(kCols) >= kLeftX, "a block in a right corner starts inside the top screen");
static_assert(kNickMax + 2 + kTextMax <= kCols * kBlockLines, "the longest nick and the longest message fit a block whole");
static_assert(kCols * kBlockLines <= 255, "the start of a line is a byte");

/// One column of a block at `left` (and `right`), then the pointers moved on to the next column, `stride` bytes on.
inline void NextColumn(u8*& left, u8*& right, u32 stride, u32 h, u32 ma, u32 mb, const gfx::Ink& ink, u32 bpp) {
    gfx::RunColumnBoth(left, right, h, ma, mb, ink, bpp);
    left += stride;
    if (right != nullptr) {
        right += stride;
    }
}

/// One block, its top-left pixel at (x, y), the colours already in the surface's format. The caller keeps it on the screen (DrawChatLines
/// checks it). The columns are written one after the other, `stride` bytes apart on both images (no address worked out again for each); a
/// cell's glyphs are looked up once for its 6 columns.
inline void DrawBlock(const gfx::Surface& s, int x, int y, const Line& l, const Wrap& w, u32 nick_raw, u32 text_raw, u32 bg_raw) {
    static_assert(kBlockLines == 3, "a block's columns are made from its three lines");
    const u32 h = static_cast<u32>(BlockHeight(w.lines));
    const u32 cols = WrapCols(w);
    const u32 all = h >= 32 ? 0xFFFFFFFFu : (1u << h) - 1; // (a shift by 32 is undefined: the block of 3 lines is 32 rows)
    const u32 pad = static_cast<u32>(PadY(w.lines));
    const gfx::Ink ink = {nick_raw, text_raw, bg_raw};
    u8* left = s.left + gfx::PixelOffset(s, static_cast<u32>(x), static_cast<u32>(y) + h - 1);
    u8* right = s.right != nullptr ? s.right + (left - s.left) : nullptr;
    const u32 bpp = s.bpp, stride = s.stride;
    // the bar and the padding after it
    for (u32 c = 0; c < static_cast<u32>(kBarW + kPadL); ++c) {
        NextColumn(left, right, stride, h, c < static_cast<u32>(kBarW) ? all : 0, 0, ink, bpp);
    }
    for (u32 cell = 0; cell < cols; ++cell) {
        // The cell's glyph on each line: its columns (the space's, which are empty, where the line has no character), where its bits go in
        // the column (the cell of line `line` has its bottom row pad + kLineH * (lines - 1 - line) rows above the block's bottom row: that
        // bit) and which mask takes them (all ones: the nick's, `a`).
        const u16* glyph[kBlockLines];
        u32 shift[kBlockLines], nick[kBlockLines];
        for (u32 line = 0; line < kBlockLines; ++line) {
            const bool here = line < w.lines && cell < w.len[line];
            const u32 at = w.start[line] + cell;
            const char ch = !here ? ' ' : w.dots && line + 1 == w.lines && cell + 3 >= w.len[line] ? '.' : BlockChar(l, at);
            glyph[line] = &gfx::kFont6x10.cols[gfx::GlyphIndex(ch) * kGlyphW];
            shift[line] = here ? pad + static_cast<u32>(kLineH) * (w.lines - 1 - line) : 0;
            nick[line] = here && at < l.nick_len ? ~0u : 0;
        }
        for (u32 g = 0; g < static_cast<u32>(kGlyphW); ++g) {
            const u32 b0 = static_cast<u32>(glyph[0][g]) << shift[0], b1 = static_cast<u32>(glyph[1][g]) << shift[1],
                      b2 = static_cast<u32>(glyph[2][g]) << shift[2];
            NextColumn(left, right, stride, h, (b0 & nick[0]) | (b1 & nick[1]) | (b2 & nick[2]),
                       (b0 & ~nick[0]) | (b1 & ~nick[1]) | (b2 & ~nick[2]), ink, bpp);
        }
    }
    // the padding at the end
    for (u32 c = 0; c < static_cast<u32>(kPadR); ++c) {
        NextColumn(left, right, stride, h, 0, 0, ink, bpp);
    }
}

/// A surface the chat draws on: one the layer may write, as large as the top screen.
inline bool TopUsable(const gfx::Surface& s) {
    return gfx::Usable(s) && s.w >= 400 && s.h >= 240;
}

/// Draws the last `shown` lines of `v` (ShownLines, or ShownNotices for the plugin's notifications) in `corner`; `notices` =
/// NoticeClock::Young (it moves the bottom-right corner only). `limit`: the row the stack may not pass, the edge of the stack in the
/// opposite corner (in a top corner no block reaches below it, in a bottom one none above it); kNoLimit, the screen's own edge. `edge`,
/// when given: the row past the stack and its gap (under it in a top corner, above it in a bottom one; the first row when nothing was
/// drawn), the `limit` of the other stack. Returns how many blocks were drawn. Nothing on a surface TopUsable refuses.
inline u32 DrawChatLines(const gfx::Surface& s, const View& v, u32 shown, int corner, u32 notices, int limit = kNoLimit,
                         int* edge = nullptr) {
    if (!TopUsable(s)) {
        return 0;
    }
    const u32 count = v.count < kLines ? v.count : kLines;
    if (shown > count) {
        shown = count;
    }
    const bool right = corner == kTopRight || corner == kBottomRight;
    const bool top = corner == kTopLeft || corner == kTopRight;
    // Top corners: from the oldest down. Bottom corners: from the newest up; what reaches above the screen's top (or the limit) is not
    // drawn (older ones neither).
    int y = top ? kTopY : (corner == kBottomRight ? BottomRightY(notices) : kBottomY);
    const int below = limit == kNoLimit || limit > s.h ? s.h : limit; // (a top stack ends above this row)
    const int above = limit == kNoLimit || limit < 0 ? 0 : limit;     // (a bottom stack begins at this row or under it)
    u32 drawn = 0;
    if (shown != 0) {
        const u32 text_raw = gfx::Raw(kText, s.format), bg_raw = gfx::Raw(kBg, s.format);
        const u32 first = count - shown; // the oldest line drawn
        for (u32 k = 0; k < shown; ++k) {
            const Line& l = v.lines[top ? first + k : count - 1 - k];
            const Wrap w = WrapLine(l);
            const int h = BlockHeight(w.lines);
            const int width = BlockWidth(WrapCols(w));
            int y0;
            if (top) {
                y0 = y;
                if (y0 + h > below) {
                    break;
                }
            } else {
                y0 = y - h;
                if (y0 < above) {
                    break;
                }
            }
            const int x0 = right ? kRightEdge - width : kLeftX;
            if (x0 < 0 || x0 + width > s.w) {
                break; // (never, by the static_asserts above: a column past the screen would be written into the game's memory)
            }
            y = top ? y0 + h + kGapY : y0 - kGapY;
            const gfx::Rgb bar = l.nick_len == 0 ? kNoticeBar : kNick[NickColour(l.nick, l.nick_len)];
            DrawBlock(s, x0, y0, l, w, gfx::Raw(bar, s.format), text_raw, bg_raw);
            ++drawn;
        }
    }
    if (edge != nullptr) {
        *edge = y;
    }
    return drawn;
}
// <<< game-thread code

} // namespace chat
} // namespace azl
