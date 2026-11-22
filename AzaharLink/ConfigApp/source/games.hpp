// What the "Enable titles for Azahar" page knows about a title and about the copy of the plugin in its folder: the title id as Luma3DS spells
// it, the name and the icons of a title (the SMDH every title carries), the order of the list, and the files that enabling and
// disabling a title create and remove. Plain C++ and POSIX file calls only (no libctru service), so Tests/settings_test builds
// this very file on a PC against a folder called "sdmc:".
#pragma once

#include <3ds.h>

#include <set>
#include <string>
#include <vector>

namespace setup {

// The plugin keeps this name inside the folder of every game it is enabled for (sd:/luma/plugins/<TID>/AzaharLink.3gx).
constexpr const char* kPluginFileName = "AzaharLink.3gx";

// ---------------------------------------------------------------------------------------------
// Title ids
// ---------------------------------------------------------------------------------------------

// The 16 UPPERCASE hex digits Luma3DS looks for ("000400000016E700").
std::string TidText(u64 tid);
// An application title: the high word is 0x00040000. Updates (0004000E), DLC (0004008C), system titles, applets and DSiWare
// (00048004) are not games to enable.
bool IsApplicationTid(u64 tid);

// A game that was SOLD (a cartridge or an eShop title), as against a homebrew, an emulator or a tool that somebody installed as a CIA: those are
// application titles too (FBI, RetroArch, 3DS Link itself). The page lists the sold games only: the plugin is made for games, and in the folder of
// 3DS Link itself or of another homebrew it does nothing good. What tells them apart is the unique id of the title, (tid >> 8) & 0xFFFFFF: the
// ids Nintendo gave out are in two ranges, and most homebrew is outside them (3DS Link is 0xF83D1, FBI 0xF8001). MOST, not all: a homebrew picks
// the id it likes, and some picked one inside (seen on the consoles of the project: 0x00978, 0x00F98 and 0x01242 in the first range, 0xF7A3E
// above the last id of the second, which is why that range ends close to its last title).
// Where the numbers come from: the list of every 3DS title the public lobbies use (titles.txt, made by tools/build_titles.py of the repository
// 3DS-Link-Public-Lobbies from three public lists; "#titles 2026-11-03", 5217 titles): 4960 ids from 0x00300 to 0x01E46, 257 from 0xF7000 to
// 0xF715F, none anywhere else. Tests/settings_test/games_test.cpp holds every id of that list against these four numbers (sold_catalog.inc).
constexpr u32 kSoldIdFirst = 0x00300;    // cartridges and eShop titles: the first id of the list ...
constexpr u32 kSoldIdLast = 0x01FFF;     // ... and room above its last one (0x01E46)
constexpr u32 kSoldNewIdFirst = 0xF7000; // New 3DS only titles and the Super Nintendo Virtual Console: the first id of the list ...
constexpr u32 kSoldNewIdLast = 0xF71FF;  // ... and a little room above its last one (0xF715F): a homebrew was seen at 0xF7A3E
constexpr u32 UniqueIdOf(u64 tid) {
    return static_cast<u32>(tid >> 8) & 0xFFFFFFu;
}
// An application title (IsApplicationTid) whose unique id is in one of the two ranges. A homebrew that took the id of a sold game passes for
// one, and a game installed under a made-up id does not: the rule reads the id, nothing else. It is what the app asks before it puts the plugin
// in a folder because the user said so: "Enable the game?" of the public lobbies asks it as it is, the switch of Enable Titles asks it with the
// exception below (IsEnableTitle).
bool IsSoldGameTid(u64 tid);

// The Download Play application of the system: a system application (high word 0x00040010) in the NAND, one per console, of the console's region. It is NOT a title of
// Enable Titles any more (1.1.4, spec 9, the user's decision of 2026-11-17: Download Play is a page of 3DS Link, and the app reads nothing of the NAND for it): its six
// ids stay for what still asks about it - the one-time removal of a copy of the plugin an earlier 3DS Link put in its folder (App::QueuePluginCopyRefresh), the games that
// may have a child (IsGuestGame) and the comparison with the guard of the plugin (Tests/policy_check.sh).
// Where they come from: the lists of the system titles of every region at yls8.mtheall.com/ninupdates/titlelist.php (read 2026-11-14: "Download Play application",
// one id per region); the European one is the one seen on the consoles of the project. A console sold in Australia has the European titles.
constexpr u64 kDownloadPlayTids[] = {
    0x0004001000020100ull, // JPN
    0x0004001000021100ull, // USA
    0x0004001000022100ull, // EUR
    0x0004001000026100ull, // CHN
    0x0004001000027100ull, // KOR
    0x0004001000028100ull, // TWN
};
bool IsDownloadPlayTid(u64 tid);

// THE EXCEPTION to that rule, the page's only (found the first time the list was looked at on a console, 2026-11-14): IsSoldGameTid is what it was, and
// "Enable the game?" of the public lobbies still asks it alone.
// The impostors: a homebrew that took the unique id of a sold game, or one inside the ranges, passes the rule, which reads the id and nothing else. The id cannot be
// taken out of the ranges - 0x00F98 is a real Japanese game in the list of the public lobbies - so what tells an impostor is the id AND a second fact the app already
// has: a part of the NAME its SMDH gives, compared without case and without anything that is not an ASCII letter or digit ("Fast PlayCoin - 300 coins NOW" holds
// "playcoin"). The real game under the same id has another name and stays a game. The table holds the titles that were SEEN on the consoles of the project and named
// (402 titles of 60 lists of eight consoles), nothing that was guessed; a fourth title that passes without being in the list of the lobbies, 0x00EC5 with the name of a
// sold game, is not here: nobody knows what it is.
// (The product code would be a second fact too, and the delivery module's catalog has it for every title of the SD card; but no impostor's code was ever written down,
// and a rule on a value nobody saw would be a guess. The name is what the lists of the consoles hold.)
struct Impostor {
    u32 unique_id;         // UniqueIdOf() of the title
    const char* name_part; // lower case ASCII letters and digits: what the name must hold, compared as said above
};
constexpr Impostor kImpostors[] = {
    {0x00F98, "playcoin"},  // "Fast PlayCoin - 300 coins NOW", a coin tool (console 63); the id is a Japanese game's
    {0x01242, "blargsnes"}, // "blargSnes", a Super Nintendo emulator (console D); no title of the list has the id
    {0x00978, "pacintime"}, // "Pac-In-Time", an old game wrapped as a title (console D); no title of the list has the id
};
// Is this title one of the impostors of the table? Its id AND its name: a title with another name, or with no name given, is not.
bool IsImpostor(u64 tid, const std::string& name);

// THE question of the Enable Titles page: may 3DS Link be switched ON for this title, and is it a row whatever its folder holds? A sold game that is not an impostor
// (never the Download Play application of the system: spec 9). `name` is the name the title's SMDH gives (GameInfo::name); with an empty one the id alone decides, as
// IsSoldGameTid does.
bool IsEnableTitle(u64 tid, const std::string& name);
// A game that may have a Download Play child prepared by the app (1.1.4, dlp_guest_decide.hpp): a title the switch may turn on, but not the system's
// Download Play application, and one whose prepared title exists (azl::title_rules::PreparedOf: never the title itself - a homebrew of that band has
// none -, never 3DS Link).
bool IsGuestGame(u64 tid, const std::string& name);

std::string GamePluginDir(u64 tid);  // sdmc:/luma/plugins/<TID>
std::string GamePluginPath(u64 tid); // sdmc:/luma/plugins/<TID>/AzaharLink.3gx

// ---------------------------------------------------------------------------------------------
// The SMDH of a game (its icon file): name and icon
// ---------------------------------------------------------------------------------------------

constexpr std::size_t kSmdhSize = 0x36C0;
constexpr int kLargeIcon = 48; // the one icon the app keeps: lists draw it at half size, the top screen at twice that

// What the app knows of a game: its name and its icon.
struct GameInfo {
    u64 tid = 0;
    u16 version = 0; // the title version it was read at (the cache reads a game again when its version changed)
    std::string name; // the long title (the short one when there is no long one), UTF-8, in the console's language (or the first language that has one)
    u16 icon[kLargeIcon * kLargeIcon]; // RGB565 in 8x8 tiles, as the SMDH stores it
};

// `language` is the console's language (libctru CFG_Language, 0..15). False when the data is not an SMDH or holds no name at all:
// a title like that is not "recognised" and is left out of the list. Fills the name and the icon (not the id and the version).
bool ParseSmdh(const u8* data, std::size_t size, int language, GameInfo& out);

// The order of the list: by name, upper and lower case alike, accented Latin letters as their plain letter.
bool NameLess(const std::string& a, const std::string& b);

// An icon as the GPU wants it: the SMDH's tiles go into a `pot` x `pot` texture (`pot` a power of two, at least `size`), in the
// first rows, the left columns: the sub-texture {0, 1, size / pot, 1 - size / pot} shows exactly them, upright. `out` holds
// pot * pot values.
void LayoutIcon(const u16* tiled, int size, int pot, u16* out);

// ---------------------------------------------------------------------------------------------
// The cache of the titles (titles.cache in the app's data folder): the name and the icon of every title, so a start reads only what is new or
// has a new version. The delivery module keeps a cache of the names only (catalog.cache, its own file, never touched here); this one adds the icon.
// ---------------------------------------------------------------------------------------------

constexpr std::size_t kCachedNameBytes = 392; // 128 UTF-16 characters (a long title) are at most 384 bytes of UTF-8, and the end
// The games of the cache file, if it is there, whole (a check value over its records), and of this language (the names depend on it).
bool LoadGameCache(const char* path, int language, std::vector<GameInfo>& out);
// Writes the file (a temporary one first, then put in place). False when it could not be written completely.
bool SaveGameCache(const char* path, int language, const std::vector<GameInfo>& games);

// ---------------------------------------------------------------------------------------------
// The copy of the plugin in a game's folder
// ---------------------------------------------------------------------------------------------

enum class Slot {
    Off,  // no .3gx in the game's folder
    On,   // AzaharLink.3gx is there and nothing else
    Other // another .3gx is there (with or without ours): Luma would not necessarily load ours, so the app leaves the folder alone
};

// The state a folder with these regular files is in (only the names ending in .3gx count, upper and lower case alike).
Slot ClassifyFolder(const std::vector<std::string>& file_names);
// A copy of the plugin that an earlier 3DS Link put in the folder of the Download Play application of the system (its row of Enable Titles, 1.1.4 builds before
// spec 9): one of the six ids (kDownloadPlayTids), and a folder that holds our plugin and nothing else (Slot::On). Only such a copy is taken away, once
// (App::QueuePluginCopyRefresh): never another .3gx of the user's (Slot::Other), never the folder of another title.
bool IsDownloadPlayLeftover(u64 tid, Slot slot);

struct SlotInfo {
    Slot slot = Slot::Off;
    u64 copy_size = 0; // the size of our copy, when there is one
};
SlotInfo ReadSlot(u64 tid);
// The names (upper case) of the sub-folders of sd:/luma/plugins: one listing tells which games can have a copy at all.
std::set<std::string> ListPluginFolders();

// What the Enable Titles page does with a title (the other lists of titles - Titles Delivery, the public lobbies - show every title).
enum class Listing {
    Hidden, // not a sold game, and no plugin in its folder: it is not in the list
    Game,   // a sold game (IsEnableTitle): in the list, with a switch that goes both ways
    OffOnly // not a sold game (a homebrew, an impostor), but a plugin is in its folder (an older 3DS Link put it there, or the user by hand): in the list so that it
            // can be switched off, and never on; once off it is Hidden the next time the list is made
};
// `name` is the title's name (what tells an impostor); without it the id alone decides.
Listing ListingOf(u64 tid, Slot slot, const std::string& name = std::string());
// (Whether a title may be switched ON is not a state a row carries: the page asks IsEnableTitle about the title itself, every time.)

struct EnableRow {
    std::size_t index = 0; // which of the titles asked about
    SlotInfo info;         // what its folder holds
    Listing listing = Listing::Game;
};
// The rows of the Enable Titles page for these titles, in their order: one listing of sd:/luma/plugins, a look at the folder of every title that
// has one, and the titles that are Listing::Hidden left out. Nothing is written. `names` are the names of the titles, one for each and in the same order (the page
// gives them: an impostor is told by its name); a title with no name here is asked by its id alone.
std::vector<EnableRow> EnableRows(const std::vector<u64>& tids, const std::vector<std::string>& names = {});


// Copies a file next to its destination first (as <dst>.part) and replaces the destination only when the copy is complete, so a
// card that fills up or is pulled never leaves a plugin that is cut short. True when the destination now is a copy of the source.
// (It is used for every file the app copies: the plugin into a title's folder and next to the app.)
bool CopyPluginFile(const char* src, const char* dst);

enum class EnableResult { Ok, SourceMissing, OtherPlugin, CopyFailed };
// Creates sd:/luma/plugins/<TID>/ and puts a copy of `source` in it (an existing copy is replaced: this is also how a copy is
// brought up to date).
EnableResult EnableGame(u64 tid, const char* source);
// Removes the copy and then the folder, if the folder is empty. True when the copy is gone.
bool DisableGame(u64 tid);
// The folder of a Download Play child the app prepared (1.1.4, dlp_guest_store.cpp): DisableGame of `prepared`, and of nothing else - false, nothing
// touched, unless `game` is an application and `prepared` is EXACTLY its prepared title (azl::title_rules::PreparedOf), so a game's own folder or a
// homebrew's is never taken by this.
bool DisableGuestFolder(u64 prepared, u64 game);
// Its copy put in: EnableGame of `prepared` with `source`, with the same guard (false for any other folder: CopyFailed, nothing touched). Called by the
// thread of the copies only (copyworker.cpp, CopyKind::Guest): one thread writes AzaharLink.3gx.part.
EnableResult EnableGuestFolder(u64 prepared, u64 game, const char* source);

// Does the copy in a game's folder need to be made again? (a copy that is not the size of the source: the plugin file was replaced)
inline bool CopyIsStale(u64 copy_size, u64 source_size) {
    return copy_size != source_size;
}

// Do two files differ? The size first, then every byte (a new build of the plugin can have the size of the old one). False when either cannot be read:
// a file that is not there is not a reason to write over the other.
bool FilesDiffer(const char* a, const char* b);

// Brings the copy of the plugin in a game's folder up to date: when the folder holds ours and nothing else (Slot::On) and it is not the same file as
// `source` (the plugin file next to the app), the copy is made again (by EnableGame: a temporary file first, so a card that is pulled never leaves a cut
// plugin). Any other state is left as it is (no folder, another plugin). `copied` says whether it was written.
EnableResult RefreshGameCopy(u64 tid, const char* source, bool* copied);

} // namespace setup
