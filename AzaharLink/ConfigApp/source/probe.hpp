// Joining a room from the app to see that it works: the part of the connection test that does not draw. The Test the connection page and the
// popup that tests a room just added both run one of these: it starts the room client the way the plugin does, follows it (the room's
// answer, the member list, the 12 s timeout, an error with its hint) and leaves the room on a helper thread, so the screen keeps drawing.
#pragma once

#include <3ds.h>

#include <atomic>
#include <string>

#include "azl/room_client.hpp"
#include "ui.hpp"

namespace pages {

struct StopJob {
    azl::RoomClient* client = nullptr;
    std::atomic<bool> done{false};
};

class RoomProbe {
public:
    enum class Phase { Running, Done, Failed, Leaving };

    RoomProbe() = default;
    ~RoomProbe();
    RoomProbe(const RoomProbe&) = delete;
    RoomProbe& operator=(const RoomProbe&) = delete;

    // Starts a join of host:port with the nickname, the password and the MAC of this console. A reason to stop at once (no room address, no
    // network layer, no WiFi) is a Failed phase, not a client.
    void Start(const std::string& host, u16 port, const std::string& password);
    // Once per frame: follows the client.
    void Update();
    // Leaves the room (the stop runs on a thread). The probe is Leaving until leave_finished().
    void BeginLeave();
    bool leave_finished() const {
        return phase_ == Phase::Leaving && !stop_;
    }
    bool stopping() const {
        return stop_ != nullptr;
    }

    Phase phase() const {
        return phase_;
    }
    // The phase the test ended in while the room is being left (so that the result stays on the screen); otherwise phase().
    Phase shown_phase() const {
        return phase_ == Phase::Leaving ? end_phase_ : phase_;
    }
    // 0: connecting, 1: connected to the server (joining), 2: joined, 3: member list received.
    int step() const;
    unsigned frame() const {
        return frame_;
    }
    bool joined_info() const {
        return joined_info_;
    }
    const std::string& error() const {
        return error_;
    }
    const std::string& hint() const {
        return hint_;
    }
    // The room closed on us after a successful test ("" when it did not).
    const std::string& closed_note() const {
        return closed_note_;
    }
    const std::string& nickname() const {
        return nickname_;
    }
    const azl::room::RoomInfo& info() const {
        return info_;
    }
    const azl::Mac& room_mac() const {
        return room_mac_;
    }
    const azl::Mac& console_mac() const {
        return console_mac_;
    }

private:
    void Fail(const std::string& error, const std::string& hint, int step);
    void UpdateSession();

    Phase phase_ = Phase::Running;
    Phase end_phase_ = Phase::Running; // what phase_ was when the leaving began
    azl::RoomClient* client_ = nullptr;
    StopJob* stop_ = nullptr;
    u64 start_ms_ = 0;
    u64 joined_ms_ = 0;
    u32 frame_ = 0;
    int info_poll_ = 0;
    int failed_step_ = 0;
    bool joined_info_ = false;
    std::string error_;
    std::string hint_;
    std::string closed_note_;
    std::string nickname_;
    azl::Mac console_mac_{};
    azl::Mac room_mac_{};
    setup::NetInfo net_;
    azl::room::RoomInfo info_;
};

// One of the three steps of a join (a check mark, a cross, moving dots or a grey dot) with its text, at `yc` on the screen being drawn.
// `index` is 0..2; the text is drawn at x + 18.
void DrawProbeStep(const RoomProbe& probe, float x, float yc, int index, const std::string& text);

} // namespace pages
