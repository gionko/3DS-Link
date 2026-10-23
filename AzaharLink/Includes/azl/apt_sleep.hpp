// The sleep protocol of the system as the game's APT session carries it. The plugin only LOOKS at the game's
// calls (never alters, delays or answers one): the game polls InquireNotification for what the system wants of
// it, and the number that comes back says where the sleep is. libctru's APT_Signal:
//   3 sleep query (the apps may refuse), 4 sleep cancelled, 5 sleep begins (no way back), 6 wake-up.
// Only 5 and 6 are acted on: a query can still be refused or cancelled, as the console itself does.
#pragma once

#include "azl/common.hpp"

namespace azl {

constexpr u32 kAptInquireNotificationHeader = 0x000B0040; // APT:InquireNotification(app id)
constexpr u32 kAptSignalSleepEnter = 5;
constexpr u32 kAptSignalWakeup = 6;

/// What the observer of the whole sleep protocol is told (ctr/hook.hpp, SetAptObserver; azl/sleep_crumbs.hpp keeps the
/// trail): a notification that arrived (the value is its number), the game's answer to the sleep query (0 reject, 1 accept,
/// 2 later), and the game's acknowledgement of a sleep (told BEFORE the call is made: it may not come back).
constexpr u32 kAptNoteNotification = 1;
constexpr u32 kAptNoteReplyQuery = 2;
constexpr u32 kAptNoteReplyComplete = 3;

enum class AptSleepEvent { None, Enter, Wake };

/// The APT signal of InquireNotification's reply word: libctru's APT_Signal values are small numbers, and some games get
/// bits above them (0x1BC00001 for the HOME button in Mario Party: The Top 100): only the low 16 bits are the signal.
inline u32 AptSignalOf(u32 reply_word) {
    return reply_word & 0xFFFFu;
}

/// What a finished APT call means for the sleep protocol. `request_header` is the header the game sent,
/// `reply` the command buffer after the call: [1] the result, [2] the notification (0 = none).
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
inline AptSleepEvent AptSleepEventOf(u32 request_header, const u32* reply) {
    if (request_header != kAptInquireNotificationHeader || static_cast<s32>(reply[1]) < 0) {
        return AptSleepEvent::None;
    }
    // The signal is the low 16 bits: Mario Party: The Top 100 gets 0x1BC00001 for the HOME button (hardware, 2026-10-14).
    const u32 signal = AptSignalOf(reply[2]);
    if (signal == kAptSignalSleepEnter) {
        return AptSleepEvent::Enter;
    }
    return signal == kAptSignalWakeup ? AptSleepEvent::Wake : AptSleepEvent::None;
}

/// Whether a sleep that begins now is acted on (the game's session and the room link end, like a console's): never with
/// sleep_stop 0, always with 2, and with 1 unless the console hosts - the one host that went to sleep this way froze (PROVA 1),
/// while one that did nothing kept its lobby through a lid of 2 s (PROVA 2). `hosting` is only looked at for 1.
inline bool SleepActsFor(int sleep_stop, bool hosting) {
    return sleep_stop != 0 && (sleep_stop >= 2 || !hosting);
}
// <<< game-thread code

} // namespace azl
