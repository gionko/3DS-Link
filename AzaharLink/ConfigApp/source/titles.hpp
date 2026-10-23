// The titles installed on this console: the scan of the titles (the delivery module's own scan and cache, taken as they are), and the name and
// icon of every title, read from its SMDH. The scan runs in a worker thread and counts what it has done, so the screens can draw a
// progress bar and the app never waits for it. The two caches live in the app's data folder (paths.hpp, setup::CurrentPaths()).
#pragma once

#include <3ds.h>

#include <memory>
#include <vector>

#include "games.hpp"
#include "status.hpp"

namespace titles {

struct Game : setup::GameInfo {
    bool card = false; // on the inserted game card, not installed on the SD card
};

// How the scan went, for the message of an empty list.
struct ScanReport {
    int applications = 0;   // games (application titles) installed on the SD card
    int recognised = 0;     // ... of which with a readable name and icon
    bool have_keys = false; // the delivery module's key code found the keys
    setup::TitlesHealth health; // the titles that are of no use (the Installation check's row; see DeleteUnusable)
};

class Scanner {
public:
    Scanner();
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // Whether the catalog cache file is on the card: without it the scan is the long first one.
    static bool CacheExists();

    // Starts the worker (once). `language` is the console's language (CFG_Language): the names are read in it.
    void Start(int language);
    bool started() const;
    // The scan has finished and games() can be read.
    bool done() const;
    // What the worker has done so far, in "titles": the names read (TitleShare's part) and then the games (the bar).
    int progress_done() const;
    int progress_total() const;
    // The same, step by step, for the words next to the bar (setup::ScanProgressText).
    setup::ScanProgress progress() const;
    // The games installed on the SD card that have a readable name and icon, by name. Only when done().
    const std::vector<std::unique_ptr<Game>>& games() const;
    // Only when done().
    ScanReport report() const;

    // Waits for the worker, at most `timeout_ms`: called when the app ends. False when it was still running.
    bool Finish(u64 timeout_ms);

    // The delivery module's own state, made by this scan (its context with the keys and the catalog of the installed titles): Titles
    // Delivery sends and receives with it, nothing is read a second time. nullptr until the scan is done (and when it has none).
    void* module_context() const;
    void* module_catalog() const;

    struct Impl; // (what the worker shares with the screens: in titles.cpp)

private:
    std::unique_ptr<Impl> impl_;
};

// The one scanner of the app (made on first use, destroyed by Shutdown()).
Scanner& Scan();
void Shutdown();
// A number that changes every time a new scanner is made: a page that holds titles of a scan (pointers into it) knows it must let them go.
int ScanGeneration();

// The title on the inserted game card, if there is a card and its title can be read. Quick: asked every time the Enable titles page
// opens, since a card can be put in at any time.
bool ReadCardGame(int language, Game& out);

// Deletes the titles the scan found of no use (ScanReport::health): what the card holds for them and, for a title the system cannot describe or a
// DLC with contents missing, the ticket; an installation that was never finished loses only its half (its ticket may belong to a title that works).
// Nothing else is touched. Returns how many went; `failed` gets how many the system refused. The caller reads the titles again afterwards
// (the list and the Installation check change).
int DeleteUnusable(const std::vector<setup::UnusableTitle>& titles, int* failed);

} // namespace titles
