// The SMDH of an installed game read with the user's keys. A 3dsx is often not allowed to ask the system for another title's icon file
// (TitleShare's logs: the system refused all the titles of four consoles, and with the keys every name came from the title's own files
// instead). This reads the same file the way TitleShare's ncch_read_name does (the title's main content on the SD card, the SD layer
// and the NCCH layer of its AES decrypted), but returns the whole icon file, so the icon comes with the name.
//
// It is built from TitleShare's own pieces (aes.h, sdcrypt.h, contentpath.h) and does not change them. The code is plain C++ and
// POSIX files, so Tests/settings_test builds it on a PC against a made-up title.
#pragma once

#include <3ds.h>

#include <cstddef>

namespace setup {

struct SdKeys {
    const u32* sd_round_keys = nullptr; // the SD layer's key, expanded (44 words): TitleShare's ctx.rk
    const u8* ncch_keyx = nullptr;      // KeyX of the NCCH layer (16 bytes): TitleShare's ctx.keyX2C
    const char* ctr_root = nullptr;     // "sdmc:/Nintendo 3DS/<ID0>/<ID1>": TitleShare's ctx.ctrRoot
};

// Reads the first kSmdhSize bytes of the "icon" file of the ExeFS of title `tid` into `out`. 0 when done; a negative number when the
// title has no icon file (DLC, an update without one) or cannot be read.
int ReadSmdhKeyed(const SdKeys& keys, u64 tid, u8* out, std::size_t out_size);

} // namespace setup
