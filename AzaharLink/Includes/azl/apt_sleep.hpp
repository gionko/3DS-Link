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
/// trail): a notification that arrived (the value is the reply word as it came), the game's answer to the sleep query (0 reject, 1 accept,
/// 2 later), and the game's acknowledgement of a sleep (told BEFORE the call is made: it may not come back).
constexpr u32 kAptNoteNotification = 1;
constexpr u32 kAptNoteReplyQuery = 2;
constexpr u32 kAptNoteReplyComplete = 3;
// (4 to 7 are the framework's, azl/sleep_crumbs.hpp. 8 to 10 were who made the call that raises the swap on an Old 3DS, up to 1.1.2: free since the
// persistent gate, which makes every real svc of the game itself.)
// A call of the game that failed for good, told as one line of words (kAptNoteWord ..., kAptNoteEnd): an invalid handle (the value: the handle), or the fatal error the game reports to err:f (the value: the session).
constexpr u32 kAptNoteBadHandle = 11;
constexpr u32 kAptNoteFatal = 12;
constexpr u32 kAptNoteWord = 13;
constexpr u32 kAptNoteEnd = 14;
// (15 to 17 are the memory regions, azl/sleep_crumbs.hpp.) Two calls of the transition, as they came back:
constexpr u32 kAptNoteReceiveReply = 18;   ///< APT:ReceiveParameter(application): sender << 16 | command, or kAptReceiveFailed
constexpr u32 kAptNoteUtility = 19;        ///< APT:AppletUtility: the first 16 bits of the input << 16 | the id; its result comes next
constexpr u32 kAptNoteUtilityResult = 20;
constexpr u32 kAptReceiveFailed = 0xFFFFFFFF;
constexpr u32 kAptNoteLimit = 21;          ///< the lines of these two calls reached their limit (once)
// (22 to 25 are the framework's, azl/sleep_crumbs.hpp.) The close of the game, with the log on (REGISTRO, phase N: a game closed from the HOME menu that left
// Luma3DS's plugin block behind, with nothing in the logs after the close command):
constexpr u32 kAptNoteReceiveWord = 26;    ///< after APT:ReceiveParameter(application): Luma3DS's plugin word (kLumaPluginWordAddress), a line of its own
constexpr u32 kAptNoteSrvNotification = 27; ///< srv:ReceiveNotification gave the game the termination request (the value: the id, 0x100)
constexpr u32 kAptNoteCloseLimit = 28;     ///< the lines after a close command reached their own limit (once)

/// Luma3DS's word of the plugin loader in the configuration page (read-only in every process, like 0x1FF80040): the state in the low 16 bits (0 none,
/// 1 running, 2 in the HOME menu, 3 exiting), the events Luma3DS raised and has not handled yet in the high 16 (1 HOME, 2 EXIT; rosalina globals.h).
constexpr u32 kLumaPluginWordAddress = 0x1FF800F0;

/// The command of APT:ReceiveParameter that closes the application: libctru's APTCMD_WAKEUP_CANCEL 12, the HOME menu's Close. 10 (APTCMD_WAKEUP_EXIT,
/// a library applet that exited) and 11 (APTCMD_WAKEUP_PAUSE, the return from the HOME menu: right after every HOME button in the real trails, with the
/// game playing on) close nothing: taken for closes, a plain HOME round trip spent the lines of the close before the close came.
constexpr u32 kAptCommandCloseFromHome = 12;
inline bool AptCommandCloses(u32 command) {
    return command == kAptCommandCloseFromHome;
}

/// The lines of APT:ReceiveParameter and APT:AppletUtility of a launch: kNormal, then the limit's line; once a close command came, kAfterClose more of
/// their own (a game that polls these used the 64 up before it was closed, and the lines after the close are the ones a lost close needs). Take says
/// what to write: the line (kNoteWrite), and before it the line of a limit reached (kNoteLimit, kNoteCloseLimit: once each).
struct AptNoteBudget {
    static constexpr u32 kNormal = 64;
    static constexpr u32 kAfterClose = 32;
    static constexpr u32 kNoteWrite = 1;
    static constexpr u32 kNoteLimit = 2;
    static constexpr u32 kNoteCloseLimit = 4;
    u32 normal = 0;
    u32 after_close = 0;
    bool closing = false;

    // >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
    /// `close`: this line is a ReceiveParameter that brought a close command (it is written, and so are the next kAfterClose).
    u32 Take(bool close) {
        if (close) {
            closing = true;
        }
        if (normal < kNormal) {
            ++normal;
            return kNoteWrite;
        }
        u32 out = 0;
        if (normal == kNormal) {
            ++normal;
            out |= kNoteLimit;
        }
        if (!closing) {
            return out;
        }
        if (after_close < kAfterClose) {
            ++after_close;
            return out | kNoteWrite;
        }
        if (after_close == kAfterClose) {
            ++after_close;
            out |= kNoteCloseLimit;
        }
        return out;
    }
    // <<< game-thread code
};

enum class AptSleepEvent { None, Enter, Wake };

/// The APT signal of InquireNotification's reply word. Only the low byte is the signal: the bytes above it change from call to call: 0x1BC00001, 0x00000601 and 0x0007E901 for the HOME button, 0x18000008 and 0x00000608 for the power button (all from real consoles).
/// With 16 bits, 0x608 was no power button, and the game's thread woke up in the plugin's freed memory (Old 3DS, issue 5, Pokemon Alpha Sapphire, 1.1.0).
inline u32 AptSignalOf(u32 reply_word) {
    return reply_word & 0xFFu;
}

/// The signals after which the game goes to the HOME menu's side of the system: the HOME button (1 and 2) and the power button (8 and 9, about 0.6 s apart on a
/// console: its dialog is the HOME menu's). On an Old 3DS the swap of the plugin's memory follows either of them, at the end of the game's next APT:ReceiveParameter.
inline bool AptSignalPrecedesSwap(u32 signal) {
    return signal == 1 || signal == 2 || signal == 8 || signal == 9;
}

/// What a finished APT call means for the sleep protocol. `request_header` is the header the game sent,
/// `reply` the command buffer after the call: [1] the result, [2] the notification (0 = none).
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
inline AptSleepEvent AptSleepEventOf(u32 request_header, const u32* reply) {
    if (request_header != kAptInquireNotificationHeader || static_cast<s32>(reply[1]) < 0) {
        return AptSleepEvent::None;
    }
    // The signal is the low byte: see AptSignalOf.
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
