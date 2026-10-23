// Pages: Settings (the nickname, the options of the plugin, the switch of 3DS Link Delivery, the test, the installation check, About) and About (the name, the version,
// who made it, how it was installed, the plugin file, and the way to the credits: pages_credits.cpp).
#include "about_text.hpp"
#include "dlp_prepare_logic.hpp" // (dlp::kAppTid: the title id of the installed app)
#include "dlp_text.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"

using namespace ui;

namespace {

// ---------------------------------------------------------------------------------------------
// Page: About 3DS Link
// ---------------------------------------------------------------------------------------------

// What the page says is in about_text.hpp (plain C++, tested on a PC): the name "3DS Link" with its version, the claim "Connect, share, play together", "Created by Gionko", how this
// run was started (the installed app, or the file the Homebrew Launcher gave), the plugin file against the plugin inside the app, and the way to the credits.
class AboutPage : public ListPage {
public:
    AboutPage() {
        g.LookAtPlugin(false); // (the plugin file on the card against the plugin inside the app: the card is read again only when the file changed)
        plugin_ = setup::DescribePlugin(g.paths.plugin_file, &plugin_exists_);
        // Which title this process is: the installed 3DS Link says so by its program id; a .3dsx under the Homebrew Launcher gives a path and runs as the launcher's title.
        u64 program_id = 0;
        const bool program_known = R_SUCCEEDED(APT_GetProgramID(&program_id));
        origin_ = about::OriginOf(g.app_path, envIsHomebrew(), program_known, program_id, dlp::kAppTid);
    }

protected:
    const char* Title() const override {
        return "About 3DS Link";
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();

        // The name and the version; the claim is the first line of its note (the top screen shows it at once: this row is the selected one when the page opens).
        Row version;
        version.kind = Kind::Info;
        version.label = about::kAppName;
        version.value = about::VersionLabel(setup::kVersion);
        version.note_title = std::string(about::kAppName) + " " + about::VersionLabel(setup::kVersion);
        version.note = about::AppNote(setup::kVersion);
        rows.push_back(version);

        Row author;
        author.kind = Kind::Info;
        author.label = about::kCreatedBy;
        author.note_title = about::kCreatedBy;
        author.note = about::CreatedByNote();
        rows.push_back(author);

        // How this run was started: it always says something true, never that it does not know.
        const about::InstalledAs started = about::DescribeOrigin(origin_, g.app_path, g.paths.appdir, g.paths.datadir, dlp::kAppTid);
        Row app;
        app.kind = Kind::Info;
        app.label = "Installed as";
        app.value = started.value;
        app.value_color = started.dim ? col::kDim : 0;
        app.note_title = "Installed as";
        app.note = started.note;
        rows.push_back(app);

        // The plugin file that goes with the app, against the plugin the app carries (byte for byte: a header that looks good says nothing).
        const about::PluginRow shown = about::DescribePluginRow(g.plugin_card, setup::EmbeddedPluginUsable(), g.paths.plugin_file, g.plugin_found_version,
                                                                  plugin_exists_ ? setup::FormatSize(plugin_.size) : std::string(), setup::kVersion);
        Row plugin;
        plugin.kind = Kind::Info;
        plugin.label = setup::kPluginFileName;
        plugin.value = shown.value;
        plugin.value_color = shown.tone == about::Tone::Good ? col::kOn : (shown.tone == about::Tone::Warn ? col::kWarn : col::kOff);
        plugin.note_title = "Plugin file";
        plugin.note = shown.note;
        rows.push_back(plugin);

        Row credits;
        credits.id = kRowCredits;
        credits.kind = Kind::Link;
        credits.label = about::kCreditsLabel;
        credits.note_title = about::kCreditsLabel;
        credits.note = about::kCreditsNote;
        rows.push_back(credits);
    }

    // A opens the credits; on the rows that only tell there is no A button.
    const char* ActionLabel() const override {
        const Row* row = Selected();
        return row && row->kind == Kind::Link ? "Open" : "";
    }

    void Activate(const Row& row, int step) override {
        if (row.id == kRowCredits && step == 0) {
            PushPage(pages::NewCreditsPage());
        }
    }

private:
    enum Id { kRowCredits = 1 };

    setup::PluginFile plugin_;
    bool plugin_exists_ = false;
    about::Origin origin_ = about::Origin::NoPath;
};

// ---------------------------------------------------------------------------------------------
// Page: Settings
// ---------------------------------------------------------------------------------------------

class ConfigPage : public ListPage {
protected:
    const char* Title() const override {
        return "Settings";
    }
    const char* Hint() const override {
        return "</> switch";
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        const auto add = [&](Row r) { rows.push_back(std::move(r)); };

        Row nick;
        nick.id = kNickname;
        nick.kind = Kind::Value;
        nick.label = "Nickname";
        nick.value = g.cfg.nickname.empty() ? g.console_name + " (console)" : g.cfg.nickname;
        nick.note_title = "Nickname";
        nick.note = "The name other players see (4-20 letters, digits, space . _ -). Empty = the profile name "
                    "of this console (recommended).";
        add(nick);

        Row autoconnect;
        autoconnect.id = kAutoconnect;
        autoconnect.kind = Kind::Toggle;
        autoconnect.label = "Join room at title start";
        autoconnect.on = g.cfg.autoconnect;
        autoconnect.note_title = "Join room at title start";
        autoconnect.note = setup::MenuOn(g.cfg)
                               ? "ON: the plugin joins the room by itself when a title starts.\n"
                                 "OFF: join from the in-game menu (" + setup::MenuChordText(g.cfg.menu_key) + ")."
                               : "The in-game menu is off, so the room is always joined when a title starts, "
                                 "whatever this switch says.";
        add(autoconnect);

        // Three values (ini key radio_like 0, 1, 2): A and D-pad right go Off -> Broadcast -> All (twice) -> Off, left goes back.
        Row radio;
        radio.id = kRadio;
        radio.kind = Kind::Value;
        radio.label = "Radio-like data";
        radio.value = setup::RadioLikeText(g.cfg.radio_like);
        radio.note_title = "Radio-like data";
        radio.note = "How game data goes through the room. Broadcast: beacons and broadcast data are sent without retries, like the "
                     "real radio (no freezes on a weak WiFi link). All (twice): unicast data too, each frame sent twice to a console "
                     "that runs 3DS Link (it drops the copy; an Azahar player gets it once, with retries), and what the room sends is "
                     "taken as it arrives. Off: everything is resent until it arrives (can stall). A or right = next value, left = "
                     "previous. Applies from the next title start.";
        add(radio);

        // Three values, so not a switch: A and D-pad right go Off -> Client only -> Always -> Off, D-pad left goes back
        // (ini key sleep_stop 0, 1, 2).
        Row sleep_row;
        sleep_row.id = kSleep;
        sleep_row.kind = Kind::Value;
        sleep_row.label = "End session on sleep";
        sleep_row.value = setup::SleepStopText(g.cfg.sleep_stop);
        sleep_row.note_title = "End session on sleep (lid)";
        sleep_row.note = "What closing the lid does to the title's session. Off: nothing (it is only logged); the title's "
                         "clock stays behind for about 90 s. Client only: the session ends like on a real console and you join "
                         "again after waking; a hosting console is left alone. Always: the host too (it once froze a "
                         "console). A or right = next value, left = previous. Applies from the next title start.";
        add(sleep_row);

        // ONE switch for every log: the plugin's (ini debug) and the title delivery's (it writes its log only when this is on).
        Row debug;
        debug.id = kDebug;
        debug.kind = Kind::Toggle;
        debug.label = "Debug log (full traffic)";
        debug.on = g.cfg.debug > 0;
        debug.note_title = "Debug log";
        debug.note = "ON: the plugin writes EVERYTHING (every UDS frame with its whole payload, every command, "
                     "service and IPC call) to AzaharLink.debug.log and its events to AzaharLink.log, both in "
                     "sd:/3ds/3DSLink, in blocks of 64 MB numbered in order (.1.log the oldest, up to 50 a log; nothing is "
                     "ever started afresh at a launch); Titles Delivery writes 3DSLink-TitleDelivery.log in the same folder "
                     "(64 MiB a file, 8 files). OFF: no log file at all. Logging slows a title: judge the speed with it OFF.";
        add(debug);

        Row menu;
        menu.id = kMenu;
        menu.kind = Kind::Toggle;
        menu.label = "In-game menu";
        menu.on = setup::MenuOn(g.cfg);
        menu.note_title = "In-game menu";
        menu.note = "ON: the plugin's menu opens in a title with " +
                    setup::MenuChordText(menu.on ? g.cfg.menu_key : g.cfg.menu_chord) +
                    ". OFF: it never opens; everything is set from this app and the ini file, the room is always "
                    "joined at title start, and errors still appear on screen. Applies from the next title start.";
        add(menu);

        // The emergency switch of 3DS Link Delivery, in the app's own file (3DSLink.cfg), never in the plugin's ini. It is here, on a page of its own, so that it is
        // reachable whatever the Download Play part does.
        Row dlp_switch;
        dlp_switch.id = kDlpDelivery;
        dlp_switch.kind = Kind::Toggle;
        dlp_switch.label = dlp::kSwitchLabel;
        dlp_switch.on = g.app_cfg.dlp_delivery;
        dlp_switch.note_title = dlp::kSwitchNoteTitle;
        dlp_switch.note = dlp::kSwitchNote;
        add(dlp_switch);

        Row test;
        test.id = kTest;
        test.kind = Kind::Link;
        test.label = "Test the connection";
        test.gap = 6.0f;
        test.note_title = "Test the connection";
        test.note = "Joins the room from here, shows who is in it, then leaves. Checks the address, the password "
                    "and the WiFi. A room that passes goes to the top of your list of rooms.";
        add(test);

        Row check;
        check.id = kCheck;
        check.kind = Kind::Link;
        check.label = "Installation check";
        check.value = setup::InstallSummary(g.install);
        check.value_color = g.install.problems > 0 ? col::kOff : (g.install.warnings > 0 ? col::kWarn : col::kOn);
        check.note_title = "Installation check";
        check.note = "What 3DS Link needs on the SD card: the plugin file next to this app, the plugin folder, boot9.bin, the card itself. "
                     "A on a line that is not right fixes it: search the card for the plugin, make the folder, find boot9.bin.";
        add(check);

        Row about_row;
        about_row.id = kAbout;
        about_row.kind = Kind::Link;
        about_row.label = "About 3DS Link";
        about_row.note_title = "About 3DS Link";
        about_row.note = "The name and version of 3DS Link, who made it, how it was installed, the plugin file, and the credits: who 3DS Link thanks.";
        add(about_row);
    }

    void Activate(const Row& row, int step) override {
        switch (row.id) {
        case kNickname:
            if (step == 0) {
                EditNickname();
            }
            break;
        case kAutoconnect:
            if (SetSwitch(g.cfg.autoconnect, step)) {
                g.Save();
            }
            break;
        case kRadio:
            g.cfg.radio_like = step < 0 ? setup::PrevRadioLike(g.cfg.radio_like) : setup::NextRadioLike(g.cfg.radio_like);
            g.Save();
            break;
        case kSleep:
            g.cfg.sleep_stop = step < 0 ? setup::PrevSleepStop(g.cfg.sleep_stop) : setup::NextSleepStop(g.cfg.sleep_stop);
            g.Save();
            break;
        case kDebug: {
            bool on = g.cfg.debug > 0;
            if (SetSwitch(on, step)) {
                g.cfg.debug = on ? 1 : 0;
                g.Save();
                g.ApplyModuleConfig(); // (title delivery follows the same switch)
            }
            break;
        }
        case kMenu: {
            const bool was_on = setup::MenuOn(g.cfg);
            if (SwitchTarget(was_on, step) != was_on) {
                setup::SetMenuOn(g.cfg, !was_on);
                g.Save();
            }
            break;
        }
        case kDlpDelivery: {
            bool on = g.app_cfg.dlp_delivery;
            if (SetSwitch(on, step)) {
                g.app_cfg.dlp_delivery = on;
                g.SaveAppConfig();
                g.ShowToast(dlp::SwitchToast(on), on ? col::kOn : col::kWarn, 3000);
            }
            break;
        }
        case kTest:
            if (step != 0) {
                break;
            }
            if (g.cfg.host.empty()) {
                g.ShowToast("Set the Azahar Emulator Room first.", col::kWarn);
            } else if (!g.soc_ok()) {
                g.ShowToast("Network layer not available.", col::kOff);
            } else {
                PushPage(pages::NewTestPage());
            }
            break;
        case kCheck:
            if (step == 0) {
                PushPage(pages::NewInstallCheckPage());
            }
            break;
        case kAbout:
            if (step == 0) {
                PushPage(pages::NewAboutPage());
            }
            break;
        default:
            break;
        }
    }

private:
    enum Id { kAutoconnect = 1, kRadio, kSleep, kDebug, kMenu, kTest, kCheck, kAbout, kNickname, kDlpDelivery };

    void EditNickname() {
        std::string input = g.cfg.nickname;
        if (!g.EditText("Nickname (empty = the console's name)", input, 20, true)) {
            return;
        }
        input = TrimSpaces(input);
        if (input.empty() || setup::ValidNickname(input)) {
            g.cfg.nickname = input;
            g.Save();
        } else {
            g.ShowToast("Invalid nickname: 4-20 chars, A-Z a-z 0-9 space . _ -", col::kOff, 5000);
        }
    }

    // A flips the switch, D-pad right sets it ON, left OFF. True when it changed.
    static bool SetSwitch(bool& value, int step) {
        const bool want = SwitchTarget(value, step);
        if (want == value) {
            return false;
        }
        value = want;
        return true;
    }
};

} // namespace

namespace pages {

ui::Page* NewConfigPage() {
    return new ConfigPage;
}

ui::Page* NewAboutPage() {
    return new AboutPage;
}

} // namespace pages
