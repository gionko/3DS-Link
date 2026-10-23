// Page: 3DS Link Delivery. This console becomes a Download Play host: a console that has only the stock Download Play (and Luma3DS) lists "3DS Link",
// downloads it from here, installs it and starts it by itself. One console at a time: the room has two places (this console and one seat) and is
// made again after every console. The work on the air is the library in dlph/ (a copy of DlpLink/host, dlph/README.md); this file is the screen
// and the joining of the library to the rest of the app:
//   * the package that is handed out is <data folder>/dlp/child.cia (paths.hpp). BEFORE the room opens the page looks at it: when it is there and is this
//     version of the app it goes straight to the room; otherwise it builds it with the composer (ConfigApp/dlpc) from the app's own CIA and the plugin that is a
//     resource of the app (never a file of the card), in a thread of its own, with a screen that says what it does and how long it takes (dlp_prepare.hpp; DlpLink/docs/COMPOSITORE.md). The room is never open
//     while the package is built;
//   * the whole feature is behind Settings > Download Play delivery (the emergency switch, in 3DSLink.cfg) and behind boot9.bin: sharing needs it, 3DS Link
//     does not. The main page does not push this page when either is missing; the page looks again before it does anything (a second lock), and with the
//     switch off nothing of the library or the composer runs, and nothing is read or written for them;
//   * the library runs in a thread of its own (dlph_poll blocks for up to 0.6 s while it sends a block and 1-4 s while it makes the room again, which a
//     screen must not wait for); the page reads the library's snapshot (dlph_get_stats, dlph_history) and only asks it things (dlph_request_kick);
//   * the local wireless is the app's Wireless (the sockets are put away while the room lives), sleep and HOME are off for as long as it is up, and
//     the words of the screens and the small decisions are in dlp_text.hpp (tested on a PC);
//   * the guide "How to get boot9.bin" opens from a key of the page while the room is up, on top of the page: the room and its thread go on, the screens are
//     lit for it, and B brings the page back (one console shows the guide while the other does the steps).
// The texts are English like the rest of the app. The scan of the titles is looked at only when the package has to be made from the installed 3DS Link (the key
// for its files comes with the scan's context); nothing here touches the Plugin Loader.
#include <3ds.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../dlph/dlph.h"
#include "../guide/guide.hpp"
#include "delivery.hpp"
#include "dlp_prepare.hpp"
#include "dlp_text.hpp"
#include "pages.hpp"
#include "paths.hpp"
#include "plugin_resource.hpp"
#include "system.hpp"
#include "titles.hpp"

using namespace ui;
using delivery::DrawPair;

namespace {

// The thread of the library: it makes calls that wait on the air, so it has a stack of its own (the library itself needs little).
constexpr u32 kRadioStack = 128 * 1024;
// The pause between two fragments, measured on hardware (DlpLink/host/dlph.h): 3000 us downloads a 2.65 MB package in about 10 s with 1-3 fragments in
// 182 sent again; no pause is slower (17 s and 2.4 times the bytes on the air: the radio is the limit, and the console cannot read what comes faster).
constexpr int kGapUs = 3000;
// The language of the name and description the other console shows (an SMDH language index; 0xFF = the language of this console).
constexpr u8 kChildLanguage = 0xFF;

enum Phase { kStarting = 0, kRunning, kFailed };

// Where the page is before the room (and in it): the question about a run that did not end, the wait for the scan of the titles, the work of building the package.
enum class Step { Room, Ask, WaitScan, Preparing };

// What the page and the thread of the library share. Everything the library does (open, start, poll, stop, close) is done by the thread; the page
// only reads the snapshot, sets `stop` and waits for `done`.
struct Radio {
    Thread thread = nullptr;
    std::atomic<int> phase{kStarting};
    std::atomic<int> failure{static_cast<int>(dlp::Failure::None)};
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false}; // the thread is past its last call into the library
    std::atomic<int> channel{0};   // the radio channel of the room (1, 6 or 11), 0 until the wireless service says: the top screen shows it as "CH n"
    LightLock lock;                // guards last_line
    char last_line[160] = "";      // the library's last line: what it says when it refuses to start
    std::string child_path;        // (the library keeps the pointer: it lives as long as the thread)
    dlph_cfg cfg{};
};

void OnLog(const char* line, void* user) {
    Radio* radio = static_cast<Radio*>(user);
    LightLock_Lock(&radio->lock);
    std::snprintf(radio->last_line, sizeof(radio->last_line), "%s", line);
    LightLock_Unlock(&radio->lock);
    dl_log_line(line);
}

void OnEvent(dlph_event ev, int arg, void*) {
    static const char* const kNames[] = {"ROOM_OPEN", "CLIENT_JOINED", "PROGRESS", "CLIENT_SERVED", "CLIENT_LOST", "ROOM_REMADE", "CLIENT_NAME"};
    if (ev == DLPH_EV_PROGRESS && arg % 10 != 0 && arg < 99) {
        return; // (one line in ten of the progress is enough for the log)
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "event %s %d", static_cast<unsigned>(ev) < 7 ? kNames[ev] : "?", arg);
    dl_log_line(buf);
}

// The thread of the library. dlph_poll has to be called at least ten times a second and does not wait when there is nothing to do, so the thread
// sleeps a little between two calls. It is a little more important than the screen (the radio keeps its rhythm while a frame is drawn).
void RadioMain(void* arg) {
    Radio* radio = static_cast<Radio*>(arg);
    bool opened = dlph_open(&radio->cfg);
    bool started = false;
    if (!opened) {
        radio->failure.store(static_cast<int>(dlp::Failure::BadChild));
    } else {
        started = dlph_start();
        if (!started) {
            radio->failure.store(static_cast<int>(dlp::Failure::Room));
        }
    }
    radio->phase.store(started ? kRunning : kFailed);
    // The channel is the firmware's choice, made again with every room: it is read here, on the thread that makes the room (the call goes to the wireless
    // service, which is busy while a room is being made: the screen must not wait on it), when the room is new and, while it is not known, once a second.
    uint32_t seen_remade = 0xFFFFFFFFu;
    int retry = 0;
    while (started && !radio->stop.load()) {
        dlph_poll();
        const dlph_stats s = dlph_get_stats();
        if (!s.remaking && (s.remade != seen_remade || (radio->channel.load() == 0 && ++retry >= 100))) {
            const int channel = dl_wifi_channel();
            radio->channel.store(channel);
            retry = 0;
            if (channel != 0) {
                seen_remade = s.remade;
            }
        }
        svcSleepThread(10ULL * 1000000ULL);
    }
    if (started) {
        dlph_stop(); // (before the wireless is closed: the page closes it when `done` is set)
    }
    if (opened) {
        dlph_close();
    }
    radio->done.store(true);
}

// The address of this console from the sockets service (the source proven on hardware), and the one on the shared page the app uses elsewhere.
bool ReadSocMac(unsigned char mac[6]) {
    if (!g.soc_ok()) {
        return false;
    }
    unsigned char tmp[6] = {};
    socklen_t len = 6;
    if (R_FAILED(SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_MAC_ADDRESS, tmp, &len)) || len != 6) {
        return false;
    }
    std::memcpy(mac, tmp, 6);
    return true;
}

u32 StateColor(const dlph_stats& s) {
    if (s.remaking) {
        return col::kWarn;
    }
    switch (s.state) {
    case DLPH_ROOM_OPEN:
    case DLPH_CLIENT_FINISHING:
        return col::kOn;
    case DLPH_CLIENT_JOINING:
    case DLPH_CLIENT_DOWNLOADING:
        return col::kAccent;
    default:
        return col::kOff;
    }
}

u32 ToneColor(dlp::Tone tone) {
    switch (tone) {
    case dlp::Tone::Good:
        return col::kOn;
    case dlp::Tone::Bad:
        return col::kOff;
    case dlp::Tone::Warn:
        return col::kWarn;
    default:
        return col::kDim;
    }
}

class DlpHostPage : public Page {
public:
    DlpHostPage() {
        Begin(false);
    }

    ~DlpHostPage() override {
        Shutdown(); // (the app is closing with the room up: the thread is asked to stop and waited for, a few seconds at most)
    }

    void Update(const Input& raw) override {
        Input in = raw;
        if (dl_screens_off()) {
            // With the backlights out only SELECT counts: a B in a pocket must not open the question that nobody can see.
            const u32 select = in.down & KEY_SELECT;
            in = Input();
            in.down = select;
        }
        const FooterRects r = LayoutFooter(MakeFooter());
        if (radio_ && !failed_ && !guide_open_ && ((in.down & KEY_SELECT) || Tapped(in, r.extra[0]))) {
            dl_toggle_screens();
        }
        Refresh();
        if (guide_open_) {
            // The guide has the keys (and the screens) until B closes it; the room goes on in its thread, and Refresh() above keeps reading what it says. The key that
            // opened the guide was read in an earlier call: it is not passed on to the viewer (L, R and the D-pad's left and right are its steps).
            if (!viewer_.Update(in.down, in.touch_pressed ? in.tx : -1, in.touch_pressed ? in.ty : -1)) {
                viewer_.Close();
                guide_open_ = false;
            }
            return;
        }
        if (closing_) {
            if (radio_ == nullptr || radio_->done.load()) {
                Leave(true);
            }
            return;
        }
        if (!failed_ && step_ != Step::Room) {
            UpdateBeforeRoom(in, r);
            return;
        }
        if (failed_ || radio_ == nullptr) {
            if (failed_ && RetryOffered() && ((in.down & KEY_A) || Tapped(in, r.action))) {
                Retry();
            } else if (failed_ && failure_ == dlp::Failure::NoBoot9 && ((in.down & KEY_Y) || Tapped(in, r.extra[0]))) {
                OpenGuide();
            } else if ((in.down & KEY_B) || Tapped(in, r.back)) {
                Leave(false);
            }
            return;
        }
        if (confirming_) {
            const PopupBox box = ConfirmBox();
            if ((in.down & KEY_A) || Tapped(in, box.yes)) {
                BeginClose();
            } else if ((in.down & KEY_B) || Tapped(in, box.no)) {
                confirming_ = false;
            }
            return;
        }
        if (radio_->phase.load() == kRunning && stats_.seat != 0 && ((in.down & KEY_X) || Tapped(in, SendAwayRect()))) {
            dlph_request_kick(); // (applied by the next poll: the console leaves and the room is made again)
            Log("the user sent the console away");
            g.ShowToast("Sending the console away...", col::kText, 2500);
        }
        if ((in.down & KEY_Y) || Tapped(in, r.extra[1])) {
            OpenGuide(); // (a key that is free while the room is up: the other console's user can follow the steps on this one)
            return;
        }
        if ((in.down & KEY_B) || Tapped(in, r.back)) {
            if (radio_->phase.load() == kRunning && dlp::CloseNeedsQuestion(stats_.seat)) {
                confirming_ = true;
            } else {
                BeginClose();
            }
        }
    }

    void DrawTop() override {
        if (guide_open_) {
            viewer_.DrawTop();
            return;
        }
        if (!failed_ && step_ != Step::Room) {
            DrawBeforeRoomTop();
            return;
        }
        DrawTitleBarStatus(gfx::kTopW, "3DS Link Delivery", failed_ || radio_ == nullptr ? 0 : radio_->channel.load());
        const bool starting = Starting();
        const dlph_stats& s = stats_;
        const u32 color = failed_ ? col::kOff : (starting ? col::kWarn : StateColor(s));

        // What is going on, and what is offered.
        DrawCard(10, 30, 380, 58);
        gfx::FillRect(10, 30, 3, 58, color);
        gfx::FillCircle(24, 44, 4.0f, color);
        std::string line;
        if (failed_) {
            line = dlp::FailureTitle(failure_);
        } else if (starting) {
            line = "Opening the room...";
        } else {
            line = dlph_state_text(s);
        }
        gfx::DrawFitted(34, 44, 0.52f, 0.4f, 346, col::kText, gfx::Clean(line));
        if (!failed_ && !starting && s.child_bytes > 0) {
            const std::string version = dlp::VersionText(s.child_version);
            const std::string name = s.child_name[0] ? gfx::Clean(s.child_name) : std::string("3DS Link");
            DrawPair(22, 60, 0.42f, "Offering", name + (version.empty() ? std::string() : " v" + version) + ", " + delivery::SizeText(s.child_bytes));
            float x = DrawPair(22, 75, 0.4f, "delivered", Fmt("%lu", static_cast<unsigned long>(s.served)), col::kOn);
            x = DrawPair(x, 75, 0.4f, "lost", Fmt("%lu", static_cast<unsigned long>(s.lost)), s.lost > 0 ? col::kWarn : col::kText);
            DrawPair(x, 75, 0.4f, "room made again", Fmt("%lu x", static_cast<unsigned long>(s.remade)));
        }

        // Below: the steps for the other console while nobody is in, what happens while one is, or why the room is not open.
        const float y = 92.0f, h = 142.0f;
        DrawCard(10, y, 380, h);
        if (failed_) {
            gfx::FillRect(10, y, 3, h, col::kOff);
            gfx::DrawText(26, y + 16, 0.55f, col::kOff, gfx::Ellipsize(dlp::FailureTitle(failure_), 0.55f, 350));
            gfx::DrawWrapped(26, y + 30, 354, 0.43f, col::kText, gfx::Clean(dlp::FailureText(failure_, failure_detail_)), 8);
        } else if (starting || s.seat == 0) {
            gfx::FillRect(10, y, 3, h, col::kAccent);
            gfx::DrawText(26, y + 13, 0.52f, col::kAccent, "On the other console");
            // Five steps of one or two lines each: the type is small and the loop stops at the bottom of the card, whatever the font makes of the words.
            constexpr float kStepScale = 0.38f;
            float yy = y + 24.0f;
            for (int i = 0; i < dlp::kStepCount && yy + gfx::LineHeight(kStepScale) <= y + h - 4.0f; ++i) {
                gfx::DrawText(26, yy + gfx::LineHeight(kStepScale) * 0.5f, kStepScale, col::kAccent, Fmt("%d", i + 1));
                yy += gfx::DrawWrapped(38, yy, 342, kStepScale, col::kText, dlp::StepText(i, gfx::Clean(g.console_name)), 2) + 2.0f;
            }
        } else {
            DrawVisitor(y, h, color);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (guide_open_) {
            viewer_.DrawBottom();
            return;
        }
        if (!failed_ && step_ != Step::Room) {
            DrawBeforeRoomBottom();
            return;
        }
        const bool running = radio_ != nullptr && radio_->phase.load() == kRunning && !failed_;
        DrawTitleBar(gfx::kBottomW, "Consoles", running ? Fmt("%lu served", static_cast<unsigned long>(stats_.served)) : std::string());

        // Who is in the room now.
        DrawCard(8, 32, 304, 56);
        if (!running) {
            gfx::DrawText(160, 54, 0.5f, failed_ ? col::kOff : col::kText, failed_ ? "Nothing is offered" : "Opening the room...", gfx::Align::Center);
            if (!failed_) {
                DrawSpinner(72.0f);
            }
        } else if (stats_.seat == 0) {
            gfx::DrawText(160, 52, 0.5f, col::kText, stats_.remaking ? "Making the room again..." : "Waiting for a console", gfx::Align::Center);
            DrawSpinner(70.0f);
        } else {
            gfx::FillRect(8, 32, 3, 56, StateColor(stats_));
            gfx::DrawFitted(22, 48, 0.52f, 0.4f, 170, col::kText, gfx::Clean(dlp::ConsoleName(stats_.client_name, stats_.seat)));
            gfx::DrawFitted(22, 68, 0.42f, 0.34f, 170, StateColor(stats_), gfx::Clean(ShortState()));
            const Rect away = SendAwayRect();
            DrawButton(away, "X", "Send away", true, PressedNow(g.input.in(), away), true);
        }

        // The consoles that have been in the room, the last one first.
        gfx::DrawText(14, 102, 0.4f, col::kDim, "Last consoles");
        if (nhist_ == 0) {
            gfx::DrawText(160, 150, 0.42f, col::kDim, running ? "None yet" : "", gfx::Align::Center);
        }
        for (int i = 0; i < nhist_ && i < kShownHistory; ++i) {
            const dlph_visit& v = hist_[i];
            const float yc = 118.0f + 18.0f * static_cast<float>(i);
            const u32 color = ToneColor(dlp::VisitTone(static_cast<int>(v.result), v.already_had));
            gfx::FillCircle(18, yc, 3.5f, color);
            gfx::DrawFitted(28, yc, 0.42f, 0.34f, 120, col::kText, Fmt("#%lu ", static_cast<unsigned long>(v.number)) + gfx::Clean(v.name[0] ? v.name : "(no name)"));
            gfx::DrawText(156, yc, 0.4f, color, dlp::VisitWord(static_cast<int>(v.result), v.already_had));
            gfx::DrawText(306, yc, 0.38f, col::kDim, dlp::DurationText(v.duration_ms), gfx::Align::Right);
        }

        DrawFooterBackground();
        DrawFooter(MakeFooter());
        DrawFrameBorder(gfx::kBottomW);
        if (confirming_) {
            const PopupBox box = ConfirmBox();
            DrawDim(gfx::kBottomW);
            DrawPopupBox(box, "Close the room?", dlp::kCloseQuestion);
            DrawButton(box.yes, "A", "Yes, close it", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
            DrawFrameBorder(gfx::kBottomW);
        } else if (closing_) {
            DrawDim(gfx::kBottomW);
            DrawCard(60, 90, 200, 60);
            gfx::FillRect(60, 90, 3, 60, col::kAccent);
            gfx::DrawText(160, 108, 0.52f, col::kText, "Closing the room", gfx::Align::Center);
            DrawSpinner(130.0f);
            DrawFrameBorder(gfx::kBottomW);
        }
    }

private:
    static constexpr int kShownHistory = 5;

    // The console in the room, on the top screen: who, how far, how fast, how long, and what happens next.
    void DrawVisitor(float y, float h, u32 color) {
        const dlph_stats& s = stats_;
        gfx::FillRect(10, y, 3, h, color);
        gfx::FillCircle(24, y + 15, 4.0f, color);
        gfx::DrawFitted(34, y + 15, 0.55f, 0.4f, 236, col::kText, gfx::Clean(dlp::ConsoleName(s.client_name, s.seat)));
        gfx::DrawText(382, y + 15, 0.44f, color, gfx::Clean(ShortState()), gfx::Align::Right);
        if (s.already_has) {
            gfx::DrawWrapped(22, y + 32, 358, 0.44f, col::kWarn,
                             "This console already has this version of 3DS Link, so nothing is sent. Remove it there to send it again.", 4);
            return;
        }
        const bool sending = s.state == DLPH_CLIENT_DOWNLOADING || s.state == DLPH_CLIENT_FINISHING;
        DrawProgressBar(22, y + 30, 356, 10, static_cast<float>(s.percent) / 100.0f);
        gfx::DrawText(22, y + 52, 0.46f, col::kText, Fmt("%d%% sent", s.percent));
        if (s.state == DLPH_CLIENT_DOWNLOADING) {
            gfx::DrawText(382, y + 52, 0.44f, col::kText, dlp::TimeLeftText(s.eta_s), gfx::Align::Right);
        }
        if (sending) {
            float x = DrawPair(22, y + 70, 0.42f, "speed", dlp::SpeedText(s.mibps), col::kAccent);
            DrawPair(x, y + 70, 0.42f, "repeated", dlp::RepeatedText(s.frags_repeated, s.frags_total), s.frags_repeated > s.frags_total / 20 ? col::kWarn : col::kText);
        }
        float x = DrawPair(22, y + 86, 0.42f, "in the room", dlp::DurationText(s.client_ms));
        DrawPair(x, y + 86, 0.42f, "last packet", Fmt("%.1f s ago", static_cast<double>(s.silent_ms) / 1000.0), s.silent_ms > 3000 ? col::kWarn : col::kText);
        gfx::DrawWrapped(22, y + 106, 358, 0.38f, col::kDim,
                         s.state == DLPH_CLIENT_FINISHING ? "Sent. The console leaves to install 3DS Link, which this screen cannot see."
                                                          : "100% means sent: the installation is on the other console, which this screen cannot see.",
                         2);
    }

    // The thread is opening the room (or has just said it could not, and the page has not yet taken that in).
    bool Starting() const {
        return radio_ != nullptr && !failed_ && radio_->phase.load() != kRunning;
    }

    // One short word for what the console in the room is doing.
    std::string ShortState() const {
        if (stats_.already_has) {
            return "Has this version";
        }
        switch (stats_.state) {
        case DLPH_CLIENT_JOINING:
            return "Joining";
        case DLPH_CLIENT_DOWNLOADING:
            return Fmt("Downloading %d%%", stats_.percent);
        case DLPH_CLIENT_FINISHING:
            return "Delivered";
        default:
            return "In the room";
        }
    }

    Footer MakeFooter() const {
        Footer f;
        f.start = false;
        if (failed_) {
            f.back_label = "Back";
            if (RetryOffered()) {
                f.action_label = "Try again"; // (A tries again, B leaves)
            }
            if (failure_ == dlp::Failure::NoBoot9) {
                f.n_extra = 1;
                f.extra[0] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y};
            }
            return f;
        }
        if (radio_ == nullptr) {
            switch (step_) {
            case Step::Ask:
                f.back_label = "Leave";
                f.action_label = "Try again";
                break;
            case Step::Preparing:
                f.back_label = "Stop";
                break;
            default:
                f.back_label = "Back";
                break;
            }
            return f;
        }
        f.back_label = "Close";
        f.n_extra = 2;
        f.extra[0] = FooterKey{"SELECT", dl_screens_off() ? "Screens on" : "Screens off", KEY_SELECT};
        f.extra[1] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y}; // (the guide "How to get boot9.bin": free while the room is up)
        return f;
    }

    static PopupBox ConfirmBox() {
        return LayoutPopup(dlp::kCloseQuestion, true);
    }

    static PopupBox CancelBox() {
        return LayoutPopup(dlp::kCancelPrepareQuestion, true);
    }

    static Rect SendAwayRect() {
        return Rect{206.0f, 44.0f, 98.0f, 28.0f};
    }

    __attribute__((format(printf, 2, 3))) void Log(const char* fmt, ...) {
        if (!log_open_) {
            return;
        }
        char buf[200];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        dl_log_line(buf);
    }

    void Fail(dlp::Failure failure, const std::string& detail) {
        failed_ = true;
        failure_ = failure;
        failure_detail_ = detail;
        Log("not opened: %s %s", dlp::FailureTitle(failure), detail.c_str());
        EndPrepareHolds();
        ReleaseResources();
    }

    // ---------------------------------------------------------------------------------------------
    // Before the room: the locks, the package, the work of making it
    // ---------------------------------------------------------------------------------------------

    // The start, and the start again of "Try again" (`force`: make the package even though the file looks good, because the library refused it). The two locks come
    // first and touch nothing; then the package: good where it lies, the room opens; otherwise it is made.
    void Begin(bool force) {
        failed_ = false;
        failure_detail_.clear();
        step_ = Step::Room;
        if (!g.app_cfg.dlp_delivery) {
            Fail(dlp::Failure::SwitchedOff, std::string()); // (the main page does not push this page when the switch is off: this is the second lock)
            return;
        }
        g.RefreshBoot9();
        if (!g.boot9.found) {
            Fail(dlp::Failure::NoBoot9, std::string()); // (sharing needs boot9.bin; 3DS Link does not)
            return;
        }
        if (!log_open_) {
            dl_log_open("dlp");
            log_open_ = true;
        }
        const unsigned version16 = dlp::Version16(setup::kVersion);
        Log("3DS Link Delivery %s (%u): package %s, plugin: the one inside this app (%llu bytes)", setup::kVersion, version16, g.paths.dlp_child.c_str(),
            static_cast<unsigned long long>(setup::EmbeddedPluginSize()));
        if (dlp::ModelVersion() == 0) {
            Fail(dlp::Failure::NoModel, std::string());
            return;
        }
        const dlp::Verdict verdict = force ? dlp::Verdict::NotCia : dlp::CheckChildFile(g.paths.dlp_child, version16);
        if (verdict == dlp::Verdict::Good) {
            Open();
            return;
        }
        Log("the package is not good (%d): it is made", static_cast<int>(verdict));
        died_ = dlp::DiedRuns(g.paths.dlp_marker);
        if (died_ > 0 && !asked_) {
            step_ = Step::Ask; // (a run that did not end: the user is told before it is tried again, not looped on silently)
            Log("the marker of a run that did not end is there (%d in a row)", died_);
            return;
        }
        BeginPrepare();
    }

    // Where the package is made from, and whether it can be: the app's own CIA as a file, or the installed 3DS Link read through the module (that needs the scan of
    // the titles, which holds the keys: the page waits for it); the plugin; room on the card. Then the work begins.
    void BeginPrepare() {
        const unsigned version16 = dlp::Version16(setup::kVersion);
        u64 app_bytes = 0;
        const dlp::Verdict app_file = dlp::CheckAppCiaFile(g.paths.dlp_app_cia, version16, &app_bytes, &app_found_);
        app_file_ = app_file;
        const bool scan_done = titles::Scan().done();
        const bool keys = scan_done && delivery::AttachModule() && dl_have_keys() != 0;
        const dlp::Source source = dlp::ChooseSource(app_file, scan_done, keys);
        Log("sources: dlp/3dslink.cia is %d, scan %s, keys %s -> %d", static_cast<int>(app_file), scan_done ? "done" : "running", keys ? "there" : "not there",
            static_cast<int>(source));
        if (source == dlp::Source::WaitScan) {
            step_ = Step::WaitScan;
            return;
        }
        if (source == dlp::Source::None) {
            FailNoSource(dlp::Failure::NoKeys, false); // (the file is no good or not there, and the key for the installed copy cannot be made: say exactly that)
            return;
        }
        // The plugin that goes in the package is the one inside this app (a resource: whatever is on the card, the package carries exactly this build's plugin, and no
        // plugin file has to exist). A build whose resource is not a plugin of this version has none to give.
        if (!setup::EmbeddedPluginUsable()) {
            Fail(dlp::Failure::NoPlugin, std::string());
            return;
        }
        const u64 plugin_size = setup::EmbeddedPluginSize();
        if (!setup::MakeDirs(g.paths.dlp_dir)) {
            Fail(dlp::Failure::Prepare, dlp::PrepareFailedText(dlp::Sentence(dlp::PrepPhrase(dlp::kPrepNoSpace)), dlp::PrepAdvice(dlp::kPrepNoSpace)));
            prep_can_retry_ = true;
            return;
        }
        const u64 app_estimate = app_bytes > 0 ? app_bytes : 3ull << 20;
        setup::ReadConsoleStatus(g.status, true);
        if (g.status.sd_known && g.status.sd_free < dlp::SpaceNeeded(source == dlp::Source::Title, app_estimate, app_estimate + plugin_size)) {
            Fail(dlp::Failure::Prepare, dlp::PrepareFailedText(dlp::Sentence(dlp::PrepPhrase(dlp::kPrepNoSpace)), dlp::PrepAdvice(dlp::kPrepNoSpace)));
            prep_can_retry_ = true;
            return;
        }

        dlp::PrepareJob job;
        job.out_path = g.paths.dlp_child;
        job.app_cia_path = g.paths.dlp_app_cia;
        job.marker_path = g.paths.dlp_marker;
        job.version_text = setup::kVersion;
        job.version16 = version16;
        job.use_file = source == dlp::Source::File;
        job.allow_title = keys;
        job.app_bytes = job.use_file ? app_bytes : 0;
        job.attempt = died_ + 1;
        prep_.reset(new dlp::Preparer);
        const int rc = prep_->Start(job);
        if (rc != 0) {
            prep_.reset();
            FailPrepare(rc, std::string());
            return;
        }
        step_ = Step::Preparing;
        holds_.Take(); // (HOME and sleep are off, and the keys of the scan's context stay, until the work ends)
        Log("the work begins: %s", job.use_file ? "from dlp/3dslink.cia" : "from the installed 3DS Link");
    }

    // The sentence on the screen when the work went wrong: the composer's (or our) phrase, what the composer said besides (never the phrase twice), what to do
    // about it, and whether A offers another try.
    void FailPrepare(int rc, const std::string& extra) {
        const char* own = dlp::PrepPhrase(rc);
        const std::string phrase = own ? own : (rc < 100 ? dlpc_result_text(static_cast<dlpc_result>(rc)) : "it went wrong");
        prep_can_retry_ = dlp::CanTryAgain(rc);
        Fail(dlp::Failure::Prepare, dlp::PrepareFailedText(dlp::FailureSentence(phrase, extra), dlp::PrepAdvice(rc)));
    }

    // The way to the app's own copy is shut (the installed 3DS Link cannot be read: no key, or no installed title). A file in the dlp folder that cannot be used is named
    // first, because "the key is missing" alone would send the user to look at the wrong thing; `refused` says that the file looked good and the composer would not
    // take it. Otherwise it is the plain case, `plain` (the file is not there).
    void FailNoSource(dlp::Failure plain, bool refused) {
        if (dlp::FileIsToBlame(app_file_) || refused) {
            const std::string found = app_file_ == dlp::Verdict::WrongVersion ? dlp::VersionText(app_found_) : std::string();
            Fail(dlp::Failure::BadAppFile, dlp::BadAppFileText(g.paths.dlp_app_cia, found, setup::kVersion));
            return;
        }
        Fail(plain, g.paths.dlp_dir);
    }

    // HOME and sleep come back (the work is over, or was stopped, or the page is going).
    void EndPrepareHolds() {
        holds_.Give();
    }

    // The keys of the screens before the room. `r` is the footer's buttons.
    void UpdateBeforeRoom(const Input& in, const FooterRects& r) {
        const bool back = (in.down & KEY_B) || Tapped(in, r.back);
        switch (step_) {
        case Step::Ask:
            if ((in.down & KEY_A) || Tapped(in, r.action)) {
                asked_ = true;
                step_ = Step::Room;
                BeginPrepare();
            } else if (back) {
                Leave(false); // (the marker stays: the question comes again next time)
            }
            break;
        case Step::WaitScan:
            if (titles::Scan().done()) {
                BeginPrepare();
            } else if (back) {
                Leave(false);
            }
            break;
        case Step::Preparing:
            UpdatePreparing(in, back);
            break;
        default:
            break;
        }
    }

    void UpdatePreparing(const Input& in, bool back) {
        if (prep_ == nullptr) {
            return;
        }
        if (prep_->Finished()) {
            confirming_cancel_ = false;
            FinishPrepare();
            return;
        }
        if (confirming_cancel_) {
            const PopupBox box = CancelBox();
            if ((in.down & KEY_A) || Tapped(in, box.yes)) {
                prep_->Cancel(); // (the composer stops within one slice and leaves nothing behind; the page waits for it)
                cancelling_ = true;
                confirming_cancel_ = false;
                Log("the user stopped the work");
            } else if ((in.down & KEY_B) || Tapped(in, box.no)) {
                confirming_cancel_ = false;
            }
            return;
        }
        if (back && !cancelling_) {
            confirming_cancel_ = true;
        }
    }

    // The work is over (the thread has made its last call).
    void FinishPrepare() {
        EndPrepareHolds();
        const dlp::PrepareState state = prep_->state();
        const int rc = prep_->result();
        const std::string detail = prep_->detail();
        const int title_error = prep_->title_error();
        const bool refused = prep_->file_refused();
        prep_.reset();
        cancelling_ = false;
        if (state == dlp::PrepareState::Done) {
            Log("the package is made: %s", g.paths.dlp_child.c_str());
            step_ = Step::Room;
            Open();
        } else if (state == dlp::PrepareState::Cancelled) {
            g.ShowToast("Stopped. Nothing was changed.", col::kText, 4000);
            Leave(false);
        } else if (rc == dlp::kPrepNoCopy && title_error == DL_CIA_NO_TITLE) {
            FailNoSource(dlp::Failure::NoSource, refused); // (3DS Link was started as a 3dsx: there is no installed title to read)
        } else if (rc == dlp::kPrepNoCopy && title_error == DL_CIA_NO_KEYS) {
            FailNoSource(dlp::Failure::NoKeys, refused);
        } else {
            FailPrepare(rc, detail);
        }
    }

    // A on a failure that another try can change: the package is looked at and made again (the library refusing a package that looked good makes it from scratch).
    void Retry() {
        const bool force = failure_ == dlp::Failure::BadChild;
        Log("the user tries again");
        // The library's thread (it said the package was no good: it is done) is joined and freed and what the page held goes back BEFORE anything is made: the room is
        // never open, and the library holds no file, while the package is built again.
        Shutdown();
        confirming_ = closing_ = false;
        asked_ = true; // (the question about an earlier run was answered; this is the user's own second try)
        Begin(force);
    }

    bool RetryOffered() const {
        return failure_ == dlp::Failure::BadChild || (failure_ == dlp::Failure::Prepare && prep_can_retry_);
    }

    void OpenGuide() {
        if (!viewer_.Open()) {
            PushPage(pages::NewMessagePage("Guide", dlp::kNoGuideToast)); // (this page does not draw toasts)
            return;
        }
        if (dl_screens_off() != 0) {
            dl_toggle_screens(); // (the guide needs the screens lit)
        }
        guide_open_ = true;
    }

    // What the screens say before the room.
    void DrawBeforeRoomTop() {
        DrawTitleBarStatus(gfx::kTopW, "3DS Link Delivery");
        DrawCard(10, 30, 380, 204);
        gfx::FillRect(10, 30, 3, 204, step_ == Step::Ask ? col::kWarn : col::kAccent);
        if (step_ == Step::Ask) {
            gfx::DrawText(26, 50, 0.58f, col::kWarn, "The last preparation did not finish");
            gfx::DrawWrapped(26, 70, 354, 0.46f, col::kText, dlp::DiedQuestion(died_), 8);
        } else if (step_ == Step::WaitScan) {
            const titles::Scanner& scan = titles::Scan();
            gfx::DrawText(26, 50, 0.6f, col::kText, dlp::kPrepareTitle);
            gfx::DrawWrapped(26, 68, 354, 0.43f, col::kDim, dlp::kPrepareWaitScanNote, 4);
            gfx::DrawText(26, 134, 0.55f, col::kText, dlp::kPrepareWaitScan);
            DrawProgressBar(26, 148, 348, 14, setup::ProgressFraction(scan.progress_done(), scan.progress_total()));
            gfx::DrawText(26, 176, 0.46f, col::kDim, setup::ScanProgressText(scan.progress()));
        } else if (prep_ != nullptr) {
            const dlp::PrepareView view = prep_->View();
            std::string stage = view.stage == dlp::kStageCopy ? std::string(dlp::kPrepareCopyStage) : std::string(dlpc_stage_text(view.stage));
            gfx::DrawText(26, 50, 0.6f, col::kText, dlp::kPrepareTitle);
            gfx::DrawWrapped(26, 68, 354, 0.43f, col::kDim, dlp::PrepareIntro(view.plan_seconds, view.measured), 4);
            gfx::DrawText(26, 134, 0.55f, col::kText, gfx::Ellipsize(dlp::Capitalized(stage), 0.55f, 350));
            DrawProgressBar(26, 148, 348, 14, static_cast<float>(view.fraction));
            gfx::DrawText(26, 176, 0.46f, col::kText, Fmt("%d%%", static_cast<int>(view.fraction * 100.0)));
            gfx::DrawText(382, 176, 0.44f, col::kText, cancelling_ ? std::string("stopping...") : dlp::TimeLeftText(view.seconds_left), gfx::Align::Right);
            gfx::DrawWrapped(26, 200, 354, 0.38f, col::kDim, dlp::kPrepareKeepAwake, 2);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBeforeRoomBottom() {
        DrawTitleBar(gfx::kBottomW, step_ == Step::Ask ? "Question" : "Please wait", step_ == Step::Preparing ? "one time only" : std::string());
        constexpr float kH = 110.0f;
        const float y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - kH) * 0.5f;
        DrawCard(8, y, 304, kH);
        gfx::FillRect(8, y, 3, kH, step_ == Step::Ask ? col::kWarn : col::kAccent);
        if (step_ == Step::Ask) {
            gfx::DrawText(160, y + 30, 0.55f, col::kText, "Try it again?", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 50, 276, 0.42f, col::kDim, "A tries again. B leaves, and the question comes again next time.", 3);
        } else if (step_ == Step::WaitScan) {
            gfx::DrawText(160, y + 28, 0.55f, col::kText, "Reading your titles", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim, "The package is made as soon as the list is ready.", 3);
            DrawSpinner(y + kH - 18.0f);
        } else {
            gfx::DrawText(160, y + 28, 0.55f, col::kText, "Building the package", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim, "Other consoles download it from here. It is kept on the SD card.", 3);
            DrawSpinner(y + kH - 18.0f);
        }
        DrawFooterBackground();
        DrawFooter(MakeFooter());
        DrawFrameBorder(gfx::kBottomW);
        if (confirming_cancel_) {
            const PopupBox box = CancelBox();
            DrawDim(gfx::kBottomW);
            DrawPopupBox(box, "Stop now?", dlp::kCancelPrepareQuestion);
            DrawButton(box.yes, "A", "Yes, stop", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
            DrawFrameBorder(gfx::kBottomW);
        }
    }

    // Everything that is checked and taken before the thread of the library starts. A step that fails puts away what the earlier ones took.
    void Open() {
        const std::string& child = g.paths.dlp_child;
        struct stat st;
        if (stat(child.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            Fail(dlp::Failure::NoChild, child);
            return;
        }
        if (st.st_size <= 0 || static_cast<u64>(st.st_size) > dlp::kMaxChildBytes) {
            Fail(dlp::Failure::BadChild, child);
            return;
        }
        if (!log_open_) {
            dl_log_open("dlp");
            log_open_ = true;
        }
        Log("3DS Link Delivery %s: child %s, %lu bytes", setup::kVersion, child.c_str(), static_cast<unsigned long>(st.st_size));

        // The address of this console is part of the checksum of what is announced; the library cannot read it by itself. Both sources are
        // written to the log, so that a console where they differ shows it.
        unsigned char soc[6] = {};
        const bool have_soc = ReadSocMac(soc);
        const azl::Mac shared = setup::ReadConsoleMac();
        Log("MAC from the sockets service: %s (%s) | from the shared page: %s", have_soc ? dlp::MacText(soc).c_str() : "unavailable",
            have_soc && dlp::MacUsable(soc) ? "usable" : "not usable", dlp::MacText(shared.data()).c_str());
        unsigned char mac[6] = {};
        if (have_soc && dlp::MacUsable(soc)) {
            std::memcpy(mac, soc, 6);
        } else if (dlp::MacUsable(shared.data())) {
            std::memcpy(mac, shared.data(), 6);
        } else {
            Fail(dlp::Failure::NoMac, std::string());
            return;
        }
        if (have_soc && dlp::MacUsable(soc) && std::memcmp(soc, shared.data(), 6) != 0) {
            Log("the two addresses differ: the one from the sockets service is used");
        }

        if (!wireless_.Open()) {
            Fail(dlp::Failure::Wireless, Fmt("(0x%08lX)", static_cast<unsigned long>(wireless_.error())));
            return;
        }
        dl_power_begin(); // sleep refused, HOME off, the shell watched: with the console asleep the room falls
        power_on_ = true;
        g.HoldHome();
        home_held_ = true;

        radio_ = new Radio;
        LightLock_Init(&radio_->lock);
        radio_->child_path = child;
        dlph_cfg& cfg = radio_->cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.child_path = radio_->child_path.c_str();
        std::memcpy(cfg.mac, mac, 6);
        cfg.lang = kChildLanguage;
        cfg.max_nodes = 2;     // this console and ONE seat: the room is made again after every console
        cfg.persistent = false;
        cfg.gap_us = kGapUs;
        cfg.log = OnLog;
        cfg.event = OnEvent;
        cfg.user = radio_;
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        radio_->thread = threadCreate(RadioMain, radio_, kRadioStack, prio > 0x19 ? prio - 1 : prio, -2, false);
        if (radio_->thread == nullptr) {
            delete radio_;
            radio_ = nullptr;
            Fail(dlp::Failure::Thread, std::string());
            return;
        }
    }

    // The thread has said how it went: copies it, and when the library refused to start puts everything away (the page stays to say why).
    void Refresh() {
        if (radio_ == nullptr) {
            return;
        }
        const int phase = radio_->phase.load();
        if (phase == kRunning) {
            stats_ = dlph_get_stats();
            nhist_ = dlph_history(hist_, 8);
            if (stats_.served > seen_served_ && nhist_ > 0) {
                seen_served_ = stats_.served;
                const std::string who = gfx::Clean(hist_[0].name[0] ? hist_[0].name : "A console");
                g.ShowToast(hist_[0].already_had ? who + " already has this version." : "Delivered to " + who + ". It installs 3DS Link now.", col::kOn, 5000);
            }
        } else if (phase == kFailed && !failed_ && radio_->done.load()) {
            failed_ = true;
            failure_ = static_cast<dlp::Failure>(radio_->failure.load());
            LightLock_Lock(&radio_->lock);
            failure_detail_ = radio_->last_line;
            LightLock_Unlock(&radio_->lock);
            Log("not opened: %s %s", dlp::FailureTitle(failure_), failure_detail_.c_str());
            ReleaseResources();
        }
    }

    void BeginClose() {
        confirming_ = false;
        closing_ = true;
        if (radio_ != nullptr) {
            radio_->stop.store(true);
        }
        Log("close the room: asked by the user");
    }

    // The room is closed (the thread is done): puts everything away and goes back to the menu.
    void Leave(bool toast) {
        if (left_) {
            return;
        }
        left_ = true;
        Shutdown();
        if (toast) {
            g.ShowToast("The room was closed.", col::kText, 4000);
        }
        PopPage();
    }

    // The thread, then the rest, in the reverse order of Open(). Safe to call again.
    void Shutdown() {
        if (guide_open_) {
            viewer_.Close();
            guide_open_ = false;
        }
        if (prep_ != nullptr) {
            prep_.reset(); // (the work is asked to stop and waited for: the composer ends within one slice and leaves nothing behind)
        }
        EndPrepareHolds();
        if (radio_ != nullptr) {
            radio_->stop.store(true);
            if (radio_->thread != nullptr) {
                threadJoin(radio_->thread, U64_MAX); // (U64_MAX never times out: a timeout would read as success)
                threadFree(radio_->thread);
                radio_->thread = nullptr;
            }
            delete radio_;
            radio_ = nullptr;
        }
        ReleaseResources();
    }

    void ReleaseResources() {
        if (power_on_) {
            dl_power_end();
            power_on_ = false;
        }
        wireless_.Close(); // (the module's power helpers and the sockets go back here)
        if (home_held_) {
            g.ReleaseHome();
            home_held_ = false;
        }
        if (log_open_) {
            Log("room closed");
            dl_log_close();
            log_open_ = false;
        }
    }

    delivery::Wireless wireless_;
    Radio* radio_ = nullptr;
    bool log_open_ = false;
    bool power_on_ = false;
    bool home_held_ = false;
    bool failed_ = false;
    bool confirming_ = false;
    bool closing_ = false;
    bool left_ = false;
    dlp::Failure failure_ = dlp::Failure::None;
    std::string failure_detail_;
    // Before the room: the work of making the package, and the guide on top of the page.
    Step step_ = Step::Room;
    std::unique_ptr<dlp::Preparer> prep_;
    dlp::Verdict app_file_ = dlp::Verdict::Missing; // what dlp/3dslink.cia was when the sources were looked at, and the version its TMD said (0 = none): for the words
    unsigned app_found_ = 0;        //   of a failure that has to name that file
    dlp::WorkHolds holds_;         // HOME and sleep are off for the work (given back by EndPrepareHolds, and by its destructor)
    bool prep_can_retry_ = false;   // the failure of the work is one another try can change
    bool confirming_cancel_ = false; // the question "stop now?" is up
    bool cancelling_ = false;       // the work was asked to stop and has not said it has
    bool asked_ = false;            // the question about a run that did not end was answered yes (or the user tried again on their own)
    int died_ = 0;                  // how many runs in a row began and did not end (the marker)
    guide::Viewer viewer_;
    bool guide_open_ = false;
    dlph_stats stats_{};
    dlph_visit hist_[8]{};
    int nhist_ = 0;
    uint32_t seen_served_ = 0;
};

} // namespace

namespace pages {

ui::Page* NewDlpHostPage() {
    return new DlpHostPage;
}

} // namespace pages
