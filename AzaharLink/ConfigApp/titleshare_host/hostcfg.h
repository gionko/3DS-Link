// What the 3DS Link app tells the title delivery module (ConfigApp/titleshare) before it starts: where the app keeps its data, where the key
// file is, whether the log is on. The module names no folder of its own: every path it uses comes from here, and the app computes them
// from the folder it was started from (ConfigApp/source/paths.hpp). Plain C with nothing of the console in it, so Tests/settings_test/
// logrot_test.cpp builds this very file on a PC.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The longest path the module handles (the app's paths are far shorter; a longer one is refused, never cut).
#define HC_PATH_MAX 256

// The setters are called by the app before the module's own code runs (a scan, or its main()). A path that does not fit, or NULL, clears
// the value. Trailing slashes are dropped.
void hc_set_app_dir(const char* dir);      // the folder the app was started from, e.g. sdmc:/3ds
void hc_set_data_dir(const char* dir);     // the app's data folder, e.g. sdmc:/3ds/3DSLink (it exists)
void hc_set_boot9_path(const char* path);  // the boot9.bin the app found ("" = there is none)
void hc_set_log_enabled(int on);           // the one log switch of the app (Debug log): 0 = the module writes no log file at all

const char* hc_app_dir(void);   // "" when not set
const char* hc_data_dir(void);  // "" when not set
const char* hc_boot9_path(void);
int hc_log_enabled(void);

// "<data dir>/<name>" into `out`. 0 when done, -1 when the data folder is not set or the path does not fit.
int hc_data_path(char* out, size_t size, const char* name);

// A path of the SD card as the archive of the file system service wants it: without the device name ("sdmc:/a/b" -> "/a/b").
int hc_sd_relative(char* out, size_t size, const char* path);

// Where a movable.sed copied to the SD card is looked for when the console's own cannot be read: next to the app, then in the folder
// GodMode9 dumps it to. `index` 0 and 1; -1 when there is no such candidate (or the path does not fit).
int hc_movable_candidate(int index, char* out, size_t size);

#ifdef __cplusplus
}
#endif
