// The files of the Download Play children 3DS Link prepares (1.1.4, piece APP-STORE: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 4.4 and
// 4.7, with its critic 6-APP-boot9-CRITICO.md 4.1-4.6 and the contract 7-CONTRATTO.md K2 and K9). Everything is in ONE folder, dlpguest::GuestsDir()
// (sdmc:/3ds/3DSLink/dlp-guests), next to what the plugin reads there:
//   - <prepared>.guest  the app's record of a prepared title: its presence is the ONE proof that the title is OURS. Nothing of the app deletes a title
//                       of the band (azl::title_rules::IsPreparedId) without it, and the scan, Titles Delivery, Enable Titles and the public lobbies never
//                       see a title that has one (IsOwnedGuestTid).
//   - <prepared>.part   the work file of a composition (removed at every end of the work, and at the start);
//   - <prepared>.bin    the block of a prepared child (azl::dlp_children: names, icon, sizes), read by the plugin;
//   - index.txt         the index the plugin reads (azl::dlp_children::kIndexPath), written again at EVERY start (a cut between the two renames of
//                       WriteTextFile leaves the plugin without one: it never reads the .bak);
//   - children.cache    what the app learnt of a game's content 2: whether it carries a child and which (a game is looked at once per version), or
//                       why the look failed (1.1.4, spec 9 point 5: the failure is remembered with the versions and boot9.bin's stamp, so that it is not
//                       tried again and again).
// (DPAPP-2j, R-B) The records - the .guest, the .part, ids.cache - are in the folder of THIS console's ID0 under it (RecordsDir: "the records of
// THIS console" below), as the system keeps every console's data of a card apart; index.txt, the .bin and children.cache stay in the folder itself.
// Every text file is written with setup::WriteTextFile (a .tmp, then the old one taken aside as .bak): SweepOrphans knows both.
// Plain C++ and POSIX calls (no libctru): Tests/settings_test/dlp_guest_store_test.cpp builds this very file against a folder called "sdmc:". The
// calls to AM of the start are the caller's (StartConsole: dlp_guest_am.cpp on the console, a fake in the test). Nothing here prints or logs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "azl/title_rules.hpp"
#include "../titleshare_host/recv_filter.h"

namespace dlpguest {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// The version of the composer a child was made with: a .guest of an older one is prepared again (Decide). One more whenever what Compose makes
/// changes (kFixCrr after S1, a change to the hidden child...). The build 2e (2026-11-18, the form makerom gives an application of the SD card)
/// changed Compose and kept 1, by the decision of its task (home-figlio-2026-11-18/CAUSA-E-RIMEDIO.md 3.2); the build 2e slot25 kept 1 too.
/// 2 since the build 2f (2026-11-19, the form M3c1 of prova-2e-2026-11-19/prova-pc/PROVA-PC.md). 3 since the build 2h (2026-11-19 evening): the
/// form kFormSystem, ONE id 00040010|low for AM, pm, NS and the loader (prova-2g: the form M3c1 left the game in memory after every exit - pm
/// knows the process by the ACI's id, NS unregisters it by the id it launched: two ids, never freed). The children of composer 1 and 2 are "to
/// update" - made again in kFormSystem when the user presses Update /// kFormSystem (D4: a heavy job, never a quiet one). A record of composer 3 in the form
/// kFormApplication is a fallback (the system did not take the system form, D2): it is NOT made again by itself (no loop at every look).
/// 4 since DPAPP-2j (2026-11-20): the console's real AM REFUSED the system form on the SD card for every child (prova-2i: install write 315 after a
/// Begin 0; Azahar's simulated AM took it), and the fallback put the children in HOME (324). The form made again is M3c1 (kFormDlpChild), the
/// only hidden one the real AM took on the SD card (prova-2g); the closed game pm keeps of it is freed by 3DS Link (zombie_free.cpp). Every
/// child of composer 1-3 - the fallbacks of the 2i in HOME, the system form of the 2h in Azahar, the M3c1 of the 2f/2g - is "to update".
constexpr u8 kComposerVersion = 4;

// ---- the forms of a prepared child (the build 2f, 2026-11-19; the build 2h, 2026-11-19 evening) -----------------------------------------
/// The KEY of a prepared child (`prepared`, `slot`) is always azl::title_rules::PreparedOf of the game, 00040000|low: the name of the app's files
/// (the index's column, the .guest, the .bin, the registry). The system knows the child by the ids of its FORM:
///   - the id of AM (the ticket, the TMD, the NCCH's 0x108 and 0x118): AmTitleFor(slot, form);
///   - the id of the PROCESS (the exheader's jump id 0x3C8, ACI 0x400, AccessDesc 0x800: the codeset Luma makes, loader.c:430, the id pm keeps,
///     launch.c:56, the folder of /luma/plugins Rosalina loads, file_loader.c:14): ProcessIdFor(slot, form).
/// The form made now (DPAPP-2j) is kFormDlpChild, M3c1: the only hidden form the console's real AM took on the SD card (prova-2g; the system form
/// 00040010 was refused by every write, prova-2i). pm keeps its process after the game (two ids: pm knows the ACI's, NS unregisters the one it
/// launched): 3DS Link frees it (zombie_free.cpp, pm:app UnregisterProcess of the key, only for a terminated process of a registered key).
/// kFormApplication is the builds up to 2e and the fallbacks of the 2i (visible in HOME), kFormSystem the build 2h's (taken only by Azahar): both
/// read, migrated and removed, never installed again.
enum : u8 {
    kFormApplication = 0, ///< AM id = process id = the key (00040000): HOME shows it (a black icon with Luma's region free); never installed again
    kFormDlpChild = 1,    ///< AM id 00040001|low, process id the key (M3c1): hidden from HOME (its category); the form every child is made in
    kFormSystem = 2,      ///< AM id = process id = 00040010|low (M3a, the build 2h): the real AM refuses it on the SD card; never installed again
};
constexpr u8 kFormCount = 3;
/// The forms, in the order a title with no record is looked for (the form made first).
constexpr u8 kForms[kFormCount] = {kFormDlpChild, kFormApplication, kFormSystem};
/// The form a child is prepared in.
constexpr u8 kPreparedForm = kFormDlpChild;
/// D2 of the plan of the build 2f: when the system does not take the hidden form (AM refuses it, or it is not on the SD card after the Finish),
/// the child is made again in kFormApplication - it works, and HOME shows it as a black icon: the job ends with kErrShownInHome. DPAPP-2j: OFF -
/// the user never wants a child in HOME without knowing it, and the 2i showed that the fallback hid the real cause (an error of the SD card is
/// taken for a refusal of the form). A refusal ends with kErrCannotHide and AM's own Result in the log.
constexpr bool kFallbackToApplicationForm = false;
/// Whether AM is ever asked to INSTALL a child in `form` (DPAPP-2j: the M3c1 form; the old form only for a fallback, which is off; the system form
/// never - dlp_guest_am.cpp BeginInstall holds the same).
constexpr bool Installable(u8 form) {
    return form == kFormDlpChild || (form == kFormApplication && kFallbackToApplicationForm);
}
/// (DPAPP-2j) Whether the Join and main.cpp's jump launch a child in `form`: M3c1 (the closed game it leaves is freed by zombie_free.cpp, and the
/// Join's gate refuses the jump while one is still in memory: 327) and the old form (one id: pm and NS free it). Never the system form: no console
/// has it (the real AM refuses it), and Azahar falls on it (DPAPP-2h-azahar).
constexpr bool Jumpable(u8 form) {
    return form == kFormDlpChild || form == kFormApplication;
}
/// The id AM knows the prepared child by, in `form` (0 in the system form for a slot that is not a key of the band).
constexpr u64 AmTitleFor(u64 prepared, u8 form) {
    return form == kFormDlpChild ? ((static_cast<u64>(azl::title_rules::kTitleHighDlpChild) << 32) | static_cast<u32>(prepared))
                                 : (form == kFormSystem ? azl::title_rules::PreparedSystemOf(prepared) : prepared);
}
/// The id of the PROCESS of the prepared child in `form`: the exheader's ids, the folder of /luma/plugins (the system form: its system id; the
/// two others: the key).
constexpr u64 ProcessIdFor(u64 prepared, u8 form) {
    return form == kFormSystem ? azl::title_rules::PreparedSystemOf(prepared) : prepared;
}
/// The KEY of any id of a prepared child: 00040000, 00040001 or 00040010 with a low word of the band give 00040000|low; any other id gives 0.
constexpr u64 SlotKeyOf(u64 any) {
    return (static_cast<u32>(any >> 32) == azl::title_rules::kTitleHighApplication || static_cast<u32>(any >> 32) == azl::title_rules::kTitleHighDlpChild ||
            static_cast<u32>(any >> 32) == azl::title_rules::kTitleHighSystemApplication) &&
                   azl::title_rules::IsPreparedId((static_cast<u64>(azl::title_rules::kTitleHighApplication) << 32) | static_cast<u32>(any))
               ? (static_cast<u64>(azl::title_rules::kTitleHighApplication) << 32) | static_cast<u32>(any)
               : 0;
}
/// The bit of a form in a mask of forms.
constexpr u8 FormBit(u8 form) {
    return static_cast<u8>(1u << form);
}
static_assert(AmTitleFor(0x000400000F8F8200ull, kFormSystem) == 0x000400100F8F8200ull && ProcessIdFor(0x000400000F8F8200ull, kFormSystem) == 0x000400100F8F8200ull,
              "Island Tour's prepared child: ONE id, 000400100F8F8200, in the system form (the build 2h)");
static_assert(AmTitleFor(0x000400000F8F8200ull, kFormDlpChild) == 0x000400010F8F8200ull && ProcessIdFor(0x000400000F8F8200ull, kFormDlpChild) == 0x000400000F8F8200ull &&
                  AmTitleFor(0x000400000F8F8200ull, kFormApplication) == 0x000400000F8F8200ull && ProcessIdFor(0x000400000F8F8200ull, kFormApplication) == 0x000400000F8F8200ull,
              "the two forms of before: M3c1 (two ids) and the fallback (the key)");
static_assert(SlotKeyOf(0x000400010F8F8200ull) == 0x000400000F8F8200ull && SlotKeyOf(0x000400100F8F8200ull) == 0x000400000F8F8200ull &&
                  SlotKeyOf(0x000400000F8F8200ull) == 0x000400000F8F8200ull && SlotKeyOf(0x00040001000F8200ull) == 0 &&
                  SlotKeyOf(0x0004001000022100ull) == 0 && SlotKeyOf(0x000400120F8F8200ull) == 0,
              "SlotKeyOf: the three highs of the band only (the game's own child 000F8200 and the system's Download Play are not a prepared title's)");
static_assert(Installable(kFormDlpChild) && !Installable(kFormSystem) && !Installable(kFormApplication) && kPreparedForm == kFormDlpChild &&
                  kForms[0] == kPreparedForm && Jumpable(kFormDlpChild) && Jumpable(kFormApplication) && !Jumpable(kFormSystem),
              "DPAPP-2j: the M3c1 form is made and jumped to; the system form never again, the old form only jumped to (to update)");
/// The children the app makes at most: one row of the index each (azl::dlp_children::kMaxRows). A game switched on past them gets none (Decide:
/// Act::Full), and the page says so.
constexpr std::size_t kMaxGuests = 16;

// ---- the record of a prepared title (<prepared>.guest) ---------------------------------------------------------------------------------
enum class GuestState : u8 {
    Preparing, ///< the work began (written BEFORE any AM call): the title may be half there, the plugin's folder and the block too
    Installed, ///< whole: the title, the plugin in its folder, the block, the row of the index
    Removing,  ///< the removal began; a start (or the next look) finishes it
};
constexpr std::size_t kProductBytes = 17; // a product code ("CTR-P-ATSP") of at most 16 characters and its NUL
struct Guest {
    u64 game = 0;      ///< 00040000...: the game whose content 2 the child came from
    u64 child = 0;     ///< 00040001...: the child the hosts announce
    u64 prepared = 0;  ///< azl::title_rules::PreparedOf(game): the hidden title on the SD card
    char where = 'S';  ///< the game was on the SD card ('S') or the game card ('C') when the child was made
    u16 game_version = 0, update_version = 0; ///< the versions of the game and of its update the child was made from
    u16 child_version = 0;                     ///< the child's TMD version (= the prepared title's version)
    u8 child_hash8[8] = {};                    ///< the first 8 bytes of the SHA-256 of the child's content as its TMD names it
    u32 transfer = 0, required = 0;            ///< what a host announces for that child (FindChildPackage): the block's two sizes
    char product[kProductBytes] = {};          ///< the child's product code (the NCCH's, which the composer never changes): what adoption compares
    u8 composer = 0;                           ///< kComposerVersion of the run that made it
    u8 language = 0;                           ///< the console's language the block was written in
    u8 variant = 0;                            ///< the Variant bits (kHide, always, on the console)
    u8 form = kFormApplication;                ///< the form AM has the title in (key form=app|dlp|sys; a record without it is of the builds before 2f)
    GuestState state = GuestState::Preparing;
    bool prev_installed = false; ///< (Preparing only) an Installed child of the same title was there before this run: AM keeps it until Finish
    u16 prev_version = 0;        ///< ... at this version: a start that finds it there goes back to it instead of deleting it
    /// (Preparing only; the review of the build 2f) a migration: the record's child was Installed in the OTHER form (OtherForm(form)) when this run
    /// began, at prev_other_version - it stays until the new form is there. A start that finds it there whole goes back to it (StartAct::RevertOther):
    /// a migration that was cut (a crash, the power) never leaves the console without the child that worked (keys prev_other, prev_other_version:
    /// an older app skips them)
    bool prev_other = false;
    u16 prev_other_version = 0;
    /// (the build 2h) the form of that old child (key prev_other_form=app|dlp|sys; a record of the builds 2f/2g has none: the other of the two
    /// forms it knew, app <-> dlp)
    u8 prev_other_form = kFormApplication;
    /// (the build 2h) the ticket of the record's id of AM in `form` is ours: AM's list of the tickets did NOT hold it before this app's install
    /// (key ticket=ours|other). A record without the key (`ticket_known` false: the builds before) is judged by TicketMayGo (dlp_guest_ids.hpp, R7)
    bool ticket_known = false;
    bool ticket_ours = false;
    /// (DPAPP-2h-sec) the console the ticket word was written on (ThisConsole: key console=, written with it; 0 = not said - the records before). The
    /// ticket is in THAT console's ticket.db, the record on the card: a card in another console must never take that console's ticket for ours
    u32 console = 0;
};
/// (DPAPP-2h-sec) the stamp of the console the app runs on (dlp_guest_am.cpp StartGuests: a hash of the console's unique id, never 0 when known); 0 =
/// not known - then no record's ticket word holds (TicketWordHere).
void SetThisConsole(u32 stamp);
u32 ThisConsole();
/// The stamp of a console's unique value (FNV-1a of its 8 bytes; never 0, the value 0 itself has none): a hash, the value is never written to the card.
constexpr u32 ConsoleStampOf(u64 unique) {
    if (unique == 0) {
        return 0;
    }
    u32 h = 2166136261u;
    for (int i = 0; i < 8; ++i) {
        h = (h ^ static_cast<u32>((unique >> (8 * i)) & 0xFFu)) * 16777619u;
    }
    return h != 0 ? h : 1u;
}
static_assert(ConsoleStampOf(0) == 0 && ConsoleStampOf(1) != 0 && ConsoleStampOf(1) != ConsoleStampOf(2), "a stamp per console, none for no value");
/// The record's word on its ticket (ticket=) holds HERE: written on this very console (console= is ThisConsole, both known). Otherwise the record says
/// nothing of the ticket of this console's ticket.db - the legacy rule (TicketMayGo): ours only with our title listed now.
inline bool TicketWordHere(const Guest& g) {
    return g.ticket_known && g.console != 0 && g.console == ThisConsole();
}
/// (DPAPP-2h-sec, second pass) the record says its ticket is ours and where that was written is not known: a record of the builds before (no
/// console=) or this console's stamp unknown (CFG and AM did not answer). The word may well be THIS console's: no ticket is dropped on it (it is not
/// TicketWordHere), and the record is never forgotten while the console holds a ticket at its id (it stays, Removing - a ticket of the slot is
/// never left with no record that names it: 326 for ever)
inline bool TicketWordUnplaced(const Guest& g) {
    return g.ticket_known && g.ticket_ours && (g.console == 0 || ThisConsole() == 0);
}
/// The record says, from now, that the ticket of its form's id on this console is ours (the guard of the ids let the run go on: none there, or ours).
inline void SayTicketOurs(Guest& g) {
    g.ticket_known = true;
    g.ticket_ours = true;
    g.console = ThisConsole();
}
/// "#dlp-guest 1" and one key=value line per field (ids as 16 hex digits, numbers in decimal).
std::string GuestText(const Guest& g);
/// The record of `text` (CRLF and unknown keys tolerated: a later version may add some). false for another header, a missing or broken field, or a
/// record that is not sound: a game that is not an application, a prepared title that is not EXACTLY PreparedOf(game) (a homebrew 0x0F8... gives 0),
/// a child that is not 00040001.
bool ParseGuest(const std::string& text, Guest* out);

// ---- (DPAPP-2j, R-B) the records of THIS console --------------------------------------------------------------------------------------
/// The system keeps every console's data of a card apart, by the console's ID0 (sdmc:/Nintendo 3DS/<ID0>: the first 16 bytes of a SHA-256 of its
/// movable.sed): a card moved or copied to another console keeps the first one's data intact and unused, and the other console makes its own
/// (3dbrew SD_Filesystem; the community's guide: one ID0 folder per console the card was in). 3DS Link does the same with its records: they live in
/// dlp-guests/<ID0>/ (the .guest, the .part, ids.cache); the files the plugin reads (index.txt, the .bin) stay where the plugin looks, and the
/// index holds this console's children only. A record of another ID0 is never read, never decided on, never deleted. Luma itself keys nothing to
/// a console (/luma/plugins/<id>: a folder of a title this console does not have does nothing).
/// The ID0 of a card's root as FSUSER_GetSdmcCtrRootPath answers it ("/Nintendo 3DS/<32 hex>/<32 hex>"): the 32 hex digits of the ID0 in lower
/// case; "" for anything else.
std::string Id0OfRoot(const std::string& root);
/// (DPAPP-2j-rev) The ID0 of a card whose folder `dir` ("sdmc:/Nintendo 3DS") holds exactly ONE folder of 32 hex digits, in lower case; "" for none
/// or two (a card that was in two consoles: never a guess). Only for an emulator, whose FS does not answer FSUSER_GetSdmcCtrRootPath (Azahar's
/// log: "unimplemented function 'GetSdmcCtrRootPath'"; its card holds one ID0 folder): dlp_guest_am.cpp ReadThisId0.
std::string Id0OfFolders(const std::vector<std::string>& names);
std::string Id0OfSdCard(const std::string& dir);
/// dlp_guest_am.cpp StartGuests (FSUSER_GetSdmcCtrRootPath: the system's answer, no key read); "" = not known: no record is read or written, so
/// no call to AM about a child is ever made (every write of AM comes after its record).
void SetThisId0(const std::string& id0);
std::string ThisId0();
/// dlp-guests/<ID0>: this console's records; "" when the ID0 is not known.
std::string RecordsDir();
/// The keys the records of OTHER consoles name (the records of the shared folder of the builds before DPAPP-2j that were not moved, and those of
/// every other ID0 folder): names only, read only - their blocks are never swept.
std::vector<u64> OtherConsolesKeys();

std::string GuestPath(u64 prepared); ///< sdmc:/3ds/3DSLink/dlp-guests/<ID0>/<prepared, 16 digits>.guest ("" when the ID0 is not known)
std::string PartPath(u64 prepared);  ///< ... .part (the composer's work file)
/// Every <16 digits>.guest of the folder that parses and whose name is its own prepared title, by prepared title (a .guest that only a .bak is left of
/// is the record: setup::ReadTextFile puts it back).
std::vector<Guest> LoadGuests();
bool SaveGuest(const Guest& g);  ///< the folder made if needed; WriteTextFile
bool DropGuest(u64 prepared);    ///< the record, its .bak and its .tmp; true when it is gone

// ---- which titles are ours (any thread) ------------------------------------------------------------------------------------------------
/// Four times kMaxGuests: the app's own records fill a quarter at most, the rest is room for records it did not make (a card copied from another
/// console). A record no slot is left for is NOT ours to this run: nothing of the app deletes, hides or settles its title (LoadRegistry counts it).
constexpr std::size_t kRegistrySlots = 64;
/// The prepared titles that have a .guest, for the scan's thread, Titles Delivery, Enable Titles and the public lobbies: lock-free slots. `tid` is
/// any id of the child (SlotKeyOf): the key, its id of AM in the form kFormDlpChild, its system id (the build 2h) - all of them are ours, the form
/// of the record or not (a migration that was cut leaves another).
bool IsOwnedGuestTid(u64 tid);
/// (DPAPP-2h-sec) a title Titles Delivery never sends: a child of ours (IsOwnedGuestTid), and ANY id of the band in a child's form - 00040001 (M3c1) or
/// 00040010 (the system form) - whose record is lost, of another console, or past the registry: only 3DS Link makes those, and a receiver would
/// install one in its NAND. (DPAPP-2j, the user: the sender sends ONLY titles of the SD card - applications, demos, updates, DLC - and NEVER a
/// Download Play guest) also every title a receiver does not take (dl_recv_refusal: not an application, demo, update or DLC of the SD card, 3DS
/// Link, the guest of a sold game in a guest's form - DL_RECV_GUEST_OTHER). (DPAPP-2j-rev, the decision of DPAPP-2h-sec) The band's applications
/// (00040000|0F8...: the homebrew of the SD card) stay sendable unless they are ours - their id alone never says a guest (a quarter of them have
/// the id PreparedOf gives to a sold game).
inline bool NeverSent(u64 tid) {
    return IsOwnedGuestTid(tid) || (SlotKeyOf(tid) != 0 && static_cast<u32>(tid >> 32) != azl::title_rules::kTitleHighApplication) ||
           dl_recv_refusal(tid) != DL_RECV_OK;
}
static_assert(DL_PREPARED_BAND == azl::title_rules::kPreparedBand && DL_PREPARED_BAND_MASK == azl::title_rules::kPreparedBandMask &&
                  DL_PREPARED_LOW_MASK == azl::title_rules::kPreparedLowMask && DL_APP_TID == azl::title_rules::kAppTid,
              "recv_filter.h's band is title_rules.hpp's (DPAPP-2j)");
bool Register(u64 prepared, u8 form); ///< (a title of the band only) false: not one, or every slot is taken; registered already: its form set
void SetForm(u64 prepared, u8 form);  ///< the form of a registered title (nothing for another)
u8 FormOf(u64 prepared);              ///< the form of a registered title; kFormApplication for another
/// The id AM knows a registered title by (AmTitleFor(prepared, FormOf(prepared))): the jump of main.cpp, the rows of the pages. No I/O.
u64 AmTitleOf(u64 prepared);
/// (the build 2h) the id of the PROCESS of a registered title (ProcessIdFor(prepared, FormOf(prepared))): its folder of /luma/plugins. No I/O.
u64 ProcessIdOf(u64 prepared);
void Unregister(u64 prepared);
void ClearRegistry();
/// Main thread, at the start, BEFORE the first scan (pages_startup.cpp): the records of the folder, every one registered; `unowned` counts the ones
/// no slot was left for.
std::vector<Guest> LoadRegistry(int* unowned = nullptr);

// ---- what the app learnt of the games' content 2 (children.cache) ----------------------------------------------------------------------
struct ChildSeen {
    u64 game = 0;
    u16 game_version = 0, update_version = 0; ///< the versions it was looked at (another version is looked at again: Probe)
    int error = 0;                            ///< a look that FAILED (a kErr*): nothing below is known; FindChildSeen never gives it, FindChildFailure does
    u32 keys = 0;                             ///< (error) the stamp of boot9.bin the look was made with (KeysStamp): another one tries again
    bool has_child = false;                   ///< false: content 2 carries no child (the game is asked once per version)
    u64 child = 0;
    u16 child_version = 0;
    u8 child_hash8[8] = {};
    u32 transfer = 0, required = 0;
    char product[kProductBytes] = {};
};
std::string CachePath();
std::string CacheText(const std::vector<ChildSeen>& seen);
bool ParseCache(const std::string& text, std::vector<ChildSeen>* out); ///< false for another header; a broken line is skipped
std::vector<ChildSeen> LoadChildrenCache();
bool SaveChildrenCache(const std::vector<ChildSeen>& seen);
/// What is KNOWN of the game at these versions (a child, or none): never a failure.
const ChildSeen* FindChildSeen(const std::vector<ChildSeen>& seen, u64 game, u16 game_version, u16 update_version);
/// The failure remembered of the game at these versions, if the last look failed (error != 0).
const ChildSeen* FindChildFailure(const std::vector<ChildSeen>& seen, u64 game, u16 game_version, u16 update_version);

// ---- what the plugin reads ---------------------------------------------------------------------------------------------------------
/// A game switched on whose child is not prepared (yet): a row with prepared 0 (the plugin then says "not prepared", not "no row").
struct Waiting {
    u64 game = 0, child = 0;
    u16 child_version = 0;
    char where = 'S';
};
/// The index: azl::dlp_children::kIndexHeader, a row per Installed guest (its prepared title), then a row per Waiting game whose child has no row yet;
/// one row per child, at most kMaxRows: the rest is counted in `dropped` (the page says so).
std::string IndexText(const std::vector<Guest>& guests, const std::vector<Waiting>& waiting, int* dropped);
/// The index written with WriteTextFile: `always` at the start (the plugin never reads the .bak), otherwise only when the text differs.
bool WriteIndex(const std::string& text, bool always);
/// The block of a prepared child (azl::dlp_children::MakeBlock of the SMDH of the meta block of its CIA), kBlockBytes at BlockPath(prepared). false, and
/// nothing written, for an SMDH that is not one.
bool WriteBlock(u64 prepared, const u8* smdh, std::size_t smdh_size, u8 language, u32 transfer, u32 required);

// ---- the leftovers ----------------------------------------------------------------------------------------------------------------
struct Sweep {
    int temps = 0;    ///< *.tmp (a write that was cut before its rename)
    int baks = 0;     ///< X.bak with X whole: the old file of a write that was cut after the new one was in place
    int restored = 0; ///< X.bak with no X: put back as X (a write cut between its two renames; never dropped before the file is written again)
    int parts = 0;    ///< <prepared>.part of no Preparing guest
    int blocks = 0;   ///< <prepared>.bin of no guest that has a child installed (Installed, or Preparing over an Installed one)
};
/// The leftovers of the folder: nothing else is touched (the .guest records, the index, the cache, a file of somebody else's).
Sweep SweepOrphans(const std::vector<Guest>& alive);

// ---- the start ----------------------------------------------------------------------------------------------------------------------
/// What AM says of a prepared id on the SD card. Only Absent says that the title is not there: a failure of AM is Unknown, never "gone" (a record
/// dropped on a failure would leave our title on the card with nothing that knows it is ours).
enum class Presence : u8 {
    Absent,  ///< AM listed the SD card's titles (AM_GetTitleList) and it is not among them
    Present, ///< listed, and described (AM_GetTitleInfo): its version
    Broken,  ///< listed, but AM cannot describe it (an installation cut in its commit: the Installation check's "Broken")
    Unknown, ///< AM could not list the titles (amInit failed, or it refused): nothing is decided about the title
};
/// What the start asks the system (dlp_guest_am.cpp on the console: every call is about OUR title, the one of a .guest).
struct StartConsole {
    virtual ~StartConsole() = default;
    // (the build 2f) every call about AM names the form: the id AM is asked about is AmTitleFor(prepared, form)
    virtual Presence Installed(u64 prepared, u8 form, u16* version) = 0; ///< what AM says of the title, and its version (Present)
    virtual bool Product(u64 prepared, u8 form, char* out) = 0;  ///< (Present, Broken) its product code, kProductBytes with its NUL; false: unread
    virtual void DeletePending(u64 prepared, u8 form) = 0;       ///< AM_DeletePendingTitle(SD, that id): THAT title, never all of them
    /// AM_DeleteTitle(SD), and its ticket only when `drop_ticket` (the build 2h, R7: never a ticket that may not be ours); true when AM no longer
    /// lists it. dlp_guest_am.cpp asks the live guard first (never a title of the NAND, of a running process, of the card)
    virtual bool DeleteTitle(u64 prepared, u8 form, bool drop_ticket) = 0;
    /// (the review of the build 2h, R1) the ticket of `form`'s id ALONE, for a record that says it is ours and whose title AM does not list (a run
    /// cut after its Begin, given up): AM_DeleteTicket behind the live guard, and never while AM lists a title at the id. true when no ticket is left
    /// there (none, or deleted); false: the guard refused, AM lists a title there, or the ticket is still there (the record stays: Removing)
    virtual bool DropTicket(u64 prepared, u8 form) = 0;
    /// (DPAPP-2h-sec, second pass) whether the console holds a ticket at `form`'s id now (its lists read again, read only): Present, Absent, or
    /// Unknown when a list was not read. Nothing is deleted
    virtual Presence TicketAt(u64 prepared, u8 form) = 0;
    /// setup::DisableGuestFolder of `process_id` (the build 2h: ProcessIdFor of a form - the start takes away the folders of both process ids)
    virtual bool DisableFolder(u64 process_id, u64 game) = 0;
};
struct StartReport {
    int guests = 0;   ///< records read
    int kept = 0;     ///< Installed, left as they are
    int reverted = 0; ///< Preparing over an Installed child that is still there at its old version (in its form, or the other form's of a
                      ///< migration that was cut), or the new child of a migration from M3c1 cut after AM's Finish (KeepNew): Installed again
                      ///< (made again by the next look)
    int taken = 0;    ///< taken away whole (a Preparing that died, a Removing): folder, block, pending title, title, record
    int failed = 0;   ///< could not be taken away (or disowned) whole: kept as Removing (the next look finishes it)
    int later = 0;    ///< AM could not say whether the title is there: left as they are (the next start settles them)
    int disowned = 0; ///< the id holds a title that is not ours any more (another product code): record, block, plugin's copy gone; the title untouched
    int unowned = 0;  ///< records no registry slot was left for: left as they are
    int moved = 0;    ///< (DPAPP-2j, R-B) records of the shared folder (the builds before) moved into this console's folder: written by this console
                      ///< (console=), or without the key and AM lists their title with their product code
    int other = 0;    ///< (DPAPP-2j) records of the shared folder left there: another console's, or unproved - intact, never read, never deleted
    Sweep sweep;
    bool index_written = false;
};
/// The start (main thread, before the first scan, before any copy of the plugin is queued): LoadRegistry, every record settled (DecideAtStart),
/// the leftovers swept, the index written again. A folder that is not there is not made.
StartReport SweepAtStart(StartConsole& console);

} // namespace dlpguest
