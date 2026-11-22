// Which IPC calls of the game go to the console's file system (2026-11-11): what the svc hook counts, with the log on only, so that the
// log's writer keeps the capture off the SD card while the game is at its files (Sources/ctr/hook.cpp, PassThroughPre / PassThroughPost;
// azl/capture_pace.hpp). Pure: the header word of the request and nothing else, so that Tests/game_card_calls_test.cpp runs it on the PC.
//
// Why every call and not the reads alone (FSFILE_Read, 0x080200C2, was the whole rule on 2026-11-10): the console has ONE file-system
// service, and ANY call of the game waits behind the write the writer has in it. Tekken 3D Prime Edition on a real console, 1.1.3 with
// the log on, 2026-11-11: at the start of a match the game's fs:USER OpenFileDirectly (0x08030204) came back after 26.4 s - 1.0 s in the
// session that held - with the writer in the card 991 ms of every second; the game sent nothing for 17 s and the host closed the session.
// The call left waiting was an OPEN: a count of the reads did not see it.
//
// What the count does NOT do (2026-11-12, review of the captures): it would not have seen THAT open in time either. The open began at
// 190.332 s, 5.7 s after the writer went into the card (184.617) and 5.9 s after the game's last file call (184.383): all that while
// the card was free of the game and the writer was right to begin. The six slow opens of that launch are all the FIRST file call after
// 3 s or more of silence. The count keeps the writer from BEGINNING a write while a call of the game is under way or just ended -
// the calls that follow the first one of a load, and one call alone for pace::kReadStuckMs at the most (then it is taken for an end
// that was lost) - and it does not shorten a write the writer already has in hand: the first call after a silence waits for that
// write, and only the short pass of azl/capture_pace.hpp (one write a pass, 8 KB once a call was slow) bounds it. The main log does not
// go through this gate at all. So at the next test on a console the log's card line ("stats: card:") is what tells the two apart:
// "longest ... ms for ... B" of the writes and "longest ... ms" of the flushes, of the capture and of the main log. Under 2-3 s with
// the slice at 8 KB, the short pass is enough. ONE call of tens of seconds, and no gate can help: something else is needed (the
// file's room taken beforehand, the main log through the gate too).
//
// The rule: the command id's high byte is 0x08. That is the whole command space of the file system as a game reaches it - fs:USER
// (0x0801 Initialize ... 0x0868 GetMediaType is where libctru's fs.c ends; the service goes on above that, and the captures have
// 0x086E and 0x086F), the file sessions it hands out (FSFILE 0x0801 OpenSubFile, 0x0802 Read, 0x0803 Write, 0x0804 GetSize, 0x0805
// SetSize, 0x0808 Close, 0x0809 Flush ... 0x080C) and the directory sessions (FSDIR 0x0801 Read, 0x0802 Close ... 0x0804) - and the
// same number is another command on another kind of session (0x0802 reads on a file session and opens a file on fs:USER), so a
// narrower rule by header cannot be told right from wrong. The few commands of the family that never touch the card (0x0862
// SetPriority, 0x0817 IsSdmcDetected) are counted too. ONE such call costs the capture one look (pace::kReadPollMs), never the game;
// a game that makes them on end costs the capture EVERY look for as long as it does, down to the forced slices (8 KB every
// pace::kSkipForceMs). Games do: Ridge Racer 3D asks IsSdmcDetected some 30 times a second for 25 s at its start (726 calls in the
// capture of 2026-11-10), Dragon Ball Z Extreme Butoden asks it and IsSdmcWritable some 50 times a second each for 4 s. There the
// capture is late, not lost (the 225 KB those 25 s produced fit in the ring); a game that did it for a whole match would leave the
// capture its forced slices and little else. They stay counted all the same: whether such a call waits behind the writer's write
// is not known. In the card line it reads as "skipped" high and "passes" close to "forced small".
//
// Other services have commands 0x08xx - cfg:i, am:net, ptm:sysm and qtm:sp in libctru - and no
// game of our captures opens one of them (the srv lines of every debug log we have: APT, cfg:u, ir:USER, dsp, hid, gsp, fs:USER,
// cecd:u, frd:u, ptm:u, boss:U, y2r:u, ldr:ro, nim:aoc, am:app, soc:U, ssl:C, nfc:u, act:u, ac:u, dlp, http:C, news:u, mic:u, cam:u);
// were one to come through the hook, the capture would step aside for it as for a file call, within pace::kSkipForceMs like
// everything else. Nothing here exists with the log off: the hook returns before it asks.
#pragma once

#include "azl/common.hpp"

namespace azl {

/// The high byte of the command id of every fs:USER, FSFILE and FSDIR command (the header word: id << 16 | sizes).
constexpr u32 kFsCommandFamily = 0x08;

// >>> game-thread code (Tests/policy_check.sh: on every IPC call of the game with the log on, the 464-byte thread of Tri Force Heroes
// included: a shift and a compare, no call, no local, nothing on the stack)
/// Is `header` (cmdbuf[0] of a request) a command of the file system?
__attribute__((always_inline)) constexpr bool IsFileSystemCall(u32 header) {
    return (header >> 24) == kFsCommandFamily;
}
// <<< game-thread code

} // namespace azl
