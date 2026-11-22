// Pages: Download Play, the one place of the Download Play children 3DS Link prepares (1.1.4, piece APP-WORKER: 6-APP-boot9.md 4.8 and 6, its critic
// 6-APP-boot9-CRITICO.md 4.3-4.5 and 6 step 6, K9 of 7-CONTRATTO.md; spec 9 of 2026-11-17: analysis/piano-1.1.4-2026-11-15/specifiche/9-DOWNLOAD-PLAY-APP.md).
//   NewDlpPage            the entry of the main page: two rows, Join lobby (the hosts of the room, pages_dlpjoin.cpp) and Enable games (the page
//                         below; the count of the games enabled is on its row, never on Join lobby's, 2026-11-18). When it opens, once, the
//                         records are looked at against the games there are now, in the thread of the look (StartGamesLook with `quiet`): a child whose
//                         game is gone is removed, a repair is made, quietly, behind the run page. A or B pressed meanwhile are done when the look is
//                         over (a moment). Nothing of the children works anywhere else (no switch of Enable Titles, no copy that ends, no page that is
//                         left starts anything, nothing in the background).
//   NewDlpGamesPage       first, the way Enable Titles does it: "Reading your titles..." with the bar of the scan (or the copies of the plugin, with
//                         theirs), then "Checking your games for Download Play..." with its bar: a thread of its own looks at the games (StartGamesLook),
//                         the worker reads the ones never read (one Probe a game: once a game, then only the games installed or updated since; B stops
//                         after the game under way and shows the list), and the screen only draws. Then the games that have a Download Play child
//                         (children.cache, at their versions), each with its icon and a switch. A, or the D-pad as in Enable Titles (right on, left
//                         off: SwitchTarget, the same way as A - 2026-11-18, the console test), on Off: boot9.bin, the plan of THAT game (in the
//                         thread of the look: "Please wait"), then "switched on in Enable Titles too?", then the space and the time, then the
//                         preparation on the run page (HOME and sleep off). A on On: "Remove ...? It frees N MB." A game updated since: "to update", A
//                         makes it again. A look that failed: grey "not read", A reads it again. Games not read yet (stopped): "N games not checked
//                         yet", A reads them. Games whose look stopped at the keys: "N games wait for boot9.bin", not read again by themselves.
//                         X removes them all,
//                         Y opens the guide of boot9.bin.
//                         The top screen, as Enable Titles and Titles Delivery show it: the selected game's icon and name, its status, what A does
//                         and the size, then the SD card with the bar of Titles Delivery (ui::DrawSpaceBar). The bar of the touch screen: "Enabled n / x".
//   the run page          the worker at work: which game of how many, the bar, the time left; B: "stop after this game?". HOME and sleep are off for
//                         the heavy run (dlp::WorkHolds); the quiet one only keeps the scan's context (a few seconds).
#include "copyworker.hpp"
#include "dlp_guest.hpp"
#include "dlp_prepare.hpp"
#include "dlp_text.hpp"
#include "pages.hpp"
#include "status.hpp"
#include "lobby_scan.hpp"
#include "titles.hpp"
#include "update_worker.hpp"

using namespace ui;

namespace {

constexpr const char* kTitle = "Download Play";

const char* DoingText(dlpguest::Act act) {
    switch (act) {
    case dlpguest::Act::Prepare: return "Preparing its Download Play guest";
    case dlpguest::Act::Probe: return "Looking at its Download Play data";
    case dlpguest::Act::Check: return "Looking at its Download Play data again";
    case dlpguest::Act::Remove: return "Removing its Download Play guest";
    case dlpguest::Act::Disown: return "Letting go of a title that is not 3DS Link's";
    case dlpguest::Act::Adopt: return "Taking back its Download Play guest";
    case dlpguest::Act::Repair: return "Repairing its Download Play guest";
    default: return "";
    }
}

std::string Mb(u64 bytes) {
    return setup::FormatSize(bytes);
}

void Guide() {
    if (!pages::OpenBoot9Guide()) {
        g.ShowToast(dlp::kNoGuideToast, col::kWarn, 3500);
    }
}

// The advice of a result, as the pages of this file say it: the one place they take it from (dlp_guest_logic.hpp's Advice, which names the page by
// the name it has now, Download Play > Enable games, since the build 2e of 2026-11-18).
std::string AdviceText(int err) {
    return dlpguest::Advice(err);
}

// The games enabled: a guest made and ours (its record Installed) - one "to update" or "not read" too, whose row says what it needs. The row Enable
// games of the page Download Play and the bar "Enabled n / x" of the page count the same (the review of 2026-11-18: "1 game ready" over "Enabled 0 / 1").
int EnabledGuests() {
    int n = 0;
    for (const dlpguest::Guest& guest : dlpguest::LoadGuests()) {
        n += guest.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(guest.prepared) ? 1 : 0;
    }
    return n;
}

// (the review of the build 2h) the game has a guest of ours that works after a run given up: its record Installed (an Update stopped, AM kept the old)
bool KeepsGuest(u64 game) {
    const u64 prepared = azl::title_rules::PreparedOf(game);
    for (const dlpguest::Guest& guest : dlpguest::LoadGuests()) {
        if (guest.prepared == prepared && guest.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(guest.prepared)) {
            return true;
        }
    }
    return false;
}

// The worker at work. It starts the run at its first frame and leaves when the run is over, with a word on how it went.
class RunPage : public Page {
public:
    // `asked`: the user pressed A on a row for it (a look again, the quiet work before a game is switched on): a failure is said, whatever the job.
    RunPage(const std::vector<dlpguest::Planned>& jobs, bool heavy, bool asked = false) : jobs_(jobs), heavy_(heavy), asked_(asked) {}
    ~RunPage() override {
        Release();
    }

    void Update(const Input& in) override {
        if (!started_) {
            started_ = true;
            const int rc = dlpguest::StartRun(jobs_);
            if (rc != 0) {
                PopPage();
                PushPage(pages::NewMessagePage(kTitle, std::string(dlpguest::Phrase(rc)) + " " + AdviceText(rc)));
                return;
            }
            // HOME and sleep off, the scan's context kept: a child is never cut by a press of HOME or a closed lid. (DPAPP-2h-sec) the quiet run of
            // the page and a removal the user confirmed too, not only the heavy one: a Remove deletes in AM, a Repair writes the folder - every work
            // of the app on AM or the SD card holds them (dlp::WorkHolds), the bar shows "HOME off"
            holds_.Take();
        }
        if (!dlpguest::Running()) {
            Finish();
            return;
        }
        const PopupBox box = StopBox();
        if (confirming_) {
            if ((in.down & KEY_A) || Tapped(in, box.yes)) {
                // (the build 2h, U2: B cancels the work - it does not hide it, nor wait for the end of a guest of minutes. The page stays, with its
                // bar, until the worker's thread is over: nothing is left half made, HOME comes back with Finish)
                dlpguest::StopNow();
                confirming_ = false;
            } else if ((in.down & KEY_B) || Tapped(in, box.no)) {
                confirming_ = false;
            }
            return;
        }
        // (DPAPP-2h-sec) the touch on the button as it is drawn - the whole footer (no START): kBackButton is only its left 68 pixels
        if (((in.down & KEY_B) || Tapped(in, LayoutFooter(StopFooter()).back)) && !dlpguest::View().stopping) {
            confirming_ = true;
        }
    }

    void DrawTop() override {
        const dlpguest::RunView v = dlpguest::View();
        DrawTitleBarStatus(gfx::kTopW, kTitle);
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        const int shown = v.count == 0 ? 0 : (v.index < v.count ? v.index + 1 : v.count);
        gfx::DrawText(26, 50, 0.55f, col::kText, gfx::Ellipsize(Fmt("%d of %d: ", shown, v.count) + dlpguest::GameName(v.game), 0.55f, 350));
        gfx::DrawText(26, 72, 0.46f, col::kDim, DoingText(v.act));
        DrawProgressBar(26, 96, 348, 14, static_cast<float>(v.fraction));
        gfx::DrawText(26, 124, 0.46f, col::kText, Fmt("%d%%", static_cast<int>(v.fraction * 100.0)));
        gfx::DrawText(382, 124, 0.44f, col::kText, v.stopping ? std::string("stopping...") : dlp::TimeLeftText(v.seconds_left), gfx::Align::Right);
        gfx::DrawWrapped(26, 146, 354, 0.40f, col::kDim,
                         heavy_ ? "HOME is off while a guest is prepared: the console must stay on. The guest is hidden from HOME: 3DS Link starts it "
                                  "from Download Play > Join lobby."
                                : (Removals() ? "A few seconds: 3DS Link removes the Download Play guest, its title and its files."
                                              : "A few seconds: 3DS Link looks at the Download Play data of your games."),
                         4);
        DrawStatusStrip(10, 206, 380, 28); // (the build 2h, U5: "3DS Link is ON / Room ..." under the card, as on every other screen)
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawTitleBar(gfx::kBottomW, "Please wait", std::string());
        constexpr float kH = 110.0f;
        // (the build 2h, U6) centred in the space between the title bar and the footer (B Stop): the same black above and below
        const float y = kBarH + 1.0f + (kFooterTop - kBarH - 1.0f - kH) * 0.5f;
        DrawCard(8, y, 304, kH);
        gfx::FillRect(8, y, 3, kH, col::kAccent);
        gfx::DrawText(160, y + 28, 0.55f, col::kText,
                      heavy_ ? "Preparing a Download Play guest" : (Removals() ? "Removing a Download Play guest" : "Looking at your games"), gfx::Align::Center);
        gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim, "B stops it: nothing is left half made.", 3);
        DrawSpinner(y + kH - 18.0f);
        DrawFooterBackground();
        DrawFooter(StopFooter());
        DrawFrameBorder(gfx::kBottomW);
        if (confirming_) {
            const PopupBox box = StopBox();
            DrawDim(gfx::kBottomW);
            DrawPopupBox(box, "Stop now?", kStopText);
            DrawButton(box.yes, "A", "Yes, stop", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
            DrawFrameBorder(gfx::kBottomW);
        }
    }

private:
    static constexpr const char* kStopText = "The work under way is given up: nothing is left half made.";

    static PopupBox StopBox() {
        return LayoutPopup(kStopText, true);
    }
    // The footer of the page, one for the drawing and the touch (B Stop, the whole width: no START)
    static Footer StopFooter() {
        Footer footer;
        footer.back_label = "Stop";
        footer.start = false;
        return footer;
    }

    // Every job is a removal (A on a row that is on, X "Remove all"): the page says that it removes, not that it looks (the review of spec 9).
    bool Removals() const {
        for (const dlpguest::Planned& p : jobs_) {
            if (p.act != dlpguest::Act::Remove && p.act != dlpguest::Act::Disown) {
                return false;
            }
        }
        return !jobs_.empty();
    }

    void Release() {
        holds_.Give();
    }

    // The run is over: HOME comes back, and the user is told how it went (the quiet run of the page says something only when a removal or a repair
    // failed; a look that failed shows on its row of Games: "not read"; a run the user asked for on a row says any failure: a look again that failed
    // for a reason not remembered would otherwise leave the old row and the old reason - the review of spec 9).
    void Finish() {
        std::vector<dlpguest::JobEnd> ends;
        dlpguest::TakeEnds(&ends);
        Release();
        PopPage();
        int ready = 0, asked = 0;
        for (const dlpguest::Planned& p : jobs_) {
            asked += p.act == dlpguest::Act::Prepare ? 1 : 0;
        }
        std::string failed, hosts, cancelled, kept;
        for (const dlpguest::JobEnd& e : ends) {
            if (e.result == dlpguest::kErrCancel && e.job.act == dlpguest::Act::Prepare) {
                // (the build 2h, U2: B stopped it - said once, never as a failure. The review of the build 2h: an Update that was stopped keeps the
                // guest it had - not "enable it again")
                std::string& to = KeepsGuest(e.job.game) ? kept : cancelled;
                to += (to.empty() ? "" : ", ") + dlpguest::GameName(e.job.game);
                continue;
            }
            const bool told = heavy_ || asked_ || e.job.act == dlpguest::Act::Remove || e.job.act == dlpguest::Act::Disown || e.job.act == dlpguest::Act::Repair;
            // (the build 2f) kErrShownInHome: the guest is made and works (the old form, a black icon in HOME): ready, and said with the failures
            const bool made = e.job.act == dlpguest::Act::Prepare && (e.result == 0 || e.result == dlpguest::kErrShownInHome);
            if (made) {
                ++ready;
                hosts += "\n" + dlpguest::GameName(e.job.game) + " - " + dlp::HostsNote(e.job.game); // (B2: who can host it; " - ", the note starts with "Hosts:")
            }
            if (e.result != 0 && told && failed.size() < 600) {
                failed += "\n" + dlpguest::GameName(e.job.game) + ": " + dlpguest::Phrase(e.result) + " " + AdviceText(e.result);
            }
        }
        const bool stopped = ends.size() < jobs_.size();
        if (heavy_) {
            std::string text = ready == asked && failed.empty() ? std::string("Ready: the game can join a host in Download Play > Join lobby.")
                                                                : Fmt("%d of %d guests ready.", ready, asked);
            if (!cancelled.empty() || !kept.empty()) {
                // (the build 2h, U2) the guest B stopped: given up cleanly, nothing of it installed
                text = cancelled.empty() ? std::string("Stopped:") : "Stopped: nothing of " + cancelled + " was installed. Enable it again when you like.";
                if (!kept.empty()) {
                    text += " " + kept + (kept.find(", ") == std::string::npos ? " keeps its guest as it was." : " keep their guests as they were.");
                }
            } else if (stopped) {
                text += " Stopped: enable the game again in Enable games when you like."; // (before the hosts: never on the last game's line)
            }
            text += hosts;
            PushPage(pages::NewMessagePage(kTitle, text + failed));
        } else if (!failed.empty()) {
            PushPage(pages::NewMessagePage(kTitle, "Not everything went well:" + failed));
        }
    }

    std::vector<dlpguest::Planned> jobs_;
    bool heavy_;
    bool asked_;
    bool started_ = false;
    bool confirming_ = false;
    dlp::WorkHolds holds_;
};

// ---- Download Play > Enable games (its row was "Games" until 2026-11-18) ----------------------------------------------------------------

class GamesPage : public ListPage {
public:
    GamesPage() : generation_(titles::ScanGeneration()), runs_(dlpguest::RunsEnded()) {}
    ~GamesPage() override {
        if (phase_ == Phase::Looking || phase_ == Phase::Planning) {
            dlpguest::DropGamesLook(); // (never on the way of B, which waits for the look: its thread reads the scan's games, which the hold keeps)
        }
        Hold(false);
        gfx::ReleaseIcons(); // (the card's icon is this page's own: its texture goes with it)
    }
    void OnResume() override {
        if (phase_ == Phase::List && runs_ != dlpguest::RunsEnded()) {
            Relook(); // (a guest prepared or removed on the run page: the rows are looked at again - once the message over this page is closed)
        }
    }

    void Update(const Input& in) override {
        switch (phase_) {
        case Phase::Titles:
            if (dlpguest::CanLook() && --retry_ < 0) {
                Look(); // (the scan is over, no copy of the plugin waits, no run: the look starts; after a look that could not start, a second later)
                return;
            }
            ListPage::Update(in); // (B goes back)
            return;
        case Phase::Looking:
            if (dlpguest::GamesLookDone()) {
                TakeLook();
            } else if (BackPressed(in)) {
                leave_ = true; // (the look ends by itself in a moment: the page goes then)
            }
            return;
        case Phase::Planning:
            if (dlpguest::GamesLookDone()) {
                TakePlan();
            } else if (BackPressed(in)) {
                skip_ = true; // (the look of the game ends by itself in a moment: then the list, nothing switched on)
            }
            return;
        case Phase::Reading:
            if (!dlpguest::Running()) {
                std::vector<dlpguest::JobEnd> ends;
                dlpguest::TakeEnds(&ends); // (a look that failed shows on its row: "not read")
                Hold(false); // (the page's own hold goes first: CanLook waits for module_holds == 0, so the look again never starts under it - DPAPP-2g)
                Relook(); // (what the reading found: the rows, in the thread of the look)
            } else if (BackPressed(in) && !dlpguest::View().stopping) {
                dlpguest::StopAfterCurrent(); // (the game under way is finished first; then the list: the others are read the next time, or with A)
            }
            return;
        case Phase::List:
            if (generation_ != titles::ScanGeneration()) {
                generation_ = titles::ScanGeneration(); // (the titles were read again - boot9.bin came -: what the rows hold points into the old scan)
                items_.clear();
                phase_ = Phase::Titles;
                read_ = false;
                return;
            }
            ListPage::Update(in);
            return;
        }
    }

    void DrawTop() override {
        if (phase_ == Phase::List) {
            DrawListTop();
            return;
        }
        std::string title, text, detail;
        float fraction = -1.0f;
        Message(title, text, detail, fraction);
        DrawTitleBarStatus(gfx::kTopW, Title());
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        gfx::DrawText(26, 48, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 350));
        gfx::DrawWrapped(26, 62, 350, 0.45f, col::kText, detail.empty() ? text : text + "\n\n" + detail, 9);
        DrawStatusStrip(10, 206, 380, 28);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (phase_ == Phase::List) {
            ListPage::DrawBottom();
            return;
        }
        std::string title, text, detail;
        float fraction = -1.0f;
        Message(title, text, detail, fraction);
        DrawCard(8, 34, 304, 168);
        gfx::DrawText(160, 54, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 280), gfx::Align::Center);
        gfx::DrawWrapped(22, 68, 276, 0.45f, col::kText, text, 6);
        if (fraction >= 0.0f) {
            DrawProgressBar(22, 100, 276, 12, fraction);
        } else {
            DrawSpinner(118);
        }
        DrawListChrome(BarTitle(), BarRight());
        DrawFooter(BackFooter());
    }

protected:
    const char* Title() const override {
        return "Download Play > Enable games";
    }
    const char* BarTitle() const override {
        return "Enable games";
    }
    // How many games are enabled of the games listed, as Titles Delivery says "n / x" (2026-11-18: not the megabytes, which the top screen says). The
    // same count as the row Enable games of the page Download Play (EnabledGuests: a guest made and ours, "to update" or "not read" too).
    std::string BarRight() const override {
        if (phase_ != Phase::List || items_.empty()) {
            return std::string();
        }
        int on = 0;
        for (const Item& it : items_) {
            on += Enabled(it) ? 1 : 0;
        }
        return Fmt("Enabled %d / %d", on, static_cast<int>(items_.size()));
    }
    const char* ActionLabel() const override {
        const Item* it = SelectedItem();
        if (it == nullptr) {
            const Row* row = Selected();
            if (row != nullptr && row->id == kWaitKeys) {
                return keys_ ? "Check" : "Guide";
            }
            return row != nullptr && row->id == kUnread ? "Check" : "";
        }
        switch (it->shown) {
        case dlpguest::Shown::Off: return "Enable";   // (what A does on this row, not "Switch": 2026-11-18)
        case dlpguest::Shown::On: return "Disable";
        case dlpguest::Shown::ToUpdate: return "Update";
        case dlpguest::Shown::NotRead: return "Read";
        default: return "";
        }
    }
    const char* Hint() const override {
        return keys_ ? "" : dlp::kGuideHint;
    }
    bool StartButton() const override {
        return false;
    }
    int ExtraKeys(FooterKey* out) const override {
        if (phase_ != Phase::List) {
            return 0;
        }
        const Item* it = SelectedItem();
        if (it != nullptr && it->shown == dlpguest::Shown::NotRead && it->child >= 0) {
            out[0] = FooterKey{"X", "Remove", KEY_X}; // (a game with a guest whose new version could not be read: A reads it again, X removes the guest)
            return 1;
        }
        if (Owned(children_).size() < 2) {
            return 0;
        }
        out[0] = FooterKey{"X", "Remove all", KEY_X};
        return 1;
    }
    void OnExtra(int, const Row*) override {
        const Item* it = SelectedItem();
        if (it != nullptr && it->shown == dlpguest::Shown::NotRead && it->child >= 0) {
            AskRemove(Owned({children_[static_cast<std::size_t>(it->child)]}));
            return;
        }
        AskRemove(Owned(children_));
    }
    void OnKeys(const Input& in) override {
        if (phase_ == Phase::List && (in.down & KEY_Y)) {
            Guide();
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        if (phase_ != Phase::List) {
            return; // (the card with the bar is drawn instead: DrawTop, DrawBottom)
        }
        if (!keys_) {
            Row r;
            r.id = kNoKeys;
            r.kind = Kind::Info;
            r.label = "Needs boot9.bin";
            r.value_color = col::kWarn;
            r.label_color = col::kWarn;
            r.note_title = "Download Play guests need boot9.bin";
            r.note = "A Download Play guest is made with boot9.bin, the key file of this console: without it none is prepared, and some games cannot "
                     "be read. Y shows how to get it with GodMode9. The guests already here keep working.";
            rows.push_back(r);
        }
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            Row r;
            r.id = kItemBase + static_cast<int>(i);
            r.label = it.name;
            r.icon = it.icon;
            r.note_title = it.name; // (no note: the top screen draws the game's card, DrawGame)
            switch (it.shown) {
            case dlpguest::Shown::Off:
                r.kind = Kind::Toggle;
                r.on = false;
                break;
            case dlpguest::Shown::On:
                r.kind = Kind::Toggle;
                r.on = true;
                break;
            case dlpguest::Shown::ToUpdate:
                r.kind = Kind::Action;
                r.value = "to update";
                r.value_color = col::kWarn;
                break;
            default: // NotRead
                r.kind = Kind::Action;
                r.dim = true;
                r.value = "not read";
                r.value_color = col::kDim;
                break;
            }
            rows.push_back(r);
        }
        if (!to_read_.empty() && !g.luma_too_old) {
            Row r;
            r.id = kUnread;
            r.kind = Kind::Action;
            r.gap = items_.empty() ? 0.0f : 6.0f;
            r.label = to_read_.size() == 1 ? std::string("1 game not checked yet") : Fmt("%d games not checked yet", static_cast<int>(to_read_.size()));
            r.value_color = col::kDim;
            r.note_title = "Games not checked yet";
            r.note = "3DS Link has not looked at these games for Download Play yet: the check was stopped, or could not start. A checks them now.";
            rows.push_back(r);
        }
        if (!no_keys_.empty() && !g.luma_too_old) {
            // (the review of 2026-11-17 evening: these were read again at every opening, and the row said "stopped")
            const bool boot9 = no_keys_error_ != dlpguest::kErrNoMovable;
            const int n = static_cast<int>(no_keys_.size());
            Row r;
            r.id = kWaitKeys;
            r.kind = Kind::Action;
            r.gap = items_.empty() && to_read_.empty() ? 0.0f : 6.0f;
            r.label = boot9 ? (n == 1 ? std::string("1 game waits for boot9.bin") : Fmt("%d games wait for boot9.bin", n))
                            : (n == 1 ? std::string("1 game waits for movable.sed") : Fmt("%d games wait for movable.sed", n));
            r.value_color = col::kWarn;
            r.label_color = col::kWarn;
            r.note_title = "Games not checked";
            r.note = "3DS Link could not look at these games for Download Play: " + std::string(dlpguest::Phrase(no_keys_error_)) + " " +
                     AdviceText(no_keys_error_) + " They are not read again by themselves until that changes. " +
                     (keys_ ? "A checks them now." : "A shows how to get boot9.bin.");
            rows.push_back(r);
        }
        if (items_.empty() && to_read_.empty() && no_keys_.empty()) {
            Row r;
            r.id = kEmpty;
            r.kind = Kind::Info;
            r.label = "No game with Download Play";
            r.value_color = col::kDim;
            r.note_title = "No game with Download Play";
            r.note = "None of your games (and of the game in the slot) has a Download Play guest 3DS Link can prepare.";
            rows.push_back(r);
        }
    }

    // A or a tap (step 0) and the D-pad (right +1 on, left -1 off) on a game's switch, as Enable Titles does (SwitchTarget: 2026-11-18, the console
    // test). The D-pad goes the very same way as A: the plan in the thread of the look (SwitchOn), the questions, the run page; the removal asks first.
    // Nothing of the work on the thread of the screen. The rows with no switch ("to update", "not read", the games not checked) answer A only.
    void Activate(const Row& row, int step) override {
        if (phase_ != Phase::List) {
            return;
        }
        const int i = row.id - kItemBase;
        const Item* item = i >= 0 && i < static_cast<int>(items_.size()) ? &items_[static_cast<std::size_t>(i)] : nullptr;
        const bool toggle = item != nullptr && (item->shown == dlpguest::Shown::On || item->shown == dlpguest::Shown::Off);
        if (!toggle && step != 0) {
            return; // (the D-pad sets a switch: this row has none)
        }
        if (row.id == kUnread) {
            Read(to_read_);
            return;
        }
        if (row.id == kWaitKeys) {
            if (keys_) {
                Read(no_keys_); // (the user asks: read again now, whatever the session remembers)
            } else {
                Guide();
            }
            return;
        }
        if (item == nullptr) {
            return;
        }
        const Item it = *item;
        if (it.shown == dlpguest::Shown::NotRead) {
            dlpguest::Planned p; // (A on "not read": that one game is read again now, whatever children.cache remembers)
            p.game = it.game;
            p.act = dlpguest::Act::Probe;
            p.card = it.card;
            p.game_version = it.game_version;
            p.update_version = it.update_version;
            PushPage(new RunPage({p}, false, true)); // (asked: a failure is said, the remembered one or not)
            return;
        }
        const bool on = it.shown == dlpguest::Shown::On;
        if (toggle && SwitchTarget(on, step) == on) {
            return; // (the D-pad towards where the switch already is: nothing, as in Enable Titles)
        }
        if (!on && it.id_taken != 0) {
            // (the build 2h) an id of its guest is somebody else's: never Enable, nothing to remove - said (the run would refuse it all the same)
            PushPage(pages::NewMessagePage(kTitle, it.name + ": " + dlpguest::Phrase(it.id_taken) + " " + AdviceText(it.id_taken)));
            return;
        }
        if (on) {
            if (it.child >= 0) {
                AskRemove(Owned({children_[static_cast<std::size_t>(it.child)]}));
            }
        } else {
            SwitchOn(it); // (Off, or A on "to update": made again)
        }
    }

private:
    enum Id { kNoKeys = 1, kUnread, kWaitKeys, kEmpty, kItemBase = 100 };
    // The top screen of the list: the card of the selected game (or the row's note), then the card of the SD card.
    static constexpr float kGameCardH = 114.0f;
    static constexpr float kSdCardY = 150.0f;
    static constexpr float kSdCardH = 52.0f;
    // Titles: the scan of the titles (or a copy of the plugin, a run) goes first. Looking: the thread of the look. Reading: the worker reads the games
    // never read. Planning: A on a game that is off, its plan in the thread of the look. List: the rows. The screen only draws: nothing of the work is
    // on its thread (2026-11-17 evening, the console test).
    enum class Phase { Titles, Looking, Reading, Planning, List };
    static constexpr int kRetryFrames = 60; // (a look that could not start - no memory for its thread - is tried again a second later)
    struct Item {
        u64 game = 0;
        std::string name;
        const u16* icon = nullptr;
        dlpguest::Shown shown = dlpguest::Shown::Hidden;
        int error = 0;
        bool card = false;
        u16 game_version = 0, update_version = 0;
        u32 child_bytes = 0;
        int child = -1; // its row of children_ (On, to update)
        u64 bytes = 0;  // (child) what it takes on the SD card
        bool shown_in_home = false; // (the build 2f) its guest is Installed in the old form: HOME shows it as a black icon
        bool old_form = false;      // ... of the builds before 2f (composer < 2): "to update" hides it
        bool test_form = false;     // (DPAPP-2j) its guest is of the system form of the test build 2h: no console takes it, Azahar falls on it
        int id_taken = 0;           // (the build 2h) the kErr* of an id of its guest that is somebody else's: Enable is never offered
    };

    Footer BackFooter() const {
        Footer footer;
        footer.action_label = "";
        footer.start = false;
        footer.back_label = phase_ == Phase::Reading ? "Stop" : "Back";
        return footer;
    }
    bool BackPressed(const Input& in) const {
        return (in.down & KEY_B) || Tapped(in, LayoutFooter(BackFooter()).back);
    }

    // From the look to the list, as while a guest is prepared (dlp::WorkHolds): HOME and sleep off - the bar shows "HOME off" - and the scan's games
    // held (no new scan under the thread of the look or the worker). The check reads your games: it is never cut by HOME or a closed lid.
    void Hold(bool on) {
        if (on) {
            holds_.Take();
        } else {
            holds_.Give();
        }
    }

    // The look again, through Phase::Titles: it starts in Update - this page on top, so HOME is not held under a message - and only when CanLook
    // (no copy of the plugin waiting, no scan to make again).
    void Relook() {
        phase_ = Phase::Titles;
        retry_ = 0;
    }

    // The look, in its thread: the page shows the card with the bar meanwhile (Phase::Looking).
    void Look() {
        Hold(true);
        if (!dlpguest::StartGamesLook()) {
            Hold(false);
            retry_ = kRetryFrames;
            start_failed_ = true; // (the card says so, and B goes back)
            phase_ = items_.empty() ? Phase::Titles : Phase::List; // (no memory now: from Titles it is tried again)
            return;
        }
        start_failed_ = false;
        generation_ = titles::ScanGeneration(); // (the scan the look reads, held until the list: a scan made again while the page waited is not a second look)
        phase_ = Phase::Looking;
    }

    // The look is over: the rows. The games never read are read now, once an opening of the page (Phase::Reading), then the look again.
    void TakeLook() {
        dlpguest::GamesLook look;
        dlpguest::TakeGamesLook(&look);
        if (leave_) {
            Hold(false);
            PopPage();
            return;
        }
        Apply(look);
        if (!read_ && !to_read_.empty() && !g.luma_too_old) {
            read_ = true;
            Read(to_read_);
            return;
        }
        Hold(false);
        phase_ = Phase::List;
    }

    // The worker reads `jobs` (Probes) with the scan's games held; the page shows the card with the bar (Phase::Reading).
    void Read(const std::vector<dlpguest::Planned>& jobs) {
        Hold(true);
        if (dlpguest::StartRun(jobs) != 0) {
            Hold(false);
            phase_ = Phase::List; // (no memory now: the row "N games not checked yet" stays, A tries again)
            return;
        }
        leave_ = false;
        phase_ = Phase::Reading;
    }

    // What the card says while the page works, as Enable Titles says it: the title, the line under way, the detail, and the bar (-1: the dots).
    void Message(std::string& title, std::string& text, std::string& detail, float& fraction) const {
        static const char* kOnce = "3DS Link looks once at each of your games for a Download Play guest it can prepare; the list appears as soon as "
                                   "they are read. The next times only the games installed or updated since are looked at. HOME is off meanwhile.";
        switch (phase_) {
        case Phase::Titles:
            if (!titles::Scan().done()) {
                title = "Reading your titles...";
                text = setup::ScanProgressText(titles::Scan().progress());
                detail = "The list appears as soon as the titles are read. This happens at every start; the first time it takes longer.";
                fraction = setup::ProgressFraction(titles::Scan().progress_done(), titles::Scan().progress_total());
            } else if (start_failed_) {
                title = "Please wait...";
                text = "There is not enough memory now to check your games: 3DS Link tries again in a moment.";
                detail = "B goes back.";
            } else if (const int left = setup::CopyWorker::Get().PendingCount()) {
                // (the review of 2026-11-17 evening: "Please wait" and the dots for the whole pass of the copies, with no word of what it waited for)
                const int total = g.plugin_pass_open && g.plugin_pass_total >= left ? g.plugin_pass_total : left;
                title = "Putting the plugin in your games...";
                text = Fmt("%d of %d folders done.", total - left, total);
                detail = "3DS Link copies the plugin into the folders of your games first; then it checks them for Download Play.";
                fraction = setup::ProgressFraction(static_cast<u64>(total - left), static_cast<u64>(total));
            } else if (g.rescan_wanted) {
                title = "Reading your titles...";
                text = "3DS Link reads your titles again (boot9.bin was found, or titles were installed).";
                detail = "The list appears as soon as the titles are read.";
            } else if (!pages::WorkUnderWay(title, text, detail, fraction)) {
                // (the build 2h, U3: the work is said by WorkUnderWay - the update, the public lobbies, a guest -, with its bar; this is a moment
                // only: the page's own look about to start)
                title = "Please wait...";
                text = "3DS Link finishes the work under way first.";
                detail.clear();
            }
            return;
        case Phase::Planning: {
            int done = 0, total = 0;
            dlpguest::GamesLookProgress(&done, &total);
            title = "Please wait...";
            text = "Looking at " + switching_.name + ".";
            detail = "3DS Link looks at the game, the card slot and its Download Play guest before it asks. B goes back to the list.";
            fraction = setup::ProgressFraction(static_cast<u64>(done), static_cast<u64>(total));
            return;
        }
        case Phase::Looking: {
            int done = 0, total = 0;
            dlpguest::GamesLookProgress(&done, &total);
            title = "Checking your games for Download Play...";
            text = "Looking at your games.";
            detail = kOnce;
            fraction = setup::ProgressFraction(static_cast<u64>(done), static_cast<u64>(total));
            return;
        }
        case Phase::Reading: {
            const dlpguest::RunView v = dlpguest::View();
            const int shown = v.count == 0 ? 0 : (v.index < v.count ? v.index + 1 : v.count);
            title = "Checking your games for Download Play...";
            text = Fmt("%d of %d: ", shown, v.count) + dlpguest::GameName(v.game) + " - " +
                   (v.stopping ? std::string("stops after this game") : dlp::TimeLeftText(v.seconds_left));
            detail = std::string(kOnce) + "\n\nB stops after the game under way and shows the list: the others are checked the next time.";
            fraction = static_cast<float>(v.fraction);
            return;
        }
        case Phase::List:
            return;
        }
    }

    // The rows of what the look found (the thread of the screen only copies: nothing is read here).
    void Apply(dlpguest::GamesLook& look) {
        keys_ = dlpguest::KeysReady();
        runs_ = dlpguest::RunsEnded();
        gfx::ReleaseIcons(); // (the card's icon may be another game's now, at the same address)
        card_ = look.card;
        has_card_ = look.has_card;
        children_ = std::move(look.children);
        to_read_ = std::move(look.to_read);
        no_keys_ = std::move(look.no_keys);
        no_keys_error_ = look.no_keys_error;
        std::vector<const titles::Game*> games;
        for (const std::unique_ptr<titles::Game>& game : titles::Scan().games()) {
            games.push_back(game.get());
        }
        if (has_card_) {
            games.push_back(&card_);
        }
        items_.clear();
        for (const dlpguest::GameView& v : look.views) {
            Item it;
            it.game = v.game;
            it.shown = v.shown;
            it.error = v.error;
            it.id_taken = v.id_taken;
            it.card = v.card;
            it.game_version = v.game_version;
            it.update_version = v.update_version;
            it.child_bytes = v.child_bytes;
            it.name = dlpguest::GameName(v.game);
            for (const titles::Game* game : games) {
                if (game->tid == v.game) {
                    it.icon = game->icon;
                    it.name = gfx::Clean(game->name);
                }
            }
            for (std::size_t c = 0; c < children_.size(); ++c) {
                if (children_[c].guest.game == v.game) {
                    it.child = static_cast<int>(c);
                    it.bytes = children_[c].title_bytes + children_[c].plugin_bytes + children_[c].block_bytes;
                    const dlpguest::Guest& guest = children_[c].guest;
                    it.shown_in_home = guest.state == dlpguest::GuestState::Installed && guest.form == dlpguest::kFormApplication;
                    it.old_form = it.shown_in_home && guest.composer < dlpguest::kComposerVersion;
                    it.test_form = guest.form == dlpguest::kFormSystem;
                }
            }
            items_.push_back(it);
        }
    }

    const Item* SelectedItem() const {
        const Row* row = Selected();
        const int i = row != nullptr ? row->id - kItemBase : -1;
        return i >= 0 && i < static_cast<int>(items_.size()) ? &items_[static_cast<std::size_t>(i)] : nullptr;
    }

    // The space a game's guest takes when it is made (children.cache knows it, else the usual size).
    static u64 GuestSize(const Item& it) {
        return it.child_bytes != 0 ? it.child_bytes : dlpguest::kUsualChildBytes;
    }

    // Enabled: its guest is made and ours (the record Installed), as EnabledGuests counts on the page Download Play.
    bool Enabled(const Item& it) const {
        if (it.child < 0 || it.child >= static_cast<int>(children_.size())) {
            return false;
        }
        const dlpguest::ChildRow& c = children_[static_cast<std::size_t>(it.child)];
        return c.guest.state == dlpguest::GuestState::Installed && c.owned;
    }

    // The top screen of the list (2026-11-18, the console test: "it looks bad"), as Enable Titles and Titles Delivery show theirs: the selected game,
    // then the SD card. It draws what the look copied (items_) and the space main.cpp reads every ten seconds (g.status): nothing is read here.
    void DrawListTop() {
        DrawTitleBarStatus(gfx::kTopW, Title());
        const Item* it = SelectedItem();
        if (it != nullptr) {
            DrawGame(*it);
            DrawSdCard(it);
        } else if (const Row* row = Selected()) {
            // (a row that is not a game - boot9.bin, the games not checked - has its whole note: what A does is its last sentence, and nothing of
            // the card changes with it - the review of 2026-11-18)
            DrawNote(10, 32, 380, 168, *row);
        }
        DrawStatusStrip(10, 206, 380, 28);
        DrawFrameBorder(gfx::kTopW);
    }

    // The selected game: its icon and name, "Status: ...", what A does ("Enable: prepares ..."), and the size on a line of its own.
    void DrawGame(const Item& it) const {
        DrawCard(10, 32, 380, kGameCardH);
        gfx::FillRect(10, 32, 3, kGameCardH, col::kAccent);
        gfx::FillRect(22, 42, 52, 52, col::kPanelAlt);
        gfx::StrokeRect(22, 42, 52, 52, 1.0f, col::kLine);
        if (it.icon != nullptr) {
            gfx::DrawIcon(it.icon, setup::kLargeIcon, 24.0f, 44.0f, 1.0f, it.shown == dlpguest::Shown::NotRead ? 140 : 255);
        }
        gfx::DrawWrapped(86, 42, 292, 0.55f, col::kText, it.name, 2);
        u32 dot = col::kTrack;
        const char* status = "Status: Off";
        const char* verb = "Enable";
        const char* action = ": prepares its Download Play guest";
        std::string size = "Size: about " + Mb(GuestSize(it)) + " on the SD card";
        std::string note = "A few minutes with HOME off; twice the size while it is made.";
        u32 note_color = col::kDim;
        switch (it.shown) {
        case dlpguest::Shown::On:
            dot = col::kOn;
            status = "Status: On";
            verb = "Disable";
            action = ": removes its Download Play guest";
            size = it.child >= 0 ? "Size: " + Mb(it.bytes) + " on the SD card" : std::string("Size: not known");
            note = std::string("Ready. ") + dlp::HostsNote(it.game); // (B2 of the build 2f: who can host it)
            if (it.shown_in_home) {
                // (the build 2f, D2: the fallback, said while it lasts. The review of the build 2f: a guest of an older 3DS Link shown On - no
                // boot9.bin, a Repair first - was said "refused" too)
                note = it.old_form ? "HOME shows it as a black icon: an older 3DS Link made it."
                                   : "HOME shows it as a black icon: the system refused the hidden guest.";
                note_color = col::kWarn;
            }
            break;
        case dlpguest::Shown::ToUpdate:
            dot = col::kWarn;
            status = "Status: to update";
            verb = "Update";
            action = ": makes its Download Play guest again";
            if (it.child >= 0) {
                size += ", " + Mb(it.bytes) + " now"; // (the red of the SD card is the difference: the guest made again takes the old one's place)
            }
            note = it.test_form ? "Made by a test build: it does not start on a console. Update it." // (DPAPP-2j: the system form of the build 2h)
                                : (it.old_form ? "Its guest shows in HOME as a black icon: update it to hide it." // (the build 2f: composer 1, the old form)
                                               : "Its guest is of another version of the game, or it was not finished.");
            break;
        case dlpguest::Shown::NotRead:
            dot = col::kDim;
            status = "Status: not read";
            verb = "Read";
            action = ": looks at its Download Play data again";
            size.clear(); // (not known: the reason and its advice take the two lines, wrapped - the review of 2026-11-18: one line cut them)
            note = std::string(dlpguest::Phrase(it.error)) + " " + AdviceText(it.error) + (it.child >= 0 ? " X removes its guest." : "");
            note_color = col::kWarn;
            break;
        default: // Off
            if (it.id_taken != 0) {
                // (the build 2h) an id of its guest is somebody else's (ids.cache): never Enable - said, with nothing to remove
                note = std::string("Number taken: ") + dlpguest::Phrase(it.id_taken) + " " + AdviceText(it.id_taken);
                note_color = col::kWarn;
                size.clear(); // (the review of the build 2h: the reason and its advice wrapped on their lines, as "not read" - one line cut them)
            }
            break;
        }
        gfx::FillCircle(92, 84, 5, dot);
        gfx::DrawFitted(104, 84, 0.52f, 0.4f, 180, col::kText, status);
        if (it.card) {
            gfx::DrawFitted(378, 84, 0.42f, 0.34f, 90, col::kDim, "game card", gfx::Align::Right);
        }
        const float verb_w = gfx::TextWidth(verb, 0.5f);
        gfx::DrawText(26, 104, 0.5f, col::kAccent, verb);
        gfx::DrawFitted(26 + verb_w, 104, 0.5f, 0.38f, 352 - verb_w, col::kText, action);
        if (size.empty()) {
            gfx::DrawWrapped(26, 111, 352, 0.40f, note_color, note, 3);
            return;
        }
        gfx::DrawFitted(26, 122, 0.46f, 0.36f, 352, col::kText, size);
        gfx::DrawFitted(26, 137, 0.40f, 0.32f, 352, note_color, note);
    }

    // The SD card as Titles Delivery shows it: green what is there now, red what enabling the selected game adds; a game that is on shows the green
    // shorter by what disabling it frees.
    void DrawSdCard(const Item* it) const {
        const u64 total = g.status.sd_known ? g.status.sd_total : 0;
        const u64 free_bytes = g.status.sd_known ? g.status.sd_free : 0;
        const u64 used = total > free_bytes ? total - free_bytes : 0;
        u64 add = 0, freed = 0;
        if (it != nullptr && it->shown == dlpguest::Shown::Off) {
            add = GuestSize(*it);
        } else if (it != nullptr && it->shown == dlpguest::Shown::ToUpdate) {
            add = GuestSize(*it) > it->bytes ? GuestSize(*it) - it->bytes : 0; // (the guest made again takes the place of the one there)
        } else if (it != nullptr && it->shown == dlpguest::Shown::On && it->child >= 0) {
            freed = it->bytes < used ? it->bytes : used;
        }
        const float y = kSdCardY;
        DrawCard(10, y, 380, kSdCardH);
        gfx::DrawText(22, y + 12, 0.44f, col::kText, "SD card");
        gfx::DrawText(380, y + 12, 0.42f, col::kDim,
                      total ? Fmt("%s free of %s", Mb(free_bytes).c_str(), Mb(total).c_str()) : std::string("space unknown"), gfx::Align::Right);
        DrawSpaceBar(22, y + 21, 356, 12, total ? static_cast<double>(used - freed) / static_cast<double>(total) : 0.0,
                     total ? static_cast<double>(add) / static_cast<double>(total) : 0.0);
        gfx::FillRect(22, y + 38, 8, 8, col::kOn);
        gfx::DrawText(34, y + 42, 0.38f, col::kDim, "in use");
        if (freed > 0) {
            gfx::FillRect(86, y + 38, 8, 8, col::kTrack);
            gfx::StrokeRect(86, y + 38, 8, 8, 1.0f, col::kBorder);
            gfx::DrawText(98, y + 42, 0.38f, col::kDim, Mb(freed) + " freed");
        } else {
            gfx::FillRect(86, y + 38, 8, 8, col::kOff);
            gfx::DrawText(98, y + 42, 0.38f, col::kDim, add > 0 ? "+" + Mb(add) + " to add" : std::string("what you add"));
        }
        // (while it is made, the guest takes its size twice: the work file and the title - "to update" too, whose red is only the difference: the review
        // of 2026-11-18)
        const u64 need = it != nullptr && (it->shown == dlpguest::Shown::Off || it->shown == dlpguest::Shown::ToUpdate) ? add + GuestSize(*it) : 0;
        if (need > 0 && total && free_bytes < need) {
            gfx::DrawText(380, y + 42, 0.4f, col::kOff, "not enough space", gfx::Align::Right);
        }
    }

    // A on Off (or "to update"): boot9.bin, then the plan of THAT game in the thread of the look (the slot, AM, the records: "Please wait", never on the
    // thread of the screen - the review of 2026-11-17 evening), then AfterPlan.
    void SwitchOn(const Item& it) {
        if (g.luma_too_old) {
            PushPage(pages::NewMessagePage(kTitle, "This Luma is not supported: a Download Play guest needs Luma3DS 13.0 or later. The guests here can "
                                                   "still be removed."));
            return;
        }
        if (!dlpguest::KeysReady()) {
            PushPage(pages::NewConfirmPage(kTitle, "A Download Play guest is made with boot9.bin, the key file of this console, and it is not there. "
                                                   "See how to get it?",
                                           "Guide", "Back", [](bool yes) {
                                               if (yes) {
                                                   Guide();
                                               }
                                           }));
            return;
        }
        Hold(true);
        if (!dlpguest::StartGamesLook(false, it.game)) {
            Hold(false);
            PushPage(pages::NewMessagePage(kTitle, it.name + ": 3DS Link cannot prepare its guest now. Try again in a moment."));
            return;
        }
        switching_ = it;
        skip_ = false;
        phase_ = Phase::Planning;
    }

    // The plan of the game is made: the list again, and what comes next (B meanwhile: nothing).
    void TakePlan() {
        dlpguest::GamesLook look;
        dlpguest::TakeGamesLook(&look);
        Hold(false);
        phase_ = Phase::List;
        if (!skip_) {
            AfterPlan(switching_, look);
        }
    }

    // "switched on in Enable Titles too?", then the space and the time, then the run; or why not.
    void AfterPlan(const Item& it, const dlpguest::GamesLook& look) {
        dlpguest::Planned p = look.wanted;
        const dlpguest::Act act = p.act;
        std::vector<dlpguest::Planned> jobs;
        switch (act) {
        case dlpguest::Act::Prepare:
            jobs.push_back(p);
            break;
        case dlpguest::Act::Adopt: // (our child is at the id without its record: its record again, then made again)
            jobs.push_back(p);
            p.act = dlpguest::Act::Prepare;
            jobs.push_back(p);
            break;
        case dlpguest::Act::Refuse:
        case dlpguest::Act::Full: {
            // (the build 2h) the guard of the ids says which id is taken, and by what (a title of the system, a ticket, a game still in memory)
            const int err = act == dlpguest::Act::Full ? dlpguest::kErrTooMany : (it.id_taken != 0 ? it.id_taken : dlpguest::kErrIdTaken);
            PushPage(pages::NewMessagePage(kTitle, it.name + ": " + dlpguest::Phrase(err) + " " + AdviceText(err)));
            return;
        }
        case dlpguest::Act::Nothing: {
            if (it.card && !(look.has_card && look.card.tid == it.game)) {
                Relook(); // (the card was taken out since the list was made: its row goes - the review of spec 9)
                PushPage(pages::NewMessagePage(kTitle, it.name + ": the game card is not in the slot any more."));
                return;
            }
            PushPage(pages::NewMessagePage(kTitle, it.name + ": 3DS Link cannot prepare its guest now. Try again in a moment."));
            return;
        }
        default: // (Probe, Check, Repair, Remove, Disown: the quiet work first; the row says what comes next)
            jobs.push_back(p);
            PushPage(new RunPage(jobs, false, true));
            return;
        }
        const u64 game = it.game;
        const std::string name = it.name;
        const u32 child_bytes = it.child_bytes;
        if (it.shown == dlpguest::Shown::Off && look.enable_off && g.plugin_ready) {
            // (Download Play does not need Enable Titles: the guest has its own folder of the plugin. A No goes on all the same. Asked only when the game
            // is switched on, never for "to update": spec 9 point 4.2, the review of spec 9)
            PushPage(pages::NewConfirmPage("Enable Titles", name + " is not switched on in Enable Titles. Switch it on too?", "Yes", "No",
                                           [game, name, child_bytes, jobs](bool yes) {
                                               if (yes) {
                                                   setup::CopyWorker::Get().Submit(game, setup::CopyKind::Enable, g.paths.plugin_file);
                                               }
                                               AskPrepare(jobs, name, child_bytes);
                                           }));
            return;
        }
        AskPrepare(jobs, name, child_bytes);
    }

    // The space and the time, said before the run (spec 9 point 4.3). From A or the D-pad on a row, the same way (Activate, then SwitchOn).
    static void AskPrepare(const std::vector<dlpguest::Planned>& jobs, const std::string& name, u32 child_bytes) {
        std::vector<dlpguest::Planned> heavy;
        for (const dlpguest::Planned& p : jobs) {
            if (p.act == dlpguest::Act::Prepare) {
                heavy.push_back(p);
            }
        }
        const u32 minutes = dlpguest::MinutesFor(heavy);
        const u64 size = child_bytes != 0 ? child_bytes : dlpguest::kUsualChildBytes;
        PushPage(pages::NewConfirmPage("Prepare?",
                                       "Prepare the Download Play guest of " + name + "? It takes about " + Mb(size) +
                                           " on the SD card (twice that while it is made) and " + Fmt("about %u minute%s", static_cast<unsigned>(minutes), minutes == 1 ? "" : "s") +
                                           ", with HOME off.",
                                       "Prepare", "Back", [jobs](bool yes) {
                                           if (yes) {
                                               PushPage(new RunPage(jobs, true));
                                           }
                                       }));
    }

    // The rows the app can remove: a record past the registry's slots is not ours to this start (its Remove would find no record and do nothing).
    static std::vector<dlpguest::ChildRow> Owned(const std::vector<dlpguest::ChildRow>& rows) {
        std::vector<dlpguest::ChildRow> out;
        for (const dlpguest::ChildRow& c : rows) {
            if (c.owned) {
                out.push_back(c);
            }
        }
        return out;
    }

    void AskRemove(const std::vector<dlpguest::ChildRow>& which) {
        if (which.empty()) {
            return;
        }
        u64 bytes = 0;
        std::vector<dlpguest::Planned> jobs;
        for (const dlpguest::ChildRow& c : which) {
            bytes += c.title_bytes + c.plugin_bytes + c.block_bytes;
            dlpguest::Planned p;
            p.game = c.guest.game;
            p.act = dlpguest::Act::Remove;
            p.card = c.guest.where == 'C';
            jobs.push_back(p);
        }
        const std::string what = which.size() == 1 ? "the Download Play guest of " + which[0].name : Fmt("all %d Download Play guests", static_cast<int>(which.size()));
        PushPage(pages::NewConfirmPage("Remove?", "Remove " + what + "? It frees " + Mb(bytes) + ".", "Remove", "Keep", [jobs](bool yes) {
            if (yes) {
                PushPage(new RunPage(jobs, false));
            }
        }));
    }

    std::vector<Item> items_;
    std::vector<dlpguest::ChildRow> children_;
    std::vector<dlpguest::Planned> to_read_; // the games never read (the check was stopped, or could not start): the row "N games not checked yet"
    std::vector<dlpguest::Planned> no_keys_; // the games whose look stopped at the keys this session: the row "N games wait for boot9.bin"
    int no_keys_error_ = 0;
    titles::Game card_; // (the game in the slot: its icon lives as long as the page)
    bool has_card_ = false;
    bool keys_ = false;
    Phase phase_ = Phase::Titles;
    dlp::WorkHolds holds_; // HOME and sleep off, the scan's games held: while the page looks and reads (Hold)
    bool read_ = false;  // the games never read were read once at this opening
    bool leave_ = false; // B while the page looks: it goes when the look is over
    bool skip_ = false;  // B while the plan of a game is made: nothing is switched on
    bool start_failed_ = false; // the last look could not start (no memory for its thread): the card says so
    int retry_ = 0;      // (Titles) frames before a look that could not start is tried again
    Item switching_;     // the game whose plan is made (Planning)
    int generation_ = 0;
    int runs_ = 0;
};

// ---- Download Play: the entry of the main page ---------------------------------------------------------------------------------------

class DlpPage : public ListPage {
public:
    DlpPage() {
        Count();
    }
    ~DlpPage() override {
        if (looking_) {
            dlpguest::DropGamesLook(); // (never on the way of B, which waits for the look: its thread reads the scan's games, which the hold keeps)
        }
        Hold(false);
    }
    void OnResume() override {
        Count();
    }

    void Update(const Input& in) override {
        if (!looked_ && !looking_ && dlpguest::CanLook() && --retry_ < 0) {
            // (one quiet pass for each opening of the page: a child whose game is gone goes, a repair is made. The records are looked at against the
            // games in the thread of the look, the scan's games held: nothing of it on the thread of the screen)
            Hold(true);
            looking_ = dlpguest::StartGamesLook(true);
            if (!looking_) {
                Hold(false);
                retry_ = 60; // (no memory for its thread now: a second later, 60 frames)
            }
        }
        if (looking_ && dlpguest::GamesLookDone()) {
            dlpguest::GamesLook look;
            dlpguest::TakeGamesLook(&look);
            Hold(false);
            looking_ = false;
            looked_ = true;
            if (leave_) {
                PopPage(); // (B while the look ran: the page goes now, its thread joined; its quiet work waits for the next opening)
                return;
            }
            if (!look.quiet.quiet.empty()) {
                PushPage(new RunPage(look.quiet.quiet, false));
                return; // (A pressed meanwhile is done when the run page is over: below)
            }
        }
        if (looked_ && !looking_ && pending_ != 0) {
            const int id = pending_;
            pending_ = 0;
            Open(id); // (A pressed while the look ran: done now, or after its quiet work)
            return;
        }
        ListPage::Update(in);
    }

protected:
    const char* Title() const override {
        return kTitle;
    }
    const char* BarTitle() const override {
        return "Options"; // (the build 2h, U4: not "Download Play" once more - it is the title of the top screen, as Titles Delivery's "Options")
    }
    bool StartButton() const override {
        return false;
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        // (2026-11-18: "Join lobby" has no value - how many lobbies there are is known only once the page is in the room, never from here - and the
        // count of the games enabled is on "Enable games", the page that makes their guests)
        Row join;
        join.id = kJoin;
        join.kind = Kind::Link;
        join.label = "Join lobby";
        if (looking_ && pending_ == kJoin) {
            join.value = "one moment..."; // (A pressed while the quiet look runs: it opens when the look is over)
            join.value_color = col::kDim;
        }
        join.note_title = "Join lobby";
        join.note = "The Download Play lobbies of your room, hosted by a console or an Azahar emulator. You join with a game whose guest is ready here "
                    "(Enable games); when the host starts, the game starts here by itself.";
        rows.push_back(join);
        Row games;
        games.id = kGames;
        games.kind = Kind::Link;
        games.label = "Enable games";
        // (the same count as the bar "Enabled n / x" of the page: "N games ready" counted the guests to update too - the review of 2026-11-18)
        games.value = enabled_ > 0 ? Fmt("%d enabled", enabled_) : std::string("none enabled");
        games.value_color = enabled_ > 0 ? col::kOn : col::kDim;
        games.note_title = "Enable games";
        games.note = "Your games that have Download Play. Enable one and 3DS Link prepares its guest: a hidden title made from your copy of the game, "
                     "which joins a lobby in the room. Disable it to remove it.";
        if (looking_ && pending_ == kGames) {
            games.value = "one moment...";
            games.value_color = col::kDim;
        }
        rows.push_back(games);
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        if (looking_) {
            pending_ = row.id; // (the quiet look ends in a moment: the row opens then)
            return;
        }
        Open(row.id);
    }
    void OnBack() override {
        if (looking_) {
            leave_ = true; // (the quiet look ends by itself in a moment: the page goes then, never with its thread still reading the scan's games)
            return;
        }
        ListPage::OnBack();
    }

private:
    enum Id { kJoin = 1, kGames };

    void Open(int id) {
        if (id == kJoin) {
            PushPage(pages::NewDlpJoinPage()); // (the page checks the rest itself: dlpjoin::JoinGate)
        } else if (id == kGames) {
            PushPage(pages::NewDlpGamesPage());
        }
    }

    // (DPAPP-2h-sec) the quiet look reads AM and the SD card: HOME and sleep off - the bar shows "HOME off" - and the scan's games held (no new scan
    // under the thread of the look), as the page Enable games does (dlp::WorkHolds)
    void Hold(bool on) {
        if (on) {
            holds_.Take();
        } else {
            holds_.Give();
        }
    }

    void Count() {
        enabled_ = EnabledGuests(); // (the records, read once, not every frame)
    }

    int enabled_ = 0;
    bool looked_ = false;
    bool looking_ = false;
    dlp::WorkHolds holds_;
    int retry_ = 0; // frames before a look that could not start is tried again
    bool leave_ = false; // B while the quiet look runs: the page goes when it is over
    int pending_ = 0;
};

} // namespace

namespace pages {

bool WorkUnderWay(std::string& title, std::string& text, std::string& detail, float& fraction) {
    fraction = -1.0f;
    detail = "This is 3DS Link's own work, not Download Play's: it ends by itself, and Download Play goes on then. B goes back meanwhile.";
    if (titles::Scan().started() && !titles::Scan().done()) {
        title = "Reading your titles...";
        text = setup::ScanProgressText(titles::Scan().progress());
        fraction = setup::ProgressFraction(titles::Scan().progress_done(), titles::Scan().progress_total());
        return true;
    }
    if (const int left = setup::CopyWorker::Get().PendingCount()) {
        const int total = g.plugin_pass_open && g.plugin_pass_total >= left ? g.plugin_pass_total : left;
        title = "Putting the plugin in your games...";
        text = Fmt("%d of %d folders done.", total - left, total);
        fraction = setup::ProgressFraction(static_cast<u64>(total - left), static_cast<u64>(total));
        return true;
    }
    if (g.rescan_wanted) {
        title = "Reading your titles...";
        text = "3DS Link reads your titles again (boot9.bin was found, or titles were installed).";
        return true;
    }
    if (dlpguest::Running()) {
        const dlpguest::RunView v = dlpguest::View();
        title = "Please wait...";
        text = "3DS Link works on the Download Play guest of " + dlpguest::GameName(v.game) + ".";
        detail = "B goes back meanwhile.";
        fraction = static_cast<float>(v.fraction);
        return true;
    }
    const update::Status u = update::Worker::Get().Read();
    if (u.stage == update::Stage::Checking) {
        title = "Looking for a new version...";
        text = "3DS Link asks whether a new version of itself is out (Settings: the check at the start).";
        return true;
    }
    if (u.stage == update::Stage::Downloading) {
        title = "Downloading the new version...";
        text = Fmt("%lu of %lu KB.", static_cast<unsigned long>(u.done / 1024), static_cast<unsigned long>(u.total / 1024));
        fraction = u.total > 0 ? static_cast<float>(u.done) / static_cast<float>(u.total) : -1.0f;
        return true;
    }
    if (u.stage == update::Stage::Installing) {
        title = "Installing the new version...";
        text = "3DS Link installs its new version: do not switch the console off.";
        return true;
    }
    if (update::Worker::Get().ListsRunning() || lobbies::Scanner::Get().running()) {
        title = "Asking the public rooms...";
        text = "3DS Link finishes asking for the public lobbies.";
        return true;
    }
    return false;
}

ui::Page* NewDlpPage() {
    return new DlpPage;
}

ui::Page* NewDlpGamesPage() {
    return new GamesPage;
}

} // namespace pages
