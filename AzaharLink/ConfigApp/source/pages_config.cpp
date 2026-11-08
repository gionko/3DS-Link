// Pages: Settings (the nickname, the options of the plugin, the switch of 3DS Link Delivery, the test, the installation check, About) and About (the name, the version,
// who made it, how it was installed, the plugin file, and the way to the credits: pages_credits.cpp).
#include "about_text.hpp"
#include "dlp_prepare_logic.hpp" // (dlp::kAppTid: the title id of the installed app)
#include "dlp_text.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"
#include "update_worker.hpp"

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
// Popup: the keys of a chord (the in-game menu's, the chat's)
// ---------------------------------------------------------------------------------------------

constexpr u32 kChordKeys = KEY_L | KEY_R | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT;

// The keys in the words the plugin reads (ParseKeys in Sources/main.cpp), Select last: "L+Right+Select".
std::string ChordText(u32 keys) {
    static const struct {
        u32 key;
        const char* name;
    } kNames[] = {{KEY_L, "L"},  {KEY_R, "R"},  {KEY_DUP, "Up"}, {KEY_DDOWN, "Down"}, {KEY_DLEFT, "Left"},   {KEY_DRIGHT, "Right"},
                  {KEY_A, "A"},  {KEY_B, "B"},  {KEY_X, "X"},    {KEY_Y, "Y"},        {KEY_START, "Start"}, {KEY_SELECT, "Select"}};
    std::string out;
    for (const auto& k : kNames) {
        if (keys & k.key) {
            out += (out.empty() ? "" : "+") + std::string(k.name);
        }
    }
    return out;
}

// The new chord is pressed, not typed: the keys held together, taken when they are let go (Select is added: the plugin always wants it).
// X alone gives back the default, B alone leaves the chord as it was. The key that opened the popup is let go first.
class ChordPage : public Page {
public:
    using SetFn = std::function<void(const std::string& chord)>;
    ChordPage(const std::string& title, const std::string& fallback, SetFn set) : title_(title), fallback_(fallback), set_(std::move(set)) {}

    void Update(const Input&) override {
        const u32 held = hidKeysHeld() & kChordKeys;
        if (!armed_) {
            armed_ = held == 0;
            return;
        }
        if (held != 0) {
            taken_ |= held;
            return;
        }
        if (taken_ == 0) {
            return;
        }
        const u32 keys = taken_;
        taken_ = 0;
        if (keys == KEY_B) {
            PopPage();
        } else if (keys == KEY_X) {
            Done(fallback_);
        } else if (__builtin_popcount(keys | KEY_SELECT) < 2) {
            g.ShowToast("Hold two keys or more together (Select is added by itself).", col::kWarn, 3500);
        } else {
            Done(ChordText(keys | KEY_SELECT));
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
        DrawPopupCard(30, 180, title_,
                      "Hold the new keys together, then let go of them: Select is always added. X alone: the default (" + fallback_ +
                          "). B alone: leave it as it is.",
                      5);
        const u32 now = armed_ ? (hidKeysHeld() & kChordKeys) | taken_ : 0;
        gfx::DrawText(160, 172, 0.6f, now ? col::kAccent : col::kDim, now ? ChordText(now | KEY_SELECT) : std::string("..."), gfx::Align::Center);
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    void Done(const std::string& chord) {
        PopPage();
        set_(chord);
    }

    std::string title_, fallback_;
    SetFn set_;
    bool armed_ = false; // every key was let go once since the popup opened
    u32 taken_ = 0;      // the keys held since the last time none was
};

// The menu's chord and the chat's: not the same, and neither within the other (azl::ini::ChordsNested: holding L+Left+Select would open
// a menu on L+Select first, and the chat would never open).
bool ChordsClash(const std::string& a, const std::string& b) {
    return azl::ini::ChordsNested(a, b);
}

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

        // (the test belongs with the joining of the room; Radio-like data and End session on sleep are not rows any more since 1.1.1: their
        // keys keep their values in the ini, radio_like=1 and sleep_stop=0 for a new file)
        Row test;
        test.id = kTest;
        test.kind = Kind::Link;
        test.label = "Test the connection";
        test.note_title = "Test the connection";
        test.note = "Joins the room from here, shows who is in it, then leaves. Checks the address, the password "
                    "and the WiFi. A room that passes goes to the top of your list of rooms.";
        add(test);

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

        Row menu;
        menu.id = kMenu;
        menu.kind = Kind::Toggle;
        menu.label = "In-game menu";
        menu.on = setup::MenuOn(g.cfg);
        menu.gap = 6.0f; // (the in-game menu and the chat: a block of their own)
        menu.note_title = "In-game menu";
        menu.note = "ON: the plugin's menu opens in a title with " +
                    setup::MenuChordText(menu.on ? g.cfg.menu_key : g.cfg.menu_chord) +
                    ": the room you are in, another room (your rooms, the public lobbies), the chat's switch and corner, the debug log. OFF: it never opens; everything is "
                    "set from this app, the room is always joined at title start, and errors still appear on screen. Applies from the "
                    "next title start.";
        add(menu);

        Row menu_keys;
        menu_keys.id = kMenuKeys;
        menu_keys.kind = Kind::Value;
        menu_keys.label = "Menu keys";
        menu_keys.value = setup::MenuChordText(menu.on ? g.cfg.menu_key : g.cfg.menu_chord);
        menu_keys.dim = !menu.on;
        menu_keys.note_title = "Menu keys";
        menu_keys.note = std::string("The keys that open the in-game menu, held together. A: press the new ones. The default is ") +
                         azl::ini::kDefaultMenuKey + ", beside Rosalina's L + Down + Select.";
        add(menu_keys);

        Row chat;
        chat.id = kChat;
        chat.kind = Kind::Toggle;
        chat.label = "In-game chat";
        chat.on = g.cfg.chat;
        chat.note_title = "In-game chat";
        chat.note = "ON: the messages of the room's chat show over the title, and " + setup::MenuChordText(g.cfg.chat_key) +
                    " opens the chat to write. OFF: nothing of the chat is shown or kept. It can be switched in a title too, from the "
                    "in-game menu.";
        add(chat);

        Row chat_keys;
        chat_keys.id = kChatKeys;
        chat_keys.kind = Kind::Value;
        chat_keys.label = "Chat keys";
        chat_keys.value = setup::MenuChordText(g.cfg.chat_key);
        chat_keys.dim = !g.cfg.chat;
        chat_keys.note_title = "Chat keys";
        chat_keys.note = std::string("The keys that open the chat in a title, held together. A: press the new ones. The default is ") +
                         azl::ini::kDefaultChatKey + ", the mirror of the menu's.";
        add(chat_keys);

        Row chat_pos;
        chat_pos.id = kChatPos;
        chat_pos.kind = Kind::Value;
        chat_pos.label = "Chat position";
        chat_pos.value = azl::ini::ChatPosLabel(g.cfg.chat_pos);
        chat_pos.dim = !g.cfg.chat;
        chat_pos.note_title = "Chat position";
        chat_pos.note = std::string("The corner of the top screen where the messages of the chat show. The plugin's notifications show in the "
                                    "opposite corner, so they never cover the chat. A or right: the next corner, left: the one before. It can "
                                    "be changed in a title too, from the in-game menu. The default is ") +
                        azl::ini::ChatPosLabel(azl::ini::kChatPosDefault) + ".";
        add(chat_pos);

        Row check;
        check.gap = 6.0f;
        check.id = kCheck;
        check.kind = Kind::Link;
        check.label = "Installation check";
        check.value = setup::InstallSummary(g.install);
        check.value_color = g.install.problems > 0 ? col::kOff : (g.install.warnings > 0 ? col::kWarn : col::kOn);
        check.note_title = "Installation check";
        check.note = "What 3DS Link needs on the SD card: the plugin file next to this app, the plugin folder, boot9.bin, the card itself. "
                     "A on a line that is not right fixes it: search the card for the plugin, make the folder, find boot9.bin.";
        add(check);

        // The update from GitHub: the row that asks now, and the switch of the quiet check at every start (both in the app's own file).
        Row updates;
        updates.id = kCheckUpdates;
        updates.kind = Kind::Link;
        updates.label = update::kCheckRowLabel;
        const update::Status update_status = update::Worker::Get().Read();
        if (update_status.stage == update::Stage::Checked && update::IsNewer(update_status.latest, setup::kVersion)) {
            updates.value = "v" + update_status.latest;
            updates.value_color = col::kOn;
        }
        updates.note_title = update::kCheckRowLabel;
        updates.note = update::kCheckRowNote;
        add(updates);

        Row update_at_start;
        update_at_start.id = kUpdateAtStart;
        update_at_start.kind = Kind::Toggle;
        update_at_start.label = update::kAtStartRowLabel;
        update_at_start.on = g.app_cfg.update_check;
        update_at_start.note_title = update::kAtStartRowLabel;
        update_at_start.note = update::kAtStartRowNote;
        add(update_at_start);

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
        case kMenuKeys:
            if (step == 0) {
                PushPage(new ChordPage("Menu keys", azl::ini::kDefaultMenuKey, [](const std::string& chord) {
                    if (ChordsClash(chord, g.cfg.chat_key)) {
                        g.ShowToast("Those keys clash with the chat's: choose others.", col::kWarn, 4000);
                        return;
                    }
                    g.cfg.menu_chord = chord;
                    if (setup::MenuOn(g.cfg)) {
                        g.cfg.menu_key = chord;
                    }
                    g.Save();
                    g.ShowToast("Menu keys: " + chord, col::kOn, 3000);
                }));
            }
            break;
        case kChat:
            if (SetSwitch(g.cfg.chat, step)) {
                g.Save();
            }
            break;
        case kChatPos:
            g.cfg.chat_pos = azl::ini::NextChatPos(g.cfg.chat_pos, step);
            g.Save();
            break;
        case kChatKeys:
            if (step == 0) {
                PushPage(new ChordPage("Chat keys", azl::ini::kDefaultChatKey, [](const std::string& chord) {
                    if (ChordsClash(chord, setup::MenuOn(g.cfg) ? g.cfg.menu_key : g.cfg.menu_chord)) {
                        g.ShowToast("Those keys clash with the menu's: choose others.", col::kWarn, 4000);
                        return;
                    }
                    g.cfg.chat_key = chord;
                    g.Save();
                    g.ShowToast("Chat keys: " + chord, col::kOn, 3000);
                }));
            }
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
        case kCheckUpdates:
            if (step == 0) {
                PushPage(pages::NewUpdateCheckPage());
            }
            break;
        case kUpdateAtStart: {
            bool on = g.app_cfg.update_check;
            if (SetSwitch(on, step)) {
                g.app_cfg.update_check = on;
                g.SaveAppConfig();
            }
            break;
        }
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
    enum Id { kAutoconnect = 1, kDebug, kMenu, kMenuKeys, kChat, kChatKeys, kChatPos, kTest, kCheck, kAbout, kNickname, kDlpDelivery, kCheckUpdates, kUpdateAtStart };

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
