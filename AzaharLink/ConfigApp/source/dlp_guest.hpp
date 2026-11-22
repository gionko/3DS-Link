// The worker and the reconciliation of the Download Play children 3DS Link prepares (1.1.4, piece APP-WORKER: 6-APP-boot9.md 4.7 and 6, its critic
// 6-APP-boot9-CRITICO.md 4 and 6 steps 5-6, K9 of 7-CONTRATTO.md, the user's decisions of REGOLE-IMPLEMENTAZIONE.md, and spec 9 of 2026-11-17:
// boot9.bin required, one hidden child per game the user switches on in Download Play > Enable games, a game gone or updated -> removed or "to update").
//
// Nothing of the children works outside the Download Play page (spec 9 point 6):
//   - the Download Play page: once when it opens, and only when nothing else works on the same things (the scan is over, no copy of the plugin is
//     waiting, nobody holds the delivery module's context, no run), the records are looked at against the games there are now (StartGamesLook with
//     `quiet`: LookAround in the thread of the look - the scan's games, AM, the records, children.cache). The quiet part of the Plan (a removal, a
//     repair, a look at content 2: seconds) runs then behind a small page. Nothing is asked: a child is prepared when the user switches its game on in
//     Download Play > Enable games (StartGamesLook with `wanted`: the plan of that game, in the thread of the look).
//   - Download Play > Enable games: "has it a child?" is read with the cache of the titles (spec 9 point 5, 2026-11-17 evening after the console test: no
//     reading of the background, no work on the thread of the screen). When Games opens, a thread of its own (StartGamesLook) looks at the games
//     (AM, the records, children.cache) while the page shows "Checking your games for Download Play" with its bar, the way Enable Titles shows
//     "Reading your titles"; the games children.cache knows nothing of at their versions (ToRead) are then read by the worker, one Probe a game, the
//     bar going on; then the list. Once a game: the next time only the games installed or updated since. A failure that is the game's is remembered
//     (FailureKept) with boot9.bin's stamp.
//
// The worker is ONE thread, made like dlp_prepare.cpp's (core -2, the default core of the app, never the Old 3DS's core 1; a little less important than
// the screen), with its big buffers on the heap. It runs the jobs of dlp_guest_run.cpp one after the other on the console (AM, the source of
// dlp_guest_source.cpp, the plugin's copies handed to the CopyWorker, the one thread that writes AzaharLink.3gx.part). Every step is one line of
// 3DSLink-dlp-guests.log in the app's folder, with its Result: never a key, never a byte of a game.
#pragma once

#include <3ds.h>

#include <atomic>
#include <string>
#include <vector>

#include "dlp_guest_reconcile.hpp"
#include "dlp_guest_run.hpp"
#include "titles.hpp"

namespace dlpguest {

/// boot9.bin is there and is a boot9 with its keys (setup::ClassifyBoot9File is not NoKeys): without it nothing new is prepared, and the app says so.
bool KeysReady();
/// boot9.bin's stamp: 0 without its keys, otherwise a hash of the file (another boot9.bin, another stamp). It reads the file: never once a frame.
u32 KeysStamp();

// (Without boot9.bin nothing is said at the start: Download Play > Enable games says it, 6-APP-boot9.md 6.)
/// Nobody else works on the same things now: the scan is over, no copy of the plugin is waiting, nobody holds the delivery module's context, no run.
bool CanLook();

/// A run of the worker as the run page shows it.
struct RunView {
    int index = 0, count = 0; ///< the job under way (0-based) and how many there are
    u64 game = 0;
    Act act = Act::Nothing;
    double fraction = 0.0;
    int seconds_left = -1;    ///< -1: not known yet
    bool stopping = false;    ///< it stops: after the current game (StopAfterCurrent), or now (StopNow: the job under way is given up cleanly)
};
struct JobEnd {
    Planned job;
    int result = 0; ///< 0 or a kErr*
    JobNote note;
};
/// Starts a run of `jobs` (main thread): 0, kErrNoMemory (no room for the buffers or the thread), kErrCancel (a run is under way already).
int StartRun(const std::vector<Planned>& jobs);
bool Running();
RunView View();
/// B on the reading of Enable games: the run ends after the job under way (a look at a game takes a few milliseconds).
void StopAfterCurrent();
/// (the build 2h, U2) B on the run page: the run STOPS - the job under way is given up where it may be, cleanly (the composition and the stream stop,
/// AM's install is cancelled, the record goes back as it was: a guest is never left half made; past AM's Finish nothing stops, the child is
/// finished), then the run ends. The page stays, with its bar, until the thread is over (HOME stays off until then).
void StopNow();
/// The run is over: its results, once (the thread is joined). False while it runs, or when there was none.
bool TakeEnds(std::vector<JobEnd>* ends);
/// The app ends (main.cpp): the job under way stops where it is (it leaves nothing half done) and the thread is joined.
void Shutdown();

/// The games' names (the scan's, or the 16 digits of the id).
std::string GameName(u64 game);

/// A game of Download Play > Enable games (spec 9 point 4).
struct GameView {
    u64 game = 0;
    Shown shown = Shown::Hidden;
    int error = 0;        ///< (NotRead) why the last look failed (children.cache)
    int id_taken = 0;     ///< (the build 2h) the kErr* of an id of its guest that is somebody else's (dlp_guest_ids.hpp, from ids.cache): Enable is not offered
    u32 child_bytes = 0;  ///< the child's CIA when children.cache knows it (the space it will take)
    bool card = false;
    u16 game_version = 0, update_version = 0; ///< the versions it was looked at (A on "not read" reads it again at them)
};
/// The thread of the look of Download Play > Enable games only (never once a frame: AM, the records, children.cache): the games given (the scan's and the
/// card's) and the records whose game is not among them, each as its row shows it (ShownOf). The Hidden ones are left out. `done` counts the games.
std::vector<GameView> LookAtGames(const std::vector<const titles::Game*>& games, std::atomic<int>* done = nullptr, const IdCensus* census = nullptr);

/// A child of the page of the children, with what it takes on the SD card.
struct ChildRow {
    Guest guest;
    std::string name;
    Presence at = Presence::Unknown;
    u64 title_bytes = 0;  ///< AM_GetTitleInfo: 0 when the record is there and the title is not
    u64 plugin_bytes = 0; ///< the plugin's copy in its folder
    u64 block_bytes = 0;  ///< the block the plugin reads
    bool owned = true;    ///< the record has its slot of the registry (IsOwnedGuestTid): only then does the app remove it
};
std::vector<ChildRow> InstalledChildren();

/// What the look of Download Play > Enable games found, in its own thread (spec 9 point 5): the rows, the children with their space, the games to read
/// (no record, children.cache knows nothing of them at their versions: one Probe each), and the game in the slot when it is not one of the scan's.
struct GamesLook {
    std::vector<GameView> views;
    std::vector<ChildRow> children;
    std::vector<Planned> to_read;
    std::vector<Planned> no_keys; ///< games whose reading stopped at the keys this session (no boot9.bin, no movable.sed): not read again by themselves
    int no_keys_error = 0;        ///< (no_keys) why, the first one's: until boot9.bin or the scan changes (spec 9 point 5); A on their row reads them
    bool has_card = false;
    titles::Game card;
    Plan quiet; ///< (a look of the Download Play page, `quiet`) the quiet work to run now: a child whose game is gone, a repair
    Planned wanted;          ///< (a look for `wanted`) what the reconciliation would do with that game switched on: Prepare, Adopt, Refuse... Nothing
    bool enable_off = false; ///< (a look for `wanted`) its folder of Enable Titles is empty (the question "switched on in Enable Titles too?")
};
/// Main thread: starts the look in a thread of its own (lower than the screen, the app's core). False when it cannot start (no memory, a look or a
/// run under way, the scan not over). The caller holds the scan's games (ui::g.module_holds) until it takes the look: no new scan under the thread.
/// `quiet`: the look of the Download Play page instead (the records against the games, the quiet Plan only: no rows, no games to read).
/// `wanted`: A on a game of Games that is off - the plan of THAT game switched on (what PlanFor did on the thread of the screen), the slot, its folder.
bool StartGamesLook(bool quiet = false, u64 wanted = 0);
/// The look started is over (TakeGamesLook then gives it).
bool GamesLookDone();
/// How far the look is: the games looked at and how many there are (0 of 0 before it knows).
void GamesLookProgress(int* done, int* total);
/// Main thread: the look that ended (its thread joined). False when there is none.
bool TakeGamesLook(GamesLook* out);
/// Main thread: a page that goes while its look runs (never on its way: B waits for the look) waits for its end - a moment - and lets it go. The
/// thread reads the scan's games, which the page's hold kept: it never outlives the hold.
void DropGamesLook();
/// How many runs of the worker have ended (TakeEnds): a page that shows what they did looks again when it changes.
int RunsEnded();

} // namespace dlpguest
