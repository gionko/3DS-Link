// The receive queue of one bind, the way nwm keeps it (fn_110fac enqueue, fn_10d9f8 pull): a byte ring of
// 32-byte units, `recv_buffer_size >> 5` of them, in which every frame takes an 8-byte header and its payload
// rounded up to whole units. A frame that does not fit pushes the OLDEST ones out (the newest state wins, the
// radio never blocks), and a ring holds at most total-1 units because head == tail means empty.
//
// It replaces a deque of whole frames (512 per bind: megabytes for 1478-byte frames, an allocation and a copy
// per frame): FFE's 0x2E30 ring is 11,808 bytes, there is no allocation after Init and a frame is copied once in
// and once out.
//
// Next to the bytes, one 32-bit stamp per frame (what the engine's clock said when the frame was stored): the statistics
// turn it into the time a frame waited for the game's PullPacket. 4 bytes per unit, since a frame takes a unit at least.
//
// Not thread safe: the engine calls it under its own lock.
#pragma once

#include <cstring>
#include <memory>
#include <new>

#include "azl/common.hpp"

namespace azl {

class RxRing {
public:
    static constexpr u32 kUnitBytes = 32;
    static constexpr u32 kHeaderBytes = 8;           // dest, src, payload size, channel
    static constexpr u32 kMaxPayload = 0x5D4 - 10;   // an aggregate holds 0x5D4 bytes, a sub-frame has 10 of header
    static constexpr u32 kMinBytes = 0x5F4;          // Bind refuses a smaller ring: it must hold the largest frame

    enum class Put { Stored, Nothing, TooBig };
    enum class Pull { Empty, TooLarge, Frame };

    static constexpr u32 UnitsFor(u32 payload) {
        return (payload + kHeaderBytes + kUnitBytes - 1) / kUnitBytes;
    }

    /// Allocates the ring (`bytes` >= kMinBytes). False when out of memory or too small.
    bool Init(u32 bytes) {
        Release();
        if (bytes < kMinBytes) {
            return false;
        }
        const u32 units = bytes / kUnitBytes;
        mem_.reset(new (std::nothrow) u8[static_cast<std::size_t>(units) * kUnitBytes]);
        stamps_.reset(new (std::nothrow) u32[units]);
        if (!mem_ || !stamps_) {
            mem_.reset();
            stamps_.reset();
            return false;
        }
        total_ = units;
        head_ = tail_ = frames_ = 0;
        stamp_head_ = stamp_tail_ = 0;
        enqueued_ = overwritten_ = 0;
        return true;
    }
    void Release() {
        mem_.reset();
        stamps_.reset();
        total_ = head_ = tail_ = frames_ = 0;
        stamp_head_ = stamp_tail_ = 0;
        enqueued_ = overwritten_ = 0;
    }
    bool valid() const {
        return mem_ != nullptr;
    }

    /// Stores a frame, pushing out the oldest ones to make room. Nothing is stored for an empty payload
    /// (nwm ignores a sub-frame that carries no data) and a payload over kMaxPayload is refused. `stamp_ms` comes back
    /// from Dequeue with the frame (0 = none).
    Put Enqueue(u16 dest, u16 src, u8 channel, const u8* payload, u32 n, u32 stamp_ms = 0) {
        if (n > kMaxPayload) {
            return Put::TooBig;
        }
        if (n == 0 || !mem_) {
            return Put::Nothing;
        }
        const u32 units = UnitsFor(n);
        while (units > Free()) {
            DropOldest();
        }
        u8* at = mem_.get() + static_cast<std::size_t>(tail_) * kUnitBytes;
        const u16 header[4] = {dest, src, static_cast<u16>(n), channel};
        std::memcpy(at, header, sizeof(header));
        WriteWrapped(at + kHeaderBytes, payload, n);
        tail_ = (tail_ + units) % total_;
        stamps_[stamp_tail_] = stamp_ms;
        stamp_tail_ = (stamp_tail_ + 1) % total_;
        ++enqueued_;
        ++frames_;
        if (tail_ == head_) { // a full ring would look empty: the oldest frame goes after all
            DropOldest();
        }
        return Put::Stored;
    }

    /// Takes the oldest frame out. A frame longer than `max_bytes` stays queued (`*src` says whose it is);
    /// `capacity` bytes at most are copied to `dst` (null = none), `*n` is the frame's whole size, `*stamp_ms` (if
    /// asked) what Enqueue was given with it.
    Pull Dequeue(u32 max_bytes, u8* dst, u32 capacity, u32* n, u16* src, u32* stamp_ms = nullptr) {
        *n = 0;
        if (head_ == tail_) {
            return Pull::Empty;
        }
        const u8* at = mem_.get() + static_cast<std::size_t>(head_) * kUnitBytes;
        u16 header[4];
        std::memcpy(header, at, sizeof(header));
        *src = header[1];
        if (header[2] > max_bytes) {
            return Pull::TooLarge;
        }
        *n = header[2];
        if (dst) {
            ReadWrapped(at + kHeaderBytes, dst, header[2] < capacity ? header[2] : capacity);
        }
        if (stamp_ms) {
            *stamp_ms = stamps_[stamp_head_];
        }
        stamp_head_ = (stamp_head_ + 1) % total_;
        head_ = (head_ + UnitsFor(header[2])) % total_;
        --frames_;
        return Pull::Frame;
    }

    /// Forgets every queued frame; returns how many there were.
    u32 Flush() {
        const u32 n = frames_;
        head_ = tail_ = frames_ = 0;
        stamp_head_ = stamp_tail_ = 0;
        return n;
    }

    u32 total_units() const {
        return total_;
    }
    u32 used_units() const {
        return total_ - Free();
    }
    u32 frames() const {
        return frames_;
    }
    u32 enqueued() const {
        return enqueued_;
    }
    u32 overwritten() const {
        return overwritten_;
    }

private:
    u32 Free() const {
        if (head_ == tail_) {
            return total_;
        }
        return head_ > tail_ ? head_ - tail_ : total_ - (tail_ - head_);
    }

    void DropOldest() {
        const u8* at = mem_.get() + static_cast<std::size_t>(head_) * kUnitBytes;
        u16 header[4];
        std::memcpy(header, at, sizeof(header));
        head_ = (head_ + UnitsFor(header[2])) % total_;
        ++overwritten_;
        --frames_;
        stamp_head_ = (stamp_head_ + 1) % total_;
    }

    // The payload of a frame starts 8 bytes into its first unit and may run past the end of the ring.
    u32 UntilEnd(const u8* ring_at) const {
        return static_cast<u32>(mem_.get() + static_cast<std::size_t>(total_) * kUnitBytes - ring_at);
    }
    void WriteWrapped(u8* ring_at, const u8* from, u32 n) {
        const u32 first = UntilEnd(ring_at) < n ? UntilEnd(ring_at) : n;
        std::memcpy(ring_at, from, first);
        if (first < n) {
            std::memcpy(mem_.get(), from + first, n - first);
        }
    }
    void ReadWrapped(const u8* ring_at, u8* to, u32 n) const {
        const u32 first = UntilEnd(ring_at) < n ? UntilEnd(ring_at) : n;
        std::memcpy(to, ring_at, first);
        if (first < n) {
            std::memcpy(to + first, mem_.get(), n - first);
        }
    }

    std::unique_ptr<u8[]> mem_;
    std::unique_ptr<u32[]> stamps_; // one per frame, in the order of the frames (total_ entries: a frame is a unit at least)
    u32 stamp_head_ = 0, stamp_tail_ = 0;
    u32 total_ = 0;
    u32 head_ = 0;
    u32 tail_ = 0; // both in units
    u32 frames_ = 0;
    u32 enqueued_ = 0;    // frames stored since Init (Unbind reports it)
    u32 overwritten_ = 0; // ... and frames pushed out by newer ones
};

} // namespace azl
