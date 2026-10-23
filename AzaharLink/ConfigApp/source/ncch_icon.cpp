#include "ncch_icon.hpp"

#include <dirent.h>
#include <inttypes.h>
#include <strings.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "games.hpp"

// TitleShare's primitives are C and their headers have no C++ guards. They are used as they are.
extern "C" {
#include "../titleshare/common/aes.h"
#include "../titleshare/common/contentpath.h"
#include "../titleshare/common/sdcrypt.h"
}

namespace setup {

namespace {

u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u32 Be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

u16 Be16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}

// Reads `len` bytes at `off` of a file of the SD card and takes the SD layer off (`off` must be a multiple of 16).
bool ReadSd(FILE* f, const u32 rk[44], const u8 ctr[16], u64 off, u8* buf, std::size_t len) {
    if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0 || std::fread(buf, 1, len, f) != len) {
        return false;
    }
    aes_ctr_crypt(rk, ctr, off, buf, len);
    return true;
}

} // namespace

// The steps are TitleShare's ncch_read_name (common/ncchname.c) one for one: the highest .tmd of the title's content folder tells the
// id of the main content (index 0); that .app holds the NCCH, whose ExeFS has the "icon" file. Only the length differs: the whole file.
int ReadSmdhKeyed(const SdKeys& keys, u64 tid, u8* out, std::size_t out_size) {
    if (!keys.sd_round_keys || !keys.ncch_keyx || !keys.ctr_root || !out || out_size < kSmdhSize) {
        return -20;
    }
    const u32 hi = static_cast<u32>(tid >> 32);
    const u32 lo = static_cast<u32>(tid);
    char content_dir[512], content_rel[512];
    std::snprintf(content_dir, sizeof(content_dir), "%s/title/%08" PRIx32 "/%08" PRIx32 "/content", keys.ctr_root, hi, lo);
    std::snprintf(content_rel, sizeof(content_rel), "/title/%08" PRIx32 "/%08" PRIx32 "/content", hi, lo);

    // The .tmd with the highest name.
    DIR* dir = opendir(content_dir);
    if (!dir) {
        return -1;
    }
    char tmd_name[64] = {0};
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::size_t n = std::strlen(entry->d_name);
        if (n > 4 && strcasecmp(entry->d_name + n - 4, ".tmd") == 0 && n < sizeof(tmd_name) && std::strcmp(entry->d_name, tmd_name) > 0) {
            std::strcpy(tmd_name, entry->d_name);
        }
    }
    closedir(dir);
    if (tmd_name[0] == 0) {
        return -2;
    }

    char path[800], rel[800];
    std::snprintf(path, sizeof(path), "%s/%s", content_dir, tmd_name);
    std::snprintf(rel, sizeof(rel), "%s/%s", content_rel, tmd_name);
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return -3;
    }
    std::vector<u8> tmd(0x20000);
    const std::size_t tmd_size = std::fread(tmd.data(), 1, tmd.size(), f);
    std::fclose(f);
    if (tmd_size < 0x200) {
        return -3;
    }
    u8 ctr[16];
    sd_path_ctr(rel, ctr);
    aes_ctr_crypt(keys.sd_round_keys, ctr, 0, tmd.data(), tmd_size);

    // The TMD: after the signature (whose size depends on its type), the header, then the 64 info records and the content chunks.
    const u32 sig_type = Be32(tmd.data());
    const std::size_t sig_size = sig_type == 0x10003 ? 0x200 : sig_type == 0x10004 ? 0x100 : sig_type == 0x10005 ? 0x3C : 0;
    if (sig_size == 0) {
        return -4;
    }
    const std::size_t header = (4 + sig_size + 0x3F) & ~static_cast<std::size_t>(0x3F);
    const int count = Be16(tmd.data() + header + 0x9E);
    const std::size_t chunks = header + 0xC4 + 0x24 * 64;
    if (count < 1 || chunks + 0x30 * static_cast<std::size_t>(count) > tmd_size) {
        return -4;
    }
    u32 content_id = 0;
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        const u8* chunk = tmd.data() + chunks + 0x30 * static_cast<std::size_t>(i);
        if (Be16(chunk + 4) == 0) { // the content with index 0: the NCCH of the game
            content_id = Be32(chunk);
            found = true;
        }
    }
    if (!found) {
        return -5;
    }

    content_dirs dirs;
    content_dirs_scan(&dirs, content_dir);
    int sub = -1;
    u64 file_size = 0;
    if (content_locate(&dirs, content_dir, content_rel, content_id, path, sizeof(path), rel, sizeof(rel), &sub, &file_size) != 0) {
        return -6;
    }
    f = std::fopen(path, "rb");
    if (!f) {
        return -6;
    }
    u8 sd_ctr[16];
    sd_path_ctr(rel, sd_ctr);

    int rc = -7;
    u8 hdr[0x200];
    u8 exh[0x200];
    u32 ncch_rk[44];
    u8 ncch_ctr[16];
    if (!ReadSd(f, keys.sd_round_keys, sd_ctr, 0, hdr, sizeof(hdr)) || std::memcmp(hdr + 0x100, "NCCH", 4) != 0) {
        std::fclose(f);
        return rc;
    }
    const u64 exefs_off = static_cast<u64>(Le32(hdr + 0x1A0)) * 0x200;
    const u64 exefs_size = static_cast<u64>(Le32(hdr + 0x1A4)) * 0x200;
    if (exefs_size < 0x200) {
        std::fclose(f);
        return -8; // no ExeFS (a DLC)
    }
    const bool no_crypto = (hdr[0x18F] & 0x04) != 0;
    if (!no_crypto) {
        u8 key[16];
        ncch_scramble(keys.ncch_keyx, hdr, key); // KeyY = the first 16 bytes of the NCCH signature
        aes_set_key(ncch_rk, key);
        std::memset(key, 0, sizeof(key));
        // The counter: the partition id (big endian), type 2 = ExeFS.
        for (int i = 0; i < 8; ++i) {
            ncch_ctr[i] = hdr[0x108 + 7 - i];
        }
        ncch_ctr[8] = 2;
        std::memset(ncch_ctr + 9, 0, 7);
    }

    if (!ReadSd(f, keys.sd_round_keys, sd_ctr, exefs_off, exh, sizeof(exh))) {
        std::fclose(f);
        return -9;
    }
    if (!no_crypto) {
        aes_ctr_crypt(ncch_rk, ncch_ctr, 0, exh, sizeof(exh));
    }
    u32 icon_off = 0;
    u32 icon_size = 0;
    for (int i = 0; i < 10; ++i) {
        const u8* file_entry = exh + 16 * i;
        if (std::memcmp(file_entry, "icon\0\0\0\0", 8) == 0) {
            icon_off = Le32(file_entry + 8);
            icon_size = Le32(file_entry + 12);
        }
    }
    const u64 in_exefs = 0x200 + static_cast<u64>(icon_off); // where the icon is inside the ExeFS
    if (icon_size < kSmdhSize || in_exefs + kSmdhSize > exefs_size) {
        std::fclose(f);
        return -10;
    }
    if (!ReadSd(f, keys.sd_round_keys, sd_ctr, exefs_off + in_exefs, out, kSmdhSize)) {
        std::fclose(f);
        return -11;
    }
    std::fclose(f);
    if (!no_crypto) {
        aes_ctr_crypt(ncch_rk, ncch_ctr, in_exefs, out, kSmdhSize);
    }
    return std::memcmp(out, "SMDH", 4) == 0 ? 0 : -12;
}

} // namespace setup
