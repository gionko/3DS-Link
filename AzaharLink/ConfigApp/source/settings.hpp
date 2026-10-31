// What 3DS Link keeps on the SD card: the settings file shared with the AzaharLink plugin (sd:/3ds/3DSLink/AzaharLink.ini, a place fixed in
// the code because the plugin has to find it without the app) and the list of saved rooms (in the app's data folder, see paths.hpp).
#pragma once

#include <3ds.h>

#include <string>
#include <vector>

#include "azl/ini_values.hpp"
#include "azl/room_proto.hpp"
#include "fileio.hpp"
#include "paths.hpp"

namespace setup {

constexpr const char* kVersion = "1.0.11";
// The places that are not the app's to choose: Luma3DS loads plugins from this folder (a title's copy of the plugin goes to
// sdmc:/luma/plugins/<title id>/, games.hpp), and the plugin reads its settings and writes its logs in sdmc:/3ds/3DSLink, whatever folder
// the app itself is in (kIniPath and the folder are in paths.hpp with the other fixed places). Until 2026-10-19 the settings were in
// sdmc:/luma/plugins/AzaharLink.ini: a file found there and not in the new place is moved by LoadSettings (the plugin reads the old place
// too while the new file is missing).
constexpr const char* kPluginsDir = "sdmc:/luma/plugins";
// Luma3DS would load this plugin in EVERY title that has no folder of its own; 3DS Link never uses it (the Installation check only says so).
constexpr const char* kDefaultPluginPath = "sdmc:/luma/plugins/default.3gx";

// Everything the plugin keeps in AzaharLink.ini. The app edits a few of the keys and writes back
// all of them, so keys it does not show (net_thread_core, menu_key...) survive a save. Lines it does not
// understand at all are kept verbatim, so a newer plugin's settings are not lost either.
struct Settings {
    bool enabled = true;
    std::string host;
    u16 port = azl::room::kDefaultPort;
    std::string nickname; // empty = the console's own profile name
    std::string password;
    bool tunnel = true;
    bool autoconnect = true;
    int radio_like = 1; // 0 off, 1 broadcast data without retries, 2 unicast too (twice) and the room's stream as it arrives
    bool socket_buffers = true;
    int net_core = -1; // ini key net_thread_core, -1..3 as the plugin takes it (the old net_core=1 written by 0.1.3 is ignored)
    std::string menu_key = azl::ini::kMenuKeyOff; // the chord that opens the in-game menu, or "off" (the default)
    std::string menu_chord = azl::ini::kDefaultMenuKey; // not in the file: the chord to come back to when the menu goes on again
    std::string game_name;
    int debug = 0; // 0 no log at all (no file is written), 1 everything (AzaharLink.log + AzaharLink.debug.log); a 2 reads as 1
    int rcvbuf_kb = azl::ini::kRcvbufKbDefault; // not shown by the app (an advanced fallback: 8, 16, 32 or 64), but a field of its own: it is written, never lost
    int sleep_stop = azl::ini::kSleepStopDefault; // the row "End session on sleep": 0 off, 1 a client, 2 a host too
    std::vector<std::string> unknown_lines;
    unsigned keys_seen = 0; // not in the file: bit i = the key azl::ini::kKeyNames[i] was in the file that LoadSettings read
    bool moved_from_old_place = false; // not in the file: LoadSettings found the file in sdmc:/luma/plugins and moved it to kIniPath
};

bool ValidNickname(const std::string& s);

// The in-game menu switch (menu_key). Off writes "off" and remembers the chord that was there; on writes that chord
// again (the default one when the file never had a chord): a custom chord survives off and on, and a file the user
// does not touch is written back as it was.
bool MenuOn(const Settings& cfg);
void SetMenuOn(Settings& cfg, bool on);
// The chord as the user has to press it: the plugin adds Select to every chord, so "L+R+Start" is shown as
// "L+R+Start+Select".
std::string MenuChordText(const std::string& chord);

// Missing file or missing keys keep the defaults above. A file that could not be read (LoadResult, fileio.hpp) is never written over with the
// defaults.
LoadResult LoadSettings(Settings& cfg);
// True when /luma/plugins exists: the first-run file is only written where the plugin could be installed.
bool PluginsDirExists();
// True when the file LoadSettings read had every one of the 15 keys (a file of 0.1.14 or older lacks two).
bool HasAllKeys(const Settings& cfg);

// The three values of the sleep row, in the order A (and D-pad right) cycles through them: 0, 1, 2. D-pad left goes back.
const char* SleepStopText(int value);
int NextSleepStop(int value);
int PrevSleepStop(int value);
// The three values of radio_like, the same way: "Off", "Broadcast", "All (twice)".
const char* RadioLikeText(int value);
int NextRadioLike(int value);
int PrevRadioLike(int value);

// Creates /luma/plugins when needed. Returns false if the file could not be written completely.
bool SaveSettings(const Settings& cfg);

// The version word in a .3gx header (offset 8: major << 24 | minor << 16 | revision << 8) as "major.minor.revision".
std::string PluginVersionText(u32 word);

// ---------------------------------------------------------------------------------------------
// The saved rooms
// ---------------------------------------------------------------------------------------------

// The longest name a room can be given, and what it may hold: A-Z a-z 0-9 space . _ - (like a nickname). Empty = the room has no name.
constexpr std::size_t kMaxServerNameChars = 12;
bool ValidServerName(const std::string& name);

// One room: the host (a name or an IPv4 address), its UDP port, a name the user gave it (may be empty) and its password (may be empty: a room
// without one). The password belongs to the room: each room keeps its own, and using a room puts that one in AzaharLink.ini.
struct Server {
    std::string name;
    std::string host;
    u16 port = azl::room::kDefaultPort;
    std::string password;

    // The same room: the host compares without regard to case, the name does not count.
    bool Same(const Server& other) const;
    // "host:port", the port always spelled out (the form the app shows).
    std::string Text() const;
    // What a row calls the room: its name, or "host:port" when it has none.
    std::string Label() const;
};

enum class ServerParse { Ok, BadHost, BadPort };
// What the keyboard holds: "host" or "host:port" (the room's default port when there is none). The name is not touched.
ServerParse ParseServer(const std::string& text, Server& out);
// A line of the servers file: "host[:port]" as before (no name), "host[:port]|name", or "host[:port]|name|password" (the name may be empty; the
// password is everything after the second bar, so it may hold one). The first parts are the old formats, so a file of an earlier build reads as
// it was; a name that is not valid (too long, a character that is not allowed) is left out and the room kept.
ServerParse ParseServerLine(const std::string& line, Server& out);

// The rooms the user has used, the most recently used first, at most kMax of them, none twice. The file (servers.txt in the app's data folder)
// is plain text, one room per line: "host", "host:port" (the port is left out when it is the default), and "|name" after it when the room has a
// name, e.g. "192.168.1.20:24999|Home PC", and "|password" after that when it has one ("host||password" for a room with no name). A name never
// holds '|': it is not one of the allowed characters.
class ServerList {
public:
    static constexpr std::size_t kMax = 10;

    // The content of the file: lines that are not a room and rooms already seen are dropped, the first kMax are kept.
    static ServerList Parse(const std::string& text);
    std::string Serialise() const;

    enum class AddResult { Added, Moved, Full };
    // A new room goes to the top; one that is in the list moves to the top (and takes the new name when it has one, else keeps its own); a
    // new one in a full list is refused (Full).
    AddResult Add(const Server& server);
    // Like Add, but a full list gives up the room that was used the longest ago: for a room that was just proven to work.
    // Returns false when the list did not change (the room was on top already).
    bool Use(const Server& server);
    // Returns false when there is no such entry.
    bool Remove(std::size_t index);
    // Puts a room in the place of entry `index` (the name, the address, the port and the password may all be new), where it was in the list.
    // Duplicate: another entry is the same room (host and port); Missing: there is no such entry.
    enum class ReplaceResult { Ok, Duplicate, Missing };
    ReplaceResult Replace(std::size_t index, const Server& server);
    // The index of the same room, or -1.
    int Find(const Server& server) const;

    const std::vector<Server>& entries() const {
        return entries_;
    }
    std::size_t size() const {
        return entries_.size();
    }
    bool full() const {
        return entries_.size() >= kMax;
    }

private:
    std::vector<Server> entries_;
};

// Missing = no such file (the caller seeds the list); a file that could not be read is never written over.
LoadResult LoadServers(const std::string& path, ServerList& list);
// Creates the folder of the file when needed. Returns false if the file could not be written completely.
bool SaveServers(const std::string& path, const ServerList& list);

} // namespace setup
