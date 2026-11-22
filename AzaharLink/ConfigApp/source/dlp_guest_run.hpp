// The jobs of the worker that prepares the Download Play children (1.1.4, piece APP-WORKER: 6-APP-boot9.md 4.7, its critic 6-APP-boot9-CRITICO.md 4.1-4.5
// and 6 step 5, K9 of 7-CONTRATTO.md): one function per Act of dlp_guest_decide.hpp, in the order that survives a cut at any moment.
//   Prepare: the record (Preparing) is written BEFORE any call to AM; the child is composed into <prepared>.part; AM is asked to install it (nothing is
//            deleted before: AM keeps the old title until its Finish, and a cut leaves it as it was); the work file goes on every way out; then the
//            plugin's copy (through the CopyWorker, the one thread that writes AzaharLink.3gx.part), the block, the record (Installed), the index.
//            The build 2h: in the system form (ONE id 00040010|low, dlp_guest_store.hpp); a child of another form is migrated (its old title goes
//            only AFTER the new one is on the SD card, its old folder after the new one); the system that refuses it gets the old one (D2:
//            kErrShownInHome). Before EVERY write the ids of the slot are read again from the console and judged (dlp_guest_ids.hpp, R0-R8).
//   Remove:  the record (Removing), the index without the row, the plugin's copy, the title (only when it is still OURS: same product code), the block,
//            the record last.
//   Disown:  a title that is not ours any more: the plugin's copy, the block, the record. AM is never asked anything about it.
//   Probe, Check: a look at content 2 (a few KB): children.cache (the child, "none", or a failure that is the game's: FailureKept), and the record of
//            a child that did not change. Without boot9.bin route F alone may still say "none"; a child it finds waits for the keys (kErrNoBoot9).
//   Adopt:   the record of a title of the band that IS this child (same product code, same version), made again by the next look (composer 0).
//   Repair:  the plugin's copy, the block, SD or card, the language.
// Portable: the system is the WorkConsole the caller gives (dlp_guest.cpp on the console, a fake in Tests/settings_test/dlp_guest_run_test.cpp), the
// files are POSIX (the store's). Nothing here writes a log: every step is handed to WorkConsole::Step with its Result, and the console writes the line
// (never a key, never a byte of a game).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dlp_guest_compose.hpp"
#include "dlp_guest_ids.hpp"
#include "dlp_guest_reconcile.hpp"
#include "dlp_guest_store.hpp"

namespace dlpguest {

using RunResult = std::int32_t; // a Result of the system, as it came

/// Where a game's content 2 was read from.
enum class Route : u8 {
    None,
    System, ///< route F: FS gives the RomFS of the content (the NCCH layer off): the only way for a game card
    Keyed,  ///< route K: the module's virtual CIA of an SD title (the scan's keys: boot9.bin and movable.sed) and the container's layer by boot9.bin
};

/// What the jobs ask of the system.
struct WorkConsole {
    virtual ~WorkConsole() = default;
    /// Opens the RomFS of content 2 of `game` (route F first, then K for the SD card): 0, or kErrNoSource, kErrNoMovable, kErrNoChild (no content 2:
    /// the game has no child), kErrContainerCrypto, kErrNcch. `route` says which way opened it.
    virtual int OpenContent(u64 game, bool card, Route* route) = 0;
    virtual bool ReadContent(u64 off, u8* buf, u32 len) = 0;
    virtual void CloseContent() = 0;
    /// The game card in the slot is `game` NOW, read again (false: another card, none, or AM could not say). A card job is planned by a look and
    /// runs later: what the slot answers is that game's only while it is still there.
    virtual bool CardHolds(u64 game) = 0;
    // (the build 2f) every call about AM names the form of the prepared title: AM is asked about AmTitleFor(prepared, form) (dlp_guest_store.hpp)
    virtual Presence Installed(u64 prepared, u8 form, u16* version) = 0; ///< what AM says of the prepared title (never Absent on a failure)
    virtual bool Product(u64 prepared, u8 form, char* out) = 0;  ///< its product code, kProductBytes with its NUL; false = unread
    virtual bool FreeBytes(u64* free) = 0;                       ///< the SD card's free space; false = not known
    /// (a title of ours) THAT pending title cancelled, a CIA install begun on the SD card. `ticket_expected`, `title_expected`: the ticket, the title
    /// of the SD card at the id may be there (ours: the guard of dlp_guest_am.cpp refuses any other, the NAND, a process, the card - the build 2h)
    virtual RunResult BeginInstall(u64 prepared, u8 form, bool ticket_expected, bool title_expected) = 0;
    /// the CIA, in order: 0, or the Result AM or FS gave the write (DPAPP-2j, R-D: never a bool again - the 2i lost the Result of every refusal)
    virtual RunResult Install(u64 off, const u8* buf, u32 len) = 0;
    virtual RunResult FinishInstall() = 0;
    virtual void CancelInstall(u64 prepared, u8 form) = 0;       ///< the install begun given up, THAT pending title cancelled
    /// (a title of ours) the title, its pending part, and its ticket only when `drop_ticket` (R7); 0 = AM no longer lists it
    virtual RunResult DeleteTitle(u64 prepared, u8 form, bool drop_ticket) = 0;
    /// the plugin's copy in the folder of `process_id` (ProcessIdFor of the form: the build 2h's system id, or the key) (true = done)
    virtual bool Folder(u64 process_id, u64 game, bool on) = 0;
    /// (the build 2h) every list of ids of the console read again NOW (dlp_guest_ids.hpp: the SD card, the NAND, the tickets, the card, the processes,
    /// the titles begun); false when a needed list was not read (R1: nothing is written then)
    virtual bool ReadIds(IdCensus* live) = 0;
    virtual bool Stop() = 0;                                     ///< the app is ending: the job stops where it is (and leaves nothing half done)
    virtual void Progress(int stage, u64 done, u64 total) = 0;   ///< a Stage of the composer
    virtual void Step(const char* what, u64 tid, RunResult rc) = 0; ///< one line of the log: a step, the title it is about, its Result
    /// (the build 2e slot25) one line of the log as it is, made by the composer's CryptoLine: the case of a child whose layer was not opened, or
    /// where the KeyX of slot 0x25 was found in boot.firm (an offset, never a key)
    virtual void Note(const char* line) = 0;
    /// (the build 2e, a diagnostic only) the icon of the prepared title read by {tid, SD}, the way HOME is believed to read it (FBI's: an open rc
    /// != 0 says that way fails, rc 0 does not prove HOME reads it): one line of the log. Nothing depends on it, it gives nothing back: a
    /// preparation never fails because of it.
    virtual void HomeIcon(u64 prepared) = 0;
};

/// What the jobs are given besides the game.
struct RunIn {
    const Keys* keys = nullptr; ///< boot9.bin's and the built-in values (ComposeKeys); null: nothing is composed or opened (kErrNoBoot9)
    u8 language = 1;            ///< the console's: the block's names
    u8* buf = nullptr;          ///< the stream's chunk, a multiple of 16
    std::size_t buf_size = 0;
    u8* work = nullptr;         ///< kWorkBytes at least
    std::size_t work_size = 0;
    Composed* composed = nullptr; ///< on the heap (31 KB)
    std::vector<Waiting> waiting; ///< the games switched on whose child is known and not prepared: rows of the index with prepared 0
    u32 keys_stamp = 0;           ///< KeysStamp() of the run: written with a failure of a look (another boot9.bin reads the game again)
    FirmFn firm = nullptr;        ///< (the build 2e slot25) boot.firm for a child of method 1, read once per run, read only (dlp_guest.cpp FirmOf)
    void* fctx = nullptr;
    ClockFn clock_ms = nullptr;   ///< the time of the search in boot.firm, for the line of the log
    /// (the build 2f) the memo of the session: the system refused the hidden form once (the controller's, dlp_guest.cpp): the next children of the
    /// session go to the old form at once (D2), with the line "hidden form skipped". Null: no memo (the tests that do not ask).
    std::atomic<bool>* hidden_refused = nullptr;
    /// (the build 2h) the cache of the ids (dlp_guest_ids.hpp): what the look saw. Before every write the console is read again and the slot held
    /// against it (R8: a difference stops the job, kErrIdStale, and the cache takes what the console says); after every write of the run it takes
    /// the console's ids again (the run's own writes are not a difference). Null: no cache to hold against (the R1-R7 still are).
    IdCensus* ids = nullptr;
};

/// What a job did besides its result (the controller's, for the pages).
struct JobNote {
    Route route = Route::None;
    bool card_checked = false; ///< (Check, Prepare of a card) the child of the card in the slot was read this session
    bool remade = false;       ///< (Check) the child changed: the record is made again by the next look
};

/// Runs one job: 0, or a kErr* of dlp_guest_logic.hpp (kErrNoMovable: route F refused and route K without movable.sed or before the scan, K9.4).
int RunJob(const Planned& job, const RunIn& in, WorkConsole& console, JobNote* note);

/// The index the plugin reads, from the records and `waiting` (only written when it changed).
bool RewriteIndex(const std::vector<Waiting>& waiting);

/// The id of a child named by the first file of a RomFS ("00040001xxxxxxxx.cia", any case): 0 when it is not such a name.
u32 ChildLowOfName(const char* name);

} // namespace dlpguest
