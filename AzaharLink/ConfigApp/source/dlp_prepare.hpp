// The one-time work of 3DS Link Delivery, on the console: looking at the files (is child.cia good, is 3dslink.cia the app's own CIA of this version), and the
// worker that builds child.cia with the composer (ConfigApp/dlpc, contract dlpc.h) from 3DS Link's own CIA and the plugin that is a resource of the app. The decisions that need no console are
// in dlp_prepare_logic.hpp (tested on a PC); the words of the screens in dlp_text.hpp; the page that shows it is pages_dlp.cpp.
//
// The work runs in a thread of its own, a little less important than the screen (the page keeps drawing, reads the progress and the time, and B asks it to stop:
// the composer looks at that every 64 KB). A marker file is written before it begins and removed when it ends, however it ends: a run that died in the middle is
// found by the marker next time. The composer writes child.cia as "<path>.part" and puts it in place only when it is whole and checked, so nothing is ever
// left half done; the copy of the app's CIA that is made from the installed title (the module's virtual CIA, titleshare_host/delivery.h) goes the same way.
#pragma once

#include <3ds.h>

#include <atomic>
#include <string>

#include "dlp_prepare_logic.hpp"

namespace dlp {

// What the page hands over. Everything is copied: the worker holds no pointer into the page. The plugin is not part of it: the composer reads the plugin that is a resource of
// the app (plugin_resource.hpp), never a file on the card.
struct PrepareJob {
    std::string out_path;      // child.cia
    std::string app_cia_path;  // dlp/3dslink.cia: what the composer reads, and where the copy made from the installed title is put
    std::string marker_path;   // dlp/child.preparing
    std::string version_text;  // "1.0.0"
    unsigned version16 = 0;
    bool use_file = false;     // app_cia_path is a good CIA of this version: compose from it
    bool allow_title = false;  // the installed title may be read (the scan is done and has the keys): the copy is made from it when there is no good file, or the file fails
    u64 app_bytes = 0;         // how big the app's CIA is (the file's size; 0 = not known: the usual size is taken until the installed title says)
    int attempt = 1;           // how many runs in a row have begun: written in the marker
};

enum class PrepareState { Idle, Running, Done, Failed, Cancelled };

struct PrepareView {
    int stage = kStageCopy;   // 0 (the app's own copy is being read) or a DLPC_STAGE_*
    double fraction = 0.0;    // 0..0.99 until it is over
    int seconds_left = -1;
    int plan_seconds = 0;     // how long the whole work takes, for the line "about N s": the plan's until the pace is known, the pace's after (0 = not known)
    bool measured = false;    // the pace of the work itself has been seen (a seventh of it, two seconds): plan_seconds is no longer a guess
};

class Preparer {
public:
    Preparer();
    ~Preparer(); // asks the work to stop and waits for it (a few tenths of a second)
    Preparer(const Preparer&) = delete;
    Preparer& operator=(const Preparer&) = delete;

    // 0 = the work has begun. Otherwise a kPrep* result and nothing runs (no installer in this build, no room in the heap, no thread).
    int Start(const PrepareJob& job);
    // Asks it to stop; the work ends within one slice (64 KB) and leaves nothing behind.
    void Cancel();
    // The thread has made its last call: state() and result() say how it went, and the preparer can be destroyed without a wait.
    bool Finished() const {
        return finished_.load();
    }
    PrepareState state() const {
        return static_cast<PrepareState>(state_.load());
    }
    // The bar, the stage, the time left and the time of the whole work. The preparer keeps its own clock (it begins with the plan and begins again when the plan is
    // made again), so the page asks with nothing but the moment.
    PrepareView View() const;
    // How it ended (Failed): a dlpc_result or a kPrep* result, the composer's own sentence or the module's words when there is one ("" = none), and, for a failure to
    // read the installed title, why (a DL_CIA_* of titleshare_host/delivery.h, 0 = not that).
    int result() const {
        return result_.load();
    }
    std::string detail() const;
    int title_error() const {
        return title_error_.load();
    }
    // The composer refused the file (dlp/3dslink.cia) and the copy from the installed title was tried instead: if that failed too, the file is what to name.
    bool file_refused() const {
        return file_refused_.load();
    }

    struct Hooks; // the callbacks of the composer and the entry of the thread (dlp_prepare.cpp)

private:
    friend struct Hooks;
    void Run();
    int Execute();
    int Compose(const std::string& app_path);
    int CopyFromTitle();
    void SetPlan(bool copy, u64 app_bytes);
    void RestartPlan(bool copy, u64 app_bytes);
    void SetProgress(int stage, u64 done, u64 total);
    void SetDetail(const std::string& text);
    void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    PrepareJob job_;
    Thread thread_ = nullptr;
    std::atomic<int> state_{static_cast<int>(PrepareState::Idle)};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> result_{0};
    std::atomic<int> title_error_{0};
    std::atomic<bool> file_refused_{false};
    LightLock lock_; // guards plan_, stage_, done_, total_, pace_ms_ and detail_
    Plan plan_;
    bool plan_copy_ = false;
    int stage_ = kStageCopy;
    u64 done_ = 0;
    u64 total_ = 0;
    u64 plugin_bytes_ = 0;
    u64 pace_ms_ = 0; // when the estimate began (ui::NowMs): the clock of the time left
    std::string detail_;
};

// What the page holds while the work runs: HOME is off and sleep is refused (a half-written file is made whole or removed by the composer, not by luck: a press
// of HOME or a closed lid in the middle must not cut the work), and the delivery module's context is not destroyed under it (the work may read the installed 3DS
// Link with the keys that context holds, and a new scan of the titles would free them: main.cpp waits while this is held). Take() and Give() may be called twice.
class WorkHolds {
public:
    ~WorkHolds() {
        Give();
    }
    void Take();
    void Give();
    bool held() const {
        return held_;
    }

private:
    bool held_ = false;
};

// ---------------------------------------------------------------------------------------------
// The files, looked at now (the page asks before it decides what to do)
// ---------------------------------------------------------------------------------------------

struct FileFacts {
    bool exists = false;
    u64 size = 0;
    CiaFacts cia;
};
FileFacts ReadCiaFile(const std::string& path);
Verdict CheckChildFile(const std::string& path, unsigned version16);
// `found_version`: the version the file's TMD says (0 = no TMD could be read), for the page that has to name what is wrong with the file.
Verdict CheckAppCiaFile(const std::string& path, unsigned version16, u64* size, unsigned* found_version = nullptr);

// The version of the model that is a resource of this build (the composer's dlpc_model_version): 0 = the placeholder, this build has no installer. The composer
// hashes the whole model to say it, so it is asked once and kept (the model is a constant of the build). Call it from the screen's thread.
unsigned ModelVersion();

// How many runs in a row began and did not end (the marker is there): 0 = none, the last run ended.
int DiedRuns(const std::string& marker_path);

} // namespace dlp
