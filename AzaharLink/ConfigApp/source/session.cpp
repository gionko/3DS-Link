#include "session.hpp"

#include <3ds.h>
#include <malloc.h>

#include <cstdio>

#include "ui.hpp"

using ui::g;

namespace session {

namespace {

constexpr u32 kSocBufferSize = 0x100000;

struct State {
    bool open = false;
    bool net_suspended = false;
    bool cfgu = false;
    bool am = false;
    u32* soc_buffer = nullptr;
};
State s;

// The sockets: a buffer for the service, and the service (g.soc_rc says whether it is open).
void OpenSockets() {
    s.soc_buffer = static_cast<u32*>(memalign(0x1000, kSocBufferSize));
    g.soc_rc = s.soc_buffer ? socInit(s.soc_buffer, kSocBufferSize) : static_cast<Result>(-1);
}

void CloseSockets() {
    if (g.soc_ok()) {
        socExit();
    }
    g.soc_rc = -1;
    free(s.soc_buffer);
    s.soc_buffer = nullptr;
}

} // namespace

bool Start() {
    osSetSpeedupEnable(true); // New 3DS / New 2DS: full CPU speed (no effect on the old models)
    gfxInitDefault();
    gfxSet3D(false);
    // The app draws every frame into the buffer that is not on the screen: double buffering is on by default and is set here so that the app
    // does not depend on what else touched it.
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    s.cfgu = R_SUCCEEDED(cfguInit());
    g.ac_ready = R_SUCCEEDED(acInit());
    s.am = R_SUCCEEDED(amInit()); // the list of the installed titles (TitleShare's scan asks AM for it)
    setup::OpenStatusServices();  // battery (ptm:u, mcu::HWC); either may be refused
    s.open = true;

    if (!gfx::Init()) {
        return false;
    }
    hidSetRepeatParameters(22, 5);

    OpenSockets();
    return true;
}

void Stop() {
    if (!s.open) {
        return;
    }
    s.net_suspended = false; // (what is not open is not closed below: g.soc_rc and g.ac_ready say what is)
    CloseSockets();
    gfx::Shutdown();
    setup::CloseStatusServices();
    if (s.am) {
        amExit();
    }
    if (g.ac_ready) {
        acExit();
    }
    g.ac_ready = false;
    if (s.cfgu) {
        cfguExit();
    }
    s.am = s.cfgu = false;
    gfxExit();
    s.open = false;
}

void SuspendNetwork() {
    if (!s.open || s.net_suspended) {
        return;
    }
    CloseSockets();
    if (g.ac_ready) {
        acExit();
    }
    g.ac_ready = false;
    s.net_suspended = true;
}

void ResumeNetwork() {
    if (!s.open || !s.net_suspended) {
        return;
    }
    g.ac_ready = R_SUCCEEDED(acInit());
    OpenSockets();
    s.net_suspended = false;
}

bool CfguOk() {
    return s.cfgu;
}

void ShowFatal(const char* message) {
    consoleInit(GFX_TOP, nullptr);
    std::printf("3DS Link %s\n\n%s\n\nPress START to exit.\n", setup::kVersion, message);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

} // namespace session
