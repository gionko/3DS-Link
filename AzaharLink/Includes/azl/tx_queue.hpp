// The frames waiting to go to the room, bounded (spec F20): a stalled network must neither pile up
// memory nor release a burst of stale game data afterwards. The real radio loses frames it cannot send
// and the newest state wins, so when the queue is full or a frame is old it is the OLDEST game frame
// that goes. The handshake (authentication, EAPoL, node map, Deauthentication, the room's own messages)
// is never discarded for age or to make room, it only has a cap of its own.
// Not thread safe: RoomClient calls it under its mutex.
#pragma once

#include <deque>
#include <vector>

#include "azl/common.hpp"

namespace azl {

class TxQueue {
public:
    struct Item {
        std::vector<u8> bytes;
        u64 queued_ms = 0;
        bool reliable = true;
        bool expendable = false; ///< game data and beacons; false = handshake / room messages
        bool duplicate = false;  ///< sent again a round later (radio_like=2 unicast game data; the receiver drops the copy by its number)
    };
    struct Limits {
        u32 max_bytes = 32768;    ///< expendable frames waiting: 0.53 s of the worst uplink seen (60 KB/s)
        u32 max_frames = 128;     ///< ... and their number
        u32 max_age_ms = 1000;    ///< an expendable frame older than this is stale
        u32 max_protected = 32;   ///< handshake frames waiting
    };
    struct Drops {
        u32 stale = 0; ///< expendable frames thrown out because they waited longer than max_age_ms
        u32 cap = 0;   ///< ... or to make room, or refused because of a cap
    };

    TxQueue() = default;
    explicit TxQueue(const Limits& limits) : limits_(limits) {}

    /// Queues an item. Older expendable frames are dropped first when an expendable one does not fit. Returns
    /// false if the item itself was refused: an expendable frame bigger than the whole byte cap, or a
    /// handshake frame beyond its cap.
    bool Push(Item&& item, u64 now_ms, Drops* drops) {
        if (item.expendable) {
            const u32 size = static_cast<u32>(item.bytes.size());
            if (size > limits_.max_bytes) {
                ++drops->cap;
                return false;
            }
            DropStale(now_ms, drops);
            while (expendable_frames_ > 0 &&
                   (expendable_frames_ >= limits_.max_frames || expendable_bytes_ + size > limits_.max_bytes)) {
                DropOldestExpendable();
                ++drops->cap;
            }
            ++expendable_frames_;
            expendable_bytes_ += size;
        } else {
            if (protected_frames_ >= limits_.max_protected) {
                ++drops->cap;
                return false;
            }
            ++protected_frames_;
        }
        bytes_ += item.bytes.size();
        queue_.push_back(std::move(item));
        return true;
    }

    /// Queues a handshake item that goes out before everything else (the join request); not limited.
    void PushFront(Item&& item) {
        item.expendable = false;
        ++protected_frames_;
        bytes_ += item.bytes.size();
        queue_.push_front(std::move(item));
    }

    /// Moves everything that is waiting to the end of `out`, except expendable frames that went stale.
    void Take(std::deque<Item>* out, u64 now_ms, Drops* drops) {
        for (Item& item : queue_) {
            if (item.expendable && Stale(item, now_ms)) {
                ++drops->stale;
                continue;
            }
            out->push_back(std::move(item));
        }
        Clear();
    }

    void Clear() {
        queue_.clear();
        expendable_frames_ = expendable_bytes_ = protected_frames_ = 0;
        bytes_ = 0;
    }

    bool empty() const {
        return queue_.empty();
    }
    std::size_t size() const {
        return queue_.size();
    }
    /// Bytes of everything waiting.
    std::size_t bytes() const {
        return bytes_;
    }

private:
    bool Stale(const Item& item, u64 now_ms) const {
        return now_ms > item.queued_ms && now_ms - item.queued_ms > limits_.max_age_ms;
    }
    void Forget(const Item& item) {
        bytes_ -= item.bytes.size();
        if (item.expendable) {
            --expendable_frames_;
            expendable_bytes_ -= static_cast<u32>(item.bytes.size());
        } else {
            --protected_frames_;
        }
    }
    void DropStale(u64 now_ms, Drops* drops) {
        // Oldest first, so the stale ones are at the front (the queue is in order of arrival).
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->expendable && Stale(*it, now_ms)) {
                Forget(*it);
                it = queue_.erase(it);
                ++drops->stale;
            } else if (it->expendable) {
                break; // the first expendable frame that is still fresh: every later one is newer
            } else {
                ++it;
            }
        }
    }
    void DropOldestExpendable() {
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->expendable) {
                Forget(*it);
                queue_.erase(it);
                return;
            }
        }
    }

    Limits limits_;
    std::deque<Item> queue_;
    u32 expendable_frames_ = 0;
    u32 expendable_bytes_ = 0;
    u32 protected_frames_ = 0;
    std::size_t bytes_ = 0;
};

} // namespace azl
