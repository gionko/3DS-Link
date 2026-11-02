// Where 3DS Link's own files are. Nothing in the app names a folder of its own: everything derives from the folder 3DSLink.3dsx was
// started from (argv[0], which the Homebrew Launcher gives), so the app, AzaharLink.3gx and boot9.bin can sit in ANY folder of the SD
// card, together. An app that is not started from a .3dsx (installed as a CIA) has no such folder: it uses sdmc:/3ds/3DSLink. The one folder
// of the app, "3DSLink", holds what it keeps (3DSLink.cfg, the rooms, the caches, the logs); when the app itself is started from a folder
// called 3DSLink that is the folder, otherwise it is made inside the one the app was started from. Only what belongs to someone else stays
// fixed: Luma3DS loads plugins from sdmc:/luma/plugins (settings.hpp), and GodMode9 dumps the files it extracts to sdmc:/gm9/out. Plain C++
// and POSIX file calls, so Tests/settings_test/paths_test.cpp builds this very file on a PC against a folder called "sdmc:".
#pragma once

#include <3ds.h>

#include <functional>
#include <string>
#include <vector>

namespace setup {

// When the launcher does not say where the app was started from (the app installed as a CIA, an emulator, a launcher that gives no argv):
// the app's own folder.
constexpr const char* kDefaultAppDir = "sdmc:/3ds/3DSLink";
// The plugin's own files - its settings (AzaharLink.ini) and its logs - are in this folder whatever folder the app is in: the plugin has
// to find them without the app, so this place is fixed (the same as the data folder of an app in /3ds/3DSLink or /3ds). Until 2026-10-19
// the settings were in sdmc:/luma/plugins/AzaharLink.ini (kOldIniPath): settings.cpp moves a file found there.
constexpr const char* kPluginDataDir = "sdmc:/3ds/3DSLink";
constexpr const char* kIniPath = "sdmc:/3ds/3DSLink/AzaharLink.ini";
constexpr const char* kOldIniPath = "sdmc:/luma/plugins/AzaharLink.ini";
// Where GodMode9 puts what it dumps (its own default; the app looks for boot9.bin there too).
constexpr const char* kGodModeOutDir = "sdmc:/gm9/out";
constexpr const char* kBoot9FileName = "boot9.bin";
constexpr const char* kDataDirName = "3DSLink";           // the app's own folder: what the app keeps, made when it is first used
constexpr const char* kConfigFileName = "3DSLink.cfg";
constexpr const char* kServersFileName = "servers.txt";
// More lists of saved rooms: servers.txt is list 1, then servers2.txt ... servers10.txt. The plugin's ROOMS list in the game reads the same files.
constexpr int kServerLists = 10;
// "servers.txt" for list 1, "servers<n>.txt" for the others (n = 1..kServerLists).
std::string ServersFileName(int list);
constexpr const char* kGamesCacheFileName = "titles.cache";
constexpr const char* kCatalogCacheFileName = "catalog.cache"; // (the name titleshare_host/hostcfg's data_path("catalog.cache") builds)
constexpr const char* kParkDirName = "park";
constexpr const char* kDlpDirName = "dlp";               // 3DS Link Delivery: the child that Download Play consoles download lives here
constexpr const char* kDlpChildFileName = "child.cia";
constexpr const char* kDlpAppCiaFileName = "3dslink.cia"; // the app's own CIA, kept for the composer of the child (the child's installer leaves it here too)
constexpr const char* kDlpMarkerFileName = "child.preparing"; // there while child.cia is being built: a run that died in the middle leaves it behind

// The folder an argv[0] says the app was started from, in the form every path of the app takes ("sdmc:/dir", the root being "sdmc:"):
//   "sdmc:/3ds/3DSLink.3dsx" -> "sdmc:/3ds"      "/3ds/3DSLink.3dsx" -> "sdmc:/3ds" (a leading '/' is the SD card)
//   "sdmc:/3DSLink.3dsx" -> "sdmc:"              "sdmc:/a/b/../c/x.3dsx" -> "sdmc:/a/c"     "sdmc://a//x.3dsx" -> "sdmc:/a"
// A launcher that gives nothing (NULL or ""), a path that is not on the SD card ("3dslink:/x.3dsx", "romfs:/...") and a bare name
// ("x.3dsx") give kDefaultAppDir.
std::string AppDirFromArgv0(const char* argv0);
// Do two names say the same thing, upper and lower case alike?
bool SameNameNoCase(const std::string& a, const std::string& b);

// "dir" + "/" + "name" ("sdmc:" + "x" is "sdmc:/x"; a dir that ends with '/' gets no second one; an empty dir gives the name).
std::string JoinPath(const std::string& dir, const std::string& name);
// The folder that holds `path` ("sdmc:/a/b" -> "sdmc:/a", "sdmc:/a" -> "sdmc:"), or "" for the root "sdmc:" (and for a path with no folder).
std::string ParentDir(const std::string& path);
// The last part of a path ("sdmc:/a/b.bin" -> "b.bin"; "sdmc:" -> "").
std::string BaseName(const std::string& path);
// "size:mtime" of a file ("" when there is none): what 3DSLink.cfg remembers of the plugin file the copies were brought up to.
std::string FileStamp(const std::string& path);
// Removes the block files (*.blk) a receiving left in the park folder (a power cut: the delivery module could not clean up). Other files stay.
// Returns how many went (0 when the folder is not there).
int RemoveParkFiles(const std::string& park_dir);

// Every file of the app, worked out from the folder it was started from.
struct Paths {
    std::string appdir;        // the folder 3DSLink.3dsx is in: "sdmc:/3ds" (sdmc:/3ds/3DSLink when there is none)
    std::string plugin_file;   // the plugin that is copied into the folder of each enabled title: plugin_beside when that is there, else plugin_in_data (ResolvePluginFile)
    std::string plugin_beside; // <appdir>/AzaharLink.3gx: next to the app, where the plugin was put by hand before it became a resource of the app
    std::string plugin_in_data; // <datadir>/AzaharLink.3gx: in the app's own folder, where the app puts the plugin it carries (sdmc:/3ds/3DSLink for a CIA install)
    std::string boot9_beside;  // <appdir>/boot9.bin: the first place the key file is looked for
    std::string boot9_in_data; // <datadir>/boot9.bin: the second (the same place when the app is in its own folder)
    std::string boot9_godmode; // sdmc:/gm9/out/boot9.bin: the third
    std::string datadir;       // the app's own folder: <appdir> when that is called 3DSLink, else <appdir>/3DSLink
    std::string config_file;   // <datadir>/3DSLink.cfg
    std::string servers_file;  // <datadir>/servers.txt
    std::string games_cache;   // <datadir>/titles.cache: the name and the icon of every title
    std::string catalog_cache; // <datadir>/catalog.cache: the names of the titles the delivery module reads
    std::string park_dir;      // <datadir>/park: the temporary files of a title being received
    std::string delivery_log;  // <datadir>/3DSLink-TitleDelivery.log (the current one; older ones are .1.log ...)
    std::string dlp_dir;       // <datadir>/dlp: what 3DS Link Delivery (the Download Play host) keeps
    std::string dlp_child;     // <datadir>/dlp/child.cia: the CIA a Download Play console downloads and installs
    std::string dlp_app_cia;   // <datadir>/dlp/3dslink.cia: the app's own CIA, the part of the child that the composer cannot make
    std::string dlp_marker;    // <datadir>/dlp/child.preparing: written before the child is built, removed after (see dlp_prepare_logic.hpp)

    static Paths FromAppDir(const std::string& appdir);
};

// The plugin file in use: the one next to the app when it is there (one put there by hand, before the plugin was a resource of the app), else the one in the app's own folder,
// which is also where the app puts the plugin it carries when neither is there (plugin_install.hpp): sdmc:/3ds/3DSLink/AzaharLink.3gx for a CIA install.
std::string ResolvePluginFile(const Paths& paths);

// The paths of this run: set once at start (main.cpp), read by the parts of the app that have no page to ask.
const Paths& CurrentPaths();
void SetCurrentPaths(const Paths& paths);

// Makes the folder and the ones above it ("sdmc:/a/b" makes "sdmc:/a" and "sdmc:/a/b"); true when it exists afterwards.
bool MakeDirs(const std::string& path);

// The app has started before when its config file is in the data folder: until then it is the first start (the app has asked its
// question, scanned and checked, and written the file at the end).
bool IsFirstStart(const Paths& paths);

// ---------------------------------------------------------------------------------------------
// boot9.bin
// ---------------------------------------------------------------------------------------------

// A boot9.bin dump is 64 KB (a full dump) or 32 KB (the protected half): the sizes the delivery module's key code accepts.
bool Boot9SizeOk(u64 size);
// Where boot9.bin is looked for, in this order: next to the app, in the app's own folder, GodMode9's default folder, then the path the user
// chose earlier (saved in the app's config; "" = none). A place that is the same as an earlier one is listed once.
std::vector<std::string> Boot9Candidates(const Paths& paths, const std::string& saved_path);

struct Boot9Lookup {
    bool found = false;
    std::string path; // where it is (the file)
    // The folder it is in, for the rows ("sdmc:/gm9/out").
    std::string folder() const;
};
// The size of a file: false when there is no such file (a folder does not count).
using SizeFn = std::function<bool(const std::string& path, u64* size)>;
// The first candidate that exists with a size Boot9SizeOk accepts. `size_of` is the real file system in the app, a made-up one in a test.
Boot9Lookup FindBoot9(const std::vector<std::string>& candidates, const SizeFn& size_of);
// The same with the real file system.
Boot9Lookup FindBoot9(const Paths& paths, const std::string& saved_path);
// Is `file` a boot9.bin this app can use (it exists with an accepted size)?
bool IsUsableBoot9(const std::string& file);

// What a boot9 dump is, by what is IN it (the bootrom is the same on every console, so a file can be told whatever it is called): the SHA-256 of
// the whole bootrom (64 KB) and of its upper half (32 KB, the part that holds the keys) are known; the lower half alone (32 KB) has no keys in it.
enum class Boot9Verdict {
    Unknown, // some other file (or a boot9 this app does not know): it counts when its NAME says boot9 and its size is right
    Good,    // a boot9 dump: whatever its name is
    NoKeys   // the lower half of the bootrom: the size of a boot9 and no use as one
};
struct Boot9Fingerprint {
    u64 size;
    std::string sha256_hex; // upper case
    Boot9Verdict verdict;
};
// The verdict on a file (nothing is read when its size is not a boot9's; the answer is remembered while the file's size and time stay the same).
Boot9Verdict ClassifyBoot9File(const std::string& path);
// For the tests: tell the app other fingerprints than the real ones (an empty list puts the real ones back).
void SetBoot9FingerprintsForTest(const std::vector<Boot9Fingerprint>& list);
// SHA-256 of a block of bytes, as 64 upper case hexadecimal digits.
std::string Sha256Hex(const u8* data, std::size_t size);

// Is this the NAME of a boot9 dump? "boot9.bin", or what GodMode9 2.x makes of it ("ABC12345678_boot9_00.bin": the console's serial, the name,
// a number): a name that ends in ".bin" and has "boot9" in it (any case) but not "boot11" (the other bootrom, which has the same size).
bool IsBoot9Name(const std::string& name);
// The best boot9 dump directly inside `dir` (its sub-folders are not looked at), or "": the file called exactly boot9.bin first, then the one
// of 64 KB before one of 32 KB, then the last by name (a later dump of the same console). Only a file of a good size counts.
std::string FindBoot9InFolder(const std::string& dir);

} // namespace setup
