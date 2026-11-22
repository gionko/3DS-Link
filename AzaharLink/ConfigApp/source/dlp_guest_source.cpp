#include "dlp_guest_source.hpp"

#include <new>

#include "../titleshare_host/delivery.h"

namespace dlpguest {

namespace {

constexpr std::size_t kScratchBytes = 64 * 1024; // the aligned reads of route K's layer (a multiple of 16)
constexpr u32 kChildContentPath = 2;             // the content of a game that carries its children (kChildContentIndex)

bool ReadVirtualCia(void*, u64 off, u8* buf, u32 len) {
    return dl_cia_read(off, buf, len) == 0;
}

} // namespace

int ContentSource::Open(u64 game, bool card, const Keys* keys, Route* route) {
    Close();
    *route = Route::None;
    // Route F: the system reads the content and takes its NCCH layer off.
    u32 archive_path[4] = {static_cast<u32>(game), static_cast<u32>(game >> 32), static_cast<u32>(card ? MEDIATYPE_GAME_CARD : MEDIATYPE_SD), 0};
    u32 file_path[5] = {0 /* NCCH data */, kChildContentPath, 0 /* its RomFS */, 0, 0};
    const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
    const FS_Path file = {PATH_BINARY, sizeof(file_path), file_path};
    f_rc_ = FSUSER_OpenFileDirectly(&file_, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0);
    if (R_SUCCEEDED(f_rc_)) {
        *route = Route::System;
        return 0;
    }
    file_ = 0;
    k_rc_ = 1;
    if (card) {
        return CardRouteFRefused(static_cast<u32>(f_rc_)); // (a card has no installed title to read with the keys; FS's "not found" is no child)
    }
    // Route K: the module's virtual CIA of the installed title, then the container's layer by boot9.bin.
    if (keys == nullptr) {
        return kErrNoBoot9;
    }
    u64 size = 0;
    char why[160];
    k_rc_ = dl_cia_open(game, &size, why, sizeof(why));
    if (k_rc_ != DL_CIA_OK) {
        dl_cia_close();
        switch (k_rc_) {
        case DL_CIA_NOT_ATTACHED:
        case DL_CIA_NO_KEYS: return kErrNoMovable;
        case DL_CIA_NOMEM: return kErrNoMemory;
        default: return kErrNoSource;
        }
    }
    scratch_ = new (std::nothrow) u8[kScratchBytes];
    if (scratch_ == nullptr) {
        dl_cia_close();
        return kErrNoMemory;
    }
    const int rc = OpenKeyedRomfs(&ReadVirtualCia, nullptr, size, *keys, scratch_, kScratchBytes, &romfs_);
    if (rc != 0) {
        dl_cia_close();
        delete[] scratch_;
        scratch_ = nullptr;
        return rc;
    }
    keyed_ = true;
    *route = Route::Keyed;
    return 0;
}

bool ContentSource::Read(u64 off, u8* buf, u32 len) {
    if (file_ != 0) {
        u32 got = 0;
        return R_SUCCEEDED(FSFILE_Read(file_, &got, off, buf, len)) && got == len;
    }
    if (keyed_) {
        return ReadKeyedRomfs(&romfs_, off, buf, len);
    }
    return false;
}

void ContentSource::Close() {
    if (file_ != 0) {
        FSFILE_Close(file_);
        file_ = 0;
    }
    if (keyed_) {
        CloseKeyedRomfs(&romfs_);
        dl_cia_close();
        keyed_ = false;
    }
    delete[] scratch_;
    scratch_ = nullptr;
}

} // namespace dlpguest
