// The ids of the console and their guard (1.1.4, the build 2h of 2026-11-19 evening: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 15.12).
// The ids of the prepared children are made up by 3DS Link (the band 0x0F8xxxxx, azl::title_rules::PreparedOf): writing over an id that is somebody
// else's - a title of the system above all - can break the console. So no write of AM about a prepared child is ever decided on a guess:
//   - the CENSUS (IdCensus): every id the console holds now, by source - the titles of the SD card and of the NAND (read only), the tickets, the game
//     card, the titles AM has begun to install (SD and NAND), the processes that run (a closed game that stays in memory too) -, each source with its
//     count, the FNV-1a 64 of its sorted list and the milliseconds it took. Kept on the SD card (ids.cache: a version and a seal; a file whose seal does
//     not check out is read again from the console, never trusted) and refreshed by the looks of Download Play (RefreshCensus: every list read again,
//     a few milliseconds each, and only the sources whose count or FNV changed are put in again). The pages show the conflicts from it in advance;
//   - the GUARD: the decision to write never trusts the cache alone. Right before EVERY write (an install, a migration, a fallback, a removal, a
//     ticket) the worker reads the console again (WorkConsole::ReadIds), holds the three ids of the slot against the cache - any difference stops the
//     job, nothing written, the cache written again from the console (kErrIdStale) - and judges them (JudgeInstall, JudgeDelete, TicketMayGo:
//     the rules R0-R8). dlp_guest_am.cpp asks AmMayWrite once more right before the call to AM: two guards, no caller goes round them.
// Plain C++ and POSIX (setup::ReadTextFile / WriteTextFile): Tests/settings_test/dlp_guest_ids_test.cpp builds this very file. The lists are read by
// the caller's IdReader (dlp_guest_ids_am.cpp on the console, read only; a fake in the tests). Nothing here prints or logs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dlp_guest_store.hpp"

namespace dlpguest {

/// Where an id was seen.
enum IdSource : int {
    kSrcSd = 0,      ///< AM_GetTitleList(MEDIATYPE_SD)
    kSrcNand,        ///< AM_GetTitleList(MEDIATYPE_NAND): read only, never written
    kSrcTicket,      ///< AM_GetTicketList
    kSrcCard,        ///< AM_GetTitleList(MEDIATYPE_GAME_CARD) (an empty slot is an empty list)
    kSrcProcess,     ///< the title ids of the processes of svcGetProcessList (Luma's svcGetProcessInfo 0x10001)
    kSrcPendingSd,   ///< AM_GetPendingTitleList(MEDIATYPE_SD)
    kSrcPendingNand, ///< AM_GetPendingTitleList(MEDIATYPE_NAND)
    kSrcCount,
};
constexpr unsigned SrcBit(int source) {
    return 1u << source;
}
/// The sources every write needs read (R1): without one of them nothing is written. The card and the pending titles are read and judged when they
/// answer; a list of theirs that fails is written in the log, it does not stop the work (a DS card in the slot answers no list of AM).
constexpr unsigned kNeededSources = SrcBit(kSrcSd) | SrcBit(kSrcNand) | SrcBit(kSrcTicket) | SrcBit(kSrcProcess);
/// The word of a source in ids.cache and in the log.
const char* SourceWord(int source);

struct SourceState {
    bool read = false; ///< the list was read whole
    u32 count = 0;
    u64 fnv = 0;       ///< FNV-1a 64 of the sorted list (8 bytes per id, little endian)
    u32 ms = 0;        ///< what reading it took the last time
};
struct IdCensus {
    bool valid = false;           ///< read from the console (or from a sealed ids.cache) at least once
    SourceState src[kSrcCount];
    std::vector<u64> ids[kSrcCount]; ///< sorted, no id twice
};

/// What reads the lists (dlp_guest_ids_am.cpp on the console).
struct IdReader {
    virtual ~IdReader() = default;
    virtual bool List(int source, std::vector<u64>* ids) = 0; ///< the whole list of `source`; false: not read (nothing known of it)
    virtual u64 NowMs() = 0;
};

u64 FnvOfIds(const std::vector<u64>& sorted);

struct RefreshReport {
    unsigned changed = 0; ///< SrcBit of each source put in again (its count or FNV changed, or it was not read before)
    unsigned unread = 0;  ///< SrcBit of each source whose list failed
    u32 ms[kSrcCount] = {};
    u32 total_ms = 0;
};
/// Every list read again; only the sources that changed are put in again (their ids, count, FNV). true when every needed source was read (R1).
bool RefreshCensus(IdCensus* census, IdReader& reader, RefreshReport* report);
/// "ids: sd 312 (2 ms) nand 168 (3 ms) ticket 1402 (11 ms) ... - changed: sd, ticket; 21 ms" (a line of the log; no printf here)
std::string RefreshLine(const IdCensus& census, const RefreshReport& report, const char* when);

/// ids.cache: "#dlp-ids 1", "seal <16 hex digits>" (FNV-1a 64 of everything after that line), one "source <word> <read> <count> <fnv> <ms>" per
/// source, then one "<id, 16 hex digits>:<sources, 2 hex digits>" per id.
std::string CensusText(const IdCensus& census);
/// false for another header or version, a seal that does not check out, a line that does not read, or lists whose counts or FNV differ from the
/// lines of their sources: a file that is not whole is never trusted (it is read again from the console).
bool ParseCensus(const std::string& text, IdCensus* out);
std::string IdsCachePath(); ///< sdmc:/3ds/3DSLink/dlp-guests/<ID0>/ids.cache (DPAPP-2j: this console's; "" when the ID0 is not known)
bool LoadCensus(IdCensus* out);
bool SaveCensus(const IdCensus& census); ///< WriteTextFile (SweepOrphans knows its .tmp and .bak)

// ---- the three ids of a slot -----------------------------------------------------------------------------------------------------------
/// What the console holds of the ids of the forms of `slot` (the key): for each form, the SrcBit of each source that holds its id of AM (a process:
/// its id of AM or of the process, without the low byte, as pm's own search does - process_data.c:37); and which sources were read.
struct LiveSlot {
    unsigned read = 0;               ///< SrcBit of the sources read
    unsigned mask[kFormCount] = {};  ///< by form
};
LiveSlot SlotIn(const IdCensus& census, u64 slot);
/// The same as far as the slot goes (the sources read and every form's sources).
bool SameSlot(const LiveSlot& a, const LiveSlot& b);
/// "sys sd+ticket, app -, dlp sd" (a line of the log)
std::string SlotText(const LiveSlot& s);

/// Why an id of the slot cannot be written (Free: it can).
enum class IdTaken : u8 {
    Free,
    Unknown, ///< a needed list was not read (R1)
    Nand,    ///< an id of the slot is a title, or a title begun, of the NAND (R2): never written, never deleted
    Running, ///< a process runs (or stays in memory) with an id of the slot (R3)
    Card,    ///< the game card holds an id of the slot (R4)
    Sd,      ///< a title of the SD card at an id of the slot is not ours (R5)
    Ticket,  ///< a ticket of the id written is there and is not ours (R5, R7)
    Pending, ///< a title begun on the SD card at the id written that no run of ours began (R5)
};
const char* TakenWord(IdTaken t);

/// What the caller knows is OURS (the record and what AM said of the titles at the ids).
struct Ownership {
    bool record = false;    ///< a record of ours names the slot
    unsigned sd_ours = 0;   ///< FormBit of each form whose title on the SD card is ours (the record, and its product code read and the record's
                            ///< - or, for an install over it, unread)
    bool ticket_ours = false; ///< the ticket of the id written is ours (TicketMayGo)
};
/// R0-R5 for an INSTALL in `form`: every form's id is looked at (they share the slot: a title of somebody else's at any of them stops it).
IdTaken JudgeInstall(const LiveSlot& live, u8 form, const Ownership& own);
/// R1-R4 and R6 for a DELETE of the title in `form`: its id is not in the NAND, runs in no process, is not on the card, and a title of the SD card at it is
/// ours (`own.sd_ours`). An id that is not on the SD card is Free (the pending part and the ticket of a run of ours: TicketMayGo says the ticket).
IdTaken JudgeDelete(const LiveSlot& live, u8 form, const Ownership& own);
/// R7: the ticket of `form`'s id may be deleted - the record's form with the key ticket= says it; a record without it (the builds before) or another
/// form: only when our title is listed in that form now with the record's product code (`title_ours_now`). (DPAPP-2h-sec) The key holds only on the
/// console it was written on (TicketWordHere): a card moved to another console is the legacy rule there.
inline bool TicketMayGo(const Guest& g, u8 form, bool title_ours_now) {
    if (form == g.form && TicketWordHere(g)) {
        return g.ticket_ours;
    }
    return title_ours_now;
}

/// The second guard (dlp_guest_am.cpp, right before the call to AM, with the lists read again there): never an id of the NAND, of a running
/// process, of the card; an install never over a ticket (`ticket_expected`) or a title of the SD card (`title_expected`) the caller's own guard did
/// not find there and judge ours. `am` and `process` are the ids of the form written.
bool AmMayWrite(const IdCensus& live, u64 am, u64 process, bool install, bool ticket_expected, bool title_expected);

/// R0: a key the app may ever write about - of the band, never 3DS Link itself (with or without its low byte), never an id whose system form would
/// fall out of the band.
inline bool SlotIsSound(u64 slot) {
    return azl::title_rules::IsPreparedId(slot) && (slot & ~0xFFull) != (azl::title_rules::kAppTid & ~0xFFull) &&
           azl::title_rules::PreparedSystemOf(slot) != 0;
}

} // namespace dlpguest
