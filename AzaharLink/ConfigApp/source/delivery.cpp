#include "delivery.hpp"

#include <cstdio>

#include "session.hpp"
#include "titles.hpp"

namespace delivery {

std::string SizeText(u64 bytes) {
    char buf[32];
    if (bytes < 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

std::string DurationText(double seconds) {
    if (seconds < 45.0) {
        return "under a minute";
    }
    const int minutes = static_cast<int>(seconds / 60.0 + 0.5);
    char buf[40];
    if (minutes < 60) {
        std::snprintf(buf, sizeof(buf), "about %d min", minutes);
    } else {
        std::snprintf(buf, sizeof(buf), "about %d h %02d min", minutes / 60, minutes % 60);
    }
    return buf;
}

std::string ShortDurationText(double seconds) {
    if (seconds < 45.0) {
        return "<1 min";
    }
    const int minutes = static_cast<int>(seconds / 60.0 + 0.5);
    char buf[32];
    if (minutes < 60) {
        std::snprintf(buf, sizeof(buf), "%d min", minutes);
    } else {
        std::snprintf(buf, sizeof(buf), "%d h %02d min", minutes / 60, minutes % 60);
    }
    return buf;
}

double SafeBytesPerSecond() {
    // How fast titles come over the local wireless on average (MiB a second, measured on the consoles), for the time the user is told.
    constexpr double kNewMiBps = 1.6;
    constexpr double kOldMiBps = 1.4;
    return (dl_is_new_3ds() ? kNewMiBps : kOldMiBps) * kMiB;
}

double EstimateSeconds(u64 bytes) {
    return static_cast<double>(bytes) / SafeBytesPerSecond();
}

float DrawPair(float x, float yc, float scale, const std::string& label, const std::string& value, u32 color) {
    gfx::DrawText(x, yc, scale, ui::col::kDim, label);
    x += gfx::TextWidth(label, scale) + 3.0f;
    gfx::DrawText(x, yc, scale, color, value);
    return x + gfx::TextWidth(value, scale) + 10.0f;
}

std::string KindText(int kind, u64 tid) {
    switch (kind) {
    case DL_KIND_APP:
        return "Game";
    case DL_KIND_UPDATE:
        return "Update";
    case DL_KIND_DLC: {
        const unsigned low = static_cast<unsigned>(tid & 0xFF);
        char buf[16];
        if (low != 0) {
            std::snprintf(buf, sizeof(buf), "DLC %02X", low);
            return buf;
        }
        return "DLC";
    }
    default:
        return "Other";
    }
}

u32 KindColor(int kind) {
    using namespace ui;
    return kind == DL_KIND_APP ? col::kGame : (kind == DL_KIND_UPDATE ? col::kUpdate : (kind == DL_KIND_DLC ? col::kDlc : col::kText));
}

unsigned KindTag(int kind) {
    return kind == DL_KIND_APP ? ui::kTagGame : (kind == DL_KIND_UPDATE ? ui::kTagUpdate : (kind == DL_KIND_DLC ? ui::kTagDlc : 0u));
}

std::string ClockText(double seconds) {
    if (seconds < 0.0 || seconds > 99.0 * 3600.0) {
        return "--:--";
    }
    const int s = static_cast<int>(seconds + 0.5);
    char buf[24];
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
    }
    return buf;
}

bool AttachModule() {
    void* ctx = titles::Scan().module_context();
    void* catalog = titles::Scan().module_catalog();
    if (!ctx || !catalog) {
        return false;
    }
    dl_attach(ctx, catalog);
    return true;
}

namespace {

void Trampoline(void* arg) {
    (*static_cast<const std::function<void()>*>(arg))();
}

} // namespace

bool RunOnBigStack(const std::function<void()>& fn) {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread thread = threadCreate(Trampoline, const_cast<std::function<void()>*>(&fn), 256 * 1024, priority, -2, false);
    if (!thread) {
        return false;
    }
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    return true;
}

void PumpInput() {
    ui::g.input.Poll();
}

bool DrawPage(ui::Page& page, bool wait) {
    if (!gfx::BeginFrame(wait)) {
        return false;
    }
    gfx::BeginTop();
    page.DrawTop();
    gfx::BeginBottom();
    page.DrawBottom();
    gfx::EndFrame();
    return true;
}

Wireless::~Wireless() {
    Close();
}

bool Wireless::Open() {
    if (open_) {
        return true;
    }
    session::SuspendNetwork(); // (the module found a console with none of this open, as a 3dsx of its own)
    u32 rc = 0;
    if (dl_net_open(&rc) != 0) {
        error_ = rc;
        session::ResumeNetwork();
        return false;
    }
    open_ = true;
    return true;
}

void Wireless::Close() {
    if (!open_) {
        return;
    }
    dl_net_close();
    dl_power_exit();
    session::ResumeNetwork();
    open_ = false;
}

std::string WirelessErrorText(u32 error) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "The local wireless could not be started (0x%08lX). Is the Wi-Fi switched on? It is the switch on the side of the console.",
                  static_cast<unsigned long>(error));
    return buf;
}

} // namespace delivery
