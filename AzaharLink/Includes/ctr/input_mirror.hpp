// The input mirror page (Sources/ctr/input_mirror.cpp; the pure parts in azl/hid_mirror.hpp): while the chat's keyboard is open over the
// running game, the game reads a neutral copy of its HID page (no key, no Circle Pad, no touch; the clocks, the accelerometer and the
// gyroscope live) at its own address, and the plugin keeps reading the real one through the framework's mapping. Nothing of it exists
// until the keyboard first opens; the page is the plugin's heap's (4 KB) only while it is in use.
#pragma once

#include <3ds.h>

namespace azl {
namespace ctr {

/// The plugin's thread, in the chat's frame right BEFORE its look at the chat's signals: the closings counted so far. Every event and
/// notification counts its signal before its closing (ChatProcessEvent, hook.cpp's NoteAptForChat), so a closing in this count is one the
/// look sees; one that comes later (SWAP_ENTER between the look and the switch) cancels this frame's switch under the lock.
void MirrorBeforeLook();

/// The plugin's thread: the keyboard opened. The switch is made at the next MirrorFrame, if no closing came after MirrorBeforeLook.
void MirrorWant();

/// The plugin's thread: the keyboard closed by the user (Cancel, Send). The game gets its page back once the keys are let go (50 ms with
/// nothing held, at most 1 s).
void MirrorRelease(u32 now_ms);

/// The plugin's thread: the page back now (the chat off, the room left, a close a notification or an event asked for).
void MirrorRestoreNow(const char* why);

/// The plugin's thread, right before the in-game menu's Process::Pause: the page back first, waiting for the lock at most 100 ms (a game's
/// thread or the keep-alive giving it back). False: the mirror is still there, the menu is not opened (the chord is pressed again). The
/// framework's pause counts up before its hold and down before its let-go, so a way back of the keep-alive amid them read its state wrong: with
/// the page given back before the pause, and no switch while the menu is open, there is none.
bool MirrorBeforePause();

/// The plugin's thread, once a frame after the chat's work: the switch, the mirror's live parts, the wait for the release, a way back
/// another thread could not finish, and the log's lines of what the other threads did.
void MirrorFrame(u32 now_ms);

/// The framework's keep-alive thread, in the chat's look at the process events right after its signals, before any of the plugin's work
/// (Plugin::NoteProcessEvent takes the hooks out and closes the swap's gate after it): HOME_ENTER, SWAP_ENTER, SLEEP_ENTER and EXIT give the
/// page back synchronously, waiting for the lock at most 1 s while the plugin's thread stands aside. At SWAP_ENTER and EXIT, the game's last
/// events, the mirror is taken off with the lock, never kept as a fallback: its memory goes with the plugin's block, and an alias left
/// mapped outlives the game (console 63, 2026-11-06). Nothing is changed without the lock: a lock not had in 1 s - its holder no longer
/// runs - leaves the way back to the plugin's thread (want_restore), and at the game's last event the mirror then stays mapped, said in the
/// shutdown's line (MirrorLastState). That line comes only after an EXIT: on an Old 3DS a game closed from the HOME menu gets one (Luma3DS
/// maps the plugin's block back first) and the line says what EXIT found; SWAP_ENTER is the net for an end with no EXIT event (a process
/// killed, a block that does not come back), whose state is then not logged.
void MirrorProcessEvent(int event);

/// What became of the mirror at the game's last event (EXIT, or SWAP_ENTER), for the shutdown's line, which only an EXIT writes.
enum MirrorLast : u32 {
    kMirrorLastNone = 0,  ///< no mirror then
    kMirrorLastGivenBack, ///< taken off with the lock
    kMirrorLastStays,     ///< still mapped, the lock had: folded with a block of the game, or not unmapped (a page of the plugin's memory outlives the game)
    kMirrorLastNoLock,    ///< still mapped: the lock not had in 1 s, nothing touched (a page of the plugin's memory outlives the game)
    kMirrorLastEmpty,     ///< taken off, the real page not mapped back (the game's address empty: the game is ending)
};
u32 MirrorLastState();
const char* MirrorLastText(u32 state);

/// A game's thread, in the hook (NoteAptForChat), for a notification that closes the keyboard: the page back before the game acts on it
/// (an SDK that finalizes HID unmaps the page, and the mirror at its address would make that fail). A try of the lock only: when the
/// plugin's thread holds it, a flag, and the plugin's thread gives the page back itself.
void MirrorRestoreFromGame();

} // namespace ctr
} // namespace azl
