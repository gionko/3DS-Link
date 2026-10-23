#include "hostcfg.h"

#include <stdio.h>
#include <string.h>

// GodMode9 dumps the files it extracts here: a place of someone else's tool, not of ours (and not a choice of this app).
#define HC_GODMODE_OUT "sdmc:/gm9/out"

// Written by the app's main thread, read by the module's threads after it started them: the app sets everything first, and a value that
// changes (the key file the user picks) is only read at the next start of the module.
static char gAppDir[HC_PATH_MAX];
static char gDataDir[HC_PATH_MAX];
static char gBoot9[HC_PATH_MAX];
static volatile int gLogOn;

static void put(char* dst, const char* src) {
    if(src == NULL) {
        dst[0] = 0;
        return;
    }

    size_t n = strlen(src);
    if(n >= HC_PATH_MAX) {
        dst[0] = 0; // a path cut short would name another file
        return;
    }

    memcpy(dst, src, n + 1);
    while(n > 0 && dst[n - 1] == '/') {
        dst[--n] = 0;
    }
}

void hc_set_app_dir(const char* dir) {
    put(gAppDir, dir);
}

void hc_set_data_dir(const char* dir) {
    put(gDataDir, dir);
}

void hc_set_boot9_path(const char* path) {
    put(gBoot9, path);
}

void hc_set_log_enabled(int on) {
    gLogOn = on != 0;
}

const char* hc_app_dir(void) {
    return gAppDir;
}

const char* hc_data_dir(void) {
    return gDataDir;
}

const char* hc_boot9_path(void) {
    return gBoot9;
}

int hc_log_enabled(void) {
    return gLogOn;
}

int hc_data_path(char* out, size_t size, const char* name) {
    if(gDataDir[0] == 0 || name == NULL) {
        return -1;
    }

    int n = snprintf(out, size, "%s/%s", gDataDir, name);
    return (n > 0 && (size_t) n < size) ? 0 : -1;
}

int hc_sd_relative(char* out, size_t size, const char* path) {
    if(path == NULL) {
        return -1;
    }

    if(strncmp(path, "sdmc:", 5) == 0) {
        path += 5;
    }

    int n = snprintf(out, size, "%s%s", path[0] == '/' ? "" : "/", path);
    return (n > 0 && (size_t) n < size) ? 0 : -1;
}

int hc_movable_candidate(int index, char* out, size_t size) {
    int n = -1;
    if(index == 0 && gAppDir[0] != 0) {
        n = snprintf(out, size, "%s/movable.sed", gAppDir);
    } else if(index == 1) {
        n = snprintf(out, size, HC_GODMODE_OUT "/movable.sed");
    }

    return (n > 0 && (size_t) n < size) ? 0 : -1;
}
