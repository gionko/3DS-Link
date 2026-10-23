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

struct SlotInfo {
    Slot slot = Slot::Off;
    u64 copy_size = 0; // the size of our copy, when there is one
};
SlotInfo ReadSlot(u64 tid);
// The names (upper case) of the sub-folders of sd:/luma/plugins: one listing tells which games can have a copy at all.
std::set<std::string> ListPluginFolders();

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
