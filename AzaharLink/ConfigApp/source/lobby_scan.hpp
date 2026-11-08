// The thread that asks the public rooms (lobby_probe.cpp does the asking): one pass at a time, a little below the thread that draws, the way
// the copies of the plugin and the update have theirs. The page starts a pass, reads what came so far once a frame, and never waits for it.
#pragma once

#include <3ds.h>

#include <vector>

#include "lobby_probe.hpp"

namespace lobbies {

class Scanner {
public:
    static Scanner& Get();

    // Starts a pass over `targets`. False when the last one has not let go yet (it was asked to stop: try again in a moment).
    bool Start(const std::vector<Target>& targets);
    // A pass is running.
    bool running() const;
    // A number that changes every time a result does: the page copies the results only then.
    u32 generation() const;
    // The results so far, in the order of the targets of the pass that runs or ran last.
    void Read(std::vector<Probe>& out) const;
    // The pass stops at its next step (a page that goes away calls it; it does not wait).
    void Stop();
    // The end of the app, or the sockets going away for Titles Delivery: Stop, and wait at most `timeout_ms`. True when no thread is left.
    bool Shutdown(u64 timeout_ms);

    struct Impl;

private:
    Scanner();
    Impl* impl_;
};

} // namespace lobbies
