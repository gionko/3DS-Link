// AM for the titles the app prepares for Download Play (1.1.4, piece APP-STORE: 6-APP-boot9.md 4.5 with its critic 4.1 and 5): the ONE file of the app
// that may delete such a title. Every call that changes something begins with the same guard: the title is OURS (a .guest says so: IsOwnedGuestTid),
// or nothing is done. Only the SD card (MEDIATYPE_SD), never the NAND; an installation that was begun is cancelled for THAT title only
// (AM_DeletePendingTitle), never for every title (Titles Delivery's or FBI's would go with it).
// The build 2f (2026-11-19): a prepared child has TWO ids (dlp_guest_store.hpp AmTitleFor). Every function takes the prepared title and the FORM, and
// asks AM about AmTitleFor(prepared, form) - 000400100F8xxxxx in the system form (the build 2h), 000400010F8xxxxx in M3c1's, the key itself in
// the old one. The NAND is never named here: its titles are only READ, by dlp_guest_ids_am.cpp (the guard of the ids). Every call that changes
// something asks that guard first, with the console's lists read again right then (AmMayWrite: never an id of the NAND, of a running process, of
// the card; never an install over a ticket nobody said; never an install in the form M3c1).
#pragma once

#include <3ds.h>

#include <vector>

#include "dlp_guest_ids.hpp"
#include "dlp_guest_store.hpp"

namespace dlpguest {

/// (the build 2h) the console's lists of ids, read only (dlp_guest_ids_am.cpp): a fresh census, or the one given brought up to date (only the
/// sources that changed are put in again). false when a needed list was not read (R1).
bool ReadConsoleIds(IdCensus* out, RefreshReport* report = nullptr);
bool RefreshConsoleIds(IdCensus* census, RefreshReport* report);

/// What AM says of the prepared title `prepared` in `form` on the SD card (a title of the band only), and its version when it is Present. A failure
/// of AM is Unknown, never Absent: a title is never taken for gone because AM did not answer.
Presence PreparedPresence(u64 prepared, u8 form, u16* version);
/// ... in the form of its record (FormOf): the Join's table and the pages.
Presence PreparedPresence(u64 prepared, u16* version);
/// Its product code (AM_GetTitleProductCode): kProductBytes with its NUL; false, and "", when AM cannot read it.
bool PreparedProduct(u64 prepared, u8 form, char* out);
/// AM_DeletePendingTitle(SD, that id), for a title of ours.
Result DeletePendingPrepared(u64 prepared, u8 form);
/// What a removal did, every Result as AM gave it (the line of the log of the children).
struct DeleteReport {
    Result title_rc = 0;   ///< AM_DeleteTitle(SD)
    Result user_rc = 0;    ///< AM_DeleteUserTitle(SD) (DeleteAppTitle): asked only in the form kFormApplication, only when AM_DeleteTitle failed (0 = not asked)
    Result ticket_rc = 0;  ///< AM_DeleteTicket (asked only when the caller says the ticket is ours: R7)
    bool ticket_skipped = false; ///< (the build 2h) the ticket was not asked: it may not be ours (DPAPP-2h-sec: or AM still lists the title, or its list was not read)
    bool guard_refused = false;  ///< (the build 2h) the guard of the ids refused: NOTHING was asked of AM
    Result pending_rc = 0; ///< AM_DeletePendingTitle(SD)
    bool still_listed = false; ///< AM's list of the SD card read again holds the title (an unreadable list: false - the caller asks AM again)
};
/// The title in `form`, for a title of ours: AM_DeleteTitle(SD) first - the one that takes a 00040001 away (Azahar's DeleteUserProgram refuses the
/// categories with bit 0x1: am.cpp:2795-2802, PROVA-PC.md 5) -, AM_DeleteUserTitle(SD) only for the old form when it failed, then the ticket and
/// what AM may have begun of it, then the list read again: 0 only when AM no longer lists the title. (DPAPP-2h-sec, second pass) With `drop_ticket`,
/// 0 also needs its ticket asked and taken away: a list not read, or a ticket AM kept, is a failure (the caller keeps the record, Removing).
Result DeletePrepared(u64 prepared, u8 form, bool drop_ticket, DeleteReport* report);
/// (the review of the build 2h, R1) the ticket of `form`'s id ALONE, for a title of ours that AM does not list (a run cut after its Begin left it):
/// the second guard first, never while AM lists a title or a title begun at the id (its ticket is its own then), AM_DeleteTicket only when the
/// console holds that ticket. 0 when no ticket is left there.
Result DeletePreparedTicket(u64 prepared, u8 form);
/// (the review of the build 2h, R4) a title the Installation check would delete (titles.cpp DeleteUnusable): a child of ours (IsOwnedGuestTid)
/// only behind the live guard of the ids - never one whose id is in the NAND, on the card, or in a process (a closed game still in memory too);
/// any other title: true (the check's own rules, as before)
bool UnusableMayGo(u64 tid);

/// The titles of the SD card as AM lists them (AM_GetTitleCount / AM_GetTitleList); false when AM could not list them (then nothing is known of any).
bool ListSdTitles(std::vector<u64>* ids);
/// What AM says of `prepared` in `form` from such a list (`listed` null: AM could not list them: Unknown).
Presence PresenceIn(const std::vector<u64>* listed, u64 prepared, u8 form, u16* version);
/// How many bytes the prepared title takes on the SD card (AM_GetTitleInfo), 0 when AM does not describe it.
u64 PreparedBytes(u64 prepared, u8 form);
/// The version of the update of `game` installed on the SD card (0004000E), 0 when there is none.
u16 UpdateVersion(u64 game);
/// The SD card's free space (FSUSER_GetArchiveResource); false when the system does not say.
bool FreeSdBytes(u64* free);
/// The game card in the slot is `game` (AM's list of the card, as titles::ReadCardGame reads it); false: another card, an empty slot, or a failure
/// of FS or AM (nothing is taken for the game's answer then).
bool CardSlotHolds(u64 game);

// The install of a child (the worker, dlp_guest.cpp): a title of ours only. NOTHING is deleted before it begins: AM keeps the title that is there until
// the Finish, so a failure on the way leaves it as it was (update_worker.cpp does the same with 3DS Link itself). Only THAT title's pending part is
// cancelled, before (what a cut run left) and when it is given up. The CIA's ticket and TMD name the id of `form` (the composer's am_title).
/// (the build 2h) never in the form kFormDlpChild; the guard of the ids first (`ticket_expected`, `title_expected`: the caller's guard found the
/// ticket, the title of the SD card at the id there, and ours).
Result BeginInstall(u64 prepared, u8 form, bool ticket_expected, bool title_expected, Handle* handle);
Result WriteInstall(Handle handle, u64 off, const u8* buf, u32 len);
Result FinishInstall(Handle handle);
void CancelInstall(Handle handle, u64 prepared, u8 form); ///< `handle` 0: the install was already closed (a Finish that failed); THAT pending title cancelled

/// The start of the app (pages_startup.cpp, before the first scan and before any copy of the plugin is queued): LoadRegistry and SweepAtStart on
/// the console's AM and the folders of /luma/plugins.
StartReport StartGuests();

} // namespace dlpguest
