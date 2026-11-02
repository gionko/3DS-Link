// The download of the public room list: GET <lobby url>/lobby over the console's http service (httpc, the same one the app's CIA is allowed
// by DlpLink/rsf/3dslink.rsf). It runs on a helper thread so the screen keeps drawing; the page polls it once per frame. The thread owns a
// share of the state, so a page that is closed (or gives up after the timeout) can leave a stuck request behind without touching freed memory.
#pragma once

#include <3ds.h>

#include <atomic>
#include <memory>
#include <string>

namespace lobby {

// "88.198.47.46:5000", "http://host:5000/" or "https://host/api" -> "http://88.198.47.46:5000/lobby" (the scheme is added, a trailing / is
// dropped, /lobby is appended). Empty when `base` is empty.
std::string LobbyUrl(const std::string& base);

class Fetch {
public:
    enum class Phase { Idle, Loading, Done, Failed };

    // Starts a download (a download still running is left to finish by itself and forgotten).
    void Start(const std::string& base_url);
    // Forgets the download in progress: its result is thrown away when it ends.
    void Abandon();

    Phase phase() const;
    // Valid when phase() is Done (the body) or Failed (the reason in words).
    const std::string& body() const;
    const std::string& error() const;
    // Milliseconds since Start (for the timeout the page applies).
    u64 elapsed_ms() const;

private:
    struct State {
        std::atomic<int> phase{0}; // 0 loading, 1 done, 2 failed (written last: the strings are ready when it changes)
        std::atomic<bool> abandoned{false};
        std::string url;
        std::string body;
        std::string error;
        u64 started_ms = 0;
    };
    std::shared_ptr<State> state_;
};

} // namespace lobby
