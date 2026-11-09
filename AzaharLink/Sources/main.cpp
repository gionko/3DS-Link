// AzaharLink - tunnels a 3DS game's local wireless (nwm::UDS) through an Azahar multiplayer room.
#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cctype>
#include <cstdlib>
#include <string>

#include "azl/ini_values.hpp"
#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "ctr/chat_overlay.hpp"
#include "ctr/game_menu.hpp"
#include "ctr/hook.hpp"
#include "ctr/swap_gate.hpp"
#include "ctr/plugin.hpp"

// libctru's hidInit() also opens the ir:rst service on a New 3DS (C-stick, ZL, ZR), and the framework calls it inside the
// game's own process. ir:rst only has two sessions (Rosalina's menu redefines this for the same reason), and opening it turns
// off the Circle Pad Pro the New 3DS emulates on ir:USER, where Monster Hunter 4 Ultimate, Monster Hunter 3 Ultimate and Tri
// Force Heroes read their C-stick and ZR (the logs show them opening ir:USER, never ir:rst): dead C-stick whenever the plugin
// was in the game's folder, even with the main switch off. The plugin needs neither the C-stick nor ZL/ZR.
extern "C" bool hidShouldUseIrrst(void) {
    return false;
}

namespace CTRPluginFramework {

using azl::ctr::Config;
using azl::ctr::Plugin;

static bool g_want_connected = false; // "be in the room": the menu leaves, joins and changes the room through it
static u32 g_menu_keys = Key::Select;
static bool g_menu_off = false; // menu_key=off: the menu never opens
static u32 g_chat_keys = 0;     // the chord of the chat's keyboard (chat_key; Sources/ctr/chat_overlay.cpp)

// ---------------------------------------------------------------------------------------------
// Menu hotkey
// ---------------------------------------------------------------------------------------------

// The menu is the plugin's own (Sources/game_menu.cpp): the framework's menu, which opens on Select with its panel of cheat tools,
// is never let open (OnMenuOpening), and the chord of the settings (default L+Right+Select) opens ours from the frame callback.

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
    return false; // (the framework's menu never opens: ours does, see OnFrame)
}

// ---------------------------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------------------------

// The plugin's work of a frame: the room link (join, keep, rejoin) and the messages of the worker threads. The menu keeps calling
// it while it is open (the game is paused, the room is not).
static void PluginWork() {
    Plugin& plugin = Plugin::Get();
    for (const std::string& message : plugin.TakeNotifications()) {
        // (in the corner opposite the chat when its callback draws the top screen: never over it; else the framework's, at the bottom right)
        if (!azl::ctr::ChatNotice(message.c_str())) {
            OSD::Notify(message);
            azl::ctr::ChatNoteNotify(); // (the chat's bottom-right corner climbs above the notifications on screen)
        }
    }
    if (!plugin.Tick(g_want_connected)) {
        g_want_connected = false; // fatal error (wrong password, nickname taken, ...) or nothing to connect to
    }
}

// The same work while the menu is open, but the messages wait in the plugin's queue: they are shown when the game runs again (and the
// menu drops the ones that are old news, the "Joined room" of a room that was left before the menu closed).
static void PluginWorkInMenu() {
    if (!Plugin::Get().Tick(g_want_connected)) {
        g_want_connected = false;
    }
}

static void OnFrame() {
    // Frame statistics (the callback runs once per game frame, synchronised with the swap): the interval
    // between two calls is the frame time as the plugin sees it, and the callback itself is timed.
    static u64 last_start = 0;
    u64 start = svcGetSystemTick();
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
    PluginWork();
    if (!g_menu_off) {
        azl::ctr::GameMenuHooks hooks;
        hooks.tick = PluginWorkInMenu;
        hooks.want_connected = &g_want_connected;
        // (the menu's chord does not open the menu while the chat's keyboard is open: its Select is the keyboard's Cancel)
        const u64 menu_from = svcGetSystemTick();
        if (azl::ctr::PollGameMenu(g_menu_keys, true, hooks, !azl::ctr::ChatKeyboardOpen())) {
            // The menu ran inside this callback with the game paused: its time is no work of the callback (a menu left open for
            // minutes went into one 5 s window of the stats as tens of thousands of ms per second), and the next interval is no frame.
            start += svcGetSystemTick() - menu_from;
            last_start = 0;
        }
    }
    // The chat over the game, after the menu's look at the pad (it reads the pad itself when the menu is off). Nothing while the chat is off.
    azl::ctr::ChatFrame(g_chat_keys, !g_menu_off);
    azl::StatsAdd64(azl::g_stats.t_cb, svcGetSystemTick() - start);
}

static void OnFrameOff() {
    Plugin::Get().RepeatStartupIssue();
    for (const std::string& message : Plugin::Get().TakeNotifications()) {
        OSD::Notify(message);
    }
    if (!g_menu_off) {
        // (it says why the plugin is off, and switches it on for the next start)
        azl::ctr::PollGameMenu(g_menu_keys, false, azl::ctr::GameMenuHooks());
    }
}

static void OnProcessEvent(Process::Event event) {
    // This runs on the framework's small keep-alive thread: flag stores only, and the swap of the plugin's memory (Old 3DS), which
    // takes the hook out of the game before Luma3DS frees the block. Sleep, home and swap are counted (the log writer reports
    // them); the exit starts the shutdown. The chat counts the events first (HOME, swap, sleep and exit close its keyboard: counters only).
    if (event == Process::Event::SWAP_ENTER) {
        // (Old 3DS: one deadline for every wait of SWAP_ENTER, counted from Luma3DS's event: ctr/swap_gate.hpp)
        azl::ctr::BeginSwapDeadline(azl::ctr::LastAptReceiveTick());
    }
    azl::ctr::ChatProcessEvent(static_cast<int>(event));
    Plugin::Get().NoteProcessEvent(static_cast<int>(event));
    if (event == Process::Event::EXIT) {
        Plugin::Get().RequestStop();
    }
}

// ---------------------------------------------------------------------------------------------
// Framework entry points
// ---------------------------------------------------------------------------------------------

// Called before main and before the game starts: the one safe moment to patch the game's code.
void PatchProcess(FwkSettings& settings) {
    // libctrpf 0.8.0 has no OnProcessExit() hook: the exit event is the only notification. It runs
    // on the framework's tiny keep-alive thread, so it only sets flags (see Plugin::RequestStop);
    // the orderly shutdown happens at the end of main().
    settings.AllowActionReplay = false; // (the framework's cheat codes and memory search are not 3DS Link's: off)
    settings.AllowSearchEngine = false;
    // The plugin is made HERE, on this one thread, before the event callback (which calls Plugin::Get()) can run: two threads in
    // the guard of the function-local static at once would abort, the plugin being built without exceptions.
    Plugin& plugin = Plugin::Get();
    Process::SetProcessEventCallback(OnProcessEvent);
    plugin.EarlyInit();
}

int main(void) {
    Plugin& plugin = Plugin::Get();
    plugin.LateInit();
    const Config& cfg = plugin.config();

    g_menu_off = plugin.menu_off();
    g_menu_keys = ParseKeys(cfg.menu_key) | Key::Select;
    // Select is in the chat's chord as in the menu's (an ini written by hand with chat_key=L+R would take the game's own keys). A chord of
    // Select alone, or one that holds the menu's or is held by it (holding the bigger completes the smaller on the way: the menu, looked at
    // first, would open instead of the chat), takes the default; if the menu's still clashes with it, the menu takes its default too.
    g_chat_keys = ParseKeys(cfg.chat_key) | Key::Select;
    const auto clash = [](u32 a, u32 b) { return !g_menu_off && ((a & b) == a || (a & b) == b); };
    if (g_chat_keys == Key::Select || clash(g_chat_keys, g_menu_keys)) {
        g_chat_keys = ParseKeys(azl::ini::kDefaultChatKey) | Key::Select;
    }
    if (clash(g_chat_keys, g_menu_keys)) {
        g_menu_keys = ParseKeys(azl::ini::kDefaultMenuKey) | Key::Select;
    }

    // The framework's windows in the colours of 3DS Link (ConfigApp/source/gfx.hpp). The password of a room is typed on the plugin's own
    // keyboard (azl/osk.hpp: the system's keyboard is an applet the game's APT session would have to start, which a plugin cannot do).
    FwkSettings& look = FwkSettings::Get();
    look.MainTextColor = Color(232, 234, 238);
    look.WindowTitleColor = Color(70, 178, 255);
    look.BackgroundMainColor = Color(13, 14, 17);
    look.BackgroundSecondaryColor = Color(24, 26, 31);
    look.BackgroundBorderColor = Color(46, 50, 58);

    // The framework's menu is only the loop that calls OnFrame once a game frame: it has no entry and never opens.
    PluginMenu* menu = new PluginMenu("AzaharLink", 1, 1, 3, "");
    menu->SynchronizeWithFrame(true);
    menu->ShowWelcomeMessage(false);
    menu->SetHexEditorState(false);
    menu->OnOpening = OnMenuOpening;

    if (!plugin.active()) {
        // Switched off (by the settings, or by itself at start-up: the reason is on screen at the start, every minute, and in
        // the menu): no hooks, no network.
        menu->Callback(OnFrameOff);
        menu->Run();
        plugin.Shutdown();
        return 0;
    }

    // Joining by itself is the only way a room is ever joined when the menu is off, so then it does not depend on
    // autoconnect (nobody could ask for it).
    g_want_connected = (cfg.autoconnect || g_menu_off) && !cfg.host.empty();

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
