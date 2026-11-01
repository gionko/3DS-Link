// The thread of the update: asks GitHub, downloads the file and installs it, while the pages keep drawing. One job at a time, each on a thread of its own, a little below the thread
// that draws. The decisions are in update_logic.hpp and the HTTPS in update_http.cpp (both tested on a PC); what is here needs the console: the thread, AM and the card.
#pragma once

#include <3ds.h>

#include <string>

#include "plugin_install.hpp"
#include "update_logic.hpp"

namespace update {

enum class Stage {
    Idle,
    Checking,    // asking GitHub
    Checked,     // GitHub answered: `latest` is the version that is out (it may be this one)
    Downloading, // `done` of `total` bytes
    Installing,
    Installed,
    Failed       // `failure` says what; nothing was changed on the console
};

enum class Failure { None, Net, WrongFile, Am, File, Busy };

struct Status {
    Stage stage = Stage::Idle;
    Failure failure = Failure::None;
    NetResult net = NetResult::Ok;                     // Failure::Net (Cancelled too: the app asked it to stop)
    Result am_rc = 0;                                  // Failure::Am
    setup::WriteResult write = setup::WriteResult::Ok; // Failure::File
    std::string latest;                                // the version that is out
    std::string whats_new;                             // the sentence of its CHANGELOG ("" when it could not be had)
    u32 done = 0, total = 0;
};

// The words for a Failed status.
std::string FailureText(const Status& status);

// The year the console's clock says.
int ConsoleYear();

class Worker {
public:
    static Worker& Get();

    // Starts asking GitHub. False: a job is still running (its result is what Read() will say).
    bool StartCheck();
    // Starts downloading and installing `version`. `path` is the file to replace (Target::File3dsx). False: a job is still running.
    bool StartInstall(Target target, const std::string& version, const std::string& path);
    Status Read() const;
    // A request stops at its next step. (An install that has begun is never cut: AM takes the file whole or not at all.)
    void Cancel();
    // Cancel, and wait for the thread at most `timeout_ms`: before the sockets are closed under it. True when no thread is left.
    bool Stop(u64 timeout_ms);
    // The end of the app: Stop, and what the requests opened once.
    void Shutdown();

    struct Impl;

private:
    Worker();
    bool Begin(Stage first);
    Impl* impl_;
};

} // namespace update
