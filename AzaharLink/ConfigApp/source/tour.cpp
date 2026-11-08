#include "tour.hpp"

#ifdef AZL_TOUR

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace tour {

namespace {

struct Step {
    std::string cmd, arg;
};

std::vector<Step> g_steps;
std::size_t g_next = 0;
bool g_active = false;
int g_wait = 0;
u32 g_keys = 0;
std::string g_text;
bool g_has_text = false;
std::string g_shot;
int g_shot_frames = 0;
bool g_quit = false;
bool g_nofetch = false;
bool g_homeoff = false;
std::string g_open;

u32 KeyOf(const std::string& k) {
    static const struct {
        const char* name;
        u32 key;
    } kKeys[] = {{"A", KEY_A},         {"B", KEY_B},        {"X", KEY_X},       {"Y", KEY_Y},         {"L", KEY_L},          {"R", KEY_R},
                 {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT}};
    for (const auto& e : kKeys) {
        if (k == e.name) {
            return e.key;
        }
    }
    return 0;
}

// One pixel of a framebuffer (the screens are stored turned: a column of 240 pixels at a time, from the bottom up) as R, G, B.
void Pixel(const u8* fb, GSPGPU_FramebufferFormat format, int x, int y, u8* rgb) {
    const int index = x * 240 + (239 - y);
    switch (format) {
    case GSP_RGBA8_OES: {
        const u8* p = fb + index * 4;
        rgb[0] = p[3];
        rgb[1] = p[2];
        rgb[2] = p[1];
        break;
    }
    case GSP_BGR8_OES: {
        const u8* p = fb + index * 3;
        rgb[0] = p[2];
        rgb[1] = p[1];
        rgb[2] = p[0];
        break;
    }
    case GSP_RGB565_OES: {
        u16 v;
        std::memcpy(&v, fb + index * 2, 2);
        rgb[0] = static_cast<u8>(((v >> 11) & 0x1F) << 3);
        rgb[1] = static_cast<u8>(((v >> 5) & 0x3F) << 2);
        rgb[2] = static_cast<u8>((v & 0x1F) << 3);
        break;
    }
    default: {
        u16 v;
        std::memcpy(&v, fb + index * 2, 2);
        rgb[0] = static_cast<u8>(((v >> 11) & 0x1F) << 3);
        rgb[1] = static_cast<u8>(((v >> 6) & 0x1F) << 3);
        rgb[2] = static_cast<u8>(((v >> 1) & 0x1F) << 3);
        break;
    }
    }
}

void WriteScreen(gfxScreen_t screen, const std::string& path) {
    u16 fb_w = 0, fb_h = 0;
    const u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fb_w, &fb_h);
    if (!fb) {
        return;
    }
    const int width = fb_h, height = fb_w; // (turned: 400 or 320 by 240)
    const GSPGPU_FramebufferFormat format = gfxGetScreenFormat(screen);
    const int row_bytes = (width * 3 + 3) & ~3;
    std::vector<u8> bmp(54 + static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(height), 0);
    auto put32 = [&](std::size_t at, u32 v) { std::memcpy(&bmp[at], &v, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<u32>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<u32>(width));
    put32(22, static_cast<u32>(height));
    bmp[26] = 1;
    bmp[28] = 24;
    for (int y = 0; y < height; ++y) {
        u8* row = &bmp[54 + static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(row_bytes)];
        for (int x = 0; x < width; ++x) {
            u8 rgb[3];
            Pixel(fb, format, x, y, rgb);
            row[x * 3 + 0] = rgb[2];
            row[x * 3 + 1] = rgb[1];
            row[x * 3 + 2] = rgb[0];
        }
    }
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bmp.data(), 1, bmp.size(), f);
        std::fclose(f);
    }
}

} // namespace

bool Begin() {
    FILE* f = std::fopen("sdmc:/azl-tour.txt", "r");
    if (!f) {
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == '#') {
            continue;
        }
        const std::size_t space = s.find(' ');
        g_steps.push_back({s.substr(0, space), space == std::string::npos ? std::string() : s.substr(space + 1)});
    }
    std::fclose(f);
    mkdir("sdmc:/azl-shots", 0777);
    g_active = !g_steps.empty();
    return g_active;
}

bool Active() {
    return g_active;
}

u32 Keys() {
    g_keys = 0;
    if (!g_active || !g_shot.empty()) {
        return 0;
    }
    if (g_wait > 0) {
        --g_wait;
        return 0;
    }
    while (g_next < g_steps.size()) {
        const Step& s = g_steps[g_next++];
        if (s.cmd == "wait") {
            g_wait = std::atoi(s.arg.c_str());
            return 0;
        }
        if (s.cmd == "key") {
            g_keys = KeyOf(s.arg);
            g_wait = 1; // (a key is let go before the next one)
            return g_keys;
        }
        if (s.cmd == "text") {
            g_text = s.arg;
            g_has_text = true;
            continue;
        }
        if (s.cmd == "shot") {
            g_shot = s.arg;
            g_shot_frames = 0;
            return 0;
        }
        if (s.cmd == "open") {
            g_open = s.arg;
            return 0;
        }
        if (s.cmd == "nofetch") {
            g_nofetch = true;
            continue;
        }
        if (s.cmd == "homeoff") {
            g_homeoff = true;
            continue;
        }
        if (s.cmd == "quit") {
            g_quit = true;
            return 0;
        }
    }
    return 0;
}

bool TakeText(std::string& value) {
    if (!g_active || !g_has_text) {
        return false;
    }
    value = g_text;
    g_has_text = false;
    return true;
}

std::string TakeOpen() {
    std::string page;
    page.swap(g_open);
    return page;
}

bool SkipFetch() {
    return g_nofetch;
}

bool HomeOff() {
    return g_homeoff;
}

bool AfterFrame() {
    if (!g_shot.empty() && ++g_shot_frames >= 3) {
        WriteScreen(GFX_TOP, "sdmc:/azl-shots/" + g_shot + "-top.bmp");
        WriteScreen(GFX_BOTTOM, "sdmc:/azl-shots/" + g_shot + "-bottom.bmp");
        g_shot.clear();
    }
    return g_quit;
}

} // namespace tour

#endif
