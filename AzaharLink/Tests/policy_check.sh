#!/bin/bash
# The defaults and the switches that decide what the plugin does to a game on a console nobody can test here, read
# from the sources: flipping one by accident (or adding a knob) fails this check. It is a plain text scan, so it
# also runs where the plugin cannot be built. Called by Tests/run.sh.
cd "$(dirname "$0")/.."
FAILED=0
# a clone that was never built (the build needs items of your own console, see the README) has none of the build products: their checks are skipped
BUILT=1
[ -f dist/3DSLink.3dsx ] && [ -f ConfigApp/data/AzaharLink.3gx ] && [ -f ConfigApp/data/child_model.bin ] || BUILT=0
[ "$BUILT" = 1 ] || echo "NOTE: this tree has not been built: the checks of the build products (dist/, ConfigApp/data/) are skipped"
fail() {
  echo "policy check FAILED: $1"
  FAILED=1
}
# $1 = file, $2 = what must be in it (a fixed string), $3 = why
need() {
  grep -F -q -- "$2" "$1" || fail "$1 no longer has '$2' ($3)"
}
# $1 = file, $2 = a whole line that must be in it (no more, no less than the text), $3 = why
need_line() {
  tr -d '\r' < "$1" | grep -F -x -q -- "$2" || fail "$1 no longer has the line '$2' ($3)"
}
# $1 = file, $2 = a line that starts a function, $3 = what must follow within the next 3 lines, $4 = why
need_after() {
  tr -d '\r' < "$1" | grep -F -A3 -- "$2" | grep -F -q -- "$3" || fail "$1: '$2' is no longer followed by '$3' ($4)"
}

# The ini defaults: the sleep action is off until the lid test, the receive ring is the 64 KB the soc service grants, and
# these two live in ONE place (Includes/azl/ini_values.hpp) that the plugin's Config and the Setup app's Settings both take
# their defaults from
need Includes/azl/ini_values.hpp "constexpr int kSleepStopDefault = 0;" "the sleep action is off until the lid test (0 off, 1 a client, 2 a host too)"
need Includes/azl/ini_values.hpp "constexpr int kRcvbufKbDefault = 64;" "the receive ring is the 64 KB the soc service grants"
need Includes/ctr/plugin.hpp "int sleep_stop = ini::kSleepStopDefault;" "the plugin takes its default from the shared header"
need Includes/ctr/plugin.hpp "int rcvbuf_kb = ini::kRcvbufKbDefault;" "the plugin takes its default from the shared header"
need ConfigApp/source/settings.hpp "int sleep_stop = azl::ini::kSleepStopDefault;" "the Setup app takes its default from the shared header"
need ConfigApp/source/settings.hpp "int rcvbuf_kb = azl::ini::kRcvbufKbDefault;" "the Setup app takes its default from the shared header"
need Includes/ctr/plugin.hpp "int debug = 0;" "the debug capture is off by default"
need Includes/ctr/plugin.hpp "bool enabled = true;" "the plugin is on by default"
# ... and the README says the same default as the code (a default that is flipped must be flipped in the table too)
SLEEP_DEFAULT=$(grep -o 'kSleepStopDefault = [0-9]*;' Includes/azl/ini_values.hpp | sed 's/.*= //; s/;//')
tr -d '\r' < README.md | grep -q "^| \`sleep_stop\` | $SLEEP_DEFAULT |" || fail "README.md does not give $SLEEP_DEFAULT as the default of sleep_stop"

# No knob beyond these (a new ini key is a decision, not a side effect). They are 15: the 13 of 0.1.14 plus
# rcvbuf_kb and sleep_stop. menu_key and debug are old keys: 0.1.15 only gave them values (menu_key=off, debug 0 or 1).
KEYS=$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -o 'key == "[a-z_]*"' | sed 's/key == "//; s/"//' | sort | tr '\n' ' ')
WANTED="autoconnect debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "
[ "$KEYS" = "$WANTED" ] || fail "the ini keys read by the plugin are '$KEYS', expected '$WANTED'"

# Both programs write all 15 keys, in one place: they fill every field of ini::Fields and call ini::Render, which writes the 15
# lines (a writer that forgets a key, or a third writer with its own list, fails here). Before 0.1.16 the plugin left out
# rcvbuf_kb and sleep_stop unless they were set and the app did not know them: a file made by either had no line for them and a
# hand edit needed the key names.
FIELDS="autoconnect debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "
for f in Sources/ctr/plugin.cpp ConfigApp/source/settings.cpp; do
  GOT=$(tr -d '\r' < "$f" | grep -o 'fields\.[a-z_]* = ' | sed 's/fields\.//; s/ = //' | sort -u | tr '\n' ' ')
  [ "$GOT" = "$FIELDS" ] || fail "$f fills the ini fields '$GOT', expected '$FIELDS'"
  tr -d '\r' < "$f" | grep -q 'ini::Render(fields' || fail "$f does not write the ini through ini::Render"
done
RENDERED=$(tr -d '\r' < Includes/azl/ini_values.hpp | sed -n '/^inline std::string Render/,/^}/p' | grep -o '"[a-z_]*=' | tr -d '="' | sort | tr '\n' ' ')
[ "$RENDERED" = "$WANTED" ] || fail "ini::Render writes '$RENDERED', expected '$WANTED'"
! tr -d '\r' < Sources/ctr/plugin.cpp | grep -q 'WriteLine' || fail "plugin.cpp writes a line of the ini by itself (it must go through ini::Render)"
# The switches (enabled, tunnel, autoconnect, socket_buffers) are read by the shared ParseBool in both programs: 1/on/true/yes
# and 0/off/false/no, and a slip is the key's default, never "on". radio_like has three values (0, 1, 2: ini::ParseRadioLike,
# which takes the words of a switch for 0 and 1) since the performance round of 2026-10-19.
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'ini::ParseBool(value, true)')" = "4" ] || fail "the plugin reads a switch of the ini without ini::ParseBool"
[ "$(tr -d '\r' < ConfigApp/source/settings.cpp | grep -c 'azl::ini::ParseBool(value, true)')" = "4" ] || fail "the Setup app reads a switch of the ini without ini::ParseBool"
need Sources/ctr/plugin.cpp "config_.radio_like = ini::ParseRadioLike(value);" "the plugin reads radio_like's three values with the shared parser"
need ConfigApp/source/settings.cpp "cfg.radio_like = azl::ini::ParseRadioLike(value);" "the Setup app reads radio_like's three values with the shared parser"
need Includes/azl/ini_values.hpp 'if (Word(value) == "2") {' "radio_like=2 is the third value"
need Includes/azl/ini_values.hpp "return known ? (on ? 1 : 0) : 1;" "the words of a switch mean 0 and 1 for radio_like, a slip is 1"
need ConfigApp/source/pages_config.cpp "g.cfg.radio_like = step < 0 ? setup::PrevRadioLike(g.cfg.radio_like) : setup::NextRadioLike(g.cfg.radio_like);" "the row 'Radio-like data' goes through its three values"
if tr -d '\r' < Sources/ctr/plugin.cpp | grep -n 'value != "0"'; then
  fail "the plugin reads a switch of the ini as 'anything but 0 is on' again (ini::ParseBool)"
fi
if tr -d '\r' < ConfigApp/source/settings.cpp | grep -n 'value != "0"'; then
  fail "the Setup app reads a switch of the ini as 'anything but 0 is on' again (ini::ParseBool)"
fi
# The 3DS Link app (the Setup app) has a row for every key that is not an advanced one (socket_buffers, net_thread_core, game_name
# and rcvbuf_kb are in the file, never lost, but are not rows; the menu chord is carried by the In-game menu row). The main page
# has the 3DS Link switch (enabled + tunnel); the room (host, port, and its own password) is chosen on the Azahar Emulator Room page; the nickname
# and the other switches are on the Settings page.
for field in enabled tunnel; do
  grep -E -q "g\.cfg\.$field([^a-z_]|$)" ConfigApp/source/pages_main.cpp || fail "the main page of the Setup app has no row for $field"
done
for field in host port password; do
  grep -E -q "g\.cfg\.$field = " ConfigApp/source/pages_servers.cpp || fail "the Setup app has no page that sets $field (a room brings its own)"
done
for field in nickname autoconnect radio_like sleep_stop debug; do
  grep -E -q "g\.cfg\.$field([^a-z_]|$)" ConfigApp/source/pages_config.cpp || fail "the Settings page of the Setup app has no row for $field"
done
need ConfigApp/source/pages_servers.cpp 'pass.label = "Password";' "the form of a room has its password row (each room keeps its own)"
need ConfigApp/source/settings.hpp "std::string password;" "a saved room has a password"
need ConfigApp/source/pages_config.cpp "g.cfg.sleep_stop = step < 0 ? setup::PrevSleepStop(g.cfg.sleep_stop) : setup::NextSleepStop(g.cfg.sleep_stop);" "the row 'End session on sleep' goes through its three values (A and right go forward, left goes back)"
need ConfigApp/source/ui.cpp "setup::HasAllKeys(cfg)" "the app completes a file that lacks keys"
# The app is called 3DS Link (3DSLink.3dsx) but it edits the plugin's own file: the plugin and its ini keep their names
tr -d '\r' < ConfigApp/Makefile | grep -E -q '^TARGET[[:space:]]*:=[[:space:]]*3DSLink[[:space:]]*$' || fail "ConfigApp/Makefile no longer builds 3DSLink.3dsx"
need ConfigApp/source/paths.hpp 'kIniPath = "sdmc:/3ds/3DSLink/AzaharLink.ini";' "the app edits AzaharLink.ini where the plugin reads it (sd:/3ds/3DSLink, with the logs)"
need ConfigApp/source/paths.hpp 'kOldIniPath = "sdmc:/luma/plugins/AzaharLink.ini";' "the app knows the old place of the ini, to move the file"
need ConfigApp/source/settings.cpp "const LoadResult old = ReadTextFile(kOldIniPath, kMaxReadBytes, content);" "a settings file left in the old place is read when the new one is missing..."
need ConfigApp/source/settings.cpp "std::remove(kOldIniPath);" "... and moved: the old copy goes once the new file is written"
need Sources/ctr/plugin.cpp 'kConfigPath = "/3ds/3DSLink/AzaharLink.ini";' "the plugin reads its settings where the app writes them"
need Sources/ctr/plugin.cpp 'kOldConfigPath = "/luma/plugins/AzaharLink.ini";' "the plugin still reads the old place while the new file is missing (an older app)"
need Sources/ctr/plugin.cpp 'kDataDir = "/3ds/3DSLink/";' "the plugin writes its logs with the app's files"
need Sources/ctr/plugin.cpp "log_files_.reset(new LogFiles(file_layer_.get(), kDataDir, start_ms_));" "the log files are made in that folder"
need Includes/azl/log_files.hpp "u64 log_rotate_bytes = 64ull * 1024 * 1024;" "the main log grows in blocks of 64 MB"
need Includes/azl/log_files.hpp "int keep = 49;" "50 blocks a log at most (49 besides the current file)"
need Sources/azl/log_files.cpp "const int next = block_hi_[kind] + 1;" "a rotated file takes the number after the highest block: the blocks read in order and never go back"
# The list of saved rooms: servers.txt in the app's data folder (a name and an address for each: "host[:port]|name"), 10 rooms at most
need ConfigApp/source/paths.hpp 'kServersFileName = "servers.txt";' "the saved rooms live in servers.txt in the data folder"
need ConfigApp/source/settings.hpp "static constexpr std::size_t kMax = 10;" "the list of saved rooms holds 10"
need ConfigApp/source/settings.hpp "constexpr std::size_t kMaxServerNameChars = 12;" "a room's name is 12 characters at most"
need ConfigApp/source/settings.cpp 'out += "|" + s.name;' "the file keeps a room's name after its address, after a '|'"
need ConfigApp/source/settings.cpp "const std::size_t bar = line.find('|');" "a line of the servers file is 'host[:port]' (as in the earlier builds) with an optional '|name'"
need ConfigApp/source/pages_servers.cpp "NewProbePage(room," "a room that is added or changed is tested at once"
need ConfigApp/source/pages_servers.cpp "Could not reach the room. Use it as the active room anyway?" "a room that cannot be reached is only used when the user says so"
need ConfigApp/source/pages_servers.cpp "g.servers.Replace(static_cast<std::size_t>(index), room)" "a room is changed in the same place of the list, all its four fields at once"
need ConfigApp/source/pages_servers.cpp "g.cfg.host = server.host;" "using a room puts its address in AzaharLink.ini"
need ConfigApp/source/pages_servers.cpp "g.cfg.port = server.port;" "...and its port"
need ConfigApp/source/pages_servers.cpp "g.cfg.password = server.password;" "using a room puts its own password in AzaharLink.ini"
need ConfigApp/source/fileio.cpp "return error == ENOENT ? LoadResult::Missing : LoadResult::Unreadable;" "only a missing file is created (the list of rooms is seeded, the settings file is made), one that could not be read is never written over"
need ConfigApp/source/fileio.cpp "if (failed) {" "...and a read that went wrong half way is not a file with half its text"
need ConfigApp/source/fileio.cpp "if (std::rename(path.c_str(), previous.c_str()) != 0) {" "a file is written whole or not at all: the new one is finished first, the old one taken aside, then the new one put in its place"
need ConfigApp/source/fileio.cpp "if (!f && error == ENOENT && std::rename(previous.c_str(), path.c_str()) == 0) {" "...and a write that was cut off between the two does not lose the old file"
need ConfigApp/source/settings.cpp "return WriteTextFile(kIniPath, azl::ini::Render(fields, extra, kMaxIniBytes));" "AzaharLink.ini is written by the one careful writer"
need ConfigApp/source/settings.cpp "return WriteTextFile(path, list.Serialise());" "...and so is the list of rooms"
need ConfigApp/source/appcfg.cpp "return WriteTextFile(path, SerialiseAppConfig(cfg));" "...and so is 3DSLink.cfg"
[ "$(grep -c 'fopen' ConfigApp/source/settings.cpp ConfigApp/source/appcfg.cpp | grep -v ':0$' | wc -l)" = "0" ] || fail "settings.cpp or appcfg.cpp opens a file by itself (the files of the app go through fileio.cpp)"
need ConfigApp/source/settings.cpp "c != '|'; // no blank, no control character, no bar" "a room's address has no bar (it ends the host in the file of the rooms)"
need ConfigApp/source/ui.cpp "if (settings_loaded == setup::LoadResult::Unreadable) {" "the settings are not saved over an AzaharLink.ini that could not be read"
need ConfigApp/source/system.cpp "facts.sd_writable = SdWritable(paths.datadir);" "the card is not written to for a test at every return to the main page"
need ConfigApp/source/system.cpp "now - tested_at < 60000" "...a card that could be written a moment ago is not tested again for a minute"
need ConfigApp/source/probe.cpp "if (client->StopWithin(kStopWaitMs)) {" "a room client is freed only when its network thread has left (it uses the client)"
need ConfigApp/source/copyworker.cpp "if (thread && threadJoin(thread, 5ULL * 1000000000ULL) == 0) {" "the copy thread is freed only when it has ended (it keeps its stack until then): only 0 means the thread is over, a timeout is 0x09401BFE"
# - 2026-10-21, the crash at START -> Exit (three Luma dumps): a timeout of a kernel wait is 0x09401BFE, NOT negative, so R_SUCCEEDED / R_FAILED
#   on threadJoin or svcWaitSynchronization takes a timeout for a success; the app freed the scan's state under the running worker. Only == 0 / != 0.
need ConfigApp/source/titles.cpp "if (threadJoin(impl_->thread, timeout_ms * 1000000ULL) == 0) {" "the scanner is taken for finished only when the join returned 0 (a timeout is not a finish)"
need ConfigApp/source/titles.cpp "    impl_->cancel.store(true); // (a worker that is still reading stops at its next step: the wait below is short)" "the scanner tells its worker to stop when the app closes"
need ConfigApp/source/titles.cpp "    std::atomic<bool> cancel{false};" "... with a flag the worker looks at"
[ "$(grep -c 'impl->cancel.load()' ConfigApp/source/titles.cpp)" = "3" ] || fail "ConfigApp/source/titles.cpp: the scan worker looks at its stop flag in more or fewer than the three places (after the catalog, at each game, before the health)"
if grep -rn -E 'R_(SUCCEEDED|FAILED)\(\s*(threadJoin|svcWaitSynchronization|svcWaitSynchronizationN)' ConfigApp/source ConfigApp/titleshare ConfigApp/titleshare_host Sources Includes --include=*.cpp --include=*.c --include=*.hpp --include=*.h; then
  fail "R_SUCCEEDED / R_FAILED on a kernel wait: a timeout (0x09401BFE) is not negative, so it counts as a success; compare with == 0 / != 0"
fi

# ---- the 3DS Link app: Enable titles, the start-up flow, the installation check, the Plugin Loader, the title delivery ----
# NOTHING is a fixed place: every file of the app is worked out at run time from the folder 3DSLink.3dsx was started from (argv[0]), in
# ConfigApp/source/paths.cpp. The places that are someone else's stay: sdmc:/luma/plugins (Luma3DS) and sdmc:/gm9/out (GodMode9).
need ConfigApp/source/paths.cpp "p.plugin_beside = JoinPath(appdir, kPluginFileName);" "the plugin file is next to the app, wherever the app is"
need ConfigApp/source/paths.cpp "p.plugin_in_data = JoinPath(p.datadir, kPluginFileName);" "...or in the app's own folder (where an app installed as a CIA keeps it)"
need ConfigApp/source/paths.cpp "p.datadir = SameNameNoCase(BaseName(appdir), kDataDirName) ? appdir : JoinPath(appdir, kDataDirName);" "the app's own folder is the folder it is in when that is called 3DSLink, else a folder of that name inside it"
need_after ConfigApp/source/paths.cpp "if (!argv0 || !*argv0) {" "return kDefaultAppDir;" "a launcher that says nothing (an emulator) gives the folder of the Homebrew Launcher"
need ConfigApp/source/paths.hpp 'kDefaultAppDir = "sdmc:/3ds/3DSLink";' "the folder used when the launcher does not say where the app is (an app installed as a CIA): the app's own folder"
need ConfigApp/source/paths.hpp 'kGodModeOutDir = "sdmc:/gm9/out";' "GodMode9's own folder, the one fixed place of boot9.bin"
need ConfigApp/source/paths.hpp 'kDataDirName = "3DSLink";' "what the app keeps is in the folder 3DSLink"
need ConfigApp/source/paths.hpp 'kConfigFileName = "3DSLink.cfg";' "the first start is over when 3DSLink.cfg is in the data folder"
need ConfigApp/source/paths.hpp 'kGamesCacheFileName = "titles.cache";' "the app's own cache of the titles (name and icon), in the data folder"
need ConfigApp/source/paths.hpp 'kCatalogCacheFileName = "catalog.cache";' "the cache of the names the delivery module reads, in the data folder"
need ConfigApp/titleshare/source/catalog.c '"catalog.cache"' "the module's cache is the file the app expects, in the data folder the app tells it"
need ConfigApp/source/games.hpp 'constexpr const char* kPluginFileName = "AzaharLink.3gx";' "a title's copy of the plugin keeps the plugin's own name"
# no folder of the app's own is written in the code: the only "sdmc:/3ds" is the default of an app that was not told where it is
[ "$(grep -rn '"sdmc:/3ds' ConfigApp/source ConfigApp/titleshare ConfigApp/titleshare_host --include=*.c --include=*.h --include=*.cpp --include=*.hpp | grep -v '^ConfigApp/source/paths.hpp:' | wc -l)" = "0" ] || fail "a folder under sdmc:/3ds is written in the code outside paths.hpp (everything derives from where the app was started)"
# (three: the default folder of an app that was not told where it is, and the two fixed places of the plugin's files - its folder and its ini -
# which the plugin has to find without the app)
[ "$(grep -n '= "sdmc:/3ds' ConfigApp/source/paths.hpp | wc -l)" = "3" ] || fail "paths.hpp names a place under sdmc:/3ds that it should not (only the default folder, the plugin's folder and its ini)"
need ConfigApp/source/paths.hpp 'kPluginDataDir = "sdmc:/3ds/3DSLink";' "the plugin's folder is the one place of the app's that is fixed"
if grep -rn -E '"[^"]*(TitleShare|Enable game|Game delivery|enable game)[^"]*"' ConfigApp/source ConfigApp/titleshare_host ConfigApp/titleshare/source ConfigApp/titleshare/common --include=*.c --include=*.h --include=*.cpp --include=*.hpp | grep -v 'kCatalogCache = '; then
  fail "a text of the app or of the delivery module says TitleShare (or Enable game / Game delivery): the user knows no such app"
fi
need ConfigApp/source/pages_main.cpp 'enable.label = "Enable Titles for Azahar Emulator Net";' "the page that enables titles is called 'Enable Titles for Azahar Emulator Net'"
need ConfigApp/source/pages_main.cpp 'room.label = "Azahar Emulator Room";' "the page of the room is called 'Azahar Emulator Room' (the row shows the name of the room, not only its address)"
# Luma3DS older than 13.0 has no Plugin Loader: the Azahar side of the app is shut (the switch OFF and locked, the room and the titles faint), at every start
need ConfigApp/source/ui.cpp "luma_too_old = setup::ClassifyLuma(facts.luma, facts.loader) == setup::LumaSupport::NotWork;" "the app asks at every start and every return to the main page whether Luma3DS is too old (the red verdict of the installation check)"
need ConfigApp/source/ui.cpp "if (luma_too_old && cfg.enabled) {" "with a Luma3DS that is too old 3DS Link goes OFF by itself, once"
need ConfigApp/source/pages_main.cpp "power.dim = lock != setup::SwitchLock::None;" "the 3DS Link switch is locked with a Luma3DS that is too old (and with a plugin that is not on the card)"
need ConfigApp/source/pages_main.cpp "room.dim = g.luma_too_old;" "the room row is faint with a Luma3DS that is too old"
need ConfigApp/source/pages_main.cpp "enable.dim = g.luma_too_old;" "the row of the titles for the Azahar emulator is faint with a Luma3DS that is too old"
[ "$(grep -c 'ShowToast("This Luma is not supported"' ConfigApp/source/pages_main.cpp)" = "3" ] || fail "ConfigApp/source/pages_main.cpp: A on a shut row (the switch, the room, the titles) must say, short, that this Luma is not supported (three places)"
need ConfigApp/source/pages_main.cpp 'message = "This Luma is not supported";' "the card of the main page says it short (the long sentence was too small to read)"
need ConfigApp/source/pages_main.cpp 'return ""; // (nothing between the buttons: the keys are the usual ones)' "the main page has no hint between its two buttons"
# the copies of the plugin in the games' folders are looked at once a start (an update of the app must not leave a game on the old plugin)
need ConfigApp/source/ui.cpp "worker.SubmitRefresh(tid, paths.plugin_file);" "every folder of sd:/luma/plugins gets a quiet look at its copy of the plugin once a start"
need ConfigApp/source/games.cpp "if (ReadSlot(tid).slot != Slot::On) {" "only a folder that holds our plugin and nothing else is brought up to date"
need ConfigApp/source/games.cpp "if (same_size && !FilesDiffer(GamePluginPath(tid).c_str(), source)) {" "a copy that is the plugin file is not written again"
need ConfigApp/source/pages_main.cpp 'Plugin updated in %d title%s.' "the main page says when it made copies of the plugin again"
[ "$(grep -c 'return "Azahar Emulator Room";' ConfigApp/source/pages_servers.cpp)" = "2" ] || fail "ConfigApp/source/pages_servers.cpp: the two pages of the rooms (the list and the form) are called like their row, Azahar Emulator Room"
need ConfigApp/source/pages_enable.cpp 'return "Enable Titles for Azahar Emulator Net";' "the page of the titles is called like its row"
need ConfigApp/source/pages_main.cpp 'delivery.label = "Titles Delivery";' "the page that sends and receives titles is called 'Titles Delivery'"
need ConfigApp/titleshare/source/main.c '3DS Link - Title delivery' "the module's own menu says '3DS Link - Title delivery'"
need ConfigApp/source/games.cpp '"%08lX%08lX"' "a game's folder is its title id in 16 UPPERCASE hex digits (Luma3DS looks for it by that name)"
need ConfigApp/source/games.cpp 'return static_cast<u32>(tid >> 32) == 0x00040000u;' "only application titles are games (not updates, DLC, system titles, applets, DSiWare)"
need ConfigApp/source/games.cpp 'return std::string(kPluginsDir) + "/" + TidText(tid);' "a game's plugin folder is sd:/luma/plugins/<TID>"
need ConfigApp/source/games.cpp "if (ReadSlot(tid).slot == Slot::Other) {" "a game whose folder holds another plugin is never touched"
need ConfigApp/source/games.cpp "rmdir(GamePluginDir(tid).c_str());" "disabling removes the folder too, but rmdir only takes an empty one"
need ConfigApp/source/games.cpp 'const std::string part = std::string(dst) + ".part";' "a copy is made as .part and put in place only when complete: never half a plugin"
[ "$(grep -c 'std::remove(' ConfigApp/source/games.cpp)" = "8" ] || fail "games.cpp removes a file in a place it did not (the only files the app deletes are a game's plugin copy and its .part, the .part of a failed copy, and the cache of the games when it is replaced)"
need ConfigApp/source/titles.cpp "setup::CurrentPaths().games_cache.c_str()" "the app's own cache of the titles (name and icon) is the one in the data folder"
need ConfigApp/source/titles.cpp "setup::ReadSmdhKeyed(keys, tid, buffer.data(), buffer.size()) == 0" "when the system refuses a game's icon file (a 3dsx on the consoles of TitleShare's logs) the icon is read with the user's keys"
need ConfigApp/source/titles.cpp "if (system_works < 0 && have_keys) {" "the system is not asked again once it refused and the keys are there (as TitleShare's catalog does)"
if grep -n -E 'fopen\([^)]*"[wa]|fwrite|std::remove|mkdir|rmdir|unlink' ConfigApp/source/ncch_icon.cpp; then
  fail "ncch_icon.cpp writes or removes something (it only reads a game's files)"
fi
if grep -n -E '\b(unlink|remove_all|rmdir)\b *\(' ConfigApp/source/*.cpp | grep -v 'games.cpp'; then
  fail "the app deletes something outside games.cpp (the only things it deletes are a game's plugin copy and the empty folder)"
fi
need ConfigApp/source/pages_enable.cpp "setup::CopyIsStale(info.copy_size, source_.size)" "a copy that is not the size of the plugin file is made again when the page opens"
need ConfigApp/source/pages_enable.cpp "row.label = e.name;" "a row is the game's name, never its id"
need ConfigApp/source/pages_enable.cpp "row.icon = e.game->icon;" "a row has the game's icon"
need_after ConfigApp/source/pages_enable.cpp "if (e.slot == setup::Slot::Other) {" "if (step == 0) {" "a row with another plugin in the folder cannot be switched (it only tells why)"
need ConfigApp/source/pages_enable.cpp "worker.Submit(game->tid, setup::CopyKind::Update, g.paths.plugin_file);" "a stale copy is made again in the background, like any other"
need ConfigApp/source/pages_enable.cpp "worker.Submit(e.game->tid, setup::CopyKind::Enable, g.paths.plugin_file);" "switching a title on queues its copy: the page never waits for the card"
need ConfigApp/source/pages_enable.cpp "worker.Submit(e.game->tid, setup::CopyKind::Disable, std::string());" "switching a title off queues its removal the same way"
need ConfigApp/source/copyworker.cpp "result.enable_result = EnableGame(job.tid, job.source.c_str());" "the worker thread is what makes the copies"
need ConfigApp/source/main.cpp "FinishCopies();" "the app waits for the copies still being made before it ends"
# The start-up flow. The FIRST start (no 3DSLink.cfg in the data folder) asks first and WRITES NOTHING until the answer is Yes: no data folder,
# no settings file, no cache, no check file. Yes starts the guided sequence (scan, boot9.bin, plugin file, plugin folder, the Plugin Loader
# question, the summary), and 3DSLink.cfg is written only after it: an app closed on the way asks again next time. Every other start scans in
# the background (a screen of its own only when there is no cache yet), asks about the Plugin Loader when it is off, and opens the main page.
need ConfigApp/source/main.cpp "ui::SetRootPage(pages::NewStartupPage());" "the first page is the start-up flow, not the main page"
need ConfigApp/source/main.cpp "g.first_start = setup::IsFirstStart(g.paths);" "the first start is 'no 3DSLink.cfg in the data folder'"
need_after ConfigApp/source/main.cpp "if (!g.first_start) {" "g.OpenDataFolder();" "only a start that is not the first one opens the data folder before it has asked"
need ConfigApp/source/pages_startup.cpp "g.quit = true; // (nothing was written: the first start comes again next time)" "No on the first start's question ends the app and writes nothing"
need_after ConfigApp/source/pages_startup.cpp "void Begin() {" "g.OpenDataFolder();" "the first start makes the data folder only after the answer Yes"
[ "$(grep -rn 'OpenDataFolder();' ConfigApp/source --include=*.cpp | wc -l)" = "2" ] || fail "the data folder is opened in a place that is not the start (a start that is not the first) or the first start's Yes"
need ConfigApp/source/pages_startup.cpp "const bool first = !titles::Scanner::CacheExists();" "the scan gets the whole screen only when the catalog cache is not there yet (and the first start always)"
need ConfigApp/source/pages_startup.cpp "if (g.first_start || first) {" "the first start always shows the scan, whatever cache a migration brought"
[ "$(grep -rn 'first_start_done = true' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "3DSLink.cfg is marked as done in more than one place (only at the end of the first start)"
need_after ConfigApp/source/pages_startup.cpp "void Finish() {" "if (g.first_start) {" "the end of the first start (and nothing before it) writes 3DSLink.cfg"
need ConfigApp/source/ui.cpp "if (!app_cfg.first_start_done) {" "3DSLink.cfg is never written before the first start is over"
need ConfigApp/source/pages_startup.cpp "PushPage(pages::NewInstallCheckPage([this](bool go_on) { summary_result_ = go_on ? 1 : 2; }, true));" "the first start ends with the summary: the installation check, each problem can be fixed from it; START goes on, B cancels"
need ConfigApp/source/pages_check.cpp "return summary_ ? \"Continue\" : \"Save & exit\";" "the first start's summary says Continue on START (it goes on to the main page, it does not leave the app)"
need ConfigApp/source/pages_startup.cpp "setup::ShouldOfferLoader(g.loader, g.cfg.enabled && g.cfg.tunnel)" "the Plugin Loader question is asked on the first start too, before the summary"
need ConfigApp/source/titles.cpp "catalog_build(&impl->ctx, &impl->catalog, OnProgress)" "the scan is the module's own catalog_build, with a progress count"
# The module's log is opened by its own sender and receiver, and by 3DS Link Delivery (the Download Play host has no module code of its own) through the bridge's
# dl_log_open / dl_log_close, which only the page of 3DS Link Delivery calls: no other file of the app, and no other line of the bridge, opens it.
if grep -rn -E 'ts_log_open|ts_log_close' ConfigApp/source; then
  fail "the app opens the delivery module's log (only its own sender and receiver, and the bridge's dl_log_open for 3DS Link Delivery, open it)"
fi
if grep -rn -E 'ts_log_open|ts_log_close' ConfigApp/titleshare_host | grep -v -E 'delivery\.c:[0-9]+:    ts_log_(open\(role\)|close\(\));$'; then
  fail "the bridge opens the delivery module's log somewhere other than dl_log_open / dl_log_close"
fi
if grep -rn -E 'dl_log_open|dl_log_close' ConfigApp/source | grep -v -E '^ConfigApp/source/pages_dlp\.cpp:'; then
  fail "the delivery module's log is opened from a place other than the page of 3DS Link Delivery"
fi
need ConfigApp/titleshare_host/delivery.c "void dl_log_open(const char* role) {" "the bridge opens the module's log for 3DS Link Delivery"
# The folders and the key file come from the app, by setters: the module has no place of its own (ConfigApp/titleshare_host/hostcfg.h)
need ConfigApp/source/module_config.cpp "hc_set_data_dir(paths.datadir.c_str());" "the module's data folder is the app's"
need ConfigApp/source/module_config.cpp "hc_set_boot9_path(boot9.found ? boot9.path.c_str() : \"\");" "the module's key file is the one the app found"
need ConfigApp/source/module_config.cpp "hc_set_log_enabled(cfg.debug > 0 ? 1 : 0);" "the module's log switch IS the Debug log switch (ONE switch for every log)"
need ConfigApp/titleshare/source/ctx.c "const char* path = hc_boot9_path();" "the module loads the boot9.bin the app found, from no place of its own"
need ConfigApp/titleshare/source/receiver.c "return hc_data_path(out, n, \"park\");" "the module's temporary files are in the app's data folder"
need ConfigApp/source/pages_config.cpp "g.ApplyModuleConfig(); // (title delivery follows the same switch)" "flipping the Debug log flips the delivery's log"
# The delivery's log: no file at all unless the switch is on, a name of its own, 64 MiB a file, 8 files, appended to
need ConfigApp/titleshare/source/log.c "if(!hc_log_enabled() || hc_data_dir()[0] == 0) {" "the delivery module creates no log file when the Debug log is off"
need_after ConfigApp/titleshare/source/log.c "if(!hc_log_enabled() || hc_data_dir()[0] == 0) {" "return;" "...and returns before it opens a file or starts a thread"
[ "$(grep -c 'fopen(' ConfigApp/titleshare/source/log.c)" = "1" ] || fail "ConfigApp/titleshare/source/log.c opens a file in more than one place (only log_open_current, behind the switch)"
[ "$(grep -c 'log_open_current();' ConfigApp/titleshare/source/log.c)" = "2" ] || fail "log_open_current is called from a place that is not the opening of the log (behind the switch) or a rotation"
need ConfigApp/titleshare/source/log.c 'fopen(path, "a")' "the delivery log is appended to, like the plugin's capture"
if grep -n 'log-prev\|log\.txt' ConfigApp/titleshare/source/log.c; then
  fail "the delivery log renames or writes the old log.txt again"
fi
need ConfigApp/titleshare_host/logrot.h "#define LOGROT_MAX_BYTES (64ull * 1024 * 1024)" "a delivery log file rotates at 64 MiB, like the plugin's capture"
need ConfigApp/titleshare_host/logrot.h "#define LOGROT_KEEP 7" "8 files at most: the current one and .1.log ... .7.log"
need ConfigApp/titleshare_host/logrot.c '"%s/3DSLink-TitleDelivery.log"' "the delivery log's name"
need ConfigApp/titleshare_host/logrot.c '"%s/3DSLink-TitleDelivery.%d.log"' "the older files of the delivery log"
need ConfigApp/titleshare/source/log.c "if(logrot_due(gBytes)) {" "the log rotates when a file is due"
# The Plugin Loader: the commands were checked against Luma3DS's sysmodules/rosalina/source/plugin/plgloader.c (the server), plgldr.c (its
# client) and CTRPluginFramework's plgldr.c: port "plg:ldr", command 2 = is it enabled (answer: result in word 1, state in word 2), command 3 =
# enable / disable (word 1 = state; answer: result in word 1). The app sends those two and nothing else, and asks only after a Yes.
need ConfigApp/source/system.cpp '#define AZL_PLG_PORT "plg:ldr"' "the Plugin Loader's port"
need ConfigApp/source/system.cpp "cmdbuf[0] = IPC_MakeHeader(2, 0, 0);" "plg:ldr command 2 (is the loader enabled?), as Luma3DS's plgloader.c checks it"
need ConfigApp/source/system.cpp "cmdbuf[0] = IPC_MakeHeader(3, 1, 0);" "plg:ldr command 3 (enable / disable), as Luma3DS's plgloader.c checks it"
need ConfigApp/source/system.cpp "state = cmdbuf[2] != 0 ? LoaderState::Enabled : LoaderState::Disabled;" "the state of the loader is word 2 of the answer to command 2"
need ConfigApp/source/system.cpp "cmdbuf[1] = enabled ? 1u : 0u;" "the new state of the loader is word 1 of command 3"
[ "$(grep -c 'cmdbuf\[0\] = IPC_MakeHeader(' ConfigApp/source/system.cpp)" = "2" ] || fail "system.cpp sends a request that is not one of the two it knows (plg:ldr 2 and 3): a command that was not verified must never be sent"
# The app never asks the launcher it was started from to run another program (the game delivery is TitleShare inside the app): the one
# service it connects to by name is the Plugin Loader, in its two places
[ "$(grep -rn 'svcConnectToPort(' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "2" ] || fail "the app connects to a service by name in a place it did not (only plg:ldr: reading and switching the Plugin Loader)"
if grep -rn 'hb:ldr' ConfigApp/source ConfigApp/titleshare_host; then
  fail "the app talks to hb:ldr again (the game delivery runs TitleShare inside the app; no other program is started)"
fi
[ "$(grep -rn 'SetPluginLoader(' ConfigApp/source --include=*.cpp | grep -v 'ConfigApp/source/system.cpp' | wc -l)" = "3" ] || fail "the Plugin Loader is switched in a place it was not (the question after a Yes, A on the Installation check's row, and the build for trying it on an emulator)"
need ConfigApp/source/paths.cpp 'lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "a boot9 dump is any .bin with boot9 in its name (GodMode9 2.x: <serial>_boot9_00.bin), and the boot11 dump of the same size is never taken for it"
need ConfigApp/source/paths.cpp "const std::string hit = FindBoot9InFolder(dir);" "when no file is called boot9.bin the places the app looks in by itself are looked into for such a name"
need ConfigApp/source/pages_check.cpp "return name.size() > 4 && setup::SameNameNoCase(name.substr(name.size() - 4), \".bin\");" "the search of the card looks at every .bin: the content (or the name GodMode9 gave it) says which is boot9"
need ConfigApp/source/pages_check.cpp "setup::FindBoot9InFolder(folder)" "so does the choice of a folder"
need ConfigApp/source/search.cpp "options_.name_match ? options_.name_match(name) : Lower(name) == file_name_" "the search can test a name instead of comparing it with one"
# The fifth round of the app (the user's fourth list): the tags that touch, the sender's screen, the receivers' names, a boot9 told by its content
need ConfigApp/source/ui.cpp "x = std::floor(x - 9.0f);" "the coloured bars of the kinds (game, update, DLC) are on whole pixels (one drawn between two pixels looked a pixel off its neighbour)"
need ConfigApp/source/ui.cpp "x = std::floor(x - 13.0f);" "the bars of the legend are on whole pixels too"
need ConfigApp/source/ui.cpp "float DrawKindMarks(float right_edge, float yc, unsigned tags) {" "the kind of a title is ONE small coloured bar with no letter, everywhere (a list, the offer, the receiving list)"
! grep -rn "DrawKindTags" ConfigApp/source || fail "the plates with the letters G U D are back (DrawKindTags): the kinds are small coloured bars with no letter everywhere"
! grep -n '{kTagGame, "G"' ConfigApp/source/ui.cpp || fail "a letter G in a plate of the kinds is back"
need ConfigApp/source/delivery_run.cpp "constexpr int kShownGroups = 3;" "the sending screen has room for three lines of what is offered"
need ConfigApp/source/delivery_run.cpp "first = Clamp(at - 1, 0, total_groups - kShownGroups);" "...and they are always the ones around the title that is being sent (its green dot never goes out of sight)"
need ConfigApp/source/delivery_run.cpp 'Fmt("sending speed: %.2f MiB/s", s_.mibps)' "the room card says how fast the radio sends (not the name of the title)"
need ConfigApp/source/delivery_eta.cpp "double n = size[t] * (1.0 - Clamp01(r.down[t]));" "the time left counts, title by title, what each receiver still lacks of it (a console that joins and wants a title the others have got makes it count again)"
need ConfigApp/source/delivery_run.cpp 'DrawPair(x, y3 + 53, 0.4f, "to go",' "the details of a receiver say how many titles it asked for, has downloaded, has installed and has still to go"
need ConfigApp/source/delivery_run.cpp '"SD free"' "...and the space it has on its card"
! grep -q "DrawNowLine" ConfigApp/source/delivery_run.cpp || fail "the details of a receiver name the titles again (there can be twenty: it gets numbers, not names)"
need ConfigApp/source/delivery_run.cpp 'DrawPopupBox(box, "Leave the room?", kLeaveText);' "B on the receiving screen asks before leaving the room"
need ConfigApp/source/delivery_run.cpp 'const std::string completed = "completed: " + Percent(static_cast<int>(s.sender_next));' "the share of the whole operation is called what it is"
! grep -q '"You: ' ConfigApp/source/delivery_run.cpp || fail "the receiving screen lists this console among the others (it is the card above)"
need ConfigApp/titleshare_host/delivery.c "node_name(st->otherNode[i], s.other_name[i], sizeof(s.other_name[i]), !st->reconnecting);" "the names of the other receivers are the names their consoles have on the wireless"
need ConfigApp/titleshare/common/proto.c "if(n >= base + 1 + (size_t) c * 3 && in[base] == (uint8_t) c) {" "the roster's trailer is read only when it is there and its count matches (an older sender sends none)"
need ConfigApp/titleshare/common/proto.c "t[0] = (uint8_t) r->n;" "...and written after the entries the frame always had"
need ConfigApp/titleshare/source/sender.c "ro.node[ro.n] = st.cNode[ro.n];" "the sender says which node each receiver speaks from"
need ConfigApp/titleshare/source/receiver.c "n->otherState[n->nOthers] = ro.ext ? ro.state[i] : 255;" "a receiver that gets no trailer shows no state (255)"
need ConfigApp/source/paths.cpp '{0x10000, "2F88744FEED717856386400A44BBA4B9CA62E76A32C715D4F309C399BF28166F", Boot9Verdict::Good},' "the bootrom of a retail console is known by its SHA-256, whatever the file is called"
need ConfigApp/source/paths.cpp '{0x8000, "DF95ED5B358ED93F2738BAE9E0FC2E4EF1865A62ABA9FCC54A4E0898301F97B4", Boot9Verdict::NoKeys},' "...the lower half alone, which has the size of a boot9 and no keys, is known too"
need ConfigApp/source/paths.cpp "return FileSizeOf(path, size) && ClassifyBoot9File(path) != Boot9Verdict::NoKeys;" "a file that is the lower half of the bootrom is no boot9, even when it is called boot9.bin"
need ConfigApp/source/pages_check.cpp "setup::ClassifyBoot9File(path) == setup::Boot9Verdict::Good" "the search of the card takes a file by its content too"
need ConfigApp/source/delivery_run.cpp 'Choosing it replaces the installed copy: the title and its data are deleted first.' "what the top screen says of an installed title is what choosing it does, not that it is installed (the list says that)"
need ConfigApp/source/pages_check.cpp "const Result rc = setup::SetPluginLoader(true);" "A on the Installation check's Plugin Loader row switches it on, through the service"
need ConfigApp/source/pages_check.cpp "return row && row->kind == Kind::Link ? \"Fix\" : \"\";" "a line that is fine has no A button (nothing to fix)"
need_after ConfigApp/source/main.cpp "#ifdef AZL_TEST_DISABLE_LOADER" "setup::SetPluginLoader(false);" "the only other switch of the loader is a build made to try the question"
need ConfigApp/source/pages_startup.cpp "setup::ShouldOfferLoader(g.loader, g.cfg.enabled && g.cfg.tunnel)" "the question is asked only for a loader that is known to be off, with 3DS Link on"
need ConfigApp/source/pages_startup.cpp "const Result rc = setup::SetPluginLoader(true);" "Yes switches the loader on directly, through the service"
need ConfigApp/source/status.cpp "return state == LoaderState::Disabled && link_on;" "an unknown state never asks"
# The installation check: the plugin file (searched on the whole card when it is not next to the app), the plugin folder (made on A), the card, and
# boot9.bin (a problem the main page names, found by a search, a folder or a how-to), and nothing about the settings file, which the app makes
# by itself. default.3gx is information only: 3DS Link never uses it, and the project no longer offers one.
need ConfigApp/source/status.cpp '"boot9.bin missing"' "boot9.bin missing is a problem the main page names, with where to go"
need ConfigApp/source/status.cpp 'MakeItem("boot9.bin", "missing", CheckLevel::Warning,' "a missing boot9.bin is a WARNING, not a problem: 3DS Link works without it (only sending titles needs it); the main page and the summary name it in yellow"
need ConfigApp/source/status.cpp "default.3gx would apply to every title; 3DS Link does not use it" "default.3gx is only told about"
need ConfigApp/source/status.cpp 'CheckAction::RestorePlugin, "plugin file missing"' "a missing plugin file is fixed by putting back the plugin that is inside the app"
need ConfigApp/source/status.cpp 'CheckAction::CreatePluginsDir, "plugin folder missing"' "a missing plugin folder is made on A"
need ConfigApp/source/status.cpp 'CheckAction::FindBoot9, "boot9.bin missing"' "a missing boot9.bin is fixed by the three ways"
need ConfigApp/source/status.cpp 'constexpr const char* kWhereToGo = " - A on Installation check";' "the main page says where to go"
if grep -n 'Settings file\|ini_present\|ini_all_keys' ConfigApp/source/status.cpp ConfigApp/source/status.hpp ConfigApp/source/system.cpp; then
  fail "the installation check has a settings-file row again (the app makes AzaharLink.ini by itself)"
fi
[ ! -e dist/default.3gx ] || fail "dist/default.3gx is back (3DS Link does not use a default plugin: a plugin is enabled per title)"
need ConfigApp/source/paths.cpp "for (const std::string& place : {paths.boot9_beside, paths.boot9_in_data, paths.boot9_godmode, saved_path}) {" "boot9.bin is looked for next to the app, in the app's own folder, in GodMode9's folder, then where the user pointed to"
need ConfigApp/source/paths.cpp "list.push_back(place);" "...each place once"
need ConfigApp/source/paths.cpp "return size == 0x10000 || size == 0x8000;" "a boot9.bin is 64 KB or 32 KB"
need ConfigApp/source/ui.cpp "boot9 = setup::FindBoot9(paths, app_cfg.boot9_path);" "the key file is looked for where the app is, not in a fixed place"
need ConfigApp/source/pages_check.cpp '{"Search the whole SD card", "Choose a folder", "How to get it"}' "the popup of a missing boot9.bin has its three choices"
need ConfigApp/source/pages_check.cpp "if (setup::MakeDirs(setup::kPluginsDir)) {" "A on a missing plugin folder makes it"
need ConfigApp/source/search.cpp 'lower == "nintendo 3ds"' "the search never goes into the console's own Nintendo 3DS folder"
need ConfigApp/source/search.cpp "queue_.emplace_back(JoinPath(dir_path_, name), dir_depth_ + 1);" "the search goes through the folders breadth first"
need ConfigApp/source/pages_enable.cpp "g.paths.plugin_file" "Enable Titles copies the plugin from the file the app found (next to it, or in its folder)"
[ "$(grep -c 'g.paths.plugin_file' ConfigApp/source/pages_enable.cpp)" -ge "3" ] || fail "pages_enable.cpp looks for the plugin file in a place that is not the app's"
[ ! -d ConfigApp/source/files ] && ! grep -rn 'FilesPage\|NewFilesPage' ConfigApp/source || fail "the Files page is back (the Configuration page has no Files row)"
# The delivery module is the original plus the documented patches (ConfigApp/titleshare/PATCHES.md): only a path, a text the user sees and the log
# layer are changed, Tests/titleshare_vendor_check.sh proves it (the copy is MANIFEST.md5, the copy minus vendor.patch is ORIGINALS.md5, the
# originals in ../TitleShare plus the patch are the copy), and the app builds all of it, listed by name, and runs the module's own main()
if ! VENDOR_OUT=$(bash Tests/titleshare_vendor_check.sh 2>&1); then
  echo "$VENDOR_OUT"
  fail "the copy of the delivery module in ConfigApp/titleshare is not the original plus its documented patches (Tests/titleshare_vendor_check.sh)"
fi
need Tests/titleshare_vendor_check.sh 'ALLOWED="common/proto.c common/proto.h source/catalog.c source/ctx.c source/log.c source/log.h source/main.c source/net.h source/power.c source/receiver.c source/receiver.h source/sender.c source/sender.h"' "only these thirteen files of the module may be patched (the last six on the user's requests, 2026-10-16 and 2026-10-17)"
need Tests/titleshare_vendor_check.sh "patch -R -p1" "the proof applies the patch backwards to the copy and compares the md5 of the originals"
need Tests/titleshare_vendor_check.sh 'cmp -s "$TMP/$f" "$DIR/$f"' "where the originals are, original plus patch is compared with the copy byte for byte"
need ConfigApp/titleshare/PATCHES.md "the thirteen patches" "the patches are documented"
need_line ConfigApp/Makefile $'TS_COMMON\t:=\taes catcache cia_front ciastream contentpath flow ncchname pace proto room runcache sched sdcrypt seed sha256 store win' "the module files the app builds, by name (all of common/)"
need_line ConfigApp/Makefile $'TS_SOURCE\t:=\tamsink catalog ctx log power receiver sender' "the module files the app builds, by name (all of source/ but main.c: the module's own text screens are not built)"
need_line ConfigApp/Makefile $'TS_HOST\t\t:=\tdelivery hostcfg logrot' "the bridge to the module (the one file that includes its headers for the app), with what the app tells the module and the log's rotation"
need_line ConfigApp/Makefile $'SOURCES\t\t:=\tsource ../Sources/azl ../Sources/enet' "TitleShare's folders are not scanned for sources: its files are built from the list only"

# 3DS Link Delivery: this console as a Download Play host (pages_dlp.cpp, over the library in ConfigApp/dlph, which is DlpLink/host unchanged:
# Tests/dlph_vendor_check.sh proves it against ConfigApp/dlph/MANIFEST.md5). What the entry promises is pinned here, because a slip in any of it
# shows only on two consoles: the room of two made again after every console, the pause between fragments the hardware gave, ONE thread that makes
# every call into the library, the thread waited for in a way that cannot read a timeout as success, sleep and HOME off while the room is up, the
# question before cutting a console off, and a menu row that does not depend on the Plugin Loader.
if ! DLPH_OUT=$(bash Tests/dlph_vendor_check.sh 2>&1); then
  echo "$DLPH_OUT"
  fail "the copy of the Download Play host library in ConfigApp/dlph is not DlpLink/host unchanged (Tests/dlph_vendor_check.sh)"
fi
need Tests/dlph_vendor_check.sh 'SOURCES="dlph.c dlph.h dlph_wire.c dlph_wire.h"' "the library is these four files and the app builds exactly them"
need_line ConfigApp/Makefile $'DLPH\t\t:=\tdlph dlph_wire' "the library files the app builds, by name"
need ConfigApp/Makefile '$(DLPH_OFILES) : %.o : $(TOPDIR)/dlph/%.c' "the library is compiled from its own folder, with the module's flags"
need ConfigApp/Makefile 'export OFILES := $(OFILES_BIN) $(OFILES_SOURCES) $(TS_OFILES) $(DLPH_OFILES)' "the library is linked"
need ConfigApp/source/pages_dlp.cpp "cfg.max_nodes = 2; " "3DS Link Delivery is a room of two places: this console and ONE seat"
need ConfigApp/source/pages_dlp.cpp "cfg.persistent = false;" "the room is made again after every console (a used seat is dirty: the next console stays half-connected for 44-49 s)"
need ConfigApp/source/pages_dlp.cpp "constexpr int kGapUs = 3000;" "the pause between two fragments is the one measured on hardware (no pause is slower: 17 s against 10 s, 2.4 times the bytes)"
need ConfigApp/source/pages_dlp.cpp "cfg.gap_us = kGapUs;" "the library is given that pause"
need ConfigApp/source/pages_dlp.cpp "constexpr u32 kRadioStack = 128 * 1024;" "the thread of the library has a stack of its own"
need ConfigApp/source/pages_dlp.cpp "prio > 0x19 ? prio - 1 : prio" "the radio thread is a little more important than the screen (a frame must not make it miss its rhythm)"
need ConfigApp/source/pages_dlp.cpp "threadJoin(radio_->thread, U64_MAX);" "the thread is waited for with no timeout (a timeout reads as success on the 3DS)"
need ConfigApp/source/pages_dlp.cpp "svcSleepThread(10ULL * 1000000ULL);" "dlph_poll does not wait when idle: the thread sleeps between two calls (it needs at least 10 a second)"
need ConfigApp/source/pages_dlp.cpp "dlph_request_kick();" "the user can send the console in the room away"
need ConfigApp/source/pages_dlp.cpp "dl_power_begin();" "sleep is refused and HOME is off while the room is up (a console asleep drops the room)"
need ConfigApp/source/pages_dlp.cpp "dl_power_end();" "...and given back when it is closed"
need ConfigApp/source/pages_dlp.cpp "g.HoldHome();" "HOME is off while the room is up, and the bar says so"
need ConfigApp/source/pages_dlp.cpp "g.ReleaseHome();" "...and given back when it is closed"
need ConfigApp/source/pages_dlp.cpp "wireless_.Close(); " "the local wireless goes back to the app when the room is closed"
need ConfigApp/source/pages_dlp.cpp "dlp::CloseNeedsQuestion(stats_.seat)" "closing the room asks first when a console is in it (and only then)"
need ConfigApp/source/pages_dlp.cpp "if (dl_screens_off()) {" "with the backlights out only SELECT counts"
need ConfigApp/source/pages_dlp.cpp "std::memcmp(soc, shared.data(), 6) != 0" "the two sources of this console's address are compared and the log says when they differ"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kCloseQuestion = "A console that has not finished will stop receiving.";' "the question before closing the room"
need ConfigApp/source/dlp_text.hpp 'return "Had it already"; // it said it has this version: nothing was sent' "a console that already had the app is never reported as delivered"
need ConfigApp/source/paths.cpp "p.dlp_child = JoinPath(p.dlp_dir, kDlpChildFileName);" "the package that is handed out is <data folder>/dlp/child.cia"
need ConfigApp/source/delivery.hpp "ui::Page* NewDlpHostPage();" "the page of 3DS Link Delivery is made through delivery.hpp"
need ConfigApp/source/pages_main.cpp "dlp.label = \"3DS Link Delivery\";" "the main page has the entry 3DS Link Delivery"
need ConfigApp/source/pages_main.cpp "PushPage(pages::NewDlpHostPage());" "...and A opens its page"
[ "$(tr -d '\r' < ConfigApp/source/pages_main.cpp | grep -c '\.gap')" = "2" ] || fail "ConfigApp/source/pages_main.cpp puts a gap above 3DS Link Delivery (the user wants none: with Titles Delivery it is one service in two parts)"   # (exactly the two there are: above Titles Delivery and above Settings)
need ConfigApp/source/pages_main.cpp "enum Id { kPower = 1, kRoom, kEnableGame, kGameDelivery, kDlpDelivery, kSettings };" "the entry is between Titles Delivery and Settings"
[ "$(grep -c 'dlp\.dim.*luma_too_old' ConfigApp/source/pages_main.cpp)" = "0" ] || fail "the 3DS Link Delivery row is shut when Luma is too old (like Titles Delivery and Settings it does not need the Plugin Loader; it is shut by its own switch and by a missing boot9.bin, never by this)"
need ConfigApp/source/main.cpp "if (!dl_skip_drawing() && gfx::BeginFrame()) {" "a frame is not drawn with the backlights out or the shell closed while a room is up"
# only the thread of the page calls into the library; the page reads the snapshot and asks for a kick from the screen's thread
[ "$(grep -rn -E 'dlph_(open|start|poll|stop|close)\(' ConfigApp/source | wc -l)" = "5" ] || fail "the library's open / start / poll / stop / close are called from more or fewer places than the thread of the page (RadioMain)"
[ "$(grep -rn -E 'dlph_(open|start|poll|stop|close)\(' ConfigApp/source | grep -v '^ConfigApp/source/pages_dlp\.cpp:' | wc -l)" = "0" ] || fail "the Download Play host library is called from outside the page of 3DS Link Delivery"
need_after ConfigApp/source/pages_dlp.cpp "        dlph_stop(); // (before the wireless is closed" "}" "the room is destroyed by the thread, before the page closes the wireless"
if tr -d '\r' < ConfigApp/Makefile | grep -n -E 'wildcard.*titleshare'; then
  fail "ConfigApp/Makefile builds TitleShare with a wildcard (its files are built by name)"
fi
if tr -d '\r' < ConfigApp/Makefile | grep -n -E -e '-Dmain|-Dmain=|-D *main='; then
  fail "ConfigApp/Makefile renames a main with a flag (the host file renames TitleShare's, for that file only)"
fi
# Titles Delivery: the module's own text screens (its main.c) are NOT built; the app draws the screens and reaches the module through the one bridge,
# ConfigApp/titleshare_host/delivery.c (the one file that includes the module's headers for the app). What the module does is called as it is.
need ConfigApp/titleshare_host/delivery.c "return sender_run(gCtx, &gMan, send_adapter, NULL);" "sending is the module's own sender_run"
need ConfigApp/titleshare_host/delivery.c "return receiver_run(gCtx, &gPicked, recv_adapter, NULL);" "receiving is the module's own receiver_run, in the room that was picked"
need ConfigApp/titleshare_host/delivery.c "sender_check_title(gCtx, gCat->items[item].tid, &t, why, sizeof(why))" "a title is checked by the module before it is offered"
need ConfigApp/titleshare_host/delivery.c "udsInit(TS_SHARED_MEM, NULL)" "the wireless is opened as the module opened it"
need ConfigApp/titleshare_host/delivery.c "return answer == DL_CANCEL && st->phase <= 1 ? TS_UI_CANCEL : TS_UI_NONE;" "the room can be closed in its first two phases only, as in the module"
if grep -rn 'main_host\|titleshare_main\|titleshare_reset_state' ConfigApp/source ConfigApp/titleshare_host ConfigApp/Makefile; then
  fail "the module's own main() is run again (the app draws the Titles Delivery screens itself)"
fi
[ "$(grep -rn -E '#include +["<][^">]*\.c[">]' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "a .c file is included where it was not (no file includes another one)"
need ConfigApp/source/delivery.cpp "session::SuspendNetwork();" "the sockets and the network status are put away while the module has the local wireless (it found a console with none of them open)"
need ConfigApp/source/delivery.cpp "dl_power_exit();" "what the module held (the backlights, the apt hook) is given back when the wireless is closed"
need ConfigApp/source/delivery.cpp "threadCreate(Trampoline," "the module's loops call the screens from a thread with a big stack"
need ConfigApp/source/delivery_run.cpp "RunOnBigStack([&]() { rc = dl_send_run(&SendTick, &run); });" "the room runs on the big stack"
need ConfigApp/source/delivery_run.cpp "RunOnBigStack([&]() { rc = dl_recv_run(&ReceiveTick, &run); });" "the receiving runs on the big stack"
need ConfigApp/source/delivery_run.cpp "DrawPage(*page, wait_for_screen);" "a transfer's frame never waits for the screen; only the choice does"
need ConfigApp/source/delivery_run.cpp "DrawPage(run->page, false);" "the sender's frames never wait for the screen (the radio loop must not stand still)"
need ConfigApp/source/pages_delivery.cpp "dl_scan_stop();" "the scan for rooms is stopped before a room is joined"
need_after ConfigApp/source/pages_delivery.cpp "StopScan();" "delivery::RunReceive" "the scan for rooms is stopped just before a room is joined"
need ConfigApp/source/pages_delivery.cpp "g.HoldHome();" "HOME is off while the list of rooms is up (a press in the middle of a scan once froze a console)"
# The fourth round of the app (the user's third hardware test): the same bar on every top screen, HOME and START while the plugin is copied, the
# loading popup, the receivers' names and their details, the join of a console to a room that is already sending, and the plain words of the screens
[ "$(grep -rn 'DrawTitleBar(gfx::kTopW' ConfigApp/source --include=*.cpp | wc -l)" = "0" ] || fail "a top screen has its own bar again (the version only, no battery and card): they all use DrawTitleBarStatus"
need ConfigApp/source/ui.cpp "limit = bx - 8.0f;" "the title of the top bar keeps to the left of the battery and the card (a long title is cut, the version goes when it does not fit)"
need ConfigApp/source/ui.cpp "return setup::CopyWorker::Get().PendingCount() == 0;" "START does nothing while the plugin is being copied"
need ConfigApp/source/ui.cpp "aptSetHomeAllowed(home_holds == 0);" "HOME is given back only when the last page that switched it off lets go"
need ConfigApp/source/main.cpp "g.HoldHome();" "HOME is off while the plugin is being copied into the folders of the titles"
need ConfigApp/source/pages_enable.cpp 'Fmt("%d / %d ON", on, static_cast<int>(entries_.size()))' "the bar of the page of the titles says how many are ON"
need ConfigApp/source/pages_delivery.cpp "PushPage(new LoadingPage(make));" "a list that takes a moment to build is opened behind a Loading popup"
need ConfigApp/source/pages_delivery.cpp 'return "Select titles to share";' "the bar of the list of titles to send says what the page is for"
need ConfigApp/source/pages_delivery.cpp "g.HoldHome(); // (the choice of what to send" "HOME is off on the page where the titles to send are chosen (the room that follows would break), and the bar says so"
need ConfigApp/source/delivery_run.cpp 'return "Choose what to download";' "the bar of the list of what to receive says what the page is for"
need ConfigApp/source/pages_delivery.cpp 'return "Options"; // (not "Titles Delivery" once more' "the menu of Titles Delivery does not repeat its name on the bottom bar"
[ "$(grep -c 'FooterKey{"SELECT"' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "the sending screen and the receiving screen both have the Screens off button"
! grep -q -i "speed mode" ConfigApp/source/delivery_run.cpp || fail "a transfer screen talks about the speed mode again (it is a setting of the ini file)"
need ConfigApp/source/delivery_run.cpp "const bool stale = s.data_age_ms > 2500 && !no_more;" "when no data comes in the receiving screen says 0 MiB/s and no time, not what it saw last (but a console that needs no more data is not stale)"
need ConfigApp/source/delivery_run.cpp 'Fmt("reconnected %d time%s", s.reconnects, s.reconnects == 1 ? "" : "s")' "the number of times the connection was lost is said once, in the line of the state"
need ConfigApp/source/delivery_run.cpp '"repeated packets"' "the packets that were repeated are called that"
need ConfigApp/source/delivery_run.cpp "row.label_color = KindColor(s.title[i].kind);" "the list of what to receive has the same colours as the list of what to send"
need ConfigApp/source/ui.cpp 'gfx::TextWidth("1023.9 MB", 0.42f)' "the size of a row of a list with boxes is a column of its own (the chips before it stay in line)"
need ConfigApp/titleshare/source/sender.c "static void note_node(uint16_t token, uint16_t node) {" "the sender knows which node each receiver is, to say its name"
need ConfigApp/titleshare/source/sender.c "joinHoldUntil = now + 500;" "a console that joins a room that is sending stops the blocks for half a second..."
need ConfigApp/titleshare/source/sender.c "holdCooldownUntil = now + 2000;" "...at most once every two seconds (consoles that join one after the other do not stall the room)"
need ConfigApp/titleshare/source/sender.c "joinBoostUntil = now + 1000;" "...and gets the manifest five times in a second (the second starts again for every new node)"
need ConfigApp/titleshare/source/sender.c "now - lastManifest >= (now < joinBoostUntil ? 200u : 1000u)" "...every 200 ms while the boost lasts"
need ConfigApp/titleshare/source/sender.c "if(r != (Result) 0xC86113F0) {" "a control frame (the manifest, the state, the roster) is sent again while the radio queue is full"
# The second hardware test (2026-10-17): the name of a receiver's console stays (by token), two new states, a failed title leaves nothing, the messages, the channel
need ConfigApp/titleshare/source/sender.c 'snprintf(st->cname[k], sizeof(st->cname[k]), "%s", name_of(cl->token));' "the name of a receiver is the last good one the sender knew (a failed lookup does not make it Receiver N)"
need ConfigApp/titleshare/source/sender.c "st->cstate[k] = err_of(cl->token) ? 6 : 2;" "a receiver that finished with a failed title is told apart from one that finished"
need ConfigApp/titleshare/source/sender.c "st->cstate[k] = 5; // it still needs blocks, but not of the title that is being sent: it waits for its turn" "a receiver that waits for its turn is not shown as downloading"
need ConfigApp/titleshare/source/sender.c "note_err(token, (flags & TS_F_ERR) != 0);" "the sender learns from the acknowledgement that a receiver finished with an error"
need ConfigApp/titleshare/source/sender.c "udsGenerateDefaultNetworkStruct(&ns, TS_WLANCOMM_ID, 0, TS_NET_NODES);" "the room takes the sender and seven receivers"
need ConfigApp/titleshare/source/net.h "#define TS_NET_NODES 8" "eight nodes: the sender and seven receivers"
! grep -q "TS_SILENCE_MS\|TS_CLIENT_TIMEOUT_MS\|TS_PAUSE_MAX_MS" ConfigApp/titleshare/source/net.h || fail "ConfigApp/titleshare/source/net.h has a dead constant again (TS_SILENCE_MS, TS_CLIENT_TIMEOUT_MS or TS_PAUSE_MAX_MS: nothing uses them and their comments were false)"
need ConfigApp/titleshare/source/net.h "char firstName[48];" "the beacon carries the whole name of the first title (after the 29 bytes an older receiver reads)"
need ConfigApp/titleshare/source/receiver.c "ackFlags |= TS_F_ERR; // (the sender shows \"finished with errors\")" "an acknowledgement tells the sender that a title failed"
[ "$(grep -c 'scrub_failed(wt, m->t\[cur\]\.tid);' ConfigApp/titleshare/source/receiver.c)" = "5" ] || fail "ConfigApp/titleshare/source/receiver.c: a title that fails in AM (a refused write, an unreadable park file twice, a refused end) or is stopped half way must be taken away with scrub_failed, five places"
need ConfigApp/titleshare/source/receiver.c "Result rt = AM_DeleteTitle(dest, tid);" "what a failed title leaves is deleted: that title and its ticket only"
need ConfigApp/titleshare/source/receiver.c 'snprintf(st.msg, sizeof(st.msg), "tutto scaricato: installo");' "a console that has everything and only installs does not say that the sender serves others"
need ConfigApp/titleshare/source/receiver.c "n->reconnects++; // (only the ones that worked)" "the number of reconnections counts the ones that worked, not the attempts"
need ConfigApp/titleshare/source/receiver.c "n->reconnFull = maxRoom > 0 && inRoom >= maxRoom;" "the room is said to be full only when its beacon says so"
need ConfigApp/titleshare/source/receiver.c 'ts_log("fine ricezione: risultato %d (%s)' "the log says why a receiving ended"
need ConfigApp/titleshare_host/delivery.c '"The room is full (%d/%d), still trying (%u s)"' "the screen says the room is full, with how many are in it, and that it tries again"
need ConfigApp/titleshare/source/power.c "static void power_thread(void* arg) {" "the shell state and the periodic system line are asked in a thread of their own, not in the interface's loop"
need ConfigApp/titleshare/source/power.c "if(!ts_log_active()) {" "the periodic system line costs nothing when the Debug log is off"
need ConfigApp/titleshare/source/power.c 'ts_log("chiamata lenta: %s %llu ms"' "a call of the system that takes long is written in the log"
need ConfigApp/source/delivery_run.cpp "static constexpr int kShownRows = 5;" "the bottom screen of a room shows five receivers (with seven it scrolls)"
need ConfigApp/source/delivery_run.cpp 'return seconds > 7200.0 ? std::string("over 2 h") : ShortDurationText(seconds);' "the time of one receiver says over 2 h beyond two hours"
need ConfigApp/source/delivery_run.cpp '"download starts in " + (mine.wait > 7200.0' "a receiver whose first title has not started is told when the download starts, not how long is left"
need ConfigApp/source/delivery_run.cpp 'return "waiting";' "a receiver that waits for its turn says so"
need ConfigApp/source/delivery_run.cpp "word += Fmt(\" %d%%\", static_cast<int>(pct > 99 ? 99 : pct));" "the title that installs says how far it is, on its row"
need ConfigApp/source/delivery_run.cpp "RefreshStatus(run->last_status_read, false);" "the receiving screen does not ask the card itself (the card may be busy with the install)"
need ConfigApp/source/ui.cpp 'const std::string chan = Fmt("Wi-Fi CH %d", wifi_channel);' "the bar of the transfer screens says the Wi-Fi channel of the room"
need ConfigApp/source/ui.cpp "latch.down_n += (down & KEY_DOWN) ? 1 : 0;" "the latch counts the presses of the keys that move a list"
need ConfigApp/source/status.cpp "return (major == 13 && minor < 3) ? LumaSupport::Maybe : LumaSupport::Tested;" "Luma3DS 13.3 and newer is tested, 13.0 to 13.2 maybe"
need ConfigApp/source/status.cpp "return loader != LoaderState::Unknown ? LumaSupport::Maybe : LumaSupport::NotWork;" "a Luma3DS older than 13.0 has no Plugin Loader: not work (unless it answers as one)"
need ConfigApp/source/pages_delivery.cpp 'row.chip = Fmt("CH %d", room.channel);' "a room of the list says its Wi-Fi channel"
need ConfigApp/source/pages_delivery.cpp '"\nEstimated time to download all: about "' "a room of the list says how long it takes to get it all, as an estimate"
need_line ConfigApp/Makefile $'APP_AUTHOR\t\t:= Powered by Gionko' "the author of the app in the home menu"
[ -f ConfigApp/tools/make_icon.py ] || fail "ConfigApp/tools/make_icon.py is missing (the icon is drawn by it: icon.png can be made again)"
# The icon has NO alpha channel: an SMDH icon is RGB565 and smdhtool flattens the PNG onto BLACK, so transparent corners come out as black wedges on HOME, in the
# Homebrew Launcher and in the Download Play list. icon.png is a 48x48 PNG of colour type 2 (RGB), and its generator never puts an alpha channel back.
[ "$(od -An -tu1 -j16 -N10 ConfigApp/icon.png | tr -s ' ')" = " 0 0 0 48 0 0 0 48 8 2" ] || fail "ConfigApp/icon.png must be a 48x48 PNG without alpha (an SMDH icon has none: smdhtool turns transparency into black corners)"
[ "$(grep -c -E 'putalpha[(]|convert[(].RGBA.[)]' ConfigApp/tools/make_icon.py)" = "0" ] || fail "ConfigApp/tools/make_icon.py makes an alpha channel again (the corners of the icon would turn black on HOME)"
need ConfigApp/titleshare/source/sender.c "send_ctl(frame, rn);" "...the roster too"
need ConfigApp/titleshare/source/sender.c "if(serving > 0 && now >= joinHoldUntil && sched_next(sc, now, &g)) {" "the blocks wait while a console is joining"

need ConfigApp/source/pages_delivery.cpp "if (row.id == kSend && ready_ && !dl_have_keys()) {" "Send Titles cannot be opened without boot9.bin"
need ConfigApp/source/pages_delivery.cpp "if (send && !dl_have_keys()) {" "...also when it was asked for while the titles were still being read"
# The third round of the app (after the first hardware test): the two new checks, the files of earlier builds, the room test, the screens of the
# delivery (what they say and how they answer to the keys)
need ConfigApp/source/system.cpp '"/data/" + id0 + "/sysdata/0001000f/00000000"' "the seed database is the system save 0001000F of this console's ID0, read from the NAND (boot9.bin is not needed for it)"
need ConfigApp/source/status.cpp 'MakeItem("Seed database", "not readable", CheckLevel::Warning,' "a seed database that cannot be read is a warning (receiving works)"
need ConfigApp/source/status.cpp 'MakeItem("Plugin Loader", "disabled", CheckLevel::Problem,' "a Plugin Loader that is off is a problem (no title can load the plugin)"
need ConfigApp/source/status.cpp 'CheckAction::EnableLoader, "Plugin Loader off"' "...that A fixes"
need ConfigApp/source/ui.cpp "setup::InstallFacts facts = setup::ReadInstallFacts(paths, boot9, loader);" "the check reads the Plugin Loader, Luma3DS and the seed database every time"
[ "$(grep -rn 'MigrateOldFiles' ConfigApp/source --include=*.cpp --include=*.hpp | wc -l)" = "0" ] || fail "the files of earlier builds are brought over again at the first start"
need ConfigApp/source/pages_startup.cpp "static constexpr int kSteps = 6;" "the first start lists six steps (the titles, boot9.bin, the seed database, the plugin file, the plugin folder, the Plugin Loader)"
need ConfigApp/source/ui.cpp "box.y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - box.h) * 0.5f;" "a popup is in the middle of the touch screen and as high as its text"
need ConfigApp/source/pages_servers.cpp 'return "Server list";' "the bottom bar of the list of rooms says what the list is"
need ConfigApp/source/pages_servers.cpp 'test.label = "Test room";' "the form of a room has a row that tests it"
need ConfigApp/source/pages_servers.cpp "PushPage(pages::NewProbePage(room_, nullptr));" "...with the values of the form, saving nothing"
need ConfigApp/source/probe.cpp "(outcome_ == ProbeOutcome::Cancelled || (close_key && was_ended_))" "the test of a room stays on the screen until the user closes it (the result and its reason can be read)"
need ConfigApp/source/pages_delivery.cpp "Up to 16 titles and 7 receivers." "a room has 8 places: the sender and 7 receivers"
need ConfigApp/source/pages_delivery.cpp 'Fmt("Selected titles %d of %d", PickedCount(), delivery::kMaxTitles)' "the card of Send Titles says how many are chosen, not the name of the page again"
need ConfigApp/source/pages_delivery.cpp "row.detail = size > 0 ? delivery::ShortDurationText" "the time of a part is a column of its own (the size does not move with its words)"
need ConfigApp/source/ui.cpp 'edge -= gfx::TextWidth("1 h 05 min", 0.42f) + 10.0f;' "...a column as wide as its longest word"
need ConfigApp/source/pages_enable.cpp "Switched on, this title connects to the Azahar emulator network room set in Azahar Emulator Room." "the page of the titles says what the switch is for"
need ConfigApp/source/pages_main.cpp "Lets your titles connect to an Azahar emulator network room" "...and so does its line on the main page"
need ConfigApp/source/delivery_run.cpp 'DrawPopupBox(box, "Close the room?", kCloseText);' "B on the sending screen asks before the room is closed"
[ "$(grep -c 'confirming_ = true;' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "B on the sending screen or on the receiving screen does not start its question"
# The time of the room (the user's fifth round: the minutes of a console that joins late): delivery_eta.cpp works it out from what every receiver
# still needs, title by title, in the order the sender serves them; the sending screen feeds it with what it measures
need ConfigApp/source/delivery_run.cpp "eta_ = EstimateRoom(in);" "the sending screen works out the time that is left from what every receiver still needs, title by title"
need ConfigApp/source/delivery_run.cpp "dl_send_estimate(pct_, eta_.kind == EtaKind::Busy ? eta_.seconds : -1.0);" "...and the receivers are given the same share and the same time"
need ConfigApp/source/delivery_run.cpp "send_rate_.Add(dt, sent - radio_bytes_);" "the speed the radio sends at is measured from everything it sends (the repeated blocks too)"
need ConfigApp/source/delivery_run.cpp "if (static_cast<double>(c.down_bytes) > static_cast<double>(c.inst_bytes) + kInstallBacklog) {" "an installer is measured only while it has a backlog (else it just keeps up with the radio)"
! grep -q "SecondsLeft" ConfigApp/source/delivery_run.cpp || fail "the sending screen has the old time estimate again (it was the module's floor, a one-second speed and no order of the titles)"
need ConfigApp/source/delivery_run.cpp 'left = "All receivers finished";' "a room with nothing left says so, not 'waiting for receivers'"
need ConfigApp/source/delivery_run.cpp 'right = "for all titles: " + ShortDurationText(eta_.seconds);' "with nobody in yet the time is what one receiver would take, said as that"
need ConfigApp/source/delivery_run.cpp "if (mine.wait >= 60.0 && (c.state == DL_C_WAITING || c.state == DL_C_JOINED)) {" "a receiver whose title comes later is told when it starts"
need ConfigApp/source/delivery_eta.cpp "slowest = std::max(slowest, n / (rate * eff[a]));" "a title lasts as long as the receiver that needs most of it (and has the worst air) needs"
need ConfigApp/source/delivery_eta.cpp "dur[t] = slowest + kLagShare * std::max(0.0, (mx - mn) - kAbsorb) / rate;" "a console far behind the others costs radio time of its own"
need ConfigApp/source/delivery_eta.cpp "better = tiny[t] < tiny[pick];" "the sender serves a small remainder first, the smallest first"
need ConfigApp/source/delivery_eta.cpp "better = k[t] > k[pick];" "...else the title that most receivers can take"
need ConfigApp/source/delivery_eta.cpp "if (n_present == 0 || (n_live == 0 && n_silent == 0 && n_choosing > 0)) {" "a console that has not chosen yet is nobody"
need ConfigApp/source/delivery_eta.cpp "f = std::max(begin + inst_left / q + kTitleFixed, takes[a][t] ? e + kTail : 0.0);" "a receiver is done when its titles have come AND been installed, one after the other"
need ConfigApp/source/delivery_eta.cpp "pct = std::min(std::max(static_cast<int>(100.0 * done / total), 0), 99);" "the percentage is the work done over the work asked for, and never 100 before everybody is done"
need ConfigApp/source/delivery_eta.cpp "if (total <= total_ + 1.0 && pct < pct_) {" "...it goes up and falls only when somebody comes in who adds work (the minutes move, the percentage does not)"
need ConfigApp/source/delivery_eta.cpp "credit_ += finished_ - eta.work_finished;" "...and a finished receiver that leaves keeps its work in the count"
need ConfigApp/source/delivery_eta.cpp "out.work_done += r.finished ? asked : installed;" "the work done is what is installed (a finished receiver counts in full)"
need ConfigApp/source/delivery_eta.cpp "if (idle_ >= 30.0) {" "the share starts again after half a minute without work"
need ConfigApp/source/delivery_run.cpp "pct_ = share_.Update(dt, eta_);" "the percentage of the room comes from the work done, not from the minutes"
[ "$(grep -rn 'RoomClock' ConfigApp/source --include=*.cpp --include=*.hpp | wc -l)" = "0" ] || fail "the percentage is a function of the minutes again (RoomClock fell with every revision of the estimate)"
# ... and the other fixes of that review
need ConfigApp/source/delivery_run.cpp "in.down = select;" "with the screens off only SELECT reaches the screens (a press of B in a pocket must not answer the invisible question)"
need ConfigApp/source/delivery_run.cpp "} else if (total && free_bytes < sp.need) {" "Y does not start a receiving that cannot fit on the card"
need ConfigApp/source/pages_delivery.cpp 'if (dl_scan_pick(row.id, &room) != 0 || gfx::Clean(room.host[0] ? room.host : "A console") != row.label) {' "the room that is joined is the one the user picked, not whatever is at that place after a new scan"
need ConfigApp/source/delivery_run.cpp '"The room was closed by an error" + (run.failure.empty()' "an error that closes the room says why"
need ConfigApp/source/delivery_run.cpp "Not enough memory to join the room: close other applications and try again." "a receiving that could not get its stack says so (it does not run on the small one)"
need ConfigApp/source/delivery_run.cpp "Not enough memory to open the room: close other applications and try again." "...nor does a sending"
need ConfigApp/source/ui.cpp "} else if (latch.nav_since != 0 && now - latch.nav_since >= 400 && now - latch.nav_last >= 100) {" "up and down repeat while held, also in the screens that read the keys through the key thread"
need ConfigApp/source/delivery_run.cpp "if (sp.who != who) {" "the speed of a receiver is measured by receiver, not by place in the list"
need ConfigApp/source/delivery_run.cpp 'Fmt("and %d more: %d installed", more, more_done)' "the list of the receiving screen says how many more there are and how many are installed"
! grep -q "dl_pace_next" ConfigApp/source/delivery_run.cpp || fail "the sending screen has the Speed button again"
[ "$(grep -c 'g.input.StartLatch();' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "a transfer screen does not keep the keys while the module runs (a short press would fall between two looks)"
[ "$(grep -c 'g.input.StopLatch();' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "a transfer screen leaves the key thread running"
[ "$(grep -A1 'bool StartButton() const override {' ConfigApp/source/pages_delivery.cpp | grep -c 'return false;')" = "2" ] || fail "a page of Titles Delivery (the menu, the rooms) has a START button: the app is left from the main page (only the summary of a receiving has one, to leave at once)"
need ConfigApp/source/pages_delivery.cpp "PopPages(2); // (this page and the menu of Titles Delivery under it)" "B on the summary of a receiving goes back to the main page, not to the menu it came from"
need ConfigApp/source/pages_delivery.cpp 'return "Main page";' "...and says so"
need ConfigApp/source/pages_delivery.cpp 'return "Continue";' "A on the summary goes on to the menu of Titles Delivery"
need ConfigApp/source/pages_delivery.cpp 'return "Exit";' "START on the summary leaves the app"
need ConfigApp/source/delivery_run.cpp "run->result.done_ms[i] = now - run->result.start_ms + 1;" "the summary knows how long after the start each title was installed"
need ConfigApp/source/pages_delivery.cpp 'x = delivery::DrawPair(x, 88, 0.44f, "in", delivery::ClockText(elapsed_));' "the summary says in how long (really) it was done"
# The clean-up before Download and Play: what was written five times is written once, and what nothing used is gone
[ "$(grep -rn 'FillRect(0, 213' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the strip behind the footer is drawn in more than one place (DrawFooterBackground in ui.cpp)"
[ "$(grep -rn 'FillCircle(142.0f' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the moving dots are drawn in more than one place (DrawSpinner in ui.cpp)"
[ "$(grep -rn 'float DrawPair(' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the pair of a label and a value is written in more than one place (delivery.cpp)"
[ "$(grep -rln '6a09e667' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "the app has its own SHA-256 again (it uses the module's common/sha256.c)"
need ConfigApp/source/paths.cpp "sha256_update(&sha, buffer, got);" "a boot9.bin is recognised by the SHA-256 of its content"
[ "$(grep -rn 'dl_pace_\|MigrateOldFiles\|FindCheckAction\|SdUsageText\|BatteryText\|is_azaharlink' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "code that nothing used is back (the pace chooser, the migration, the texts only the tests read, the plugin name guess)"
need ConfigApp/titleshare_host/delivery.c "_Static_assert(SAME_NUMBER(DL_RES_PENDING, TS_RES_PENDING)" "the numbers the screens read are the module's"
need ConfigApp/source/ui.cpp "while (g_pop_extra > 0 && g_pages.size() > 1) {" "PopPages takes several pages off at once"
need ConfigApp/source/pages_config.cpp "g.cfg.nickname = input;" "the nickname typed on the Settings page is saved"
need ConfigApp/source/titles.cpp "UseAppNames(impl);" "the catalog takes over the names the app reads, so every list (and what is sent) says the same name"
if grep -rn 'AZL_TEST_DELIVERY_DEMO' ConfigApp --include=*.cpp --include=*.hpp --include=*.h | grep -v 'ConfigApp/titleshare_host/'; then
  fail "the test build of the delivery screens (made-up rooms and numbers) is used outside the bridge"
fi
if grep -rn 'AZL_TEST_TS_' ConfigApp; then
  fail "a test switch of the old game delivery is back"
fi
need ConfigApp/source/paths.cpp 'p.delivery_log = JoinPath(p.datadir, "3DSLink-TitleDelivery.log");' "the delivery log is in the data folder, with the name logrot.c gives it"
need ConfigApp/source/main.cpp "if (!session::Start()) {" "the services and the graphics of the app are opened in one place"
need_line ConfigApp/source/session.cpp '    gfxSetDoubleBuffering(GFX_TOP, true);' "the app draws every frame into the buffer that is not on the screen: the double buffering is turned on whenever the graphics are opened"

# The log has two levels and nothing between: 0 writes no file at all, 1 writes everything
if grep -rn -E "g_debug_level *(>=|==|>) *2|g_debug_level *< *2|debug_level\(\) *\+ *1\) *% *3|kDebugNames" Sources Includes ConfigApp/source --include=*.cpp --include=*.hpp --include=*.h | grep -v "^Sources/enet"; then
  fail "a third log level is back (debug is 0 or 1)"
fi
need Includes/azl/ini_values.hpp "std::strtol(value.c_str(), nullptr, 10) >= 1 ? 1 : 0" "debug in the ini: any value from 1 up is 1, the rest 0"
need Sources/ctr/plugin.cpp "config_.debug = ini::DebugLevel(value);" "the plugin reads debug through the shared rule"
need ConfigApp/source/settings.cpp "cfg.debug = azl::ini::DebugLevel(value);" "the Setup app reads debug through the shared rule"
need_after Sources/azl/log.cpp "void Log(const char* fmt, ...) {" "if (g_debug_level <= 0) {" "Log() returns at once when the log is off"
need_after Sources/azl/log.cpp "LogLine::LogLine() :" "if (g_debug_level <= 0) {" "a LogLine is inactive when the log is off"
need_after Sources/azl/log.cpp "DebugIpc(u32 handle, u32 header, u32 arg, u32 result) {" "if (g_debug_level <= 0 || !g_dbg_ring) {" "the capture does nothing when the log is off"
# No log file exists unless the log is on: the files of the log are opened in two places, both behind the level check, and
# the console's file layer is the only user of File::Open besides the ini (read in LoadConfig, written in SaveConfig) and the memory
# record of the last launch (read and written in CompareMemoryWithLastLaunch, with the log on only)
need Sources/azl/log_files.cpp "if (log_file_ || g_debug_level <= 0) {" "the main log is never opened with the log off"
need Sources/azl/log_files.cpp "if (debug_file_ || g_debug_level <= 0 || !DebugRingReady()) {" "the capture file is never opened with the log off"
need Sources/azl/log_files.cpp "if (crumb_file_ || g_debug_level <= 0) {" "the trail of a sleep (AzaharLink.sleep.log) is never opened with the log off"
# (the capture, the main log, and the trail's file: opened and, for the trail, reopened after the old one is removed)
[ "$(grep -c 'OpenAppend(' Sources/azl/log_files.cpp)" = "4" ] || fail "Sources/azl/log_files.cpp opens a file in a place that is not behind the level check"
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -o 'File::Open(' | wc -l)" = "5" ] || fail "Sources/ctr/plugin.cpp opens a file besides the log layer, the ini read, the ini write and the memory record (read and write)"
need_after Sources/ctr/plugin.cpp "void Plugin::CompareMemoryWithLastLaunch() {" "if (g_debug_level <= 0) {" "the memory record is read and written with the log on only (debug=0 leaves the card alone)"
need Sources/ctr/plugin.cpp "mem against the last launch of this title:" "two launches of a title are compared (2026-10-20: the proof that an exit leaves nothing)"
need Sources/ctr/plugin.cpp "    CompareMemoryWithLastLaunch();" "... at the start, after the memory line"
need Sources/ctr/plugin.cpp "    constexpr int kRecords = 16;" "... with a record for each of the last 16 titles (a launch of the same title right after the last one is rare)"
need Sources/ctr/plugin.cpp "void Plugin::CheckLinkSilence(const Stats& s) {" "with the log on, a notice when the room's datagrams stop for 150 ms or more (the WiFi link: 2026-10-20, one console 17-23 a minute, the other 0-2)"
need Sources/ctr/plugin.cpp "    CheckLinkSilence(s);" "... checked every statistics window"
need Sources/ctr/plugin.cpp "    if (s.rx_datagrams >= 100 && silent >= 2) {" "... in a window with traffic and at least two silences"
need_after Sources/ctr/plugin.cpp "void Plugin::CheckLinkSilence(const Stats& s) {" "if (g_debug_level <= 0) {" "... and only with the log on"
need Sources/ctr/plugin.cpp "if (config_.debug > 0) {" "the capture ring is allocated only when debug is on"
need Sources/ctr/plugin.cpp "config_.debug = g_debug_level;" "the menu label and the toggle follow what the log really does"
need Sources/ctr/plugin.cpp ': DebugRingReady() ? "AzaharLink.log and AzaharLink.debug.log"' "the line that says the capture is written is only true when the ring exists"
# ... and nothing else in the product touches a file: not one call that creates, writes, removes or renames outside the
# log layer (Sources/azl/log_files.cpp, which only calls its FileLayer) and plugin.cpp (counted above; its FSUSER_*
# calls only read the game's icon)
if grep -rn -E 'File::(Open|Remove|Rename)|fopen|freopen|std::(o|i)?fstream|FSUSER_(OpenFile|DeleteFile|RenameFile|CreateFile)|FS_OPEN_(WRITE|CREATE)|mkdir|\bopen\(' Sources Includes --include=*.cpp --include=*.hpp --include=*.c --include=*.h | grep -v "^Sources/ctr/plugin.cpp:\|^Sources/azl/log_files.cpp:\|^Sources/ctr/boot_diag.cpp:"; then
  fail "a source file besides the log layer and plugin.cpp touches a file (debug=0 must leave the card alone)"
fi
grep -q 'FS_OPEN_WRITE\|FS_OPEN_CREATE' Sources/ctr/plugin.cpp && fail "plugin.cpp opens a file of the card for writing through FSUSER (only the icon is read that way)"
# ... except the diagnostic build's own file (Sources/ctr/boot_diag.cpp, AzaharLink/diag/README.md): it writes /3ds/3DSLink/AzaharLink.boot.log even at debug=0, so the whole file is
# compiled only with -DAZL_BOOT_DIAG, which only diag/build_diag.sh gives (in a copy of the folders), never the delivered plugin's Makefile or build_all.sh.
[ "$(tr -d '\r' < Sources/ctr/boot_diag.cpp | grep -c '^#ifdef AZL_BOOT_DIAG$')" = "1" ] || fail "Sources/ctr/boot_diag.cpp is not compiled only under AZL_BOOT_DIAG (it writes to the card at debug=0)"
tr -d '\r' < Sources/ctr/boot_diag.cpp | tail -1 | grep -q -F -- '#endif // AZL_BOOT_DIAG' || fail "Sources/ctr/boot_diag.cpp does not end with the #endif of AZL_BOOT_DIAG"
! grep -q 'AZL_BOOT_DIAG' Makefile ../DlpLink/tools/build_all.sh || fail "the delivered plugin's Makefile or build_all.sh names AZL_BOOT_DIAG (the diagnostic build is made only by diag/build_diag.sh)"
need diag/build_diag.sh "sed -i 's/-DAZL_USE_CTRPF/-DAZL_USE_CTRPF -DAZL_BOOT_DIAG/' \"\$WORK/Makefile\"" "the diagnostic flag goes into a COPY of the Makefile"

# The in-game menu switch: menu_key=off (or none) vetoes every opening, and nothing waits for the menu to join the room
need Sources/main.cpp "g_menu_off = plugin.menu_off();" "main reads the switch"
tr -d '\r' < Sources/main.cpp | grep -A2 "static bool OnMenuOpening" | grep -q "if (g_menu_off) {" || fail "OnMenuOpening does not veto first when the menu is off"
need Sources/main.cpp "if ((cfg.autoconnect || g_menu_off) && !cfg.host.empty()) {" "with the menu off the room is joined at start whatever autoconnect says"
need Includes/azl/ini_values.hpp 'return word == "off" || word == "none";' "off and none switch the menu off"

# The scan filter only counts (spec C7): nothing in the plugin turns it on
if grep -rn "SetScanFilterEnforced" Sources/ctr Sources/main.cpp >/dev/null 2>&1; then
  fail "the plugin calls SetScanFilterEnforced: the filter's mode is the engine's default (enforced since 2026-10-20), the switch is the tests'"
fi
need Includes/azl/uds_engine.hpp "bool scan_filter_enforced_ = true;" "the scan filter leaves the networks of another game out (2026-10-20)"

# The sleep action exists in one place, behind sleep_stop
[ "$(grep -c 'StopService(' Sources/ctr/plugin.cpp)" = "1" ] || fail "StopService is called from more than one place in Sources/ctr/plugin.cpp"
need Sources/ctr/plugin.cpp "if (!acts) {" "OnSleepEnter returns before it stops anything unless the sleep is acted on"
need Includes/azl/apt_sleep.hpp "return sleep_stop != 0 && (sleep_stop >= 2 || !hosting);" "sleep_stop=0 never acts, 1 acts for a console that is not hosting, 2 forces it for a host too (the one host that was stopped by a sleep froze the console)"
need Sources/ctr/plugin.cpp "return SleepActsFor(mode, mode == 1 && engine_->status() == NetworkStatus::ConnectedAsHost);" "the sleep action asks the engine whether the console hosts"
need Sources/ctr/plugin.cpp "config_.sleep_stop = ini::SleepStop(value);" "the plugin reads sleep_stop through the shared rule"
need Sources/ctr/plugin.cpp "sleep_.NeedsActs(event_count_[kEventSleepExit]) && SleepActs()" "the frame callback works out whether the sleep acts only when a wake-up is to be looked at (with sleep_stop=1 that takes the engine's lock)"
need Includes/azl/ini_values.hpp 'return Word(value) == "2" ? 2 : kSleepStopDefault;' "the ini value of sleep_stop: 0, 1 (or on/true/yes), 2; a value that is none of them is the default, off (the action ends a game session)"

# The link (0.1.16): what the room link does when the WiFi goes and comes back, and what it lets into the game
# - the schedule of the attempts is fixed (2 s after the last began; no growing interval), and held back while the console's
#   WiFi is down (shared page byte 0x1FF81067, read every 500 ms, an attempt at once when it is back)
need Includes/azl/link_policy.hpp "static constexpr u32 kRetryMs = 2000;" "a failed attempt is followed by the next one 2 s after it began"
need Includes/azl/link_policy.hpp "static constexpr u32 kWifiPollMs = 500;" "the WiFi state is read every 500 ms"
need Includes/azl/link_policy.hpp "static constexpr u8 kWifiConnected = 2;" "the shared page's network state 2 is a console on its access point"
if grep -n "kStepMs\|kMaxWaitMs\|interval_ms_" Includes/azl/link_policy.hpp; then
  fail "the growing interval of the attempts (2, 4, 6, 8, 10 s) is back: the schedule is fixed since 0.1.16"
fi
need Sources/ctr/plugin.cpp "in.wifi_state = *reinterpret_cast<volatile const u8*>(kSharedPageWifiState);" "the policy gets the WiFi byte of the shared page"
# - the room client: a bounded join (a stuck network thread is abandoned, not waited for), an escalating pause after socket
#   errors, and no wait for the SD card in the state callback
need Sources/azl/room_client.cpp "thread_.JoinTimeout(kStopJoinMs)" "RoomClient::Stop waits for the network thread a bounded time"
if grep -n "thread_.Join()" Sources/azl/room_client.cpp; then
  fail "RoomClient waits for its thread without a limit (JoinTimeout)"
fi
need Sources/azl/room_client.cpp "platform::SleepMs(ServiceErrorPauseMs(errors_in_a_row));" "a failing socket is answered by a pause that grows with the errors in a row"
# ... a thread that Stop() gave up on leaves the loop right after the callbacks (where it gets stuck) and, before that, changes
# the client's state and error only through the lambdas that check its session (the one Fail(Error:: left is Start()'s)
need Sources/azl/room_client.cpp "if (!keep_running || !Live(generation)) {" "an abandoned network thread must not go on to the queue of the session that replaced it"
[ "$(grep -c 'Fail(Error::' Sources/azl/room_client.cpp)" = "1" ] || fail "the network thread of Sources/azl/room_client.cpp sets an error without checking that its session is still the client's (fail())"
if grep -n 'SetState(State::Joined)' Sources/azl/room_client.cpp; then
  fail "the network thread of Sources/azl/room_client.cpp sets Joined without checking that its session is still the client's (set_state())"
fi
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'log_files_->Flush()')" = "2" ] || fail "Sources/ctr/plugin.cpp calls log_files_->Flush() from more than the log writer's two places (the room state callback must not wait for the SD card)"
# - the foreign-lobby filter: a host always refuses, a client only counts until the switch is turned (nothing in the plugin
#   turns it)
need Includes/azl/uds_engine.hpp "bool client_foreign_filter_enforced_ = false;" "a client only counts the frames of another lobby (until a session on hardware shows it can drop them)"
if grep -rn "SetClientForeignFilterEnforced" Sources/ctr Sources/main.cpp >/dev/null 2>&1; then
  fail "the plugin turns the client's foreign-lobby filter on: that waits for a hardware session with zero would-be drops"
fi
need Sources/azl/uds_engine.cpp "const bool refuse = status == NetworkStatus::ConnectedAsHost || client_foreign_filter_enforced_;" "a host refuses the frames of another lobby always, a client when its filter is on"
# - the beacon timer is an absolute schedule on the exact period, and the late second Deauthentication is swallowed
need Sources/azl/uds_engine.cpp "next_beacon_tenths_ += kBeaconPeriodTenths;" "the host's beacon is due a period after the last one was DUE (105.5 ms per beacon on hardware before)"
need Sources/azl/uds_engine.cpp "constexpr u64 kBeaconPeriodTenths = 1024;" "100 TU = 102.4 ms"
need Sources/azl/uds_engine.cpp "it->second.deauth_guard = false;" "the guard of the late second Deauthentication is used up by the first copy it swallows"
# - a game thread that polls a command that keeps failing is slowed down (never answered differently)
need Sources/ctr/uds_hle.cpp "svcSleepThread(kFailPauseNs);" "the throttle of a failing-reply spin"
need Sources/ctr/uds_hle.cpp "ThrottleFailing(ended);" "every failing reply goes through the throttle"

# The capture (0.1.16): a ring with a lane for control records, markers for the holes, a producer that never waits for the
# ring's lock, the diet of the IPC lines, header-only records for the frames that are not for us, bounded passes of the writer
need Sources/azl/log.cpp "constexpr u32 kControlReserveBytes = 32 * 1024;" "the last 32 KB of the ring are for the control records (a flood of frames cannot push them out)"
need Sources/azl/log.cpp "constexpr u32 kHeavyLimit = kDebugRingBytes - kControlReserveBytes;" "the heavy lane stops where the control lane's room begins"
need Sources/azl/log.cpp "const u32 limit = lane == kLaneHeavy ? kHeavyLimit : kDebugRingBytes;" "a heavy record is held to the heavy limit, a control record to the whole ring"
need Sources/azl/log.cpp "EmitLostLocked(lane, now);" "the first record after a hole is preceded by the marker of the hole"
need Sources/azl/log.cpp "if (mutex.try_lock()) {" "a producer of the capture tries the ring's lock a few times and gives up (it never waits: the writer has the lowest priority)"
[ "$(grep -c 'platform::LockGuard lock(g_dbg_mutex);' Sources/azl/log.cpp)" = "7" ] || fail "Sources/azl/log.cpp: a function that waits for the ring's lock besides those of the writer side (DebugPop twice, DebugOldestMs, DebugFlushAggregates, DebugCaptureTotals, DebugReset, DebugTakeDropped)"
need Sources/azl/log.cpp "constexpr u32 kLockSpins = 256;" "a producer tries the ring's lock 256 times (about 30 us) and gives the record up: a game thread never waits for the writer"
if grep -n 'g_dbg_mutex.lock()' Sources/azl/log.cpp; then
  fail "Sources/azl/log.cpp waits for the ring's lock with lock(): a producer must use TryLockBriefly"
fi
need Sources/azl/log.cpp "constexpr u32 kDietFullCalls = 3;" "the diet writes the first three calls of a kind in a second whole"
need Sources/azl/log.cpp "constexpr u32 kDietWindowMs = 1000;" "... and counts the rest of that second in one line"
need Sources/azl/log.cpp "if ((result != 0 || !DietLocked(handle, header, arg, now)) &&" "a call with a result other than 0 is never counted"
need Sources/azl/uds_engine.cpp "DebugSecureHeader(kDirEcho, packet, secure_data);" "the echo of our own frame is a header-only record"
need Sources/azl/uds_engine.cpp "DebugSecureHeader(kDirOther, packet, secure_data);" "a frame for another node is a header-only record"
need Sources/azl/uds_engine.cpp "DebugFrame(kDirEcho, src_node, dest_node, data_channel, nullptr, frame_size);" "the prefilter writes the same header-only echo record"
need Sources/azl/uds_engine.cpp "DebugFrame(kDirOther, src_node, dest_node, data_channel, nullptr, frame_size);" "... and the same header-only record of a frame for another node"
if grep -n 'DebugSecure(kDirOther\|DebugSecure(kDirEcho' Sources/azl/uds_engine.cpp; then
  fail "the bytes of a frame that is not for this node are in the capture again (half of its volume)"
fi
need Sources/azl/log_files.cpp "constexpr std::size_t kPassBytes = 256 * 1024;" "a pass of the capture's writer puts at most 256 KB of text on the card"
need Sources/azl/log_files.cpp "constexpr u64 kPassMs = 200;" "... and takes at most 200 ms"
need Sources/azl/log_files.cpp "if (more && !final && (written + out.size() >= kPassBytes || platform::NowMs() - begin_ms > kPassMs)) {" "both bounds end a pass that is not the last"
need Sources/azl/log_files.cpp "DebugFlushAggregates(final);" "the writer closes the counts of the diet before it drains"
# - the writer is light on the card (2026-10-20, the user: "a volte si inchioda il gioco"): the capture goes to the card in writes of 64 KB
#   (16 KB until then) and is flushed ONCE a pass (every write was flushed until then: a flush costs the card as much as a small write), the
#   main log at every write (its last lines are what a post-mortem needs); the console's file layer flushes only in Flush(); a writer whose
#   pass was cut short rests as long as the card was busy (20..250 ms) before the next pass; the hex of a frame is formatted in one go
need Sources/azl/log_files.cpp "constexpr std::size_t kWriteBytes = 64 * 1024;" "the capture goes to the card in writes of 64 KB"
[ "$(grep -c 'files_->Flush(debug_file_);' Sources/azl/log_files.cpp)" = "1" ] || fail "Sources/azl/log_files.cpp: the capture is flushed in more or fewer than one place (once a pass, at the end of DrainDebug)"
grep -A4 "    if (written != 0) {" Sources/azl/log_files.cpp | grep -q "files_->Flush(debug_file_);" || fail "Sources/azl/log_files.cpp: the capture is not flushed at the end of the pass (when something was written)"
need Sources/azl/log_files.cpp "files_->Flush(log_file_); // (every time: the main log is small, and its last lines are what a post-mortem needs)" "the main log is flushed at every write"
need Sources/azl/log_files.cpp "files_->Flush(crumb_file_); // (at once: the trail is what is read after a freeze)" "the trail of a sleep is flushed at once"
[ "$(grep -c 'static_cast<File\*>(file)->Flush();' Sources/ctr/plugin.cpp)" = "1" ] || fail "Sources/ctr/plugin.cpp: the console's file layer flushes in more or fewer than one place (Flush())"
if grep -n 'f->Flush()' Sources/ctr/plugin.cpp; then
  fail "Sources/ctr/plugin.cpp: the console's file layer flushes at every write again (every 16 KB of the capture flushed the card until 2026-10-20)"
fi
need Sources/ctr/plugin.cpp "const u32 wait_ms = backlog ? rest_ms : g_debug_level > 0 ? 250 : 1000;" "a writer whose pass was cut short rests before the next pass ..."
need Sources/ctr/plugin.cpp "rest_ms = card_ms < 20 ? 20 : card_ms > 250 ? 250 : card_ms;" "... for as long as the card was busy, 20 ms at least, 250 at most"
need Sources/azl/log.cpp "out->resize(at + 2 * static_cast<std::size_t>(n) + 1);" "the hex of a frame is formatted in one go, not a char at a time"
# - the session of 2026-10-20 with Friend-A (Tekken, DOA, SSF4, NSMB2 over the internet, the log on): the writer runs ABOVE the game's
#   threads (0x2E: a thread that holds a lock the game's threads take must not be preemptable by them), a scan leaves out the networks of
#   another game (NSMB2 listed the emulator's Tekken lobby of the same room and joined it, in a loop), and with the log on a notice on
#   the screen says when it is the log that slows the game (the SD card busy, the capture dropping records)
need Sources/ctr/plugin.cpp "constexpr int kLogWriterPriority = 0x2E;" "the writer runs above the game's threads and the network thread"
need Sources/ctr/plugin.cpp "            0, kLogWriterPriority)) {" "... and that is the priority its thread is started with"
need Includes/azl/uds_engine.hpp "bool scan_filter_enforced_ = true;" "a scan leaves out the networks of another game (2026-10-20: NSMB2 listed and joined the emulator's Tekken lobby)"
need Sources/ctr/plugin.cpp "AzaharLink debug: SD card busy %lu%%, capture lost %lu%% (the log slows the game)" "with the log on the screen says when the log slows the game"
need Sources/ctr/plugin.cpp "    CheckDebugLoad(s);" "... checked every statistics window"
need Sources/ctr/plugin.cpp "    if (card_pct >= 50 || lost_pct >= 30) {" "... when the card was ours half the time, or the ring dropped almost a third"
# - no thread above the log writer ever waits for a lock of the log (2026-10-20, Tekken stuck at the publisher's screen with the log on: the
#   writer has the lowest priority, a LightLock has no priority inheritance, and a game that spins never lets the writer back on the core):
#   the writer marks itself and is the one thread that may wait; Log(), LogLine and Append() try briefly and give the line up, counted, and
#   the writer says the count in the log; the queue's room is made outside its lock
need Includes/azl/platform.hpp "void MarkLogWriterThread();" "the writer marks itself ..."
need Sources/ctr/plugin.cpp "platform::MarkLogWriterThread(); // (the one thread that may wait for the log's locks: azl/platform.hpp)" "... at the start of its thread"
need Sources/azl/log.cpp "inline bool LockLogBriefly() {" "the log's lock is tried briefly"
need_after Sources/azl/log.cpp "    if (platform::IsLogWriterThread()) {" "g_log_mutex.lock();" "... and only the writer waits for it"
[ "$(grep -c 'g_log_mutex.lock();' Sources/azl/log.cpp)" = "1" ] || fail "Sources/azl/log.cpp waits for the log's lock with lock() somewhere besides the writer's branch of LockLogBriefly"
[ "$(grep -c '    if (!LockLogBriefly()) {' Sources/azl/log.cpp)" = "2" ] || fail "Sources/azl/log.cpp: Log() and LogLine do not both go through LockLogBriefly"
[ "$(grep -c 'platform::LockGuard lock(g_log_mutex);' Sources/azl/log.cpp)" = "1" ] || fail "Sources/azl/log.cpp: a line of the log waits for the log's lock with a LockGuard (only SetLogSink, at start-up, may)"
need Sources/azl/log_files.cpp "    if (!platform::TryLockBriefly(log_mutex_)) {" "Append() tries the queue's lock briefly"
need_after Sources/azl/log_files.cpp "        if (!platform::IsLogWriterThread()) {" "__atomic_fetch_add(&append_busy_, 1u, __ATOMIC_RELAXED);" "... and a thread that is not the writer gives the line up, counted"
need Sources/azl/log_files.cpp "log: %lu lines of other threads were given up: a lock of the log was busy" "a hole in the log is never silent"
need Sources/azl/log_files.cpp "    ReportBusyDrops();" "... the writer says it at every flush"
[ "$(grep -c '    fresh.reserve(kQueueReserve);' Sources/azl/log_files.cpp)" = "2" ] || fail "Sources/azl/log_files.cpp: the room for the next lines is made under the queue's lock again (Flush and FlushWithin make it outside)"
[ "$(grep -c 'pending_.reserve(kQueueReserve);' Sources/azl/log_files.cpp)" = "1" ] || fail "Sources/azl/log_files.cpp: a reserve of the queue under its lock besides Open()'s (a malloc under the lock a game thread tries)"
# - the end of a launch: the capture and the main log are closed once the writer thread has stopped (a breadcrumb thread that is
#   stuck in a write does not keep them from it: Close() does not wait for the trail's lock), and the first line of a capture file
#   stands where the oldest record waiting for it does
need Sources/ctr/plugin.cpp "if (writer_stopped) {" "the logs are closed when the writer has stopped, whatever the breadcrumb thread is doing"
need Sources/azl/log_files.cpp "if (crumb_mutex_.try_lock()) {" "closing the logs does not wait for the trail's lock"
need Sources/azl/log_files.cpp "if (DebugOldestMs(&oldest)) {" "the first line of a capture file is not later than the first record that follows it"
need Sources/ctr/plugin.cpp "the trail of this launch begins" "the trail's file is created when its thread starts, not by the first event of a sleep"
# - the capture does not keep the game from leaving (0.1.17): once Shutdown begins the capture has half a second, then every pass stops after the
#   write in hand; the main log is flushed first and the lines are never dropped for the capture; a writer stuck in a write to the card gets a
#   short wait, not Close()'s drain. (On the full 128 GB card of the Zelda session, 2026-10-14, the exit took very long and the log's last lines
#   - EXIT, shutdown - were never written.)
need Includes/azl/log_files.hpp "u32 exit_capture_ms = 500;" "the capture gets half a second at the end of a launch"
need Sources/ctr/plugin.cpp "log_files_->BeginShutdown();" "Shutdown arms the capture's deadline"
need Sources/ctr/plugin.cpp "log_files_->FlushWithin(kLogStopTimeoutMs);" "a writer that did not stop in time does not keep the main log's last lines from the file"
need Sources/azl/log_files.cpp "if (deadline != 0 && platform::NowMs() >= deadline) {" "a pass of the capture stops at the deadline of the end of the launch"
need Sources/azl/log_files.cpp "shutdown: the capture was cut at the end: " "the main log says how much of the capture was not written"
# - a count line of the diet that finds the control lane full takes its calls into the lost count
need Sources/azl/log.cpp "g_dbg_totals.lost_full[kLaneControl] += count - 1;" "a count line that is lost loses the calls it stood for, and the LOST marker says so"
need Sources/ctr/plugin.cpp "backlog = log_files_->DrainDebug(false);" "a pass cut at its bound is followed soon by the next, not after the usual pause"
# - Log() and DebugEvent() write the plugin's conversions themselves (the HLE proper calls them on the game's UDS thread, where newlib's
#   vsnprintf chain needed 1.4 KB), a raw record carries a command's words to the writer, and the request copy of the capture is as long as
#   the words the header declares
need Sources/azl/log.cpp "if (printf_lite::Supported(fmt)) {" "Log() and DebugEvent() use the plugin's formatter for the formats it writes"
need Sources/ctr/uds_hle.cpp "u32 request[request_words != 0 ? request_words : 1];" "the request copy of the capture is as long as the header says, not 256 bytes"
need Tests/chain_sim/chain_sim.py "HLE_CEILING = {0: 320, 1: 520}" "the HLE proper at debug 1 is held to 520 bytes (1896 with newlib's printf under Log())"
need Tests/chain_sim/chain_sim.py "the printf family ran in the HLE proper" "the chain test fails a printf in the HLE proper"
need Sources/ctr/uds_hle.cpp "const u32 started = static_cast<u32>(platform::NowTicks());" "the hot frame passes no 64-bit argument on the stack (the canary of Tri Force Heroes' thread saw 12 bytes more with one)"
need Tests/chain_sim/chain_sim.py "PURE_WRITTEN = {0: 88, 1: 156}" "the pure path is held to what the hotfix wrote below the entry SP (the emulator's canary: 108 / 176)"

# The screen (0.1.16): the numbers that decide what the plugin shows the player
# - the CPU figure is CPU. Every soc call is timed (sampling one in four put the ENet share at 0 in 98% of the windows of PROVA 2),
#   each section of the network loop is its wall time less the waits that fell in it (the soc calls, the engine's lock, the pause
#   after a socket error), the writer's pass less its SD card time, and every sum is 64 bits (a 32-bit tick sum wraps after 16 s)
if grep -n 'AZL_SOC_TIMED\|Counter = 0' Sources/enet/enet_3ds.c; then
  fail "enet_3ds.c samples the soc calls again (every call is timed)"
fi
need Sources/enet/enet_3ds.c 'azl_c_soc_call (3, svcGetSystemTick () - startTick);' "the poll (the network thread idle) is timed on every call"
need Sources/enet/enet_3ds.c 'azl_c_soc_call (2, svcGetSystemTick () - startTick);' "every send is timed"
need Sources/enet/enet_3ds.c 'azl_c_soc_call (recvLength > 0 ? 0 : 1, now - startTick);' "every receive is timed"
need Sources/azl/log.cpp "platform::NoteNetWait(ticks);" "a soc call is a wait of the network loop: it comes off the section it fell in"
need Sources/azl/log.cpp "if (!platform::IsNetThread()) {" "the soc statistics are those of the network thread"
need Includes/azl/platform.hpp "NoteNetWait(waited);" "the network thread's wait for the engine's lock comes off its sections too"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_service, service_end - round_start);" "ENet's own code: the service call less the soc calls in it"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_events, events_end - service_end);" "the frames and the engine: less the engine-lock waits"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_send, done_ticks - events_end);" "the queue and the tick: less the flush's soc calls, the lock waits and the pause"
need Sources/azl/room_client.cpp "platform::NoteNetWait(pause_to_note);" "the pause after a failing socket call is a wait, not CPU"
need Sources/azl/room_client.cpp "StatsAdd64(g_stats.t_net_wall, done_ticks - round_start);" "the wall time of the loop, to show that its pieces add up"
need Sources/ctr/plugin.cpp "StatsAdd64(g_stats.t_writer, pass_ticks - waited_ticks);" "the log writer's CPU is its pass less the SD card"
need Sources/ctr/plugin.cpp "StatsAdd64(g_stats.t_writer_wait, waited_ticks);" "... and the SD card time is counted apart"
# ... and the line keeps the two labels the analysis scripts of PROVA 1 and 2 read ("plugin total N", "poll N (waiting)"): the
# figures are CPU and waiting now, the words are the same
need Sources/ctr/plugin.cpp "| plugin total %lu.%lu (CPU) = " "the CPU line starts with 'plugin total N' (parse_main.py and the other scripts of the PROVA analyses)"
need Sources/ctr/plugin.cpp "poll %lu.%lu (waiting)," "... and gives the idle poll as 'poll N (waiting)'"
[ "$(grep -c 'CardTimer card(&card_wait_ticks_);' Sources/azl/log_files.cpp)" = "3" ] || fail "Sources/azl/log_files.cpp: the main log's flush, the capture's write and the capture's flush must both time their time in the file layer"
need Sources/ctr/uds_hle.cpp "StatsAdd64(g_stats.ipc_sum_ticks, ticks);" "the game threads' time in the commands is a 64-bit sum"
need Sources/main.cpp "azl::StatsAdd64(azl::g_stats.t_cb, svcGetSystemTick() - start);" "the frame callback's time is a 64-bit sum"
need Sources/ctr/plugin.cpp "const Stats s = TakeStats();" "the window is taken whole, the 64-bit sums read and cleared atomically"
if grep -n 'const Stats s = g_stats' Sources/ctr/plugin.cpp; then
  fail "the statistics window is copied from g_stats again (a 64-bit sum can be read in two halves): TakeStats()"
fi
for sum in t_net_service t_net_events t_net_send t_cb t_writer ipc_sum_ticks t_net_wall t_net_pause t_writer_wait lock_wait_ticks lock_wait_net_ticks frame_gap_sum; do
  grep -q "Ticks64 $sum;" Includes/azl/stats.hpp || fail "Includes/azl/stats.hpp: $sum is not a 64-bit sum (Ticks64) any more"
done
need Includes/azl/stats.hpp "Ticks64 soc_ticks[kSocKinds];" "the soc time is a 64-bit sum"
need Includes/azl/stats.hpp "static_assert(sizeof(Stats) - offsetof(Stats, t_net_service) ==" "TakeStats clears every sum after t_net_service, and the struct ends with them"
# - the memory the game has left: read about 20 s after the start (the game is done allocating) and every 30 s, one toast, a warning
#   only on a fall below the limit (ONE limit for both layouts: 5 MB, the size of the plugin's own block; no other number is known
#   for an Old 3DS), and no "Memory margin low" for a game that is promised more than is free (Tri Force Heroes: 56,320 KB free
#   all session)
need Includes/azl/screen_text.hpp "constexpr long kLowKb = 5 * 1024;" "the limit of the memory warning, the same on a New and an Old 3DS: the size of the plugin's own block"
if grep -n 'kLowNewKb\|kLowOldKb\|LowLimitKb' Includes/azl/screen_text.hpp Sources/ctr/plugin.cpp; then
  fail "a second, per-layout limit of the memory warning is back (one limit, 5 MB, for both layouts)"
fi
need Includes/azl/screen_text.hpp "constexpr u32 kFirstReadingMs = 20000;" "the first reading is 20 s after the start"
need Includes/azl/screen_text.hpp "constexpr u32 kRefreshMs = 30000;" "... then every 30 s"
need Includes/azl/screen_text.hpp 'const std::string mode = std::string(r.new_layout ? "New 3DS" : "Old 3DS") + " memory mode: ";' "the texts of the toast of the start"
need Includes/azl/screen_text.hpp 'return AllInUse(r) ? mode + "all memory in use" : mode + FreeText(r.free_kb) + " free";' "a game that takes the whole region reads 'all memory in use', not '0 KB free'"
need Includes/azl/screen_text.hpp "constexpr long kAllInUseKb = 256;" "under 256 KB free the texts say that all the memory is in use"
need Includes/azl/apt_sleep.hpp "return reply_word & 0xFFFFu;" "the APT signal is the low 16 bits of the reply word (0x1BC00001 is the HOME button in Mario Party)"
need Sources/ctr/hook.cpp "const u32 id = AptSignalOf(raw);" "the hook classifies the notification by its low 16 bits"
need Includes/azl/screen_text.hpp 'return "Game memory almost full: " + FreeText(r.free_kb) + " free";' "the text of the warning"
if grep -rn 'Memory margin low\|AzaharLink uses\|memory margin low' Sources Includes ConfigApp/source; then
  fail "the alarm about a memory margin (a game promised more than is free), or a toast about what AzaharLink takes, is back"
fi
need Sources/ctr/plugin.cpp "if (!mem_watch_.Due(uptime)) {" "the reading waits for its moment"
need Sources/ctr/plugin.cpp "const std::string text = gamemem::InfoText(reading);" "the toast of the start says the memory mode and the figure"
need Sources/ctr/plugin.cpp "const std::string text = gamemem::LowText(reading);" "... and a figure below the limit says so instead"
need_after Sources/ctr/plugin.cpp "void Plugin::CheckGameMemory(u64 now_ms) {" "if (!active_) {" "a plugin that is off for this launch has nothing to say about the game's memory"
need Sources/ctr/plugin.cpp "socplan::StartupIssue(SocketBufferSize(), g_debug_level > 0)" "the start-up text drops the pointer to the log when the log is on"
# - frames lost: the text depends on the game's UDS calls (no call in the window, or none in its last second: not reading)
need Includes/azl/screen_text.hpp "constexpr u32 kNotReadingMs = 1000;" "a game that has made no UDS call for a second is not reading"
need Includes/azl/screen_text.hpp "constexpr unsigned long kWarnAbove = 50;" "more than 50 frames in a window is worth a toast"
need Sources/ctr/plugin.cpp "frameslost::NotReading(s.ipc_requests, idle_ms)" "the text is chosen from the game's UDS calls"
need Sources/ctr/plugin.cpp "frameslost::Text(s.ring_overwritten, not_reading)" "... and built in one place"
if grep -n 'reads too slowly' Sources/ctr/plugin.cpp; then
  fail "plugin.cpp says \"reads too slowly\" by itself: the text is frameslost::Text's, chosen from the game's UDS calls"
fi
need Sources/ctr/soc.cpp 'g_debug_level > 0 ? "Could not start the network layer (0x%08lx)"' "no pointer to the log when the log is on"
need Sources/main.cpp 'azl::g_debug_level > 0 ? "" : " Set debug=1 for the details in the log."' "no pointer to the log when the log is on"

# No management frame is ever sent into the room (Azahar asserts on one): the only builder of a data frame
# writes a zero management byte (the rule checker of the unit tests watches every frame the engines send)
need Sources/azl/uds_frames.cpp "h[6] = 0; " "data frames carry no management bit"

# The socket buffer: 64 KB is a hard limit, SO_SNDBUF / SO_BROADCAST are not touched
need Includes/azl/rcvbuf.h "#define AZL_RCVBUF_MAX 65536" "more than 64 KB makes the soc service fail or crash"
if grep "setsockopt" Sources/enet/enet_3ds.c | grep -q "SO_SNDBUF\|SO_BROADCAST"; then
  fail "enet_3ds.c sets SO_SNDBUF or SO_BROADCAST (the soc service answers errno 109 for UDP)"
fi

# ---- Nothing on a GAME thread formats text with the printf family ------------------------------------------------------
# The svc hook runs the plugin's C++ on the stack of whatever thread of the game made an IPC call, and some of those stacks
# are tiny (Tri Force Heroes' srv-notification thread: 464 bytes below its call). 0.1.15 at debug=1 ran DebugEvent ->
# vsnprintf (~1.3 KB of stack) there and the game crashed on exit (hardware, 2026-10-10). The code that can run on a game
# thread is marked in the sources with  // >>> game-thread code  ...  // <<< game-thread code  and this scan forbids in it:
#   - the printf family (printf, snprintf, vsnprintf, sprintf, fprintf, ...), Log( and DebugEvent( (they take a format and
#     format on the caller's stack; the printf-free LogLine / DebugLine / DebugIpc / DebugSvcReal take their place),
#   - any char buffer over 64 bytes, std::string / std::vector / new / malloc (nothing allocated on a game thread),
# String literals are blanked first (a word inside a quoted text is not code), then the // comments. The buffer rule reads
# `char` arrays only: a big array of another type (RunTraced's u32 request[64], 256 bytes, is in a region on purpose) is held by
# the frame budgets of Tests/stack_usage.sh.
# The marked code is what the hook reaches before the HLE proper, written out here (this scan is lexical; the EXACT answer,
# from the machine code, is Tests/callgraph_check.py, which Tests/stack_usage.sh runs on the built ELF - it follows direct
# calls, tail calls and function addresses from the entries of the hook and from the callbacks the hook reaches through a
# pointer, and fails if the printf family, Log() or DebugEvent() is among them):
#   Sources/ctr/hook.cpp         the whole hook: PassThrough and its three branches, PassThroughApt, FindTraced,
#                                WorthLogging, the name tests, azl_hook_SendSyncRequest, azl_hook_CloseHandle
#   Sources/ctr/uds_hle.cpp      the front of the IPC front-end: FakeKind, MarkInitialized, ForgetFakeHandle,
#                                NoteHandleClosed, InterceptGetServiceHandle, HandleSendSyncRequest; and RunTraced
#   Sources/ctr/plugin.cpp       the observers of the game's APT session: OnSleepEnter (and SleepActs), OnWake, OnHomeButton,
#                                NoteSleepEvent (the trail of a sleep)
#   Includes/azl/apt_sleep.hpp   AptSleepEventOf (PassThroughApt)      Includes/azl/link_policy.hpp  SleepWatch::Enter, Woke
#   Includes/azl/sleep_crumbs.hpp  SleepCrumbs::Push (the ring NoteSleepEvent stores into)
#   Sources/ctr/uds_hle.cpp      also ThrottleFailing (the pause of a failing reply of a polling game thread)
#   Sources/azl/uds_engine.cpp   what the front reaches in the engine: the role line (TraceRoleLocked) and StopService
#                                (UdsHle::NoteHandleClosed -> Shutdown, OnSleepEnter -> StopService)
#   Sources/azl/room_client.cpp  DebugWifi (the Deauthentications that ending a session sends)
#   Sources/azl/ndm_hle.cpp      NdmHle::OpenSession      Sources/azl/platform.cpp  the clocks
#   Sources/azl/log.cpp          what they call in the log: the producers of the capture - DebugIpc, DebugSvcReal, DebugLine,
#                                DebugCommand, DebugServiceCommand, DebugPullPacket, DebugWifiLine, DebugFrame, DebugBeacon, their ring
#                                (the lanes, the lost markers, the diet, the try-lock) - and LogLine
#   Sources/azl/log_files.cpp    LogFiles::Append (the sink of the log)    Includes/azl/line.hpp  LineBuf
#   Includes/azl/printf_lite.hpp the formatter Log() and DebugEvent() use for the formats it writes (no printf inside it)
# NOT in the set, and allowed to log with formats: UdsHle::Run -> Dispatch / RunNdm / NoteCommand and the engine below
# them - the HLE proper, which a fake session's command reaches. At debug 0 Log() and DebugEvent() cost 32 bytes; at debug 1
# the HLE proper needs 464 bytes since 0.1.16 (1896 before): the formats it logs are written by printf_lite.hpp, not newlib's
# vsnprintf (a format printf_lite does not write still goes to newlib: Tests/line_test.cpp holds every format of the
# sources to the ones it writes), and the lines of the capture are raw words for the writer thread.
GT_BEGIN='^[[:space:]]*// >>> game-thread code'
GT_END='^[[:space:]]*// <<< game-thread code'
game_thread_file() { # $1 = file, $2 = how many regions it must have
  local begins ends
  begins=$(tr -d '\r' < "$1" | grep -c "$GT_BEGIN")
  ends=$(tr -d '\r' < "$1" | grep -c "$GT_END")
  [ "$begins" = "$2" ] && [ "$ends" = "$2" ] || { fail "$1 has $begins region starts and $ends ends of 'game-thread code', expected $2 (the scan for printf on game threads reads only what is between them)"; return; }
  # the lines of the regions (no CR, string literals blanked, no // comments), numbered; a region that starts inside another
  # or never ends is an error
  local text
  text=$(tr -d '\r' < "$1" | awk -v b="$GT_BEGIN" -v e="$GT_END" '
      $0 ~ b { if (on) print "UNBALANCED"; on = 1; next }
      $0 ~ e { if (!on) print "UNBALANCED"; on = 0; next }
      on { line = $0; gsub(/"([^"\\]|\\.)*"/, "\"\"", line); sub(/\/\/.*$/, "", line); print FNR ": " line }
      END { if (on) print "UNBALANCED" }')
  case "$text" in *UNBALANCED*) fail "$1: the 'game-thread code' markers are not balanced"; return ;; esac
  local hit
  hit=$(printf '%s\n' "$text" | grep -E '\b(printf|snprintf|sprintf|vsnprintf|vsprintf|fprintf|vfprintf|asprintf|puts|sscanf)[[:space:]]*\(|\bLog[[:space:]]*\(|\bDebugEvent[[:space:]]*\(|azl_c_log|std::(to_string|stringstream|ostringstream|string|vector)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(' | head -3)
  [ -z "$hit" ] || fail "$1 formats or allocates on a game thread (printf family, Log/DebugEvent with a format, heap): $hit"
  # (the static buffers the lines are built in are named g_*, as every static of the plugin: they are not on the stack)
  hit=$(printf '%s\n' "$text" | awk '{ s = $0; while (match(s, /char[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(\[[[:space:]]*[0-9]+[[:space:]]*\])+/)) { m = substr(s, RSTART, RLENGTH); name = m; sub(/^char[[:space:]]+/, "", name); n = m; sub(/^[^\[]*\[[[:space:]]*/, "", n); if (n + 0 > 64 && name !~ /^g_/) print $0; s = substr(s, RSTART + RLENGTH) } }' | head -2)
  [ -z "$hit" ] || fail "$1 has a char buffer over 64 bytes on a game thread: $hit"
}
game_thread_file Sources/ctr/hook.cpp 1
game_thread_file Sources/ctr/uds_hle.cpp 2
game_thread_file Sources/ctr/plugin.cpp 1
game_thread_file Sources/azl/log.cpp 4
game_thread_file Sources/azl/log_files.cpp 1
game_thread_file Includes/azl/line.hpp 1
game_thread_file Sources/azl/uds_engine.cpp 2
game_thread_file Sources/azl/room_client.cpp 1
game_thread_file Includes/azl/printf_lite.hpp 1
game_thread_file Includes/azl/apt_sleep.hpp 1
game_thread_file Includes/azl/link_policy.hpp 1
game_thread_file Includes/azl/sleep_crumbs.hpp 1
game_thread_file Sources/azl/ndm_hle.cpp 1
game_thread_file Sources/azl/platform.cpp 1
# ... and the code on the game's threads does call the printf-free lines (the scan above would also pass if the lines were
# simply dropped): the two pass-through lines are stored raw, the others are built piece by piece
need Sources/ctr/hook.cpp "DebugIpc(handle, header, arg," "the pass-through line of every IPC call is stored as raw words"
need Sources/ctr/hook.cpp "DebugSvcReal(traced->name, command, arg0, result);" "the line of a traced service is stored as raw words"
[ "$(grep -c 'g_home_observer();' Sources/ctr/hook.cpp)" = "2" ] || fail "Sources/ctr/hook.cpp: the HOME button must wake the log writer in both places of PassThroughApt (after the notification's lines, and when the lines are over)"
need Sources/ctr/uds_hle.cpp "DebugServiceCommand(uds, id, request," "the capture of a command is built without printf"
need Sources/ctr/uds_hle.cpp "DebugPullPacket(request[0]," "the capture of a PullPacket is built without printf"
need Sources/azl/uds_engine.cpp 'line.Str("uds: role ")' "the role line of the engine is built without printf (a game thread that closes its session gets there)"
need Sources/azl/uds_engine.cpp 'line.Str("uds: the service stops (")' "the sleep's StopService line is built without printf"
need Sources/azl/room_client.cpp "DebugWifiLine(dir," "the capture line of a frame that ends a session is built without printf"
# Log() and DebugEvent() keep the 400 / 200-byte line out of their own frame (a call at debug 0 costs a few words of stack)
need_after Sources/azl/log.cpp "void DebugEvent(const char* fmt, ...) {" "if (g_debug_level <= 0 || !g_dbg_ring) {" "DebugEvent() returns at once when the capture is off"
need Sources/azl/log.cpp "__attribute__((noinline)) static void LogV(const char* fmt, va_list args) {" "Log()'s 400-byte line is in a frame of its own"
need Sources/azl/log.cpp "__attribute__((noinline)) static void DebugEventV(const char* fmt, va_list args) {" "DebugEvent()'s 200-byte line is in a frame of its own"

# The gates that see this class: Tests/stack_usage.sh runs the whole-chain test, and the chain test holds the pass-through
# path to 300 bytes at both log levels (a frame budget alone missed the 944 bytes of libc below DebugEvent)
need Tests/stack_usage.sh 'chain_sim.py "$WORK/AzaharLink.elf"' "the stack check runs the whole-chain test on the built plugin"
need Tests/stack_usage.sh 'callgraph_check.py "$WORK/cg"' "the stack check asks the machine code whether a formatter can be reached from a game thread"
need Tests/stack_usage.sh 'callgraph_check.py --selftest' "the call graph checker is tried against a made-up listing first"
need Tests/chain_sim/chain_sim.py "BUDGET_PURE = 300 " "the pure pass-through path is held to 300 bytes below the entry SP, at debug 0 and 1"
need Tests/chain_sim/chain_sim.py "for level in (0, 1):" "the chain test runs at both log levels"
need_line Tests/stack_usage.sh 'budget hook "azl::ctr::PassThrough" 32' "the hot frame of every IPC call stays small"

# Test hygiene. A test that no script runs is a gate that is not there: every test source is built AND run by the script
# that owns it (the host tests by Tests/run.sh, the 32-bit ones, which drive UdsHle, by Tests/ipc_test/build.sh and then
# Tests/hle_tests.sh and Tests/integration.sh), and a new *_test.cpp that is in none of them fails here
for t in test_main ring_test dedupe_test link_policy_test soc_plan_test screen_text_test log_files_test line_test sleep_crumbs_test capture_test settings_test games_test paths_test plugin_install_test eta_test dlp_text_test dlp_prepare_test guide_test about_test logrot_test log_module_test ncch_icon_test reloc_test; do
  tr -d '\r' < Tests/run.sh | grep -q -E "^  \"\\\$OUT/$t\"( |\$)" || fail "Tests/run.sh does not run the unit test $t (a line of its own that calls \"\$OUT/$t\")"
  need Tests/run.sh "-o \"\$OUT/$t\"" "the unit test $t is built by Tests/run.sh"
done
for t in ffe_replay_test trace_replay_test result_closure_test command_matrix_test ndm_wiring_test; do
  need Tests/ipc_test/build.sh "$t" "the 32-bit test $t is built by Tests/ipc_test/build.sh"
  need Tests/hle_tests.sh "\"\$OUT/$t\"" "the 32-bit test $t is run by Tests/hle_tests.sh"
  need Tests/integration.sh "/tmp/azl_tests32/$t" "the 32-bit test $t is run by Tests/integration.sh"
done
for src in Tests/*_test.cpp Tests/settings_test/*_test.cpp; do
  name=$(basename "$src" .cpp)
  grep -w -q -- "$name" Tests/run.sh Tests/ipc_test/build.sh Tests/hle_tests.sh || fail "$src is built by no script (Tests/run.sh, Tests/ipc_test/build.sh)"
done
# The golden data of the replays are made from the real captures by Tests/make_goldens.py and carry its name: a file edited by
# hand, or one that lost its origin, fails here (run the script again with the captures instead)
for f in Tests/ffe_host_golden.inc Tests/tfh_host_golden.inc Tests/lobby_trace.inc; do
  head -1 "$f" | grep -q "Generated by Tests/make_goldens.py" || fail "$f is not marked as generated by Tests/make_goldens.py"
done
need Tests/trace_replay_test.cpp '#include "ffe_host_golden.inc"' "the FFE host replay runs on the golden data of PROVA 2"
need Tests/trace_replay_test.cpp '#include "tfh_host_golden.inc"' "the TFH host replay runs on the golden data of the first TFH trace"
need Tests/trace_replay_test.cpp '#include "lobby_trace.inc"' "the lid episode and the client stretch run on the frames of the real traces"
# the client-role replay has Azahar's own models around the 3DS, not our engine (what it is tested against must not change with it)
need Tests/ffe_replay_test.cpp "AzaharClient peer(room," "the other players of the client-role replay are the frozen Azahar client model"
need Tests/ffe_replay_test.cpp "AzaharHost host(room," "the host of the client-role replay is the frozen Azahar host model"

# A client's GetApplicationData is the application data of its host's beacon (3dbrew: "from the current beacon"). TEKKEN 3D PRIME EDITION
# reads it 100 ms after it joins and leaves the network when it gets 0 bytes (log of 2026-10-18, both consoles, both ways): the NetworkInfo a
# game joins with is the 0x3C bytes of the old Connect and holds none of it.
need_line Sources/azl/uds_engine.cpp "    if (reads_host_beacon) {" "a client or a spectator answers from the beacon of its host"
need_line Sources/azl/uds_engine.cpp "                                   connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsSpectator);" "a spectator reads its host's beacon like a client"
need Sources/azl/uds_engine.cpp "if (heard.packet.transmitter_address != network_info_.host_mac_address) {" "only the beacon of the host it joined is read"
need Sources/azl/uds_engine.cpp "if (GetBeaconApplicationData(FindBeaconTags(heard.packet.data), &beacon_data, &beacon_size)) {" "the data is the one the beacon's tag carries"
need Sources/azl/uds_engine.cpp "data = beacon_data;" "what the beacon carries is what the game is given"
need Sources/azl/uds_frames.cpp "constexpr std::size_t kBeforeData = kNetworkInfoTagSize - 2;" "the application data of a beacon starts where the generator of the tag puts it"
need Sources/azl/uds_frames.cpp "const std::size_t count = tags.network[kBeforeData - 1];" "its size is the byte the generator writes right before it"
need Sources/azl/uds_frames.cpp "if (count > kApplicationDataSize || tags.network_size < kBeforeData + count) {" "a size the tag cannot hold is not read"
need Tests/test_main.cpp "TestBeaconApplicationData();" "the test of the beacon's application data runs"

# The replies the audit of 2026-10-18 (every UDS command against the firmware) put right, each pinned where it is made:
need Sources/azl/uds_engine.cpp "if (network_node_id == 0) {" "GetNodeInformation(0) is the empty slot of nwm's table: found, as zeros, in every state"
need Sources/azl/uds_engine.cpp "connection_status_.changed_nodes |= static_cast<u16>(old_bitmask ^ connection_status_.node_bitmask);" "a client's changed nodes accumulate until the game reads them (the status event is one-shot)"
need Sources/azl/uds_engine.cpp "network_channel_ = kDefaultNetworkChannel; // (not the channel of the network hosted or joined before)" "hosting on channel 0 takes the default channel for GetChannel and for the frames"
need Sources/azl/uds_engine.cpp "return; // (a Logoff with id 0 would be adopted by every client that is still Connecting)" "an EAPoL-Start of an unknown type admits nobody and is not answered"
need Sources/azl/uds_engine.cpp "if (conn_type_ == ConnectionType::Client && logoff.assigned_node_id != 0) {" "a client that is still Connecting takes the Logoff that names its own node"
need Sources/azl/uds_engine.cpp "return pa != pb ? pb : a.rx_ms < b.rx_ms;" "the beacon of the host a client joined is never the one that makes room for another"
need Sources/ctr/uds_hle.cpp "size == 0 ? engine_->SetApplicationData(&kNone, 0) : res::kTooLarge" "SetApplicationData with size 0 and no buffer clears the data (the firmware never sees the pointer)"
for t in TestRepliesByRole TestJoinsInOneRoundTrip TestChangedNodesAccumulate TestHostBeaconKept; do
  need Tests/test_main.cpp "    $t();" "the test of the replies by role ($t) runs"
done

# The lag analysis of 2026-10-19 (NOTE_LAG_2026-10-19.md): ENet's packet throttle never destroys a game frame, expendable frames go out
# UNSEQUENCED, a pause of the network thread is neither a timeout nor a round-trip sample, and the network thread may be asked for a
# New 3DS core.
need Sources/azl/room_client.cpp "    // Only once the connection is up: the room refuses this command from a peer that is not connected yet, and a refused" "the throttle is configured only after the connect event (2026-10-20: queued before it, the real room never answered the connect)"
need_after Sources/azl/room_client.cpp "    // never answered the connect)." "enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);" "... right there"
need Sources/azl/room_client.cpp "enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);" "the packet throttle never decelerates: no frame is destroyed at the sender"
need Sources/azl/room_client.cpp "                    : static_cast<u32>(ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);" "beacons and broadcast game data go out unsequenced (never held behind a reliable packet)"
[ "$(grep -c 'EnetPacketFlags(item.reliable)' Sources/azl/room_client.cpp)" = "2" ] || fail "both places that hand a frame to ENet use EnetPacketFlags"
need Sources/enet/protocol.c "switch (enet_protocol_send_outgoing_commands (host, event, 0))" "the socket is read before the timeouts are looked at (an acknowledgement that waited is not a loss)"
need Sources/enet/protocol.c "if (peer -> lastReceiveTime > 0 && host -> azlServiceGap >= ENET_AZL_PAUSE_MS &&" "a round-trip sample taken after a pause of the thread is not taken"
need Sources/enet/protocol.c "++ enet_azl_throttled_packets;" "the frames the throttle destroys are counted"
need Sources/ctr/plugin.cpp "if (core >= -1 && core <= 3) {" "net_thread_core accepts the extra cores of a New 3DS"
need Sources/ctr/plugin.cpp ', rtt samples skipped %lu |' "the link line says how many samples were left out"
need Sources/ctr/plugin.cpp 'dropped: stale %lu cap %lu throttled %lu |' "the link line says how many frames the throttle destroyed"
need Tests/test_main.cpp "    TestEnetThrottleAndPause();" "the ENet test runs"

# The app review of 2026-10-19 (README, tappa N): nothing of a receiving stays half installed, what is half installed is found and can be deleted,
# the scan says what it is doing, the caches and the copies cost what they must and no more.
need ConfigApp/titleshare/source/receiver.c "wt->err[cur] = 0xC8604C03; // (fermata prima della fine: la pagina del risultato lo dice con queste parole)" "a title stopped half way is an error the result page explains"
need ConfigApp/source/pages_delivery.cpp 'if (s.result[i] == DL_RES_ERROR && s.err[i] == 0xC8604C03u) {' "the result page says in words that a title was stopped before the end"
need ConfigApp/titleshare/source/catalog.c "if(R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &c->titleIds[i], &infos[i]))) {" "the module asks the versions title by title when the list is refused (one broken title must not cost every icon at every start)"
need ConfigApp/titleshare/source/catalog.c "int verOk = haveVer && !verBad[i];" "a title AM cannot describe stays out of the names cache"
need ConfigApp/source/titles.cpp "if (R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &infos[i]))) {" "the app asks the versions title by title when the list is refused, and names the broken ones"
need ConfigApp/source/titles.cpp "if (R_SUCCEEDED(AM_GetPendingTitleList(&got, ids.data(), pending, MEDIATYPE_SD))) {" "the installations that were begun and never finished are found"
need ConfigApp/source/titles.cpp "if (R_FAILED(AM_ListExistingContentInfos(&got, infos.data(), 0, n, tid, MEDIATYPE_SD))) {" "a DLC with contents its ticket grants but that are not on the card is found"
need ConfigApp/source/titles.cpp "missing += owned && !there ? 1 : 0;" "a DLC content counts as missing only when the ticket grants it and it is not there"
need ConfigApp/source/titles.cpp "rc = AM_DeletePendingTitle(MEDIATYPE_SD, t.tid); // (the ticket stays: it may be the one of a title that works, which this never finished replacing)" "deleting an unfinished installation never deletes a ticket"
need ConfigApp/source/status.cpp 'report.items.push_back(MakeItem("Installed titles", Counted(static_cast<int>(t.unusable.size()), "unusable title"), CheckLevel::Warning, "Titles of no use",' "the titles of no use are a warning of the installation check"
need ConfigApp/source/status.cpp "CheckAction::CleanTitles, \"unusable titles\"));" "A on the row deletes them, and the main page names them"
need ConfigApp/source/pages_check.cpp 'PushPage(NewConfirmPage("Titles of no use", text, "Yes, delete", "No", [to_delete](bool yes) {' "the deletion is asked first, with the list"
need ConfigApp/source/pages_check.cpp "const int gone = titles::DeleteUnusable(to_delete, &failed);" "A deletes what the scan found of no use"
need ConfigApp/source/pages_check.cpp "g.rescan_wanted = true; // (the list of titles and the check change: the scan runs again and the row is made from it)" "the titles are read again after the deletion"
need ConfigApp/source/pages_delivery.cpp 'g.ShowToast(gfx::Clean(dl_item_name(item)) + ": incomplete on this console, it cannot be sent (see Installation check).", col::kWarn, 5000);' "a title that is not whole on the card is not offered for sending"
need ConfigApp/source/ui.cpp "facts.titles = titles::Scan().report().health; // (the row of the titles: what the scan found of no use)" "the check's row of the titles comes from the scan"
need ConfigApp/source/main.cpp "if (!g.titles_health_applied && titles::Scan().done()) {" "the check is made again when a scan ends"
need ConfigApp/source/status.cpp 'std::snprintf(buf, sizeof(buf), "names: %d of %d titles", done, total);' "the scan says which step it is in (the names of every title, then the games) instead of one count over both"
need ConfigApp/source/titles.cpp "impl->phase.store(1);" "the scan reports its second step"
! grep -q "TitlesProgressText" ConfigApp/source/*.cpp ConfigApp/source/*.hpp || fail "the old progress text (one count over the two steps of the scan) is back"
need ConfigApp/source/settings.cpp "entry.password = stored.password; // (... and its password: a caller that knows only the address must not wipe it)" "a room used again by its address keeps its password"
need ConfigApp/source/pages_test.cpp "tested.password = g.cfg.password; // (the test used it: the room in the list keeps it)" "the connection test hands the room its password"
need ConfigApp/source/main.cpp "if (!g.save_ok && g.settings_loaded != setup::LoadResult::Unreadable) {" "the retry of a failed save on the way out never writes over a file that could not be read"
need ConfigApp/source/games.cpp "ok = ok && fstat(fileno(f), &st) == 0 && static_cast<u64>(st.st_size) == sizeof(header) + static_cast<u64>(count) * kCacheRecordBytes;" "the games cache is measured before memory is asked for it"
need ConfigApp/source/games.cpp "const bool same_size = FileSize(GamePluginPath(tid), &have) && have == want; // (another size is stale without reading a byte of either)" "a copy of another size is brought up to date without reading it"
need ConfigApp/source/delivery_run.cpp "if (run.result.start_ms != 0) {" "the titles are read again only after a receiving that began"
need ConfigApp/source/ui.cpp "setup::RemoveParkFiles(paths.park_dir);" "the block files a cut-off receiving left are removed when the app starts"
need ConfigApp/source/paths.cpp 'if (name.size() > 4 && name.compare(name.size() - 4, 4, ".blk") == 0) {' "only the block files go from park/"
need ConfigApp/source/ui.cpp "if (plugin_copies_checked || !plugin_ready || !titles::Scan().done()) {" "the quiet look at the copies of the plugin waits for the scan of the titles"
need ConfigApp/source/ui.cpp "if (!stamp.empty() && stamp == app_cfg.plugin_stamp) {" "the copies are not read again while the plugin file is the one they were brought up to"
need ConfigApp/source/main.cpp "g.app_cfg.plugin_stamp = g.plugin_stamp_pending;" "the stamp of the plugin file is remembered when the looks are done"
need ConfigApp/source/appcfg.cpp '} else if (key == "plugin_stamp") {' "3DSLink.cfg holds the stamp"
for t in TestTitlesRow; do
  need Tests/settings_test/games_test.cpp "    $t();" "the test of the titles row ($t) runs"
done
need Tests/settings_test/paths_test.cpp "    TestFileStampAndPark();" "the test of the file stamp and the park cleanup runs"

# The performance round of 2026-10-19 (NOTE_LAG_2026-10-19.md, points 5-8; README, tappa O). The engine's lock: events are
# signalled after it is released (Guard), SendTo builds and hands over its frame without it, Tick takes it only when due; the
# network thread looks at the queue through an atomic and waits less after the game sends; the diagnostics behind the log;
# radio_like=2: frames numbered and the copy dropped, the room's reliable stream handed over as it arrives.
need Includes/azl/uds_engine.hpp "        explicit Guard(const UdsEngine* e);" "every entry point of the engine takes its lock through the guard"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'platform::LockGuard lock(mu_);')" = "0" ] || fail "Sources/azl/uds_engine.cpp takes mu_ without the guard (a signal under the lock again)"
need Sources/azl/uds_engine.cpp "    engine->mu_.unlock();" "the guard releases the lock ..."
need_after Sources/azl/uds_engine.cpp "    engine->mu_.unlock();" "engine->kernel_->SignalEvent(handles[i]);" "... and signals the queued events only then"
need Sources/azl/uds_engine.cpp "    QueueSignalLocked(bind->event); // (given once the lock is free: the game's PullPacket must not find it taken)" "a data frame's signal is queued, not given under the lock"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'QueueSignalLocked(connection_status_event_);')" = "11" ] || fail "the status event is signalled under the lock somewhere (QueueSignalLocked expected in 11 places)"
need Sources/azl/uds_engine.cpp "    packet.data = GenerateDataPayload(data, data_size, data_channel, dest_node_id, own_node_id, sequence);" "SendTo builds its frame outside the lock (the guard's block ends before it)"
need Sources/azl/uds_engine.cpp "    if (!beacon_due && !presence) {" "Tick takes the lock only for a beacon that is due or a member being timed"
need Sources/azl/uds_engine.cpp "        host_beacon_ms_.compare_exchange_strong(heard, now_ms, std::memory_order_relaxed); // just joined: the clock starts now" "the client's beacon bookkeeping runs without the lock"
need Sources/azl/uds_engine.cpp "    role_.store(connection_status_.status, std::memory_order_relaxed);" "the role mirror follows every status change"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'connection_status_.status = static_cast<u32>(NetworkStatus::')" = "0" ] || fail "connection_status_.status is assigned outside SetStatusLocked (the mirror would lag)"
need Sources/azl/uds_engine.cpp "    connection_status_.status = static_cast<u32>(status);" "SetStatusLocked is the one place the status changes"
need Includes/azl/uds_ring.hpp "    Put Enqueue(u16 dest, u16 src, u8 channel, const u8* payload, u32 n, u32 stamp_ms = 0) {" "the ring keeps a stamp per frame"
need Sources/azl/uds_engine.cpp "        NotePullLatency(static_cast<u32>(platform::NowMs()) - queued_ms); // (from the ring's stamp: how long the game let it wait)" "PullPacket measures the wait of the frame it takes (with the log on)"
need Includes/azl/room_client.hpp "inline u32 ServiceTimeoutFor(bool queued, u32 now_ms, u32 last_enqueue_ms) {" "the wait in the socket follows the game's frames"
need Includes/azl/room_client.hpp "constexpr u32 kQuickPollMs = 1;" "1 ms after the game sent"
need Includes/azl/room_client.hpp "constexpr u32 kQuickPollWindowMs = 10;" "... for 10 ms"
need Sources/azl/room_client.cpp "        if (tx_pending_.load(std::memory_order_acquire) != 0) { // (the game's thread sets it under the lock: nothing pushed, no lock)" "the queue is looked at through an atomic, not the lock, every round"
need Sources/ctr/uds_hle.cpp "            svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);" "the first call of a command says the thread and its priority"
need Sources/azl/log.cpp "    c.plugin10 = c.net10 + c.uds10 + c.frame10;" "the writer's wall time is not in the CPU total"
need Includes/enet/enet.h "   enet_uint32          azlDeliverOutOfOrder;" "ENet can hand a reliable packet over ahead of a hole"
need Sources/enet/peer.c "          placeholder -> azlDispatched = 1;" "... leaving a placeholder for the sequence accounting"
need_after Sources/enet/peer.c "          if (incomingCommand -> azlDispatched)" "enet_free (incomingCommand);" "... that is freed, never delivered, when its turn comes"
need Sources/enet/protocol.c "return 0; /* AzaharLink: the packet of this fragment was handed over already (a late copy of one of its fragments) */" "a late fragment of a packet handed over already is ignored"
need Sources/azl/room_client.cpp "        client->azlDeliverOutOfOrder = fast_level_.load() >= 2 ? 1u : 0u; // (the room's reliable stream handed over as it arrives)" "the room's stream is handed over as it arrives only at radio_like=2"
need Includes/azl/room_client.hpp "    } else if (level >= 2 && peer_numbers) {" "unicast game data goes unreliable and twice only at radio_like=2, and only to a console that numbers its frames"
need_after Includes/azl/room_client.hpp "    } else if (level >= 2 && peer_numbers) {" "d.duplicate = true;" "... twice"
need Sources/azl/uds_engine.cpp "        peer_numbers = dest_node_id != kBroadcastNetworkNodeId && copy_dedupe_.Numbers(dest_node_id);" "the engine says whether the destination numbers its frames (an Azahar gets the frame once, reliable)"
need Sources/azl/uds_engine.cpp "            out_packet.peer_numbers = copy_dedupe_.Numbers(secure_data.dest_node_id);" "... for a relayed frame too"
need Sources/azl/room_client.cpp "                ++g_stats.tx_duplicates;" "the copies are counted"
need Sources/azl/uds_engine.cpp "        sequence = ++tx_seq_;" "SendTo numbers its frames"
need Sources/azl/uds_engine.cpp "    tx_seq_ = static_cast<u16>(platform::NowMs() & 0x7FFFu);" "the numbers start from the clock"
need Includes/azl/uds_dedupe.hpp "        if (sequence == 0 || node == 0 || node >= kNodes) {" "an unnumbered frame (Azahar, an older plugin) is never dropped as a copy"
need_after Sources/azl/uds_engine.cpp "    // The copy a radio_like=2 sender adds (the same frame, the same number, a round later): the first one was delivered." "if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {" "a copy is dropped before the ring"
need Sources/azl/uds_engine.cpp "            if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {" "... and before the host forwards"
need Sources/azl/uds_engine.cpp "    copy_dedupe_.ForgetNode(node.node_id); // (the next console with this id numbers its frames from its own start)" "a node that leaves is forgotten (host)"
need Sources/azl/uds_engine.cpp "            copy_dedupe_.ForgetNode(logoff.assigned_node_id);" "a console that rejoined its old slot is forgotten (client)"
need Sources/azl/uds_engine.cpp "    kernel_->SignalEvent(connect_event_);" "the connect event is signalled under the lock (a deferred one could follow the next attempt's ClearEvent)"
need Sources/azl/uds_engine.cpp "        DrainPendingSignalLocked(bind.event); // (a teardown queued one: given before the close, as before the round)" "a signal queued for an event is given before the event is closed"
need Sources/azl/uds_engine.cpp "            out_packet.relayed = true; // (for the room client: a unicast frame of the radio, retried, not a broadcast)" "a relayed frame is unicast for the delivery rule"
need Sources/azl/room_client.cpp "                                               packet.destination_address == kBroadcastMac && !packet.relayed, packet.peer_numbers);" "... and the room client treats it so, and gives the copy only to a peer that numbers"
need Sources/azl/room_client.cpp "    client->azlEarlyFilter = EarlyFilter;" "only game frames and beacons go ahead of a hole"
need Sources/azl/room_client.cpp "    return ether == static_cast<u16>(EtherType::SecureData) ? 1 : 0;" "... the handshake and the room's messages keep their order"
need Sources/enet/peer.c "          if (peer -> host -> azlEarlyFilter != NULL &&" "ENet asks the filter before the early hand-over"
need Includes/azl/uds_dedupe.hpp "    static constexpr u32 kSeen = 128;" "the receiver remembers a whole batch of copies"
need Sources/azl/room_client.cpp 'static_assert(kRepeatMax * 2 <= CopyDedupe::kSeen, "the receiver remembers a whole batch of copies and what came between");' "... pinned against the sender's batch"
need Sources/azl/log.cpp "    out.lock_hold_ticks = take(g_stats.lock_hold_ticks);" "TakeStats takes the lock's hold sum"
need ConfigApp/source/settings.cpp "if (c >= -1 && c <= 3) {" "the app accepts net_thread_core 2 and 3 (the A/B key must survive a save)"
need Sources/ctr/uds_hle.cpp "        static bool said[kStatsCommands]; // (never cleared: the line below is said once per command in the life of the plugin)" "the first-caller line is said once, not once a window"
need Sources/main.cpp "        const int level = radio ? g_radio_level : 0;" "the in-game switch keeps the configured level (radio_like=2 is not turned into 1)"
need Sources/azl/uds_engine.cpp "    if (twin_unmatched) {" "a forwarded copy with no twin is counted once the copy rule let it through"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c '        ForgetChangedNodesLocked(before);')" = "2" ] || fail "a client does not forget the numbers of a slot that changed console (both Logoff refreshes)"
need Tests/azahar_rules.hpp "            Add(\"secure-shape\", \"SecureData with aggregate count \" + std::to_string(LoadBE16(raw + 2)) + \" (Azahar emits 0)\");" "the Azahar rule leaves the sequence number free"
for t in TestSignalsAfterUnlock TestTickWithoutLock TestServiceTimeoutRule TestEnetOutOfOrder TestNumberedFrames; do
  need Tests/test_main.cpp "    $t();" "the test $t of the performance round runs"
done
need Tests/dedupe_test.cpp "    TestCopyDedupeUnit();" "the test of the copy dedupe runs"
need Tests/chain_sim/chain_sim.py "sim.log_lines[0].startswith('uds: command 0x23 is first called by thread ')" "the chain test expects the first-caller line"
need ConfigApp/titleshare_host/delivery.c "gSizes[i] = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &ids[i], &infos[i])) ? infos[i].size : 0;" "the sizes of the send list are asked title by title when the list is refused"

# 3DS Link is ONE product with ONE version (the user's rule, 2026-10-21): the app, the plugin, both CIAs, the Download Play child and the documents all say the same
# X.Y.Z, and the file VERSION holds it. A patch runs Tests/set_version.py, which rewrites every point below; this check refuses a tree where one of them says
# another number (a forgotten point is a failed check, not a surprise on a console). The TMD of the CIAs and the SMDH descriptions are made from VERSION when
# DlpLink/tools/build_all.sh builds them (DlpLink/tools/verify_cia.py checks the result).
[ -f VERSION ] || fail "VERSION is missing (the one number of 3DS Link: Tests/set_version.py writes it)"
VERSION=$(tr -d '\r\n ' < VERSION)
case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) fail "VERSION is not X.Y.Z ('$VERSION')" ;;
esac
MAJOR=${VERSION%%.*}; REST=${VERSION#*.}; MINOR=${REST%%.*}; REV=${REST#*.}
{ [ "$MAJOR" -lt 64 ] && [ "$MINOR" -lt 64 ] && [ "$REV" -lt 16 ]; } 2>/dev/null || fail "VERSION $VERSION does not fit the TMD of the CIAs (major 0-63, minor 0-63, micro 0-15: major << 10 | minor << 4 | micro)"
need Includes/ctr/plugin.hpp "kVersion = \"$VERSION\";" "the plugin says $VERSION (its log header and menu text)"
need Sources/main.cpp "PluginMenu(\"AzaharLink\", $MAJOR, $MINOR, $REV," "the menu's version integers say $VERSION"
for kv in "Major: $MAJOR" "Minor: $MINOR" "Revision: $REV"; do
  tr -d '\r' < CTRPluginFramework.plgInfo | grep -q -x "    $kv" || fail "CTRPluginFramework.plgInfo does not say '$kv' (the header of the 3gx says $VERSION)"
done
need ConfigApp/source/settings.hpp "kVersion = \"$VERSION\";" "the app says $VERSION"
tr -d '\r' < README.md | grep -q "^> \*\*Stato attuale: $VERSION" || fail "README.md does not say '$VERSION' as the current version (Stato attuale)"
tr -d '\r' < README.md | grep -q "^## Novità $VERSION" || fail "README.md has no section 'Novità $VERSION'"
need PROVE_HARDWARE.md "# Prove su hardware - AzaharLink $VERSION" "the plan of the hardware tests names $VERSION"
need dist/LEGGIMI.txt "AzaharLink $VERSION - LEGGIMI" "the short guide names $VERSION"
#  - every OTHER point that carries the number (the log header the tests expect, the CIA row of the short guide, the README's lines about the one version, the
#    test plan of the installer) and the README's own section and history row: Tests/set_version.py --check rewrites them all with VERSION and compares
PY=python; command -v python >/dev/null 2>&1 || PY=python3
"$PY" Tests/set_version.py --check >/dev/null || fail "a point that carries the version does not say $VERSION, or the README has the stub of a new version or no history row for it (Tests/set_version.py --check says which)"
#  - the .3gx files carry the version in their header: byte 9 = revision, 10 = minor, 11 = major
for f in AzaharLink.3gx ConfigApp/data/AzaharLink.3gx; do
  [ -f "$f" ] || continue
  [ "$(od -An -tu1 -j9 -N3 "$f" | tr -s ' ')" = " $REV $MINOR $MAJOR" ] || fail "$f is not a plugin of version $VERSION (its header says $(od -An -tu1 -j11 -N1 "$f" | tr -d ' ').$(od -An -tu1 -j10 -N1 "$f" | tr -d ' ').$(od -An -tu1 -j9 -N1 "$f" | tr -d ' ')): build the plugin again"
done
#  - ONE package (the user's rule, 2026-10-22): the plugin is inside the app, so it is not a file of dist/ (a plugin that sits there is a second way to install, and goes stale: build_all.sh puts an old one in dist/old)
[ ! -e dist/AzaharLink.3gx ] || fail "dist/AzaharLink.3gx is there: 3DS Link is ONE package, the plugin is inside 3DSLink.cia and 3DSLink.3dsx and the app writes it to the card (DlpLink/tools/build_all.sh --dist moves an old one to dist/old)"
#  - the descriptions of the SMDH (home menu) say the version, from the same file
need ConfigApp/Makefile 'APP_VERSION	:= $(shell cat $(TOPDIR)/../VERSION)' "the app's Makefile reads the version from VERSION"
need_line ConfigApp/Makefile $'APP_DESCRIPTION\t:= Connect, share, play together · v$(APP_VERSION)' "the description of the app in the home menu says the version"
need_line ConfigApp/Makefile $'$(OUTPUT).smdh\t:\t$(TOPDIR)/../VERSION' "the SMDH of the app is made again when VERSION changes (otherwise its description keeps the old number)"

# The documents the user reads (stage C). They call the app and its pages what the app calls them, they say where the files go, and
# the numbers of the two files of dist/ in them are the numbers of the files.
#  - the old name of the delivery module: one sentence of the README says where the module comes from, and no other document (nor any
#    line of the README) names it; the old names of the two pages are gone from all three
[ "$(tr -d '\r' < README.md | grep -c 'TitleShare')" = "1" ] || fail "README.md names TitleShare on a number of lines other than one (a single history sentence says where the module comes from)"
for f in PROVE_HARDWARE.md dist/LEGGIMI.txt dist/old/LEGGI.txt; do
  ! grep -q 'TitleShare' "$f" || fail "$f names TitleShare: the user knows no such app"
done
for f in README.md PROVE_HARDWARE.md dist/LEGGIMI.txt dist/old/LEGGI.txt; do
  ! grep -q -E 'Enable game|Game delivery|\*Files\*|"Files"' "$f" || fail "$f uses a name the app no longer has (Enable game, Game delivery, the Files page)"
done
#  - dist/LEGGIMI.txt is plain ASCII (the person who reads it on a console-side PC may not have the accents)
! LC_ALL=C grep -q -E '[^[:print:][:space:]]' dist/LEGGIMI.txt || fail "dist/LEGGIMI.txt has a character that is not ASCII"
#  - the install rule and the first-start question are said as the app says them
need README.md '**un pacchetto solo: il plugin `AzaharLink.3gx` è dentro l'"'"'app**, che lo scrive da sola in `sdmc:/3ds/3DSLink/AzaharLink.3gx`' "the table of what to copy says that the plugin is inside the app and where the app puts it (one package: nothing else to copy)"
need PROVE_HARDWARE.md '**Un pacchetto solo: il plugin `AzaharLink.3gx` è dentro l'"'"'app**, che lo scrive da sola in `sdmc:/3ds/3DSLink/AzaharLink.3gx`' "the plan says that the plugin is inside the app and where the app puts it (one package)"
need dist/LEGGIMI.txt "UN PACCHETTO SOLO: il plugin AzaharLink.3gx e' DENTRO l'app, e l'app lo scrive da sola in SD:/3ds/3DSLink/AzaharLink.3gx" "the short guide says that the plugin is inside the app and where the app puts it (one package)"
! grep -q "NELLA STESSA CARTELLA di 3DSLink.3dsx" dist/LEGGIMI.txt || fail "dist/LEGGIMI.txt tells to put the plugin in the folder of the app again (the app carries it and writes it)"
! grep -q 'dist/AzaharLink.3gx` | \*\*nella stessa cartella' README.md || fail "README.md tells to copy dist/AzaharLink.3gx next to the app again (the app carries the plugin and writes it)"
FIRST_START_QUESTION="First start: 3DS Link will scan the SD card and check your installation. This takes a while. Continue?"
need ConfigApp/source/pages_startup.cpp "\"${FIRST_START_QUESTION}\";" "the question of the first start, as the page holds it (not as the comment at the top of the file says it)"
need ConfigApp/source/pages_startup.cpp "DrawPopupBox(box, \"Continue?\", kQuestion);" "the welcome popup draws that question, in a box that fits it"
for f in README.md PROVE_HARDWARE.md dist/LEGGIMI.txt; do
  need "$f" "$FIRST_START_QUESTION" "the document quotes the question of the first start as the app asks it"
done
need dist/LEGGIMI.txt "3DSLink-TitleDelivery.log   il registro" "the delivery log is described next to the plugin's logs"
#  - the numbers of the files that are delivered: size and md5, in the two documents that give them. The plugin is not a file of dist/ (it is inside the app): its numbers are those of the
#    resource the app is built with, ConfigApp/data/AzaharLink.3gx, which is the file that has to appear on the card
for f in dist/3DSLink.3dsx ConfigApp/data/AzaharLink.3gx; do
  [ -f "$f" ] || { [ "$BUILT" != 1 ] || fail "$f is missing"; continue; }
  SUM=$(md5sum "$f" | cut -d' ' -f1)
  SIZE=$(stat -c %s "$f" | sed -E ':a;s/([0-9])([0-9]{3})($|\.)/\1.\2\3/;ta')
  need PROVE_HARDWARE.md "$SUM" "the md5 of $f"
  need dist/LEGGIMI.txt "$SUM" "the md5 of $f"
  need PROVE_HARDWARE.md "$SIZE byte" "the size of $f"
  need dist/LEGGIMI.txt "$SIZE byte" "the size of $f"
done
for f in 3DSLink.cia; do   # (3DS Link is ONE CIA: no dist/dlp/child.cia, the app makes the Download Play child)
  [ -f "dist/$f" ] || continue
  SUM=$(md5sum "dist/$f" | cut -d' ' -f1)
  SIZE=$(stat -c %s "dist/$f" | sed -E ':a;s/([0-9])([0-9]{3})($|\.)/\1.\2\3/;ta')
  need PROVE_HARDWARE.md "$SUM" "the md5 of dist/$f"
  need dist/LEGGIMI.txt "$SUM" "the md5 of dist/$f"
  need PROVE_HARDWARE.md "$SIZE byte" "the size of dist/$f"
  need dist/LEGGIMI.txt "$SIZE byte" "the size of dist/$f"
done
#  - dist/3DSLink.3dsx is the app of the tree (where the tree has been built)
[ ! -f ConfigApp/3DSLink.3dsx ] || cmp -s dist/3DSLink.3dsx ConfigApp/3DSLink.3dsx || fail "dist/3DSLink.3dsx is not ConfigApp/3DSLink.3dsx (copy the product of the clean build, and the numbers in the documents)"

# 3DS Link is ONE CIA (the user's rule, 2026-10-21): the Download Play child is not a file that is handed out, the app builds it on the console (ConfigApp/dlpc, DlpLink/docs/COMPOSITORE.md)
# from its own CIA, the plugin and the MODEL of the child that it carries as a resource (the child without its RomFS: DlpLink/tools/make_model.py). The app's Makefile takes in the composer,
# the model and the pictures of the guide; the model in the tree is a real one of this version; and what is delivered carries it. The build chain's order has no cycle.
need_line ConfigApp/Makefile $'SOURCES\t\t+=\tdlpc guide' "the app builds the composer (dlpc/) and the guide (guide/) whole"
need_line ConfigApp/Makefile $'DATA\t\t:=\tdata' "data/child_model.bin is linked in as the model resource"
need_line ConfigApp/Makefile $'GRAPHICS\t:=\tguide/gfx' "the pictures of the guide are made from the .t3s files of guide/gfx"
need_line ConfigApp/Makefile $'INCLUDES\t:=\t../Includes source' "the guide includes the program's own gfx.hpp, so source/ is on the include path"
need_line ConfigApp/Makefile $'GFXBUILD\t:=\t$(BUILD)' "the pictures are linked in as data, made next to the objects"
need ConfigApp/Makefile '@tex3ds -i $< -H $*.h -d $*.d -o $*.t3x' "a picture is made from its .t3s into a .t3x and a header, next to the objects"
need ConfigApp/Makefile $'%.t3x.o\t%_t3x.h :\t%.t3x' "a .t3x is linked in as data (its symbol is stepN_t3x)"
need ConfigApp/Makefile $'%.bin.o\t%_bin.h :\t%.bin' "the model (data/child_model.bin) is linked in by bin2o as child_model_bin"
[ "$(ls ConfigApp/dlpc/*.c | wc -l)" = "1" ] || fail "ConfigApp/dlpc holds another .c file than dlpc.c (the Makefile builds the whole folder: nothing else may be there)"
[ -f ConfigApp/dlpc/dlpc.c ] && [ -f ConfigApp/dlpc/dlpc.h ] || fail "the composer (ConfigApp/dlpc/dlpc.c and dlpc.h) is not there"
need ConfigApp/source/dlp_prepare.cpp 'g_model_version = dlpc_model_version(child_model_bin, child_model_bin_size);' "the model's version is asked of the composer, which hashes the whole resource"
need ConfigApp/source/dlp_prepare.cpp 'if (!g_model_version_known) {' "...once: the model is a constant of the build, and the page asks three times each time it opens (the hash of 250 KB is not repeated on the screen's thread)"
need ConfigApp/source/pages_dlp.cpp 'if (dlp::ModelVersion() == 0) {' "a build whose resource is no model (the placeholder) says so and does not go on"
need ConfigApp/source/dlp_prepare.cpp 'if (ModelVersion() == 0) {' "the work does not start without a model (a result of its own: nothing is written)"
need ConfigApp/dlpc/dlpc.c 'if (c->mi.version != j->version16) {' "the composer refuses a model of another version than the app (the app and its model are from one build)"
need ConfigApp/dlpc/dlpc.c 'if (c->mi.emu != c->emu) {' "the composer refuses the emulator model for the real child and the other way round"
need ConfigApp/dlpc/dlpc.c '#define MAX_CHILD 0x02000000u' "the child cannot pass 32 MiB: what dlph_open takes"
need ConfigApp/dlpc/dlpc.c 'remove(j->out_path); // a failure leaves no child' "a job starts by removing the old child, and a failure leaves none"
need ConfigApp/dlpc/dlpc.c 'if (rename(c->part, j->out_path) != 0) {' "the child is put in place by a rename of the finished and verified .part file"
need ConfigApp/dlpc/dlpc.c 'snprintf(c->part, sizeof(c->part), "%s.part", j->out_path);' "the child is written as <path>.part first"
need ConfigApp/source/dlp_prepare.cpp 'job.emu_variant = 0;' "the package that is delivered is the real child: the emulator variant is for the tests"
if [ -d ../DlpLink ]; then
  M16=$(( (MAJOR << 10) | (MINOR << 4) | REV ))
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py model ConfigApp/data/child_model.bin --version16 "$M16" >/dev/null 2>&1 || fail "ConfigApp/data/child_model.bin is not a model of the child of version $VERSION (make_model.py --check, TMD version $M16): DlpLink/tools/build_all.sh makes it"
  # the delivered files carry exactly this model and the pictures of the guide (the pictures are the build's own t3x files; a tree that was never built has none)
  WHAT="ConfigApp/data/child_model.bin"; [ ! -f ConfigApp/build/step1.t3x ] || WHAT="$WHAT ConfigApp/build/step1.t3x ConfigApp/build/step2.t3x ConfigApp/build/step3.t3x ConfigApp/build/step4.t3x ConfigApp/build/step5.t3x ConfigApp/build/step6.t3x ConfigApp/build/step7.t3x"
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py carries $WHAT --in dist/3DSLink.3dsx dist/3DSLink.cia >/dev/null 2>&1 || fail "dist/3DSLink.3dsx or dist/3DSLink.cia does not carry the model that is in ConfigApp/data (and the pictures of the guide): the app that is delivered must be built with it (DlpLink/tools/build_all.sh --dist)"
  # the installer inside the model carries the guide as well (the same pictures as the app's, the same bytes)
  [ ! -f ConfigApp/build/step1.t3x ] || "$PY" ../DlpLink/tools/check_chain.py carries ConfigApp/build/step1.t3x ConfigApp/build/step2.t3x ConfigApp/build/step3.t3x ConfigApp/build/step4.t3x ConfigApp/build/step5.t3x ConfigApp/build/step6.t3x ConfigApp/build/step7.t3x --in ConfigApp/data/child_model.bin >/dev/null 2>&1 || fail "the installer in ConfigApp/data/child_model.bin does not carry the pictures of the guide"
fi
# The package page: before the room opens it looks at child.cia; a good one (the app's version) goes straight to the room, otherwise it is built (never with the room open), from the
# app's own CIA (a file left by the installer, else the installed title through the module), the plugin and the model, on a thread of its own, with a screen that says what it does.
need ConfigApp/source/pages_dlp.cpp 'const dlp::Verdict verdict = force ? dlp::Verdict::NotCia : dlp::CheckChildFile(g.paths.dlp_child, version16);' "a package that is there and is this version of the app is looked at first"
need ConfigApp/source/pages_dlp.cpp 'if (verdict == dlp::Verdict::Good) {' "a good package opens the room at once; nothing is built"
[ "$(grep -c -E '^ +Open\(\);' ConfigApp/source/pages_dlp.cpp)" = "2" ] || fail "ConfigApp/source/pages_dlp.cpp opens the room from more or fewer places than two (a good package, and the work that ended in Done): the room is never open while the package is built"
need_after ConfigApp/source/pages_dlp.cpp 'if (state == dlp::PrepareState::Done) {' 'Open();' "the room opens only when the work ended in Done"
need ConfigApp/source/pages_dlp.cpp 'died_ = dlp::DiedRuns(g.paths.dlp_marker);' "a marker left by a run that died is looked for before the work starts"
need ConfigApp/source/pages_dlp.cpp 'if (died_ > 0 && !asked_) {' "a run that did not end is asked about, not looped on"
need ConfigApp/source/pages_dlp.cpp 'const bool keys = scan_done && delivery::AttachModule() && dl_have_keys() != 0;' "the installed 3DS Link is read only when the scan is done and its context has the keys"
need ConfigApp/source/pages_dlp.cpp 'if (source == dlp::Source::WaitScan) {' "without a good file the page waits for the scan of the titles (the key comes with it)"
need ConfigApp/source/pages_dlp.cpp 'if (source == dlp::Source::None) {' "with neither a file nor a key the page says exactly that"
need ConfigApp/source/pages_dlp.cpp 'if (g.status.sd_known && g.status.sd_free < dlp::SpaceNeeded(' "the card is asked for room before the work"
need ConfigApp/source/pages_dlp.cpp 'job.use_file = source == dlp::Source::File;' "a good file is composed from as it is"
need ConfigApp/source/pages_dlp.cpp 'job.allow_title = keys;' "the installed title may be read only with the keys"
need ConfigApp/source/pages_dlp.cpp $'        holds_.Take(); // (HOME and sleep are off, and the keys of the scan\'s context stay, until the work ends)' "HOME and sleep are off while the package is built, and the scan's context is not freed under the work"
need ConfigApp/source/pages_dlp.cpp 'if (back && !cancelling_) {' "B asks first, and only once: it does not stop the work by itself"
need ConfigApp/source/pages_dlp.cpp 'prep_->Cancel(); // (the composer stops within one slice and leaves nothing behind; the page waits for it)' "the question's A stops the work through the composer, which leaves nothing behind"
need ConfigApp/source/dlp_prepare.cpp 'if (!setup::WriteTextFile(job_.marker_path, MarkerText(job_.attempt, job_.version_text))) {' "the marker goes down BEFORE the work begins"
need_after ConfigApp/source/dlp_prepare.cpp 'const int rc = Execute();' 'std::remove(job_.marker_path.c_str());' "the marker comes up AFTER the work, however it ends"
need ConfigApp/source/dlp_prepare.cpp 'std::remove((job_.out_path + ".part").c_str());' "what a run that died left of the package is removed before the work"
need ConfigApp/source/dlp_prepare.cpp 'thread_ = threadCreate(Hooks::ThreadMain, this, kWorkerStack, prio < 0x3F ? prio + 1 : prio, -2, false);' "the work is a thread a little less important than the screen (the page keeps drawing and B keeps answering)"
need ConfigApp/source/dlp_prepare.cpp 'return p->cancel_.load() ? 1 : 0; // (nonzero stops the composer: it leaves nothing behind)' "the composer is asked after every slice whether to stop"
need ConfigApp/source/dlp_prepare.cpp 'if (dlpc_memory_needed(&sizes) + kWorkerSlackBytes + kWorkerStack > dl_heap_free()) {' "a heap that cannot hold the package is told in words before anything is written"
need ConfigApp/source/dlp_prepare.cpp 'if (!facts.parsed || facts.tid != kAppTid || facts.version != job_.version16) {' "the installed 3DS Link that is copied is the app's title, of this version"
need ConfigApp/source/dlp_prepare.cpp 'if (std::rename(part.c_str(), job_.app_cia_path.c_str()) != 0) {' "the copy of the app's CIA is put in place by a rename of a finished .part file"
need ConfigApp/source/dlp_prepare.cpp 'ui::g.HoldHome();' "the work holds HOME"
need ConfigApp/source/dlp_prepare.cpp 'aptSetSleepAllowed(false);' "the work refuses sleep"
need ConfigApp/source/dlp_prepare.cpp '++ui::g.module_holds;' "the work keeps the module's context (no rescan under it)"
need ConfigApp/source/dlp_prepare.cpp 'threadJoin(thread_, U64_MAX); // (U64_MAX never times out: a timeout would read as success)' "the work is waited for with no timeout (a timeout reads as success on the 3DS)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (app_file == Verdict::Good) {' "the first source of the app's CIA is the good file (the installer leaves it: no keys needed)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (!scan_done) {' "the second is the installed title, which needs the scan's keys: the page waits for the scan"
need ConfigApp/source/dlp_prepare_logic.hpp 'return keys ? Source::Title : Source::None;' "with the scan done: the installed title when the keys are there, nothing otherwise"
need ConfigApp/source/dlp_prepare_logic.hpp 'constexpr u64 kAppTid = 0x000400000F83D100ull;' "the title that the child installs is 3DS Link's (DlpLink/tools/common.sh: unique id 0x0F83D1)"
need ConfigApp/source/dlp_prepare_logic.hpp 'constexpr u64 kMaxChildBytes = 32ull << 20;' "a child past 32 MiB is no package (dlph_open)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if ((facts.tid >> 32) != 0x00040001u) {' "child.cia must be a title of the Download Play kind"
need ConfigApp/source/dlp_prepare_logic.hpp 'return f < 0.0 ? 0.0 : (f > 0.99 ? 0.99 : f);' "the bar is never full before the end"
[ "$(grep -c -F 'return facts.version == version16 ? Verdict::Good : Verdict::WrongVersion;' ConfigApp/source/dlp_prepare_logic.hpp)" = "2" ] || fail "ConfigApp/source/dlp_prepare_logic.hpp: a package and an app copy are good only when their TMD says the version of this app (two judgements, one line each)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (facts.tid != kAppTid) {' "the app's own copy must be the app's title"
need ConfigApp/source/dlp_prepare_logic.hpp 'double aes = 0.7;' "the speed of the software AES is taken low: a time longer than the work is better than a shorter one"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpMarkerFileName = "child.preparing";' "the marker of a run in progress is dlp/child.preparing"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpAppCiaFileName = "3dslink.cia";' "the app's own CIA is dlp/3dslink.cia: where the installer of the child leaves it"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpChildFileName = "child.cia";' "the package that is handed out is dlp/child.cia"
# The emergency switch (Settings > Download Play delivery, 3DSLink.cfg: the app's own file, never the plugin's ini): on by default, an unreadable value is on, it is written every time (a hand edit
# flips a digit), and with it off the entry is faint, the page does not open and nothing of the feature runs. The same entry is shut, as Titles Delivery's Send Titles is, without boot9.bin.
need ConfigApp/source/appcfg.hpp 'bool dlp_delivery = true;' "the feature is on by default"
need ConfigApp/source/appcfg.cpp 'cfg.dlp_delivery = azl::ini::ParseBool(value, true);' "the switch is read with the plugin's words, and a slip means on"
need ConfigApp/source/appcfg.cpp $'out += std::string("dlp_delivery=") + (cfg.dlp_delivery ? "1" : "0") + "\\n";' "the switch is written every time, so that a hand edit only flips a digit"
need ConfigApp/source/pages_config.cpp 'dlp_switch.on = g.app_cfg.dlp_delivery;' "the row of Settings shows the switch"
need ConfigApp/source/pages_config.cpp 'g.app_cfg.dlp_delivery = on;' "the row of Settings flips the switch"
need ConfigApp/source/pages_config.cpp 'g.SaveAppConfig();' "the switch is saved when it is flipped"
need ConfigApp/source/pages_config.cpp 'dlp_switch.label = dlp::kSwitchLabel;' "the row is called as the text says"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kSwitchLabel = "Download Play delivery";' "the row of the switch is called Download Play delivery (as the notes and the documents call it)"
need ConfigApp/source/dlp_text.hpp '"OFF: the whole feature is shut. Its entry is faint and nothing of it runs or is read or written.' "the note of the switch says what off means"
need ConfigApp/source/dlp_text.hpp 'return boot9_found ? Gate::Open : Gate::NoBoot9;' "the entry needs boot9.bin as Titles Delivery's Send Titles does"
need_after ConfigApp/source/dlp_text.hpp 'inline Gate GateOf(bool switch_on, bool boot9_found) {' 'if (!switch_on) {' "the switch comes first: switched off, nothing else about the entry is looked at"
need ConfigApp/source/dlp_text.hpp 'return g == Gate::Off ? "switched off" : (g == Gate::NoBoot9 ? "needs boot9.bin" : "");' "the shut entry says why at the right of its row, as Send Titles says \"needs boot9.bin\""
need ConfigApp/source/dlp_text.hpp 'return g == Gate::Off ? "Switched off: Settings > Download Play delivery." : "boot9.bin is missing: press Y to see how to get it.";' "A on the shut entry answers with a line that says why and where"
need ConfigApp/source/dlp_text.hpp $'return "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link itself works without it.\\n\\nPress Y to see, step by step, how to get it.";' "the note says that sharing needs boot9.bin, that 3DS Link works without it, and the key of the guide (two lines, a blank one and the Y line: all it has room for on the main page, where the emulator's wider font shows four lines of the note)"
need ConfigApp/source/dlp_text.hpp 'The first time it builds the package, and says how long.";' "the note of the open entry is honest about the first time and still short enough to be shown whole (the main page's note has four lines: two, a blank one and one)"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kGuideHint = "Y guide";' "the hint of the guide's key"
need ConfigApp/source/dlp_text.hpp 'text = "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link works without it: it is only needed to share. "' "the page without boot9.bin says what the file is for and that 3DS Link works without it"
need ConfigApp/source/dlp_text.hpp 'text = "It is switched off in Settings (Download Play delivery). Switch it on there to use it.";' "the page with the switch off says where to switch it on"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kPrepareKeepAwake = "B stops it. Nothing is left half done. HOME and sleep are off until it ends.";' "the work's screen says how to stop it and that nothing is left half done"
need ConfigApp/source/dlp_text.hpp 'std::string s = "One time only: 3DS Link builds the package that other consoles download.";' "the work's screen says that it is one time only and what it builds (and, next, how long)"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kCancelPrepareQuestion = "Nothing is left half done, and the work starts again the next time you open this page.";' "the question before a stop says that nothing is left half done"
need ConfigApp/source/pages_dlp.cpp 'DrawPopupBox(box, "Stop now?", dlp::kCancelPrepareQuestion);' "the popup before a stop asks \"Stop now?\" in its title, and the text under it does not ask it again"
! grep -q -F -- 'kCancelPrepareQuestion = "Stop now' ConfigApp/source/dlp_text.hpp || fail "ConfigApp/source/dlp_text.hpp: the text of the stop popup starts with 'Stop now' again (the popup's title is that question: the card would ask it twice)"
need ConfigApp/source/dlp_text.hpp 'inline std::string PrepareIntro(int seconds, bool measured) {' "the time on the work's screen knows whether it is a guess or the measured pace"
need ConfigApp/source/dlp_text.hpp '" It should take about " + time + ", and it can take longer."' "until the pace is measured the time is a first guess, and says that it can take longer (the speeds were never measured on a console)"
need ConfigApp/source/pages_dlp.cpp 'dlp::PrepareIntro(view.plan_seconds, view.measured)' "the screen tells the guess or the measured time, as the work says"
need ConfigApp/source/dlp_prepare.cpp 'RestartPlan(true, job_.app_bytes > 0 ? job_.app_bytes : kUsualAppBytes);' "the copy from the installed title, after the composer refused the file, begins the estimate again (the bar and the clock): the time left must not count the first attempt"
need ConfigApp/source/dlp_prepare.cpp 'pace_ms_ = ui::NowMs();' "the preparer keeps the clock of the estimate itself (it begins with a plan and begins again with the next one)"
need ConfigApp/source/dlp_prepare_logic.hpp 'return PaceKnown(fraction, elapsed_s) ? elapsed_s / fraction : plan.total;' "the whole time is the plan's until a seventh of the work and two seconds have shown the pace"
need ConfigApp/source/dlp_prepare_logic.hpp 'return fraction >= 0.15 && elapsed_s >= 2.0;' "the pace is trusted after a seventh of the work and two seconds (the time left and the time of the whole work say the same)"
need ConfigApp/source/pages_dlp.cpp 'Fail(dlp::Failure::Prepare, dlp::PrepareFailedText(dlp::FailureSentence(phrase, extra), dlp::PrepAdvice(rc)));' "the card never reads the composer's phrase twice (the composer's detail starts with it)"
need ConfigApp/source/pages_dlp.cpp 'if (dlp::FileIsToBlame(app_file_) || refused) {' "when there is no way to the installed copy, a file in the dlp folder that cannot be used is named first (not \"the key is missing\" alone)"
need ConfigApp/source/pages_dlp.cpp 'Fail(dlp::Failure::BadAppFile, dlp::BadAppFileText(g.paths.dlp_app_cia, found, setup::kVersion));' "...with its path, what is wrong with it, its version when known, and the two ways out"
need ConfigApp/source/dlp_prepare_logic.hpp 'return app_file != Verdict::Missing && app_file != Verdict::Good;' "a file that is there and is no good is to blame; one that is not there is the plain case"
need ConfigApp/source/pages_main.cpp 'return dlp::GateOf(g.app_cfg.dlp_delivery, g.boot9.found);' "the main page decides from the switch and from boot9.bin as the app found it"
need ConfigApp/source/pages_main.cpp 'dlp.dim = gate != dlp::Gate::Open;' "a shut entry is faint"
need ConfigApp/source/pages_main.cpp 'if (gate != dlp::Gate::Open) {' "A on a shut entry only says why: the page is not pushed"
need ConfigApp/source/pages_main.cpp 'g.RefreshBoot9(); // (the card is looked at now: a boot9.bin put there since the last look counts)' "the card is looked at when A is pressed"
need ConfigApp/source/pages_main.cpp 'return row != nullptr && row->id == kDlpDelivery && EntryGate() == dlp::Gate::NoBoot9;' "the guide's key is offered on the entry that is shut for the lack of boot9.bin, and only there"
need ConfigApp/source/pages_main.cpp 'if ((in.down & KEY_Y) && GuideKeyOffered()) {' "Y opens the guide on that entry"
need ConfigApp/source/pages_dlp.cpp 'if (!g.app_cfg.dlp_delivery) {' "the page looks at the switch again before it does anything (a second lock)"
need ConfigApp/source/pages_dlp.cpp 'if (!g.boot9.found) {' "the page looks at boot9.bin again before it does anything (a second lock)"
[ "$(grep -rn "NewDlpHostPage()" ConfigApp/source | grep -v "ConfigApp/source/pages_main.cpp\|ConfigApp/source/pages_dlp.cpp\|ConfigApp/source/delivery.hpp" | wc -l)" = "0" ] || fail "the page of 3DS Link Delivery is opened from a place other than the main page's entry (which is shut by the switch and by boot9.bin)"
[ "$(grep -rn -E "dlpc_(compose|stage_work)\(" ConfigApp/source | grep -v "^ConfigApp/source/dlp_prepare" | wc -l)" = "0" ] || fail "the composer is called from a file other than the work of the package (dlp_prepare.cpp, dlp_prepare_logic.hpp)"
# The guide "How to get boot9.bin" (ConfigApp/guide, shared by the app and the installer child): its keys (L/R and the D-pad's up and down move, B closes: the steps are a vertical list), the seven pictures, the folders it
# names, which are the ones the app looks in (the guide must not lie), and the three places it shows: the installer when it ends without boot9.bin on the card, the app where it explains
# boot9.bin, and the 3DS Link Delivery page while the room is up (a free key, the room goes on).
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyB = 1u << 1;' "the guide's B is libctru's KEY_B"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyDUp = 1u << 6;' "the D-pad's up is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyDDown = 1u << 7;' "the D-pad's down is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyR = 1u << 8;' "R is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyL = 1u << 9;' "L is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyCPadUp = 1u << 30;' "the Circle Pad's up is libctru's (KEY_UP includes it: every list of the app answers to it)"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyCPadDown = 1u << 31;' "the Circle Pad's down is libctru's"
need ConfigApp/guide/guide_logic.hpp 'const bool back = (keys_down & (kKeyL | kKeyDUp | kKeyCPadUp)) != 0;' "L, the D-pad's up and the Circle Pad's up go back (the steps are a vertical list)"
need ConfigApp/guide/guide_logic.hpp 'const bool forward = (keys_down & (kKeyR | kKeyDDown | kKeyCPadDown)) != 0;' "R, the D-pad's down and the Circle Pad's down go forward"
! grep -q -E 'kKeyDLeft|kKeyDRight' ConfigApp/guide/guide_logic.hpp ConfigApp/guide/guide.cpp || fail "the guide names the D-pad's left or right again (the steps are a vertical list: up and down move them, left and right do nothing)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBackKeyText = "Up/L";' "the footer's Back button names the keys that go back"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kNextKeyText = "Down/R";' "the footer's Next button names the keys that go forward"
need ConfigApp/guide/guide.cpp 'DrawKeyButton(kBackButton, kBackKeyText, kBackLabel, step_ > 0);' "the footer draws the words of guide_logic.hpp (the test counts them against the button)"
need ConfigApp/guide/guide.cpp 'DrawKeyButton(kNextButton, kNextKeyText, kNextLabel, step_ < total - 1);' "the footer draws the words of guide_logic.hpp (the test counts them against the button)"
need ConfigApp/guide/guide_logic.hpp 'if (back == forward) {' "both directions at once do nothing"
need ConfigApp/guide/guide_logic.hpp 'constexpr int kImageCount = 7;' "the guide has seven pictures (step8 is text only)"
need ConfigApp/guide/guide_logic.hpp '"The dump is in gm9/out on the SD card."' "the last step names the folder GodMode9 dumps to"
need ConfigApp/guide/guide_logic.hpp '"3DS Link finds it there by itself."' "the last step says that 3DS Link finds it (true: paths.cpp looks in gm9/out)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBoot9AppDir = "sdmc:/3ds/3DSLink";' "the guide's folder of an app installed as a CIA is the app's default folder (paths.hpp kDefaultAppDir)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBoot9GodModeDir = "sdmc:/gm9/out";' "the guide's other folder is GodMode9's (paths.hpp kGodModeOutDir, hostcfg.c HC_GODMODE_OUT)"
need ConfigApp/guide/guide_logic.hpp 'return size == 0x10000 || size == 0x8000;' "a boot9 dump is 64 KB or 32 KB, as paths.cpp says"
need ConfigApp/guide/guide_logic.hpp 'return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "a boot9 name has boot9 in it and not boot11, as paths.cpp says"
need ConfigApp/source/paths.cpp 'return size == 0x10000 || size == 0x8000;' "the app's own size rule (the guide has the same line)"
need ConfigApp/source/paths.cpp 'return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "the app's own name rule (the guide has the same line)"
need ConfigApp/source/paths.cpp 'for (const std::string& place : {paths.boot9_beside, paths.boot9_in_data, paths.boot9_godmode, saved_path}) {' "the app looks for boot9.bin next to itself, in its own folder, in GodMode9's folder and where the user pointed: what the guide's last step says"
need ConfigApp/source/paths.cpp 'for (const std::string& dir : {paths.appdir, paths.datadir, std::string(kGodModeOutDir)}) {' "a dump with GodMode9 2.x's name (<serial>_boot9_00.bin) is found in the same folders"
need ConfigApp/source/module_config.cpp 'hc_set_boot9_path(boot9.found ? boot9.path.c_str() : "");' "the delivery module is given the boot9.bin the app found (hc_boot9_path), by the same search the guide describes"
need ConfigApp/titleshare_host/hostcfg.c '#define HC_GODMODE_OUT "sdmc:/gm9/out"' "the module looks for movable.sed where GodMode9 dumps it, in the same folder as boot9.bin"
need ConfigApp/guide/guide.cpp $'static_assert(kKeyB == KEY_B && kKeyDUp == KEY_DUP && kKeyDDown == KEY_DDOWN && kKeyR == KEY_R && kKeyL == KEY_L && kKeyCPadUp == KEY_CPAD_UP && kKeyCPadDown == KEY_CPAD_DOWN, "guide_logic.hpp\'s key bits are libctru\'s");' "the bits of the guide's keys are checked against libctru's at build time"
for n in 1 2 3 4 5 6 7; do
  [ -f ConfigApp/guide/gfx/step$n.png ] && [ -f ConfigApp/guide/gfx/step$n.t3s ] || fail "the guide has no picture $n (ConfigApp/guide/gfx/step$n.png and .t3s: DlpLink/tools/make_guide.py makes them)"
  grep -q -x "step$n.png" ConfigApp/guide/gfx/step$n.t3s || fail "ConfigApp/guide/gfx/step$n.t3s does not name step$n.png"
done
[ "$(ls ConfigApp/guide/gfx/*.t3s | wc -l)" = "7" ] || fail "the guide has a number of pictures other than seven (guide_logic.hpp kImageCount)"
need ConfigApp/source/pages_startup.cpp '} else if ((in.down & KEY_Y) || Tapped(in, GuideRect())) {' "Y on the first start's question opens the guide"
need ConfigApp/source/pages_startup.cpp 'DrawButton(GuideRect(), "Y", "How to get boot9.bin", true, PressedNow(g.input.in(), GuideRect()), true);' "the first start's question shows the key of the guide"
need ConfigApp/source/pages_check.cpp 'if ((in.down & KEY_Y) && Boot9RowToFix() && !pages::OpenBoot9Guide()) {' "Y on the boot9.bin row of the installation check opens the guide"
need ConfigApp/source/pages_check.cpp 'return Boot9RowToFix() ? dlp::kGuideHint : "";' "the installation check's footer says Y on the boot9.bin row that is not fine"
need ConfigApp/source/pages_check.cpp '} else if (!pages::OpenBoot9Guide()) {' "the installation check's choice \"how to get it\" opens the guide"
need ConfigApp/source/pages_delivery.cpp 'if (GuideKeyOffered() && !pages::OpenBoot9Guide()) {' "Y on Titles Delivery's Send Titles without boot9.bin opens the guide (the footer's Y button: OnExtra, which a press of Y and a tap on the button both reach)"
need ConfigApp/source/pages_delivery.cpp 'out[0] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y};' "the footer of Titles Delivery's page shows Y as a button (the page has no START button, so a hint line would not be drawn: the layout gives it room only beside START)"
! tr -d '\r' < ConfigApp/source/pages_delivery.cpp | sed -n '/^class DeliveryPage/,/^};/p' | grep -q 'Hint() const override' || fail "ConfigApp/source/pages_delivery.cpp: Titles Delivery's page has a footer hint again (it has no START button: the hint is never drawn there)"
need ConfigApp/source/pages_delivery.cpp 'return row != nullptr && row->id == kSend && ready_ && !g.boot9.found;' "the guide's key is offered on Send Titles while boot9.bin is missing"
need ConfigApp/source/status.cpp 'A: search the SD card, choose a folder, or see how to get it. Y: the guide, step by step.' "the note of the boot9.bin row says A and Y"
need ConfigApp/source/pages.hpp 'bool OpenBoot9Guide();' "the guide opens from any page through one function"
need ConfigApp/source/pages_dlp.cpp 'if ((in.down & KEY_Y) || Tapped(in, r.extra[1])) {' "Y (or a tap on its button) opens the guide while the room is up"
need ConfigApp/source/pages_dlp.cpp 'f.extra[1] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y}; // (the guide "How to get boot9.bin": free while the room is up)' "the footer of the room's page shows the guide's key"
need ConfigApp/source/pages_dlp.cpp 'guide::Viewer viewer_;' "the guide is a viewer INSIDE the page of the room (the room and its thread go on): not a page pushed over it"
need_after ConfigApp/source/pages_dlp.cpp '        Refresh();' 'if (guide_open_) {' "the room's snapshot is read before the guide takes the keys: the room goes on while the guide is up"
need ConfigApp/source/pages_dlp.cpp 'if (dl_screens_off() != 0) {' "the guide needs the screens: they are lit when it opens"
# The top screen of 3DS Link Delivery shows the radio channel of the room ("Wi-Fi CH 6") like Titles Delivery does (1.0 did not: the user's console test, 2026-10-22). The channel is the firmware's
# choice for each room, so the thread that makes the room asks the wireless service for it (when the room is new, and once a second while it is not known) and the page only reads the number: a
# call made by the screen would wait while a room is being made.
need ConfigApp/titleshare_host/delivery.h 'int dl_wifi_channel(void);' "the delivery module can say the channel of the room"
need ConfigApp/titleshare_host/delivery.c 'if(!gUdsOk || R_FAILED(udsGetChannel(&channel))) {' "no network, no channel (0: the top screen draws nothing)"
need ConfigApp/source/pages_dlp.cpp 'std::atomic<int> channel{0};' "the channel is a number the thread writes and the page reads"
need ConfigApp/source/pages_dlp.cpp 'if (!s.remaking && (s.remade != seen_remade || (radio->channel.load() == 0 && ++retry >= 100))) {' "the channel is read when the room is new and once a second while unknown, never in the middle of a re-make"
need ConfigApp/source/pages_dlp.cpp 'DrawTitleBarStatus(gfx::kTopW, "3DS Link Delivery", failed_ || radio_ == nullptr ? 0 : radio_->channel.load());' "the top screen of the room shows its channel"
[ "$(grep -c 'dl_wifi_channel()' ConfigApp/source/pages_dlp.cpp)" = "1" ] || fail "ConfigApp/source/pages_dlp.cpp asks the wireless service for the channel from more or fewer places than the radio thread"
! grep -q -F -- 'OpenBoot9Guide' ConfigApp/source/pages_dlp.cpp || fail "ConfigApp/source/pages_dlp.cpp has 'OpenBoot9Guide' (the page of the room does not push the guide as a page (it would stop the page that keeps the room alive))"
[ "$(grep -c "NewDlpHostPage" ConfigApp/source/pages_main.cpp)" = "1" ] || fail "ConfigApp/source/pages_main.cpp opens 3DS Link Delivery from more or fewer places than the one entry"
# About 3DS Link and the credits (ConfigApp/source/about_text.hpp, pages_config.cpp, pages_credits.cpp, and AzaharLink/CREDITS.md, the file the GitHub repository carries). The NAME of the app is
# "3DS Link" and "Connect, share, play together" is its claim (the first line of the note of the first row); the row "This app: unknown" is gone (the page says how the app was installed); the page says
# "Created by Gionko"; the credits are ONE list that the page and CREDITS.md both say (Tests/settings_test/about_test.cpp compares them, word for word, and looks each claim up in the tree).
need ConfigApp/source/about_text.hpp 'constexpr const char* kAppName = "3DS Link";' "the name of the app is 3DS Link"
need ConfigApp/source/about_text.hpp 'constexpr const char* kClaim = "Connect, share, play together";' "Connect, share, play together is the claim, not the name"
need ConfigApp/source/about_text.hpp 'constexpr const char* kCreatedBy = "Created by Gionko";' "About says who made the app"
need ConfigApp/source/pages_config.cpp 'version.label = about::kAppName;' "the first row of About is the name of the app"
need ConfigApp/source/pages_config.cpp 'version.note = about::AppNote(setup::kVersion);' "the claim and the one version are the note of the first row"
need ConfigApp/source/pages_config.cpp 'author.label = about::kCreatedBy;' "About says Created by Gionko"
need ConfigApp/source/pages_config.cpp 'app.label = "Installed as";' "About says how the app was installed, not where it was started from"
need ConfigApp/source/pages_config.cpp 'origin_ = about::OriginOf(g.app_path, envIsHomebrew(), program_known, program_id, dlp::kAppTid);' "the installed app is known by its title id, not only by the lack of a path"
need ConfigApp/source/pages_config.cpp 'about::DescribeOrigin(origin_, g.app_path, g.paths.appdir, g.paths.datadir, dlp::kAppTid);' "the row Installed as says what is true for the way the app was started"
need ConfigApp/source/pages_config.cpp 'about::DescribePluginRow(g.plugin_card, setup::EmbeddedPluginUsable(),' "the plugin row of About is the card against the plugin inside the app"
need ConfigApp/source/pages_config.cpp 'credits.label = about::kCreditsLabel;' "About leads to the credits"
need ConfigApp/source/pages_config.cpp 'PushPage(pages::NewCreditsPage());' "A on the credits row opens the credits"
! grep -q -F '"unknown"' ConfigApp/source/pages_config.cpp || fail "ConfigApp/source/pages_config.cpp says \"unknown\" again (About says what is true: the installed app, the file, or no path given)"
! grep -q -F '"This app"' ConfigApp/source/pages_config.cpp || fail "ConfigApp/source/pages_config.cpp has the row \"This app\" again (it is \"Installed as\")"
need ConfigApp/source/pages_credits.cpp 'row.label = credit.name;' "a row of the credits is a credit of the list"
need ConfigApp/source/pages_credits.cpp 'row.note = about::CreditNote(credit);' "the note of a credit says what was taken, from whom, under which license"
need ConfigApp/source/pages.hpp 'ui::Page* NewCreditsPage();' "the credits open from About through one function"
[ -f CREDITS.md ] || fail "AzaharLink/CREDITS.md is not there (the credits: the file the GitHub repository carries; it says what about_text.hpp says)"
for n in "Azahar Emulator" FBI 3dbrew devkitPro libctru "citro2d, citro3d" Luma3DS CTRPluginFramework ENet GodMode9 "makerom, ctrtool" bannertool Pillow; do
  tr -d '\r' < CREDITS.md | grep -F -x -q -- "### $n" || fail "CREDITS.md has no entry for $n (the list of about_text.hpp and CREDITS.md are one list)"
done
# The build chain (DlpLink/tools): plugin -> installer -> MODEL (a seed child with stand-in files: no app needed) -> app -> 3dslink.cia -> reference child (makerom: the oracle of the tests, not
# a deliverable) -> the checks of the chain. The model does not depend on the app, so there is no cycle; the guard of the registry looks at what is delivered (the app's CIA, the model, the plugin).
if [ -d ../DlpLink ]; then
  BA=../DlpLink/tools/build_all.sh
  L1=$(grep -n "^make_in DlpLink/child " "$BA" | head -1 | cut -d: -f1); L2=$(grep -n "bash DlpLink/tools/make_child_model.sh" "$BA" | head -1 | cut -d: -f1)
  L3=$(grep -n "^  make_in AzaharLink/ConfigApp " "$BA" | head -1 | cut -d: -f1); L4=$(grep -n "bash DlpLink/tools/build_cias.sh" "$BA" | head -1 | cut -d: -f1)
  { [ -n "$L1" ] && [ -n "$L2" ] && [ -n "$L3" ] && [ -n "$L4" ] && [ "$L1" -lt "$L2" ] && [ "$L2" -lt "$L3" ] && [ "$L3" -lt "$L4" ]; } || fail "DlpLink/tools/build_all.sh does not build in the order installer, model, app, CIAs (the app embeds the model, which is made from the installer without the app)"
  BC=../DlpLink/tools/build_cias.sh
  C1=$(grep -n "make_3dslink_cia.sh" "$BC" | tail -1 | cut -d: -f1); C2=$(grep -n "make_dlp_child.sh" "$BC" | tail -1 | cut -d: -f1); C3=$(grep -n "check_chain.sh" "$BC" | tail -1 | cut -d: -f1); C4=$(grep -n "run_checks.sh" "$BC" | tail -1 | cut -d: -f1)
  { [ -n "$C1" ] && [ -n "$C2" ] && [ -n "$C3" ] && [ -n "$C4" ] && [ "$C1" -lt "$C2" ] && [ "$C2" -lt "$C3" ] && [ "$C3" -lt "$C4" ]; } || fail "DlpLink/tools/build_cias.sh does not run in the order app CIA, reference child, checks of the chain, tests"
  ! grep -q "OUT/3dslink" ../DlpLink/tools/make_child_model.sh || fail "DlpLink/tools/make_child_model.sh uses the app's CIA: the model must not depend on the app (the app embeds the model, the child embeds the app: a cycle)"
  ! grep -q "dist/dlp" ../DlpLink/tools/build_all.sh || ! grep -q "cp .*dist/dlp" ../DlpLink/tools/build_all.sh || fail "DlpLink/tools/build_all.sh copies a child into dist/dlp (the child is not delivered: the app makes it)"
fi
[ ! -e dist/dlp ] || fail "dist/dlp is there: the Download Play child is not delivered (3DS Link is ONE CIA: the app builds the child)"
# The plugin is ONE package with the app (the user's rule, 2026-10-22: "pacchetto unico"): AzaharLink.3gx is a RESOURCE of the app (data/AzaharLink.3gx, linked in as AzaharLink_3gx), the app puts
# it on the SD card by itself, at every start and from the Installation check, comparing the file BYTE for byte with the plugin inside it, the 3DS Link switch cannot be ON without it, and the
# Download Play child is composed from the resource, never from the card. The build copies the plugin it has just built into data/ before the app, and what is delivered carries exactly it.
need_line ConfigApp/Makefile $'%.3gx.o\t%_3gx.h :\t%.3gx' "the plugin (data/AzaharLink.3gx) is linked in by bin2o as AzaharLink_3gx"
need ConfigApp/source/plugin_resource.cpp '#include "AzaharLink_3gx.h"' "the plugin resource is the symbol of data/AzaharLink.3gx"
need ConfigApp/source/plugin_resource.cpp 'return word != 0 && PluginVersionText(word) == kVersion;' "a plugin resource can be used only when it is a .3gx of the version of the app"
[ "$BUILT" != 1 ] || [ -f ConfigApp/data/AzaharLink.3gx ] || fail "ConfigApp/data/AzaharLink.3gx is missing (the plugin the app carries as a resource: DlpLink/tools/build_all.sh copies the plugin it built there)"
[ ! -f AzaharLink.3gx ] || cmp -s AzaharLink.3gx ConfigApp/data/AzaharLink.3gx || fail "ConfigApp/data/AzaharLink.3gx is not the plugin of the last build (AzaharLink.3gx): the app would carry another plugin (DlpLink/tools/build_all.sh copies it before the app)"
if [ -d ../DlpLink ]; then
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py plugin ConfigApp/data/AzaharLink.3gx --version "$VERSION" --copy ConfigApp/data/AzaharLink.3gx --in dist/3DSLink.3dsx dist/3DSLink.cia >/dev/null 2>&1 || fail "dist/3DSLink.3dsx or dist/3DSLink.cia does not carry the plugin that is in ConfigApp/data, or that plugin does not say $VERSION (the app that is delivered must carry exactly the plugin of its own build: DlpLink/tools/build_all.sh --dist; check_chain.py plugin says which and why)"
  BA=../DlpLink/tools/build_all.sh
  LP=$(grep -n 'libctrpf' "$BA" | head -1 | cut -d: -f1); LC=$(grep -n 'cp AzaharLink/AzaharLink.3gx AzaharLink/ConfigApp/data/AzaharLink.3gx' "$BA" | head -1 | cut -d: -f1); LA=$(grep -n "^  make_in AzaharLink/ConfigApp " "$BA" | head -1 | cut -d: -f1)
  { [ -n "$LP" ] && [ -n "$LC" ] && [ -n "$LA" ] && [ "$LP" -lt "$LC" ] && [ "$LC" -lt "$LA" ]; } || fail "DlpLink/tools/build_all.sh does not build the plugin, copy it into ConfigApp/data and only then build the app (the app carries the plugin of THIS build)"
  need ../DlpLink/tools/check_chain.sh '$CHK plugin "$PLUGIN_3GX" --version "$VERSION" --copy "$APP_PLUGIN" --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1' "the app's ELF, .3dsx and CIA carry exactly the plugin of this build (size and SHA-256), the resource is that file, and its header says VERSION"
  need ../DlpLink/tools/check_chain.py 'say(found == a.version,' "the check of the plugin compares the version in its header with VERSION"
  need ../DlpLink/tools/check_chain.py 'say(same, f"{a.copy} is the plugin of this build' "... and the resource the app is linked with with the plugin that was built"
  need ../DlpLink/tools/check_chain.py 'say(at >= 0, f"{os.path.basename(where)} carries the plugin' "... and looks for every byte of it in each file"
  need ../DlpLink/tests/selftest_chain.py 'def plugin_checks(tmp):' "the check of the plugin is tried against another version, a copy that differs by a byte, an app with another plugin or a cut one (a check that cannot say NO checks nothing)"
  need ../DlpLink/tests/selftest_chain.py '        plugin_checks(tmp)' "... and that try runs"
fi
#  - the file on the card is compared with the plugin inside the app, BYTE for byte (a header that looks good says nothing), and a file that is not the same is replaced (one version), never kept
need ConfigApp/source/plugin_install.hpp 'if (p.same_bytes) {' "the file on the card is the plugin of the app when its bytes say so (and only then)"
need ConfigApp/source/plugin_install.hpp 'return p.version_word != wanted_version_word ? PluginCard::OtherVersion : PluginCard::Altered;' "a good header over bytes that are not the app's is Altered, never the same"
need ConfigApp/source/plugin_install.hpp 'if (!embedded_usable || card == PluginCard::Same) {' "a plugin that is the app's is left alone, and a build with no plugin inside puts none"
need ConfigApp/source/plugin_install.hpp 'return card == PluginCard::Missing ? PluginStep::Put : PluginStep::Replace;' "a file that is not the plugin of the app is replaced, never kept: the app and its plugin are one version"
need ConfigApp/source/plugin_install.cpp 'probe.same_bytes = probe.size == size && FileEqualsBytes(path, data, size);' "the compare is the size and every byte"
need ConfigApp/source/plugin_install.cpp 'const std::string part = dest + ".part";' "the plugin is written to a .part file first"
need ConfigApp/source/plugin_install.cpp 'if (!FileEqualsBytes(part, data, size)) {' "... read back against the plugin before it takes the place of the old file"
need ConfigApp/source/plugin_install.cpp 'if (std::rename(part.c_str(), dest.c_str()) != 0) {' "... and put in place by a rename"
need ConfigApp/source/plugin_install.cpp 'std::rename(old.c_str(), dest.c_str()); // (the old file goes back)' "a swap that fails puts the old file back"
need ConfigApp/source/plugin_install.cpp 'result.write = WriteResult::NotVerified; // (written, and the card does not hold it: say so, never "put")' "a file that was written and is not the plugin on the card is a failure, never 'put'"
need ConfigApp/source/plugin_ensure.cpp 'setup::EmbeddedPluginSize(), setup::EmbeddedPluginUsable(),' "the app puts only a plugin that is a .3gx of its own version"
need ConfigApp/source/plugin_ensure.cpp 'plugin_copies_checked = false;' "a plugin file that was just written is another file: the copies in the titles' folders are looked at again with it"
need ConfigApp/source/plugin_ensure.cpp 'app_cfg.plugin_stamp.clear();' "... and the stamp of the old file is forgotten, or the quiet look would be skipped"
need ConfigApp/source/paths.cpp 'return paths.plugin_in_data;' "when no plugin file is anywhere, the one in use is in the app's own folder (sdmc:/3ds/3DSLink for a CIA install), where the app puts it"
#  - when it is done: at every start (the first start after its question was answered Yes, never before: until then the app only reads), and from the Installation check
need ConfigApp/source/ui.cpp '    EnsurePlugin();' "at every start the app puts its own plugin on the card when it is missing or is not the same (OpenDataFolder ends with it)"
need_after ConfigApp/source/pages_startup.cpp 'void Begin() {' 'g.OpenDataFolder();' "the first start opens the data folder (and so puts the plugin on the card) only after the question was answered Yes"
! grep -q 'EnsurePlugin' ConfigApp/source/pages_startup.cpp || fail "pages_startup.cpp puts the plugin on the card by itself: the first start's question comes first, and until it is answered Yes the app only reads (App::OpenDataFolder, which Begin() calls after Yes, does it)"
[ "$(grep -c 'g.EnsurePlugin()' ConfigApp/source/pages_check.cpp)" = "2" ] || fail "ConfigApp/source/pages_check.cpp: the plugin is put back when the Installation check opens and on Fix (two places call EnsurePlugin)"
need_after ConfigApp/source/pages_check.cpp 'case setup::CheckAction::RestorePlugin:' 'pages::RestorePlugin();' "A on the plugin row puts the plugin back from the app (nothing is searched for)"
! grep -rq 'StartPluginSearch' ConfigApp/source || fail "a search of the card for the plugin is back: the plugin is a resource of the app, and Fix puts it back from there"
#  - the 3DS Link switch cannot be turned ON without the plugin (after the install step had its chance): dim, A and the D-pad say why, it goes OFF by itself when it was ON and stays OFF when the file is back
need ConfigApp/source/plugin_install.hpp 'return luma_too_old ? SwitchLock::Luma : (plugin_ready ? SwitchLock::None : SwitchLock::Plugin);' "a Luma3DS that is too old shuts the switch first, then a plugin that is not on the card"
need ConfigApp/source/plugin_install.hpp 'return enabled && tunnel && lock == SwitchLock::None;' "a shut switch shows OFF"
need ConfigApp/source/ui.cpp 'plugin_ready = plugin_card == setup::PluginCard::Same;' "the plugin is ready only when it is, byte for byte, the plugin of the app"
need ConfigApp/source/ui.cpp 'plugin_locked = setup::LockOfSwitch(luma_too_old, plugin_ready) == setup::SwitchLock::Plugin;' "the switch is shut by a plugin that is not on the card"
need ConfigApp/source/ui.cpp '} else if (plugin_locked && cfg.enabled) {' "a switch that was ON goes OFF by itself, once, when the plugin is not on the card (like with a Luma3DS that is too old)"
need ConfigApp/source/pages_main.cpp 'power.on = setup::SwitchIsOn(g.cfg.enabled, g.cfg.tunnel, lock);' "the main page's switch shows what the rule says"
need_after ConfigApp/source/pages_main.cpp 'if (g.plugin_locked) {' 'g.ShowToast(g.PluginLockText(), col::kWarn, 12000);' "A and the D-pad on the shut switch say why in a toast"
need ConfigApp/source/pages_main.cpp 'g.ShowToast(g.plugin_notice, g.plugin_notice_color, 7000);' "what the app did about its plugin at the start is said once, on the main page"
# The review of 1.0.1 (F): the reason a plugin could not be put is a sentence of 100 to 150 letters that runs by on the main page, so a long toast stays up for the run (two seconds and 150 ms a
# letter); the main card says the FILE is not right (it may be there, of another version); the "puts it back by itself" of the note is only for a build that has a plugin to put; the
# stray ".old" file of a power cut at the end of the swap is taken away; the child is about 3 MB (12 s, not 10).
need ConfigApp/source/plugin_install.hpp 'inline std::uint64_t ToastMsToRead(std::size_t letters, std::uint64_t given_ms) {' "a long toast stays up for the run of the marquee"
need ConfigApp/source/plugin_install.hpp 'const std::uint64_t run = 2000 + 150 * static_cast<std::uint64_t>(letters);' "...two seconds and 150 ms a letter (the step of gfx::MarqueeText)"
need ConfigApp/source/plugin_install.hpp 'return letters > 40 && run > given_ms ? run : given_ms;' "...for a text of more than 40 letters; a short one keeps its time"
need ConfigApp/source/ui.cpp 'toast_until = toast_from + setup::ToastMsToRead(text.size(), ms);' "every toast goes through the time it needs"
need ConfigApp/source/pages_main.cpp 'message = "The plugin file is not right";' "the main card says the file is not right (it may be there, of another version)"
need ConfigApp/source/pages_main.cpp '(setup::EmbeddedPluginUsable() ? std::string(" ") + setup::kPluginLockMore : std::string())' "the note promises that the plugin is put back only when the app has one to put"
need ConfigApp/source/plugin_install.cpp 'if (result.after == PluginCard::Same && IsFile(dest + ".old")) {' "a stray .old file of a power cut is taken away once the card holds the plugin"
need ConfigApp/source/dlp_text.hpp '. About 12 s to download, a few more to install."' "the Download Play child is about 3 MB: 12 s"
need ConfigApp/source/about_text.hpp '{"ENet", "MIT", "Copyright (c) 2002-2024 Lee Salzman",' "ENet's MIT notice is carried in its credit"
#  - the Download Play child is composed from the plugin INSIDE the app (a memory read), never from the file on the card
need ConfigApp/source/dlp_prepare.cpp 'job.read_plugin = &Hooks::ReadEmbeddedPlugin;' "the composer reads the plugin from the resource of the app"
need ConfigApp/source/dlp_prepare.cpp 'plugin_bytes_ = setup::EmbeddedPluginSize();' "the size of the plugin in the plan is the resource's"
need ConfigApp/source/pages_dlp.cpp 'if (!setup::EmbeddedPluginUsable()) {' "a build whose plugin resource is no plugin of the app's version has none to give: the page says so and does not go on"
! grep -q 'plugin_path' ConfigApp/source/dlp_prepare.cpp ConfigApp/source/dlp_prepare.hpp ConfigApp/source/pages_dlp.cpp || fail "the package page or the work has a plugin_path again: the composer's plugin is the resource of the app, never a file of the card"
! grep -q -E 'DescribePlugin|ResolvePluginFile' ConfigApp/source/dlp_prepare.cpp ConfigApp/source/pages_dlp.cpp || fail "the package page or the work looks at the plugin file of the card: the package carries the plugin inside the app, whatever is on the card"
need ConfigApp/source/dlp_text.hpp 'built without the plugin inside' "a build with no plugin says that, and the way out is the CIA"
# Tests: the unit tests of the new parts (dlp_prepare_test, guide_test: in the list of the test hygiene above) are built AND run by Tests/run.sh, and the installer's own are run by the
# checks of the package
# The neighbour DlpLink (the installer child, the build chain, its tests): the pins that read it, only where the tree has it (a copy of AzaharLink alone has none)
if [ -d ../DlpLink ]; then
  need_line ../DlpLink/child/Makefile $'GUIDE\t\t:=\t../../AzaharLink/ConfigApp/guide' "the installer builds the same guide module as the app"
  need_line ../DlpLink/child/Makefile $'SOURCES\t\t:=\tsource $(GUIDE)' "the installer compiles guide.cpp"
  need_line ../DlpLink/child/Makefile $'INCLUDES\t:=\tsource $(GUIDE)' "the installer's include path has its own gfx.hpp and the guide"
  need_line ../DlpLink/child/Makefile $'GRAPHICS\t:=\t$(GUIDE)/gfx' "the installer carries the pictures"
  need_line ../DlpLink/child/Makefile $'GFXBUILD\t:=\t$(BUILD)' "the pictures are linked in as data"
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u32 kGuideKey = kPadY;' "the guide's key on the installer's finished screen is Y"
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr int kScreenGuide = 100;' "the guide is a screen of its own for the key lockout"
  need ../DlpLink/child/source/installer_logic.hpp 'if (done && !boot9_found && LeavesWorkingApp(outcome)) {' "the notice is for a finished job that left a working app, without boot9.bin on the card (it never blocks and never shows when boot9.bin is there)"
  need ../DlpLink/child/source/installer_logic.hpp 'return outcome == OUT_INSTALLED || outcome == OUT_UPDATED || outcome == OUT_DOWNGRADED || outcome == OUT_REPAIRED || outcome == OUT_CURRENT;' "the notice is for installed, updated, replaced, restored and kept-the-same, not for \"left as it is\" and \"a newer one stays\""
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u32 kLeavePads = kPadA | kPadB;' "only A and B take the user off a finished screen (1.0.1: every button did, and a user who pressed X for Y was thrown to the HOME menu)"
  need ../DlpLink/child/source/installer_logic.hpp 'if (done && guide_offered && (down & kGuideKey) != 0) {' "the guide's key opens the guide, whatever else is down in the same frame"
  need ../DlpLink/child/source/installer_logic.hpp 'const u32 leave = done ? kLeavePads : kPadB;' "a failure screen that cannot be tried again is left with B only (A is not on its footer)"
  need ../DlpLink/child/source/main.cpp 'L("leave: the user pressed %s (buttons down 0x%08lX, phase %d)"' "the log says which button took the user off the screen (and the line below says when the system closed the app: the two looked the same)"
  need ../DlpLink/child/source/main.cpp 'L("leave: aptMainLoop() returned false (the system asks the app to close)");' "...and when it was the system"
  [ "$(grep -c '"Done. Press A: the console goes back to the HOME menu, where 3DS Link is waiting."' ../DlpLink/child/source/main.cpp)" = "2" ] || fail "../DlpLink/child/source/main.cpp: the finished screen (with and without the notice) says which button leaves (A), in both places"
  ! grep -q -F -- 'Press any button' ../DlpLink/child/source/main.cpp || fail "../DlpLink/child/source/main.cpp has 'Press any button' (only A and B leave a finished screen)"
  need ../DlpLink/child/source/main.cpp 'g_boot9_found = guide::Boot9Found();' "the installer looks for boot9.bin once for each finished screen"
  need ../DlpLink/child/source/main.cpp 'return std::string("boot9.bin was not found in ") + guide::kBoot9AppDir + " or " + guide::kBoot9GodModeDir;' "the installer's heading names the two folders the search looked in (the guide's own constants: Boot9Found looks nowhere else)"
  need ../DlpLink/child/source/main.cpp 'const float head_h = gfx::DrawWrapped(34, card_y + 8.0f, 270, 0.44f, col::kText, Boot9Heading(), 2);' "the heading is longer than a line: it wraps (two lines) and the explanation starts under its real height"
  # The installer installs 3DS Link and nothing else: the job deletes the title the manifest names, and its ticket, before it installs, so a manifest that names another title is refused whole.
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u64 kAppTid = 0x000400000F83D100ull;' "the installer installs 3DS Link's title and nothing else (DlpLink/tools/common.sh: unique id 0x0F83D1)"
  need ../DlpLink/child/source/installer_logic.hpp 'return m.tid == kAppTid;' "a manifest names 3DS Link's title or the package is refused"
  need ../DlpLink/child/source/main.cpp 'if (!inst::NamesApp(man)) {' "the job refuses a manifest that names another title before it reads the CIA or touches the console"
  need ../DlpLink/child/source/main.cpp $'Fail(inst::kErrNotThis, "the manifest names a title that is not 3DS Link\'s");' "...with an error of its own in the log (the screen says the package is damaged)"
  [ "$(grep -o 'kAppTid = 0x[0-9A-Fa-f]*ull' ../DlpLink/child/source/installer_logic.hpp)" = "$(grep -o 'kAppTid = 0x[0-9A-Fa-f]*ull' ConfigApp/source/dlp_prepare_logic.hpp)" ] || fail "the installer and the app do not say the same title for 3DS Link (installer_logic.hpp and dlp_prepare_logic.hpp kAppTid)"
  need ../DlpLink/child/source/main.cpp 'const bool lock_open = lock.Open(g_guide_open ? inst::kScreenGuide : phase, osGetTime());' "the guide is its own screen for the lockout (the B that closes it does not also leave)"
  need ../DlpLink/child/source/main.cpp 'const inst::ResultKey key = inst::ResultKeyFor(down, phase == PH_DONE, GuideOffered(notice, gok));' "the finished screen asks the logic what a press does"
  need ../DlpLink/child/source/main.cpp 'constexpr const char* kCopyPath = "sdmc:/3ds/3DSLink/dlp/3dslink.cia";' "the installer leaves the app's CIA where the app looks for it: <data folder>/dlp/3dslink.cia of an app with no folder of its own"
  need ../DlpLink/child/source/main.cpp 'return inst::SaveCopy("romfs:/3dslink.cia", kDlpDir, size, kCopyTarget, g_buf, sizeof(g_buf), g_hooks);' "the copy is made by SaveCopy: a .part file, a rename, a read back from its final place"
  need ../DlpLink/child/source/main.cpp 'const bool copy_same = SameFileAs(cia, kCopyPath, cia_size);' "the copy is compared once and written only when it is not already the right file"
  need ../DlpLink/child/source/safe_write.hpp 'inline bool SaveCopy(' "the copy goes through safe_write.hpp (a .part file and a rename, the old file put back on a failure)"
  need ../DlpLink/tools/build_all.sh 'cp DlpLink/out/child_model.bin AzaharLink/ConfigApp/data/child_model.bin' "the model that was just made is the resource the app is built with"
  need ../DlpLink/tools/build_all.sh 'rm -f AzaharLink/dist/dlp/child.cia && rmdir AzaharLink/dist/dlp 2>/dev/null || true' "a child left in dist/dlp by an older build is removed: it is not a deliverable"
  need ../DlpLink/tools/build_cias.sh 'step "catena (modello, app, guida, ciclo, registro)"' "the checks of the chain run after the reference child"
  need ../DlpLink/tools/check_chain.sh '$CHK agree "$OUT_MODEL" "$OUT/model_seed/model_of_reference.bin" || rc=1' "the cycle is proven at every build: the model of the reference child (made with the real app in its RomFS) is the model that was made without the app"
  need ../DlpLink/tools/check_chain.sh '$CHK carries "$APP_MODEL" "$APP_DIR"/build/step[1-7].t3x --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1' "the app's ELF, 3dsx and CIA carry the model and the seven pictures of the guide"
  need ../DlpLink/tools/check_chain.sh '$CHK carries "$CHILD_DIR"/build/step[1-7].t3x --in "$ELF_CHILD" "$CHILD_DIR/child.3dsx" "$OUT_MODEL" "$OUT/3dslink-child.cia" || rc=1' "the installer (ELF, 3dsx, the model, the reference child) carries the pictures of the guide"
  need ../DlpLink/tools/check_chain.sh '$CHK model "$OUT_MODEL" --version16 "$VER16" || rc=1' "the model carries the version of 3DS Link (VERSION) in its TMD"
  need ../DlpLink/tools/check_chain.sh 'if cmp -s "$OUT_MODEL" "$APP_MODEL"; then' "the model in the app is the model of this build (the same bytes, or with --no-app the same outside the random fields)"
  need ../DlpLink/tools/version.py '  guard APP.cia MODEL.bin PLUGIN.3gx TSV [--check-only]' "the guard of the registry looks at what is delivered: the app's CIA, the model and the plugin (a Download Play client may have kept the child of that version)"
  need ../DlpLink/tools/version.py 'app, model, plugin, tsv = args' "the guard takes the three files"
  ! grep -q -F -- 'version.py" guard' ../DlpLink/tools/make_dlp_child.sh || fail "../DlpLink/tools/make_dlp_child.sh has 'version.py\" guard' (the reference child is not a deliverable: it has no guard (check_chain.sh has it))"
  need ../DlpLink/tests/run_checks.sh 'bash "$PKG/tests/ship_check.sh" || rc=1' "the model that ships is proven at every build: it composes a child that verify_cia.py accepts, and is the child without its RomFS whatever app is in it"
  need ../DlpLink/tests/run_checks.sh 'bash "$PKG/tests/dlpc_check.sh" || rc=1' "the composer is proven against the reference child at every build"
  need ../DlpLink/tests/ship_check.sh 'same_bytes "$W/m2.bin" "$MODEL" "the model of the child of the OTHER app is the shipped model too: it does not depend on the app"' "the model is the same bytes whatever app is in the child"
  need ../DlpLink/tests/ship_check.sh 'same_bytes "$W/m1.bin" "$MODEL" "the model of the composed child is the shipped model (real variant)"' "the child composed with the shipped model gives the shipped model back"
  need ../DlpLink/tests/run_checks.sh '/tmp/dlplink_child_logic/child_logic_test "${CL_ARGS[@]}" | tail -3 || rc=1' "the installer's decisions (the notice, the keys, the copy) are tested against real CIAs at every build"
  # What the HOME menu shows of the app CIA (the user's console test, 2026-10-22): an unopened package shows the NAME ("3DS Link") and the publisher, not the claim; the top screen has a banner (the
  # small 3D model with the tune: ConfigApp/banner, made by DlpLink/tools/bannertool and put in the ExeFS by makerom) and verify_cia.py refuses a CIA without it.
  need ../DlpLink/tools/common.sh 'APP_CIA_NAME="${APP_CIA_NAME:-3DS Link}"; APP_CIA_DESC="${APP_CIA_DESC:-3DS Link}"; APP_CIA_AUTHOR="${APP_CIA_AUTHOR:-Powered by Gionko}"' "the HOME menu shows the name of an unopened package, never the claim"
  need ../DlpLink/tools/make_3dslink_cia.sh 'smdhtool --create "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR" "$ICON_LINK" "$OUT/3dslink-cia.smdh"' "the CIA has an SMDH of its own, not the .3dsx's (that one says the claim)"
  need ../DlpLink/tools/make_3dslink_cia.sh '-icon "$OUT/3dslink-cia.smdh" -banner "$OUT/banner.bnr"' "makerom takes the SMDH of the CIA and the banner"
  need ../DlpLink/tools/make_3dslink_cia.sh '"$BANNERTOOL" makebanner -i "$BANNER_DIR/banner.png" -a "$BANNER_DIR/jingle.wav" -o "$OUT/banner.bnr"' "the banner is made from the picture and the tune of ConfigApp/banner"
  need ../DlpLink/tools/make_3dslink_cia.sh '--expect-banner --expect-strings "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR"' "the CIA is checked for the banner and for the name, the description and the publisher"
  need ../DlpLink/tools/verify_cia.py 'if a.expect_banner:' "the verifier knows how to look for the banner"
  need ../DlpLink/tools/verify_cia.py 'bn[:4] == b"CBMD"' "the banner of the ExeFS is a CBMD"
  need ../DlpLink/tools/bannertool/build.sh 'OUT="${OUT:-$HERE/../../out}"' "bannertool is fetched at a fixed commit and built by build.sh"
  need ConfigApp/banner/make_banner.py 'CLAIM = "Connect, share, play together"' "the banner says the claim the user chose"
  [ "$(od -An -tu1 -j16 -N8 ConfigApp/banner/banner.png | tr -s ' ')" = " 0 0 1 0 0 0 0 128" ] || fail "ConfigApp/banner/banner.png must be 256x128 (the front face of the banner)"
  [ -s ConfigApp/banner/jingle.wav ] || fail "ConfigApp/banner/jingle.wav is missing (the tune of the banner)"
fi

# The documents say what the user decided (2026-10-21): 3DS Link is ONE CIA and the app makes the Download Play child; the switch; no gap between Titles Delivery and 3DS Link Delivery; the numbers of
# dist/ are the numbers of the files (the loops above). A document may name dist/dlp only to say it is gone.
need README.md $'**3DS Link è UN CIA solo, e il figlio per il Download Play lo fa l\'app' "the README says that 3DS Link is one CIA and the app makes the child"
need README.md 'Nessuno spazio fra *Titles Delivery* e *3DS Link Delivery*' "the README says there is no gap between the two entries"
need README.md 'Un interruttore di emergenza, *Settings > Download Play delivery*, spegne tutta la funzione' "the README names the emergency switch"
need README.md $'il modello del figlio di riferimento (fatto con l\'app vera nel RomFS) è lo stesso modello di quello fatto senza l\'app' "the README says how the cycle of the build is broken"
need dist/LEGGIMI.txt $'3DS Link e\' UN CIA SOLO, e il figlio per il Download Play lo fa l\'app' "the short guide says that 3DS Link is one CIA and the app makes the child"
need dist/LEGGIMI.txt $'dist/dlp/child.cia NON c\'e\' piu\' e non va copiato da nessuna parte' "the short guide says there is no child to copy"
need dist/LEGGIMI.txt $'Fra "Titles Delivery" e "3DS Link Delivery" nella pagina principale non c\'e\' spazio' "the short guide says there is no gap between the two entries"
need PROVE_HARDWARE.md $'**Il CIA unico di 3DS Link, il compositore del figlio, la guida per `boot9.bin` e l\'icona' "the plan of the tests says what the one CIA, the composer and the guide need"
need PROVE_HARDWARE.md '**3DS Link è UN CIA solo: il figlio Download Play non è un file di `dist/`**' "the plan of the tests says the child is not a file of dist/"
for r in O15 O16 O17 O18 O19 O20; do
  grep -q "^| $r |" CHECKLIST_DA_VERIFICARE.md || fail "CHECKLIST_DA_VERIFICARE.md has no row $r (the console rows of the one CIA: the package, the sources, the time, the switch, the guide, the size over the air)"
done
# The 1.0.1 round (2026-10-22, the first clean install of 3DSLink.cia through FBI): the plugin is inside the app and the app puts it on the card, the switch cannot be ON without it, About says
# what the app is and who made it, the credits, and the guide moves with the D-pad's up and down. The console rows of it, and the documents that say what the guide's keys are.
for r in P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12; do
  grep -q "^| $r |" CHECKLIST_DA_VERIFICARE.md || fail "CHECKLIST_DA_VERIFICARE.md has no row $r (the console rows of 1.0.1: the clean install through FBI, the plugin that comes back, the shut switch, Enable Titles, About and the credits, the guide's keys, the package made from the plugin inside the app, the install from Download Play)"
done
need CHECKLIST_DA_VERIFICARE.md 'croce su / giù' "the checklist says that the guide's keys are the D-pad's up and down"
need README.md 'croce su / giù' "the README says that the guide's keys are the D-pad's up and down"
need PROVE_HARDWARE.md 'croce su / giù' "the plan says that the guide's keys are the D-pad's up and down"
need dist/LEGGIMI.txt "croce su/giu'" "the short guide says that the guide's keys are the D-pad's up and down"
need README.md 'è in [`CREDITS.md`](CREDITS.md)' "the README points to the credits (the thanks to Azahar, FBI, 3dbrew and the others, with author, use and license)"
need dist/LEGGIMI.txt 'Created by Gionko' "the short guide says what About says: who made 3DS Link"
# a line that names dist/dlp must say that it is gone (the child is not a deliverable: the app makes it on the console)
! grep -n "dist/dlp" README.md PROVE_HARDWARE.md dist/LEGGIMI.txt CHECKLIST_DA_VERIFICARE.md | grep -v -E "non c'è più|NON c'e' piu'|non c'e' piu'|non ha più|non è un file|non esiste|non va copiato" | grep -q . || fail "a document speaks of dist/dlp without saying that it is gone: the child is not a deliverable any more (the app makes it on the console)"
# The neighbour DlpLink (the installer child, the build chain, its tests): the pins that read it, only where the tree has it (a copy of AzaharLink alone has none)
if [ -d ../DlpLink ]; then
  need ../DlpLink/docs/COMPOSITORE.md '* **Un CIA solo.** Si distribuisce `3DSLink.cia`.' "the design of the composer starts from the decision: one CIA"
  need ../DlpLink/docs/COMPOSITORE.md '### Come si rompe il ciclo nella costruzione' "the design says how the cycle of the build is broken"
  need ../DlpLink/docs/GUIDA-BOOT9.md $'## Dove compare (le tre posizioni decise dall\'utente)' "the design of the guide names the three places"
  need ../DlpLink/docs/GUIDA-BOOT9.md '## La guida non deve mentire (e Tests/policy_check.sh lo fissa)' "the design of the guide says that what it tells is pinned to the code"
  need ../DlpLink/docs/3DSLINK-DELIVERY.md $'**Fra Titles Delivery e 3DS Link Delivery NON c\'è nessuno spazio**' "the hand-over document says there is no gap between the entries (the user changed his mind)"
  need ../DlpLink/test/PROVA-INSTALLATORE.md $'**Non c\'è più un `child.cia` da copiare**' "the test of the installer does not tell to copy a child"
  need ../DlpLink/tests/run_checks.sh 'python3 "$PKG/tests/selftest_chain.py" "$OUT/child_model.bin" || rc=1' "the checks of the chain are tried against another installer, another version and a missing picture at every build (a check that cannot say NO checks nothing)"
  # ONE package: what is delivered is the CIA and the .3dsx, and the plugin inside them (the 1.0.1 round)
  need ../DlpLink/tools/build_all.sh '  cp AzaharLink/ConfigApp/3DSLink.3dsx AzaharLink/dist/3DSLink.3dsx' "the app is delivered as the .3dsx of the build"
  need ../DlpLink/tools/build_all.sh '  cp DlpLink/out/3dslink.cia AzaharLink/dist/3DSLink.cia' "the app is delivered as the CIA of the build"
  ! grep -q 'cp AzaharLink/AzaharLink.3gx AzaharLink/dist/AzaharLink.3gx' ../DlpLink/tools/build_all.sh || fail "DlpLink/tools/build_all.sh copies the plugin into dist/ again (ONE package: the plugin is inside the app, and not a file of dist/)"
  need ../DlpLink/tools/build_all.sh 'old_dest="AzaharLink/dist/old/AzaharLink-$old_major.$old_minor.$old_rev.3gx"' "a plugin left in dist/ by an older build is put in dist/old under its version, not thrown away (a console may have that file)"
  need ../DlpLink/docs/COMPOSITORE.md 'è la **risorsa** che l'"'"'app porta' "the design of the composer says that the plugin is the resource of the app (no file of the card)"
  need ../DlpLink/test/PROVA-INSTALLATORE.md 'croce su / giù' "the test of the installer says that the guide's keys are the D-pad's up and down"
fi

if [ "$FAILED" = 0 ]; then
  echo "policy check: defaults and switches as intended"
fi
exit $FAILED
