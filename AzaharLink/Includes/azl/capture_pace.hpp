// The pace of the capture's writer against the SD card (2026-11-10): how much it writes in one call, how long it then leaves the card
// alone, and when it steps aside for a file read of the game. Pure state and arithmetic, no clock and no file of its own: the plugin's
// LogFiles (azl/log_files.hpp) feeds it, and Tests/log_files_test.cpp runs it on the PC.
//
// 2026-11-11: "a read of the game" below is ANY call of the game to the file system - an open, a write, a close, a size, a directory,
// an archive as well as a read (azl/game_card_calls.hpp says which calls the hook counts, and why: Tekken 3D Prime Edition, an open
// left waiting 26 s behind the writer). Nothing here changed for that: the two counts are the same two words, the names and the
// constants are those of the reads.
//
// Why: the console has ONE file-system service, and a file read of the game waits behind the call the writer has in it. With the log on
// and a capture the card could not keep up with (Kirby Battle Royale with four players: 77-87 KB/s offered, 13 KB/s taken by the user's
// card) the writer made two writes of 64 KB and a flush (about 9 s), rested 250 ms at the most and began again: the card was the
// writer's 97% of the time, the game read one block of its files per pass, the load of the match ran at 3% of its speed and the session
// fell. Not CPU and not memory: the card. Nothing here exists with the log off.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace pace {

/// One write of the capture: 64 KB while the card answers ...
constexpr u32 kSliceLargeBytes = 64 * 1024;
/// ... and 8 KB once a call of the capture (a write or a flush) took longer than kSlowCallMs, until kFastCallsToGrow calls in a row
/// were fast again: a read of the game waits for the call in hand, never for less, so on a slow card the call must be short.
constexpr u32 kSliceSmallBytes = 8 * 1024;
constexpr u32 kSlowCallMs = 250;
constexpr u32 kFastCallsToGrow = 8;
/// After a pass the capture stays off the card for as long as the pass kept it, within these bounds (until 2026-11-10 the upper one was
/// 250 ms, which a pass of 9 s made nothing of).
constexpr u32 kRestMinMs = 20;
constexpr u32 kRestMaxMs = 5000;
/// The writer's wake with the log on (the main log and the statistics are looked at this often whatever the capture does) ...
constexpr u32 kWakeMs = 250;
/// ... and how soon it looks again after it stepped aside for a read of the game.
constexpr u32 kReadPollMs = 50;
/// The game's reads come first, but not for ever: after this long of stepping aside in a row the capture writes ONE small slice.
constexpr u32 kSkipForceMs = 10000;
/// A read that began and whose end was never counted (the post of a call is skipped when an Old 3DS swaps the plugin's block out during
/// the call): after this long with reads "under way" and neither count moving, the count is put right. With other reads going on
/// around the lost end the counts never stand still: then the fewest seen open over this long is taken as lost, but ONLY when the
/// gate did skip a post since the count was last right (2026-11-11: without that proof a game that simply read for 3 s on end -
/// a long load, which is when the capture must keep off the card - was taken for a lost end, and the capture wrote in the middle
/// of every read longer than a look. On a New 3DS no post is ever skipped, so there the rule could only be wrong).
constexpr u32 kReadStuckMs = 3000;
/// The last resort, for an end lost with no skipped post to show for it (none is known): reads under way at EVERY look for this long.
constexpr u32 kReadLostMs = 300000;
/// "At every look" stands for "all the time" only while the looks are close together (the writer looks every kWakeMs at the most, unless
/// it is in the card or the console stopped it: the HOME menu of an Old 3DS, the lid): the time between two looks further apart than
/// this is no time of the fewest-open rule (2026-11-11: two looks five minutes apart, each with a read under way, were "reads under
/// way at every look for five minutes").
constexpr u32 kLookGapMs = 1000;

/// How long the capture leaves the card alone after a pass that kept it `card_ms`.
inline u32 RestAfterPass(u64 card_ms) {
    return card_ms < kRestMinMs ? kRestMinMs : card_ms > kRestMaxMs ? kRestMaxMs : static_cast<u32>(card_ms);
}

/// The share of a statistics window the writer spent in the card, for the notice on the screen: by the REAL length of the window (the
/// writer closes the window, and it is the thread that sits in the card: a window "of 5 s" lasted up to 43 s, and the notice said 868%).
inline u32 CardBusyPercent(u64 card_ms, u64 window_ms) {
    if (window_ms == 0) {
        return 0;
    }
    const u64 pct = card_ms * 100 / window_ms;
    return pct > 100 ? 100u : static_cast<u32>(pct);
}

/// The size of the next write of the capture (see kSliceLargeBytes).
struct SliceSize {
    bool small = false;
    u32 fast_in_a_row = 0;

    /// A call of the capture into the file layer (a write or a flush) took `ms`.
    void NoteCall(u64 ms) {
        if (ms > kSlowCallMs) {
            small = true;
            fast_in_a_row = 0;
        } else if (small && ++fast_in_a_row >= kFastCallsToGrow) {
            small = false;
            fast_in_a_row = 0;
        }
    }
    u32 Bytes() const {
        return small ? kSliceSmallBytes : kSliceLargeBytes;
    }
};

/// The game's calls to the file system ("reads": every fs command since 2026-11-11, the note at the top), as the svc hook counts them
/// with the log on (begun / ended: two words, atomic adds, nothing else on the game's thread), looked at by the writer every time it
/// wakes. The game is "at the card" while a call is under way or one ended since the writer's last look; a pass of the capture that
/// wanted the card then steps aside.
struct GameReadWatch {
    enum Verdict : u8 {
        kPass = 0,   ///< the card is free of the game: write
        kSkip = 1,   ///< the game is at the card: not now
        kForced = 2, ///< it has been at the card for kSkipForceMs of looks in a row: one small slice all the same
    };

    /// One look. `done` must have been read BEFORE `begun` (a read that begins and ends between the two loads then shows as under way,
    /// never as -1). `wanted`: the capture would write now if the game let it; a look that wants nothing only keeps the counts fresh.
    /// `posts_skipped`: the gate's count of the posts it did not call (Sources/ctr/gate.s, azl_gate_post_skipped: any call of the game
    /// that came back while the plugin's block was away, a file read or not); 0 where there is no gate. It must have been read BEFORE
    /// the two counts: a post skipped after it was read is seen at the next look, with counts that hold the read whose end it lost.
    /// Read after them, a read that began and lost its end between the loads showed its skipped post with counts that did not hold
    /// the read yet, and a look that found nothing under way wrote the post off as one that had lost no end.
    Verdict Look(u32 begun, u32 done, u64 now_ms, bool wanted, u32 posts_skipped = 0) {
        if (!looked) {
            quiet_skipped = posts_skipped; // (what was skipped before the first look is before the counts)
        } else if (open_run && now_ms > last_look_ms && now_ms - last_look_ms > kLookGapMs) {
            open_since_ms += now_ms - last_look_ms; // (nobody looked for that long: it is no time "at every look", kLookGapMs)
        }
        last_look_ms = now_ms;
        u32 open = begun - done;
        if (open < lost_posts) {
            lost_posts = open; // (an end thought lost came after all: a read that really lasted that long)
        }
        bool reading = open > lost_posts;
        if (reading) {
            if (!open_run || posts_skipped != last_skipped) {
                // (a post skipped just now: the fewest open is counted from here, not from what the reads were before it)
                open_run = true;
                open_since_ms = now_ms;
                open_min = open;
            } else if (open < open_min) {
                open_min = open;
            }
            if (!still_run || begun != still_begun || done != still_done) {
                still_run = true;
                still_since_ms = now_ms;
                still_begun = begun;
                still_done = done;
            }
            u32 lost = lost_posts;
            if (now_ms - still_since_ms >= kReadStuckMs) {
                lost = open; // the plain case: nothing began and nothing ended for kReadStuckMs - what is open is not coming
            } else if (now_ms - open_since_ms >= (posts_skipped != quiet_skipped ? kReadStuckMs : kReadLostMs)) {
                // Reads go on around a lost end (the count would otherwise stay one too many for good, and the capture would only
                // ever write its forced slices): the fewest seen open in that time is what never ended.
                // ... but never more ends than posts were skipped since the count was right (2026-11-11): the fewest seen open holds
                // the REAL reads of a load that goes on after the swap as well - the HOME button during a load, reads of 200 ms with
                // half a millisecond between them: 60 looks in 3 s hardly ever fall in a pause - and the count went one too far, so
                // that the capture wrote in the middle of that load. (A skipped post that was not of a read still passes for one
                // lost end here, if the game then reads on end for kReadStuckMs: the gate does not know whose post it skipped. It
                // comes back at the first look that sees fewer open.)
                lost = open_min;
                const u32 skipped = posts_skipped - quiet_skipped;
                if (skipped != 0 && lost - lost_posts > skipped) {
                    lost = lost_posts + skipped;
                }
            }
            if (lost != lost_posts) {
                lost_posts = lost;
                ++realigned;
                open_run = false;
                still_run = false;
                quiet_skipped = posts_skipped; // (the skipped post is accounted for: the next one is another)
                reading = open > lost_posts;
            }
        } else {
            open_run = false;
            still_run = false;
            quiet_skipped = posts_skipped; // (the count is right here: a post skipped before now lost no end of a read)
        }
        last_skipped = posts_skipped;
        const bool ended_since = looked && done != last_done;
        if (looked) {
            reads += done - last_done;
        }
        last_done = done;
        looked = true;
        if (!wanted) {
            return kPass; // (nothing was asked: the streak of skips is a streak of passes that WANTED the card)
        }
        if (!reading && !ended_since) {
            skipping = false;
            return kPass;
        }
        if (!skipping) {
            skipping = true;
            skip_since_ms = now_ms;
        }
        if (now_ms - skip_since_ms >= kSkipForceMs) {
            skipping = false; // (the next streak starts from nothing)
            return kForced;
        }
        return kSkip;
    }

    /// A pass was made without asking (the HOME button, the end of the launch): the streak of skips is over.
    void PassedAnyway() {
        skipping = false;
    }

    bool looked = false;
    u64 last_look_ms = 0;
    u32 last_done = 0;
    u32 lost_posts = 0; ///< reads counted as begun whose end will never be counted
    bool open_run = false; ///< reads under way at every look since open_since_ms (or since a post was skipped), open_min the fewest
    u64 open_since_ms = 0;
    u32 open_min = 0;
    bool still_run = false; ///< ... and neither count has moved since still_since_ms
    u64 still_since_ms = 0;
    u32 still_begun = 0;
    u32 still_done = 0;
    u32 quiet_skipped = 0; ///< the gate's count of skipped posts when the count of the reads was last known to be right
    u32 last_skipped = 0;  ///< ... and at the last look
    bool skipping = false;
    u64 skip_since_ms = 0;
    u32 reads = 0;     ///< reads of the game that ended, as the looks saw them (the writer takes and clears it for its line)
    u32 realigned = 0; ///< times the count was put right
};

} // namespace pace
} // namespace azl
