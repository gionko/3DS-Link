// The in-game menu of 3DS Link: the plugin's own, on both screens, in the colours of the app (it replaced the framework's menu and
// its panel of cheat tools). It opens with the chord of AzaharLink.ini (menu_key, L + Right + Select by default) and B closes it.
// The game is paused while it is open, as with the framework's menu and Rosalina's: the room link and the plugin's own work go on
// (`tick`). The top screen is the room 3DS Link is in (state, ping, name) and the menu: change the room (the saved rooms of the
// app, the public lobbies), leave or join it, the debug log. The bottom screen is the room: who is in it and what they play, or the
// room under the cursor. It takes no memory while it is closed; open, the list shown (a few KB) and the room's information.
#pragma once

#include <3ds.h>

namespace azl {
namespace ctr {

struct GameMenuHooks {
    void (*tick)() = nullptr;          ///< the plugin's per-frame work (the room link), kept going while the menu is open
    bool* want_connected = nullptr;    ///< "be in the room": the menu leaves, joins and changes the room through it
};

/// Called once per game frame: when Select was just pressed with the rest of `chord` held, the menu opens and this returns when B
/// has closed it (the game is paused meanwhile). `active`: false when the plugin stayed out of the game (it then only says why and
/// offers to switch it on for the next start). `may_open` false: the chord is followed (its edge) but opens nothing (the chat's keyboard
/// is open).
/// True when the menu was open in this call (the game paused inside it: the frame callback leaves that time out of its own).
bool PollGameMenu(u32 chord, bool active, const GameMenuHooks& hooks, bool may_open = true);

#ifdef AZL_MENU_TOUR
/// A test build only: both screens to sdmc:/azl-shots/plugin-NAME-*.bmp (Sources/game_menu_tour.cpp).
void TourShotScreens(const char* name);
#endif

} // namespace ctr
} // namespace azl
