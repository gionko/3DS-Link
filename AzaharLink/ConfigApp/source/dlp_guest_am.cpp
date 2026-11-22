#include "dlp_guest_am.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "azl/title_rules.hpp"
#include "dlp_guest_logic.hpp"
#include "games.hpp"

namespace dlpguest {

namespace {

constexpr Result kNotOurs = static_cast<Result>(0xD8E0806Au); // (any failure: the guard refused, nothing was asked of AM)
constexpr Result kShortWrite = static_cast<Result>(kResultShortWrite); // (AM took fewer bytes than it was given: dlp_guest_logic.hpp, an error of the SD card)
constexpr Result kStillListed = static_cast<Result>(0xD8E0806Cu); // (AM answered 0, and lists the title all the same)
constexpr Result kIdsRefused = static_cast<Result>(0xD8E0806Du);  // (the build 2h: the guard of the ids refused - nothing was asked of AM)
constexpr Result kTicketLeft = static_cast<Result>(0xD8E0806Eu);  // (DPAPP-2h-sec, second pass: the title gone, the ticket said ours not known gone)

// A prepared title of the band, never 3DS Link (the guard of every reading call).
bool PreparedBand(u64 prepared) {
    return azl::title_rules::IsPreparedId(prepared) && prepared != azl::title_rules::kAppTid;
}

// (the build 2h) the second guard of the ids, right before AM is asked to change something about `prepared` in `form`: the console's lists read
// again here (never a cache) and AmMayWrite - never an id of the NAND, of a running process, of the card, never an install over a ticket the caller
// did not say. A list that was not read refuses too (R1).
bool IdsAllow(u64 prepared, u8 form, bool install, bool ticket_expected, bool title_expected) {
    IdCensus live;
    if (!ReadConsoleIds(&live)) {
        return false;
    }
    return AmMayWrite(live, AmTitleFor(prepared, form), ProcessIdFor(prepared, form), install, ticket_expected, title_expected);
}

} // namespace

bool ListSdTitles(std::vector<u64>* ids) {
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count))) {
        return false;
    }
    ids->assign(count, 0);
    u32 got = 0;
    if (count > 0 && R_FAILED(AM_GetTitleList(&got, MEDIATYPE_SD, count, ids->data()))) {
        return false;
    }
    ids->resize(std::min(got, count));
    return true;
}

Presence PresenceIn(const std::vector<u64>* listed, u64 prepared, u8 form, u16* version) {
    if (listed == nullptr || !PreparedBand(prepared)) {
        return Presence::Unknown; // AM could not list the titles (or the id is no prepared title's): never read as "not there"
    }
    u64 tid = AmTitleFor(prepared, form);
    if (std::find(listed->begin(), listed->end(), tid) == listed->end()) {
        return Presence::Absent;
    }
    AM_TitleEntry entry{};
    if (R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &entry))) {
        return Presence::Broken; // listed, but AM cannot describe it: still there (the start tries to delete it)
    }
    if (version) {
        *version = static_cast<u16>(entry.version);
    }
    return Presence::Present;
}

namespace {

// What the start asks the console: AM about our titles (the SD card's list read once), and the folders of /luma/plugins.
struct ConsoleStart final : StartConsole {
    bool asked = false;
    bool listed = false;
    std::vector<u64> ids;
    Presence Installed(u64 prepared, u8 form, u16* version) override {
        if (!asked) {
            asked = true;
            listed = ListSdTitles(&ids);
        }
        return PresenceIn(listed ? &ids : nullptr, prepared, form, version);
    }
    bool Product(u64 prepared, u8 form, char* out) override {
        return PreparedProduct(prepared, form, out);
    }
    void DeletePending(u64 prepared, u8 form) override {
        DeletePendingPrepared(prepared, form);
    }
    bool DeleteTitle(u64 prepared, u8 form, bool drop_ticket) override {
        DeleteReport report;
        const bool gone = R_SUCCEEDED(DeletePrepared(prepared, form, drop_ticket, &report));
        asked = false; // (the list changed: the next question reads it again)
        return gone;
    }
    bool DropTicket(u64 prepared, u8 form) override {
        const bool gone = R_SUCCEEDED(DeletePreparedTicket(prepared, form));
        asked = false;
        return gone;
    }
    Presence TicketAt(u64 prepared, u8 form) override {
        // (DPAPP-2h-sec, second pass) the console's lists read now, read only: Unknown when one was not read
        IdCensus live;
        if (!PreparedBand(prepared) || !ReadConsoleIds(&live)) {
            return Presence::Unknown;
        }
        const u64 am = AmTitleFor(prepared, form);
        return std::binary_search(live.ids[kSrcTicket].begin(), live.ids[kSrcTicket].end(), am) ? Presence::Present : Presence::Absent;
    }
    bool DisableFolder(u64 process_id, u64 game) override {
        return setup::DisableGuestFolder(process_id, game);
    }
};

} // namespace

Presence PreparedPresence(u64 prepared, u8 form, u16* version) {
    std::vector<u64> ids;
    const bool listed = ListSdTitles(&ids);
    return PresenceIn(listed ? &ids : nullptr, prepared, form, version);
}

Presence PreparedPresence(u64 prepared, u16* version) {
    return PreparedPresence(prepared, FormOf(prepared), version);
}

bool PreparedProduct(u64 prepared, u8 form, char* out) {
    std::memset(out, 0, kProductBytes);
    if (!PreparedBand(prepared)) {
        return false;
    }
    char code[16] = {};
    if (R_FAILED(AM_GetTitleProductCode(MEDIATYPE_SD, AmTitleFor(prepared, form), code))) {
        return false;
    }
    std::memcpy(out, code, sizeof(code)); // (16 bytes at most: out keeps its NUL)
    return out[0] != '\0';
}

Result DeletePendingPrepared(u64 prepared, u8 form) {
    const u64 am = AmTitleFor(prepared, form);
    if (!PreparedBand(prepared) || !IsOwnedGuestTid(am)) {
        return kNotOurs;
    }
    if (!IdsAllow(prepared, form, false, false, false)) {
        return kIdsRefused; // (the build 2h: the id is in the NAND, in a process, on the card, or a list was not read)
    }
    return AM_DeletePendingTitle(MEDIATYPE_SD, am);
}

Result DeletePrepared(u64 prepared, u8 form, bool drop_ticket, DeleteReport* report) {
    DeleteReport r;
    const u64 am = AmTitleFor(prepared, form);
    if (!PreparedBand(prepared) || !IsOwnedGuestTid(am)) {
        r.title_rc = kNotOurs;
        if (report) {
            *report = r;
        }
        return kNotOurs;
    }
    if (!IdsAllow(prepared, form, false, false, false)) {
        // (the build 2h) the second guard of the ids: NOTHING is asked of AM - not the title, not the ticket, not the pending part
        r.title_rc = kIdsRefused;
        r.guard_refused = true;
        if (report) {
            *report = r;
        }
        return kIdsRefused;
    }
    // AM_DeleteTitle first: it takes the hidden form (00040001) away, which DeleteUserTitle refuses; DeleteUserTitle only for the old form, and only
    // when AM_DeleteTitle failed (the way of the builds before 2f, kept as the fallback of that form)
    r.title_rc = AM_DeleteTitle(MEDIATYPE_SD, am);
    if (R_FAILED(r.title_rc) && form == kFormApplication) {
        r.user_rc = AM_DeleteUserTitle(MEDIATYPE_SD, am);
    }
    r.pending_rc = AM_DeletePendingTitle(MEDIATYPE_SD, am);
    std::vector<u64> ids;
    const bool listed_read = ListSdTitles(&ids);
    r.still_listed = listed_read && std::find(ids.begin(), ids.end(), am) != ids.end();
    // (DPAPP-2h-sec) the ticket only once AM no longer lists the title: a delete that failed never leaves our title without its ticket (the ticket of
    // a listed title is its own, as DeletePreparedTicket holds). R7: only a ticket the caller says is ours
    if (drop_ticket && listed_read && !r.still_listed) {
        r.ticket_rc = AM_DeleteTicket(am);
    } else {
        r.ticket_skipped = true;
    }
    if (report) {
        *report = r;
    }
    if (r.still_listed) {
        return R_FAILED(r.title_rc) ? r.title_rc : kStillListed; // (the caller keeps the record, as Removing)
    }
    if (drop_ticket && (r.ticket_skipped || R_FAILED(r.ticket_rc))) {
        // (DPAPP-2h-sec, second pass) the title is gone and its ticket - ours, the caller said - was not asked (AM's list not read) or AM kept it:
        // never 0, the caller keeps the record (Removing) with its word, and the next look drops the ticket (DropLeftTicket, DropTicket) or says it
        return kTicketLeft;
    }
    return R_SUCCEEDED(r.title_rc) || (form == kFormApplication && R_SUCCEEDED(r.user_rc)) ? 0 : r.title_rc;
}

Result DeletePreparedTicket(u64 prepared, u8 form) {
    const u64 am = AmTitleFor(prepared, form);
    if (!PreparedBand(prepared) || !IsOwnedGuestTid(am)) {
        return kNotOurs;
    }
    if (!IdsAllow(prepared, form, false, false, false)) {
        return kIdsRefused; // (the second guard of the ids: the NAND, a process, the card, or a list not read - nothing asked of AM)
    }
    IdCensus live;
    if (!ReadConsoleIds(&live)) {
        return kIdsRefused;
    }
    const auto listed = [&](int source) { return std::binary_search(live.ids[source].begin(), live.ids[source].end(), am); };
    if (listed(kSrcSd) || listed(kSrcPendingSd)) {
        return kStillListed; // (a title, or a title begun, at the id: the ticket is its own - never taken from under it here)
    }
    if (!listed(kSrcTicket)) {
        return 0; // (no ticket there: nothing to do)
    }
    const Result rc = AM_DeleteTicket(am);
    return R_FAILED(rc) ? rc : 0;
}

bool UnusableMayGo(u64 tid) {
    if (!IsOwnedGuestTid(tid)) {
        return true; // (not a child of ours: the Installation check's own rules, as before)
    }
    IdCensus live;
    return ReadConsoleIds(&live) && AmMayWrite(live, tid, SlotKeyOf(tid), false, false, false) &&
           AmMayWrite(live, tid, ProcessIdFor(SlotKeyOf(tid), kFormSystem), false, false, false);
}

u64 PreparedBytes(u64 prepared, u8 form) {
    if (!PreparedBand(prepared)) {
        return 0;
    }
    u64 tid = AmTitleFor(prepared, form);
    AM_TitleEntry entry{};
    return R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &entry)) ? entry.size : 0;
}

u16 UpdateVersion(u64 game) {
    u64 tid = 0x0004000E00000000ull | (game & 0xFFFFFFFFull);
    AM_TitleEntry entry{};
    return R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &entry)) ? static_cast<u16>(entry.version) : 0;
}

bool FreeSdBytes(u64* free) {
    FS_ArchiveResource res{};
    if (R_FAILED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD))) {
        return false;
    }
    *free = static_cast<u64>(res.freeClusters) * res.clusterSize;
    return true;
}

bool CardSlotHolds(u64 game) {
    bool inserted = false;
    if (R_FAILED(FSUSER_CardSlotIsInserted(&inserted)) || !inserted) {
        return false;
    }
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_GAME_CARD, &count)) || count == 0 || count > 8) {
        return false;
    }
    u64 ids[8];
    u32 got = 0;
    if (R_FAILED(AM_GetTitleList(&got, MEDIATYPE_GAME_CARD, count, ids))) {
        return false;
    }
    for (u32 i = 0; i < got && i < 8; ++i) {
        if (ids[i] == game) {
            return true;
        }
    }
    return false;
}

Result BeginInstall(u64 prepared, u8 form, bool ticket_expected, bool title_expected, Handle* handle) {
    *handle = 0;
    if (!Installable(form) || !PreparedBand(prepared) || !IsOwnedGuestTid(AmTitleFor(prepared, form))) {
        return kNotOurs; // (the build 2h: never the form M3c1 - two ids, the game never freed by pm)
    }
    if (!IdsAllow(prepared, form, true, ticket_expected, title_expected)) {
        return kIdsRefused; // (the build 2h: the second guard of the ids - nothing asked of AM)
    }
    bool available = false;
    AM_QueryAvailableExternalTitleDatabase(&available);
    AM_DeletePendingTitle(MEDIATYPE_SD, AmTitleFor(prepared, form)); // (what a run that was cut may have left of THIS title, behind the guard above)
    return AM_StartCiaInstall(MEDIATYPE_SD, handle);
}

Result WriteInstall(Handle handle, u64 off, const u8* buf, u32 len) {
    u32 written = 0;
    const Result rc = FSFILE_Write(handle, &written, off, buf, len, 0);
    return R_FAILED(rc) ? rc : (written == len ? 0 : kShortWrite);
}

Result FinishInstall(Handle handle) {
    return AM_FinishCiaInstall(handle);
}

void CancelInstall(Handle handle, u64 prepared, u8 form) {
    if (handle != 0) {
        AM_CancelCIAInstall(handle);
    }
    DeletePendingPrepared(prepared, form); // (behind the guard of the ids)
}

// (DPAPP-2h-sec) the stamp of this console for the records' ticket word (dlp_guest_store.hpp ThisConsole): CFG's hash unique to the console (read only),
// AM's device id when CFG does not answer. Neither: 0 - no record's ticket word holds, no ticket is dropped on a record's word
static u32 ReadConsoleStamp() {
    u64 unique = 0;
    if (R_FAILED(CFGU_GenHashConsoleUnique(0x3D51, &unique)) || unique == 0) {
        s32 internal = 0;
        u32 device = 0;
        unique = R_SUCCEEDED(AM_GetDeviceId(&internal, &device)) && internal == 0 ? device : 0;
    }
    return ConsoleStampOf(unique);
}

// (DPAPP-2j-rev) On an emulator only (svcGetSystemInfo 0x20000: 0 on a console's Luma, 1 on Citra, 2 on Azahar): Azahar does not implement
// FSUSER_GetSdmcCtrRootPath (its log: "unimplemented function 'GetSdmcCtrRootPath'") and every record stopped there - the ONE ID0 folder of its
// card. A console never guesses (a card that was in two consoles has two folders): "".
static std::string Id0OnEmulator() {
    s64 v = 0;
    svcGetSystemInfo(&v, 0x20000, 0); // (DPAPP-2k: any value but 0 is an emulator - Azahar writes 2; never the Result: Luma's is 1, positive)
    return v != 0 ? Id0OfSdCard("sdmc:/Nintendo 3DS") : std::string();
}

// (DPAPP-2j, R-B) the ID0 of this console, as the system itself names the folder of its data on the card (FSUSER_GetSdmcCtrRootPath: no key, no
// movable.sed read); "" when it does not answer - then no record is read or written (on an emulator: its one folder, Id0OnEmulator)
static std::string ReadThisId0() {
    u16 root[256] = {};
    if (R_FAILED(FSUSER_GetSdmcCtrRootPath(reinterpret_cast<u8*>(root), sizeof(root)))) {
        return Id0OnEmulator();
    }
    std::string ascii;
    for (std::size_t k = 0; k < 255 && root[k] != 0; ++k) {
        ascii += root[k] < 0x80 ? static_cast<char>(root[k]) : '?';
    }
    const std::string id0 = Id0OfRoot(ascii);
    return id0.empty() ? Id0OnEmulator() : id0; // (an answer with nothing written in it: the emulator's)
}

StartReport StartGuests() {
    SetThisConsole(ReadConsoleStamp()); // (before the start reads a record: SweepAtStart, then every run)
    SetThisId0(ReadThisId0());          // (DPAPP-2j, R-B: the folder of this console's records)
    ConsoleStart console;
    return SweepAtStart(console);
}

} // namespace dlpguest
