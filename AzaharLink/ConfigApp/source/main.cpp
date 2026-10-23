// 3DS Link - a homebrew app to switch 3DS Link on or off, choose the Azahar room, check that the console can reach it, enable titles for
// it, and store the settings the AzaharLink plugin reads when a title starts.
//
// This file only works out where the app is, loads the settings and runs the loop; the console services and the graphics are opened in
// session.cpp, the interface is in ui.cpp and the pages in pages_*.cpp.
#include <3ds.h>

#include <cstdio>
#include <string>

#include "../titleshare_host/delivery.h"
#include "copyworker.hpp"
#include "pages.hpp"
#include "paths.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "system.hpp"
#include "titles.hpp"
#include "ui.hpp"

using ui::g;

namespace {

// The copies of the plugin that are still being made are finished before the app ends: a copy cut in the middle would leave a half file.
// With the screens still up, a short message says so; when the system is closing the app there is no drawing, only the wait.
void FinishCopies() {
    setup::CopyWorker& worker = setup::CopyWorker::Get();
    const u64 start = ui::NowMs();
    while (worker.PendingCount() > 0 && ui::NowMs() - start < 20000) {
        if (!aptMainLoop()) {
            worker.WaitIdle(8000);
            break;
        }
        if (gfx::BeginFrame()) {
            gfx::BeginTop();
            ui::DrawTitleBarStatus(gfx::kTopW, "3DS Link");
            ui::DrawCard(10, 32, 380, 198);
            gfx::DrawText(200, 112, 0.6f, ui::col::kText, "Finishing the copies of the plugin", gfx::Align::Center);
            gfx::DrawText(200, 142, 0.5f, ui::col::kDim, ui::Fmt("%d left", worker.PendingCount()), gfx::Align::Center);
            ui::DrawFrameBorder(gfx::kTopW);
            gfx::BeginBottom();
            ui::DrawFrameBorder(gfx::kBottomW);
            gfx::EndFrame();
        } else {
            gspWaitForVBlank();
        }
    }
    worker.Stop();
}

} // namespace

int main(int argc, char** argv) {
    if (!session::Start()) {
        session::ShowFatal((std::string("The graphics could not be started: ") + gfx::InitError()).c_str());
        session::Stop();
        return 1;
    }
#ifdef AZL_TEST_DISABLE_LOADER
    setup::SetPluginLoader(false); // (a build made to try the question "enable the Plugin Loader?" on an emulator, which starts with it on)
#endif

    // Where the app is, and so where everything of ours is: the folder 3DSLink.3dsx was started from (argv[0] of the Homebrew Launcher).
    // An emulator gives no argv: the folder of the Homebrew Launcher then. A build made with -DAZL_TEST_ARGV0='"sdmc:/dir/3DSLink.3dsx"'
    // pretends the launcher said that, to try the app from another folder on an emulator.
    const char* argv0 = argc > 0 && argv && argv[0] ? argv[0] : nullptr;
#ifdef AZL_TEST_ARGV0
    argv0 = AZL_TEST_ARGV0;
#endif
    g.paths = setup::Paths::FromAppDir(setup::AppDirFromArgv0(argv0));
    setup::SetCurrentPaths(g.paths);
    if (argv0) {
        g.app_path = argv0;
    }

    // Until the app has been given leave to write (the first start's question) it only READS the card: the settings are loaded, and
    // nothing is made, completed or saved. A start that is not the first opens the data folder at once.
    g.settings_loaded = setup::LoadSettings(g.cfg);
    if (g.cfg.moved_from_old_place) {
        // (an app older than 2026-10-19 kept the file in sd:/luma/plugins: it now lives with the logs, and the user should know where)
        g.ShowToast("Settings moved to sd:/3ds/3DSLink/AzaharLink.ini (with the logs).", ui::col::kOn, 7000);
    }
    g.first_start = setup::IsFirstStart(g.paths);
    if (!g.first_start) {
        g.OpenDataFolder();
    }
    if (session::CfguOk()) {
        u8 language = 1;
        if (R_SUCCEEDED(CFGU_GetSystemLanguage(&language))) {
            g.language = language; // the names of the titles are read in the console's language
        }
        g.console_name = setup::ReadConsoleName();
    } else {
        g.console_name = setup::FallbackConsoleName();
    }
    g.net = setup::ReadNetwork(g.ac_ready, g.soc_ok());
    setup::ReadConsoleStatus(g.status, true);

    // The start-up flow (the first start's question, scan and checks; the Plugin Loader question) runs on this page and hands over to the main page.
    ui::SetRootPage(pages::NewStartupPage());

    int net_countdown = 60;
    int sd_countdown = 600;
    bool copy_hold = false;
    while (aptMainLoop() && !g.quit) {
        g.input.Poll();
        if (--net_countdown <= 0) {
            net_countdown = 60;
            g.net = setup::ReadNetwork(g.ac_ready, g.soc_ok());
            sd_countdown -= 60;
            setup::ReadConsoleStatus(g.status, sd_countdown <= 0); // the card's space moves slowly: every ten seconds
            if (sd_countdown <= 0) {
                sd_countdown = 600;
            }
        }
        // The key file appeared after the titles were read: they are read again as soon as the scan is idle (the names and the icons that
        // the system refused can be read with it).
        if (g.rescan_wanted && g.module_holds == 0 && titles::Scan().done()) {
            g.rescan_wanted = false;
            titles::Shutdown();
            gfx::ReleaseIcons(); // (the icons of the old scan are gone: a texture kept under the address of one would show the wrong picture)
            titles::Scan().Start(g.language);
            g.titles_health_applied = false;
        }
        // The end of a scan: the Installation check gets its row of the titles (what the scan found of no use), and the quiet look at the copies of the
        // plugin, which waits for the scan, is queued.
        if (!g.titles_health_applied && titles::Scan().done()) {
            g.titles_health_applied = true;
            g.RefreshInstall();
        }
        // The quiet looks at the copies are over: 3DSLink.cfg remembers which plugin file they were made for (the next start skips them).
        if (!g.plugin_stamp_pending.empty() && setup::CopyWorker::Get().PendingCount() == 0) {
            g.app_cfg.plugin_stamp = g.plugin_stamp_pending;
            g.plugin_stamp_pending.clear();
            g.SaveAppConfig();
        }
        // While the plugin is being copied into the folders of the titles HOME does nothing (a press in the middle of a copy on a slow card
        // would leave a half file), and the bar says so.
        if ((setup::CopyWorker::Get().PendingCount() > 0) != copy_hold) {
            copy_hold = !copy_hold;
            if (copy_hold) {
                g.HoldHome();
            } else {
                g.ReleaseHome();
            }
        }
        // Page changes are applied before the update so that a page is always updated at least
        // once before its first frame is drawn.
        ui::ApplyPageRequests();
        ui::CurrentPage().Update(g.input.in());
        if (g.quit) {
            break;
        }

        // With the backlights put out (SELECT) or the shell closed while a room of 3DS Link Delivery is up, there is nothing to look at: the frame is not drawn
        // (the module says so only while a room is up; at any other time this is false).
        if (!dl_skip_drawing() && gfx::BeginFrame()) {
            ui::Page& page = ui::CurrentPage();
            gfx::BeginTop();
            page.DrawTop();
            gfx::BeginBottom();
            page.DrawBottom();
            gfx::EndFrame();
        } else {
            gspWaitForVBlank(); // never spin: the frame could not start, wait for the next one
        }
    }

    // A failed save is retried once on the way out (never over a file that could not be read: what is in it is not known).
    if (!g.save_ok && g.settings_loaded != setup::LoadResult::Unreadable) {
        setup::SaveSettings(g.cfg);
    }
    FinishCopies();
    g.home_holds = 0;
    g.ApplyHome();
    ui::ClearPages(); // leaves the room if a test is still running
    titles::Shutdown(); // waits (a few seconds at most) for the scan of the titles if it is still running
    session::Stop();
    return 0;
}
