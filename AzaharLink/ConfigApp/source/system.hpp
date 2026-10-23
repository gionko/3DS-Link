// Things the app asks the console and the SD card about: WiFi state, profile name, MAC address, battery and SD space, the files
// of the installation, the Plugin Loader, and the plugin file on the SD card.
#pragma once

#include <3ds.h>

#include <string>
#include <vector>

#include "azl/common.hpp"
#include "paths.hpp"
#include "status.hpp"

namespace setup {

struct NetInfo {
    bool known = false; // false when the AC service could not be opened
    bool wifi = false;  // connected to a network (an access point)
    int strength = 0;   // the signal, 0..3 (osGetWifiStrength)
    std::string ip;     // the console's address, when sockets are up
};
NetInfo ReadNetwork(bool ac_ready, bool soc_ready);

// What the main page shows of the console. Every figure can be unknown: the services are asked for, not assumed.
struct ConsoleStatus {
    int battery_percent = -1; // 0..100, from mcu::HWC when a 3dsx may use it
    int battery_steps = -1;   // 0..5, from ptm:u
    bool charging = false;
    bool sd_known = false;
    u64 sd_total = 0;         // bytes
    u64 sd_free = 0;
};
// Opens ptm:u and mcu::HWC (either may be refused); the readings are unknown when it was.
void OpenStatusServices();
void CloseStatusServices();
void ReadConsoleStatus(ConsoleStatus& out, bool with_sd);

// Rosalina's Plugin Loader, through Luma3DS's "plg:ldr" service (libctru has no wrapper). The commands are the ones of
// Luma3DS's plgloader.c: 2 = is it enabled, 3 = enable / disable. Where the service is not there (an emulator, a Luma3DS
// without the Plugin Loader) the state is Unknown, at once: nothing is waited for.
LoaderState ReadPluginLoader();
Result SetPluginLoader(bool enabled);

// The database of the seeds of the eShop titles: a file of the console's own memory (the system save 0001000F of the console's ID0), which
// Titles Delivery reads to send titles that were downloaded with a seed. It is not extracted and boot9.bin is not needed to read it, but it has
// to be readable: this asks for it, like the delivery module does. state: 1 readable, 2 not (no card folder to take the ID0 from, or the system said no).
struct SeedReading {
    int state = 0;
    u64 size = 0;
    Result rc = 0;
};
SeedReading ReadSeedDb();

// The facts the installation check needs, read from the SD card now: the plugin file of 3DS Link (where `paths` says: its size and version; what it is against the plugin inside
// the app, byte for byte, is the caller's to fill: ui.cpp App::RefreshInstall), the plugin folder, whether the card can be written, `boot9`, which the caller has looked for
// (paths.hpp), the seed database and Rosalina's Plugin Loader (`loader`).
InstallFacts ReadInstallFacts(const Paths& paths, const Boot9Lookup& boot9, LoaderState loader);

// The profile name set on the console, made safe for the room (the plugin does the same). cfgu
// must be initialised.
std::string ReadConsoleName();
// The name of a console whose profile name cannot be used (the plugin's fallback too): "3DS XXYY", from the end of its WiFi MAC.
std::string FallbackConsoleName();

// The console's real WiFi MAC address.
azl::Mac ReadConsoleMac();

std::string MacToString(const azl::Mac& mac);

struct FileInfo {
    bool exists = false;
    u64 size = 0;
};
FileInfo StatFile(const char* path);

// One .3gx file found on the SD card.
struct PluginFile {
    std::string path;
    u64 size = 0;
    bool header_ok = false; // starts with the 3GX magic
    u32 version = 0;        // the version word of the header (see setup::PluginVersionText); only meaningful with header_ok
};

// Reads the header of a .3gx (size, magic, version). `exists` of the result tells whether the file was there.
PluginFile DescribePlugin(const std::string& path, bool* exists = nullptr);

} // namespace setup
