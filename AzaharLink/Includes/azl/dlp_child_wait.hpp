// H5 (1.1.4): the host of a game that hosts the REAL Download Play (the RealDlp column of its row in azl/title_rules.hpp: azl/dlp_real_net.hpp)
// waits, in its state 9, for the guests that came through the system's Download Play application and left the room to start their child: the
// console's plugin leaves the room with the Download Play application and comes back with the child, under the same MAC. The game reads
// GetConnectingClients in its state 9 and goes to its own network when it is 0 (Mario Party: Island Tour, analysis/island-tour-2026-11-13/host/
// RAPPORTO.md 3.4: at most 20 s, then its own network waits 20 s for every player it counted); the wait makes those 20 s start with the child up.
// Design: analysis/piano-1.1.4-2026-11-15/specifiche/3-G2.md part H5, with the user's caps (REGOLE-IMPLEMENTAZIONE.md, 2026-11-15): 15 s at most,
// 10 s if one of the guests is an Azahar (an emulator never leaves the room: its child looks for the network 10 rounds of 1 s), 6 s after the
// first guest is back (its child already looks for the network). A guest still in the room kLeaveMs after the end is an Azahar: it holds nothing.
// Pure: the presence in the room is the caller's function, the clock the caller's; nothing is sent (nothing of the child ever travels through the
// room), no log, no allocation. The adapter keeps one (its wait_); the guests it notes are only those the room lists in the Download Play
// application at the final (every other guest - a console or an Azahar that plays the game itself - is never waited for: Luigi's Mansion and
// Mario Kart 7 with their own copies stay as they were).
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace dlp_real {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

class ChildWait {
public:
    static constexpr u32 kSlots = 7;              // the clients of a host (== the adapter's kRealClients: asserted there)
    static constexpr u32 kLeaveMs = 5000;         // still in the room this long after the end: an emulator, it does not hold [estimate]
    static constexpr u32 kMaxMs = 15000;          // the longest wait, whatever happens (the user's cap)
    static constexpr u32 kStayedCapMs = 10000;    // with a guest that stayed in the room (an Azahar): its child already looks for the network (the user's cap)
    static constexpr u32 kBackGraceMs = 6000;     // after the first one back: its child looks for the network too (the user's cap)
    static_assert(kMaxMs < 20000, "the game waits for GetConnectingClients = 0 at most 20 s in its state 9 (host/RAPPORTO.md 3.4)");
    static_assert(kLeaveMs < kStayedCapMs && kStayedCapMs <= kMaxMs && kBackGraceMs < kStayedCapMs, "the caps of the wait, shortest first");

    /// How one wait ended, for ONE line of the log (no MAC).
    struct End {
        u16 waited_ms;
        u8 back;     // guests that left the room and came back (their child)
        u8 stayed;   // guests that never left it (an emulator)
        u8 missing;  // guests still awaited at the end (a cap)
    };

    void SetOn(bool on) {
        on_ = on;
    }
    bool on() const {
        return on_;
    }
    /// A new lobby, or the end of a wait: nothing noted, not armed (on_ stays).
    void Reset() {
        noted_ = 0;
        left_ = 0;
        back_ = 0;
        stayed_ = 0;
        armed_ = false;
    }
    /// At the final to `slot` (node - 2): remembered only if the wait is on, the slot is one of a host's and `listed` - the room lists the guest
    /// NOW in the Download Play application (a guest that is not seen cannot be waited for). Not after the count started.
    void Note(u32 slot, const Mac& mac, bool listed) {
        if (!on_ || armed_ || slot >= kSlots || !listed) {
            return;
        }
        mac_[slot] = mac;
        noted_ = static_cast<u8>(noted_ | (1u << slot));
        left_ = static_cast<u8>(left_ & ~(1u << slot));
    }
    /// The Download Play's network ends (the lobby's state 9): the count starts. Nothing if off, if nobody was noted, or already armed.
    void Arm(u32 now_ms) {
        if (!on_ || armed_ || noted_ == 0) {
            return;
        }
        armed_ = true;
        t0_ = now_ms;
        first_back_ = now_ms;
        back_ = 0;
        stayed_ = 0;
    }
    /// The slots still awaited (0 when not armed): bit s = node s + 2. The lobby reads it in its state 9 only.
    u8 Awaited() const {
        return armed_ ? noted_ : 0;
    }
    /// One round (the network thread): in_room(mac) says whether the room lists the guest now. When the wait ends, *end is filled, the wait is
    /// Reset and true is returned (once per wait).
    template <typename InRoom>
    bool Step(u32 now_ms, InRoom in_room, End* end) {
        if (!armed_) {
            return false;
        }
        const u32 e = Elapsed(now_ms, t0_);
        for (u32 s = 0; s < kSlots; ++s) {
            const u8 bit = static_cast<u8>(1u << s);
            if ((noted_ & bit) == 0) {
                continue;
            }
            const bool in = in_room(mac_[s]);
            if ((left_ & bit) != 0) {
                if (in) {  // back: its child is in the room
                    noted_ = static_cast<u8>(noted_ & ~bit);
                    if (back_++ == 0) {
                        first_back_ = now_ms;
                    }
                }
            } else if (!in) {
                left_ = static_cast<u8>(left_ | bit);  // out of the room: its Download Play application ended, the child starts
            } else if (e >= kLeaveMs) {
                noted_ = static_cast<u8>(noted_ & ~bit);  // never left: an emulator, it holds nothing
                ++stayed_;
            }
        }
        const bool over = noted_ == 0 || e >= kMaxMs || (stayed_ != 0 && e >= kStayedCapMs) ||
                          (back_ != 0 && Elapsed(now_ms, first_back_) >= kBackGraceMs);
        if (!over) {
            return false;
        }
        end->waited_ms = static_cast<u16>(e < 0xFFFFu ? e : 0xFFFFu);
        end->back = back_;
        end->stayed = stayed_;
        end->missing = static_cast<u8>(Count(noted_));
        Reset();
        return true;
    }

private:
    static u32 Elapsed(u32 now, u32 stamp) {  // (a clock that went back counts as no time, as the adapter's own)
        const s32 d = static_cast<s32>(now - stamp);
        return d < 0 ? 0u : static_cast<u32>(d);
    }
    static u32 Count(u8 mask) {
        u32 n = 0;
        for (; mask != 0; mask = static_cast<u8>(mask & (mask - 1))) {
            ++n;
        }
        return n;
    }

    Mac mac_[kSlots] = {};
    u32 t0_ = 0;
    u32 first_back_ = 0;
    u8 noted_ = 0;   // the guests awaited (bit s = node s + 2)
    u8 left_ = 0;    // those of them that left the room
    u8 back_ = 0;
    u8 stayed_ = 0;
    bool armed_ = false;
    bool on_ = false;
};

// <<< game-thread code

}  // namespace dlp_real
}  // namespace azl
