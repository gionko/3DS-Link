// In-memory stand-in for the room server + consoles, shared by the host-side tests.
// MockRoom routes like Azahar's Room::HandleWifiPacket: broadcast MAC -> everybody but the sender,
// otherwise only the member with that MAC. Every engine that sends through a Console is watched by
// the Azahar-peer rule checker (azahar_rules.hpp).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "azl/room_proto.hpp"
#include "azl/uds_engine.hpp"
#include "azahar_rules.hpp"
#include "support.hpp"

class MockRoom {
public:
    MockRoom() : thread_([this] { Loop(); }) {}
    ~MockRoom() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    int AddMember(const Mac& mac, std::function<void(const WifiPacket&)> deliver) {
        std::lock_guard<std::mutex> l(m_);
        members_.push_back({mac, std::move(deliver), true});
        g_rules.AddMember(mac);
        return static_cast<int>(members_.size()) - 1;
    }
    /// After this returns nothing is delivered to the member any more, and no delivery to it is still
    /// running (the owner may be destroyed right away).
    void RemoveMember(int idx) {
        std::lock_guard<std::mutex> delivering(deliver_m_);
        std::lock_guard<std::mutex> l(m_);
        members_[idx].present = false;
    }
    // What RoomMember::SendWifiPacket + Room::HandleWifiPacket do.
    void Send(int from, WifiPacket p) {
        // Go through the real wire encoding to exercise it end to end.
        auto enc = room::EncodeWifiPacket(p);
        std::lock_guard<std::mutex> l(m_);
        queue_.push_back({from, std::move(enc)});
        cv_.notify_all();
    }
    bool Idle() {
        std::lock_guard<std::mutex> l(m_);
        return queue_.empty() && !busy_;
    }
    void WaitIdle() {
        for (int i = 0; i < 2000 && !Idle(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

private:
    struct Member {
        Mac mac;
        std::function<void(const WifiPacket&)> deliver;
        bool present;
    };
    struct Item {
        int from;
        std::vector<u8> bytes;
    };
    void Loop() {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) {
                    return;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
                busy_ = true;
            }
            WifiPacket p;
            if (room::DecodeWifiPacket(item.bytes.data(), item.bytes.size(), &p)) {
                std::lock_guard<std::mutex> delivering(deliver_m_);
                std::vector<Member> snapshot;
                {
                    std::lock_guard<std::mutex> l(m_);
                    snapshot = members_;
                }
                for (std::size_t i = 0; i < snapshot.size(); ++i) {
                    if (!snapshot[i].present) {
                        continue;
                    }
                    const bool is_sender = static_cast<int>(i) == item.from;
                    const bool deliver = p.destination_address == kBroadcastMac
                                             ? !is_sender
                                             : snapshot[i].mac == p.destination_address;
                    if (deliver) {
                        snapshot[i].deliver(p);
                    }
                }
            }
            std::lock_guard<std::mutex> l(m_);
            busy_ = false;
        }
    }

    std::mutex m_;
    std::mutex deliver_m_; // held while a frame is handed to the members
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::vector<Member> members_;
    bool stop_ = false;
    bool busy_ = false;
    std::thread thread_;
};

/// One console of a test: an engine whose frames go through the mock room (and the rule checker).
/// `kernel` can be replaced by a test that needs to count events (see CountingKernel); the default is
/// the plain FakeKernel.
struct Console {
    Console(MockRoom& room, u8 mac_last) : room_(room) {
        Setup(mac_last, &fake_kernel);
    }
    Console(MockRoom& room, u8 mac_last, KernelOps* kernel_ops) : room_(room) {
        Setup(mac_last, kernel_ops);
    }
    ~Console() {
        room_.RemoveMember(index);
    }
    void Setup(u8 mac_last, KernelOps* kernel_ops) {
        mac = {0x00, 0x1F, 0x32, 0x00, 0x00, mac_last};
        UdsEngine::Transport tr;
        tr.send = [this](WifiPacket& p) {
            p.transmitter_address = mac;
            g_rules.Inspect(p);
            if (sniff) {
                sniff(p);
            }
            room_.Send(index, p);
        };
        tr.get_mac = [this] { return mac; };
        engine.reset(new UdsEngine(kernel_ops, tr));
        index = room_.AddMember(mac, [this](const WifiPacket& p) {
            if (tap) {
                tap(p); // tests: observe / emulate extra behaviour of this member
            }
            engine->OnWifiPacket(p);
        });
    }
    MockRoom& room_;
    std::function<void(const WifiPacket&)> tap;   // every frame that reaches the engine
    std::function<void(const WifiPacket&)> sniff; // every frame the engine sends
    FakeKernel fake_kernel;
    std::unique_ptr<UdsEngine> engine;
    Mac mac;
    int index = -1;
};

[[maybe_unused]] static std::vector<u8> PullAll(Console& c, u32 bind_id, u16* src, u32* size) {
    std::vector<u8> out;
    CHECK_EQ(PullInto(*c.engine, bind_id, &out, size, src), 0u);
    return out;
}

/// Remembers the frames a console sends (attach it before the traffic starts).
class SentFrames {
public:
    explicit SentFrames(Console& console) {
        console.sniff = [this](const WifiPacket& p) {
            std::lock_guard<std::mutex> l(m_);
            frames_.push_back(p);
        };
    }
    int Count(WifiPacket::Type type) {
        std::lock_guard<std::mutex> l(m_);
        int n = 0;
        for (const WifiPacket& p : frames_) {
            n += p.type == type;
        }
        return n;
    }
    /// Frames of `type` addressed to `to`.
    int CountTo(WifiPacket::Type type, const Mac& to) {
        std::lock_guard<std::mutex> l(m_);
        int n = 0;
        for (const WifiPacket& p : frames_) {
            n += p.type == type && p.destination_address == to;
        }
        return n;
    }
    /// Data frames that carry UDS data (EtherType 0x876D), not the EAPoL handshake.
    int CountSecureData() {
        std::lock_guard<std::mutex> l(m_);
        int n = 0;
        for (const WifiPacket& p : frames_) {
            EtherType t;
            n += p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &t) && t == EtherType::SecureData;
        }
        return n;
    }
    std::size_t Total() {
        std::lock_guard<std::mutex> l(m_);
        return frames_.size();
    }
    /// Everything sent so far, in order.
    std::vector<WifiPacket> All() {
        std::lock_guard<std::mutex> l(m_);
        return frames_;
    }
    void Clear() {
        std::lock_guard<std::mutex> l(m_);
        frames_.clear();
    }

private:
    std::mutex m_;
    std::vector<WifiPacket> frames_;
};
