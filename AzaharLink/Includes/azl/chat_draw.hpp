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
constexpr int kGapY = 0;         ///< rows of the game between two blocks: none (the user's decision, 2026-11-11: the messages closer, taking less of the screen); a multiple of 4 or the words would not be aligned
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
/// The colour of each PLAYER of a match, by the node it has (the user's decision, 2026-11-11): 1 light blue (the host), 2 green, 3 yellow, 4 orange, 5 red, 6 purple, 7 white,
/// 8 fuchsia. A player takes it as he comes in, whatever his name: the bar and the name of his messages in the chat (Line::pad = his node, 0 = not known: the colour
/// of the nick's hash, kNick) and of his latency line, and the bar under his tile, are all that colour.
constexpr u32 kNodeColours = 8;
constexpr gfx::Rgb kNodeColour[kNodeColours] = {{70, 178, 255}, {74, 218, 128}, {255, 198, 88}, {255, 140, 56}, {255, 72, 72}, {186, 140, 255}, {255, 255, 255}, {255, 90, 210}};
constexpr u32 kNickColours = 5;
constexpr gfx::Rgb kNick[kNickColours] = {{70, 178, 255}, {74, 218, 128}, {255, 198, 88}, {255, 140, 56}, {186, 140, 255}};
constexpr gfx::Rgb kNoticeBar = {255, 255, 255}; ///< the bar of a notification of the plugin (a line with no nick): no nick's colour
/// Who writes while this console is in a match without being one of its players (Line::pad = kPadOutside): grey, the latency scale's "no value" - never the
/// colour of a node (the nicks' hash colours are those of nodes 1, 2, 3, 4 and 6: he would look like a player of the match).
constexpr gfx::Rgb kOutside = {150, 156, 168};
/// The bar and the name of a message: the player's node, else grey for one outside the match, else (no match) the hash of the nick.
constexpr gfx::Rgb NickRgb(u8 pad, u32 hash_colour) {
    return pad >= 1 && pad <= kNodeColours ? kNodeColour[pad - 1] : pad == kPadOutside ? kOutside : kNick[hash_colour];
}

/// The latency scale (the colours of the AzaharPing plugin): blue up to 30 ms, green 31-60, yellow 61-100, orange 101-150, red over 150, and a
/// grey for a player with no value yet. LatencyColour() gives the index.
constexpr u32 kLatColours = 6;
constexpr gfx::Rgb kLatColour[kLatColours] = {{70, 178, 255}, {74, 218, 128}, {255, 198, 88}, {255, 140, 56}, {255, 72, 72}, {150, 156, 168}};
constexpr u8 LatencyColour(int rtt_ms) {
    return rtt_ms < 0 ? 5 : rtt_ms <= 30 ? 0 : rtt_ms <= 60 ? 1 : rtt_ms <= 100 ? 2 : rtt_ms <= 150 ? 3 : 4;
}

/// More than four players in the lobby (more than 3 others: Tetris-like games take up to 8): the names do not fit, so the board is ONE line
/// (pad = kLatTiles) whose text is two characters a player - the node's digit (H for the host, node 1) and the latency colour's index ('0'..'5') - and the drawing
/// shows rows of small tiles side by side (3, 2, 2: TilesInRow): the tile in the colour of the latency - with an H in it for the host, and nothing in the others - and a small bar under it in the
/// colour of the node (kNodeColour: the colour the player's messages have in the chat), to tell who is who. The log (latency.log) has every player's name and value always.
constexpr u8 kLatTiles = 0xFF;
constexpr int kTileW = 8;    ///< columns of a tile: side by side, with no black between them (the user's decision, 2026-11-11)
constexpr int kTileH = 12;   ///< rows of a tile (a block's height)
constexpr int kTileBarH = 4; ///< rows under a tile: the player's bar of 2 rows (directly under the tile) and 2 rows of black between this row of tiles and the next
constexpr int kTileRowH = kTileH + kTileBarH; ///< rows of one row of tiles (a multiple of 4: 16)
constexpr int kTileRows = 3;                   ///< rows of tiles at most
/// The tiles of a row of the strip: 3, then 2, then 2 (for 7 players; 4 players: 3 and 1, 5: 3 and 2, 6: 3, 2 and 1), so that the strip is as narrow as the names are.
constexpr u32 TilesInRow(u32 n, u32 row) {
    return row == 0 ? (n < 3 ? n : 3) : row == 1 ? (n > 3 ? (n - 3 < 2 ? n - 3 : 2) : 0) : (n > 5 ? (n - 5 < 2 ? n - 5 : 2) : 0);
}
constexpr u32 TileRowsFor(u32 n) {
    return TilesInRow(n, 2) != 0 ? 3 : TilesInRow(n, 1) != 0 ? 2 : n != 0 ? 1 : 0;
}
/// The columns of a row of `tiles` tiles: the tiles and nothing else (no bar, no padding: the user's decision, 2026-11-11).
constexpr int TilesWidth(u32 tiles) {
    return static_cast<int>(tiles) * kTileW;
}
static_assert(kTileRowH % 4 == 0 && kTileH % 4 == 0 && kTileBarH == 4 && kTileRowH <= 32, "a row of tiles is two runs of whole words: 12 rows of tiles, 4 of bars");
static_assert(TilesInRow(7, 0) == 3 && TilesInRow(7, 1) == 2 && TilesInRow(7, 2) == 2 && TilesInRow(4, 1) == 1 && TilesInRow(6, 2) == 1 && TileRowsFor(4) == 2 && TileRowsFor(7) == 3,
              "7 tiles are 3, 2, 2");
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
    return l.nick_len == 0 ? l.text_len : static_cast<u32>(l.nick_len) + (l.sep != '\0' ? 2 : 1) + l.text_len; // ("Nick: text"; a latency line has no separator: "Nick:64ms")
}
inline char BlockChar(const Line& l, u32 i) {
    if (l.nick_len == 0) {
        return l.text[i];
    }
    if (l.sep == '\0') {
        return i < l.nick_len ? l.nick[i] : i == l.nick_len ? ':' : l.text[i - l.nick_len - 1];
    }
    return i < l.nick_len ? l.nick[i] : i == l.nick_len ? ':' : i == l.nick_len + 1u ? l.sep : l.text[i - l.nick_len - 2];
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

/// How far the shadow of a letter reaches (pixels, to the right and down): 2 (the user's decision, 2026-11-11; 1 is thinner, -DAZL_SHADOW_PX=1).
#ifndef AZL_SHADOW_PX
#define AZL_SHADOW_PX 2
#endif
constexpr u32 kShadowPx = AZL_SHADOW_PX;
static_assert(kShadowPx >= 1 && kShadowPx <= 2 && kShadowPx <= static_cast<u32>(kPadR), "the shadow of the last letter falls in the block's right padding");

/// One column of a block at `left` (and `right`), then the pointers moved on to the next column, `stride` bytes on. Opaque by default (the background's colour wherever the masks have no bit).
/// With `shadow_prev`: NO background - only the pixels of the masks are written, and a black shadow (`ink.bg`) on the pixel to the right, the one below and the one right-and-below of every pixel of the previous column's
/// masks, `*shadow_prev` (this column's masks go there for the next one). With `fg_only`: no background and no shadow.
inline void NextColumn(u8*& left, u8*& right, u32 stride, u32 h, u32 ma, u32 mb, const gfx::Ink& ink, u32 bpp, u32* shadow_prev = nullptr, bool fg_only = false) {
    if (shadow_prev != nullptr) {
        // The shadow: every pixel within kShadowPx to the right and down of a pixel of a letter (a column of the masks is bit i = the row i above the bottom, so "down" is a shift right):
        // under a pixel of this column, and to the right of one of the previous column(s), with the rows under it.
        const u32 fg = ma | mb, p1 = shadow_prev[0], p2 = shadow_prev[1];
        u32 ms = fg >> 1 | p1 | p1 >> 1;
        if (kShadowPx >= 2) {
            ms |= fg >> 2 | p1 >> 2 | p2 | p2 >> 1 | p2 >> 2;
        }
        gfx::RunColumnFgOnly(left, right, h, ma, mb, ms, ink, bpp);
        shadow_prev[1] = p1;
        shadow_prev[0] = fg;
    } else if (fg_only) {
        gfx::RunColumnFgOnly(left, right, h, ma, mb, 0, ink, bpp);
    } else {
        gfx::RunColumnBoth(left, right, h, ma, mb, ink, bpp);
    }
    left += stride;
    if (right != nullptr) {
        right += stride;
    }
}
/// One block, its top-left pixel at (x, y), the colours already in the surface's format. The caller keeps it on the screen (DrawChatLines
/// checks it). The columns are written one after the other, `stride` bytes apart on both images (no address worked out again for each); a
/// cell's glyphs are looked up once for its 6 columns.
inline void DrawBlock(const gfx::Surface& s, int x, int y, const Line& l, const Wrap& w, u32 nick_raw, u32 text_raw, u32 bg_raw,
                      u32 nick_extra = 0, bool shadow = false) {
    static_assert(kBlockLines == 3, "a block's columns are made from its three lines");
    const u32 h = static_cast<u32>(BlockHeight(w.lines));
    const u32 cols = WrapCols(w);
    const u32 all = h >= 32 ? 0xFFFFFFFFu : (1u << h) - 1; // (a shift by 32 is undefined: the block of 3 lines is 32 rows)
    const u32 pad = static_cast<u32>(PadY(w.lines));
    const gfx::Ink ink = {nick_raw, text_raw, bg_raw};
    // The characters drawn in the first colour (`nick_raw`, like the bar): the nick, as always, and with `nick_extra` (the latency lines: 2, the ": ") that many
    // more; the rest of the line is in `text_raw` - there the value, in the colour of the scale.
    const u32 coloured_to = l.nick_len + nick_extra;
    u8* left = s.left + gfx::PixelOffset(s, static_cast<u32>(x), static_cast<u32>(y) + h - 1);
    u8* right = s.right != nullptr ? s.right + (left - s.left) : nullptr;
    const u32 bpp = s.bpp, stride = s.stride;
    // With `shadow` the block has no background: the game shows through everywhere, the bar is written in full and the glyphs get a black shadow (one pixel right and down).
    u32 prev[2] = {0, 0}; // (the masks of the last two columns: what casts a shadow on the next)
    u32* const sp = shadow ? prev : nullptr;
    // the bar and the padding after it
    for (u32 c = 0; c < static_cast<u32>(kBarW + kPadL); ++c) {
        const bool bar = c < static_cast<u32>(kBarW);
        NextColumn(left, right, stride, h, bar ? all : 0, 0, ink, bpp, bar ? nullptr : sp, shadow && !bar); // (the bar: opaque, no shadow)
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
            nick[line] = here && at < coloured_to ? ~0u : 0;
        }
        for (u32 g = 0; g < static_cast<u32>(kGlyphW); ++g) {
            const u32 b0 = static_cast<u32>(glyph[0][g]) << shift[0], b1 = static_cast<u32>(glyph[1][g]) << shift[1],
                      b2 = static_cast<u32>(glyph[2][g]) << shift[2];
            NextColumn(left, right, stride, h, (b0 & nick[0]) | (b1 & nick[1]) | (b2 & nick[2]),
                       (b0 & ~nick[0]) | (b1 & ~nick[1]) | (b2 & ~nick[2]), ink, bpp, sp);
        }
    }
    // the padding at the end
    for (u32 c = 0; c < static_cast<u32>(kPadR); ++c) {
        NextColumn(left, right, stride, h, 0, 0, ink, bpp, sp, shadow && prev[0] == 0 && prev[1] == 0); // (the first columns after the glyphs get their shadow)
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
                         int* edge = nullptr, bool translucent = false) {
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
    // drawn (older ones neither). A top stack that does not fit drops its OLDEST lines, as a bottom one does (it kept the oldest and lost the newest message).
    int y = top ? kTopY : (corner == kBottomRight ? BottomRightY(notices) : kBottomY);
    const int below = limit == kNoLimit || limit > s.h ? s.h : limit; // (a top stack ends above this row)
    const int above = limit == kNoLimit || limit < 0 ? 0 : limit;     // (a bottom stack begins at this row or under it)
    u32 drawn = 0;
    if (shown != 0) {
        const u32 text_raw = gfx::Raw(kText, s.format), bg_raw = gfx::Raw(kBg, s.format);
        u32 first = count - shown; // the oldest line drawn
        if (top) {
            // (the newest lines that fit, counted from the newest back: the older ones that would reach the limit are not drawn)
            int room = below - y;
            u32 fit = 0;
            while (fit < shown) {
                const int need = BlockHeight(WrapLine(v.lines[count - 1 - fit]).lines) + (fit != 0 ? kGapY : 0);
                if (need > room) {
                    break;
                }
                room -= need;
                ++fit;
            }
            first = count - fit;
            shown = fit;
        }
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
            const gfx::Rgb bar = l.nick_len == 0 ? kNoticeBar : NickRgb(l.pad, NickColour(l.nick, l.nick_len));
            DrawBlock(s, x0, y0, l, w, gfx::Raw(bar, s.format), text_raw, bg_raw, 0, translucent);
            ++drawn;
        }
    }
    if (edge != nullptr) {
        *edge = y;
    }
    return drawn;
}

/// One row of the strip of tiles of a board with many players (kLatTiles): the tiles `first` .. `first + count - 1` of `l`, side by side with no black between
/// them and no bar or padding at the sides: each of the colour of its player's latency (kTileW columns by kTileH rows) with an H in black on the host's tile and no letter on the others, and directly under it a small
/// bar of 2 rows in the colour of the node (kNodeColour: the colour of the player's messages in the chat), which tells who is who; then 2 rows of black. `l.text` is
/// "<digit or H><latency colour>" a player. Two runs a column - the tiles' 12 rows and the bars' 4 - each a whole number of words.
inline void DrawTiles(const gfx::Surface& s, int x, int y, const Line& l, u32 first, u32 count, u32 bg_raw, bool shadow = false) {
    const u32 h = static_cast<u32>(kTileH), hb = static_cast<u32>(kTileBarH);
    const u32 all = (1u << h) - 1;
    const u32 pad = static_cast<u32>(PadY(1));
    u8* left = s.left + gfx::PixelOffset(s, static_cast<u32>(x), static_cast<u32>(y) + h - 1);
    u8* right = s.right != nullptr ? s.right + (left - s.left) : nullptr;
    u8* lbar = s.left + gfx::PixelOffset(s, static_cast<u32>(x), static_cast<u32>(y) + h + hb - 1);
    u8* rbar = s.right != nullptr ? s.right + (lbar - s.left) : nullptr;
    const u32 bpp = s.bpp, stride = s.stride;
    for (u32 t = first; t < first + count; ++t) {
        const u32 lat_i = static_cast<u32>(static_cast<u8>(l.text[2 * t + 1]) - '0');
        const u32 colour = lat_i < kLatColours ? lat_i : kLatColours - 1u;
        const char digit_ch = l.text[2 * t];
        const u32 node = digit_ch == 'H' ? 1u : static_cast<u32>(static_cast<u8>(digit_ch) - '0');
        const u32 node_i = node >= 1 && node <= kNodeColours ? node - 1 : 0u;
        const u16* glyph = &gfx::kFont6x10.cols[gfx::GlyphIndex(digit_ch) * kGlyphW];
        const gfx::Ink ink = {gfx::Raw(kLatColour[colour], s.format), bg_raw, bg_raw};                     // (a: the tile, b: the H, in the strip's black)
        const gfx::Ink name = {gfx::Raw(kNodeColour[node_i], s.format), bg_raw, bg_raw};                    // (a: the bar under it)
        for (u32 c = 0; c < static_cast<u32>(kTileW); ++c) {
            // (only the host's tile has a letter, the H; the others are plain: who they are is told by the colour of the bar under them)
            const u32 letter = digit_ch == 'H' && c >= 1 && c <= static_cast<u32>(kGlyphW) ? static_cast<u32>(glyph[c - 1]) << pad : 0;
            NextColumn(left, right, stride, h, all & ~letter, letter, ink, bpp);
            NextColumn(lbar, rbar, stride, hb, 0xC, 0, name, bpp, nullptr, shadow); // (rows 2 and 3 of the run: directly under the tile; the two rows under them are black, or the game's when `shadow`)
        }
    }
}

/// The latency of the other players, drawn while the keyboard of the chat is open (the user's request, 2026-11-11): one block of one line per
/// player, "Name: 64 ms", like a message of the chat: the bar and the name in the colour the chat gives that nick, the VALUE in the colour of the scale.
/// A line of the view is [nick = the name, text = the value, own = the index of the node's colour (kNodeColour: node - 1), pad = the index into kLatColour]; a line with no text (the board is empty) is skipped. The corner is a free one: neither the chat's nor the one the notifications
/// are in, but the one beside the notifications on the same row (`corner ^ 2`: the half of the screen the chat does not use, where no message of the chat, however long, reaches); `start` is the first row of its stack (kTopY or kBottomY,
/// or the edge of a stack that is already there: a plugin's notification, the framework's), `limit` the row it may not pass (kNoLimit: the
/// screen's edge). A top stack runs down from `start` in the order of the view, a bottom one up from it with the first line at its top.
/// `edge`, when given, gets the row past the stack and its gap (the `limit` of the chat). Returns the blocks drawn.
inline u32 DrawLatencyLines(const gfx::Surface& s, const View& v, int corner, int start, int limit = kNoLimit, int* edge = nullptr) {
    static_assert(BlockHeight(1) + kGapY == 12, "a latency block and its gap are 12 rows");
    if (!TopUsable(s)) {
        return 0;
    }
    const u32 count = v.count < kLines ? v.count : kLines;
    const bool right = corner == kTopRight || corner == kBottomRight;
    const bool top = corner == kTopLeft || corner == kTopRight;
    const int below = limit == kNoLimit || limit > s.h ? s.h : limit;
    const int above = limit == kNoLimit || limit < 0 ? 0 : limit;
    const u32 bg_raw = gfx::Raw(kBg, s.format);
    int y = start;
    u32 drawn = 0;
    if (count == 1 && v.lines[0].pad == kLatTiles) { // many players: rows of tiles, 3, 2, 2
        const u32 tiles = v.lines[0].text_len / 2u;
        const u32 rows = TileRowsFor(tiles);
        const int total = static_cast<int>(rows) * kTileRowH;
        const int y0 = top ? y : y - total; // (a bottom stack: the bottom row of black is the 4 rows above the edge - the same height as three blocks and their gaps)
        const int span = top ? y0 + total : y0;
        bool fits = rows != 0 && (top ? y0 + total <= below : y0 >= above);
        for (u32 r = 0; r < rows && fits; ++r) {
            const int width = TilesWidth(TilesInRow(tiles, r));
            const int x0 = right ? kRightEdge - width : kLeftX;
            fits = x0 >= 0 && x0 + width <= s.w;
        }
        if (fits) {
            u32 first = 0;
            for (u32 r = 0; r < rows; ++r) {
                const u32 k = TilesInRow(tiles, r);
                const int width = TilesWidth(k);
                DrawTiles(s, right ? kRightEdge - width : kLeftX, y0 + static_cast<int>(r) * kTileRowH, v.lines[0], first, k, bg_raw, true);
                first += k;
            }
            y = top ? span : y0 - kGapY;
            drawn = 1;
        }
        if (edge != nullptr) {
            *edge = y;
        }
        return drawn;
    }    for (u32 k = 0; k < count; ++k) {
        const Line& l = v.lines[top ? k : count - 1 - k]; // (a bottom stack is built from the bottom: the last line first)
        if (l.text_len == 0) {
            continue;
        }
        const Wrap w = WrapLine(l);
        const int h = BlockHeight(w.lines);
        const int width = BlockWidth(WrapCols(w));
        const int y0 = top ? y : y - h;
        if (top ? y0 + h > below : y0 < above) {
            break;
        }
        const int x0 = right ? kRightEdge - width : kLeftX;
        if (x0 < 0 || x0 + width > s.w) {
            break;
        }
        y = top ? y0 + h + kGapY : y0 - kGapY;
        const u32 colour = l.pad < kLatColours ? l.pad : kLatColours - 1u;
        // the bar and the name ("Ana: ") in the colour of the nick, as in the chat (Line::own is its index); the value in the colour of the scale
        DrawBlock(s, x0, y0, l, w, gfx::Raw(kNodeColour[l.own % kNodeColours], s.format), gfx::Raw(kLatColour[colour], s.format), bg_raw, l.sep != '\0' ? 2u : 1u, true);
        ++drawn;
    }
    if (edge != nullptr) {
        *edge = y;
    }
    return drawn;
}
// <<< game-thread code

} // namespace chat
} // namespace azl
