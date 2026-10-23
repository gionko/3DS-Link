// The names and the rotation of the title delivery log: the same scheme as the plugin's capture (AzaharLink.debug.log, .1.log ... .7.log: 64 MiB a
// file, at most 8 files), with the delivery's own name. Pure functions, no file is touched here: the log layer (ConfigApp/titleshare/source/log.c)
// does what logrot_plan says, and Tests/settings_test/logrot_test.cpp builds this file on a PC and does it on a real folder.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LOGROT_MAX_BYTES // (only Tests/settings_test/log_module_test.cpp defines it, to rotate a few KB instead of 64 MiB)
#define LOGROT_MAX_BYTES (64ull * 1024 * 1024) // a file is closed and the older ones move up when it reaches this size
#endif
#define LOGROT_KEEP 7                          // older files kept: .1.log ... .7.log (8 files with the current one)
#define LOGROT_MAX_OPS (LOGROT_KEEP + 2)

// The path of file `index` (0 = the current one) in `dir` (no trailing slash): "<dir>/3DSLink-TitleDelivery.log", "<dir>/3DSLink-TitleDelivery.1.log"...
// 0 when done, -1 when it does not fit or `index` is not 0..LOGROT_KEEP.
int logrot_name(char* out, size_t size, const char* dir, int index);

// Is a file of `bytes` bytes due to be rotated?
int logrot_due(unsigned long long bytes);

typedef struct {
    int remove; // 1: delete file `from`; 0: rename file `from` to file `to`
    int from;
    int to;
} logrot_op;

// What one rotation does, in this order: the oldest file goes, every other file moves up one place (.6 to .7, ... .1 to .2), and the
// current file becomes .1. Afterwards no file 0 exists: the caller opens a new one. Returns the number of operations (<= LOGROT_MAX_OPS).
int logrot_plan(logrot_op* ops, int max);

#ifdef __cplusplus
}
#endif
