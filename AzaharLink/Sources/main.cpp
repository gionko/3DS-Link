// AzaharLink - tunnels a 3DS game's local wireless (nwm::UDS) through an Azahar multiplayer room.
#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "azl/chat.hpp"
#include "azl/ini_values.hpp"
#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "azl/text_wrap.hpp"
#include "ctr/chat_overlay.hpp"
#include "ctr/plugin.hpp"

// libctru's hidInit() also opens the ir:rst service on a New 3DS (C-stick, ZL, ZR), and the framework calls it inside the
// game's own process. ir:rst only has two sessions (Rosalina's menu redefines this for the same reason): the plugin took one
// and the game, which reads its C-stick from ir:rst, was left without (Monster Hunter 4 Ultimate: dead C-stick whenever the
// plugin was in the game's folder, even with the main switch off). The plugin needs neither the C-stick nor ZL/ZR.
extern "C" bool hidShouldUseIrrst(void) {
    return false;
}

namespace CTRPluginFramework {

using azl::ctr::Config;
using azl::ctr::Plugin;

static MenuEntry* g_address_entry = nullptr;
static MenuEntry* g_nickname_entry = nullptr;
static MenuEntry* g_password_entry = nullptr;
static MenuEntry* g_connect_entry = nullptr;
static MenuEntry* g_radio_entry = nullptr;
static MenuEntry* g_enabled_entry = nullptr;
static MenuEntry* g_debug_entry = nullptr;
static MenuEntry* g_status_entry = nullptr;
static MenuEntry* g_rooms_entry = nullptr;
static MenuEntry* g_chatpos_entry = nullptr;
static MenuEntry* g_chatbtn_entry = nullptr;
static std::string g_last_status;
static bool g_last_radio = true;
static int g_radio_level = 1; // what ON stands for (ini radio_like 1 or 2): the in-game entry is a switch, the level is the file's
static u32 g_menu_keys = Key::Select;
static bool g_menu_off = false; // menu_key=off: the menu never opens

// ---------------------------------------------------------------------------------------------
// Menu hotkey
// ---------------------------------------------------------------------------------------------

// The framework opens its menu when Select is pressed, but plenty of games use Select themselves.
// OnOpening() lets us veto the opening unless the configured chord (default L+R+Select) is held, and
// with menu_key=off it vetoes every opening.

static u32 ParseKeys(const std::string& text) {
    u32 keys = 0;
    std::string token;
    const auto flush = [&]() {
        std::string t;
        for (char c : token) {
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        token.clear();
        if (t == "select") keys |= Key::Select;
        else if (t == "start") keys |= Key::Start;
        else if (t == "a") keys |= Key::A;
        else if (t == "b") keys |= Key::B;
        else if (t == "x") keys |= Key::X;
        else if (t == "y") keys |= Key::Y;
        else if (t == "l") keys |= Key::L;
        else if (t == "r") keys |= Key::R;
        else if (t == "up") keys |= Key::DPadUp;
        else if (t == "down") keys |= Key::DPadDown;
        else if (t == "left") keys |= Key::DPadLeft;
        else if (t == "right") keys |= Key::DPadRight;
    };
    for (char c : text) {
        if (c == '+' || c == ' ' || c == ',') {
            flush();
        } else {
            token += c;
        }
    }
    flush();
    return keys;
}

static bool OnMenuOpening() {
    if (g_menu_off) {
        return false; // in no case: the settings and the Setup app are the way to change anything
    }
    // Without Select in the chord the menu could never open (the framework only asks when Select
    // is pressed), so Select is always part of it.
    const bool open = Controller::IsKeysDown(g_menu_keys);
    if (open) {
        azl::Log("menu: opened"); // (a veto is not logged: a game that uses Select would fill the log with them)
    }
    return open;
}

static void OnMenuClosing() {
    azl::Log("menu: closed");
}

// ---------------------------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------------------------

static std::string AddressLabel() {
    const Config& cfg = Plugin::Get().config();
    if (cfg.host.empty()) {
        return "Room address: (not set)";
    }
    return "Room address: " + cfg.host + ":" + std::to_string(cfg.port);
}

static void RefreshLabels() {
    Plugin& plugin = Plugin::Get();
    const Config& cfg = plugin.config();
    g_address_entry->Name() = AddressLabel();
    g_nickname_entry->Name() =
        "Nickname: " + plugin.EffectiveNickname() + (cfg.nickname.empty() ? " (from console)" : "");
    g_password_entry->Name() = std::string("Password: ") + (cfg.password.empty() ? "(none)" : "(set)");
    g_enabled_entry->Name() =
        std::string("AzaharLink at game start: ") + (cfg.enabled ? "ON" : "OFF (next launch)");
    g_debug_entry->Name() = std::string("Debug log: ") + (cfg.debug > 0 ? "ON (everything)" : "OFF");
    if (g_chatpos_entry) {
        g_chatpos_entry->Name() = std::string("Chat lines: ") + azl::chat::ChatPositionName(chat_overlay::Position());
    }
    if (g_chatbtn_entry) {
        g_chatbtn_entry->Name() = std::string("Chat button: ") + azl::chat::ChatPositionName(chat_overlay::ButtonPosition());
    }
}

// ---------------------------------------------------------------------------------------------
// Editors
// ---------------------------------------------------------------------------------------------

static bool ValidNickname(const std::string& s) {
    // Same rule as the room server: ^[ a-zA-Z0-9._-]{4,20}$
    if (s.size() < 4 || s.size() > 20) {
        return false;
    }
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool NicknameCheck(const void* input, std::string& error) {
    const std::string& s = *static_cast<const std::string*>(input);
    if (!s.empty() && !ValidNickname(s)) {
        error = "4-20 characters: letters, digits,\nspace . _ -\n(or leave empty)";
        return false;
    }
    return true;
}

static void EditAddress(MenuEntry*) {
    Config& cfg = Plugin::Get().config();
    std::string input = cfg.host;
    if (!input.empty() && cfg.port != azl::room::kDefaultPort) {
        input += ":" + std::to_string(cfg.port);
    }
    Keyboard keyboard("Azahar room address\n\nIP or host name, optionally followed by\n:port (default 24872)");
    keyboard.SetMaxLength(80);
    if (keyboard.Open(input, input) < 0) {
        return;
    }
    // Trim spaces.
    while (!input.empty() && input.back() == ' ') {
        input.pop_back();
    }
    while (!input.empty() && input.front() == ' ') {
        input.erase(input.begin());
    }

    std::string host = input;
    long port = azl::room::kDefaultPort;
    const std::size_t colon = input.rfind(':');
    if (colon != std::string::npos) {
        host = input.substr(0, colon);
        port = std::strtol(input.c_str() + colon + 1, nullptr, 10);
    }
    if (port <= 0 || port > 65535) {
        OSD::Notify("Invalid port");
        return;
    }
    cfg.host = host;
    cfg.port = static_cast<u16>(port);
    Plugin::Get().SaveConfig();
    RefreshLabels();
    azl::Log("menu: room address set to '%s:%u' (applies the next time the room is joined)", cfg.host.c_str(), cfg.port);
}

static void EditNickname(MenuEntry*) {
    Config& cfg = Plugin::Get().config();
    std::string input = cfg.nickname;
    Keyboard keyboard("Your nickname in the room\n\nLeave empty to use the name set\non the console.");
    keyboard.SetMaxLength(20);
    keyboard.SetCompareCallback(NicknameCheck);
    if (keyboard.Open(input, input) < 0) {
        return;
    }
    cfg.nickname = input;
    Plugin::Get().SaveConfig();
    RefreshLabels();
    azl::Log("menu: nickname set to '%s'%s", cfg.nickname.c_str(), cfg.nickname.empty() ? " (the console's name)" : "");
    OSD::Notify("Nickname applies the next time you join");
}

static void EditPassword(MenuEntry*) {
    Config& cfg = Plugin::Get().config();
    std::string input = cfg.password;
    Keyboard keyboard("Room password\n(leave empty if the room has none)");
    keyboard.SetMaxLength(64);
    if (keyboard.Open(input, input) < 0) {
        return;
    }
    cfg.password = input;
    Plugin::Get().SaveConfig();
    RefreshLabels();
    azl::Log("menu: room password %s", cfg.password.empty() ? "cleared" : "set (never written to the log)");
}

static void ToggleEnabled(MenuEntry*) {
    Config& cfg = Plugin::Get().config();
    cfg.enabled = !cfg.enabled;
    if (cfg.enabled) {
        cfg.tunnel_enabled = true; // "on" means on: a leftover tunnel=0 would keep it off
    }
    Plugin::Get().SaveConfig();
    RefreshLabels();
    azl::Log("menu: 'AzaharLink at game start' switched %s", cfg.enabled ? "ON" : "OFF");
    OSD::Notify(cfg.enabled ? "AzaharLink will be ON at the next game start"
                            : "AzaharLink will be OFF at the next game start");
}

static void ToggleDebug(MenuEntry*) {
    Plugin& plugin = Plugin::Get();
    const bool turned_on = plugin.debug_level() == 0;
    const bool ring_ok = plugin.SetDebugLevel(turned_on ? 1 : 0);
    RefreshLabels();
    if (!turned_on) {
        OSD::Notify("Debug log OFF: nothing is written");
    } else if (ring_ok) {
        OSD::Notify("Debug log ON: AzaharLink.log + AzaharLink.debug.log");
    } else {
        OSD::Notify("Debug log ON: no memory for the capture, log only");
    }
}

static std::string Wrapped(const std::string& text, std::size_t width);

/// The menu's browser of the public rooms (the list the Setup app saved): pages of rooms, the last room joined first, a question
/// before joining, and the password when the room has one.
static void BrowsePublicRooms(MenuEntry*) {
    std::string text;
    if (!Plugin::Get().ReadRoomList(0, text)) {
        MessageBox("Public rooms",
                   "No public room list yet.\n\nOpen the 3DS Link app, go to the lobby page and\n"
                   "refresh it: the list is saved for the game (and the\nnext time you open this menu it is here).")();
        return;
    }
    std::vector<azl::chat::RoomEntry> rooms = azl::chat::ParseRoomList(text, azl::chat::kPublicRooms);
    text.clear();
    if (rooms.empty()) {
        MessageBox("Public rooms", "The saved list has no rooms.\nRefresh the lobby page in the 3DS Link app.")();
        return;
    }
    std::string last_host;
    u16 last_port = 0;
    const bool has_last = Plugin::Get().LoadLastRoom(last_host, last_port) && azl::chat::MoveRoomFirst(rooms, last_host, last_port);
    const std::size_t pages = azl::chat::PageCount(rooms.size());
    std::size_t page = 0;
    enum class Act { Room, Next, Prev, Jump };
    for (;;) {
        std::vector<std::string> options;
        std::vector<Act> acts;
        const std::size_t first = azl::chat::PageFirst(page);
        for (std::size_t i = first; i < rooms.size() && i < first + azl::chat::kRoomRows; ++i) {
            // the name as the lobby gives it (what fits the row is shown; the question before joining shows all of it, with the address)
            const std::string name = azl::chat::Clean(rooms[i].name, azl::chat::kRoomNameChars);
            options.push_back(std::string(has_last && i == 0 ? "* " : "") + (name.empty() ? rooms[i].host : name));
            acts.push_back(Act::Room);
        }
        if (pages > 1) {
            options.push_back("Go to page...");
            acts.push_back(Act::Jump);
        }
        if (page + 1 < pages) {
            options.push_back("Next page >>");
            acts.push_back(Act::Next);
        }
        if (page > 0) {
            options.push_back("<< Previous page");
            acts.push_back(Act::Prev);
        }
        Keyboard keyboard("Public rooms  (page " + std::to_string(page + 1) + " of " + std::to_string(pages) +
                              ")\n\nPick a room to join it now." + (has_last && page == 0 ? "\n* = the room you joined last" : ""),
                          options);
        const int choice = keyboard.Open();
        if (choice < 0) {
            return;
        }
        const Act act = acts[static_cast<std::size_t>(choice)];
        if (act == Act::Next) {
            ++page;
        } else if (act == Act::Prev) {
            --page;
        } else if (act == Act::Jump) {
            u32 number = static_cast<u32>(page) + 1; // (the framework's number keyboard takes u32, not int)
            Keyboard jump("Go to page (1 - " + std::to_string(pages) + ")");
            jump.IsHexadecimal(false);
            if (jump.Open(number, number) >= 0) {
                page = static_cast<std::size_t>(number < 1 ? 0 : (number > pages ? pages - 1 : number - 1));
            }
        } else {
            azl::chat::RoomEntry room = rooms[first + static_cast<std::size_t>(choice)];
            if (room.password == azl::chat::kAskPassword) {
                std::string typed;
                Keyboard pw("This room has a password.\n\nType it (empty = back to the list).");
                pw.SetMaxLength(64);
                if (pw.Open(typed, typed) < 0 || typed.empty()) {
                    continue;
                }
                room.password = typed;
            }
            const std::string address = room.host + (room.port != azl::chat::kDefaultRoomPort ? ":" + std::to_string(room.port) : "");
            if (!MessageBox(Wrapped(room.name.empty() ? address : room.name, 40) + "\n" + address,
                            "Leave the game's lobby first:\nswitching drops the game's link.\n\nJoin this room now?",
                            DialogType::DialogOkCancel)()) {
                continue;
            }
            Plugin::Get().SaveLastRoom(room.host, room.port);
            Plugin::Get().SwitchRoom(room.host, room.port, room.password);
            if (g_connect_entry) {
                g_connect_entry->Enable(); // (the switch is applied by the connection logic, which runs while this is on)
            }
            azl::Log("menu: public room '%s:%u' chosen", room.host.c_str(), room.port);
            OSD::Notify("Switching room...");
            return;
        }
    }
}

/// Where the chat lines go on the top screen, or where the small Chat button sits on the bottom screen (CHAT=0 builds leave the overlay out).
static void ChooseChatPlace(bool button) {
    std::vector<std::string> options;
    for (int p = 0; p < azl::chat::kChatPositions; ++p) {
        options.push_back(azl::chat::ChatPositionName(p));
    }
    Keyboard keyboard(button ? "Where should the Chat button be on the\ntouch screen? Pick a spot the game does\nnot use." : "Where should the chat lines be shown\non the top screen?", options);
    const int choice = keyboard.Open();
    if (choice < 0) {
        return;
    }
    if (button) {
        chat_overlay::SetButtonPosition(choice);
    } else {
        chat_overlay::SetPosition(choice);
    }
    Plugin::Get().SaveChatPositions(chat_overlay::Position(), chat_overlay::ButtonPosition());
    RefreshLabels();
    azl::Log("menu: chat %s set to %s", button ? "button" : "lines", azl::chat::ChatPositionName(choice));
}
static void ChooseChatPosition(MenuEntry*) {
    ChooseChatPlace(false);
}
static void ChooseChatButton(MenuEntry*) {
    ChooseChatPlace(true);
}

static void ShowDetails(MenuEntry*) {
    azl::Log("menu: the status details were opened");
    MessageBox("AzaharLink", Plugin::Get().DetailText())();
}

/// Word wrap with explicit line breaks (the notes and the boxes of the framework break lines only where told to).
static std::string Wrapped(const std::string& text, std::size_t width) {
    std::string out;
    for (const std::string& line : azl::WrapLines(text, width)) {
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

static void ShowIssue(MenuEntry*) {
    azl::Log("menu: the reason for OFF was opened");
    MessageBox("AzaharLink is OFF", Wrapped(Plugin::Get().startup_issue(), 44) +
                                        "\n\nThe plugin stays out of this game: the real local\n"
                                        "wireless works." +
                                        (azl::g_debug_level > 0 ? "" : " Set debug=1 for the details in the log."))();
}

static void Noop(MenuEntry*) {}

// ---------------------------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------------------------

static void OnFrame() {
    // Frame statistics (the callback runs once per game frame, synchronised with the swap): the interval
    // between two calls is the frame time as the plugin sees it, and the callback itself is timed.
    static u64 last_start = 0;
    const u64 start = svcGetSystemTick();
    if (last_start && start - last_start < 5ull * 1000 * AZL_TICKS_PER_MS) { // (longer: HOME menu or sleep)
        const u32 gap = static_cast<u32>(start - last_start);
        ++azl::g_stats.frames;
        azl::StatsAdd64(azl::g_stats.frame_gap_sum, gap);
        if (gap > azl::g_stats.frame_gap_max) {
            azl::g_stats.frame_gap_max = gap;
        }
        if (gap > azl::kSlowTicks) {
            ++azl::g_stats.frame_hitches;
        }
    }
    last_start = start;
    Plugin& plugin = Plugin::Get();
    azl::ctr::RearmHandedBackSites(); // (a call handed back to its call site with no swap after it: the site goes back into the hook)

    for (const std::string& message : plugin.TakeNotifications()) {
        OSD::Notify(message);
    }

    const bool want_connected = g_connect_entry->IsActivated();
    if (!plugin.Tick(want_connected)) {
        // Fatal error (wrong password, nickname taken, ...) or nothing to connect to.
        g_connect_entry->Disable();
    }

    const bool radio = g_radio_entry->IsActivated();
    if (radio != g_last_radio) {
        g_last_radio = radio;
        const int level = radio ? g_radio_level : 0;
        azl::Log("menu: 'Radio-like broadcast data' switched %s (radio_like=%d)", radio ? "ON" : "OFF", level);
        plugin.room().SetFastData(level);
        plugin.config().radio_like = level;
        plugin.SaveConfig();
        OSD::Notify(radio ? "Broadcast data: radio-like (unreliable, low lag)"
                          : "Broadcast data: reliable (may stall on a weak link)");
    }

    // The status text copies the room's member list; twice a second is plenty and keeps this
    // per-frame callback (which runs in the game's frame loop) cheap.
    static u32 frame = 0;
    if (frame++ % 30 == 0) {
        const std::string status = plugin.StatusLine();
        if (status != g_last_status) {
            g_last_status = status;
            g_status_entry->Name() = "Status: " + status;
        }
    }
    azl::StatsAdd64(azl::g_stats.t_cb, svcGetSystemTick() - start);
}

static void OnFrameOff() {
    Plugin::Get().RepeatStartupIssue();
    for (const std::string& message : Plugin::Get().TakeNotifications()) {
        OSD::Notify(message);
    }
}

static void OnProcessEvent(Process::Event event) {
    // This runs on the framework's small keep-alive thread: flag stores only, and the swap of the plugin's memory (Old 3DS), which
    // takes the hook out of the game before Luma3DS frees the block. Sleep, home and swap are counted (the log writer reports
    // them); the exit starts the shutdown.
    Plugin::Get().NoteProcessEvent(static_cast<int>(event));
    if (event == Process::Event::EXIT) {
        Plugin::Get().RequestStop();
    }
}

// ---------------------------------------------------------------------------------------------
// Framework entry points
// ---------------------------------------------------------------------------------------------

// Called before main and before the game starts: the one safe moment to patch the game's code.
void PatchProcess(FwkSettings&) {
    // libctrpf 0.8.0 has no OnProcessExit() hook: the exit event is the only notification. It runs
    // on the framework's tiny keep-alive thread, so it only sets flags (see Plugin::RequestStop);
    // the orderly shutdown happens at the end of main().
    Process::SetProcessEventCallback(OnProcessEvent);
    Plugin::Get().EarlyInit();
}

static void InitMenu(PluginMenu& menu) {
    g_status_entry = new MenuEntry("Status: starting...", nullptr, ShowDetails,
                                   "Press A for details: players in the room, MAC address, link\nquality, game fps and plugin CPU, hook and UDS diagnostics.");
    g_address_entry = new MenuEntry("Room address", nullptr, EditAddress,
                                    "IP address (or host name) of the Azahar room.\nPort 24872 is used unless you add :port.");
    g_nickname_entry = new MenuEntry("Nickname", nullptr, EditNickname,
                                     "Name shown to the other players (4-20 chars).\nEmpty = the name set on the console.");
    g_password_entry = new MenuEntry("Password", nullptr, EditPassword, "Only if the room is password protected.");
    g_connect_entry = new MenuEntry("Connect to room", Noop,
                                    "Joins the room now and keeps the connection.\nIt retries by itself if the link drops.");
    g_radio_entry = new MenuEntry("Radio-like broadcast data", Noop,
                                  "On: broadcast game data is sent without retries, like the\n"
                                  "real radio. Avoids freezes on a weak WiFi link.\n"
                                  "Off: everything is retransmitted until it arrives.");
    g_enabled_entry = new MenuEntry("AzaharLink at game start", nullptr, ToggleEnabled,
                                    "Press A to switch AzaharLink on/off for the next game\n"
                                    "start. OFF = the plugin stays out of the game entirely,\n"
                                    "so you can play with real local wireless.");

    g_rooms_entry = new MenuEntry("Public rooms...", nullptr, BrowsePublicRooms,
                                  "Browse the public rooms the Setup app saved from the lobby,\n"
                                  "5 per page, and join one. Refresh the list in the\n"
                                  "3DS Link app (lobby page).");
    if (chat_overlay::Enabled()) {
        g_chatpos_entry = new MenuEntry("Chat lines", nullptr, ChooseChatPosition,
                                        "Where the incoming chat lines appear on the top screen:\n"
                                        "top / bottom, left / middle / right.");
        g_chatbtn_entry = new MenuEntry("Chat button", nullptr, ChooseChatButton,
                                        "Where the small Chat button is on the touch screen.\n"
                                        "Pick a spot where the game has nothing to touch.");
    }

    menu += g_status_entry;
    menu += g_connect_entry;
    menu += g_rooms_entry;
    if (g_chatpos_entry) {
        menu += g_chatpos_entry;
        menu += g_chatbtn_entry;
    }
    menu += g_radio_entry;
    g_debug_entry = new MenuEntry("Debug log", nullptr, ToggleDebug,
                                  "A: switch ON / OFF. ON writes EVERYTHING (every UDS frame with\n"
                                  "its whole payload, every command, service and IPC call) to\n"
                                  "AzaharLink.debug.log and the events to AzaharLink.log, both in\n"
                                  "sd:/3ds/3DSLink in blocks of 64 MB (up to 50 a log). OFF writes\n"
                                  "no file at all. Logging costs CPU and SD time: judge speed with\n"
                                  "it OFF (the numbers are under Status). For bugs send the newest\n"
                                  "files.");
    menu += g_debug_entry;
    menu += g_address_entry;
    menu += g_nickname_entry;
    menu += g_password_entry;
    menu += g_enabled_entry;
}

int main(void) {
    Plugin& plugin = Plugin::Get();
    plugin.LateInit();
    const Config& cfg = plugin.config();

    g_menu_off = plugin.menu_off();
    g_menu_keys = ParseKeys(cfg.menu_key) | Key::Select;

    PluginMenu* menu = new PluginMenu("AzaharLink", 1, 1, 0,
                                      "Plays 3DS local wireless games over the internet with\n"
                                      "Azahar emulator players and other 3DS consoles.\n\n"
                                      "1. Set the room address (and nickname).\n"
                                      "2. Switch on \"Connect to room\".\n"
                                      "3. Open the game's multiplayer menu as usual.");
    menu->SynchronizeWithFrame(true);
    menu->ShowWelcomeMessage(false);
    menu->OnOpening = OnMenuOpening;
    menu->OnClosing = OnMenuClosing;

    if (!plugin.active() && !plugin.startup_issue().empty()) {
        // The plugin switched itself off at start-up although the settings say ON: say why. The reason is the
        // text itself (on screen at the start and every minute, here, and under A), not a pointer to a log: with
        // debug=0 there is none.
        g_enabled_entry = new MenuEntry("AzaharLink is OFF: press A for the reason", nullptr, ShowIssue,
                                        Wrapped(plugin.startup_issue(), 50) + "\n"
                                        "The plugin stays out of this game (the real local wireless\n"
                                        "works)." + (azl::g_debug_level > 0 ? "" : " With debug=1 the log has the details."));
        *menu += g_enabled_entry;
        menu->Callback(OnFrameOff);
        menu->Run();
        plugin.Shutdown();
        return 0;
    }
    if (!plugin.active()) {
        // Switched off: no hooks, no network. The menu only offers the way back.
        g_enabled_entry = new MenuEntry("AzaharLink is OFF - press A to turn it on", nullptr,
                                        [](MenuEntry*) {
                                            Config& c = Plugin::Get().config();
                                            c.enabled = true;
                                            c.tunnel_enabled = true;
                                            Plugin::Get().SaveConfig();
                                            OSD::Notify("AzaharLink will be ON at the next game start");
                                        },
                                        "Takes effect the next time a game starts.");
        *menu += g_enabled_entry;
        menu->Callback(OnFrameOff);
        menu->Run();
        plugin.Shutdown();
        return 0;
    }

    InitMenu(*menu);
    RefreshLabels();

    g_radio_level = cfg.radio_like > 0 ? cfg.radio_like : 1;
    g_last_radio = cfg.radio_like > 0;
    if (cfg.radio_like > 0) {
        g_radio_entry->Enable();
    }
    // Joining by itself is the only way a room is ever joined when the menu is off, so then it does not depend on
    // autoconnect (nobody could switch "Connect to room" on).
    if ((cfg.autoconnect || g_menu_off) && !cfg.host.empty()) {
        g_connect_entry->Enable();
    }

    menu->Callback(OnFrame);
    menu->Run();

    // The game is closing. The framework waits about one second for main() to return: stop the
    // network, put the game's code back and write the log, all with deadlines.
    plugin.Shutdown();

    // The menu is deliberately not deleted: this runs while the game is closing and tearing the
    // framework's menu tree down at that point crashed in free() on real hardware (the root
    // folder was already gone). The process is about to exit, so there is nothing to give back.
    return 0;
}

} // namespace CTRPluginFramework
