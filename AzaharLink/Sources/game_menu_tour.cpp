#ifdef AZL_MENU_TOUR
// A test build only (make EXTRA_CFLAGS=-DAZL_MENU_TOUR; the whole file is inside this #ifdef, so a release has none of it, and
// Tests/policy_check.sh lets it write to the card for that reason): both screens of the in-game menu to
// sdmc:/azl-shots/plugin-NAME-top.bmp and -bottom.bmp, for its screenshots in Azahar (see Sources/game_menu.cpp, kTour).
#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cstdio>
#include <vector>

#include "ctr/game_menu.hpp"

namespace azl {
namespace ctr {

namespace {

using CTRPluginFramework::File;
using CTRPluginFramework::OSD;
using CTRPluginFramework::Screen;

constexpr int kScreenH = 240;

// One screen as a 24-bit BMP, a row at a time (no big buffer in the plugin's heap).
void ShotScreen(const Screen& scr, int w, const char* name, const char* which) {
    char path[96];
    std::snprintf(path, sizeof(path), "/azl-shots/plugin-%s-%s.bmp", name, which);
    CTRPluginFramework::Directory dir;
    CTRPluginFramework::Directory::Open(dir, "/azl-shots", true);
    File file;
    if (File::Open(file, path, File::WRITE | File::CREATE | File::TRUNCATE) != File::SUCCESS) {
        return;
    }
    const u32 row = static_cast<u32>(w) * 3;
    const u32 size = 54 + row * kScreenH;
    u8 head[54] = {'B', 'M'};
    const auto put32 = [&](int at, u32 v) {
        for (int i = 0; i < 4; ++i) {
            head[at + i] = static_cast<u8>(v >> (8 * i));
        }
    };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<u32>(w));
    put32(22, kScreenH);
    head[26] = 1;
    head[28] = 24;
    put32(34, row * kScreenH);
    file.Write(head, sizeof(head));
    std::vector<u8> line(row);
    for (int y = kScreenH - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const u8* p = scr.GetFramebuffer(static_cast<u32>(x), static_cast<u32>(y));
            u8 r = 0, g = 0, b = 0;
            const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
            switch (scr.Format) {
            case GSP_RGBA8_OES:
                r = p[3], g = p[2], b = p[1];
                break;
            case GSP_BGR8_OES:
                r = p[2], g = p[1], b = p[0];
                break;
            case GSP_RGB565_OES:
                r = static_cast<u8>(((v >> 11) & 31) << 3), g = static_cast<u8>(((v >> 5) & 63) << 2), b = static_cast<u8>((v & 31) << 3);
                break;
            case GSP_RGB5_A1_OES:
                r = static_cast<u8>(((v >> 11) & 31) << 3), g = static_cast<u8>(((v >> 6) & 31) << 3), b = static_cast<u8>(((v >> 1) & 31) << 3);
                break;
            default:
                r = static_cast<u8>(((v >> 12) & 15) << 4), g = static_cast<u8>(((v >> 8) & 15) << 4), b = static_cast<u8>(((v >> 4) & 15) << 4);
                break;
            }
            line[static_cast<std::size_t>(x) * 3] = b;
            line[static_cast<std::size_t>(x) * 3 + 1] = g;
            line[static_cast<std::size_t>(x) * 3 + 2] = r;
        }
        file.Write(line.data(), row);
    }
    file.Close();
}

} // namespace

void TourShotScreens(const char* name) {
    ShotScreen(OSD::GetTopScreen(), 400, name, "top");
    ShotScreen(OSD::GetBottomScreen(), 320, name, "bottom");
}

} // namespace ctr
} // namespace azl
#endif // AZL_MENU_TOUR
