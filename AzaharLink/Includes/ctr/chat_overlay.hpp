// The chat over the running game (Sources/ctr/chat_overlay.cpp; the pure parts in azl/chat_host.hpp): the room's messages on the top screen
// and, with the chat's chord (chat_key, L + Left + Select by default), the plugin's keyboard (azl/osk.hpp) on the bottom one. Nothing of it
// runs while the chat is off: the framework's OSD callback is registered the first time the chat is on (and then returns at once while it is
// off), the keyboard and its picture exist only while it is open.
#pragma once

#include <3ds.h>

namespace azl {
namespace ctr {

/// The plugin's thread, once a game frame (OnFrame), after the in-game menu's look at the pad: the chat's switch and corner for the game's
/// thread, the chord, the keyboard and its picture, the rules that close it. `pad_read`: the pad was read this frame already
/// (Controller::Update, by PollGameMenu); false when the menu is off.
void ChatFrame(u32 chord, bool pad_read);

/// True while the chat's keyboard is open (the in-game menu's chord does not open the menu then).
bool ChatKeyboardOpen();

/// A notification of the plugin's thread, drawn by the chat's OSD callback on the top screen in the corner opposite the chat (never over
/// it). False when the callback cannot show it (the chat off or never on, no top screen drawn on within chat::kTopSeenMs): the caller then
/// hands it to OSD::Notify, and ChatNoteNotify.
bool ChatNotice(const char* text);

/// Next to every OSD::Notify of the plugin's thread: the bottom-right corner of the chat climbs above the notifications on screen.
void ChatNoteNotify();

/// The framework's process events, on its keep-alive thread, BEFORE Plugin::NoteProcessEvent: only counted (HOME_ENTER, SWAP_ENTER, SLEEP_ENTER
/// and EXIT close the keyboard at the next frame; between SWAP_ENTER and SWAP_EXIT it does not open).
void ChatProcessEvent(int event);

} // namespace ctr
} // namespace azl
