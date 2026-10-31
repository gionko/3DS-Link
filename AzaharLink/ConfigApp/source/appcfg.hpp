// The app's own small config file (<data folder>/3DSLink.cfg), not to be confused with AzaharLink.ini, which the plugin reads. Plain text, one
// "key=value" per line, lines it does not know are ignored:
//   first_start_done=1        written at the end of the first start (the file's presence is what says "not the first start")
//   boot9=<path>              the boot9.bin the user pointed the app to (the Installation check's search or folder choice); one of the places boot9.bin is looked for
//   plugin_stamp=<size>:h<hash> the plugin (its size and the hash of its bytes: setup::PluginContentStamp) the copies in the titles' folders were last brought up to (the quiet look is skipped while it is the same plugin; an older app wrote <size>:<mtime>, which never equals it: one more look)
//   dlp_delivery=0|1          the emergency switch of 3DS Link Delivery (Settings > Download Play delivery): 0 (or off, false, no) shuts the whole feature; 1 (on,
//                             true, yes) and anything the app cannot read, or no line at all, leave it on. It is written every time so that a hand edit on
//                             a PC only flips the digit.
// Plain C++ and POSIX file calls (Tests/settings_test/paths_test.cpp builds this very file on a PC).
#pragma once

#include <string>

namespace setup {

struct AppConfig {
    bool first_start_done = false;
    std::string boot9_path; // "" = none chosen
    std::string plugin_stamp; // "" = the copies were never looked at
    bool dlp_delivery = true; // 3DS Link Delivery (the Download Play host) is on: the default, and what an unknown or damaged value means
};

AppConfig ParseAppConfig(const std::string& text);
std::string SerialiseAppConfig(const AppConfig& cfg);

// False when the file is not there or cannot be read (the config stays as it was).
bool LoadAppConfig(const std::string& path, AppConfig& cfg);
// Writes the whole file (a temporary one first, then put in place). The folder must exist.
bool SaveAppConfig(const std::string& path, const AppConfig& cfg);

} // namespace setup
