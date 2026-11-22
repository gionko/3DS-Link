// Where the worker reads a game's content 2 from (1.1.4, piece APP-WORKER: 6-APP-boot9.md 3.1 and 4.6, its critic 6-APP-boot9-CRITICO.md 4.5, K9.4 of
// 7-CONTRATTO.md): the RomFS of the "child container" that holds the game's Download Play children, by one of two routes.
//   Route F (first, and the only one for a game card): the system's own reading, FSUSER_OpenFileDirectly on ARCHIVE_SAVEDATA_AND_CONTENT, content 2,
//            its RomFS - the archive the plugin reads a running game's content 2 with (Sources/ctr/plugin.cpp); the NCCH layer is the system's.
//   Route K (a game of the SD card, when F is refused): the delivery module's virtual CIA of the title (the scan's context: boot9.bin AND this console's
//            movable.sed, and the scan must be over: DL_CIA_NOT_ATTACHED / DL_CIA_NO_KEYS are "needs movable.sed", kErrNoMovable), its content 2 and
//            the container's NCCH layer by boot9.bin (dlp_guest_route.hpp).
// One source at a time, from the worker's thread (the module's stream is one title at a time). Nothing here logs: the worker writes what Open says.
#pragma once

#include <3ds.h>

#include "dlp_guest_route.hpp"
#include "dlp_guest_run.hpp"

namespace dlpguest {

class ContentSource {
public:
    ContentSource() = default;
    ~ContentSource() {
        Close();
    }
    ContentSource(const ContentSource&) = delete;
    ContentSource& operator=(const ContentSource&) = delete;

    /// 0, or kErrNoSource (route F refused, and no route K: a card, or the title is not on the SD card), kErrNoMovable, kErrNoChild, kErrNotCia,
    /// kErrNcch, kErrContainerCrypto, kErrNoBoot9 (route K without boot9.bin's keys), kErrNoMemory. `keys` may be null (route F only).
    int Open(u64 game, bool card, const Keys* keys, Route* route);
    bool Read(u64 off, u8* buf, u32 len);
    void Close();
    /// What the two routes said, for the log: FS's Result of route F, the module's DL_CIA_* of route K (1 = not tried).
    Result route_f() const {
        return f_rc_;
    }
    int route_k() const {
        return k_rc_;
    }

private:
    Handle file_ = 0;
    bool keyed_ = false;
    KeyedRomfs romfs_;
    u8* scratch_ = nullptr;
    Result f_rc_ = 0;
    int k_rc_ = 1;
};

} // namespace dlpguest
