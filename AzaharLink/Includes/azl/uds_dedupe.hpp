// Suppression of the copies of a UDS data frame that an Azahar HOST re-broadcasts.
//
// Azahar's nwm_uds re-sends every frame it receives for another node with the broadcast MAC, whatever the
// destination MAC of the received frame was. A real nwm host does that only for a frame that was NOT sent to
// the broadcast address: a broadcast-MAC frame has reached every member already. FFE and every game
// that sends with the broadcast flag (bit 1 of SendTo's flags) therefore makes the room deliver a frame twice to
// the other clients: the direct copy and the host's re-broadcast (transmitter = host MAC, same LLC and
// secure-data header - the sequence number is always 0 -, same payload). A console never sees that.
//
// A forwarded copy is dropped only when its direct twin was delivered before (so it can never lose a frame: a
// forwarded copy without a direct one is the only copy there is and goes through). Each entry is a counter, so
// the same payload sent twice in a row by one node (d1 d2 f1 f2 in the captures) keeps both direct copies.
#pragma once

#include <cstring>

#include "azl/common.hpp"

namespace azl {

/// 32-bit hash of a whole frame (LLC + secure-data header + payload), a word at a time. Accidental collisions
/// only matter between frames of one sender with the same size inside the 250 ms window.
inline u32 HashFrame(const u8* p, std::size_t n) {
    u32 h = 0x9E3779B1u;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        u32 w;
        std::memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x85EBCA6Bu;
        h ^= h >> 15;
    }
    u32 tail = 0;
    for (u32 shift = 0; i < n; ++i, shift += 8) {
        tail |= static_cast<u32>(p[i]) << shift;
    }
    h = (h ^ tail) * 0x85EBCA6Bu;
    h ^= static_cast<u32>(n) * 0x27D4EB2Fu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

class ForwardDedupe {
public:
    /// How long a direct copy waits for its twin. Measured gap direct -> forwarded: p99 9 ms, p99.9 34-56 ms,
    /// maximum 130 ms (3 players) / 189 ms (4 players) over 12,128 pairs. A longer window costs nothing: an
    /// entry that never gets its twin matters only if the same node later sends the same bytes again AND that
    /// frame's own direct copy is lost.
    static constexpr u32 kWindowMs = 250;
    /// Direct copies tracked at once: (non-host peers) x ~13 frames/s x 0.25 s = 8 live at 4 players, 20 at 8.
    static constexpr std::size_t kSlots = 64;

    void Reset() {
        for (Slot& s : slots_) {
            s.pending = 0;
        }
    }

    /// A copy that reached us straight from its sender: a forwarded twin may follow. Returns true when a live
    /// entry had to make room for it (more senders than slots).
    bool NoteDirect(u16 src, u16 size, u32 hash, u32 now_ms) {
        Slot* free_slot = nullptr;
        Slot* oldest = nullptr;
        u32 oldest_age = 0;
        for (Slot& s : slots_) {
            const u32 age = now_ms - s.ms;
            if (s.pending == 0 || age > kWindowMs) {
                if (!free_slot) {
                    free_slot = &s;
                }
                continue;
            }
            if (s.hash == hash && s.size == size && s.src == src) {
                if (s.pending < 255) {
                    ++s.pending;
                }
                s.ms = now_ms;
                return false;
            }
            if (!oldest || age > oldest_age) {
                oldest = &s;
                oldest_age = age;
            }
        }
        Slot* target = free_slot;
        const bool evict = target == nullptr;
        if (evict) {
            target = oldest; // full: forget the oldest; its twin, if it ever comes, is simply delivered
        }
        target->hash = hash;
        target->ms = now_ms;
        target->src = src;
        target->size = size;
        target->pending = 1;
        return evict;
    }

    /// A copy that came through the host. True = its direct twin was delivered already: that twin is used up
    /// and this copy is to be dropped.
    bool ConsumeForwarded(u16 src, u16 size, u32 hash, u32 now_ms) {
        for (Slot& s : slots_) {
            if (s.pending != 0 && s.hash == hash && s.size == size && s.src == src &&
                now_ms - s.ms <= kWindowMs) {
                --s.pending;
                return true;
            }
        }
        return false;
    }

private:
    struct Slot {
        u32 hash = 0;
        u32 ms = 0;
        u16 src = 0;
        u16 size = 0;
        u8 pending = 0; ///< direct copies delivered whose forwarded twin has not arrived yet
        u8 pad[3] = {};
    };
    static_assert(sizeof(Slot) == 16, "one slot is 16 bytes");

    Slot slots_[kSlots];
};

/// The copy a radio_like=2 sender adds to every unicast game frame (the same frame again, a network round later, so that
/// one lost datagram costs nothing instead of a retransmission timeout), dropped by the sequence number the sender puts
/// in the secure-data header (1, 2, 3 ... per sender, as the real nwm does; 0 = an unnumbered frame, Azahar's or an older
/// plugin's, never dropped). Per source node the last kSeen numbers: the copies of a round go out after all its originals
/// (RoomClient's `repeat`, kRepeatMax of them at most), so the window must hold a whole batch of originals plus what came
/// between: 128 (4.3 KB for the 16 nodes). A node that leaves the network is forgotten (the next console with its id
/// numbers from its own start).
class CopyDedupe {
public:
    static constexpr u32 kNodes = 17; // node ids 1..16
    static constexpr u32 kSeen = 128;

    void Reset() {
        for (Node& n : nodes_) {
            n.count = 0;
            n.next = 0;
        }
    }
    void ForgetNode(u16 node) {
        if (node < kNodes) {
            nodes_[node].count = 0;
            nodes_[node].next = 0;
        }
    }
    /// This node has sent numbered frames: it runs this plugin, so it drops a copy (a sender gives the copy only to such a
    /// node; an Azahar or an older plugin, which sends 0, would hand both to its game).
    bool Numbers(u16 node) const {
        return node != 0 && node < kNodes && nodes_[node].count > 0;
    }
    /// True = this (node, number) was seen lately: a copy, to be dropped. False = the first time: remembered.
    bool Seen(u16 node, u16 sequence) {
        if (sequence == 0 || node == 0 || node >= kNodes) {
            return false; // (0 is no number and no node)
        }
        Node& n = nodes_[node];
        for (u32 i = 0; i < n.count; ++i) {
            if (n.seen[i] == sequence) {
                return true;
            }
        }
        n.seen[n.next] = sequence;
        n.next = static_cast<u8>((n.next + 1) % kSeen);
        if (n.count < kSeen) {
            ++n.count;
        }
        return false;
    }

private:
    struct Node {
        u16 seen[kSeen] = {};
        u8 count = 0;
        u8 next = 0;
    };
    Node nodes_[kNodes];
};

} // namespace azl
