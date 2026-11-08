// The chat of the room (Azahar's IdChatMessage): what comes in, what is kept, what goes out. Plain C++ and no heap: the block is
// allocated once by the plugin (Sources/ctr/plugin.cpp, WireChat) and never freed, and the PC tests run the same code
// (Tests/chat_core_test.cpp, and Tests/chat_room_probe.cpp against the real room through Tests/integration.sh).
//
// The room relays a member's message to everybody else as [0x06][u32 BE nick][u32 BE username][u32 BE message] (strings are a
// big-endian u32 length and the bytes, no NUL). It checks nothing but the length (500 bytes, cut even in the middle of a UTF-8
// character): no rate, no controls, no empty text; and it never sends a message back to the member who wrote it. So:
// - at most kRateMax messages in any sliding second are decoded (Admit, on the network thread, before the decoding), and at most
//   kRatePerMember of them from one member (SenderTag: a hash of the raw nickname, the room's own for the member, read before the
//   decoding): a member who floods the room costs the network thread a few compares per packet, not the cleaning and the drawing,
//   and leaves the other half of the second to the others;
// - the decoding reads the lengths by hand with bounds checks (PacketReader::String would malloc a std::string per field);
// - the text is cleaned to printable ASCII, the one thing the font has, and cut to kTextMax with "..." at the end;
// - our own lines are pushed here when they go out (PushOwn): the room will not echo them.
// The status messages of the room (IdStatusMessage, 14) are not read: who comes and goes is told in the chat from the room's list of
// members instead (OnPresence, from Plugin::NoteRoomMembers on the network thread: azl/room_members.hpp, RoomMemberChanges), a line
// with no nickname, drawn like the plugin's notices: "Friend-A joined the room" (a request of the testers, 2026-11-08: you did not know
// you could talk to a friend who had not written anything yet).
//
// Threads: the network thread is the only writer of the ring (the messages that come in and our own); the drawing is its only
// reader, through a seqlock that never makes either wait (ChatReader keeps the last complete copy when a read meets a write).
// The outgoing box has one producer (the keyboard) and one consumer (the network thread), handed over by an atomic flag.
#pragma once

#include <atomic>

#include "azl/common.hpp"

namespace azl {
namespace chat {

constexpr u8 kIdChatMessage = 6;   ///< room::IdChatMessage (room_client.cpp asserts they are the same)
constexpr u32 kNickMax = 20;       ///< the room's own rule for a nickname: [ a-zA-Z0-9._-]{4,20}
constexpr u32 kTextMax = 128;      ///< characters kept of a message, and the most one of ours may have
constexpr u32 kLines = 5;          ///< messages kept in all: the next one pushes the oldest out
constexpr u32 kRateMax = 4;        ///< messages decoded in any sliding second ...
constexpr u32 kRateWindowMs = 1000; ///< ... of this length
constexpr u32 kRatePerMember = 2;  ///< of them, the most from one member (one flooding leaves the others 2 a second)
constexpr u32 kSendGapMs = 1000;   ///< at most one message of ours a second
constexpr u32 kPacketMax = 1 + 4 + kTextMax; ///< [0x06][u32 BE length][text]: what one of ours costs on the wire
// The cleaning reads at most kScanPerChar input bytes per character it may keep (a 4-byte UTF-8 character is the most one
// character costs): 5000 characters of a room that does not cut cost what 128 do.
constexpr u32 kScanPerChar = 4;

inline bool IsSpace(char c) {
    return c == ' ';
}

/// How many bytes an escape sequence that starts at in[i] (the byte after ESC) takes: a CSI (ESC [ parameters final) up to its
/// final byte, a two-byte escape (ESC and one of 0x40-0x5F) its one byte, anything else none. Never past `end`.
inline std::size_t EscapeLength(const u8* in, std::size_t i, std::size_t end) {
    if (i >= end) {
        return 0;
    }
    if (in[i] == '[') {
        std::size_t k = i + 1;
        while (k < end && in[k] >= 0x20 && in[k] <= 0x3F) {
            ++k; // parameters and intermediates
        }
        if (k < end && in[k] >= 0x40 && in[k] <= 0x7E) {
            ++k; // the final byte
        }
        return k - i;
    }
    return in[i] >= 0x40 && in[i] <= 0x5F ? 1 : 0;
}

/// Cleans `n` bytes of the room into `out` (room for cap + 1 chars, NUL included) and returns how many it kept; 0 means nothing is
/// left and the field is dropped. Printable ASCII (0x20-0x7E) stays; a UTF-8 sequence (a lead byte and the continuation bytes it
/// announces) becomes one '?'; a control character, DEL or an escape sequence becomes a space; spaces in a row are one; spaces at
/// the start and the end go. Longer than `cap`: the first cap - 3 characters and "..." when `ellipsis` (the reader sees that the
/// message was cut), the first `cap` otherwise. The scan stops after cap * kScanPerChar bytes (and the "..." says so).
inline u32 Sanitize(const u8* in, std::size_t n, char* out, u32 cap, bool ellipsis) {
    const std::size_t limit = static_cast<std::size_t>(cap) * kScanPerChar + 16;
    const std::size_t end = n < limit ? n : limit;
    u32 len = 0;
    bool space = false;
    bool cut = false;
    std::size_t i = 0;
    while (i < end) {
        const u8 c = in[i++];
        char put;
        if (c > 0x20 && c < 0x7F) {
            put = static_cast<char>(c);
        } else if (c >= 0x80) {
            // (the continuation bytes the lead announces, if they are there: a stray one is a '?' of its own)
            const u32 follow = c >= 0xF0 && c <= 0xF7 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC0 && c <= 0xDF ? 1 : 0;
            for (u32 k = 0; k < follow && i < end && (in[i] & 0xC0) == 0x80; ++k) {
                ++i;
            }
            put = '?';
        } else {
            if (c == 0x1B) {
                i += EscapeLength(in, i, end);
            }
            space = true; // the space itself, a control, DEL or an escape
            continue;
        }
        const u32 need = space && len > 0 ? 2 : 1;
        if (len + need > cap) {
            cut = true;
            break;
        }
        if (space && len > 0) {
            out[len++] = ' ';
        }
        space = false;
        out[len++] = put;
    }
    if (!cut && end < n && len > 0) {
        cut = true; // (the scan stopped with input left: what it holds is not known, but the message was not read to its end)
    }
    if (cut && ellipsis && cap > 3) {
        if (len > cap - 3) {
            len = cap - 3;
        }
        while (len > 0 && IsSpace(out[len - 1])) {
            --len;
        }
        if (len > 0) {
            out[len++] = '.';
            out[len++] = '.';
            out[len++] = '.';
        }
    }
    out[len] = '\0';
    return len;
}

/// One line of the chat. Its size is a whole number of words: the ring copies it a word at a time.
struct Line {
    u32 t_ms = 0;   ///< when it came (or went), on the clock of the network thread
    u8 nick_len = 0;
    u8 text_len = 0;
    u8 own = 0;     ///< 1: one of ours (the room does not send those back)
    u8 pad = 0;
    char nick[kNickMax + 1] = {};
    char text[kTextMax + 1] = {};
};
constexpr u32 kLineWords = (sizeof(Line) + 3) / 4;
static_assert(sizeof(Line) == kLineWords * 4, "a line is copied a word at a time");
static_assert(kTextMax <= 255 && kNickMax <= 255, "the lengths are bytes");
static_assert(kNickMax + 16 <= kTextMax, "a presence line (OnPresence) fits a line's text");

/// What the reader sees: the lines oldest first.
struct View {
    Line lines[kLines];
    u32 count = 0;
    u32 seq = 0; ///< the ring's sequence this copy was taken at (0: the empty ring)
};

/// How a chat packet of the room was read.
enum class Decoded { Ok, Malformed, Empty };

/// Reads one length-prefixed string at *pos: false when its length or its bytes are not all in the packet.
inline bool ReadField(const u8* data, std::size_t size, std::size_t* pos, const u8** bytes, std::size_t* len) {
    if (size - *pos < 4) {
        return false;
    }
    const u32 n = LoadBE32(data + *pos);
    *pos += 4;
    if (n > size - *pos) {
        return false;
    }
    *bytes = data + *pos;
    *len = n;
    *pos += n;
    return true;
}

/// Decodes [0x06][str nick][str username][str message] (the whole ENet packet) into `out`: the nick and the text cleaned, the
/// username skipped. Malformed: not a chat packet, or a length past the end. Empty: the nick or the text has nothing left.
inline Decoded DecodeChatMessage(const u8* data, std::size_t size, Line* out) {
    if (size < 1 || data[0] != kIdChatMessage) {
        return Decoded::Malformed;
    }
    std::size_t pos = 1;
    const u8* nick = nullptr;
    const u8* user = nullptr;
    const u8* text = nullptr;
    std::size_t nick_n = 0, user_n = 0, text_n = 0;
    if (!ReadField(data, size, &pos, &nick, &nick_n) || !ReadField(data, size, &pos, &user, &user_n) ||
        !ReadField(data, size, &pos, &text, &text_n)) {
        return Decoded::Malformed;
    }
    out->nick_len = static_cast<u8>(Sanitize(nick, nick_n, out->nick, kNickMax, false));
    out->text_len = static_cast<u8>(Sanitize(text, text_n, out->text, kTextMax, true));
    out->own = 0;
    return out->nick_len == 0 || out->text_len == 0 ? Decoded::Empty : Decoded::Ok;
}

/// Who sent a chat packet, read before the decoding: a hash (FNV-1a) of the raw bytes of its first field, the nickname, which the
/// room writes itself from the member's own (unique in the room: a member cannot write another's). At most kNickMax bytes are read
/// (the room's rule); 0 when the field is not in the packet (the decoding then finds it malformed). Two nicknames with the same hash
/// share their quota: never more than kRateMax in all.
inline u32 SenderTag(const u8* data, std::size_t size) {
    std::size_t pos = 1;
    const u8* nick = nullptr;
    std::size_t n = 0;
    if (size < 1 || !ReadField(data, size, &pos, &nick, &n)) {
        return 0;
    }
    u32 h = 2166136261u;
    for (std::size_t i = 0; i < n && i < kNickMax; ++i) {
        h = (h ^ nick[i]) * 16777619u;
    }
    return h;
}

/// "More than kRateMax in a sliding second are dropped, and more than kRatePerMember of one member": the times and the senders of
/// the last kRateMax messages taken, a ring (every message taken in the last second is in it); a new one is taken only if fewer than
/// kRatePerMember of its sender's are less than kRateWindowMs old and the oldest of them all is at least kRateWindowMs old. The clock
/// is u32 milliseconds: the differences are taken unsigned, so it may wrap.
class RateLimit {
public:
    bool Admit(u32 now_ms, u32 tag) {
        u32 same = 0;
        for (u32 i = 0; i < filled_; ++i) {
            same += (tags_[i] == tag && now_ms - ts_[i] < kRateWindowMs) ? 1 : 0;
        }
        if (same >= kRatePerMember) {
            return false;
        }
        u32 slot = filled_;
        if (filled_ < kRateMax) {
            ++filled_;
        } else if (now_ms - ts_[oldest_] < kRateWindowMs) {
            return false;
        } else {
            slot = oldest_;
            oldest_ = (oldest_ + 1) % kRateMax;
        }
        ts_[slot] = now_ms;
        tags_[slot] = tag;
        return true;
    }

private:
    u32 ts_[kRateMax] = {};
    u32 tags_[kRateMax] = {};
    u32 oldest_ = 0;
    u32 filled_ = 0;
};

/// The kept lines: kLines slots, one writer, one reader, a seqlock. The writer makes the sequence odd, writes, makes it even; the
/// reader copies only an even sequence that did not change while it copied. Every word of the slots is an atomic access, a release
/// store and an acquire load (no fences: ThreadSanitizer follows these, and a word of the next write that the reader sees makes it
/// see the odd sequence before it too), so a copy that meets a write is thrown away, never undefined. The cost is a barrier per
/// word: 40 a line written (at most a few a second), 40 a line read when the ring changed.
class ChatRing {
public:
    /// The writer (the network thread only).
    void Push(const Line& line) {
        u32 words[kLineWords];
        std::memcpy(words, &line, sizeof(Line));
        const u32 s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        for (u32 w = 0; w < kLineWords; ++w) {
            __atomic_store_n(&slots_[head_][w], words[w], __ATOMIC_RELEASE);
        }
        head_ = (head_ + 1) % kLines;
        if (count_ < kLines) {
            ++count_;
        }
        __atomic_store_n(&meta_, head_ | (count_ << 8), __ATOMIC_RELEASE);
        seq_.store(s + 2, std::memory_order_release);
    }

    u32 seq() const {
        return seq_.load(std::memory_order_acquire);
    }

    // >>> game-thread code (Tests/policy_check.sh: the reader runs in the drawing, on the game's thread: no heap, no wait, no big buffer)
    /// The reader: copies the lines into `out`, oldest first. False when a write was under way (`out` is then half written: see
    /// ChatReader, which copies into the buffer it does not show).
    bool ReadInto(View* out) const {
        const u32 s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) {
            return false;
        }
        const u32 meta = __atomic_load_n(&meta_, __ATOMIC_ACQUIRE);
        const u32 head = meta & 0xFF;
        u32 count = meta >> 8;
        if (head >= kLines || count > kLines) {
            count = 0; // (a torn read: the check below throws it away)
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = (head + kLines - count + i) % kLines;
            u8* dst = reinterpret_cast<u8*>(&out->lines[i]);
            for (u32 w = 0; w < kLineWords; ++w) {
                const u32 word = __atomic_load_n(&slots_[slot][w], __ATOMIC_ACQUIRE);
                std::memcpy(dst + w * 4, &word, 4);
            }
        }
        if (seq_.load(std::memory_order_relaxed) != s1) {
            return false;
        }
        out->count = count;
        out->seq = s1;
        return true;
    }
    // <<< game-thread code

private:
    std::atomic<u32> seq_{0};
    u32 meta_ = 0;  ///< head | count << 8, written under the sequence
    u32 head_ = 0;  ///< the writer's own: the next slot
    u32 count_ = 0; ///< the writer's own
    u32 slots_[kLines][kLineWords] = {};
};

/// The one reader's side of the ring: two views, the one shown and the one a refresh fills; a refresh that meets a write keeps the
/// one shown. Never waits.
class ChatReader {
public:
    // >>> game-thread code (Tests/policy_check.sh)
    const View& Refresh(const ChatRing& ring) {
        if (ring.seq() != views_[shown_].seq && ring.ReadInto(&views_[shown_ ^ 1])) {
            shown_ ^= 1;
        }
        return views_[shown_];
    }
    // <<< game-thread code
    const View& shown() const {
        return views_[shown_];
    }

private:
    View views_[2];
    u32 shown_ = 0;
};

/// The counters of the chat (relaxed: the log and the tests read them).
struct Counters {
    std::atomic<u32> taken{0};      ///< messages decoded and kept
    std::atomic<u32> rate_drops{0}; ///< dropped by Admit (more than kRateMax in a second, or kRatePerMember of one member)
    std::atomic<u32> malformed{0};  ///< a length past the end
    std::atomic<u32> empty{0};      ///< nothing left after the cleaning
    std::atomic<u32> sent{0};       ///< ours that went out
    std::atomic<u32> refused{0};    ///< ours dropped: not in the room, or ENet did not take the packet (all of them)
    std::atomic<u32> refused_left{0}; ///< ... of them, those dropped because we were not in the room (the rest: ENet's)
    std::atomic<u32> presence{0};   ///< lines of a member who came into the room or left it (OnPresence)
};

} // namespace chat

/// The chat's block: the rate limit, the kept lines and their reader's copy, the outgoing box and the counters. One per process,
/// allocated the first time the chat is on and never freed (a network thread that RoomClient::Stop gave up on may still hold it).
/// The room client calls Admit, OnPacket, TakeOutgoing and SentOutgoing on its network thread; the keyboard calls Post; the drawing
/// calls reader.Refresh(ring).
class ChatSink {
public:
    /// `send_gap_ms`: the least time between two messages of ours (the plugin keeps kSendGapMs; the room probe floods with 0).
    explicit ChatSink(u32 send_gap_ms = chat::kSendGapMs) : send_gap_ms_(send_gap_ms) {}
    ChatSink(const ChatSink&) = delete;
    ChatSink& operator=(const ChatSink&) = delete;

    // ---- the network thread ----
    /// The rate limit, before the decoding: false (and counted) for a message past kRateMax in the last second, or past
    /// kRatePerMember of its sender's (`tag`: chat::SenderTag of the packet).
    bool Admit(u32 now_ms, u32 tag) {
        if (rate_.Admit(now_ms, tag)) {
            return true;
        }
        counters.rate_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    /// A chat packet of the room (the whole ENet packet): decoded, cleaned and kept. False when it was dropped (counted).
    bool OnPacket(const u8* data, std::size_t size, u32 now_ms) {
        chat::Line line;
        const chat::Decoded d = chat::DecodeChatMessage(data, size, &line);
        if (d != chat::Decoded::Ok) {
            (d == chat::Decoded::Malformed ? counters.malformed : counters.empty).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        line.t_ms = now_ms;
        ring.Push(line);
        counters.taken.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    /// The packet of the message in the box, into `packet` (kPacketMax bytes): its size, or 0 when there is nothing to send this
    /// round (an empty box, or the last one went less than send_gap_ms ago: it waits in the box). Not in the room (`joined`
    /// false): the message is refused and the box emptied. A packet returned must be followed by SentOutgoing.
    u32 TakeOutgoing(u32 now_ms, bool joined, u8* packet) {
        if (out_full_.load(std::memory_order_acquire) == 0) {
            return 0;
        }
        if (!joined) {
            counters.refused_left.fetch_add(1, std::memory_order_relaxed); // (before the sum: a look that sees the refusal sees why)
            counters.refused.fetch_add(1, std::memory_order_relaxed);
            out_full_.store(0, std::memory_order_release);
            return 0;
        }
        if (has_sent_ && now_ms - last_send_ms_ < send_gap_ms_) {
            return 0;
        }
        packet[0] = chat::kIdChatMessage;
        StoreBE32(packet + 1, out_len_);
        std::memcpy(packet + 5, out_, out_len_);
        return 5 + out_len_;
    }
    /// After the packet of TakeOutgoing: `sent` = it went to ENet. Ours is kept as a line with our nickname (the room does not send
    /// it back to us) and the box is free again.
    void SentOutgoing(const char* nick, std::size_t nick_len, u32 now_ms, bool sent) {
        if (sent) {
            chat::Line line;
            line.nick_len = static_cast<u8>(chat::Sanitize(reinterpret_cast<const u8*>(nick), nick_len, line.nick, chat::kNickMax, false));
            std::memcpy(line.text, out_, out_len_ + 1);
            line.text_len = static_cast<u8>(out_len_);
            line.own = 1;
            line.t_ms = now_ms;
            ring.Push(line);
            has_sent_ = true;
            last_send_ms_ = now_ms;
            counters.sent.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.refused.fetch_add(1, std::memory_order_relaxed);
        }
        out_full_.store(0, std::memory_order_release);
    }

    /// A member came into the room (`joined`) or left it: a line with no nickname (drawn like a notice), "<nick> joined the room" /
    /// "<nick> left the room", the nickname cleaned like the room's. The network thread only (the ring's one writer).
    void OnPresence(const char* nick, std::size_t nick_len, bool joined, u32 now_ms) {
        chat::Line line;
        const u32 n = chat::Sanitize(reinterpret_cast<const u8*>(nick), nick_len, line.text, chat::kNickMax, false);
        if (n == 0) {
            return;
        }
        const char* const what = joined ? " joined the room" : " left the room";
        const std::size_t what_len = std::strlen(what);
        std::memcpy(line.text + n, what, what_len + 1); // (kNickMax + the longer of the two fit kTextMax)
        line.text_len = static_cast<u8>(n + what_len);
        line.nick_len = 0;
        line.t_ms = now_ms;
        ring.Push(line);
        counters.presence.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- the keyboard (one producer) ----
    /// Puts a message in the box, cleaned like the room's (and cut at kTextMax). False while the last one has not gone yet (the
    /// keyboard keeps its draft) or when nothing is left of it.
    bool Post(const char* text, std::size_t n) {
        if (out_full_.load(std::memory_order_acquire) != 0) {
            return false;
        }
        const u32 len = chat::Sanitize(reinterpret_cast<const u8*>(text), n, out_, chat::kTextMax, true);
        if (len == 0) {
            return false;
        }
        out_len_ = len;
        out_full_.store(1, std::memory_order_release);
        return true;
    }
    bool OutboxBusy() const {
        return out_full_.load(std::memory_order_acquire) != 0;
    }
    /// The text of the last message posted (kTextMax + 1 bytes, ended by 0). Read with the box free: the network thread is done with it
    /// and only Post writes it.
    const char* Outbox() const {
        return out_;
    }

    chat::ChatRing ring;
    chat::ChatReader reader; ///< the drawing's (the one reader)
    chat::Counters counters;

private:
    chat::RateLimit rate_;      ///< the network thread's
    u32 send_gap_ms_;
    bool has_sent_ = false;     ///< the network thread's ...
    u32 last_send_ms_ = 0;      ///< ... when ours last went
    std::atomic<u32> out_full_{0};
    u32 out_len_ = 0;
    char out_[chat::kTextMax + 1] = {};
};

} // namespace azl
