// The plugin's on-screen keyboard (azl/osk.hpp: its table and its state; azl/osk_draw.hpp: its drawing), run by Tests/run.sh with
// ASan/UBSan. What it holds:
// - the table: the keys where PREP-TASTIERA-SISTEMA sections 2, 3 and 5 measured the system's (written out here again), every pixel of a
//   page's keyboard in exactly one key, no two keys of a page on each other, every key's centre and corners hit-tested to that key and the
//   pixels just outside it to another;
// - the state: text typed by touch (taken on the release over the same key: moved off it, nothing; moved back on, the key; a finger already
//   down at the opening, nothing), by the D-pad and A (the first direction only shows the selection, the moves up/down to the nearest key
//   of the next row and left/right round a row, the Circle Pad as the D-pad), the repeat of A, B and the directions (400 ms, then 90 ms; on
//   a clock that wraps; looked at 30 times a second), shift for one character and caps lock, the two pages (R and the tabs), B deleting and
//   closing only on a new press with no text, START sending only with text, SELECT closing with the draft kept, enter sending, the limits
//   (64 and 128) and the draft cleaned, the password's mask with the last character for 1 s, the scroll of the text area, a key held when
//   the keyboard opened, the lit keys, and no allocation after the opening (malloc is wrapped by the link);
// - the drawing: every pixel of the screen written (nothing outside it: guard bytes), the five formats the same picture, the pixels that
//   change when a key is lit or selected all inside that key's rectangle of the table (what is seen is what is touched), the icons, the cost
//   of a whole screen (stores counted).
// With a folder as its argument it writes the pictures there as BMP files (ABC, shift, Symbol, a selected key, a pressed key, the chat with
// its text and count, the password masked and with its last character).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static unsigned long g_stores[5]; // [1], [2], [4]: stores of that many bytes
#define AZL_GFX_COUNT_STORE(bytes) (++g_stores[(bytes)])

#include "azl/osk.hpp"
#include "azl/osk_draw.hpp"

using namespace azl;
using namespace azl::osk;
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
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return std::malloc(n ? n : 1);
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

static const char* const kFormatNames[5] = {"rgba8", "bgr8", "rgb565", "rgb5a1", "rgba4"};

// ---------------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------------

static const KeyDef& Key(u8 i) {
    return kKeys[i];
}

static u8 CharKey(u32 page, char c) {
    return FindKey(page, kChar, c);
}

// A key of `page` whose rectangle is [x0, x1) x [y0, y0 + h).
static bool Is(u8 i, int x0, int x1, int y0, int h) {
    return i != kNoKey && Key(i).x == x0 && Key(i).x + Key(i).w == x1 && Key(i).y == y0 && Key(i).h == h;
}

static void TestTable() {
    // The system's x (PREP-TASTIERA-SISTEMA section 3): R1 1[2-27] .. -[252-277] backspace[277-317]; R2 q[2-31] .. p[231-256]
    // enter[256-317]; R3 a[2-35] .. =[260-285] /[285-317]; R4 z[2-39] .. ![264-289] @[289-317]; R5 caps[2-57] shift[57-112] and the space
    // to 317 where the system has the "English" key at 212-317.
    const char* r1 = "1234567890-";
    for (int i = 0; i < 11; ++i) {
        CHECK(Is(CharKey(kAbc, r1[i]), 2 + 25 * i, 27 + 25 * i, 78, 22));
    }
    CHECK(Is(FindKey(kAbc, kBackspace), 277, 317, 78, 22));
    CHECK(Is(CharKey(kAbc, 'q'), 2, 31, 100, 22));
    const char* r2 = "wertyuiop";
    for (int i = 0; i < 9; ++i) {
        CHECK(Is(CharKey(kAbc, r2[i]), 31 + 25 * i, 56 + 25 * i, 100, 22));
    }
    CHECK(Is(FindKey(kAbc, kEnter), 256, 317, 100, 22));
    CHECK(Is(CharKey(kAbc, 'a'), 2, 35, 122, 22));
    const char* r3 = "sdfghjkl'=";
    for (int i = 0; i < 10; ++i) {
        CHECK(Is(CharKey(kAbc, r3[i]), 35 + 25 * i, 60 + 25 * i, 122, 22));
    }
    CHECK(Is(CharKey(kAbc, '/'), 285, 317, 122, 22));
    CHECK(Is(CharKey(kAbc, 'z'), 2, 39, 144, 22));
    const char* r4 = "xcvbnm,.?!";
    for (int i = 0; i < 10; ++i) {
        CHECK(Is(CharKey(kAbc, r4[i]), 39 + 25 * i, 64 + 25 * i, 144, 22));
    }
    CHECK(Is(CharKey(kAbc, '@'), 289, 317, 144, 22));
    CHECK(Is(FindKey(kAbc, kCaps), 2, 57, 166, 23));
    CHECK(Is(FindKey(kAbc, kShift), 57, 112, 166, 23));
    CHECK(Is(FindKey(kAbc, kSpace), 112, 317, 166, 23));
    // Symbol (section 5): the 9 x 4 grid of 24 at x 33-249, backspace and the enter of three rows at 249-287, caps 33-88, space to 287
    const char* grid = ".,:;'\"`_-!?@#$%^&*()[]{}<>~+=/\\|";
    for (int i = 0; i < 32; ++i) {
        CHECK(Is(CharKey(kSymbol, grid[i]), 33 + 24 * (i % 9), 57 + 24 * (i % 9), 78 + 22 * (i / 9), 22));
    }
    CHECK(Is(FindKey(kSymbol, kBackspace), 249, 287, 78, 22));
    CHECK(Is(FindKey(kSymbol, kEnter), 249, 287, 100, 66));
    CHECK(Is(FindKey(kSymbol, kCaps), 33, 88, 166, 23));
    CHECK(Is(FindKey(kSymbol, kSpace), 88, 287, 166, 23));
    // both pages: the tabs (two wide ones where the system has four at 4-84-160-236-315), Cancel | OK (0-159 | 160-319, the line above at
    // 217), the scroll column (from x 285, split at y 29)
    for (u32 page = 0; page < 2; ++page) {
        CHECK(Is(FindKey(page, kTabAbc), 4, 160, 192, 22));
        CHECK(Is(FindKey(page, kTabSymbol), 160, 316, 192, 22));
        CHECK(Is(FindKey(page, kCancel), 0, 160, 217, 23));
        CHECK(Is(FindKey(page, kSubmit), 160, 320, 217, 23));
        CHECK(Is(FindKey(page, kScrollUp), 285, 320, 0, 29));
        CHECK(Is(FindKey(page, kScrollDown), 285, 320, 29, 30));
    }
    // all the printable ASCII on the two pages: the letters and digits on ABC, the 32 others on Symbol (some on ABC too)
    for (int c = 0x21; c < 0x7F; ++c) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c >= 'A' && c <= 'Z') {
            continue;
        }
        CHECK((alnum ? CharKey(kAbc, static_cast<char>(c)) : CharKey(kSymbol, static_cast<char>(c))) != kNoKey);
    }
    // every key inside the screen, every pixel of a page's keyboard in exactly one of its keys (the blank cells too), no pixel of the screen
    // in two keys of a page; the hit-test of every pixel is the key that holds it, when that key does something
    for (u32 page = 0; page < 2; ++page) {
        int overlaps = 0, holes = 0, wrong = 0;
        for (int y = 0; y < kScreenH; ++y) {
            for (int x = 0; x < kScreenW; ++x) {
                int count = 0;
                u8 holder = kNoKey;
                for (u32 i = 0; i < kKeyCount; ++i) {
                    const KeyDef& k = kKeys[i];
                    if (OnPage(k, page) && x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) {
                        ++count;
                        holder = static_cast<u8>(i);
                    }
                }
                overlaps += count > 1;
                const bool in_frame = x >= kFrameX0[page] && x < kFrameX1[page] && y >= kKeysY && y < kFrameBottom;
                holes += in_frame && count == 0;
                const u8 want = holder != kNoKey && Live(kKeys[holder], page) ? holder : kNoKey;
                wrong += HitTest(page, x, y) != want;
            }
        }
        CHECK(overlaps == 0);
        CHECK(holes == 0);
        CHECK(wrong == 0);
    }
    for (u32 i = 0; i < kKeyCount; ++i) {
        const KeyDef& k = kKeys[i];
        CHECK(k.x + k.w <= kScreenW && k.y + k.h <= kScreenH && k.w > 2 && k.h > 2);
        for (u32 page = 0; page < 2; ++page) {
            if (!Live(k, page)) {
                continue;
            }
            const u8 me = static_cast<u8>(i);
            CHECK(HitTest(page, k.x + k.w / 2, k.y + k.h / 2) == me);
            CHECK(HitTest(page, k.x, k.y) == me);
            CHECK(HitTest(page, k.x + k.w - 1, k.y) == me);
            CHECK(HitTest(page, k.x, k.y + k.h - 1) == me);
            CHECK(HitTest(page, k.x + k.w - 1, k.y + k.h - 1) == me);
            CHECK(HitTest(page, k.x - 1, k.y + k.h / 2) != me);
            CHECK(HitTest(page, k.x + k.w, k.y + k.h / 2) != me);
            CHECK(HitTest(page, k.x + k.w / 2, k.y - 1) != me);
            CHECK(HitTest(page, k.x + k.w / 2, k.y + k.h) != me);
        }
    }
    // the blank cells and the grey caps lock of Symbol are not keys there
    CHECK(HitTest(kSymbol, 165, 155) == kNoKey);
    CHECK(HitTest(kSymbol, 40, 170) == kNoKey);
    CHECK(HitTest(kAbc, 40, 170) == FindKey(kAbc, kCaps));
    CHECK(HitTest(kAbc, 100, 30) == kNoKey); // (the text area)
    // the icons are as wide as they say
    const Icon* icons[] = {&kIconBackspace, &kIconEnter, &kIconShift, &kIconTurn, &kIconUp, &kIconDown};
    for (const Icon* ic : icons) {
        for (u32 r = 0; r < ic->h; ++r) {
            CHECK(std::strlen(ic->rows[r]) == ic->w);
        }
        CHECK(ic->h <= 16);
    }
}

// ---------------------------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------------------------

struct Driver {
    Osk& k;
    u32 now;
    u32 step = 33; // (the host looks 30 times a second)
    Driver(Osk& osk, u32 start = 1000) : k(osk), now(start) {}
    Result Feed(u32 held, bool touch = false, int x = 0, int y = 0) {
        Input in;
        in.held = held;
        in.touch = touch;
        in.tx = static_cast<u16>(x);
        in.ty = static_cast<u16>(y);
        in.now_ms = now;
        const Result r = k.Feed(in);
        now += step;
        return r;
    }
    // a button pressed for one look and let go
    Result Press(u32 bit) {
        const Result r = Feed(bit);
        const Result r2 = Feed(0);
        return r != Result::kNone ? r : r2;
    }
    // a tap on key `i`: the finger down on its centre, then up
    Result Tap(u8 i) {
        const KeyDef& kd = Key(i);
        Feed(0, true, kd.x + kd.w / 2, kd.y + kd.h / 2);
        return Feed(0, false);
    }
    Result Type(const char* s) {
        Result r = Result::kNone;
        for (const char* p = s; *p; ++p) {
            const char c = *p;
            u8 key = c == ' ' ? FindKey(k.page(), kSpace) : CharKey(k.page(), c);
            if (key == kNoKey) {
                Press(kKeyR); // (the other page)
                key = c == ' ' ? FindKey(k.page(), kSpace) : CharKey(k.page(), c);
            }
            r = Tap(key);
        }
        return r;
    }
    View Snap() const {
        View v;
        k.Snapshot(&v);
        return v;
    }
};

static std::string Text(const Osk& k) {
    return std::string(k.text(), k.length());
}

static Options Chat() {
    Options o;
    o.cancel = "Close"; // (the chat's keyboard stays open after Send: its Cancel key closes it, Sources/ctr/chat_overlay.cpp)
    return o;
}
static Options Password() {
    Options o;
    o.max_len = 64;
    o.mask = true;
    o.title = "Room password";
    o.submit = "OK";
    return o;
}

static void TestTouch() {
    {
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Type("hello world");
        CHECK(Text(k) == "hello world");
        d.Type("1+1=2");
        CHECK(Text(k) == "hello world1+1=2");
        d.Type("#");
        CHECK(k.page() == kSymbol); // (# took the Symbol page)
        CHECK(Text(k) == "hello world1+1=2#");
    }
    {
        // the key is taken when the finger leaves the screen over it: moved off it, nothing; moved back on, the key; lit only while on it
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        const u8 h = CharKey(kAbc, 'h'), j = CharKey(kAbc, 'j');
        d.Feed(0, true, Key(h).x + 5, Key(h).y + 5);
        CHECK(d.Snap().pressed == h);
        d.Feed(0, true, Key(j).x + 5, Key(j).y + 5);
        CHECK(d.Snap().pressed == kNoKey);
        d.Feed(0, false);
        CHECK(Text(k).empty());
        d.Feed(0, true, Key(h).x + 5, Key(h).y + 5);
        d.Feed(0, true, Key(j).x + 5, Key(j).y + 5);
        d.Feed(0, true, Key(h).x + 20, Key(h).y + 15);
        CHECK(d.Snap().pressed == h);
        d.Feed(0, false);
        CHECK(Text(k) == "h");
        CHECK(d.Snap().pressed == h); // (lit a moment after the release ...)
        d.now += kFlashMs;
        d.Feed(0);
        CHECK(d.Snap().pressed == kNoKey); // (... and then not)
        // a tap outside every key: nothing
        d.Feed(0, true, 100, 30);
        d.Feed(0, false);
        CHECK(Text(k) == "h");
    }
    {
        // a finger already on the screen when the keyboard opens is not a key
        Osk k(Chat());
        Driver d(k);
        const u8 q = CharKey(kAbc, 'q');
        d.Feed(0, true, Key(q).x + 5, Key(q).y + 5);
        d.Feed(0, true, Key(q).x + 5, Key(q).y + 5);
        d.Feed(0, false);
        CHECK(Text(k).empty());
        d.Tap(q);
        CHECK(Text(k) == "q");
    }
    {
        // a tap that lasts one look only (the host saw the finger once): still a key
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Tap(CharKey(kAbc, 'x'));
        CHECK(Text(k) == "x");
    }
}

static void TestPad() {
    {
        // the first direction shows the selection where it is (q), then the moves
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        CHECK(d.Snap().selected == kNoKey);
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == CharKey(kAbc, 'q'));
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == CharKey(kAbc, 'w'));
        d.Press(kKeyA);
        CHECK(Text(k) == "w");
        CHECK(d.Snap().pressed == CharKey(kAbc, 'w')); // (lit by A)
        // down from w (centre 43): the nearest of row 3 is s (47); down again: x (51); up twice: back to w (the column kept)
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == CharKey(kAbc, 's'));
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == CharKey(kAbc, 'x'));
        d.Press(kKeyUp);
        d.Press(kKeyUp);
        CHECK(d.Snap().selected == CharKey(kAbc, 'w'));
        d.Press(kKeyUp);
        CHECK(d.Snap().selected == CharKey(kAbc, '2'));
        d.Press(kKeyUp); // (no row above: it stays)
        CHECK(d.Snap().selected == CharKey(kAbc, '2'));
        // left round the row: 2 -> 1 -> backspace
        d.Press(kKeyLeft);
        d.Press(kKeyLeft);
        CHECK(d.Snap().selected == FindKey(kAbc, kBackspace));
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == CharKey(kAbc, '1'));
        // down to the bottom: row 2, 3, 4, 5 (caps), the ABC tab, Cancel, and no further
        for (int i = 0; i < 4; ++i) {
            d.Press(kKeyDown);
        }
        CHECK(d.Snap().selected == FindKey(kAbc, kCaps));
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == FindKey(kAbc, kTabAbc));
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == FindKey(kAbc, kCancel));
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == FindKey(kAbc, kCancel));
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == FindKey(kAbc, kSubmit));
        CHECK(d.Press(kKeyA) == Result::kSubmit); // (A on Send, with text)
    }
    {
        // the Circle Pad is the D-pad
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Press(kCpadDown);
        d.Press(kCpadDown);
        CHECK(d.Snap().selected == CharKey(kAbc, 'a'));
        d.Press(kCpadRight);
        CHECK(d.Snap().selected == CharKey(kAbc, 's'));
        d.Press(kCpadLeft);
        d.Press(kCpadUp);
        CHECK(d.Snap().selected == CharKey(kAbc, 'q'));
    }
    {
        // the Symbol page: the tall enter is reached from its three rows and leaves to the row under it; the empty cells are skipped
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Press(kKeyR);
        CHECK(k.page() == kSymbol);
        d.Press(kKeyRight); // (shows: q is not on this page, so the nearest key of Symbol)
        const u8 shown = d.Snap().selected;
        CHECK(shown != kNoKey && OnPage(Key(shown), kSymbol));
        const u8 star = CharKey(kSymbol, '*');
        u8 sel = shown;
        for (int i = 0; i < 20 && sel != star; ++i) { // (to '*' by the pad: right along the row, down a row)
            d.Press(Key(sel).y < Key(star).y ? kKeyDown : kKeyRight);
            sel = d.Snap().selected;
        }
        CHECK(sel == star);
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == FindKey(kSymbol, kEnter));
        d.Press(kKeyDown);
        CHECK(d.Snap().selected == FindKey(kSymbol, kSpace));
        d.Press(kKeyUp); // (back to the enter: the column kept)
        CHECK(d.Snap().selected == FindKey(kSymbol, kEnter));
        d.Press(kKeyLeft); // (the enter's middle row: '~')
        CHECK(d.Snap().selected == CharKey(kSymbol, '~'));
        d.Press(kKeyDown); // (the cell under '~' is empty: the nearest key of the row is '|')
        CHECK(d.Snap().selected == CharKey(kSymbol, '|'));
        d.Press(kKeyRight); // (past '|' the empty cells, then the enter that is three rows tall)
        CHECK(d.Snap().selected == FindKey(kSymbol, kEnter));
        for (int i = 0; i < 5; ++i) {
            d.Press(kKeyLeft);
        }
        CHECK(d.Snap().selected == CharKey(kSymbol, '+'));
        d.Press(kKeyLeft); // (round the row: the enter again)
        CHECK(d.Snap().selected == FindKey(kSymbol, kEnter));
        d.Press(kKeyRight);
        CHECK(d.Snap().selected == CharKey(kSymbol, '+'));
        d.Press(kKeyDown); // (from '+': the grey caps lock is not a key here, the space is)
        CHECK(d.Snap().selected == FindKey(kSymbol, kSpace));
    }
}

static void TestRepeat() {
    {
        // A held on a letter: one at once, then at 400, 490, 580, ... (looked at every 10 ms)
        Osk k(Chat());
        Driver d(k);
        d.step = 10;
        d.Feed(0);
        d.Press(kKeyRight); // (shows q)
        const u32 t0 = d.now;
        while (d.now - t0 < 1000) {
            d.Feed(kKeyA);
        }
        d.Feed(0);
        // at 0, 400, 490, 580, 670, 760, 850, 940
        CHECK(Text(k) == std::string(8, 'q'));
    }
    {
        // looked at 30 times a second: the same rhythm, never a burst
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Press(kKeyRight);
        d.Press(kKeyRight); // (w)
        const u32 t0 = d.now;
        u32 max_jump = 0;
        u32 last_len = 0;
        while (d.now - t0 < 2000) {
            d.Feed(kKeyA);
            const u32 grew = k.length() - last_len;
            max_jump = grew > max_jump ? grew : max_jump;
            last_len = k.length();
        }
        d.Feed(0);
        CHECK(max_jump == 1);
        CHECK(k.length() >= 17 && k.length() <= 19); // (1 + (2000 - 400) / 90 = 18, with the 33 ms of a look)
    }
    {
        // the directions repeat too, and on a clock that wraps
        Osk k(Chat());
        Driver d(k, 0xFFFFFF00u);
        d.step = 10;
        d.Feed(0);
        d.Press(kKeyDown); // (shows q)
        CHECK(d.Snap().selected == CharKey(kAbc, 'q'));
        const u32 t0 = d.now;
        while (d.now - t0 < 500) {
            d.Feed(kKeyRight);
        }
        d.Feed(0);
        // at 0, 400, 490: three moves
        CHECK(d.Snap().selected == CharKey(kAbc, 'r'));
    }
    {
        // B held deletes the text and stops at nothing; only a new press on no text closes
        Osk k(Chat(), "abcdef");
        Driver d(k);
        d.step = 10;
        d.Feed(0);
        const u32 t0 = d.now;
        Result r = Result::kNone;
        while (d.now - t0 < 1500 && r == Result::kNone) {
            r = d.Feed(kKeyB);
        }
        CHECK(r == Result::kNone);
        CHECK(Text(k).empty());
        d.Feed(0);
        CHECK(d.Press(kKeyB) == Result::kCancel);
    }
    {
        // an A that only shows the selection does not type when it is held (it would repeat q); pressed again, it types and repeats
        Osk k(Chat());
        Driver d(k);
        d.step = 10;
        d.Feed(0);
        u32 t0 = d.now;
        while (d.now - t0 < 1000) {
            d.Feed(kKeyA);
        }
        d.Feed(0);
        CHECK(d.Snap().selected == CharKey(kAbc, 'q'));
        CHECK(Text(k).empty());
        t0 = d.now;
        while (d.now - t0 < 450) {
            d.Feed(kKeyA);
        }
        d.Feed(0);
        CHECK(Text(k) == "qq"); // (at 0 and 400)
    }
    {
        // a finger on a key when R changes the page: lifted, no key (the key under it was of the page no longer shown)
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        const u8 q = CharKey(kAbc, 'q');
        d.Feed(0, true, Key(q).x + 5, Key(q).y + 5);
        d.Feed(kKeyR, true, Key(q).x + 5, Key(q).y + 5);
        CHECK(k.page() == kSymbol);
        CHECK(d.Snap().pressed != q);
        d.Feed(0, false);
        CHECK(Text(k).empty());
        d.Tap(CharKey(kSymbol, '#')); // (the next tap is a key again)
        CHECK(Text(k) == "#");
    }
}

static void TestShiftCapsPages() {
    Osk k(Chat());
    Driver d(k);
    d.Feed(0);
    d.Press(kKeyL);
    CHECK(d.Snap().shift == 1);
    CHECK(d.Snap().pressed == FindKey(kAbc, kShift));
    d.Type("ab");
    CHECK(Text(k) == "Ab"); // (shift for one character)
    d.Tap(FindKey(kAbc, kShift));
    d.Type(" x");
    CHECK(Text(k) == "Ab x"); // (the space took the shift)
    d.Tap(FindKey(kAbc, kCaps));
    CHECK(d.Snap().caps == 1);
    d.Type("cd1");
    CHECK(Text(k) == "Ab xCD1");
    d.Press(kKeyL);
    d.Type("ef");
    CHECK(Text(k) == "Ab xCD1eF"); // (shift with caps lock: lower case, once)
    d.Tap(FindKey(kAbc, kCaps));
    d.Type("g");
    CHECK(Text(k) == "Ab xCD1eFg");
    // R and the tabs
    d.Press(kKeyR);
    CHECK(k.page() == kSymbol);
    CHECK(d.Snap().pressed == FindKey(kSymbol, kTabSymbol));
    d.Press(kKeyL); // (no shift on Symbol)
    CHECK(d.Snap().shift == 0);
    d.Tap(FindKey(kSymbol, kCaps)); // (grey: nothing)
    CHECK(d.Snap().caps == 0);
    d.Tap(CharKey(kSymbol, '#'));
    d.Tap(FindKey(kSymbol, kTabAbc));
    CHECK(k.page() == kAbc);
    d.Press(kKeyR);
    d.Press(kKeyR);
    CHECK(k.page() == kAbc);
    CHECK(Text(k) == "Ab xCD1eFg#");
    // backspace keys of both pages
    d.Tap(FindKey(kAbc, kBackspace));
    d.Press(kKeyR);
    d.Tap(FindKey(kSymbol, kBackspace));
    CHECK(Text(k) == "Ab xCD1eF");
}

static void TestCloseAndLimits() {
    {
        // START sends only with text; SELECT closes and the draft stays; enter sends; Cancel closes
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        CHECK(d.Press(kKeyStart) == Result::kNone);
        CHECK(d.Snap().submit_on == 0);
        CHECK(d.Tap(FindKey(kAbc, kSubmit)) == Result::kNone);
        CHECK(d.Tap(FindKey(kAbc, kEnter)) == Result::kNone);
        d.Type("hi");
        CHECK(d.Snap().submit_on == 1);
        CHECK(d.Press(kKeySelect) == Result::kCancel);
        CHECK(Text(k) == "hi");
        CHECK(d.Press(kKeyStart) == Result::kSubmit);
        CHECK(d.Tap(FindKey(kAbc, kEnter)) == Result::kSubmit);
        d.Press(kKeyR);
        CHECK(d.Tap(FindKey(kSymbol, kEnter)) == Result::kSubmit);
        CHECK(d.Tap(FindKey(kSymbol, kCancel)) == Result::kCancel);
        CHECK(Text(k) == "hi");
    }
    {
        Options o = Chat();
        o.allow_empty = true;
        Osk k(o);
        Driver d(k);
        d.Feed(0);
        CHECK(d.Press(kKeyStart) == Result::kSubmit);
    }
    {
        // the limits: the password 64, the chat 128, nothing past them
        Osk p(Password());
        Driver d(p);
        d.Feed(0);
        for (int i = 0; i < 70; ++i) {
            d.Tap(CharKey(kAbc, 'k'));
        }
        CHECK(p.length() == 64);
        CHECK(d.Snap().at_max == 1 && d.Snap().max_len == 64 && d.Snap().len == 64);
        Osk c(Chat());
        Driver e(c);
        e.Feed(0);
        for (int i = 0; i < 140; ++i) {
            e.Tap(CharKey(kAbc, 'm'));
        }
        CHECK(c.length() == 128);
    }
    {
        // a draft: what is not printable ASCII is left out, what is past the limit too
        Osk k(Password(), "p\xC3\xA9ss\tword\x7F!");
        CHECK(Text(k) == "pssword!");
        const std::string longer(200, 'z');
        Osk c(Chat(), longer.c_str());
        CHECK(c.length() == 128);
        Options big;
        big.max_len = 1000;
        Osk b(big, longer.c_str());
        CHECK(b.length() == kTextMax);
    }
    {
        // a key held when the keyboard opens (the chord that opened it, A of the menu) counts only once let go
        Osk k(Chat());
        Driver d(k);
        d.Feed(kKeyA | kKeyB | kKeyStart | kKeySelect | kKeyL | kKeyLeft);
        d.Feed(kKeyA | kKeyB | kKeyStart | kKeySelect | kKeyL | kKeyLeft);
        CHECK(d.Snap().selected == kNoKey);
        CHECK(d.Snap().shift == 0);
        d.Feed(0);
        CHECK(d.Press(kKeyB) == Result::kCancel);
    }
}

// The chat's keyboard stays open after Send (the user's request, 2026-11-08): Clear empties the text and leaves the rest as it is (the page,
// caps lock, the selection), the count and Send go back to nothing; SetText puts a refused message back (cleaned, cut at the limit). The key
// that closes says "Close" for the chat and "Cancel" for the room's password, and closes the same way (the host keeps the draft).
static void TestClearAndClose() {
    {
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        d.Type("hello");
        d.Press(kKeyR); // (the Symbol page)
        d.Feed(kKeyRight);
        d.Feed(0);
        const View before = d.Snap();
        CHECK(d.Press(kKeyStart) == Result::kSubmit);
        k.Clear();
        const View v = d.Snap();
        CHECK(Text(k) == "" && k.length() == 0);
        CHECK(v.len == 0 && v.text_len[0] == 0 && v.submit_on == 0 && v.cursor_row == 0 && v.cursor_col == 0);
        CHECK(v.page == kSymbol && v.page == before.page && v.selected == before.selected); // (the keyboard as it was)
        CHECK(d.Press(kKeyStart) == Result::kNone); // (nothing to send)
        d.Press(kKeyR);
        d.Type("again");
        CHECK(Text(k) == "again");
        CHECK(d.Press(kKeyStart) == Result::kSubmit);
        // a long text scrolled: cleared, the cursor's line is shown again
        k.Clear();
        for (int i = 0; i < 100; ++i) {
            d.Tap(CharKey(kAbc, 'q'));
        }
        d.Tap(FindKey(kAbc, kScrollUp));
        k.Clear();
        CHECK(d.Snap().cursor_row == 0 && d.Snap().can_up == 0 && d.Snap().can_down == 0);
        // a refused message back: cleaned and cut like a draft
        k.SetText("not sent\x01 \xC3\xA9");
        CHECK(Text(k) == "not sent ");
        const std::string longer(200, 'r');
        k.SetText(longer.c_str());
        CHECK(k.length() == kTextMax);
        k.SetText(nullptr);
        CHECK(k.length() == 0);
    }
    {
        // the labels: "Close" for the chat, "Cancel" for the password and by default; the key closes in both
        Osk c(Chat());
        Osk p(Password());
        Osk n{Options{}};
        View vc, vp, vn;
        c.Snapshot(&vc);
        p.Snapshot(&vp);
        n.Snapshot(&vn);
        CHECK(std::string(vc.cancel) == "Close" && std::string(vp.cancel) == "Cancel" && std::string(vn.cancel) == "Cancel");
        Driver d(c);
        d.Feed(0);
        d.Type("x");
        CHECK(d.Tap(FindKey(kAbc, kCancel)) == Result::kCancel);
        CHECK(Text(c) == "x"); // (closing keeps the text: the host keeps it as the draft)
        Options o = Chat();
        o.cancel = "A label too long";
        Osk t(o);
        View vt;
        t.Snapshot(&vt);
        CHECK(std::strlen(vt.cancel) == kSubmitMax);
    }
}

static void TestMaskAndScroll() {
    {
        Osk k(Password(), "abc");
        Driver d(k);
        d.Feed(0);
        View v = d.Snap();
        CHECK(std::string(v.text[0], v.text_len[0]) == "***"); // (the kept password: masked)
        CHECK(std::string(v.title) == "Room password" && std::string(v.submit) == "OK");
        d.Type("d");
        v = d.Snap();
        CHECK(std::string(v.text[0], v.text_len[0]) == "***d"); // (the last one, just typed)
        d.now += kRevealMs - 100;
        d.Feed(0);
        CHECK(std::string(d.Snap().text[0], 4) == "***d");
        d.now += 100;
        d.Feed(0);
        CHECK(std::string(d.Snap().text[0], 4) == "****"); // (1 s after it was typed)
        d.Type("e");
        d.Press(kKeyB);
        CHECK(std::string(d.Snap().text[0], d.Snap().text_len[0]) == "****"); // (deleting shows none)
        CHECK(Text(k) == "abcd");
    }
    {
        // 100 characters: 3 lines, the last two shown (the cursor's); up shows the first two, down comes back; typing follows the cursor
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        for (int i = 0; i < 100; ++i) {
            d.Tap(CharKey(kAbc, static_cast<char>('a' + i % 26)));
        }
        View v = d.Snap();
        CHECK(v.text_len[0] == kCols && v.text_len[1] == 100 - kCols * 2);
        CHECK(v.text[0][0] == static_cast<char>('a' + kCols % 26));
        CHECK(v.cursor_row == 1 && v.cursor_col == 100 - kCols * 2);
        CHECK(v.can_up == 1 && v.can_down == 0);
        d.Tap(FindKey(kAbc, kScrollUp));
        v = d.Snap();
        CHECK(v.text[0][0] == 'a' && v.text_len[1] == kCols && v.cursor_row == kRows);
        CHECK(v.can_up == 0 && v.can_down == 1);
        d.Tap(FindKey(kAbc, kScrollUp)); // (no further)
        CHECK(d.Snap().text[0][0] == 'a');
        d.Tap(FindKey(kAbc, kScrollDown));
        CHECK(d.Snap().cursor_row == 1 && d.Snap().can_down == 0);
        d.Tap(FindKey(kAbc, kScrollUp));
        d.Tap(CharKey(kAbc, 'z'));
        CHECK(d.Snap().cursor_row == 1); // (typing brings the cursor back)
        // the scroll keys are touch only: the pad never selects them
        for (int i = 0; i < 10; ++i) {
            d.Press(kKeyUp);
            d.Press(kKeyRight);
            const u8 s = d.Snap().selected;
            CHECK(Key(s).kind != kScrollUp && Key(s).kind != kScrollDown);
        }
    }
    {
        // the cursor blinks
        Osk k(Chat());
        Driver d(k);
        d.step = 50;
        d.Feed(0);
        d.Type("a");
        const u8 on = d.Snap().cursor_on;
        CHECK(on == 1);
        d.now += kBlinkMs;
        d.Feed(0);
        CHECK(d.Snap().cursor_on == 0);
        d.Type("b");
        CHECK(d.Snap().cursor_on == 1); // (typing shows it at once)
    }
}

// No allocation once the keyboard is made: a long session of looks, snapshots and drawings.
static void TestNoAllocation() {
    Osk* k = new (std::nothrow) Osk(Chat(), "draft");
    CHECK(k != nullptr);
    std::vector<u8> fb(320 * 240 * 3 + 64);
    gfx::Surface s;
    s.left = fb.data();
    s.stride = 240 * 3;
    s.bpp = 3;
    s.format = gfx::kBGR8;
    s.w = 320;
    s.h = 240;
    Driver d(*k);
    const unsigned before = g_allocs.load();
    d.Feed(0);
    d.Type("no heap here ");
    for (int i = 0; i < 50; ++i) {
        d.Press(i % 3 == 0 ? kKeyRight : i % 3 == 1 ? kKeyDown : kKeyA);
        d.Press(kKeyR);
        View v = d.Snap();
        DrawOsk(s, v);
    }
    d.Press(kKeyB);
    d.Press(kKeyL);
    CHECK(g_allocs.load() == before);
    delete k;
}

// ---------------------------------------------------------------------------------------------
// The drawing
// ---------------------------------------------------------------------------------------------

static unsigned Bpp(unsigned format) {
    return format == 0 ? 4 : format == 1 ? 3 : 2;
}

static void Encode(Rgb c, unsigned format, u8* out) {
    unsigned v = 0;
    switch (format) {
    case 0:
        out[0] = 0xFF, out[1] = c.b, out[2] = c.g, out[3] = c.r;
        return;
    case 1:
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

constexpr unsigned kGuard = 64;

// The bottom screen as the framework hands it: 320 columns of 240 rows, guard bytes around.
struct Fb {
    unsigned format, bpp;
    std::vector<u8> mem;
    explicit Fb(unsigned fmt) : format(fmt), bpp(Bpp(fmt)) {
        mem.assign(kGuard * 2 + 320u * 240u * bpp, 0);
        for (std::size_t i = 0; i < mem.size(); ++i) {
            mem[i] = static_cast<u8>(0xA5 ^ (i * 7));
        }
        if (fmt == 0) {
            for (std::size_t p = 0; p < 320u * 240u; ++p) {
                mem[kGuard + p * 4] = 0; // (alpha 0: a pixel never written; the drawing writes 0xFF)
            }
        }
    }
    gfx::Surface Surface() {
        gfx::Surface s;
        s.left = mem.data() + kGuard;
        s.stride = 240 * bpp;
        s.bpp = static_cast<u8>(bpp);
        s.format = static_cast<u8>(format);
        s.w = 320;
        s.h = 240;
        return s;
    }
    std::size_t Offset(unsigned x, unsigned y) const {
        return kGuard + (static_cast<std::size_t>(x) * 240 + (239 - y)) * bpp;
    }
    Rgb At(unsigned x, unsigned y) const {
        return Decode(&mem[Offset(x, y)], format);
    }
};

static std::string g_out;

static void WriteBmp(const Fb& fb, const std::string& name) {
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
    const unsigned w = 320, h = 240, row = w * 3, size = 54 + row * h;
    u8 head[54] = {'B', 'M'};
    const auto put32 = [&](int at, unsigned v) {
        for (int i = 0; i < 4; ++i) {
            head[at + i] = static_cast<u8>(v >> (8 * i));
        }
    };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    head[26] = 1;
    head[28] = 24;
    put32(34, row * h);
    std::fwrite(head, 1, sizeof(head), f);
    std::vector<u8> line(row);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (unsigned x = 0; x < w; ++x) {
            const Rgb c = fb.At(x, static_cast<unsigned>(y));
            line[x * 3] = c.b, line[x * 3 + 1] = c.g, line[x * 3 + 2] = c.r;
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

static void ResetStores() {
    std::memset(g_stores, 0, sizeof(g_stores));
}
static unsigned long StoredBytes() {
    return g_stores[1] + g_stores[2] * 2 + g_stores[4] * 4;
}

static bool SameRgb(Rgb a, Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// A view of the chat with some text, from a real Osk.
static View ChatView(const char* text, u32 page = kAbc) {
    Osk k(Chat(), text);
    Driver d(k);
    d.Feed(0);
    if (page == kSymbol) {
        d.Press(kKeyR);
    }
    d.now += kFlashMs; // (the R that turned the page no longer lit)
    d.Feed(0);
    return d.Snap();
}

static void TestDrawing() {
    // every pixel of the screen written once at least, nothing outside it, the same picture in every format
    const View v = ChatView("Hello everyone!");
    Fb ref(gfx::kRGBA8);
    ResetStores();
    DrawOsk(ref.Surface(), v);
    const unsigned long ref_bytes = StoredBytes();
    int unwritten = 0;
    for (std::size_t p = 0; p < 320u * 240u; ++p) {
        unwritten += ref.mem[kGuard + p * 4] != 0xFF;
    }
    CHECK(unwritten == 0);
    std::printf("  a whole screen: rgba8 %lu bytes stored (%.2f times the screen)", ref_bytes, ref_bytes / (320.0 * 240 * 4));
    CHECK(ref_bytes <= 320ul * 240 * 4 * 115 / 100); // (the keys once, the text area and the bar a little more)
    for (unsigned f = 1; f < gfx::kFormats; ++f) {
        Fb fb(f);
        const std::vector<u8> before = fb.mem;
        ResetStores();
        DrawOsk(fb.Surface(), v);
        std::printf(", %s %lu", kFormatNames[f], StoredBytes());
        CHECK(StoredBytes() <= 320ul * 240 * fb.bpp * 115 / 100);
        bool guards = true;
        for (unsigned i = 0; i < kGuard; ++i) {
            guards = guards && fb.mem[i] == before[i] && fb.mem[fb.mem.size() - 1 - i] == before[fb.mem.size() - 1 - i];
        }
        CHECK(guards);
        int diff = 0;
        for (unsigned x = 0; x < 320; ++x) {
            for (unsigned y = 0; y < 240; ++y) {
                u8 want[4];
                Encode(ref.At(x, y), f, want);
                diff += !SameRgb(fb.At(x, y), Decode(want, f));
            }
        }
        CHECK(diff == 0);
    }
    std::printf("\n");
    // a surface too small, or one the layer refuses: nothing
    {
        Fb fb(gfx::kRGB565);
        const std::vector<u8> before = fb.mem;
        gfx::Surface s = fb.Surface();
        s.w = 319;
        DrawOsk(s, v);
        s = fb.Surface();
        s.bpp = 3;
        DrawOsk(s, v);
        CHECK(fb.mem == before);
    }
    // What is seen is what is touched: for every key of both pages, the pixels that change when the key is lit (or selected) are all
    // inside its rectangle of the table, and its inside does change.
    for (u32 page = 0; page < 2; ++page) {
        View base = ChatView("abc", page);
        base.can_up = base.can_down = 1; // (so that the scroll keys can be lit)
        Fb plain(gfx::kRGB565);
        DrawOsk(plain.Surface(), base);
        int outside = 0, unseen = 0;
        for (u32 i = 0; i < kKeyCount; ++i) {
            const KeyDef& k = kKeys[i];
            if (!Live(k, page)) {
                continue;
            }
            for (int how = 0; how < 2; ++how) {
                View lit = base;
                (how == 0 ? lit.pressed : lit.selected) = static_cast<u8>(i);
                Fb fb(gfx::kRGB565);
                DrawOsk(fb.Surface(), lit);
                int changed = 0;
                for (unsigned x = 0; x < 320; ++x) {
                    for (unsigned y = 0; y < 240; ++y) {
                        if (!SameRgb(fb.At(x, y), plain.At(x, y))) {
                            ++changed;
                            outside += HitTest(page, static_cast<int>(x), static_cast<int>(y)) != static_cast<u8>(i);
                        }
                    }
                }
                // (the active tab and Send already look lit: there only the selection changes them)
                const bool looks_lit = (k.kind == kTabAbc && page == kAbc) || (k.kind == kTabSymbol && page == kSymbol) || k.kind == kSubmit;
                if (!(how == 0 && looks_lit)) {
                    unseen += changed < (k.w - 2) * (k.h - 2) / 2;
                }
            }
        }
        CHECK(outside == 0);
        CHECK(unseen == 0);
    }
    // the look of a few things: the line between two keys, a key's top edge and fill, the green of a function key, Send off and on, the
    // active tab, the dotted lines, the cursor
    {
        View e = ChatView("");
        Fb fb(gfx::kRGBA8);
        DrawOsk(fb.Surface(), e);
        const KeyDef& q = Key(CharKey(kAbc, 'q'));
        CHECK(SameRgb(fb.At(q.x, q.y + 5), kLineDark));
        CHECK(SameRgb(fb.At(q.x + 3, q.y), kLineDark));
        CHECK(SameRgb(fb.At(q.x + 3, q.y + 1), kLookKey.edge));
        CHECK(SameRgb(fb.At(q.x + 2, q.y + q.h - 1), kLookKey.fill));
        const KeyDef& bs = Key(FindKey(kAbc, kBackspace));
        CHECK(SameRgb(fb.At(bs.x + 2, bs.y + bs.h - 1), kGreen));
        const KeyDef& send = Key(FindKey(kAbc, kSubmit));
        CHECK(SameRgb(fb.At(send.x + 3, send.y + send.h - 1), kLookSubmitOff.fill));
        const KeyDef& tab = Key(FindKey(kAbc, kTabAbc));
        CHECK(SameRgb(fb.At(tab.x + 3, tab.y + tab.h - 1), kAccent));
        CHECK(SameRgb(fb.At(kTextX - 2, kDotY[0]), kDots) && SameRgb(fb.At(kTextX + 1, kDotY[0]), kField));
        CHECK(SameRgb(fb.At(kTextX, kTextY[0] + 4), kAccent)); // (the cursor at the start)
        CHECK(SameRgb(fb.At(kFrameX1[kAbc], 120), kLineDark) && SameRgb(fb.At(100, kFrameBottom), kLineDark));
        View t = ChatView("x");
        DrawOsk(fb.Surface(), t);
        CHECK(SameRgb(fb.At(send.x + 3, send.y + send.h - 1), kAccent));
        // the selected key's outline, the pressed key's fill
        View s = t;
        s.selected = CharKey(kAbc, 'g');
        s.pressed = CharKey(kAbc, 'h');
        DrawOsk(fb.Surface(), s);
        const KeyDef& g = Key(s.selected);
        const KeyDef& h = Key(s.pressed);
        CHECK(SameRgb(fb.At(g.x + 1, g.y + 10), kAccent) && SameRgb(fb.At(g.x + 3, g.y + g.h - 2), kSel));
        CHECK(SameRgb(fb.At(h.x + 3, h.y + h.h - 2), kAccent));
    }
}

// The keyboard drawn again over what it showed (DrawOskOver, the chat's Image on the plugin's thread): every byte as a whole new drawing,
// along a keyboard used at random (the pad, taps, shift, caps lock, the pages, scrolling, Cancel and Send, the blink as time goes by) and over
// views changed at random field by field; and the blink, a key moved or a character typed write a small part of what the whole screen does.
static u32 g_seed = 12345;
static u32 Rand(u32 n) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) % n;
}

static void TestDrawOver() {
    int wrong = 0, steps = 0, blinks = 0, small_blinks = 0;
    unsigned long over_bytes = 0, whole_bytes = 0;
    for (unsigned f = 0; f < gfx::kFormats; ++f) {
        Fb inc(f), whole(f);
        Osk k(f % 2 ? Password() : Chat(), "Hi all");
        Driver d(k);
        d.Feed(0);
        View was = d.Snap();
        DrawOsk(inc.Surface(), was);
        const unsigned long screen = 320ul * 240 * inc.bpp;
        for (int step = 0; step < (f == gfx::kRGB565 ? 900 : 300); ++step) {
            const u32 what = Rand(16);
            if (what < 4) {
                d.Feed(0); // (time goes by: the blink, the flash of a key, a password's last character)
            } else if (what < 6) {
                d.now += 100 + Rand(600);
                d.Feed(0);
            } else if (what < 9) {
                const u32 dirs[4] = {kKeyUp, kKeyDown, kKeyLeft, kKeyRight};
                d.Feed(dirs[Rand(4)]);
            } else if (what < 11) {
                d.Feed(kKeyA); // (types the selected key, or holds it: the repeat)
            } else if (what == 11) {
                const u32 other[4] = {kKeyB, kKeyL, kKeyR, kKeyStart};
                d.Feed(other[Rand(4)]);
            } else {
                const u8 key = static_cast<u8>(Rand(kKeyCount));
                const KeyDef& kd = Key(key);
                d.Feed(0, true, kd.x + 1 + static_cast<int>(Rand(kd.w - 1)), kd.y + 1 + static_cast<int>(Rand(kd.h - 1)));
                if (Rand(2)) {
                    d.Feed(0, false);
                }
            }
            const View v = d.Snap();
            View blink = was;
            blink.cursor_on = v.cursor_on;
            const bool only_blink = !Same(v, was) && Same(v, blink);
            ResetStores();
            DrawOskOver(inc.Surface(), v, was);
            const unsigned long stored = StoredBytes();
            ResetStores();
            DrawOsk(whole.Surface(), v);
            over_bytes += stored;
            whole_bytes += StoredBytes();
            wrong += inc.mem != whole.mem;
            if (only_blink) {
                ++blinks;
                small_blinks += stored <= screen / 12; // (one line of the text: 280 x 16 pixels, a 17th of the screen)
            }
            was = v;
            ++steps;
        }
    }
    std::printf("  drawn over: %d steps, %lu bytes stored (a whole screen every step: %lu), %d blinks\n", steps, over_bytes, whole_bytes, blinks);
    CHECK(wrong == 0);
    CHECK(blinks > 20 && small_blinks == blinks);
    CHECK(over_bytes * 4 < whole_bytes);
    // views changed at random, field by field
    wrong = 0;
    for (int t = 0; t < 400; ++t) {
        const unsigned f = static_cast<unsigned>(t) % gfx::kFormats;
        View was = ChatView(t % 3 ? "Ready when you are" : "", t % 2 ? kAbc : kSymbol);
        View v = was;
        for (u32 n = 1 + Rand(3); n > 0; --n) {
            switch (Rand(15)) {
            case 0: {
                const u32 r = Rand(kRows);
                v.text_len[r] = static_cast<u8>(Rand(kCols + 1));
                for (u32 c = 0; c < v.text_len[r]; ++c) {
                    v.text[r][c] = static_cast<char>(0x20 + Rand(0x5F));
                }
                break;
            }
            case 1:
                v.cursor_row = static_cast<u8>(Rand(kRows + 1));
                break;
            case 2:
                v.cursor_col = static_cast<u8>(Rand(kCols));
                break;
            case 3:
                v.cursor_on ^= 1;
                break;
            case 4:
                v.page ^= 1;
                break;
            case 5:
                v.shift ^= 1;
                break;
            case 6:
                v.caps ^= 1;
                break;
            case 7:
                v.selected = Rand(4) ? static_cast<u8>(Rand(kKeyCount)) : kNoKey;
                break;
            case 8:
                v.pressed = Rand(4) ? static_cast<u8>(Rand(kKeyCount)) : kNoKey;
                break;
            case 9:
                v.can_up ^= 1;
                v.can_down = static_cast<u8>(Rand(2));
                break;
            case 10:
                v.submit_on ^= 1;
                break;
            case 11:
                v.at_max ^= 1;
                break;
            case 12:
                v.len = static_cast<u16>(Rand(kTextMax + 1));
                break;
            case 13:
                v.cancel[0] = static_cast<char>(v.cancel[0] == 'C' ? 'X' : 'C');
                break;
            default:
                v.submit[0] = static_cast<char>(v.submit[0] == 'S' ? 'O' : 'S');
                break;
            }
        }
        Fb inc(f), whole(f);
        DrawOsk(inc.Surface(), was);
        DrawOskOver(inc.Surface(), v, was);
        DrawOsk(whole.Surface(), v);
        wrong += inc.mem != whole.mem;
    }
    CHECK(wrong == 0);
}

// The pictures for the user.
static void Pictures() {
    struct Shot {
        const char* name;
        View v;
    };
    std::vector<Shot> shots;
    shots.push_back({"osk-abc", ChatView("Hello everyone! Ready for a match?")});
    {
        View v = ChatView("Good game ");
        v.shift = 1;
        shots.push_back({"osk-abc-shift", v});
    }
    {
        View v = ChatView("GG, see you at 9 ");
        v.caps = 1;
        shots.push_back({"osk-abc-caps", v});
    }
    shots.push_back({"osk-symbol", ChatView("Score: 3-1 (rematch?) ", kSymbol)});
    {
        // the D-pad's selection, from a real Osk
        Osk k(Chat(), "On my way");
        Driver d(k);
        d.Feed(0);
        d.Press(kKeyDown);
        d.Press(kKeyDown);
        d.Press(kKeyRight);
        d.Press(kKeyRight);
        d.Press(kKeyRight);
        d.Press(kKeyRight);
        d.now += kFlashMs;
        d.Feed(0);
        shots.push_back({"osk-selected", d.Snap()});
    }
    {
        // a finger on 'h'
        Osk k(Chat(), "Hi everyone, h");
        Driver d(k);
        d.Feed(0);
        const KeyDef& h = Key(CharKey(kAbc, 'h'));
        d.Feed(0, true, h.x + 10, h.y + 10);
        shots.push_back({"osk-pressed", d.Snap()});
    }
    {
        // the chat with a long text: three lines, the last two shown, the count near the limit
        std::string text = "Anyone up for another round after this one? I will host the next lobby, join when you see it in the list";
        View v = ChatView(text.c_str());
        shots.push_back({"osk-chat-text-counter", v});
    }
    {
        Osk k(Chat());
        Driver d(k);
        d.Feed(0);
        shots.push_back({"osk-chat-empty", d.Snap()});
    }
    {
        // the password: masked, the last character just typed visible; then 1 s later all masked
        Osk k(Password(), "secret");
        Driver d(k);
        d.Feed(0);
        d.Type("7");
        shots.push_back({"osk-password-last-visible", d.Snap()});
        d.now += kRevealMs;
        d.Feed(0);
        shots.push_back({"osk-password-masked", d.Snap()});
    }
    for (const Shot& s : shots) {
        Fb fb(gfx::kBGR8);
        DrawOsk(fb.Surface(), s.v);
        WriteBmp(fb, s.name);
    }
}

// Same, the guard of the redraw (Sources/ctr/chat_overlay.cpp's Redraw, Sources/game_menu.cpp's DrawKeyboard): two views of a keyboard
// nothing happened to are the same, so the picture is not drawn again; a view with any field changed is not, so nothing is missed. A
// keyboard left alone at 30 looks a second changes only with the blink: a host that draws on !Same draws 2 times a second, not 30.
static void TestSame() {
    Osk k(Chat(), "Hi all");
    Driver d(k);
    d.Feed(0);
    d.now += kFlashMs;
    d.Feed(0);
    const View v = d.Snap();
    CHECK(Same(v, v));
    CHECK(Same(v, d.Snap())); // (two snapshots, no look in between)
    d.step = 0;
    d.Feed(0); // (a look with nothing pressed, at the same time)
    CHECK(Same(v, d.Snap()));
    d.step = 33;
    // every field changed alone
    int differ = 0, fields = 0;
    for (int f = 0; f < 19; ++f) {
        View w = v;
        switch (f) {
        case 0: w.text[0][0] ^= 1; break;
        case 1: w.text[kRows - 1][kCols - 1] ^= 1; break;
        case 2: ++w.text_len[0]; break;
        case 3: ++w.cursor_row; break;
        case 4: ++w.cursor_col; break;
        case 5: w.cursor_on ^= 1; break;
        case 6: w.page ^= 1; break;
        case 7: w.shift ^= 1; break;
        case 8: w.caps ^= 1; break;
        case 9: ++w.selected; break;
        case 10: ++w.pressed; break;
        case 11: w.can_up ^= 1; break;
        case 12: w.can_down ^= 1; break;
        case 13: w.submit_on ^= 1; break;
        case 14: w.at_max ^= 1; break;
        case 15: ++w.len; break;
        case 16: w.title[0] ^= 1; break;
        case 17: w.cancel[0] ^= 1; break;
        default: w.submit[0] ^= 1; break;
        }
        ++fields;
        differ += !Same(v, w) && !Same(w, v);
    }
    CHECK(differ == fields);
    // what the keyboard does: a direction (the selection shown), a character typed, shift, the other page
    View was = d.Snap();
    d.Feed(kKeyRight);
    View now = d.Snap();
    CHECK(!Same(now, was) && now.selected != kNoKey);
    d.Feed(0);
    was = d.Snap();
    d.Tap(CharKey(kAbc, 'x'));
    now = d.Snap();
    CHECK(!Same(now, was) && now.len == was.len + 1);
    d.now += kFlashMs;
    d.Feed(0);
    was = d.Snap();
    d.Press(kKeyL);
    now = d.Snap();
    CHECK(!Same(now, was) && now.shift != was.shift);
    was = now;
    d.Press(kKeyR);
    now = d.Snap();
    CHECK(!Same(now, was) && now.page != was.page);
    d.Press(kKeyR);
    d.now += kFlashMs;
    d.Feed(0);
    // left alone for 3 s at 30 looks a second: drawn again only when the cursor blinks
    View drawn = d.Snap();
    int looks = 0, redraws = 0, blinks = 0;
    for (; looks < 90; ++looks) {
        d.Feed(0);
        const View cur = d.Snap();
        blinks += cur.cursor_on != drawn.cursor_on;
        if (!Same(cur, drawn)) {
            ++redraws;
            drawn = cur;
        }
    }
    std::printf("  same: %d of %d fields seen, %d redraws in %d idle looks (%d blinks)\n", differ, fields, redraws, looks, blinks);
    CHECK(redraws == blinks && blinks >= 5 && blinks <= 7);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        g_out = argv[1];
    }
    TestTable();
    TestTouch();
    TestPad();
    TestRepeat();
    TestShiftCapsPages();
    TestCloseAndLimits();
    TestClearAndClose();
    TestMaskAndScroll();
    TestNoAllocation();
    TestDrawing();
    TestDrawOver();
    TestSame();
    Pictures();
    std::printf("osk_test: %d checks, %d failed%s%s\n", g_checks, g_failures, g_out.empty() ? "" : "; pictures in ", g_out.c_str());
    return g_failures == 0 ? 0 : 1;
}
