// The plugin's one on-screen keyboard (the chat in the game and the password of a room in the paused menu): its layout and its state.
// Plain C++, no heap, no framework: the host feeds it the pad and the touch screen (Feed), copies what to draw (Snapshot -> View, a POD)
// and draws that with azl/osk_draw.hpp. Tests/osk_test.cpp drives it on the PC.
//
// The layout is the system keyboard's (the English QWERTY page of the 3DS's own software keyboard, measured to the pixel in
// analysis/chat-in-game-2026-11-04/PREP-TASTIERA-SISTEMA.md sections 2-3, with the user's changes of DECISIONI-IMPLEMENTAZIONE.md): the
// text on the bottom screen at the top (y 0-58, the up/down column at x 285-319 to scroll it), a bar (y 59-77: the title where the system
// has its predictive text, the count where it has its arrows), the keys in rows of 22 pixels from y 78, the tabs ABC | Symbol (y 192) and
// Cancel | Send at the bottom (y 217; the chat calls its Cancel "Close": its keyboard stays open after Send). The "English" key of the
// dictionary is gone (there is no dictionary) and the space takes its room.
// ONE table, kKeys, says where every key is: the touch is hit-tested on it and osk_draw.hpp draws it, so what is seen is what is touched
// (osk_test holds every key's pixels to its hit-test). A key's rectangle is [x, x + w) x [y, y + h); its left column and its top row are the
// dark line between it and the key before it (the keys of a row share their edges, as on the system's keyboard).
//
// The pad (no system applet: a plugin cannot start one from a game's APT session): the touch always works and a key is taken when the
// finger leaves it over the same key; the D-pad and the Circle Pad move the selected key, A presses it, B deletes the last character (and
// closes when there is none), L is shift for one character, R switches ABC / Symbol, START sends, SELECT closes (the host keeps the draft).
// A, B and the directions repeat after 400 ms, then every 90 ms. The edges are this class's own, from the keys HELD (Input::held): in the
// game the framework's own loop calls Controller::Update every frame, so "pressed since the last update" is nearly always 0 there
// (STUDIO-CHAT section 11). A key held when the keyboard opens (the chord that opened it) counts only once it is let go and pressed again.
//
// Memory: an Osk is about 300 bytes and owns no heap; the host makes it when it opens (new (std::nothrow)) and deletes it when it closes,
// and nothing that may run after the close (the game's OSD callback) points into it.
#pragma once

#include <cstddef>
#include <cstring>

#include "azl/common.hpp"

namespace azl {
namespace osk {

/// The pad's bits as the HID reports them (libctru's KEY_*, the framework's Key::*: Sources/game_menu.cpp asserts they are the same).
constexpr u32 kKeyA = 1u << 0, kKeyB = 1u << 1, kKeySelect = 1u << 2, kKeyStart = 1u << 3;
constexpr u32 kKeyRight = 1u << 4, kKeyLeft = 1u << 5, kKeyUp = 1u << 6, kKeyDown = 1u << 7;
constexpr u32 kKeyR = 1u << 8, kKeyL = 1u << 9;
constexpr u32 kKeyTouch = 1u << 20;
constexpr u32 kCpadRight = 1u << 28, kCpadLeft = 1u << 29, kCpadUp = 1u << 30, kCpadDown = 1u << 31;
constexpr u32 kDirs = kKeyRight | kKeyLeft | kKeyUp | kKeyDown;
constexpr u32 kRepeatable = kDirs | kKeyA | kKeyB;
constexpr u32 kUsedKeys = kRepeatable | kKeySelect | kKeyStart | kKeyR | kKeyL;

constexpr u32 kRepeatDelayMs = 400; ///< a held key repeats after this ...
constexpr u32 kRepeatMs = 90;       ///< ... and then this often
constexpr u32 kRevealMs = 1000;     ///< a password's last character stays visible this long (the system's SWKBD_PASSWORD_HIDE_DELAY)
constexpr u32 kBlinkMs = 500;       ///< the cursor's half period
constexpr u32 kFlashMs = 120;       ///< a key taken by a button of the pad (not the touch) is lit this long

constexpr u32 kTextMax = 128; ///< the most a text may have (the chat's: azl/chat_core.hpp kTextMax; the password asks 64)
constexpr u32 kCols = 35;     ///< characters on a line of the text area (x 4-283 in Spleen 8x16)
constexpr u32 kRows = 2;      ///< lines of the text area shown (on the dotted lines at y 27 and 45)
constexpr u32 kTitleMax = 23; ///< characters of the title in the bar
constexpr u32 kSubmitMax = 7; ///< characters of the label of the submit key ("Send", "OK") and of the cancel key ("Cancel", "Close")

/// The geometry of the screen (the bottom one, 320 x 240).
constexpr int kScreenW = 320, kScreenH = 240;
constexpr int kTextH = 59;            ///< the text area: rows 0-58
constexpr int kTextW = 285;           ///< ... and columns 0-284 (the scroll column from 285)
constexpr int kTextX = 4;             ///< the first column of the text
constexpr int kTextY[kRows] = {10, 28}; ///< the top row of each line (the dotted lines under them at 27 and 45)
constexpr int kDotY[kRows] = {27, 45};
constexpr int kHintY = 47;            ///< the line of the keys of the pad (Spleen 6x12, rows 47-58)
constexpr int kBarY = 59, kBarH = 19; ///< the bar: rows 59-77
constexpr int kKeysY = 78;            ///< the top line of the first row of keys
constexpr int kFrameBottom = 189;     ///< the line under the last row of keys
constexpr int kTabsY = 192, kTabsH = 22, kTabsEnd = 316; ///< the tabs (their closing lines at x 316 and y 214)
constexpr int kFootY = 217;           ///< Cancel | Send: rows 217-239

/// What a key does.
enum Kind : u8 {
    kChar = 0,   ///< types `ch` (a letter in upper case with shift XOR caps lock)
    kBackspace,  ///< deletes the last character
    kEnter,      ///< sends (the text is one line: there is no new line to type)
    kShift,      ///< upper case for the next character
    kCaps,       ///< caps lock (a<->A); on the Symbol page it is there, grey, and does nothing, as on the system's keyboard
    kSpace,      ///< types ' '
    kBlank,      ///< an empty cell of the Symbol grid: no character, not touched, not selected
    kTabAbc,     ///< the ABC page
    kTabSymbol,  ///< the Symbol page
    kCancel,     ///< closes; the host keeps the draft (its label is the host's: "Cancel", the chat's "Close")
    kSubmit,     ///< sends (Send / OK); off while the text is empty
    kScrollUp,   ///< the text area one line up (touch only)
    kScrollDown, ///< ... one line down (touch only)
};

/// The pages, and the bits of KeyDef::pages.
enum Page : u8 { kAbc = 0, kSymbol = 1 };
constexpr u8 kOnAbc = 1, kOnSymbol = 2, kOnBoth = 3;

struct KeyDef {
    u16 x, y, w, h;
    u8 kind;
    u8 pages;
    char ch; ///< kChar: the character (a letter in lower case)
};

// The table. Rows of the keyboard at y 78, 100, 122, 144 (22 rows each: the line, then 21 of key) and 166 (23 rows), the line under them at
// 189. The x of the ABC page are the system's own (PREP-TASTIERA-SISTEMA section 3), the Symbol page is its 9 x 4 grid of 24-pixel keys at
// x 33-249 with backspace and the tall enter at x 249-287 (section 5), its 4 last cells empty: the 32 printable ASCII characters that are not
// letters or digits, which is all the chat keeps (azl/chat_core.hpp cleans to 0x20-0x7E).
// clang-format off
// inline: one table in the plugin for game_menu.cpp and chat_overlay.cpp (a namespace constexpr has a copy in each unit, 1,152 bytes).
inline constexpr KeyDef kKeys[] = {
    // ABC, row 1: 1 2 3 4 5 6 7 8 9 0 - backspace
    {2, 78, 25, 22, kChar, kOnAbc, '1'}, {27, 78, 25, 22, kChar, kOnAbc, '2'}, {52, 78, 25, 22, kChar, kOnAbc, '3'},
    {77, 78, 25, 22, kChar, kOnAbc, '4'}, {102, 78, 25, 22, kChar, kOnAbc, '5'}, {127, 78, 25, 22, kChar, kOnAbc, '6'},
    {152, 78, 25, 22, kChar, kOnAbc, '7'}, {177, 78, 25, 22, kChar, kOnAbc, '8'}, {202, 78, 25, 22, kChar, kOnAbc, '9'},
    {227, 78, 25, 22, kChar, kOnAbc, '0'}, {252, 78, 25, 22, kChar, kOnAbc, '-'}, {277, 78, 40, 22, kBackspace, kOnAbc, 0},
    // ABC, row 2: q w e r t y u i o p enter
    {2, 100, 29, 22, kChar, kOnAbc, 'q'}, {31, 100, 25, 22, kChar, kOnAbc, 'w'}, {56, 100, 25, 22, kChar, kOnAbc, 'e'},
    {81, 100, 25, 22, kChar, kOnAbc, 'r'}, {106, 100, 25, 22, kChar, kOnAbc, 't'}, {131, 100, 25, 22, kChar, kOnAbc, 'y'},
    {156, 100, 25, 22, kChar, kOnAbc, 'u'}, {181, 100, 25, 22, kChar, kOnAbc, 'i'}, {206, 100, 25, 22, kChar, kOnAbc, 'o'},
    {231, 100, 25, 22, kChar, kOnAbc, 'p'}, {256, 100, 61, 22, kEnter, kOnAbc, 0},
    // ABC, row 3: a s d f g h j k l ' = /
    {2, 122, 33, 22, kChar, kOnAbc, 'a'}, {35, 122, 25, 22, kChar, kOnAbc, 's'}, {60, 122, 25, 22, kChar, kOnAbc, 'd'},
    {85, 122, 25, 22, kChar, kOnAbc, 'f'}, {110, 122, 25, 22, kChar, kOnAbc, 'g'}, {135, 122, 25, 22, kChar, kOnAbc, 'h'},
    {160, 122, 25, 22, kChar, kOnAbc, 'j'}, {185, 122, 25, 22, kChar, kOnAbc, 'k'}, {210, 122, 25, 22, kChar, kOnAbc, 'l'},
    {235, 122, 25, 22, kChar, kOnAbc, '\''}, {260, 122, 25, 22, kChar, kOnAbc, '='}, {285, 122, 32, 22, kChar, kOnAbc, '/'},
    // ABC, row 4: z x c v b n m , . ? ! @
    {2, 144, 37, 22, kChar, kOnAbc, 'z'}, {39, 144, 25, 22, kChar, kOnAbc, 'x'}, {64, 144, 25, 22, kChar, kOnAbc, 'c'},
    {89, 144, 25, 22, kChar, kOnAbc, 'v'}, {114, 144, 25, 22, kChar, kOnAbc, 'b'}, {139, 144, 25, 22, kChar, kOnAbc, 'n'},
    {164, 144, 25, 22, kChar, kOnAbc, 'm'}, {189, 144, 25, 22, kChar, kOnAbc, ','}, {214, 144, 25, 22, kChar, kOnAbc, '.'},
    {239, 144, 25, 22, kChar, kOnAbc, '?'}, {264, 144, 25, 22, kChar, kOnAbc, '!'}, {289, 144, 28, 22, kChar, kOnAbc, '@'},
    // ABC, row 5: caps lock, shift, the space to the edge (the system's "English" key of the dictionary is gone)
    {2, 166, 55, 23, kCaps, kOnAbc, 0}, {57, 166, 55, 23, kShift, kOnAbc, 0}, {112, 166, 205, 23, kSpace, kOnAbc, 0},
    // Symbol, the 9 x 4 grid
    {33, 78, 24, 22, kChar, kOnSymbol, '.'}, {57, 78, 24, 22, kChar, kOnSymbol, ','}, {81, 78, 24, 22, kChar, kOnSymbol, ':'},
    {105, 78, 24, 22, kChar, kOnSymbol, ';'}, {129, 78, 24, 22, kChar, kOnSymbol, '\''}, {153, 78, 24, 22, kChar, kOnSymbol, '"'},
    {177, 78, 24, 22, kChar, kOnSymbol, '`'}, {201, 78, 24, 22, kChar, kOnSymbol, '_'}, {225, 78, 24, 22, kChar, kOnSymbol, '-'},
    {33, 100, 24, 22, kChar, kOnSymbol, '!'}, {57, 100, 24, 22, kChar, kOnSymbol, '?'}, {81, 100, 24, 22, kChar, kOnSymbol, '@'},
    {105, 100, 24, 22, kChar, kOnSymbol, '#'}, {129, 100, 24, 22, kChar, kOnSymbol, '$'}, {153, 100, 24, 22, kChar, kOnSymbol, '%'},
    {177, 100, 24, 22, kChar, kOnSymbol, '^'}, {201, 100, 24, 22, kChar, kOnSymbol, '&'}, {225, 100, 24, 22, kChar, kOnSymbol, '*'},
    {33, 122, 24, 22, kChar, kOnSymbol, '('}, {57, 122, 24, 22, kChar, kOnSymbol, ')'}, {81, 122, 24, 22, kChar, kOnSymbol, '['},
    {105, 122, 24, 22, kChar, kOnSymbol, ']'}, {129, 122, 24, 22, kChar, kOnSymbol, '{'}, {153, 122, 24, 22, kChar, kOnSymbol, '}'},
    {177, 122, 24, 22, kChar, kOnSymbol, '<'}, {201, 122, 24, 22, kChar, kOnSymbol, '>'}, {225, 122, 24, 22, kChar, kOnSymbol, '~'},
    {33, 144, 24, 22, kChar, kOnSymbol, '+'}, {57, 144, 24, 22, kChar, kOnSymbol, '='}, {81, 144, 24, 22, kChar, kOnSymbol, '/'},
    {105, 144, 24, 22, kChar, kOnSymbol, '\\'}, {129, 144, 24, 22, kChar, kOnSymbol, '|'}, {153, 144, 24, 22, kBlank, kOnSymbol, 0},
    {177, 144, 24, 22, kBlank, kOnSymbol, 0}, {201, 144, 24, 22, kBlank, kOnSymbol, 0}, {225, 144, 24, 22, kBlank, kOnSymbol, 0},
    // Symbol: backspace, the enter three rows tall, caps lock (grey) and the space (where the system has its page count)
    {249, 78, 38, 22, kBackspace, kOnSymbol, 0}, {249, 100, 38, 66, kEnter, kOnSymbol, 0},
    {33, 166, 55, 23, kCaps, kOnSymbol, 0}, {88, 166, 199, 23, kSpace, kOnSymbol, 0},
    // Both pages: the two tabs, Cancel | Send, the up/down column of the text area
    {4, 192, 156, 22, kTabAbc, kOnBoth, 0}, {160, 192, 156, 22, kTabSymbol, kOnBoth, 0},
    {0, 217, 160, 23, kCancel, kOnBoth, 0}, {160, 217, 160, 23, kSubmit, kOnBoth, 0},
    {285, 0, 35, 29, kScrollUp, kOnBoth, 0}, {285, 29, 35, 30, kScrollDown, kOnBoth, 0},
};
// clang-format on
constexpr u32 kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr u8 kNoKey = 0xFF;
static_assert(kKeyCount < kNoKey, "a key's index is a byte");

/// The frame of the keyboard of a page: [x0, x1) x [kKeysY, kFrameBottom), the closing lines at x1 and kFrameBottom.
constexpr int kFrameX0[2] = {2, 33};
constexpr int kFrameX1[2] = {317, 287};

inline bool OnPage(const KeyDef& k, u32 page) {
    return (k.pages & (1u << page)) != 0;
}

/// A key that does something on `page` (an empty cell and the grey caps lock of the Symbol page do nothing).
inline bool Live(const KeyDef& k, u32 page) {
    return OnPage(k, page) && k.kind != kBlank && !(k.kind == kCaps && page == kSymbol);
}

/// The key under the point (x, y) of the bottom screen on `page`, or kNoKey.
inline u8 HitTest(u32 page, int x, int y) {
    for (u32 i = 0; i < kKeyCount; ++i) {
        const KeyDef& k = kKeys[i];
        if (Live(k, page) && x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) {
            return static_cast<u8>(i);
        }
    }
    return kNoKey;
}

/// The first key of `kind` on `page` (kNoKey if there is none; for kChar the one of `ch`).
inline u8 FindKey(u32 page, u8 kind, char ch = 0) {
    for (u32 i = 0; i < kKeyCount; ++i) {
        const KeyDef& k = kKeys[i];
        if (OnPage(k, page) && k.kind == kind && (kind != kChar || k.ch == ch)) {
            return static_cast<u8>(i);
        }
    }
    return kNoKey;
}

struct Options {
    u32 max_len = kTextMax;   ///< the chat 128, the password 64
    bool mask = false;        ///< the password: '*', the last character typed visible for kRevealMs
    bool allow_empty = false; ///< Send / OK also with no text
    const char* title = "Chat";
    const char* submit = "Send";
    const char* cancel = "Cancel"; ///< the label of the key that closes (the chat's "Close", the password's "Cancel")
};

struct Input {
    u32 held = 0;     ///< the keys held (hidKeysHeld / Controller::GetKeysDown): the edges are made here
    bool touch = false;
    u16 tx = 0, ty = 0; ///< where the screen is touched (when `touch`)
    u32 now_ms = 0;   ///< a clock in milliseconds (it may wrap)
};

enum class Result : u8 { kNone = 0, kSubmit, kCancel };

/// What there is to draw, copied out (Osk::Snapshot): no pointer, so a host may keep it, compare it with the last one it drew (Same) and
/// hand it to another thread. Every byte is written (the padding too), so two equal views are equal byte for byte.
struct View {
    char text[kRows][kCols]; ///< the characters of the lines shown (a password's already masked)
    u8 text_len[kRows];
    u8 cursor_row;  ///< the line of the cursor among those shown, kRows when it is scrolled away
    u8 cursor_col;
    u8 cursor_on;   ///< the blink
    u8 page;        ///< kAbc / kSymbol
    u8 shift;       ///< the next letter is upper case (shift, not caps lock)
    u8 caps;        ///< caps lock
    u8 selected;    ///< the key of the D-pad, kNoKey while it is hidden (the touch hides it, a direction or A shows it)
    u8 pressed;     ///< the key lit: under the finger, or just taken by a button; kNoKey
    u8 can_up, can_down; ///< the text area can scroll
    u8 submit_on;   ///< Send / OK would send
    u8 at_max;      ///< the text is as long as it may be
    u16 len, max_len;
    char title[kTitleMax + 1];
    char submit[kSubmitMax + 1];
    char cancel[kSubmitMax + 1];
};

inline bool Same(const View& a, const View& b) {
    return std::memcmp(&a, &b, sizeof(View)) == 0;
}

class Osk {
public:
    /// `draft`: the text to start from (the chat's draft, the password kept); what is not printable ASCII is left out, what is past
    /// max_len too.
    explicit Osk(const Options& o, const char* draft = nullptr)
        : max_len_(o.max_len == 0 || o.max_len > kTextMax ? kTextMax : o.max_len), mask_(o.mask), allow_empty_(o.allow_empty) {
        CopyLabel(title_, sizeof(title_), o.title);
        CopyLabel(submit_, sizeof(submit_), o.submit);
        CopyLabel(cancel_, sizeof(cancel_), o.cancel);
        CopyText(draft);
        sel_ = FindKey(kAbc, kChar, 'q');
        nav_x_ = CentreX(sel_);
        nav_y_ = CentreY(sel_);
    }

    /// One look at the pad and the touch screen: Submit (Send / OK, or enter, or START) and Cancel (Cancel, SELECT, B with no text) close.
    Result Feed(const Input& in) {
        now_ = in.now_ms;
        // (the Circle Pad is the D-pad: its four bits are the D-pad's moved up 24 places)
        u32 held = in.held | ((in.held >> 24) & kDirs);
        held &= kUsedKeys;
        const u32 pressed = held & ~prev_held_;
        prev_held_ = held;
        u32 repeated = 0;
        if (pressed & kRepeatable) {
            const u32 rep = pressed & kRepeatable;
            rep_key_ = rep & (0u - rep); // (one of them repeats: the lowest bit)
            rep_at_ = now_ + kRepeatDelayMs;
        } else if (rep_key_ != 0 && (held & rep_key_) != 0) {
            if (static_cast<s32>(now_ - rep_at_) >= 0) {
                repeated = rep_key_;
                rep_at_ += kRepeatMs;
                if (static_cast<s32>(now_ - rep_at_) >= 0) {
                    rep_at_ = now_ + kRepeatMs; // (a host that looked late: no burst of repeats)
                }
            }
        } else {
            rep_key_ = 0;
        }

        Result r = Touch(in);
        if (r != Result::kNone) {
            return r;
        }
        const u32 act = pressed | repeated;
        if (act & kDirs) {
            if (!sel_shown_) {
                sel_shown_ = true; // (the first direction shows where the selection is)
            } else {
                Move(act & kDirs);
            }
        }
        if (act & kKeyA) {
            if (!sel_shown_) {
                sel_shown_ = true;
                if (rep_key_ == kKeyA) {
                    rep_key_ = 0; // (an A that only showed the selection does not type when held: it is pressed again to type)
                }
            } else {
                Flash(sel_);
                r = Take(sel_, (repeated & kKeyA) != 0);
                if (r != Result::kNone) {
                    return r;
                }
            }
        }
        if (act & kKeyB) {
            if (len_ == 0) {
                if (pressed & kKeyB) {
                    Flash(FindKey(page_, kBackspace));
                    return Result::kCancel; // (a repeat that emptied the text does not close: only a new press on no text)
                }
            } else {
                Flash(FindKey(page_, kBackspace));
                Backspace();
            }
        }
        if ((pressed & kKeyL) && page_ == kAbc) {
            shift_ = !shift_;
            Flash(FindKey(kAbc, kShift));
        }
        if (pressed & kKeyR) {
            SetPage(page_ == kAbc ? kSymbol : kAbc);
            Flash(FindKey(page_, page_ == kAbc ? kTabAbc : kTabSymbol));
        }
        if ((pressed & kKeyStart) && CanSubmit()) {
            Flash(FindKey(page_, kSubmit));
            return Result::kSubmit;
        }
        if (pressed & kKeySelect) {
            Flash(FindKey(page_, kCancel));
            return Result::kCancel;
        }
        return Result::kNone;
    }

    /// What to draw now (as of the last Feed's clock).
    void Snapshot(View* v) const {
        std::memset(v, 0, sizeof(View));
        const u32 lines = len_ / kCols + 1; // (the line of the cursor counts, empty or not)
        const u32 last_first = lines > kRows ? lines - kRows : 0;
        const u32 first = follow_ ? last_first : (first_ < last_first ? first_ : last_first);
        const bool reveal = mask_ && reveal_ && len_ > 0 && now_ - reveal_at_ < kRevealMs;
        for (u32 row = 0; row < kRows; ++row) {
            const u32 from = (first + row) * kCols;
            u32 n = 0;
            for (; n < kCols && from + n < len_; ++n) {
                const u32 i = from + n;
                v->text[row][n] = !mask_ || (reveal && i + 1 == len_) ? text_[i] : '*';
            }
            v->text_len[row] = static_cast<u8>(n);
        }
        const u32 cursor_line = len_ / kCols;
        v->cursor_row = static_cast<u8>(cursor_line >= first && cursor_line < first + kRows ? cursor_line - first : kRows);
        v->cursor_col = static_cast<u8>(len_ % kCols);
        v->cursor_on = static_cast<u8>(((now_ - blink_at_) / kBlinkMs) % 2 == 0);
        v->page = page_;
        v->shift = shift_;
        v->caps = caps_;
        v->selected = sel_shown_ ? sel_ : kNoKey;
        v->pressed = touch_on_ ? touch_key_ : (flash_key_ != kNoKey && now_ - flash_at_ < kFlashMs ? flash_key_ : kNoKey);
        v->can_up = first > 0;
        v->can_down = first < last_first;
        v->submit_on = CanSubmit();
        v->at_max = len_ >= max_len_;
        v->len = static_cast<u16>(len_);
        v->max_len = static_cast<u16>(max_len_);
        std::memcpy(v->title, title_, sizeof(title_));
        std::memcpy(v->submit, submit_, sizeof(submit_));
        std::memcpy(v->cancel, cancel_, sizeof(cancel_));
    }

    const char* text() const {
        return text_;
    }
    /// The text emptied, the keyboard left as it is (the chat's Send: it stays open for the next message; its page, shift and caps lock
    /// stay, the cursor's line is shown again).
    void Clear() {
        CopyText(nullptr);
        Edited();
    }
    /// The text replaced by `text` (a message the room refused, given back to an empty keyboard): what is not printable ASCII is left
    /// out, what is past max_len too.
    void SetText(const char* text) {
        CopyText(text);
        Edited();
    }
    u32 length() const {
        return len_;
    }
    u32 page() const {
        return page_;
    }

private:
    void CopyText(const char* text) {
        len_ = 0;
        for (const char* p = text; p && *p && len_ < max_len_; ++p) {
            if (*p >= 0x20 && *p <= 0x7E) {
                text_[len_++] = *p;
            }
        }
        text_[len_] = 0;
        reveal_ = false;
        first_ = 0;
    }
    static void CopyLabel(char* out, std::size_t size, const char* in) {
        std::size_t n = 0;
        for (; in && in[n] && n + 1 < size; ++n) {
            out[n] = in[n] >= 0x20 && in[n] <= 0x7E ? in[n] : ' ';
        }
        std::memset(out + n, 0, size - n);
    }
    static int CentreX(u8 k) {
        return kKeys[k].x + kKeys[k].w / 2;
    }
    static int CentreY(u8 k) {
        return kKeys[k].y + kKeys[k].h / 2;
    }
    static bool Navigable(const KeyDef& k, u32 page) {
        return Live(k, page) && k.kind != kScrollUp && k.kind != kScrollDown;
    }

    bool CanSubmit() const {
        return allow_empty_ || len_ > 0;
    }

    void Flash(u8 key) {
        flash_key_ = key;
        flash_at_ = now_;
    }

    void Edited() {
        follow_ = true;
        blink_at_ = now_;
    }

    void Insert(char c) {
        if (len_ >= max_len_) {
            return;
        }
        if (c >= 'a' && c <= 'z' && (shift_ != caps_)) {
            c = static_cast<char>(c - 'a' + 'A');
        }
        text_[len_++] = c;
        text_[len_] = 0;
        shift_ = false; // (shift is for one character, whatever it is)
        reveal_ = true;
        reveal_at_ = now_;
        Edited();
    }

    void Backspace() {
        if (len_ > 0) {
            text_[--len_] = 0;
        }
        reveal_ = false;
        Edited();
    }

    void SetPage(u8 page) {
        if (page == page_) {
            return;
        }
        const KeyDef& old = kKeys[sel_];
        page_ = page;
        // a finger on a key of the other page (R pressed while touching) is no key any more: lifted, it would type a key not shown
        touch_on_ = false;
        touch_key_ = kNoKey;
        if (!Navigable(old, page_)) {
            // the key nearest to the one that was selected
            long best = -1;
            u8 pick = sel_;
            for (u32 i = 0; i < kKeyCount; ++i) {
                if (!Navigable(kKeys[i], page_)) {
                    continue;
                }
                const long dx = CentreX(static_cast<u8>(i)) - CentreX(sel_), dy = CentreY(static_cast<u8>(i)) - CentreY(sel_);
                const long d = dx * dx + dy * dy;
                if (best < 0 || d < best) {
                    best = d;
                    pick = static_cast<u8>(i);
                }
            }
            sel_ = pick;
            nav_x_ = CentreX(sel_);
            nav_y_ = CentreY(sel_);
        }
    }

    /// The selection one key in `dir` (one of the D-pad's bits; with two at once, the vertical one first). Up and down go to the next
    /// row above or below and the key whose centre is nearest to the column the moves started from; left and right go round the row.
    void Move(u32 dir) {
        const KeyDef& k = kKeys[sel_];
        if (dir & (kKeyUp | kKeyDown)) {
            const bool down = (dir & kKeyDown) != 0 && (dir & kKeyUp) == 0;
            int row = -1; // the top (down) or bottom (up) line of the next row
            for (u32 i = 0; i < kKeyCount; ++i) {
                const KeyDef& c = kKeys[i];
                if (!Navigable(c, page_)) {
                    continue;
                }
                if (down && c.y >= k.y + k.h && (row < 0 || c.y < row)) {
                    row = c.y;
                } else if (!down && c.y + c.h <= k.y && (row < 0 || c.y + c.h > row)) {
                    row = c.y + c.h;
                }
            }
            if (row < 0) {
                return; // (no row past the edge: the selection stays)
            }
            int best = -1;
            u8 pick = sel_;
            for (u32 i = 0; i < kKeyCount; ++i) {
                const KeyDef& c = kKeys[i];
                if (!Navigable(c, page_) || (down ? c.y != row : c.y + c.h != row)) {
                    continue;
                }
                const int d = CentreX(static_cast<u8>(i)) > nav_x_ ? CentreX(static_cast<u8>(i)) - nav_x_ : nav_x_ - CentreX(static_cast<u8>(i));
                if (best < 0 || d < best) {
                    best = d;
                    pick = static_cast<u8>(i);
                }
            }
            sel_ = pick;
            nav_y_ = CentreY(sel_);
            return;
        }
        const bool right = (dir & kKeyRight) != 0;
        const int cx = CentreX(sel_);
        int next = -1, wrap = -1;
        u8 next_key = sel_, wrap_key = sel_;
        for (u32 i = 0; i < kKeyCount; ++i) {
            const KeyDef& c = kKeys[i];
            if (i == sel_ || !Navigable(c, page_) || nav_y_ < c.y || nav_y_ >= c.y + c.h) {
                continue;
            }
            const int x = CentreX(static_cast<u8>(i));
            if (right ? x > cx && (next < 0 || x < next) : x < cx && (next < 0 || x > next)) {
                next = x;
                next_key = static_cast<u8>(i);
            }
            if (right ? (wrap < 0 || x < wrap) : (wrap < 0 || x > wrap)) {
                wrap = x;
                wrap_key = static_cast<u8>(i);
            }
        }
        sel_ = next >= 0 ? next_key : wrap_key;
        nav_x_ = CentreX(sel_);
    }

    /// The key `key` taken (touch, A); `repeat`: by A held (only what types or deletes repeats).
    Result Take(u8 key, bool repeat) {
        if (key == kNoKey) {
            return Result::kNone;
        }
        const KeyDef& k = kKeys[key];
        if (repeat && k.kind != kChar && k.kind != kSpace && k.kind != kBackspace) {
            return Result::kNone;
        }
        switch (k.kind) {
        case kChar:
            Insert(k.ch);
            break;
        case kSpace:
            Insert(' ');
            break;
        case kBackspace:
            Backspace();
            break;
        case kShift:
            shift_ = !shift_;
            break;
        case kCaps:
            if (page_ == kAbc) {
                caps_ = !caps_;
            }
            break;
        case kTabAbc:
            SetPage(kAbc);
            break;
        case kTabSymbol:
            SetPage(kSymbol);
            break;
        case kEnter:
        case kSubmit:
            if (CanSubmit()) {
                return Result::kSubmit;
            }
            break;
        case kCancel:
            return Result::kCancel;
        case kScrollUp:
        case kScrollDown: {
            const u32 lines = len_ / kCols + 1;
            const u32 last_first = lines > kRows ? lines - kRows : 0;
            u32 first = follow_ ? last_first : (first_ < last_first ? first_ : last_first);
            if (k.kind == kScrollUp && first > 0) {
                --first;
            } else if (k.kind == kScrollDown && first < last_first) {
                ++first;
            }
            first_ = static_cast<u8>(first);
            follow_ = first == last_first; // (back at the cursor: typing keeps it in view again)
            break;
        }
        default:
            break;
        }
        return Result::kNone;
    }

    /// The touch screen: a key is lit while the finger is on it and taken when the finger leaves the screen over it (moved off it and back
    /// on, it is still the key; lifted elsewhere, nothing). A touch that was on when the keyboard opened is not a key.
    Result Touch(const Input& in) {
        Result r = Result::kNone;
        if (in.touch) {
            const u8 under = HitTest(page_, in.tx, in.ty);
            if (!touch_prev_) {
                touch_key_ = under;
            }
            touch_on_ = touch_key_ != kNoKey && under == touch_key_;
        } else {
            if (touch_prev_ && touch_on_ && touch_key_ != kNoKey) {
                const u8 key = touch_key_;
                sel_shown_ = false; // (the touch hides the D-pad's selection, and puts it where the finger was)
                if (Navigable(kKeys[key], page_)) {
                    sel_ = key;
                    nav_x_ = CentreX(key);
                    nav_y_ = CentreY(key);
                }
                Flash(key); // (lit a moment after the finger leaves: a tap shorter than a look of the host is seen too)
                r = Take(key, false);
            }
            touch_on_ = false;
            touch_key_ = kNoKey;
        }
        touch_prev_ = in.touch;
        return r;
    }

    char text_[kTextMax + 1] = {};
    u32 len_ = 0;
    u32 max_len_;
    bool mask_;
    bool allow_empty_;
    char title_[kTitleMax + 1];
    char submit_[kSubmitMax + 1];
    char cancel_[kSubmitMax + 1];
    u32 now_ = 0;
    u32 prev_held_ = 0xFFFFFFFFu; // (everything "held" before the first look: what is held at the opening is no press)
    u32 rep_key_ = 0;
    u32 rep_at_ = 0;
    bool touch_prev_ = true;      // (the same for a finger already on the screen)
    bool touch_on_ = false;
    u8 touch_key_ = kNoKey;
    u8 flash_key_ = kNoKey;
    u32 flash_at_ = 0;
    u8 page_ = kAbc;
    bool shift_ = false;
    bool caps_ = false;
    u8 sel_ = 0;
    bool sel_shown_ = false;
    int nav_x_ = 0, nav_y_ = 0;   // the column and the row the D-pad's moves keep
    bool follow_ = true;          // the text area shows the cursor's line
    u8 first_ = 0;                // the first line shown when it does not
    bool reveal_ = false;
    u32 reveal_at_ = 0;
    u32 blink_at_ = 0;
};

} // namespace osk
} // namespace azl
