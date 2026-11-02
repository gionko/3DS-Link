// In-game chat overlay (CTRPF): incoming lines on the top screen (in the corner / edge position chosen in the plugin menu) and a CHAT button
// on the bottom screen that opens a touch keyboard drawn over the game. The game is never paused: the framework calls our drawing function
// once per frame, and the touch is read from the same shared page the game uses. Built with -DAZL_CHAT_OVERLAY (the Makefile's default, CHAT=0 leaves it out; see
// Sources/ctr/chat_overlay.cpp); without it Attach/Detach do nothing and Enabled() is false.
#pragma once

namespace azl {
class RoomClient;
}

namespace chat_overlay {

/// True when the overlay is compiled in (the menu offers the position switch only then).
bool Enabled();

/// Where the chat lines go on the top screen: azl::chat::kChatPositions values (0 top-left ... 5 bottom-right). Safe to call at any time.
void SetPosition(int position);
int Position();
/// Where the small Chat button is on the bottom screen (0..5, the same six places): choose an empty spot of the game's touch screen.
void SetButtonPosition(int position);
int ButtonPosition();

/// Starts showing the chat of `room` and lets the keyboard send into it. Call after the room client exists, before Start().
void Attach(azl::RoomClient& room);
/// Stops sending (the room client is going away); the overlay keeps drawing the lines already received until they expire.
void Detach();

} // namespace chat_overlay
