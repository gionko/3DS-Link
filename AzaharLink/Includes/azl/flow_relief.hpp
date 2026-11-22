// The relief of a dense broadcast stream, for the titles listed here and for no other (1.1.4, 2026-11-14).
//
// Mario Party: Island Tour hosted by a console with an Azahar as its guest ended with a communication error at the first throw of the
// dice, five sessions of five (four with the log on, one with it off). In that phase the game sends a broadcast frame on its data
// channel every 5.5 ms - 180 a second from each side - with every block the other has not confirmed yet, 124 to 584 bytes, four in
// five of them a copy of the one before, byte for byte (the relay's capture of two Azahar, whole frames, 187 s of that phase: 79.9 and
// 79.5 % of the broadcast frames of channel 1 equal to the previous one in every byte; the console's captures, 32 bytes a frame: 78 to
// 83 %): the radio loses broadcast frames, so the game repeats them. Through a room nothing of that is free.
// Every frame of ours was a send to the soc service (3.5 to 8 ms on that console), the network thread was inside the service for 0.9 s
// of every second, and it handed the game ONE of the room's packets per round, each round a send long: the room's 180 packets a second
// came in at 90 to 160, the rest piled up in the socket until it overflowed (the model's reading: where the datagrams were lost was
// not measured), the room's reliable stream got holes and fell 2 s behind, the confirmations came late, the frames grew, and it wound
// itself up until the limit of waiting data closed for good (that dead end is ENet's and is removed for every game:
// Includes/enet/enet.h, ENetHost::azlCapRefused).
//
// Two things keep the room's stream from falling behind, both ONLY where the game's own protocol makes them safe, which is why they
// are behind a list of titles and not in the transport of every game:
//   - SameFrame (UdsEngine::SendTo): a broadcast data frame that is byte for byte the one last sent on that data channel, less than
//     kSameFrameMs ago, is not sent again. The game still gets "success", as it does for a frame the radio loses. This game's frames
//     are states, not events: every one carries all that is not confirmed yet, and one that differs in a single byte goes out at once.
//   - the hand-over burst (RoomClient::Run): up to kHandOverBurst of the room's packets that are ready go to the game in one round,
//     before the queue and the flush, instead of one. No soc call is added (enet_host_check_events makes none). A burst lands in the
//     bind's ring at once, and the ring keeps the newest frames when it is full - right for frames that are states.
// A game whose broadcast frames are events (a frame that must arrive as many times as it was sent), or that needs the pace of one
// packet per round, would be changed by either: no title is added without a capture of the game that shows what its frames are.
// The numbers are the model's (Tests/vsim/island_dice.sh: the game as a closed loop over the costs of that console; 5.8 to 8.2 ms a
// send were measured): with both, the game keeps its 30 steps a second and no frame of the room reaches it later than 0.5 s at 4 to
// 10 ms a send (1 s at 12). The burst alone gives way at 8 ms a send; the filter alone holds with half the margin. Neither takes the
// thread out of the soc service - it stays there for most of every second, because every datagram of the room's reliable stream has
// to be acknowledged within a few ms and there are 175 a second: what changes is that reading and handing over no longer wait for
// the sends. All of that is a party of TWO (the console and one other side): the relief works on what the console sends, and every
// other player adds 180 frames a second to the room's stream; in the model a party of three holds only up to about 6 ms a send and
// one of four does not hold at any cost measured. Never seen on a console.
//
// The titles are the rows of azl/title_rules.hpp that turn these on (one row per game: every region, the application and its Download Play
// child, so that it is the same whichever side the console is on), each with its own numbers. Island Tour's: 30 ms, 16 packets a round.
// Pure header: no engine, no room, no allocation. The statistics line is formatted on the statistics' thread.
#pragma once

#include <cstddef>
#include <cstdio>

#include "azl/common.hpp"
#include "azl/stats.hpp"
#include "azl/title_rules.hpp"

namespace azl {
namespace flow {

/// Does `title_id` run with any of the relief of a dense broadcast stream: its row of azl/title_rules.hpp turns on the same-frame filter,
/// the hand-over burst or the drop at ENet's limit (the application or its Download Play child, any region)?
constexpr bool IsDenseTitle(u64 title_id) {
    const title_rules::Rules rules = title_rules::RulesFor(title_id);
    return rules.same_frame_ms != 0 || rules.hand_over_burst > 1 || rules.cap_drop_max != 0 || rules.send_budget_bytes != 0;
}

// (The window of the same-frame filter, a row's same_frame_ms: Island Tour's 30 ms. A broadcast frame equal to the last one sent on its
// data channel is not sent again for that long. The game's step is 33 ms and a frame that differs goes at once, so the other side is
// told of every step when it happens and hears an unchanged state 30 times a second instead of 180.)
// (The hand-over burst, a row's hand_over_burst: Island Tour's 16. The room's packets handed to the game in one round of the network
// thread, the first included. What a read pass of ENet brings
// is 32 datagrams at the most (ENET_AZL_READ_MAX_DATAGRAMS): two rounds of 16 hand a full pass over, and the queue waits for 16 frames
// through the engine at the most (a few ms). The held acknowledgements (ENetHost::azlAckHoldEvents) do not see the burst: only the
// packets enet_host_service hands over count towards their limit of packets, those of enet_host_check_events do not, so for these
// titles their limit is the time one (azlAckHoldMs), looked at in the next service call, after the burst - and "events before ack" of
// the statistics counts the first packet of a round only.)
/// The limit of waiting data the room client gives ENet (Sources/azl/room_client.cpp holds it to this), for the statistics line.
inline constexpr u32 kWaitingLimitBytes = 192 * 1024;
/// ENet's ENET_AZL_AWAY_MS, for the line of the losses (Sources/azl/room_client.cpp holds it to this).
inline constexpr u32 kAwayMs = 100;

/// (2026-11-15, N3 of the plan) After the network thread was away, a title with a send budget (azl/title_rules.hpp) reads what waited in the socket's
/// ring before it sends its own queue again: the acknowledgements still go (the service call's), the game's frames wait - for this long at the most
/// from the first round that found the ring's backlog (ENetHost::azlAwayBacklog), so a room that never lets the socket empty cannot hold them for good.
/// 50 ms: two read passes of ENet and more (a 64 KB ring of datagrams of the MTU is about 46 of them) in the rounds that come at once.
inline constexpr u32 kAwayDrainMaxMs = 50;
/// The start of the ring's backlog, kept round by round (`since`: 0 = none): set at the first round that finds it, voided at the first that does not.
inline void NoteAwayBacklog(bool backlog, u64 now_ms, u64* since) {
    if (!backlog) {
        *since = 0;
    } else if (*since == 0) {
        *since = now_ms != 0 ? now_ms : 1;
    }
}
/// Does this round hold the game's queue back for the ring? Only within kAwayDrainMaxMs of the start of the backlog.
inline bool HoldsForTheRing(u64 since, u64 now_ms) {
    return since != 0 && now_ms >= since && now_ms - since < kAwayDrainMaxMs;
}

/// Does the round go on handing over (`handed` packets went, `burst` is the round's share, `alive`: the session is still this thread's)?
constexpr bool MoreThisRound(u32 handed, u32 burst, bool alive) {
    return alive && handed < burst;
}

/// What the filter keeps of a frame's bytes: two digests made in one pass (FNV-1a, and a rotating sum that weighs the position). A
/// different frame that has both equal, and the same length, in the same 30 ms is not going to happen - and would cost one frame
/// the game repeats anyway.
struct FrameDigest {
    u32 hash = 0;
    u32 mix = 0;
};
inline FrameDigest DigestOf(const u8* data, u32 size) {
    FrameDigest d;
    d.hash = 2166136261u;
    d.mix = size;
    for (u32 i = 0; i < size; ++i) {
        d.hash = (d.hash ^ data[i]) * 16777619u;
        d.mix = ((d.mix << 5) | (d.mix >> 27)) + data[i] + i;
    }
    return d;
}

/// The last frame sent on each data channel (two channels: this game has two binds), as its digests, its length and when it went.
/// Not thread safe: the engine calls it under its lock, with the digests made before the lock was taken. 32 bytes.
class SameFrame {
public:
    /// True when the frame is the one last sent on `channel` and that one went less than `window_ms` ago: the caller does not send it.
    /// Otherwise the frame is remembered as the one that goes now. `window_ms` 0: never true, nothing is remembered.
    bool Repeats(u8 channel, const u8* data, u32 size, u32 now_ms, u32 window_ms) {
        return window_ms != 0 && Repeats(channel, DigestOf(data, size), size, now_ms, window_ms);
    }
    bool Repeats(u8 channel, FrameDigest digest, u32 size, u32 now_ms, u32 window_ms) {
        if (window_ms == 0) {
            return false;
        }
        const u32 hash = digest.hash, mix = digest.mix;
        Slot* slot = &slots_[0];
        if (!(slot->used && slot->channel == channel)) {
            Slot* other = &slots_[1];
            if (other->used && other->channel == channel) {
                slot = other;
            } else if (slot->used && (!other->used || now_ms - other->sent_ms > now_ms - slot->sent_ms)) {
                slot = other; // (a third channel: the one that sent longer ago makes room)
            }
        }
        if (slot->used && slot->channel == channel && slot->size == size && slot->hash == hash && slot->mix == mix &&
            now_ms - slot->sent_ms < window_ms) {
            return true;
        }
        slot->hash = hash;
        slot->mix = mix;
        slot->sent_ms = now_ms;
        slot->size = size;
        slot->channel = channel;
        slot->used = 1;
        return false;
    }

private:
    struct Slot {
        u32 hash = 0;
        u32 mix = 0;
        u32 sent_ms = 0;
        u16 size = 0;
        u8 channel = 0;
        u8 used = 0;
    };
    Slot slots_[2];
};
static_assert(sizeof(SameFrame) == 32, "the state of the same-frame filter is two slots of 16 bytes");

/// The flow of the room's stream in a statistics window, one line (with the log on, every window: a zero is an answer too):
///   "stats: room stream flow: lag max 37 ms (the room's send to the hand-over; 0 behind a hole), waiting data peak 3 KB of 192 |
///    at the limit: 0 commands refused, 0 taken over it, 0 game frames dropped | dense-broadcast relief off: 0 same frames not sent, 0 packets handed over
///    behind another in a round (most in a round 1)"
/// `relief`: the title is one of the list. Returns the length.
inline int FormatStatsLine(const Stats& s, bool relief, char* text, std::size_t size) {
    return std::snprintf(text, size,
                         "stats: room stream flow: lag max %lu ms (the room's send to the hand-over; %lu behind a hole), waiting data peak"
                         " %lu KB of %lu | at the limit: %lu commands refused, %lu taken over it, %lu game frames dropped | dense-broadcast relief %s: %lu same"
                         " frames not sent, %lu packets handed over behind another in a round (most in a round %lu), %lu rounds sent at the send budget",
                         static_cast<unsigned long>(s.enet_lag_max_ms), static_cast<unsigned long>(s.enet_hol_max_ms),
                         static_cast<unsigned long>((s.enet_waiting_max + 1023) / 1024),
                         static_cast<unsigned long>(kWaitingLimitBytes / 1024), static_cast<unsigned long>(s.enet_cap_refused),
                         static_cast<unsigned long>(s.enet_cap_admitted), static_cast<unsigned long>(s.enet_cap_dropped), relief ? "on" : "off",
                         static_cast<unsigned long>(s.tx_same_dropped), static_cast<unsigned long>(s.rx_same_round),
                         static_cast<unsigned long>(s.rx_round_max), static_cast<unsigned long>(s.tx_budget_rounds));
}

/// What the room's stream lost on the way and what the bound of the placeholders refused, in a statistics window, one line (every window, as the one above):
///   "stats: room stream losses: 0 sequence numbers skipped (0 of them in what waited while the network thread was away 100 ms or more) |
///    at the limit: 0 refused for the placeholders at their maximum, most held 0 of 2048 | away: 0 times, most read after one 0 datagrams 0 KB, 0 of them sent again,
///    the game's queue held 0 rounds for it"
/// (the last part, 2026-11-15: the times the thread was away 100 ms or more, and the most that waited in the socket's ring after one of them, read up to the
/// empty socket - datagrams, KB, and the copies the room sent again among them: whether the ring is bound by datagrams or bytes, and what the resends cost)
/// `cap_drop_max`: the title's bound of the placeholders (its row's cap_drop_max; 0 = the drop at the limit is off for the title).
/// A skipped number is a new command of the room that never came (Wi-Fi or the socket's ring); one found away may be the ring's, one found sooner is not.
/// Returns the length.
inline int FormatLossLine(const Stats& s, u32 cap_drop_max, char* text, std::size_t size) {
    return std::snprintf(text, size,
                         "stats: room stream losses: %lu sequence numbers skipped (%lu of them in what waited while the network thread was away %u ms or more)"
                         " | at the limit: %lu refused for the placeholders at their maximum, most held %lu of %u"
                         " | away: %lu times, most read after one %lu datagrams %lu KB, %lu of them sent again, the game's queue held %lu rounds for it",
                         static_cast<unsigned long>(s.enet_seq_skipped), static_cast<unsigned long>(s.enet_seq_skipped_away), static_cast<unsigned>(kAwayMs),
                         static_cast<unsigned long>(s.enet_cap_full), static_cast<unsigned long>(s.enet_cap_held_max), static_cast<unsigned>(cap_drop_max),
                         static_cast<unsigned long>(s.enet_away_episodes), static_cast<unsigned long>(s.enet_away_dgrams_max),
                         static_cast<unsigned long>((s.enet_away_bytes_max + 1023) / 1024), static_cast<unsigned long>(s.enet_away_dup_max),
                         static_cast<unsigned long>(s.tx_drain_rounds));
}

} // namespace flow
} // namespace azl
