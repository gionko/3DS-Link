// Page: the main page. The 3DS Link switch, the room (its name, address, port and password are one thing), the titles that run with 3DS Link, and
// the doors to the other pages. The top screen tells the state of everything 3DS Link depends on: the console's network, the Plugin Loader, the
// player's name, the room, the installation (which of its parts is wrong, in a few words), and the note of the selected row; its bar has the
// battery and the free space of the card at its right.

#include "copyworker.hpp"
#include "delivery.hpp"
#include "dlp_text.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"
#include "titles.hpp"

using namespace ui;

namespace {

// Every line of the info block is this size (the captions too): the block is uniform, a little smaller than a note, so that all of it fits.
constexpr float kInfoScale = 0.48f;

void DrawCaption(float x, float yc, const char* text) {
    gfx::DrawText(x, yc, kInfoScale, col::kDim, text);
}

// A value that never changes size: what does not fit is shortened with "...", not shrunk.
void DrawValue(float x, float yc, u32 color, const std::string& text, float max_w) {
    gfx::DrawText(x, yc, kInfoScale, color, gfx::Ellipsize(text, kInfoScale, max_w));
}

class MainPage : public ListPage {
public:
    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());

        // Status card.
        DrawCard(10, 32, 380, 106);
        // The plugin is OFF for a whole launch when enabled=0 OR tunnel=0; and 3DS Link is OFF while something shuts the switch (a Luma3DS that is too old, a plugin that is
        // not on the card).
        const bool on = setup::SwitchIsOn(g.cfg.enabled, g.cfg.tunnel, SwitchLockNow());
        gfx::FillCircle(28, 48, 7, on ? col::kOn : col::kOff);
        const char* state = on ? "3DS Link is ON" : "3DS Link is OFF";
        gfx::DrawText(44, 48, 0.65f, col::kText, state);
        // The one message of the moment, right of the state: the last toast, an error, the hint of the scan, or what the state means.
        const titles::Scanner& scan = titles::Scan();
        std::string message;
        u32 message_color = col::kDim;
        if (g.ToastActive()) {
            message = g.toast;
            message_color = g.toast_color;
        } else if (!g.save_ok) {
            message = "The settings file could not be written!";
            message_color = col::kOff;
        } else if (g.luma_too_old) {
            message = "This Luma is not supported";
            message_color = col::kOff;
        } else if (g.plugin_locked) {
            message = "The plugin file is not right";
            message_color = col::kOff;
        } else if (!on) {
            message = "Titles use the real local wireless.";
            message_color = col::kWarn;
        } else if (!g.soc_ok()) {
            message = Fmt("Sockets unavailable (0x%08lX)", static_cast<unsigned long>(g.soc_rc));
            message_color = col::kOff;
        } else if (scan.started() && !scan.done()) {
            message = "Updating the list of titles... " + setup::ScanProgressText(scan.progress());
        } else {
            message = "Changes apply when a title starts.";
        }
        const float message_x = 44.0f + gfx::TextWidth(state, 0.65f) + 14.0f;
        const std::string clean_message = gfx::Clean(message);
        if (g.ToastActive() && gfx::TextWidth(clean_message.c_str(), 0.32f) > 382.0f - message_x) {
            // A toast that is too long for its place (the reason the plugin is not on the card is a sentence) runs by, letter by letter, from the moment it was shown, so
            // that all of it can be read; the note of the selected row has it whole.
            gfx::DrawText(message_x, 49, 0.36f, message_color, gfx::MarqueeText(clean_message, 0.36f, 382.0f - message_x, NowMs() - g.toast_from));
        } else {
            gfx::DrawFitted(382, 49, 0.42f, 0.32f, 382.0f - message_x, message_color, clean_message, gfx::Align::Right);
        }
        gfx::FillRect(14, 62, 372, 1, col::kLine);

        // The info block: four lines of ONE size. Two columns for the first two; the room and the installation have a whole line each (a room has
        // a name and an address, the installation says what is wrong and where to go). The values start at the same x on every line of the left
        // column. (The battery and the card are in the bar above.)
        const float col1 = 18.0f, col2 = 204.0f, edge = 386.0f;
        const float v1 = col1 + gfx::TextWidth("Installation", kInfoScale) + 8.0f;
        const float v2 = col2 + gfx::TextWidth("Plugin Loader", kInfoScale) + 8.0f;
        const float limit1 = col2 - 6.0f;
        const float pitch = 17.0f;
        float yc = 77.0f;

        DrawCaption(col1, yc, "WiFi");
        if (g.net.known && g.net.wifi) {
            DrawSignalBars(v1, yc, g.net.strength);
            DrawValue(v1 + 20.0f, yc, col::kText, "connected", limit1 - (v1 + 20.0f));
        } else if (g.net.known) {
            DrawSignalBars(v1, yc, 0);
            DrawValue(v1 + 20.0f, yc, col::kWarn, "not connected", limit1 - (v1 + 20.0f));
        } else {
            DrawValue(v1, yc, col::kDim, "unknown", limit1 - v1);
        }
        DrawCaption(col2, yc, "Plugin Loader");
        const u32 loader_dot = g.loader == setup::LoaderState::Enabled ? col::kOn
                               : g.loader == setup::LoaderState::Disabled ? col::kWarn : col::kDim;
        DrawDotText(v2, yc, loader_dot, g.loader == setup::LoaderState::Disabled ? col::kWarn : col::kText,
                    setup::LoaderStateText(g.loader), kInfoScale, edge - v2);

        yc += pitch;
        DrawCaption(col1, yc, "Name");
        DrawValue(v1, yc, col::kText, gfx::Clean(g.EffectiveNickname()) + (g.cfg.nickname.empty() ? " (console)" : ""), limit1 - v1);
        DrawCaption(col2, yc, "Join");
        DrawValue(v2, yc, col::kText, g.cfg.autoconnect || !setup::MenuOn(g.cfg) ? "at title start" : "from the menu", edge - v2);

        yc += pitch;
        DrawCaption(col1, yc, "Room");
        const std::string room_name = RoomName();
        DrawValue(v1, yc, g.cfg.host.empty() ? col::kWarn : col::kText,
                  gfx::Clean(room_name.empty() ? RoomText() : room_name + "  " + RoomText()), edge - v1);

        yc += pitch;
        DrawCaption(col1, yc, "Installation");
        // Green when all is well, red while something has to be put right, yellow when only something that is not needed is missing.
        const u32 install_color = g.install.problems > 0 ? col::kOff : (g.install.warnings > 0 ? col::kWarn : col::kOn);
        const setup::InstallLine line = setup::InstallLineText(g.install);
        const float text_w = edge - (v1 + 14.0f);
        DrawDotText(v1, yc, install_color, install_color == col::kOn ? col::kText : install_color,
                    gfx::TextWidth(line.full, kInfoScale) <= text_w ? line.full : line.compact, kInfoScale, edge - v1);

        if (const Row* row = Selected()) {
            DrawNote(10, 142, 380, 92, *row);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    // Back from another page (the Installation check, a popup, the Enable page...): what is on the card may have changed.
    void OnResume() override {
        g.RefreshInstall();
    }

    // The quiet look at the copies of the plugin (once a start, in the thread of the copies) says so when it made one. What the app did about its own plugin at the start
    // (put it on the card, or could not) is said once, here: the first page the user sees.
    void Update(const Input& in) override {
        ListPage::Update(in);
        if (!g.plugin_notice.empty()) {
            g.ShowToast(g.plugin_notice, g.plugin_notice_color, 7000);
            g.plugin_notice.clear();
        }
        // The quiet look at the copies of the plugin in the titles' folders starts with the app (it does not wait for the scan): while it runs the toast says how many folders
        // are left, and when it is over it says the result, so that whoever has just installed an update knows when it is done and the app can be closed.
        int refreshed = 0;
        if (g.plugin_pass_open) {
            const int left = setup::CopyWorker::Get().PendingCount();
            if (left > 0) {
                if (left != pass_left_shown_) {
                    pass_left_shown_ = left;
                    g.ShowToast(Fmt("Updating the plugin in your games: %d of %d folders done. Please wait.", g.plugin_pass_total > left ? g.plugin_pass_total - left : 0, g.plugin_pass_total), col::kWarn, 3000);
                }
                return;
            }
            g.plugin_pass_open = false;
            pass_left_shown_ = 0;
            refreshed = setup::CopyWorker::Get().TakeRefreshedCount();
            if (refreshed == 0) {
                g.ShowToast("The plugin is up to date in all your games. You can close 3DS Link.", col::kOn, 8000);
                return;
            }
        } else {
            refreshed = setup::CopyWorker::Get().TakeRefreshedCount();
        }
        if (refreshed > 0) {
            g.ShowToast(Fmt("Plugin updated in %d title%s. You can close 3DS Link.", refreshed, refreshed == 1 ? "" : "s"), col::kOn, 8000);
        }
    }

protected:
    const char* Title() const override {
        return "3DS Link";
    }
    const char* BarTitle() const override {
        return "Options";
    }
    const char* Hint() const override {
        if (GuideKeyOffered()) {
            return dlp::kGuideHint; // (only while the entry that needs boot9.bin is under the cursor and boot9.bin is not there)
        }
        return ""; // (nothing between the buttons: the keys are the usual ones)
    }
    bool IsRoot() const override {
        return true;
    }
    // Y opens the guide "How to get boot9.bin" on the entry that is shut for the lack of it (the footer's hint says so).
    void OnKeys(const Input& in) override {
        if ((in.down & KEY_Y) && GuideKeyOffered()) {
            if (!pages::OpenBoot9Guide()) {
                g.ShowToast(dlp::kNoGuideToast, col::kWarn, 3500);
            }
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();

        // 3DS Link's own: the switch, the room, the titles that run with it. One block.
        // With a Luma3DS that is too old the three rows of the Azahar side are shut: faint, off, and A says why (the Plugin Loader that games need to load
        // the plugin does not exist before Luma3DS 13.0). Titles Delivery and Settings do not need it and stay as they are.
        const std::string too_old = "Needs Luma3DS 13.0 or newer: this console runs " + g.luma_text + ".\n";

        // The switch is shut by a plugin that is not on the card too (setup::LockOfSwitch): 3DS Link carries its plugin and puts it in sdmc:/3ds/3DSLink by itself, and
        // without it there is nothing to switch on. It stays OFF until the file is back (the next start, or Settings > Installation check) and the user switches it on.
        const setup::SwitchLock lock = SwitchLockNow();
        Row power;
        power.id = kPower;
        power.kind = Kind::Toggle;
        power.label = "3DS Link";
        power.on = setup::SwitchIsOn(g.cfg.enabled, g.cfg.tunnel, lock);
        power.dim = lock != setup::SwitchLock::None;
        power.note_title = g.luma_too_old ? "3DS Link is off: Luma3DS is too old" : (g.plugin_locked ? "3DS Link is off: plugin file" : "3DS Link on / off");
        power.note_color = lock != setup::SwitchLock::None ? col::kOff : 0;
        power.note = (g.luma_too_old ? too_old + "It stays off until Luma3DS is updated, and then until you switch it on.\n\n"
                                     : (g.plugin_locked ? g.PluginLockText() + (setup::EmbeddedPluginUsable() ? std::string(" ") + setup::kPluginLockMore : std::string()) + "\n\n" : std::string())) +
                     "ON: a title's local wireless goes through the Azahar room, so you can play with other consoles and Azahar "
                     "players over the internet.\n"
                     "OFF: 3DS Link does nothing; titles use the real wireless. Applies from the next title start.";
        rows.push_back(power);

        Row room;
        room.id = kRoom;
        room.kind = Kind::Link;
        room.label = "Azahar Emulator Room";
        room.value = g.cfg.host.empty() ? RoomText() : RoomLabel();
        room.value_color = g.luma_too_old ? col::kDim : (g.cfg.host.empty() ? col::kWarn : 0);
        room.dim = g.luma_too_old;
        room.note_title = g.luma_too_old ? "Azahar Emulator Room: Luma3DS is too old" : "Azahar Emulator Room";
        room.note_color = g.luma_too_old ? col::kOff : 0;
        room.note = (g.luma_too_old ? too_old : std::string()) +
                    "The Azahar room this console joins. A opens your list of rooms: each one has a name, an address, a port and its own "
                    "password. Pick one, add one or change one.";
        rows.push_back(room);

        Row enable;
        enable.id = kEnableGame;
        enable.kind = Kind::Link;
        enable.label = "Enable Titles for Azahar Emulator Net";
        enable.dim = g.luma_too_old;
        enable.note_title = g.luma_too_old ? "Enable Titles for Azahar Emulator Net: Luma3DS is too old" : "Enable Titles for Azahar Emulator Net";
        enable.note_color = g.luma_too_old ? col::kOff : 0;
        enable.note = (g.luma_too_old ? too_old : std::string()) +
                      "Lets your titles connect to an Azahar emulator network room: the one in Azahar Emulator Room. Your installed titles are listed, "
                      "each with a switch; a title that is switched off never notices 3DS Link.";
        rows.push_back(enable);

        Row delivery;
        delivery.id = kGameDelivery;
        delivery.kind = Kind::Link;
        delivery.label = "Titles Delivery";
        delivery.gap = 6.0f;
        delivery.note_title = "Titles Delivery";
        delivery.note = "Send the titles installed on this console to other consoles, or receive titles from one, over the local "
                        "wireless: no internet, no media.";
        rows.push_back(delivery);

        // 3DS Link Delivery needs neither the Plugin Loader nor the scan of the titles: an old Luma3DS does not shut it, like Titles Delivery and Settings. It is shut
        // by its own switch (Settings > Download Play delivery: nothing of it then runs) and, as Titles Delivery's Send Titles is, by a missing boot9.bin: sharing needs
        // that file, 3DS Link does not. A shut entry is faint, says why at the right of its row and in its note, and A answers with a line; Y opens the guide.
        const dlp::Gate gate = EntryGate();
        Row dlp;
        dlp.id = kDlpDelivery;
        dlp.kind = Kind::Link;
        dlp.label = "3DS Link Delivery";   // (no gap above it: with Titles Delivery it is one service in two parts)
        dlp.dim = gate != dlp::Gate::Open;
        dlp.value = dlp::GateValue(gate);
        dlp.value_color = gate == dlp::Gate::NoBoot9 ? col::kWarn : col::kDim;
        dlp.note_title = dlp::GateNoteTitle(gate);
        dlp.note_color = gate == dlp::Gate::NoBoot9 ? col::kWarn : 0;
        dlp.note = dlp::GateNote(gate);
        rows.push_back(dlp);

        Row config;
        config.id = kSettings;
        config.kind = Kind::Link;
        config.label = "Settings";
        config.gap = 6.0f;
        config.note_title = "Settings";
        config.note = "Your nickname, when to join the room, the in-game menu, the log, the connection test, the installation check, and About.";
        rows.push_back(config);
    }

    void Activate(const Row& row, int step) override {
        switch (row.id) {
        case kPower: {
            if (g.luma_too_old) {
                g.ShowToast("This Luma is not supported", col::kWarn, 4500);
                break;
            }
            if (g.plugin_locked) {
                g.ShowToast(g.PluginLockText(), col::kWarn, 12000); // (A and the D-pad alike: the sentence says what is missing and why it could not be put there)
                break;
            }
            const bool is_on = g.cfg.enabled && g.cfg.tunnel;
            const bool want = SwitchTarget(is_on, step);
            if (want == is_on) {
                break;
            }
            g.cfg.enabled = want;
            if (want) {
                g.cfg.tunnel = true; // "ON" must repair a leftover tunnel=0, which also keeps the plugin off
            }
            g.Save();
            break;
        }
        case kRoom:
            if (step == 0 && g.luma_too_old) {
                g.ShowToast("This Luma is not supported", col::kWarn, 4500); // (the note above says what is needed and what this console runs)
            } else if (step == 0) {
                PushPage(pages::NewServersPage());
            }
            break;
        case kEnableGame:
            if (step == 0 && g.luma_too_old) {
                g.ShowToast("This Luma is not supported", col::kWarn, 4500);
            } else if (step == 0) {
                PushPage(pages::NewEnableGamePage());
            }
            break;
        case kGameDelivery:
            if (step == 0) {
                PushPage(pages::NewDeliveryPage());
            }
            break;
        case kDlpDelivery:
            if (step == 0) {
                g.RefreshBoot9(); // (the card is looked at now: a boot9.bin put there since the last look counts)
                const dlp::Gate gate = EntryGate();
                if (gate != dlp::Gate::Open) {
                    g.ShowToast(dlp::GateToast(gate), col::kWarn, 4500);
                    break;
                }
                PushPage(pages::NewDlpHostPage());
            }
            break;
        case kSettings:
            if (step == 0) {
                PushPage(pages::NewConfigPage());
            }
            break;
        default:
            break;
        }
    }

private:
    enum Id { kPower = 1, kRoom, kEnableGame, kGameDelivery, kDlpDelivery, kSettings };

    int pass_left_shown_ = 0; // the number of folders of the pass of quiet looks the toast last said (so that it is said again only when it changes)

    static setup::SwitchLock SwitchLockNow() {
        return setup::LockOfSwitch(g.luma_too_old, g.plugin_ready);
    }

    // Whether the entry is open, from what the app already knows (no file is read: the key file was looked for when the main page came back into view).
    static dlp::Gate EntryGate() {
        return dlp::GateOf(g.app_cfg.dlp_delivery, g.boot9.found);
    }

    // The guide's key is offered on the entry that is shut for the lack of boot9.bin, and only while the cursor is on it.
    bool GuideKeyOffered() const {
        const Row* row = Selected();
        return row != nullptr && row->id == kDlpDelivery && EntryGate() == dlp::Gate::NoBoot9;
    }
};

} // namespace

namespace pages {

ui::Page* NewMainPage() {
    return new MainPage;
}

} // namespace pages
