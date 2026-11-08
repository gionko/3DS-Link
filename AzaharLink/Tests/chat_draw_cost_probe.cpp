// The chat's lines on the top screen (azl/chat_draw.hpp) built for the plugin's own target, with the plugin's compiler and flags, as a bare
// program that Tests/chat_draw_cost.py runs in an ARM emulator to count the instructions of one frame's drawing (Tests/chat_draw_cost.sh).
// Nothing of the 3DS: two framebuffers of the top screen in the plugin's layout (rotated columns of 240 rows) and a view of the chat.
// azl_cost_setup builds the frame, azl_cost_draw draws it: only the second is counted. The keyboard's copy onto the bottom screen
// (azl/chat_host.hpp, CopyImage) likewise: azl_copy_setup fills an Image and a framebuffer, azl_copy_run copies (counted), azl_copy_check
// says how many bytes are not what they must be (the visible rows the Image's, the rest of every column untouched).
#include "azl/chat_draw.hpp"
#include "azl/chat_host.hpp"
#include "azl/osk_draw.hpp"

using namespace azl;

// (the compiler may call these for a structure's copy or its zeroing; the probe has no C library)
extern "C" void* memset(void* d, int c, unsigned n) {
    unsigned char* p = static_cast<unsigned char*>(d);
    while (n--) {
        *p++ = static_cast<unsigned char>(c);
    }
    return d;
}
extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) unsigned strlen(const char* s) {
    unsigned n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}
extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) int memcmp(const void* a, const void* b, unsigned n) {
    const unsigned char* p = static_cast<const unsigned char*>(a);
    const unsigned char* q = static_cast<const unsigned char*>(b);
    for (; n--; ++p, ++q) {
        if (*p != *q) {
            return *p < *q ? -1 : 1;
        }
    }
    return 0;
}
extern "C" void* memcpy(void* d, const void* s, unsigned n) {
    unsigned char* p = static_cast<unsigned char*>(d);
    const unsigned char* q = static_cast<const unsigned char*>(s);
    while (n--) {
        *p++ = *q++;
    }
    return d;
}

static u8 g_left[400 * 240 * 4] __attribute__((aligned(8)));
static u8 g_right[400 * 240 * 4] __attribute__((aligned(8)));
static chat::View g_view;
static gfx::Surface g_surface;
static u32 g_lines;
static int g_corner;

/// The frame: `format` (gfx::Format), the 3D on or off, `lines` messages of `text_len` characters from a nick of `nick_len`, in `corner`.
extern "C" __attribute__((used, noinline)) void azl_cost_setup(u32 format, u32 three_d, u32 lines, u32 nick_len, u32 text_len, u32 corner) {
    const u32 bpp = gfx::BytesPerPixel(format);
    g_surface.left = g_left;
    g_surface.right = three_d ? g_right : nullptr;
    g_surface.stride = 240 * bpp;
    g_surface.bpp = static_cast<u8>(bpp);
    g_surface.format = static_cast<u8>(format);
    g_surface.w = 400;
    g_surface.h = 240;
    g_view.count = lines;
    for (u32 k = 0; k < lines; ++k) {
        chat::Line& l = g_view.lines[k];
        l.t_ms = 1;
        l.nick_len = static_cast<u8>(nick_len);
        for (u32 i = 0; i < nick_len; ++i) {
            l.nick[i] = static_cast<char>('A' + (i + k) % 26);
        }
        l.text_len = static_cast<u8>(text_len);
        for (u32 i = 0; i < text_len; ++i) {
            l.text[i] = i % 7 == 6 ? ' ' : static_cast<char>('a' + (i * 5 + k) % 26);
        }
    }
    g_lines = lines;
    g_corner = static_cast<int>(corner);
}

/// One frame's drawing, as the OSD callback does it: the blocks drawn.
extern "C" __attribute__((used, noinline)) u32 azl_cost_draw() {
    return chat::DrawChatLines(g_surface, g_view, g_lines, g_corner, 0);
}

static u8 g_image[320 * 240 * 4] __attribute__((aligned(8)));
static chat::Image g_im;
static gfx::Surface g_fb;

/// An Image of `format` with a pattern, and a framebuffer of the bottom screen of `rows` rows a column (240, or more: the copy skips the rest).
extern "C" __attribute__((used, noinline)) void azl_copy_setup(u32 format, u32 rows) {
    const u32 bpp = gfx::BytesPerPixel(format);
    g_im.pixels = g_image;
    g_im.col_bytes = 240 * bpp;
    g_im.format = static_cast<u8>(format);
    g_im.bpp = static_cast<u8>(bpp);
    for (u32 i = 0; i < 320 * g_im.col_bytes; ++i) {
        g_image[i] = static_cast<u8>(i * 7 + i / 251);
    }
    g_fb.left = g_left;
    g_fb.right = nullptr;
    g_fb.stride = rows * bpp;
    g_fb.bpp = static_cast<u8>(bpp);
    g_fb.format = static_cast<u8>(format);
    g_fb.w = 320;
    g_fb.h = 240;
    for (u32 i = 0; i < sizeof(g_left); ++i) {
        g_left[i] = 0xA5;
    }
}

/// One frame's copy, as the OSD callback does it (0 when the Image does not fit the framebuffer).
extern "C" __attribute__((used, noinline)) u32 azl_copy_run() {
    if (!chat::ImageFits(g_im, g_fb)) {
        return 0;
    }
    chat::CopyImage(g_im, g_fb);
    return 1;
}

/// The bytes of the framebuffer that are wrong after the copy.
extern "C" __attribute__((used, noinline)) u32 azl_copy_check() {
    u32 wrong = 0;
    for (u32 i = 0; i < sizeof(g_left); ++i) {
        const u32 x = i / g_fb.stride, in = i % g_fb.stride;
        const bool visible = x < 320 && in < g_im.col_bytes;
        wrong += g_left[i] != (visible ? g_image[x * g_im.col_bytes + in] : 0xA5);
    }
    return wrong;
}

// The keyboard's picture redrawn on the plugin's thread (azl/osk_draw.hpp): the whole screen, or over what it showed (DrawOskOver) after
// one change. azl_osk_setup draws `was` (not counted) and makes `now`, azl_osk_draw draws it (counted), azl_osk_check says how many bytes
// differ from a whole new drawing of `now`.
static osk::View g_was, g_now;
static u32 g_change;

static void OskSurface(gfx::Surface& s, u8* mem, u32 format) {
    const u32 bpp = gfx::BytesPerPixel(format);
    s.left = mem;
    s.right = nullptr;
    s.stride = 240 * bpp;
    s.bpp = static_cast<u8>(bpp);
    s.format = static_cast<u8>(format);
    s.w = 320;
    s.h = 240;
}

/// `change`: 0 the whole screen, 1 the blink, 2 the D-pad's key moved, 3 a character typed (the line, the count, the key lit), 4 the key
/// no longer lit, 5 shift (every letter's label).
extern "C" __attribute__((used, noinline)) void azl_osk_setup(u32 format, u32 change) {
    OskSurface(g_fb, g_left, format);
    memset(&g_was, 0, sizeof(g_was));
    const char* text = "Hello everyone! Ready";
    for (u32 i = 0; text[i]; ++i) {
        g_was.text[0][i] = text[i];
        g_was.text_len[0] = static_cast<u8>(i + 1);
    }
    g_was.cursor_col = g_was.text_len[0];
    g_was.cursor_on = 1;
    g_was.page = osk::kAbc;
    g_was.selected = osk::FindKey(osk::kAbc, osk::kChar, 'g');
    g_was.pressed = osk::kNoKey;
    g_was.submit_on = 1;
    g_was.len = g_was.text_len[0];
    g_was.max_len = 128;
    memcpy(g_was.title, "Chat", 5);
    memcpy(g_was.submit, "Send", 5);
    memcpy(g_was.cancel, "Close", 6);
    osk::DrawOsk(g_fb, g_was);
    g_now = g_was;
    g_change = change;
    if (change == 1) {
        g_now.cursor_on = 0;
    } else if (change == 2) {
        g_now.selected = osk::FindKey(osk::kAbc, osk::kChar, 'h');
    } else if (change == 3) {
        g_now.text[0][g_now.text_len[0]++] = 'g';
        ++g_now.cursor_col;
        ++g_now.len;
        g_now.pressed = g_now.selected;
    } else if (change == 4) {
        g_was.pressed = g_was.selected; // (drawn lit: the setup's drawing again)
        osk::DrawOsk(g_fb, g_was);
    } else if (change == 5) {
        g_now.shift = 1;
    }
}

extern "C" __attribute__((used, noinline)) void azl_osk_draw() {
    if (g_change == 0) {
        osk::DrawOsk(g_fb, g_now);
    } else {
        osk::DrawOskOver(g_fb, g_now, g_was);
    }
}

extern "C" __attribute__((used, noinline)) u32 azl_osk_check() {
    gfx::Surface whole;
    OskSurface(whole, g_right, g_fb.format);
    osk::DrawOsk(whole, g_now);
    u32 wrong = 0;
    for (u32 i = 0; i < 320 * 240 * g_fb.bpp; ++i) {
        wrong += g_left[i] != g_right[i];
    }
    return wrong;
}

extern "C" __attribute__((noreturn)) void _start() {
    for (;;) {
    }
}
