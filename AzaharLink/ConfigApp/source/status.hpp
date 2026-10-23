// The rules behind what the app tells about the installation and the console: which files must be on the SD card and what to
// do when one is not, what the Plugin Loader's state means, and the texts of the scan's progress.
// Plain C++ with no libctru service: the facts are gathered elsewhere (system.cpp) and handed in, so Tests/settings_test runs
// this very file on a PC.
#pragma once

#include <3ds.h>

#include <string>
#include <vector>

#include "plugin_install.hpp"

namespace setup {

// "812 B", "3.5 KB", "1.02 MB".
std::string FormatSize(u64 bytes);

// ---------------------------------------------------------------------------------------------
// The installation check
// ---------------------------------------------------------------------------------------------

// Rosalina's Plugin Loader: on, off, or not known (no such service: an emulator, an old Luma3DS).
enum class LoaderState { Unknown, Enabled, Disabled };

// ---------------------------------------------------------------------------------------------
// The titles that are of no use: the scan of the titles finds them, the Installation check shows them, A deletes them
// ---------------------------------------------------------------------------------------------

struct UnusableTitle {
    enum class Kind {
        Unfinished, // an installation the system still lists as begun and never finished (a receiving that stopped half way on an app older than
                    // 2026-10-19, a power cut while installing): nothing can run it
        Broken,     // an installed title the system cannot describe (AM refuses its information): what is on the card for it cannot be read
        PartialDlc  // a DLC whose ticket grants contents that are not on the card (a receiving that stopped half way): it works for what is there
    };
    Kind kind = Kind::Unfinished;
    u64 tid = 0;
    std::string name;  // the name of the game it belongs to; "" when nothing can name it (the row shows the title id)
    int installed = 0; // PartialDlc: the contents on the card
    int missing = 0;   // PartialDlc: the contents the ticket grants that are not there
};

struct TitlesHealth {
    bool checked = false; // the scan has looked (until then the check has no row for the titles)
    int titles = 0;       // what the system lists on the SD card (games, updates, DLC, system titles)
    std::vector<UnusableTitle> unusable;
    int Count(UnusableTitle::Kind kind) const;
};
// One line for a title of no use ("- 0004000E00179500: begun, never finished").
std::string UnusableTitleLine(const UnusableTitle& title);

// What the app found on the SD card.
struct InstallFacts {
    std::string plugin_path;       // the plugin file of 3DS Link (paths.plugin_file): sdmc:/3ds/3DSLink/AzaharLink.3gx with a CIA install
    PluginCard plugin_card = PluginCard::Missing; // what the card holds there, byte for byte against the plugin inside the app (plugin_install.hpp)
    u64 plugin_size = 0;           // the file's size (0 when there is none)
    u32 plugin_version = 0;        // the version word of its header (when it has one)
    bool embedded_ok = true;       // the app carries a plugin it can put on the card (false: a build with no plugin inside)
    bool plugin_tried = false;     // the app tried to put its plugin on the card since it started ...
    WriteResult plugin_write = WriteResult::Ok; // ... and how that went
    bool plugins_dir = false;      // sd:/luma/plugins exists
    bool default_plugin = false;   // sd:/luma/plugins/default.3gx exists
    bool boot9 = false;            // a boot9.bin the app can use (next to the app, in GodMode9's folder, or where the user pointed to)
    std::string boot9_path;        // ... and where it is
    bool sd_writable = false;      // a test file could be written and removed
    std::string app_dir;           // the folder the app and its files live in (where boot9.bin and the plugin are looked for first)
    int seed = 0;                  // the database of the seeds, read from the console's own memory: 0 not asked, 1 readable, 2 not readable
    u64 seed_size = 0;             // ... its size when it is readable
    u32 seed_error = 0;            // ... the system's answer when it is not
    LoaderState loader = LoaderState::Unknown; // Rosalina's Plugin Loader (Unknown: the row is left out)
    u32 luma = 0;                  // the Luma3DS version word (svcGetSystemInfo 0x10000: major << 24 | minor << 16 | build << 8), 0 = not known (an emulator)
    TitlesHealth titles;           // what the scan of the titles found of no use (checked = false until it has run: no row)
};

enum class CheckLevel {
    Problem, // something that has to be put right: counted, red
    Warning, // something that is missing but 3DS Link works without it (boot9.bin: only sending titles needs it): counted, yellow
    Ok,
    Optional // information: never counted
};

// 0 = the most urgent: the order of the rows of the check (a problem first, then a warning, then what is fine).
int SeverityRank(CheckLevel level);

// What A does on a row of the installation check (the page runs it; this file only says which).
enum class CheckAction {
    None,
    RestorePlugin,    // put the plugin that is inside 3DS Link on the card (where 3DS Link keeps it), replacing what is there
    CreatePluginsDir, // make sdmc:/luma/plugins
    FindBoot9,        // search the card, choose a folder, or how to get it
    EnableLoader,     // switch Rosalina's Plugin Loader on
    CleanTitles       // delete the titles that are of no use (after a question that lists them)
};

struct CheckItem {
    std::string label;
    std::string value; // short: what the row shows
    CheckLevel level = CheckLevel::Ok;
    std::string note_title;
    std::string note; // what it is, and what to do when it is missing
    CheckAction action = CheckAction::None;
    std::string short_problem; // a few words for the main page ("boot9.bin missing"); only for a Problem or a Warning
};

struct InstallReport {
    std::vector<CheckItem> items;
    int problems = 0; // red
    int warnings = 0; // yellow
};

// `app_version` is the version this app says (kVersion): a plugin file of another version is a problem too.
InstallReport EvaluateInstall(const InstallFacts& facts, const std::string& app_version);
// The order the rows of the check are shown in: the indexes of report.items, most urgent first (a problem, then a warning, then what is
// fine, then the information); the order of the items inside one group is kept.
std::vector<std::size_t> InstallRowOrder(const InstallReport& report);
// "All OK", "1 problem", "2 problems", "1 warning", "2 warnings", "1 problem, 1 warning"...
std::string InstallSummary(const InstallReport& report);
// The line of the main page: "All OK", or what is not right in a few words and where to go ("boot9.bin missing - A on Installation check").
// `compact` is the same with the things counted ("2 problems - A on Installation check"), for when the full one does not fit.
struct InstallLine {
    std::string full;
    std::string compact;
};
InstallLine InstallLineText(const InstallReport& report);

// Writes a small file in `dir` and removes it: can this card be written?
bool ProbeWritable(const char* dir);

// ---------------------------------------------------------------------------------------------
// The Plugin Loader of Luma3DS (Rosalina's "Plugin Loader: Enabled")
// ---------------------------------------------------------------------------------------------

const char* LoaderStateText(LoaderState state);

// What the version of Luma3DS means for the plugin: the Plugin Loader exists in the official Luma3DS from 13.0 (2023-07-20); 3DS Link was tried on 13.3.3 (and
// the plugin is built for it), so from 13.3 it is "tested", 13.0 to 13.2 "maybe" (the loader exists, never tried), below 13.0 "not work" (no loader: no game can load the
// plugin; Titles Delivery does not need it). An unofficial Luma3DS with an older number that answers the Plugin Loader is "maybe". Unknown: no row.
enum class LumaSupport { Unknown, NotWork, Maybe, Tested };
LumaSupport ClassifyLuma(u32 version_word, LoaderState loader);
std::string LumaVersionText(u32 version_word); // "13.3.3"; "" when it is not known
// The question "enable it?" is asked only when the loader is known to be off and 3DS Link is on (with 3DS Link off no plugin is
// needed): an unknown state (no such service: an emulator, an old Luma3DS) never asks.
bool ShouldOfferLoader(LoaderState state, bool link_on);
// What to tell after the answer Yes: the result of the request (0 = fine) and the state read back afterwards. `good` says whether
// the message is good news.
std::string LoaderEnableMessage(u32 set_result, LoaderState after, bool* good);

// ---------------------------------------------------------------------------------------------
// The scan's progress
// ---------------------------------------------------------------------------------------------

// How far the scan of the titles is. It has three steps, each counted on its own: the names (the delivery module's part, over every title the
// system lists), the games (the name and icon of every application) and the look at the titles' health; then it is finished.
struct ScanProgress {
    int phase = 0;  // 0 the names, 1 the games, 2 the titles' health, 3 finished
    int done = 0;   // of the step
    int total = 0;  // of the step
    int titles = 0; // the titles the system lists (known from the start)
    int games = 0;  // the games recognised (known at the end)
};
// "names: 123 of 221 titles", "games: 30 of 172", "checking the titles", "221 titles, 172 games". A count is never shown above its total.
std::string ScanProgressText(const ScanProgress& progress);
// 0.0 .. 1.0 (a scan that has not counted its titles yet has done nothing: the bar must not flash full at the start).
float ProgressFraction(int done, int total);

} // namespace setup
