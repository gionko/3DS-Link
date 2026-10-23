/* Stand-in for libctru's <3ds.h> so that ConfigApp/source/settings.cpp (the Setup app's reading and writing of
   AzaharLink.ini) builds and runs on a PC: all it takes from it is the integer types. */
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
