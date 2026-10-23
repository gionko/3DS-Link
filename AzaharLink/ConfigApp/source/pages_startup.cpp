// Page: the start-up flow. It is the first page of the app and the main page takes its place when it is over.
//
// The FIRST start (there is no 3DSLink.cfg in the app's data folder) is a guided sequence:
//   question   "First start: 3DS Link will scan the SD card and check your installation. This takes a while. Continue?" Yes goes on. No ends
//              the app at once and writes nothing (not even the data folder): the app has not been given leave to touch the card yet;
//   (a) the scan of the installed titles, with its progress bar (the caches are built the first time);
//   (b) boot9.bin, looked for next to the app, in GodMode9's folder and where the user pointed to;
//   (b2) the seed database, which the console keeps in its own memory (it has to be readable; it is not extracted);
//   (c) the plugin file: 3DS Link carries its plugin inside and has put it on the card by itself as soon as the question was answered Yes (when the data folder was opened);
//   this step says what is there now;
//   (d) the plugin folder (offered to be made when it is missing);
//   (e) Rosalina's Plugin Loader, with the question to enable it (only when it is off and 3DS Link is on), then a summary page (the installation check: what is fine, what
//   is not, each problem can be fixed from it), and only after that 3DSLink.cfg is written and the main page opens.
// Every other start only scans the titles (the first screen only when there is no cache yet; otherwise it runs on behind the main page),
// asks about the Plugin Loader when it is off, and opens the main page.
#include "dlp_text.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"
#include "titles.hpp"

using namespace ui;

namespace {

class StartupPage : public Page {
public:
    StartupPage() {
        if (g.first_start) {
            phase_ = Phase::Welcome;
        } else {
            g.RefreshInstall();
            EnterScan();
        }
    }

    void Update(const Input& in) override {
        switch (phase_) {
        case Phase::Welcome:
            if ((in.down & KEY_A) || Tapped(in, WelcomeBox().yes)) {
                Begin();
            } else if ((in.down & KEY_B) || Tapped(in, WelcomeBox().no)) {
                g.quit = true; // (nothing was written: the first start comes again next time)
            } else if ((in.down & KEY_Y) || Tapped(in, GuideRect())) {
                // The text above says the check looks for boot9.bin: how to get it is a key away. The guide only shows pictures; nothing is written.
                if (!pages::OpenBoot9Guide()) {
                    PushPage(pages::NewMessagePage("Guide", dlp::kNoGuideToast));
                }
            }
            break;
        case Phase::Scan:
            UpdateScan();
            break;
        case Phase::Boot9:
        case Phase::Seed:
        case Phase::PluginFile:
        case Phase::LoaderCheck:
            UpdateQuickStep();
            break;
        case Phase::PluginDir:
            UpdatePluginDir();
            break;
        case Phase::DirAsk:
            if (dir_answer_ != 0) {
                FinishPluginDir(dir_answer_ == 1);
            }
            break;
        case Phase::DirDone:
            if (NowMs() >= hold_until_) {
                EnterLoaderCheck();
            }
            break;
        case Phase::Loader:
            UpdateLoader(in);
            break;
        case Phase::Summary:
            if (summary_result_ == 1) {
                Finish();
            } else if (summary_result_ == 2) {
                g.quit = true; // (cancelled: 3DSLink.cfg is not written, the first start comes again next time)
            }
            break;
        case Phase::Done:
            ReplaceRootPage(pages::NewMainPage());
            break;
        }
    }

    void DrawTop() override {
        if (phase_ == Phase::Done) {
            return; // (one frame at most, before the main page)
        }
        DrawTitleBarStatus(gfx::kTopW, "3DS Link");
        // One card for the whole screen: the same margin (10) on the left, the right and the bottom, so that no black strip is left.
        constexpr float kCardY = 32.0f, kCardH = 198.0f;
        DrawCard(10, kCardY, 380, kCardH);
        gfx::FillRect(10, kCardY, 3, kCardH, col::kAccent);
        if (phase_ == Phase::Welcome) {
            gfx::DrawText(26, 56, 0.62f, col::kText, "First start");
            gfx::DrawWrapped(26, 78, 354, 0.5f, col::kText,
                             "3DS Link will scan the SD card for your installed titles and check that everything it needs is where it should be: "
                             "the plugin file, the plugin folder, the Plugin Loader, the seed database and boot9.bin.\n\nThis takes a while, and it is done only once.\n\n"
                             "If you say No, 3DS Link closes and writes nothing on the card.",
                             10);
        } else if (phase_ == Phase::Loader) {
            gfx::DrawText(26, 56, 0.62f, col::kText, "Plugin Loader is disabled");
            gfx::DrawWrapped(26, 80, 354, 0.5f, col::kText,
                             "Rosalina's Plugin Loader is switched off, so no title can load the 3DS Link plugin.\n\n"
                             "Enable it to use 3DS Link over the internet?\n\n"
                             "It is the same switch as Rosalina's menu (L+Down+Select, Plugin Loader), set from here.",
                             9);
        } else if (g.first_start) {
            DrawSteps();
        } else {
            gfx::DrawText(26, 74, 0.62f, col::kText, "Reading your titles");
            gfx::DrawWrapped(26, 98, 350, 0.5f, col::kText,
                             "First time only: it builds the cache of your installed titles. Next starts only read what is new or changed.", 4);
            DrawProgressBar(26, 166, 348, 18, setup::ProgressFraction(titles::Scan().progress_done(), titles::Scan().progress_total()));
            gfx::DrawText(200, 200, 0.5f, col::kDim, setup::ScanProgressText(titles::Scan().progress()), gfx::Align::Center);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (phase_ == Phase::Done) {
            return;
        }
        if (phase_ == Phase::Welcome) {
            DrawTitleBar(gfx::kBottomW, "First start", std::string());
            const PopupBox box = WelcomeBox();
            DrawPopupBox(box, "Continue?", kQuestion);
            DrawButton(box.yes, "A", "Yes", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
            DrawButton(GuideRect(), "Y", "How to get boot9.bin", true, PressedNow(g.input.in(), GuideRect()), true);
            DrawFrameBorder(gfx::kBottomW);
            return;
        }
        DrawTitleBar(gfx::kBottomW, phase_ == Phase::Loader ? "Plugin Loader" : "Please wait", phase_ == Phase::Loader ? "" : "first start");
        if (phase_ == Phase::Loader) {
            const PopupBox box = LoaderBox();
            DrawPopupBox(box, "Enable the Plugin Loader?", kLoaderText);
            DrawButton(box.yes, "A", "Yes, enable it", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
        } else {
            // The card is in the middle of the screen (below the title bar), not at its top.
            constexpr float kH = 110.0f;
            const float y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - kH) * 0.5f;
            DrawCard(8, y, 304, kH);
            gfx::DrawText(160, y + 28, 0.55f, col::kText,
                          phase_ == Phase::Scan ? "Building the list of your titles" : "Checking your installation", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim,
                             phase_ == Phase::Scan ? "This takes a moment, and only the first time: the list is saved on the SD card."
                                                   : "boot9.bin, the seed database, the plugin file, the plugin folder and the Plugin Loader.",
                             3);
            DrawSpinner(y + kH - 18.0f); // (a moving mark, so it is clear that the app is working)
        }
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    static constexpr const char* kQuestion =
        "First start: 3DS Link will scan the SD card and check your installation. This takes a while. Continue?";
    static constexpr const char* kLoaderText = "Without it titles start without 3DS Link. No: you can switch it on later in Rosalina's menu.";
    static PopupBox WelcomeBox() {
        return LayoutPopup(kQuestion, true);
    }
    // The key of the guide "How to get boot9.bin" under the question, where the footer of the other pages is (the first start's text names boot9.bin).
    static Rect GuideRect() {
        return Rect{8.0f, 216.0f, 304.0f, 20.0f};
    }
    static PopupBox LoaderBox() {
        return LayoutPopup(kLoaderText, true);
    }

    enum class Phase { Welcome, Scan, Boot9, Seed, PluginFile, PluginDir, DirAsk, DirDone, LoaderCheck, Loader, Summary, Done };
    static constexpr int kSteps = 6; // the titles, boot9.bin, the seed database, the plugin file, the plugin folder, the Plugin Loader
    enum class StepState { Pending, Running, Ok, Problem };
    struct Step {
        const char* label = "";
        StepState state = StepState::Pending;
        std::string detail;
    };

#ifdef AZL_TEST_HOLD_MS
    static constexpr u64 kHoldMs = AZL_TEST_HOLD_MS; // (a build made to look at every step on an emulator: make EXTRA_CFLAGS=-DAZL_TEST_HOLD_MS=4000)
#else
    static constexpr u64 kHoldMs = 900; // a finished step stays a moment, so that it can be read
#endif

    // The question was answered Yes: from here the app may write. The data folder is made and the sequence begins
    // with the scan.
    void Begin() {
        g.OpenDataFolder();
        g.RefreshLoader();
        steps_[0].label = "Reading your titles";
        steps_[1].label = "boot9.bin";
        steps_[2].label = "Seed database";
        steps_[3].label = "Plugin file";
        steps_[4].label = "Plugin folder";
        steps_[5].label = "Plugin Loader";
        EnterScan();
    }

    // Step (a): starts the scan. With no cache yet this is the long first scan, and it gets the whole screen until it is done (the first start
    // always shows it). Otherwise it runs on behind the main page and the flow goes straight on.
    void EnterScan() {
        const bool first = !titles::Scanner::CacheExists();
        titles::Scan().Start(g.language);
        if (g.first_start || first) {
            phase_ = Phase::Scan;
            steps_[0].state = StepState::Running;
        } else {
            EnterLoader();
        }
    }

    void UpdateScan() {
        if (!titles::Scan().done()) {
            return;
        }
        if (hold_until_ == 0) {
            hold_until_ = NowMs() + kHoldMs; // the full bar stays a moment, so that it can be read
            steps_[0].state = StepState::Ok;
            steps_[0].detail = setup::ScanProgressText(titles::Scan().progress()); // ("221 titles, 172 games")
        } else if (NowMs() >= hold_until_) {
            hold_until_ = 0;
            if (g.first_start) {
                EnterBoot9();
            } else {
                EnterLoader();
            }
        }
    }

    // Step (b): the key file, in the default places (the lookup was made when the data folder was opened; the scan has used it).
    void EnterBoot9() {
        phase_ = Phase::Boot9;
        g.RefreshBoot9();
        steps_[1].state = g.boot9.found ? StepState::Ok : StepState::Problem;
        steps_[1].detail = g.boot9.found ? "found: " + g.boot9.folder() : std::string("not found yet");
        hold_until_ = NowMs() + kHoldMs;
    }

    // Step (b2): the database of the seeds, a file of the console's own memory: it is not extracted (boot9.bin is not needed for it) but it has to be
    // readable, or the titles that came with a seed cannot be sent.
    void EnterSeed() {
        phase_ = Phase::Seed;
        const setup::SeedReading seed = setup::ReadSeedDb();
        steps_[2].state = seed.state == 1 ? StepState::Ok : StepState::Problem;
        steps_[2].detail = seed.state == 1 ? "readable, " + setup::FormatSize(seed.size) : std::string("not readable");
        hold_until_ = NowMs() + kHoldMs;
    }

    // Step (c): the plugin file. The app put it on the card when the data folder was opened (it is a resource of the app), so this is what the card holds now, against the
    // plugin inside the app; `plugin_notice` is there when the file was put (or could not be) a moment ago.
    void EnterPluginFile() {
        phase_ = Phase::PluginFile;
        g.LookAtPlugin(false);
        const bool ok = g.plugin_card == setup::PluginCard::Same && setup::EmbeddedPluginUsable();
        steps_[3].state = ok ? StepState::Ok : StepState::Problem;
        steps_[3].detail = setup::PluginStepDetail(setup::EmbeddedPluginUsable(), g.plugin_card, !g.plugin_notice.empty(), g.plugin_found_version);
        hold_until_ = NowMs() + kHoldMs;
    }

    // Step (e): Rosalina's Plugin Loader. When it is off the question comes next.
    void EnterLoaderCheck() {
        phase_ = Phase::LoaderCheck;
        g.RefreshLoader();
        steps_[5].state = g.loader == setup::LoaderState::Disabled ? StepState::Problem : StepState::Ok;
        steps_[5].detail = g.loader == setup::LoaderState::Enabled ? "enabled" : (g.loader == setup::LoaderState::Disabled ? "disabled" : "not asked here");
        hold_until_ = NowMs() + kHoldMs;
    }

    // The quick steps: each stays a moment on the screen.
    void UpdateQuickStep() {
        if (NowMs() < hold_until_) {
            return;
        }
        if (phase_ == Phase::Boot9) {
            EnterSeed();
        } else if (phase_ == Phase::Seed) {
            EnterPluginFile();
        } else if (phase_ == Phase::LoaderCheck) {
            EnterLoader();
        } else {
            phase_ = Phase::PluginDir;
            steps_[4].state = StepState::Running;
        }
    }

    // Step (d): the plugin folder. When it is not there the user is asked whether to make it.
    void UpdatePluginDir() {
        if (setup::PluginsDirExists()) {
            FinishPluginDir(false);
            return;
        }
        phase_ = Phase::DirAsk;
        dir_answer_ = 0;
        PushPage(pages::NewConfirmPage("Plugin folder",
                                       std::string("The folder ") + setup::kPluginsDir +
                                           " is not there. Luma3DS loads plugins from it, and 3DS Link keeps its settings file in it. Make it now?",
                                       "Yes, make it", "No", [this](bool yes) { dir_answer_ = yes ? 1 : 2; }));
    }

    void FinishPluginDir(bool make) {
        bool ok = setup::PluginsDirExists();
        if (!ok && make) {
            ok = setup::MakeDirs(setup::kPluginsDir);
            g.EnsureIniFile(); // (the settings file is made as soon as there is a folder for it)
        }
        dir_answer_ = 0;
        steps_[4].state = ok ? StepState::Ok : StepState::Problem;
        steps_[4].detail = ok ? std::string(setup::kPluginsDir) : "not there";
        hold_until_ = NowMs() + kHoldMs; // (the finished list stays a moment, then the Plugin Loader)
        phase_ = Phase::DirDone;
    }

    // The Plugin Loader question: asked only when the loader is known to be off.
    void EnterLoader() {
        g.RefreshLoader();
        phase_ = setup::ShouldOfferLoader(g.loader, g.cfg.enabled && g.cfg.tunnel) ? Phase::Loader : AfterLoader();
        if (phase_ == Phase::Summary) {
            ShowSummary();
        }
    }

    Phase AfterLoader() {
        return g.first_start ? Phase::Summary : Phase::Done;
    }

    void UpdateLoader(const Input& in) {
        const bool yes = (in.down & KEY_A) || Tapped(in, LoaderBox().yes);
        const bool no = (in.down & KEY_B) || Tapped(in, LoaderBox().no);
        if (yes) {
            const Result rc = setup::SetPluginLoader(true);
            g.RefreshLoader(); // what the loader says now, not what was asked
            bool good = false;
            const std::string text = setup::LoaderEnableMessage(static_cast<u32>(rc), g.loader, &good);
            g.ShowToast(text, good ? col::kOn : col::kOff, 6000);
        }
        if (yes || no) {
            phase_ = AfterLoader();
            if (phase_ == Phase::Summary) {
                ShowSummary();
            }
        }
    }

    // The summary: the installation check page. START goes on to the main page, B cancels (nothing is saved).
    void ShowSummary() {
        summary_result_ = 0;
        PushPage(pages::NewInstallCheckPage([this](bool go_on) { summary_result_ = go_on ? 1 : 2; }, true));
    }

    // The end of the first start: only now is 3DSLink.cfg written (an app that was closed on the way asks again next time).
    void Finish() {
        if (g.first_start) {
            g.app_cfg.first_start_done = true;
            g.SaveAppConfig();
            g.first_start = false;
        }
        g.RefreshInstall();
        phase_ = Phase::Done;
    }

    // The steps of the first start with where each one is. The place of every row is fixed (the first one has room for its bar whether it is
    // drawn or not), so nothing moves when a step ends.
    void DrawSteps() const {
        gfx::DrawText(26, 54, 0.62f, col::kText, "First start");
        float yc = 80.0f;
        for (int i = 0; i < kSteps; ++i) {
            const Step& s = steps_[i];
            const float x = 30.0f;
            if (s.state == StepState::Ok) {
                gfx::FillCircle(x, yc, 6.0f, col::kOn);
                gfx::Line(x - 3.0f, yc, x - 1.0f, yc + 3.0f, 2.0f, col::kBg);
                gfx::Line(x - 1.0f, yc + 3.0f, x + 3.5f, yc - 3.0f, 2.0f, col::kBg);
            } else if (s.state == StepState::Problem) {
                gfx::FillCircle(x, yc, 6.0f, col::kWarn);
                gfx::FillRect(x - 1.0f, yc - 4.0f, 2.0f, 5.0f, col::kBg);
                gfx::FillRect(x - 1.0f, yc + 2.0f, 2.0f, 2.0f, col::kBg);
            } else if (s.state == StepState::Running) {
                const int dot = static_cast<int>((NowMs() / 200) % 3);
                for (int k = 0; k < 3; ++k) {
                    gfx::FillCircle(x - 6.0f + 6.0f * static_cast<float>(k), yc, 2.0f, k == dot ? col::kAccent : col::kTrack);
                }
            } else {
                gfx::FillCircle(x, yc, 4.0f, col::kTrack);
            }
            const bool lit = s.state != StepState::Pending;
            gfx::DrawText(48, yc, 0.52f, lit ? col::kText : col::kDim, s.label);
            if (i == 0) {
                const bool running = s.state == StepState::Running;
                gfx::DrawText(380, yc, 0.46f, col::kDim, setup::ScanProgressText(titles::Scan().progress()), gfx::Align::Right);
                DrawProgressBar(48, yc + 11, 328, 11, running ? setup::ProgressFraction(titles::Scan().progress_done(), titles::Scan().progress_total()) : 1.0f);
                yc += 38.0f;
            } else {
                if (!s.detail.empty()) {
                    gfx::DrawFitted(380, yc, 0.46f, 0.34f, 200, s.state == StepState::Problem ? col::kWarn : col::kDim, gfx::Clean(s.detail),
                                    gfx::Align::Right);
                }
                yc += 23.0f;
            }
        }
    }

    Phase phase_ = Phase::Welcome;
    Step steps_[kSteps];
    u64 hold_until_ = 0;
    int dir_answer_ = 0; // 0 not answered, 1 yes, 2 no (the popup that offers to make the plugin folder)
    int summary_result_ = 0; // 0 the summary is open, 1 it was closed with START (go on), 2 with B (cancel)
};

} // namespace

namespace pages {

ui::Page* NewStartupPage() {
    return new StartupPage;
}

} // namespace pages
