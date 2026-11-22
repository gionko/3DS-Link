// What Titles Delivery may install on THIS console (DPAPP-2h-sec). The title ids of an offer come from the sender over the air, and the receiver of
// the module deletes the title, its ticket and a pending install of each title it is given BEFORE it reads a byte of its CIA (amsink.c ts_am_begin,
// receiver.c scrub_failed), on the media of ts_title_dest: the NAND for a system title, a Download Play child (00040001), a system application
// (00040010: the build 2h's guests). The app gives the module only what this says: an application, a demo, an update or a DLC of the SD card - never a title
// of the NAND, never 3DS Link itself, never the guest of a sold game (DPAPP-2j: any console's - each prepares its own); the app adds its own guests (a
// title of the band 3DS Link prepared on this console: dl_set_recv_owned). The SENDER offers only what a receiver takes (dlp_guest_store.hpp NeverSent).
// (DPAPP-2j-rev) The guest of another console is refused by its id only in a guest's own forms (00040001, 00040010): in the application form the id
// alone proves nothing - a homebrew of the band may have it (a quarter of the band's ids are PreparedOf of a sold game) - and the record says.
// Plain C:
// titleshare_host/delivery.c uses it, Tests/settings_test/dlp_guest_store_test.cpp tries it on a PC.
#ifndef AZL_RECV_FILTER_H
#define AZL_RECV_FILTER_H

#include <stdint.h>

#define DL_RECV_OK 0
#define DL_RECV_NOT_SD 1 // not an application, a demo, an update or a DLC of the SD card: a system title, a Download Play child, a guest of a console...
#define DL_RECV_SELF 2   // 3DS Link itself (with or without its low byte)
#define DL_RECV_GUEST 3  // a Download Play guest 3DS Link prepared on this console (the app's own registry says so)
#define DL_RECV_GUEST_OTHER 4 // (DPAPP-2j) the id of the prepared guest of a sold game in a guest's form (M3c1, the system form), whatever the record

// (DPAPP-2j) The two ranges of the unique ids of the games that were sold (ConfigApp/source/games.hpp kSoldIdFirst...: games.cpp holds them equal
// with static_asserts), and the band of the prepared guests (azl/title_rules.hpp kPreparedBand, kPreparedLowMask, kAppTid: dlp_guest_store.hpp
// holds them equal).
#define DL_SOLD_ID_FIRST 0x00300u
#define DL_SOLD_ID_LAST 0x01FFFu
#define DL_SOLD_NEW_ID_FIRST 0xF7000u
#define DL_SOLD_NEW_ID_LAST 0xF71FFu
#define DL_PREPARED_BAND 0x0F800000u
#define DL_PREPARED_BAND_MASK 0xFF800000u
#define DL_PREPARED_LOW_MASK 0x007FFFFFu
#define DL_APP_TID 0x000400000F83D100ull

// An application (00040000) whose unique id is in one of the two ranges (games.cpp IsSoldGameTid).
static inline int dl_is_sold_tid(uint64_t tid) {
    const uint32_t id = (uint32_t) (tid >> 8) & 0xFFFFFFu;
    return (uint32_t) (tid >> 32) == 0x00040000u && ((id >= DL_SOLD_ID_FIRST && id <= DL_SOLD_ID_LAST) || (id >= DL_SOLD_NEW_ID_FIRST && id <= DL_SOLD_NEW_ID_LAST));
}

// (DPAPP-2j, the user: Titles Delivery never sends a Download Play guest) `tid` - with the high word of a guest's own form: 00040001 (M3c1),
// 00040010 (the build 2h), which only 3DS Link makes in the band - is the id azl::title_rules::PreparedOf gives to a SOLD game: a guest 3DS Link
// made, on this console or another, whatever the registry knows. Its low word is in the band, never 3DS Link's; the game is 00040000|c with c = the
// 23 low bits, or them under 0x0F000000 (the catalogue's band 0x0F7xxxxx). (DPAPP-2j-rev) Never the application form 00040000 (the old form, the
// fallback of the build 2i): a homebrew of the SD card may have the very same id (0x0F830000-0x0F9FFFFF, 0x0FF00000-0x0FF1FFFF) - a guest of that
// form is known by its record (the registry: NeverSent, dl_set_recv_owned), never by its id.
static inline int dl_is_prepared_of_sold(uint64_t tid) {
    const uint32_t high = (uint32_t) (tid >> 32);
    const uint32_t low = (uint32_t) tid;
    if(high != 0x00040001u && high != 0x00040010u) {
        return 0;
    }
    if((low & DL_PREPARED_BAND_MASK) != DL_PREPARED_BAND || (low & ~0xFFu) == (uint32_t) DL_APP_TID) {
        return 0;
    }
    const uint64_t key = 0x0004000000000000ull | low;
    const uint32_t c[2] = {low & DL_PREPARED_LOW_MASK, 0x0F000000u | (low & DL_PREPARED_LOW_MASK)};
    for(int i = 0; i < 2; i++) {
        const uint64_t game = 0x0004000000000000ull | c[i];
        const uint64_t prepared = 0x0004000000000000ull | (DL_PREPARED_BAND | (c[i] & DL_PREPARED_LOW_MASK));
        if(c[i] != 0 && dl_is_sold_tid(game) && prepared != game && prepared != DL_APP_TID && prepared == key) {
            return 1;
        }
    }
    return 0;
}

// The reason the title `tid` is never installed here, DL_RECV_OK when it may be. The same rule as the module's ts_title_dest for the SD card, on the
// kinds of titles that are sold: high 00040000 (but its variation 0x02, which FBI puts in the NAND), 00040002 (a demo: the sender's catalogue offers
// it, from the SD card's list - DPAPP-2h-sec, second pass), 0004000E, 0004008C.
static inline int dl_recv_refusal(uint64_t tid) {
    const uint32_t high = (uint32_t) (tid >> 32);
    if(dl_is_prepared_of_sold(tid)) {
        return DL_RECV_GUEST_OTHER; // (DPAPP-2j: a guest is the console's own - every console prepares its own from its game. -rev: its forms only)
    }
    if(high != 0x00040000u && high != 0x00040002u && high != 0x0004000Eu && high != 0x0004008Cu) {
        return DL_RECV_NOT_SD;
    }
    if(high == 0x00040000u && (tid & 0xFFu) == 0x02u) {
        return DL_RECV_NOT_SD;
    }
    if(high == 0x00040000u && (tid & ~(uint64_t) 0xFFu) == 0x000400000F83D100ull) {
        return DL_RECV_SELF;
    }
    return DL_RECV_OK;
}

#endif
