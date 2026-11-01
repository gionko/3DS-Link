// Pages: the update of 3DS Link from GitHub. The quiet check at the start (it says nothing unless a new version is out, or the clock is wrong: once), the row of Settings that asks
// and says what came of it, the question, and the page that downloads, installs and restarts. What is said and decided is in update_logic.hpp, the work in update_worker.cpp.
#include "dlp_prepare_logic.hpp" // (dlp::kAppTid: the title id of the installed app)
#include "pages.hpp"
#include "update_worker.hpp"

using namespace ui;

namespace {

// How long the check of a start waits for the WiFi: a console that is not connected by then is not asked again until the next start.
constexpr u64 kWaitForWifiMs = 30000;
// A popup that comes by itself ignores the keys for a moment: an A meant for the page under it must not say yes to an update.
constexpr u64 kArmMs = 500;

update::Target CurrentTarget() {
    u64 program_id = 0;
    const bool known = R_SUCCEEDED(APT_GetProgramID(&program_id));
    return update::ChooseTarget(about::OriginOf(g.app_path, envIsHomebrew(), known, program_id, dlp::kAppTid), g.app_path);
}

void ForgetClockWarning() {
    if (g.app_cfg.clock_warned) {
        g.app_cfg.clock_warned = false;
        g.SaveAppConfig();
    }
}

// ---------------------------------------------------------------------------------------------
// Downloading and installing
// ---------------------------------------------------------------------------------------------

class UpdateRunPage : public Page {
public:
    UpdateRunPage(const std::string& version, update::Target target) : version_(version), target_(target) {
        // HOME and the lid wait until this is over (a sleep would cut the connection; a title that is being installed is not to be left).
        g.HoldHome();
        aptSetSleepAllowed(false);
        if (!update::Worker::Get().StartInstall(target, version, g.app_path)) {
            busy_ = true;
        }
    }

    ~UpdateRunPage() override {
        aptSetSleepAllowed(true);
        g.ReleaseHome();
    }

    void Update(const Input& in) override {
        if (busy_) {
            status_.stage = update::Stage::Failed;
            status_.failure = update::Failure::Busy;
        } else {
            status_ = update::Worker::Get().Read();
        }
        const FooterRects rects = LayoutFooter(MakeFooter());
        switch (status_.stage) {
        case update::Stage::Downloading:
            if ((in.down & KEY_B) || Tapped(in, rects.back)) {
                update::Worker::Get().Cancel();
            }
            break;
        case update::Stage::Installed:
            if ((in.down & KEY_A) || Tapped(in, rects.action)) {
                g.relaunch = target_ == update::Target::Cia; // (main.cpp: the system starts this title again when the app ends; a .3dsx goes back to its launcher)
                g.quit = true;
            }
            break;
        case update::Stage::Failed:
            if (status_.failure == update::Failure::Net && status_.net == update::NetResult::Cancelled) {
                PopPage(); // (the user's own B: nothing to say)
            } else if ((in.down & (KEY_A | KEY_B)) || Tapped(in, rects.back)) {
                PopPage();
            }
            break;
        default:
            break;
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "3DS Link");
        DrawCard(10, 32, 380, 200);
        gfx::FillRect(10, 32, 3, 200, col::kAccent);
        gfx::DrawText(26, 48, 0.55f, col::kAccent, "Update to v" + version_);
        gfx::DrawWrapped(26, 62, 354, 0.45f, col::kText, TopText(), 12);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawTitleBar(gfx::kBottomW, "Update", std::string());
        DrawCard(10, 32, 300, 176);
        switch (status_.stage) {
        case update::Stage::Failed:
            gfx::DrawText(160, 50, 0.55f, col::kOff, "The update did not work", gfx::Align::Center);
            gfx::DrawWrapped(20, 68, 280, 0.45f, col::kText, update::FailureText(status_), 9);
            break;
        case update::Stage::Installed:
            gfx::DrawText(160, 50, 0.55f, col::kOn, "Update installed", gfx::Align::Center);
            gfx::DrawWrapped(20, 68, 280, 0.45f, col::kText, update::DoneText(target_, version_), 9);
            break;
        case update::Stage::Installing:
            gfx::DrawText(160, 90, 0.6f, col::kText, "Installing...", gfx::Align::Center);
            DrawSpinner(130.0f);
            break;
        default: {
            gfx::DrawText(160, 80, 0.6f, col::kText, "Downloading", gfx::Align::Center);
            const float fraction = status_.total > 0 ? static_cast<float>(status_.done) / static_cast<float>(status_.total) : 0.0f;
            DrawProgressBar(30, 106, 260, 14, fraction);
            if (status_.total > 0) {
                gfx::DrawText(160, 138, 0.5f, col::kDim, Fmt("%.1f of %.1f MB", status_.done / 1048576.0, status_.total / 1048576.0), gfx::Align::Center);
            } else {
                gfx::DrawText(160, 138, 0.5f, col::kDim, "Asking GitHub for the file...", gfx::Align::Center);
            }
            break;
        }
        }
        DrawFooterBackground();
        DrawFooter(MakeFooter());
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    Footer MakeFooter() const {
        Footer footer;
        footer.start = false;
        footer.back = false;
        switch (status_.stage) {
        case update::Stage::Downloading:
            footer.back = true;
            footer.back_label = "Cancel";
            break;
        case update::Stage::Installed:
            footer.action_label = target_ == update::Target::Cia ? "Restart" : "Close";
            break;
        case update::Stage::Failed:
            footer.back = true;
            footer.back_label = "Back";
            break;
        default:
            break;
        }
        return footer;
    }

    const char* TopText() const {
        switch (status_.stage) {
        case update::Stage::Downloading:
            return "HOME and sleep are off until this is over. B stops the download: nothing has changed yet.";
        case update::Stage::Installing:
            return "Do not switch the console off.";
        case update::Stage::Installed:
            return "Until 3DS Link has started again and said that the plugin is up to date, your games still have the old plugin. Do not skip this: a console with some "
                   "games on the old plugin and some on the new one is hard to make sense of.";
        case update::Stage::Failed:
            return "Nothing has changed: you still have the version you had. You can try again from Settings > Check for updates.";
        default:
            return "";
        }
    }

    std::string version_;
    update::Target target_;
    update::Status status_;
    bool busy_ = false;
};

// ---------------------------------------------------------------------------------------------
// The question
// ---------------------------------------------------------------------------------------------

class UpdatePromptPage : public Page {
public:
    UpdatePromptPage(const std::string& latest, const std::string& whats_new, update::Target target)
        : latest_(latest), whats_new_(whats_new), target_(target), text_(update::PromptText(latest, setup::kVersion)), box_(LayoutPopup(text_, true)), opened_(NowMs()) {}

    void Update(const Input& in) override {
        if (NowMs() - opened_ < kArmMs) {
            return;
        }
        if ((in.down & KEY_A) || Tapped(in, box_.yes)) {
            PopPage();
            PushPage(new UpdateRunPage(latest_, target_));
        } else if ((in.down & KEY_B) || Tapped(in, box_.no)) {
            // Not at the next start either; a newer version asks again, and so does the row of Settings.
            g.app_cfg.update_not_now = latest_;
            g.SaveAppConfig();
            PopPage();
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "3DS Link");
        DrawCard(10, 32, 380, 200);
        gfx::FillRect(10, 32, 3, 200, col::kAccent);
        gfx::DrawText(26, 48, 0.55f, col::kAccent, "What's new in v" + latest_);
        gfx::DrawWrapped(26, 62, 354, 0.45f, col::kText, update::PromptTopText(whats_new_), 12);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (Page* below = PageBelow()) {
            below->DrawBottom();
        }
        DrawDim(gfx::kBottomW);
        DrawPopupBox(box_, update::kPromptTitle, text_);
        const Input& in = g.input.in();
        DrawButton(box_.yes, "A", "Update", true, PressedNow(in, box_.yes));
        DrawButton(box_.no, "B", "Not now", true, PressedNow(in, box_.no));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    std::string latest_, whats_new_;
    update::Target target_;
    std::string text_;
    PopupBox box_;
    u64 opened_;
};

// ---------------------------------------------------------------------------------------------
// The row of Settings: asks now, and says what came of it
// ---------------------------------------------------------------------------------------------

class UpdateCheckPage : public Page {
public:
    UpdateCheckPage() : box_(LayoutPopup("Asking GitHub for the latest version...", false)) {}

    void Update(const Input& in) override {
        if (!started_) {
            started_ = true;
            if (update::ClockTooOld(update::ConsoleYear())) {
                Say(update::kClockTitle, update::kClockText);
                return;
            }
            if (!g.soc_ok() || (g.net.known && !g.net.wifi)) {
                Say("No connection", "This console is not connected to the WiFi.");
                return;
            }
            update::Worker::Get().StartCheck(); // (false: the check of this start is still running, and its answer is as good)
        }
        const update::Status status = update::Worker::Get().Read();
        if (status.stage == update::Stage::Checked) {
            ForgetClockWarning();
            if (!update::IsNewer(status.latest, setup::kVersion)) {
                Say(update::kUpToDateTitle, update::UpToDateText(setup::kVersion));
                return;
            }
            const update::Target target = CurrentTarget();
            if (target == update::Target::None) {
                Say(update::kPromptTitle, update::NoFileText(status.latest));
                return;
            }
            PopPage();
            PushPage(new UpdatePromptPage(status.latest, status.whats_new, target));
        } else if (status.stage == update::Stage::Failed) {
            if (status.failure == update::Failure::Net && status.net == update::NetResult::Cancelled) {
                PopPage();
            } else {
                Say(status.net == update::NetResult::Clock ? update::kClockTitle : "No answer", update::FailureText(status));
            }
        } else if ((in.down & KEY_B) || Tapped(in, box_.ok)) {
            update::Worker::Get().Cancel(); // (the request stops at its next step, on its own thread: nobody has to wait for it here)
            PopPage();
        }
    }

    void DrawTop() override {
        if (Page* below = PageBelow()) {
            below->DrawTop();
        }
        DrawDim(gfx::kTopW);
    }

    void DrawBottom() override {
        if (Page* below = PageBelow()) {
            below->DrawBottom();
        }
        DrawDim(gfx::kBottomW);
        static const char* const kDots[] = {"Asking GitHub for the latest version", "Asking GitHub for the latest version.", "Asking GitHub for the latest version..",
                                            "Asking GitHub for the latest version..."};
        DrawPopupBox(box_, "Check for updates", kDots[(NowMs() / 400) % 4]);
        DrawButton(box_.ok, "B", "Cancel", true, PressedNow(g.input.in(), box_.ok));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    void Say(const std::string& title, const std::string& text) {
        PopPage();
        PushPage(pages::NewMessagePage(title, text));
    }

    PopupBox box_;
    bool started_ = false;
};

// ---------------------------------------------------------------------------------------------
// The check of a start
// ---------------------------------------------------------------------------------------------

enum class AtStart { WaitForWifi, Asking, ClockDue, Over };
AtStart g_at_start = AtStart::WaitForWifi;
u64 g_at_start_since = 0;

} // namespace

namespace pages {

ui::Page* NewUpdateCheckPage() {
    return new UpdateCheckPage;
}

bool PollUpdateAtStart(const ui::Input& in) {
    if (g_at_start == AtStart::Over) {
        return false;
    }
    if (!g.app_cfg.update_check) {
        g_at_start = AtStart::Over;
        return false;
    }
    // A popup that comes by itself waits for a quiet moment: no key pressed now, and the pass over the games' plugin copies (it tells how it goes on this page) is over.
    const bool quiet = in.down == 0 && !in.touch_held && !g.plugin_pass_open;
    switch (g_at_start) {
    case AtStart::WaitForWifi:
        if (update::ClockTooOld(update::ConsoleYear())) {
            g_at_start = update::ClockPopupDue(true, g.app_cfg.clock_warned) ? AtStart::ClockDue : AtStart::Over;
            return false;
        }
        if (g_at_start_since == 0) {
            g_at_start_since = NowMs();
        }
        if (g.soc_ok() && g.net.known && g.net.wifi) {
            update::Worker::Get().StartCheck();
            g_at_start = AtStart::Asking;
        } else if (NowMs() - g_at_start_since > kWaitForWifiMs) {
            g_at_start = AtStart::Over;
        }
        return false;
    case AtStart::Asking: {
        const update::Status status = update::Worker::Get().Read();
        if (status.stage == update::Stage::Checking) {
            return false;
        }
        if (status.stage == update::Stage::Failed) {
            // Whatever failed stays quiet (no WiFi, no internet: this is nobody's business until the user asks), but for the clock.
            const bool clock = status.failure == update::Failure::Net && status.net == update::NetResult::Clock;
            g_at_start = update::ClockPopupDue(clock, g.app_cfg.clock_warned) ? AtStart::ClockDue : AtStart::Over;
            return false;
        }
        if (status.stage == update::Stage::Checked) {
            ForgetClockWarning(); // (GitHub's certificate passed: the clock is right, and the next time it goes wrong the popup may come again)
        }
        const update::Target target = CurrentTarget();
        if (status.stage != update::Stage::Checked || target == update::Target::None || !update::AskAtStart(status.latest, setup::kVersion, g.app_cfg.update_not_now)) {
            g_at_start = AtStart::Over;
            return false;
        }
        if (!quiet) {
            return false;
        }
        g_at_start = AtStart::Over;
        PushPage(new UpdatePromptPage(status.latest, status.whats_new, target));
        return true;
    }
    case AtStart::ClockDue:
        if (!quiet) {
            return false;
        }
        g_at_start = AtStart::Over;
        g.app_cfg.clock_warned = true;
        g.SaveAppConfig();
        PushPage(NewMessagePage(update::kClockTitle, update::kClockText));
        return true;
    default:
        return false;
    }
}

} // namespace pages
