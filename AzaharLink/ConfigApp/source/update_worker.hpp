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

// The lists of the page "Public lobbies" (public_lobbies.hpp): the job that fetches them says how it went here, apart from the update's Status.
struct ListsStatus {
    enum class Stage { Idle, Running, Done, Failed };
    Stage stage = Stage::Idle;
    NetResult net = NetResult::Ok;
    bool list_changed = false;   // a new list waits in <list>.new (the page applies it: public_lobbies.hpp, Remember)
    bool titles_changed = false; // new titles wait in <titles>.new (the list said they changed)
};

class Worker {
public:
    static Worker& Get();

    // Starts asking GitHub. False: a job is still running (its result is what Read() will say).
    bool StartCheck();
    // Starts downloading and installing `version`. `path` is the file to replace (Target::File3dsx). False: a job is still running.
    bool StartInstall(Target target, const std::string& version, const std::string& path);
    Status Read() const;
    // Fetches the list of public rooms and, when its "#titles" line differs from the one of the file at `titles_path`, the titles too. What came
    // is never written over the files the page uses: a whole list that differs from `list_path` goes to `list_path`.new, the titles to
    // `titles_path`.new, and the page applies them on its own thread (so a page that closed before the end loses nothing: the next one applies
    // them). False: a job is still running (try again later).
    bool StartLists(const std::string& list_path, const std::string& titles_path);
    ListsStatus ReadLists() const;
    // The job of the lists is running (its .new files are being written).
    bool ListsRunning() const;
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
    bool Spawn();
    Impl* impl_;
};

} // namespace update
