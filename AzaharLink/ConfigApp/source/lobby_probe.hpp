// Asking the public rooms how they are, without joining any (ENet and the room's packets, nothing of the console: Tests/lobby_probe_test.cpp
// runs it against real azahar-room containers).
//
// A room is asked in three steps on one ENet connection: the connection itself is the ping; a game-info packet makes the room send its
// information (name, slots, members and what they play) to every peer it has, ours too; and a join request with a protocol version no room
// speaks makes it say "full", "wrong password" or "another version" (with its own version) and never take us in. No nickname and no MAC of the
// console are sent. A room that does not answer the connection is offline.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "public_lobbies.hpp"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace lobbies {

struct Target {
    std::string host;
    std::uint16_t port = 0;
    std::string secret; // the password we have for it ("" = none): a room that wants one is asked whether this is it
};

struct PassOptions {
    int at_once = 8;                  // rooms asked at the same time (one socket for all)
    std::uint32_t connect_ms = 2500;  // no connection by then: offline
    std::uint32_t answer_ms = 1000;   // after the connection (or after the second ask, with our password), how long the room has to answer
    const std::atomic<bool>* stop = nullptr; // set by another thread: no new room is started (a name lookup can take seconds)
};

class ProbePass {
public:
    ProbePass(std::vector<Target> targets, PassOptions options);
    ~ProbePass();
    ProbePass(const ProbePass&) = delete;
    ProbePass& operator=(const ProbePass&) = delete;

    // The socket. False when there is none (no WiFi): nothing can be asked.
    bool Begin();
    // Serves the socket for at most `wait_ms` and starts the next rooms. False when every room has its answer.
    bool Step(std::uint32_t wait_ms);
    // The rooms whose result changed since the last call.
    std::vector<std::size_t> TakeChanged();
    const Probe& result(std::size_t i) const {
        return slots_[i].probe;
    }
    std::size_t size() const {
        return slots_.size();
    }

private:
    struct Slot {
        Target target;
        Probe probe;
        ENetPeer* peer = nullptr;
        std::uint64_t started_ms = 0, connected_ms = 0, asked_ms = 0;
        bool answered = false;     // the join request had its answer
        bool second_ask = false;   // the room wants a password and we have one: asked again with it
        bool joined = false;       // a room that took us in after all (it must not happen): we leave at once
        bool changed = false;
        bool done = false;
    };

    void StartNext(std::uint64_t now);
    void OnConnect(Slot& s, std::uint64_t now);
    void OnPacket(Slot& s, const std::uint8_t* data, std::size_t size);
    void Finish(Slot& s, State state);

    std::vector<Slot> slots_;
    PassOptions options_;
    ENetHost* host_ = nullptr;
    bool enet_ready_ = false;
    std::size_t next_ = 0;
    int asking_ = 0;
};

} // namespace lobbies
