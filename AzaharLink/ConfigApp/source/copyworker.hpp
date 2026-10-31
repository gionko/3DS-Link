// The thread that puts the plugin into the folders of the titles and takes it out again. A copy of about 1 MB on the SD card takes a second or
// more (more when the card is full), and the page of the titles must not stand still meanwhile: the user goes on through the list and switches
// other titles on, and their copies wait in a queue, one at a time (the card does one thing at a time anyway).
//
// The worker belongs to the app, not to the page: a page that is closed while copies are still being made leaves them to it, and the app
// waits for it before it ends (WaitIdle).
#pragma once

#include <3ds.h>

#include <string>

#include "games.hpp"

namespace setup {

enum class CopyKind { Enable, Update, Disable }; // Update: the copy is made again because the plugin file was replaced by a newer one

struct CopyDone {
    u64 tid = 0;
    CopyKind kind = CopyKind::Enable;
    EnableResult enable_result = EnableResult::Ok; // Enable and Update
    bool disable_ok = false;                       // Disable
};

class CopyWorker {
public:
    // The one worker of the app; its thread is started with the first job.
    static CopyWorker& Get();

    // Queues a job. `source` is the plugin file (Enable and Update).
    void Submit(u64 tid, CopyKind kind, const std::string& source);
    // Queues a look at the copy of the plugin in a game's folder: if the folder holds ours and it differs from `source` it is made again, else nothing is
    // written. Done quietly: no finished job is reported, only the number of copies that were made (TakeRefreshedCount).
    void SubmitRefresh(u64 tid, const std::string& source);
    // How many copies the quiet looks made since the last call.
    int TakeRefreshedCount();
    // Drops the quiet looks that are still waiting in the queue (the one that runs is finished: a copy is never cut in the middle). The user leaves in the
    // middle of a pass over many games: nothing more is made, and 3DSLink.cfg does not remember the plugin, so the next start makes the pass again.
    // Returns how many were dropped. Copies the user asked for (enable, disable) are left.
    int CancelRefreshes();
    // Is a job for this title waiting or running? `kind` says which.
    bool Pending(u64 tid, CopyKind* kind) const;
    int PendingCount() const;
    // One finished job (the oldest first). False when there is none.
    bool TakeDone(CopyDone& out);
    // Waits until nothing is queued or running (at most `timeout_ms`). True when idle.
    bool WaitIdle(u64 timeout_ms);
    // Ends the thread once the queue is empty.
    void Stop();

    struct Impl;

private:
    CopyWorker();
    void Submit(u64 tid, CopyKind kind, const std::string& source, bool refresh);
    Impl* impl_;
};

} // namespace setup
