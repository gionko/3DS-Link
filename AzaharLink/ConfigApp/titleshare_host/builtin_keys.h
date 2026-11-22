#pragma once
#include <stdint.h>

// The 16-byte values 3DS Link needs and does not read from the console: the constant of the hardware's AES key generator (what turns a KeyX and a KeyY
// into the key: the SD layer and the NCCH layer of a title) and the IV of the Download Play checksum. They are the values Azahar ships as its default keys
// (src/core/hw/default_keys.h, the names generatorConstant and dlpChecksumModIv), and they are carried here the same way: a small "name=hex" text,
// obfuscated with AES-128 and a key of zeros (builtin_keys_blob.h). That is no protection and is not meant as one.
// 1.1.4: and the KeyY of the common key 0 (the one of every retail eShop title's ticket, with boot9's KeyX 0x3D), for the Download Play child the app
// prepares with boot9.bin (ConfigApp/source/dlp_guest_compose.cpp ComposeKeys). The same way, never inline.
#ifdef __cplusplus
extern "C" {
#endif

#define BK_GENERATOR_CONSTANT "generatorConstant"
#define BK_DLP_CHECKSUM_IV    "dlpChecksumModIv"
#define BK_COMMON_KEY_Y0      "commonKeyY0"

// Writes the 16 bytes of `name` to `out` (in the order of the hex digits). 0 = done, -1 = no such name or a blob that is not what it should be (then `out` is zeros).
// Nothing is kept between two calls (the blob is a hundred bytes): any thread can call it.
int bk_get(const char* name, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
