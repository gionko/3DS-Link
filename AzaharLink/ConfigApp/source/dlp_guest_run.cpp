#include "dlp_guest_run.hpp"

#include <cstdio>
#include <cstring>

#include "azl/dlp_children.hpp"
#include "azl/dlp_title_block.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_paths.hpp"
#include "dlp_guest_route.hpp"

namespace dlpguest {

namespace {

namespace dr = azl::dlp_real;

constexpr u64 kSpaceSlack = 4ull << 20; // what the SD card keeps free besides the work file and the title (2 x the child + 4 MiB)

bool ReadContentAt(void* ctx, u64 off, u8* buf, u32 len) {
    return static_cast<WorkConsole*>(ctx)->ReadContent(off, buf, len);
}

// The work file: written in order by Compose, read in order by Emit.
bool PartWrite(void* ctx, u64 off, const u8* buf, u32 len) {
    FILE* f = static_cast<FILE*>(ctx);
    return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 && std::fwrite(buf, 1, len, f) == len;
}

bool PartRead(void* ctx, u64 off, u8* buf, u32 len) {
    FILE* f = static_cast<FILE*>(ctx);
    return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 && std::fread(buf, 1, len, f) == len;
}

// (DPAPP-2h-sec, second pass) the sink of the installs' writes, with how far AM took the CIA: the free space of a refused write is held against
// what was still to write (F12). (DPAPP-2j, R-D) and the Result AM or FS gave the write that failed: the line of the log says it, ClassifyAm reads it
struct CountedSink {
    WorkConsole* c = nullptr;
    u64 end = 0;
    RunResult last = 0;
};

bool SinkCounted(void* ctx, u64 off, const u8* buf, u32 len) {
    CountedSink* s = static_cast<CountedSink*>(ctx);
    const RunResult rc = s->c->Install(off, buf, len);
    if (rc != 0) {
        s->last = rc;
        return false;
    }
    s->end = off + len;
    return true;
}

// (DPAPP-2j, R-D) a write or a Finish AM did not take: its own Result, where AM was, and what it says (FaultLine) - one line of the log
InstallFault NoteFault(WorkConsole& c, const char* step, u64 am, RunResult result, u64 at, u64 total) {
    if (result >= kErrFirst && result <= kErrLast) {
        // (DPAPP-2j-rev) a code of 3DS Link's own (InstallOverHigher's: its second write said its Result already) is never said as AM's Result
        c.Note((std::string("guest: ") + step + " " + HexDigits(am, 16) + ": 3DS Link's own code " + std::to_string(result) + ", no Result of AM or FS").c_str());
        return InstallFault::None;
    }
    const InstallFault fault = ClassifyAm(static_cast<u32>(result));
    c.Note(FaultLine(step, am, static_cast<u32>(result), at, total, fault).c_str());
    return fault;
}

bool OnStep(void* ctx, int stage, u64 done, u64 total) {
    WorkConsole* c = static_cast<WorkConsole*>(ctx);
    c->Progress(stage, done, total);
    return !c->Stop();
}

// The record of `prepared`, when there is one and it is ours.
bool FindRecord(u64 prepared, Guest* out) {
    if (!IsOwnedGuestTid(prepared)) {
        return false;
    }
    for (const Guest& g : LoadGuests()) {
        if (g.prepared == prepared) {
            *out = g;
            return true;
        }
    }
    return false;
}

// The game's child package in its content 2: where its CIA is and what a host announces of it. The content stays open on success.
struct Found {
    Route route = Route::None;
    u64 child = 0; // the child whose package it is
    dr::ChildPackage pkg{};
    u64 cia_at = 0;
};

int OpenChild(WorkConsole& c, const Planned& job, Found* out) {
    const int open = c.OpenContent(job.game, job.card, &out->route);
    c.Step(out->route == Route::Keyed ? "content 2 open (route K)" : "content 2 open (route F)", job.game, open);
    if (job.card && !c.CardHolds(job.game)) {
        // (the slot read again AFTER the open: a card swapped or pulled since the look that planned this job answers for another game, or for
        // none - its "not found" or its child is never written under this game: no source, tried again by the next look. The review of the final
        // review's fixes of 1.1.4)
        c.Step("card in the slot: not this game", job.game, -1);
        if (open == 0) {
            c.CloseContent();
        }
        return kErrNoSource;
    }
    if (open != 0) {
        return open;
    }
    u32 low = static_cast<u32>(job.game) & 0xFFFFFF00u; // (the child 0 of a game has its low id: Island Tour's is 000F8200)
    dr::PackageWhy why = dr::FindChildPackage(&ReadContentAt, &c, low, &out->pkg);
    if (why == dr::PackageWhy::NoFile) {
        const u32 named = ChildLowOfName(out->pkg.first_name); // (another id: the one the container's first file is named after)
        if (named != 0 && named != low) {
            low = named;
            why = dr::FindChildPackage(&ReadContentAt, &c, low, &out->pkg);
        }
    }
    c.Step("child package", 0x0004000100000000ull | low, static_cast<RunResult>(why));
    int rc = 0;
    switch (why) {
    case dr::PackageWhy::Ok: break;
    case dr::PackageWhy::NoFile: rc = kErrNoChild; break;
    // (not a RomFS: from the system that is a game with no child in content 2; through our keys it may be the layer taken off wrong)
    case dr::PackageWhy::RomFs: rc = out->route == Route::System ? kErrNoChild : kErrNcch; break;
    case dr::PackageWhy::Cia: rc = kErrNotCia; break;
    default: rc = kErrNoSource; break;
    }
    if (rc == 0 && out->pkg.smdh_at < static_cast<u64>(out->pkg.transfer_size) + dr::kCiaMetaSmdhAt) {
        rc = kErrNotCia;
    }
    if (rc != 0 && job.card && !c.CardHolds(job.game)) {
        rc = kErrNoSource; // (the card was taken out while its content was read: its "no child" or its broken package is not the game's - the review of spec 9)
    }
    if (rc != 0) {
        c.CloseContent();
        return rc;
    }
    out->child = 0x0004000100000000ull | low;
    out->cia_at = out->pkg.smdh_at - out->pkg.transfer_size - dr::kCiaMetaSmdhAt;
    return 0;
}

// children.cache: the facts of `game` at these versions take the place of what was known of it.
void Remember(const ChildSeen& now) {
    std::vector<ChildSeen> seen = LoadChildrenCache();
    std::vector<ChildSeen> kept;
    for (const ChildSeen& s : seen) {
        if (s.game != now.game) {
            kept.push_back(s);
        }
    }
    kept.push_back(now);
    SaveChildrenCache(kept);
}

ChildSeen SeenOf(const Planned& job, const Found& f, const ChildFacts& facts) {
    ChildSeen s;
    s.game = job.game;
    s.game_version = job.game_version;
    s.update_version = job.update_version;
    s.has_child = true;
    s.child = facts.child;
    s.child_version = facts.version;
    std::memcpy(s.child_hash8, facts.hash8, sizeof(s.child_hash8));
    s.transfer = f.pkg.transfer_size;
    s.required = f.pkg.required_size;
    std::memcpy(s.product, facts.product, sizeof(s.product));
    return s;
}

// A failed look that is the game's (FailureKept): remembered with its versions, its reason and boot9.bin's stamp, in place of what was known of the
// game (1.1.4, spec 9 point 5: it is not tried again at every pass; another version, another boot9.bin or A on its row "not read" try again).
int Failed(const Planned& job, const RunIn& in, int err) {
    if (FailureKept(err, job.card)) {
        ChildSeen failed;
        failed.game = job.game;
        failed.game_version = job.game_version;
        failed.update_version = job.update_version;
        failed.error = err;
        failed.keys = in.keys_stamp;
        Remember(failed);
    }
    return err;
}

// A look at content 2: the child's facts, or "no child" (written in children.cache: the game is not looked at again at these versions). Without the
// keys route F alone is tried: it may say "no child"; a child it finds, or a content only route K opens, waits for boot9.bin (kErrNoBoot9: the game
// stays to read, it is not a failure).
int LookAt(const Planned& job, const RunIn& in, WorkConsole& c, ChildSeen* out) {
    Found f;
    const int open = OpenChild(c, job, &f);
    if (open == kErrNoChild) {
        ChildSeen none;
        none.game = job.game;
        none.game_version = job.game_version;
        none.update_version = job.update_version;
        Remember(none);
        *out = none;
        return 0;
    }
    if (open != 0) {
        return Failed(job, in, open);
    }
    if (in.keys == nullptr) {
        c.CloseContent();
        // (route F found the child: its facts are read with the keys. It is written so - kErrNoBoot9 with the stamp of the run, 0 without boot9.bin -:
        // Download Play > Enable games shows the game, A on it says that boot9.bin is needed, and it is not read again at every start but when boot9.bin
        // comes, ToRead. Not a failure of the game's: FailureKept still says no. The review of spec 9)
        ChildSeen waits;
        waits.game = job.game;
        waits.game_version = job.game_version;
        waits.update_version = job.update_version;
        waits.error = kErrNoBoot9;
        waits.keys = in.keys_stamp;
        Remember(waits);
        return kErrNoBoot9;
    }
    ChildFacts facts;
    int rc = ReadChildFacts(&ReadContentAt, &c, f.cia_at, f.pkg.required_size, *in.keys, &facts);
    if (rc != 0 && job.card && !c.CardHolds(job.game)) {
        rc = kErrNoSource; // (the card was taken out while its child was read: not the game's, never remembered - the review of spec 9)
    }
    c.CloseContent();
    c.Step("child facts", facts.child, rc);
    if (rc != 0) {
        return Failed(job, in, rc);
    }
    *out = SeenOf(job, f, facts);
    Remember(*out);
    return 0;
}

int Probe(const Planned& job, const RunIn& in, WorkConsole& c) {
    ChildSeen seen;
    return LookAt(job, in, c, &seen);
}

int Check(const Planned& job, const RunIn& in, WorkConsole& c, JobNote* note) {
    const u64 prepared = PreparedFor(job.game);
    Guest g;
    if (!FindRecord(prepared, &g)) {
        return 0;
    }
    ChildSeen seen;
    const int rc = LookAt(job, in, c, &seen);
    if (rc != 0) {
        return rc;
    }
    note->card_checked = job.card;
    const bool same = seen.has_child && seen.child == g.child && seen.child_version == g.child_version &&
                      std::memcmp(seen.child_hash8, g.child_hash8, sizeof(g.child_hash8)) == 0;
    if (same) {
        g.game_version = job.game_version; // the same child: only the versions it was found at, and where
        g.update_version = job.update_version;
        g.where = job.card ? 'C' : 'S';
    } else {
        g.composer = 0; // another child (or none): the next look prepares it again (an Installed record is never left naming a child that is not there)
        note->remade = true;
    }
    const bool saved = SaveGuest(g);
    c.Step(same ? "check: same child" : "check: another child", prepared, saved ? 0 : -1);
    return saved ? 0 : kErrTemp;
}

// The record of a run that did not reach AM's Finish: the record as it was (an Installed child that AM kept, in its form), or none.
void Undo(const Guest* before, u64 prepared) {
    if (before != nullptr && before->state == GuestState::Installed) {
        SaveGuest(*before);
        SetForm(prepared, before->form); // (the build 2f: the jump and the pages ask AM by the record's form again)
        return;
    }
    DropGuest(prepared);
    Unregister(prepared);
}

// (DPAPP-2h-sec) Undo when the last install of the run may have left a ticket of ours at `form`'s id (ComposeAndInstall's `ticket_left`: the ticket it
// left could not be dropped, or the one that was there and its guard judged ours - InstallOverHigher may have deleted its title). The record never
// forgets it: an Installed record of that form says the ticket is ours; with no Installed record the record stays, Removing in `form` with the ticket
// said ours - the next look (Remove) or the start (TakeAway) drops the ticket, then the record. Without it the next child of the slot found "a ticket
// that is not ours" (326) for ever.
void UndoLeft(const Guest* before, Guest g, u8 form, bool ticket_left) {
    if (!ticket_left) {
        Undo(before, g.prepared);
        return;
    }
    if (before != nullptr && before->state == GuestState::Installed) {
        Guest kept = *before;
        if (kept.form == form) {
            SayTicketOurs(kept);
        }
        SaveGuest(kept);
        SetForm(kept.prepared, kept.form);
        return;
    }
    g.form = form;
    g.state = GuestState::Removing;
    g.prev_installed = false;
    g.prev_version = 0;
    g.prev_other = false;
    g.prev_other_version = 0;
    g.prev_other_form = kFormApplication;
    SayTicketOurs(g);
    if (!SaveGuest(g)) {
        Undo(before, g.prepared); // (the record cannot say it: as before)
        return;
    }
    SetForm(g.prepared, form);
}

// ---- the guard of the ids (the build 2h: dlp_guest_ids.hpp, the rules R0-R8) ------------------------------------------------------------

std::string HexText(u64 v) {
    char p[16];
    azl::dlp_children::PutHex16(p, v);
    return std::string(p, 16);
}

// The kErr* of an id that cannot be written.
int TakenError(IdTaken t) {
    switch (t) {
    case IdTaken::Free: return 0;
    case IdTaken::Unknown: return kErrInstall; // (a list of AM not read: nothing is written on a guess, as when AM says nothing of a title)
    case IdTaken::Nand: return kErrIdSystem;
    case IdTaken::Running: return kErrIdRunning;
    case IdTaken::Ticket: return kErrIdTicket;
    case IdTaken::Card:
    case IdTaken::Sd:
    case IdTaken::Pending: return kErrIdTaken;
    }
    return kErrIdTaken;
}

// The console's ids NOW (R1), held against the cache (R8): 0 and `*live`, or the kErr* that stops the job - nothing written. A difference is said in
// the log and the cache takes what the console says (the page reads it again: "try again").
int ReadLive(WorkConsole& c, const RunIn& in, u64 slot, const char* before_what, IdCensus* live) {
    *live = IdCensus();
    if (!c.ReadIds(live)) {
        c.Note(("guest: ids before " + std::string(before_what) + " " + HexText(slot) + ": a list of AM was not read (" + SlotText(SlotIn(*live, slot)) +
                "): nothing written").c_str());
        return kErrInstall;
    }
    if (in.ids != nullptr) {
        if (in.ids->valid) {
            const LiveSlot was = SlotIn(*in.ids, slot);
            const LiveSlot now = SlotIn(*live, slot);
            if (!SameSlot(was, now)) {
                c.Note(("guest: ids before " + std::string(before_what) + " " + HexText(slot) + ": the cache said " + SlotText(was) +
                        ", the console says " + SlotText(now) + ": nothing written, the cache taken again").c_str());
                *in.ids = *live;
                return kErrIdStale;
            }
        }
        *in.ids = *live; // (what the console holds now: the next guard of the run holds the slot against it)
    }
    return 0;
}

// After a write of this run: the cache takes the console's ids again (the run's own writes are no difference for the next guard).
void Wrote(WorkConsole& c, const RunIn& in) {
    if (in.ids != nullptr) {
        IdCensus now;
        c.ReadIds(&now);
        *in.ids = now;
    }
}

// What AM says of the three ids of the slot on the SD card: presence, version, product code.
struct Forms {
    Presence at[kFormCount] = {Presence::Absent, Presence::Absent, Presence::Absent};
    u16 version[kFormCount] = {};
    char product[kFormCount][kProductBytes] = {};
    bool read[kFormCount] = {};
};

bool Listed(Presence p) {
    return p == Presence::Present || p == Presence::Broken;
}

// false when AM said nothing of one of them (nothing is decided on a guess)
bool LookForms(WorkConsole& c, u64 slot, Forms* f) {
    for (const u8 form : kForms) {
        f->at[form] = c.Installed(slot, form, &f->version[form]);
        if (f->at[form] == Presence::Unknown) {
            return false;
        }
        if (Listed(f->at[form])) {
            f->read[form] = c.Product(slot, form, f->product[form]);
        }
    }
    return true;
}

// The title at `form`'s id is OURS by the record `g` for a DELETE (R6): listed, and its product code read and the record's - or a Broken title whose
// code does not read (an installation of ours cut in its commit).
bool OursToDelete(const Forms& f, u8 form, const Guest& g) {
    if (!Listed(f.at[form])) {
        return false;
    }
    if (f.read[form] && f.product[form][0] != '\0' && g.product[0] != '\0') {
        return !OtherProduct(f.product[form], g.product);
    }
    return f.at[form] == Presence::Broken && !(f.read[form] && f.product[form][0] != '\0');
}

// The ownership of the slot for an install in `form` (R5): a title is ours at a form's id when the record is there and its code is not another's
// (an install over our own title whose code does not read stays possible, as ever: AM keeps it until the Finish); the ticket of `form` by TicketMayGo.
Ownership InstallOwnership(const Forms& f, u8 form, bool had, const Guest& before) {
    Ownership own;
    own.record = had;
    for (const u8 x : kForms) {
        if (had && Listed(f.at[x]) && !(f.read[x] && OtherProduct(f.product[x], before.product))) {
            own.sd_ours |= FormBit(x);
        }
    }
    own.ticket_ours = had && TicketMayGo(before, form, OursToDelete(f, form, before));
    return own;
}

// The guard right before a DELETE of the title in `form`: the console read again, held against the cache, judged (R1-R4, R6). 0, or the kErr*
// (said in the log: nothing deleted).
int GuardDelete(WorkConsole& c, const RunIn& in, const Guest& g, u8 form, bool ours, const char* what) {
    IdCensus live;
    const int rc = ReadLive(c, in, g.prepared, what, &live);
    if (rc != 0) {
        return rc;
    }
    Ownership own;
    own.record = true;
    own.sd_ours = ours ? FormBit(form) : 0;
    const IdTaken t = JudgeDelete(SlotIn(live, g.prepared), form, own);
    if (t != IdTaken::Free) {
        c.Note(("guest: " + std::string(what) + " " + HexText(AmTitleFor(g.prepared, form)) + ": not deleted, " + TakenWord(t)).c_str());
    }
    return TakenError(t);
}

// The delete of the title of ours in `form`, behind its guard; the cache after it. 0 when AM no longer lists it.
RunResult DeleteGuarded(WorkConsole& c, const RunIn& in, const Guest& g, u8 form, bool ours, bool drop_ticket, const char* what) {
    const int guard = GuardDelete(c, in, g, form, ours, what);
    if (guard != 0) {
        return guard;
    }
    const RunResult rc = c.DeleteTitle(g.prepared, form, drop_ticket);
    Wrote(c, in);
    c.Step(what, AmTitleFor(g.prepared, form), rc);
    return rc;
}

// (the review of the build 2h, R1) the ticket a run of ours left at `form`'s id with no title: an install given up after its Begin (B, a write AM
// refused, a Finish that failed - AM_CancelCIAInstall does not take back the ticket AM took from the CIA's first bytes), a run cut there, a
// removal whose title had gone. Without it the next install of the slot would find "a ticket that is not ours" (326) for ever. Only when AM lists
// no title at the id (a title's ticket is its own) and the console holds a ticket there; behind the guard, as every delete. true when no ticket is
// left there (none, or dropped); false when AM or a list said nothing, or the delete did not take it away (the caller keeps what says it is ours).
bool DropLeftTicket(WorkConsole& c, const RunIn& in, const Guest& g, u8 form, const char* what) {
    u16 version = 0;
    const Presence at = c.Installed(g.prepared, form, &version);
    if (at == Presence::Unknown) {
        return false;
    }
    if (at != Presence::Absent) {
        return true; // (a title at the id: its ticket is its own - nothing of a run of ours to drop)
    }
    IdCensus now;
    if (!c.ReadIds(&now) || !now.src[kSrcTicket].read) {
        return false;
    }
    if ((SlotIn(now, g.prepared).mask[form] & SrcBit(kSrcTicket)) == 0) {
        return true;
    }
    DeleteGuarded(c, in, g, form, true, true, what);
    IdCensus after;
    return c.ReadIds(&after) && after.src[kSrcTicket].read && (SlotIn(after, g.prepared).mask[form] & SrcBit(kSrcTicket)) == 0;
}

// (DPAPP-2h-sec, second pass) whether the console holds a ticket at `form`'s id now (read only; Unknown: a list not read) for a record whose word on
// it is not known to be this console's (TicketWordUnplaced): nothing is dropped on that word; a ticket there is said, and the record stays with it
Presence TicketAtId(WorkConsole& c, const Guest& g, u8 form, const char* what) {
    IdCensus now;
    if (!c.ReadIds(&now) || !now.src[kSrcTicket].read) {
        return Presence::Unknown;
    }
    if ((SlotIn(now, g.prepared).mask[form] & SrcBit(kSrcTicket)) == 0) {
        return Presence::Absent;
    }
    c.Note(("guest: " + std::string(what) + " " + HexText(AmTitleFor(g.prepared, form)) +
            ": a ticket is there, the record's word on it not known as this console's: ticket and record kept").c_str());
    return Presence::Present;
}

// The step of the stream that does not stop (the second write of InstallOverHigher: the old child is already gone - a stop there would leave the
// console with no child at all; the review of the build 2h).
bool OnStepToTheEnd(void* ctx, int stage, u64 done, u64 total) {
    static_cast<WorkConsole*>(ctx)->Progress(stage, done, total);
    return true;
}

// AM does not put a lower version over a higher one: a child made again from a game whose child is older (the card of revision 1.0 after the eShop
// copy of 1.1) is refused at the Finish. Only then, and only over OUR child (Prepare checked its record and its product code), the old title is
// deleted and the same work file installed once more (6-APP-boot9-CRITICO.md 1.6: deleted first only when the Finish refuses). A failure after the
// delete leaves no child: the record goes back to the Installed one of before, whose title is gone, and the next look prepares it again like a title
// taken away by hand. Any other refusal of the Finish keeps the old child (a remake that fails never leaves the console without one). The build 2h:
// the delete behind its guard, the ticket kept (ours, written again by the install), and the second write to its end - B never cuts it.
RunResult InstallOverHigher(WorkConsole& c, const RunIn& in, const Guest& g, u8 form, const Composed& made, FILE* file, u64 total) {
    const u64 am = AmTitleFor(g.prepared, form);
    c.CancelInstall(g.prepared, form);
    Wrote(c, in);
    const RunResult deleted = DeleteGuarded(c, in, g, form, true, false, "install over a higher version: old child deleted");
    if (deleted != 0) {
        return deleted;
    }
    const RunResult begun_again = c.BeginInstall(g.prepared, form, true, false); // (our old child is gone: no title expected at the id)
    Wrote(c, in);
    c.Step("install begin again", am, begun_again);
    if (begun_again != 0) {
        return begun_again;
    }
    CountedSink sent;
    sent.c = &c;
    const int written = Emit(made, &PartRead, file, &SinkCounted, &sent, &OnStepToTheEnd, &c, in.buf, in.buf_size);
    c.Step("install write again", am, written);
    if (written != 0) {
        if (written == kErrInstall) {
            NoteFault(c, "install write again", am, sent.last, sent.end, total); // (DPAPP-2j, R-D: AM's own Result)
        }
        // (DPAPP-2j-rev) what its Result says, as for the first write: an error of the SD card 330, a full card 311 - never 315
        const InstallFault again = written == kErrInstall ? ClassifyAm(static_cast<u32>(sent.last)) : InstallFault::None;
        return again == InstallFault::SdIo ? kErrSdIo : again == InstallFault::NoSpace ? kErrNoSpace : written; // (the caller gives the install up)
    }
    const RunResult finished_again = c.FinishInstall();
    Wrote(c, in);
    c.Step("install finish again", am, finished_again);
    return finished_again;
}

// How one composition and its install went (ComposeAndInstall).
enum class Took : u8 {
    Done,    ///< AM took the title in the form asked: the work file is gone
    Refused, ///< AM did not take it - a write of the sink, the Finish -: THAT pending title cancelled, the work file gone, the record as it is
    Failed,  ///< not the title's: the composition, the work file, a stop, the begin, the guard (`*rc` says which): nothing of AM is left begun
};

// The same child as the record's (its TMD as read: id, version, the hash of its content), for the fallback that keeps the old form.
bool SameChild(const Guest& g, const Composed& made) {
    return g.child == made.tmd.tid && g.child_version == made.tmd.version && std::memcmp(g.child_hash8, made.tmd.hash, sizeof(g.child_hash8)) == 0;
}

// (the review of the build 2f) AM took the hidden form of this title before: the record is of that form (an Installed hidden child, whose title may
// have gone since). A failure over it is a fault of the moment, never a refusal of the form: the memo of the session never skips it, the fallback
// never turns it back into the old form. One condition for both. DPAPP-2j: the hidden form is the one made, M3c1 (kPreparedForm; a record of the
// system form of the build 2h, or of the old form, is migrated)
bool HiddenTakenBefore(bool had, const Guest& before) {
    return had && before.form == kPreparedForm;
}

// (DPAPP-2j-rev) a guest of the old form - Installed, HOME shows it (the 2i's fallback) - that AM lists: a refused Update leaves it as it was
bool OldFormStays(const Guest* before, const Forms& fs) {
    return before != nullptr && before->state == GuestState::Installed && before->form == kFormApplication && fs.at[kFormApplication] == Presence::Present;
}

// The child of the open content `f` composed into the work file with the ids of `form` (the build 2h: the exheader's are ProcessIdFor(prepared, form),
// the ticket's, the TMD's and the NCCH's 0x108/0x118 AmTitleFor(prepared, form) - in the system form ONE id), then installed behind the guard of the
// ids (the console read again right before AM is asked: `own` says what is ours). The content is closed on every way out. `over_ours`: our child of
// the same form is there at `at_version` (InstallOverHigher may delete it when AM refuses an older one). `smdh` and `product` get the meta block's
// SMDH and the NCCH's product code. The record `g` gets its product code when it had none (before AM is asked: K9.1).
Took ComposeAndInstall(const Planned& job, const RunIn& in, WorkConsole& c, const Found& f, Guest& g, u8 form, const Ownership& own, bool over_ours,
                       u16 at_version, u8* smdh, char* product, int* rc_out, bool* ticket_left) {
    const u64 prepared = g.prepared;
    *ticket_left = false;
    const u64 am = AmTitleFor(prepared, form);
    const std::string part = PartPath(prepared);
    FILE* file = std::fopen(part.c_str(), "w+b");
    if (file == nullptr) {
        c.CloseContent();
        *rc_out = kErrTemp;
        return Took::Failed;
    }
    ComposeIn ci;
    ci.read = &ReadContentAt;
    ci.rctx = &c;
    ci.cia_at = f.cia_at;
    ci.cia_size = f.pkg.required_size;
    ci.keys = in.keys;
    ci.prepared = ProcessIdFor(prepared, form); // (the build 2h: the exheader's ids - the process's; in the system form 00040010|low)
    ci.am_title = am; // (the ticket, the TMD, 0x108 and 0x118; in the system form the same id: ONE id for pm, NS and AM)
    ci.variant = kHide; // the user's decision: never visible in HOME (Compose refuses anything else on the console)
    ci.temp = &PartWrite;
    ci.tctx = file;
    ci.step = &OnStep;
    ci.sctx = &c;
    ci.buf = in.buf;
    ci.buf_size = in.buf_size;
    ci.work = in.work;
    ci.work_size = in.work_size;
    ci.firm = in.firm;
    ci.fctx = in.fctx;
    ci.clock_ms = in.clock_ms;
    Composed& made = *in.composed;
    int rc = Compose(ci, &made);
    c.Step("compose", made.tmd.tid, rc);
    if (rc == kErrChildCrypto || made.slot25 != Slot25Step::None) {
        // (the build 2e slot25) why the child's layer was not opened - its 0x188-0x18F, the case, the step of the search in boot.firm - or where
        // the KeyX of slot 0x25 was found: an offset, never the key. Also when the key was found and the child failed after it (the review: 309 of
        // the ExeFS's list, 310, the SD card, a stop): the step "compose" above says the result (Composed is fresh: Compose clears it first)
        char line[kCryptoLineBytes];
        CryptoLine(made, job.game, line, sizeof(line));
        c.Note(line);
    }
    const bool smdh_read = rc == 0 && ReadContentAt(&c, f.pkg.smdh_at, smdh, kSmdhSize); // (the block's names and icon: the meta block's SMDH)
    c.CloseContent();
    std::memset(product, 0, kProductBytes);
    if (rc == 0 && !PartRead(file, 0x150, reinterpret_cast<u8*>(product), 16)) {
        rc = kErrTemp;
    }
    if (rc == 0 && !smdh_read) {
        rc = kErrNoSource;
    }
    if (rc == 0 && g.product[0] == '\0' && product[0] != '\0') {
        // (a first record has no product code: it is written BEFORE AM is asked, so that a cut from here on leaves a record the start can hold
        // against the title at the id - a title with another code is never deleted, K9.1; the final review of 1.1.4)
        std::memcpy(g.product, product, sizeof(g.product));
        rc = SaveGuest(g) ? 0 : kErrTemp;
    }
    // (the build 2h) the guard of the ids, right before AM is asked: the console read again, held against the cache, judged for this form (R1-R5, R8)
    bool ticket_there = false, title_there = false;
    if (rc == 0) {
        IdCensus live;
        rc = ReadLive(c, in, prepared, "the install", &live);
        if (rc == 0) {
            const LiveSlot slot = SlotIn(live, prepared);
            const IdTaken t = JudgeInstall(slot, form, own);
            if (t != IdTaken::Free) {
                c.Note(("guest: install " + HexText(am) + ": not begun, " + std::string(TakenWord(t)) + " (" + SlotText(slot) + ")").c_str());
                rc = TakenError(t);
            }
            ticket_there = (slot.mask[form] & SrcBit(kSrcTicket)) != 0;
            title_there = (slot.mask[form] & SrcBit(kSrcSd)) != 0; // (judged ours above: our child that AM keeps until the Finish)
        }
        if (rc == 0 && !(TicketWordHere(g) && g.ticket_ours)) {
            // (DPAPP-2h-sec) the ticket of THIS form's id is ours only now that its guard let the run go on (none there, or ours): the fallback's
            // record said nothing of it until here - a cut from now on drops only what the record says, and the record says it before AM is asked
            SayTicketOurs(g);
            rc = SaveGuest(g) ? 0 : kErrTemp;
        }
    }
    if (rc != 0) {
        std::fclose(file);
        std::remove(part.c_str());
        *rc_out = rc;
        return Took::Failed;
    }

    // AM: nothing is deleted first. AM keeps the title that is there until the Finish; a failure on the way leaves it as it was.
    const RunResult begun = c.BeginInstall(prepared, form, ticket_there, title_there);
    Wrote(c, in);
    c.Step("install begin", am, begun);
    if (begun != 0) {
        std::fclose(file);
        std::remove(part.c_str());
        *rc_out = kErrInstall;
        // (the review of the build 2f) never a refusal of the form: AM_StartCiaInstall is asked before AM reads a byte of the ticket or the TMD
        // (dlp_guest_am.cpp BeginInstall): AM busy, a fault of the moment, the guard of dlp_guest_am.cpp - no fallback, no memo
        return Took::Failed;
    }
    CountedSink sent;
    sent.c = &c;
    rc = Emit(made, &PartRead, file, &SinkCounted, &sent, &OnStep, &c, in.buf, in.buf_size);
    c.Step("install write", am, rc);
    if (rc != 0) {
        // (DPAPP-2j, R-D) the Result AM or FS gave the write, said with where AM was; only AM's refusal (InstallFault::FormRefused) is a refusal of
        // the form - an error of the SD card is kErrSdIo, a full card kErrNoSpace, anything else kErrInstall with its Result: never the memo
        const InstallFault fault = rc == kErrInstall ? NoteFault(c, "install write", am, sent.last, sent.end, f.pkg.required_size) : InstallFault::None;
        // (DPAPP-2h-sec, second pass) the free space read NOW, while AM's pending title and the work file still hold theirs - after the cancel and the
        // remove it is back to what the check before the run found: below what was still to write, plus the slack, the card is full (F12)
        u64 free_now = 0;
        const u64 left_to_write = f.pkg.required_size > sent.end ? f.pkg.required_size - sent.end : 0;
        const bool card_full = rc == kErrInstall && (fault == InstallFault::NoSpace || (c.FreeBytes(&free_now) && free_now < left_to_write + kSpaceSlack));
        c.CancelInstall(prepared, form);
        Wrote(c, in);
        std::fclose(file);
        std::remove(part.c_str());
        // (DPAPP-2h-sec) what may be left of ours at the id: the ticket the run left when it could not be dropped, or the one that was there
        *ticket_left = ticket_there || !DropLeftTicket(c, in, g, form, "install given up: the ticket it left dropped"); // (the review of the build 2h, R1: B leaves nothing)
        *rc_out = rc;
        if (card_full) {
            // (DPAPP-2h-sec) a write AM refused with the card full is no refusal of the form: no memo, no fallback (the black icon of HOME), said
            c.Step("install write: the SD card is full", am, static_cast<RunResult>(free_now >> 20));
            *rc_out = kErrNoSpace;
            return Took::Failed;
        }
        if (fault == InstallFault::SdIo) {
            *rc_out = kErrSdIo; // (DPAPP-2j, R-D: an error of the SD card is no refusal of the form)
            return Took::Failed;
        }
        // (a stop or the work file are not AM's; only AM's refusal is the form's - DPAPP-2j)
        return fault == InstallFault::FormRefused ? Took::Refused : Took::Failed;
    }
    RunResult finished = c.FinishInstall();
    Wrote(c, in);
    c.Step("install finish", am, finished);
    if (finished != 0 && over_ours && at_version > made.tmd.version) {
        finished = InstallOverHigher(c, in, g, form, made, file, f.pkg.required_size); // (AM refused the older child over ours)
    }
    // (DPAPP-2h-sec, second pass) a Finish refused with the card full: the free space read before the cancel and the remove of the work file (F12)
    u64 free_at_finish = 0;
    const bool full_at_finish = finished != 0 && c.FreeBytes(&free_at_finish) && free_at_finish < kSpaceSlack;
    std::fclose(file);
    std::remove(part.c_str());
    if (finished != 0) {
        // (DPAPP-2j, R-D) the Finish's own Result and what it says: only AM's refusal is the form's
        const InstallFault fault = NoteFault(c, "install finish", am, finished, f.pkg.required_size, f.pkg.required_size);
        c.CancelInstall(prepared, form);
        Wrote(c, in);
        *ticket_left = ticket_there || !DropLeftTicket(c, in, g, form, "install not finished: the ticket it left dropped"); // (the review of the build 2h, R1)
        *rc_out = kErrInstall;
        if (full_at_finish || fault == InstallFault::NoSpace) {
            // (no refusal of the form: no memo, no fallback - the black icon of HOME -, said)
            c.Step("install finish: the SD card is full", am, static_cast<RunResult>(free_at_finish >> 20));
            *rc_out = kErrNoSpace;
            return Took::Failed;
        }
        if (finished == kErrSdIo || finished == kErrNoSpace) {
            *rc_out = finished; // (DPAPP-2j-rev: InstallOverHigher's second write said the SD card - its Result in the log -, never 315)
            return Took::Failed;
        }
        if (fault == InstallFault::SdIo) {
            *rc_out = kErrSdIo;
            return Took::Failed;
        }
        return finished == kErrCancel || finished == kErrTemp || fault != InstallFault::FormRefused ? Took::Failed : Took::Refused;
    }
    *rc_out = 0;
    return Took::Done;
}

// The ids of the OTHER forms of our child, when AM lists one with the record's product code (the half a migration or a fallback that was cut left):
// deleted behind the guard, with the step `what`. Another title there, or one whose code does not read, is never touched (K9.1, R6). True when
// nothing of ours is left there (absent, deleted, or not ours); false when AM said nothing or the delete did not take it away (the record says
// what is still to do).
bool DropOtherForm(WorkConsole& c, const RunIn& in, const Guest& g, const char* what) {
    bool clean = true;
    for (const u8 other : kForms) {
        if (other == g.form) {
            continue;
        }
        const u64 am = AmTitleFor(g.prepared, other);
        u16 version = 0;
        Presence at = c.Installed(g.prepared, other, &version);
        if (at == Presence::Unknown) {
            clean = false;
            continue;
        }
        if (at == Presence::Absent) {
            continue;
        }
        char product[kProductBytes] = {};
        if (!c.Product(g.prepared, other, product) || product[0] == '\0' || g.product[0] == '\0' || OtherProduct(product, g.product)) {
            c.Step("other form not ours: kept", am, 0);
            continue;
        }
        const RunResult rc = DeleteGuarded(c, in, g, other, true, true, what); // (ours, listed now in that form: its ticket too - R7)
        at = c.Installed(g.prepared, other, &version);
        // (DPAPP-2h-sec, second pass) the title gone and its ticket not known gone (DeletePrepared's kTicketLeft): the ticket alone, now - the record
        // stays while it is there
        clean = clean && at == Presence::Absent && (rc == 0 || DropLeftTicket(c, in, g, other, what));
    }
    return clean;
}

int Prepare(const Planned& job, const RunIn& in, WorkConsole& c, JobNote* note) {
    if (in.keys == nullptr) {
        return kErrNoBoot9;
    }
    const u64 prepared = PreparedFor(job.game);
    if (prepared == 0 || !SlotIsSound(prepared)) {
        return kErrIdIsApp; // (R0: never 3DS Link's id, with or without its low byte; never an id out of the band)
    }
    if (in.buf == nullptr || in.work == nullptr || in.composed == nullptr) {
        return kErrNoMemory;
    }
    Guest before;
    const bool had = FindRecord(prepared, &before);
    // (the build 2f) the form: the hidden one (kPreparedForm: M3c1 since DPAPP-2j), unless the system refused it earlier in this session and the
    // fallback is on - then the old one at once, never for a child already in the hidden form (AM took that one: its remake stays in it).
    // DPAPP-2j: the fallback is off, and so is the memo (a child is never put in HOME by a refusal of another game)
    const bool refused_before = kFallbackToApplicationForm && in.hidden_refused != nullptr && in.hidden_refused->load();
    u8 form = kPreparedForm;
    bool fell_back = false; // the child ends in the old form because the system refused the hidden one: the job says so (kErrShownInHome)
    if (refused_before && form != kFormApplication && !HiddenTakenBefore(had, before)) {
        form = kFormApplication;
        fell_back = true; // (the review of the build 2f: it ended with 0, "Ready", and HOME showed the black icon unsaid)
        c.Step("hidden form skipped: refused earlier in this session", prepared, 0);
    }
    c.Step("form", AmTitleFor(prepared, form), static_cast<RunResult>(form));
    // (the build 2h) what AM lists at the three ids of the slot, then the console's ids read again and judged (R1-R5, R8), BEFORE anything is written
    Forms fs;
    const bool am_said = LookForms(c, prepared, &fs);
    c.Step("prepared title before", AmTitleFor(prepared, form), static_cast<RunResult>(fs.at[form]));
    for (const u8 other : kForms) {
        if (other != form) {
            c.Step("prepared title before, other form", AmTitleFor(prepared, other), static_cast<RunResult>(fs.at[other]));
        }
    }
    if (!am_said) {
        return kErrInstall; // (AM said nothing: nothing is installed over a title nobody knows of)
    }
    Ownership own = InstallOwnership(fs, form, had, before);
    {
        IdCensus live;
        const int rc = ReadLive(c, in, prepared, "the record", &live);
        if (rc != 0) {
            return rc;
        }
        const LiveSlot slot = SlotIn(live, prepared);
        const IdTaken t = JudgeInstall(slot, form, own);
        c.Note(("guest: ids of " + HexText(prepared) + ": " + SlotText(slot) + " - " + TakenWord(t)).c_str());
        if (t != IdTaken::Free) {
            return TakenError(t); // a title, a ticket, a process that is not ours: never installed over, never next to it (K9.1, R2-R5)
        }
    }
    // (the old child stays until the new one is there: the console is never left without one - a cut too: Guest::prev_other, StartAct::RevertOther)
    const bool migrating = had && before.form != form;
    Found f;
    int rc = OpenChild(c, job, &f);
    if (rc != 0) {
        return rc;
    }
    note->route = f.route;
    const u64 cia_size = f.pkg.required_size;
    u64 free = 0;
    if (c.FreeBytes(&free) && free < 2 * cia_size + kSpaceSlack) {
        c.CloseContent();
        c.Step("free space", prepared, static_cast<RunResult>(free >> 20));
        return kErrNoSpace;
    }

    // The title becomes OURS here, before AM is asked anything: a cut from now on is found by the start (SweepAtStart).
    Guest g = had ? before : Guest();
    g.game = job.game;
    g.prepared = prepared;
    g.child = f.child; // (a record names its child: the package's, until the composer reads its TMD)
    g.where = job.card ? 'C' : 'S';
    g.form = form;
    g.state = GuestState::Preparing;
    g.prev_installed = had && before.state == GuestState::Installed && before.form == form && fs.at[form] == Presence::Present;
    g.prev_version = g.prev_installed ? fs.version[form] : 0;
    // (the review of the build 2f) a migration: the old child, in its form, is said in the record - a start after a cut goes back to it
    g.prev_other = migrating && before.state == GuestState::Installed && fs.at[before.form] == Presence::Present;
    g.prev_other_version = g.prev_other ? fs.version[before.form] : 0;
    g.prev_other_form = g.prev_other ? before.form : static_cast<u8>(kFormApplication);
    // (the build 2h) the ticket of the id written: the guard let the run go on only with none there or one that is ours - it is ours from here
    SayTicketOurs(g); // (DPAPP-2h-sec: with the stamp of this console - the ticket is in its ticket.db)
    if (!SaveGuest(g)) {
        c.CloseContent();
        return kErrTemp;
    }
    // (the review of the build 2f) registered in the form the console HAS until the new one is there: the jump and the Join ask AM by FormOf, and
    // a migration under way still has only the old child (SetForm below, once the new one is listed)
    if (!Register(prepared, migrating ? before.form : form)) {
        DropGuest(prepared);
        c.CloseContent();
        return kErrTooMany;
    }
    const Guest* const undo = had ? &before : nullptr;

    // The child, composed into the work file and installed.
    u8 smdh[kSmdhSize];
    char product[kProductBytes] = {};
    // (the review of the build 2h, R3) InstallOverHigher may delete our child of the form only when it is ours by R6 (its code read and the record's,
    // or Broken): a listed title whose code does not read is installed over as ever, never deleted first
    bool ticket_left = false; // (DPAPP-2h-sec) a ticket of ours the last install may have left at its id (UndoLeft)
    Took took = ComposeAndInstall(job, in, c, f, g, form, own, g.prev_installed && OursToDelete(fs, form, before), fs.version[form], smdh, product, &rc,
                                  &ticket_left);
    if (took == Took::Done) {
        // (the build 2h) where AM put it: the NAND is looked at (read only) - a child the system put there is never deleted, never fallen back from:
        // the record goes as it was and the job says so (kErrPutInNand; the NAND's id then refuses every write of the slot, R2)
        IdCensus after;
        c.ReadIds(&after);
        const bool nand_read = after.src[kSrcNand].read; // (the review of the build 2h, R5: a NAND not read is never "absent")
        const LiveSlot slot = SlotIn(after, prepared);
        const bool in_nand = (slot.mask[form] & (SrcBit(kSrcNand) | SrcBit(kSrcPendingNand))) != 0;
        c.Step(in_nand ? "NAND after the install: PRESENT" : (nand_read ? "NAND after the install: absent" : "NAND after the install: NOT READ"),
               AmTitleFor(prepared, form), in_nand || !nand_read ? -1 : 0);
        c.Step((slot.mask[form] & SrcBit(kSrcTicket)) != 0 ? "ticket after the install: present" : "ticket after the install: absent",
               AmTitleFor(prepared, form), 0);
        if (in_nand) {
            Undo(undo, prepared);
            return kErrPutInNand;
        }
        u16 sd_version = 0;
        const Presence on_sd = nand_read ? Presence::Present : c.Installed(prepared, form, &sd_version);
        if (on_sd == Presence::Absent || on_sd == Presence::Unknown) {
            // (the review of the build 2h, R5) not on the SD card, and the NAND not read: where AM put it is not known - no fallback (it would
            // install the old form next to a copy that may be in the NAND), no delete, the record as it was; the next look reads the console again
            c.Step("not on the SD card, the NAND not read: nothing more done", AmTitleFor(prepared, form), -1);
            Undo(undo, prepared);
            return kErrInstallCheck;
        }
    }
    if (took == Took::Done && form != kFormApplication) {
        // (the build 2f) where AM put it: the SD card, as asked. Not there: refused
        u16 sd_version = 0;
        const Presence sd = c.Installed(prepared, form, &sd_version);
        c.Step("media after the install: SD", AmTitleFor(prepared, form), static_cast<RunResult>(sd));
        if (sd == Presence::Absent) {
            c.Step("not on the SD card: where AM put it is not looked at", AmTitleFor(prepared, form), -1);
            c.CancelInstall(prepared, form);
            Wrote(c, in);
            // (DPAPP-2j, R-D) AM answered 0 to the Finish and does not list the title: nothing says AM refused the form - no memo, no fallback, said.
            // The ticket AM wrote with it goes (AM lists no title at the id: it is the run's), or the record says it (UndoLeft, F4)
            ticket_left = !DropLeftTicket(c, in, g, form, "install not on the SD card: the ticket it left dropped");
            took = Took::Failed;
            rc = kErrInstallCheck;
        }
    }
    if (took == Took::Refused && form != kFormApplication && !HiddenTakenBefore(had, before)) {
        // The system did not take the hidden form (a hidden child of this title that it took before is not this case: a Finish that fails over it
        // is a fault of the moment, kErrInstall as ever). D2: the old form - it works, HOME shows it as a black icon, and the job says so.
        c.Step("hidden form not taken", AmTitleFor(prepared, form), rc);
        u64 free_now = 0;
        const bool no_space = c.FreeBytes(&free_now) && free_now < 2 * cia_size + kSpaceSlack; // (DPAPP-2h-sec: a full card is no refusal of the form)
        if (kFallbackToApplicationForm && in.hidden_refused != nullptr && !no_space) {
            in.hidden_refused->store(true); // (DPAPP-2j: only with the fallback on - off, every game tries the hidden form itself)
        }
        if (fs.at[form] == Presence::Absent) {
            // (the review of the build 2f) no hidden title of this id was there: what AM may have kept of the refused one - its ticket, which
            // CancelInstall leaves - goes (the title, the pending title, the ticket: it was not there before, R7). Behind the guard (the build 2h)
            const RunResult dropped = DeleteGuarded(c, in, g, form, true, true, "hidden form not taken: its ticket dropped");
            (void)dropped;
        }
        if (no_space) {
            c.Step("hidden form not taken: the SD card is full, no fallback", prepared, static_cast<RunResult>(free_now >> 20));
            UndoLeft(undo, g, form, ticket_left); // (the system form's ticket, if it could not be dropped: said, never forgotten)
            return kErrNoSpace;
        }
        if (!kFallbackToApplicationForm && OldFormStays(undo, fs)) {
            // (DPAPP-2j-rev) the Update of a guest HOME shows: AM refused the hidden one, the old guest stays as it was (its record put back) - said
            // as it is (kErrShownInHome: it works, HOME shows it), never "not installed"
            UndoLeft(undo, g, form, ticket_left);
            return kErrShownInHome;
        }
        if (!kFallbackToApplicationForm) {
            // (DPAPP-2j) no child in HOME: the record as it was, and a ticket the refused install may have left said, never forgotten (F4)
            UndoLeft(undo, g, form, ticket_left);
            return kErrCannotHide;
        }
        if (migrating && before.form == kFormApplication && before.state == GuestState::Installed && fs.at[kFormApplication] == Presence::Present &&
            fs.version[kFormApplication] == before.child_version && SameChild(before, *in.composed)) {
            // our child of the old form is there, and it is this very child: kept as it is, nothing made again; composer kComposerVersion, so that
            // it is not made again at every look (D4). Only the form the fallback goes to (the build 2h: a child of M3c1 is never kept)
            Guest kept = before;
            kept.composer = kComposerVersion;
            kept.form = kFormApplication;
            const bool saved = SaveGuest(kept);
            SetForm(prepared, kFormApplication);
            c.Step("fallback: the old form kept", prepared, saved ? 0 : -1);
            return saved ? kErrShownInHome : kErrTemp;
        }
        form = kFormApplication;
        g.form = form;
        g.prev_installed = had && before.state == GuestState::Installed && before.form == form && fs.at[form] == Presence::Present;
        g.prev_version = g.prev_installed ? fs.version[form] : 0;
        // (the old child of another form - M3c1 - stays the migration's old child; the old form's own is prev_installed now)
        g.prev_other = had && before.form != form && before.state == GuestState::Installed && fs.at[before.form] == Presence::Present;
        g.prev_other_version = g.prev_other ? fs.version[before.form] : 0;
        g.prev_other_form = g.prev_other ? before.form : static_cast<u8>(kFormApplication);
        // (DPAPP-2h-sec) the word on the ticket was the system form's: the old form's id is judged by its own guard (ComposeAndInstall), and until then
        // the record says nothing of its ticket - a cut before it never drops a ticket at 00040000|low that was never looked at
        g.ticket_known = false;
        g.ticket_ours = false;
        g.console = 0;
        if (!SaveGuest(g)) {
            Undo(undo, prepared);
            return kErrTemp;
        }
        c.Step("fallback: made again in the old form", AmTitleFor(prepared, form), 0);
        Found again;
        rc = OpenChild(c, job, &again);
        if (rc != 0) {
            Undo(undo, prepared);
            return rc;
        }
        f = again;
        own = InstallOwnership(fs, form, had, before);
        took = ComposeAndInstall(job, in, c, f, g, form, own, g.prev_installed && OursToDelete(fs, form, before), fs.version[form], smdh, product, &rc,
                                 &ticket_left);
        fell_back = true;
    }
    if (took != Took::Done) {
        UndoLeft(undo, g, form, ticket_left);
        return took == Took::Refused ? kErrInstall : rc;
    }
    SetForm(prepared, form); // (AM took the child in `form`: the jump, the Join and the line of HOME's icon below ask AM by it from now on)
    const Composed& made = *in.composed;
    u16 version = 0;
    const Presence now = c.Installed(prepared, form, &version);
    c.Step("title version", AmTitleFor(prepared, form), now == Presence::Present ? static_cast<RunResult>(version) : -1);
    if (now == Presence::Present) {
        // (the build 2e, a diagnostic only; only when AM lists the title) right after AM's commit, before the record, the folder and the block: what
        // {tid, SD}, the way HOME is believed to use (FBI's), reads of the new title when HOME learns of it - nothing of ours written in between
        c.HomeIcon(prepared);
    }
    // From here the title is the new child: the record says so whatever comes next (a folder or a block that fails is a Repair of the next look).
    // (the review of the build 2h, R2) written Installed HERE, before the old form of a migration goes and before the folders: a cut after AM's
    // Finish never takes the new child away at the start (RevertOther, TakeAway) - what is left of the old form is a Repair of the next look
    // (DropOtherForm), its folder too
    g.child = made.tmd.tid;
    g.child_version = made.tmd.version;
    std::memcpy(g.child_hash8, made.tmd.hash, sizeof(g.child_hash8));
    g.transfer = f.pkg.transfer_size;
    g.required = f.pkg.required_size;
    std::memcpy(g.product, product, sizeof(g.product));
    g.game_version = job.game_version;
    g.update_version = job.update_version;
    g.composer = kComposerVersion;
    g.language = in.language;
    g.variant = kHide;
    g.form = form;
    g.prev_installed = false;
    g.prev_version = 0;
    g.prev_other = false;
    g.prev_other_version = 0;
    g.prev_other_form = kFormApplication;
    SayTicketOurs(g);
    g.state = GuestState::Installed;
    const bool saved = SaveGuest(g);
    SetForm(prepared, form);
    if (saved && migrating && form != before.form && now == Presence::Present) {
        // (DPAPP-2h-sec) only when the record says the new child: a record that was not written is still the migration's (Preparing, prev_other) -
        // with the old form there the start goes back to it (RevertOther); without it TakeAway would take the new child too. What is left is a Repair
        // (the build 2f) the migration: the old form goes only now, after the new one is there - and only our child, behind the guard of the ids
        // (R6, the build 2h: its product code read and the record's, or a Broken title whose code does not read; another code, or a listed title
        // whose code does not read, is kept and said)
        Forms old;
        old.at[before.form] = c.Installed(prepared, before.form, &old.version[before.form]);
        if (Listed(old.at[before.form])) {
            old.read[before.form] = c.Product(prepared, before.form, old.product[before.form]);
            if (old.read[before.form] && OtherProduct(old.product[before.form], before.product)) {
                c.Step("old form not ours: kept", AmTitleFor(prepared, before.form), 0);
            } else if (!OursToDelete(old, before.form, before)) {
                c.Step("old form unverified: kept", AmTitleFor(prepared, before.form), 0);
            } else {
                DeleteGuarded(c, in, before, before.form, true, TicketMayGo(before, before.form, true),
                              before.form == kFormSystem ? "old form removed (the system form of the build 2h)" : "old form removed (HOME showed it)");
            }
        }
    }

    // (the build 2h) the folder of the process's id of the form (the system id 00040010|low: the folder Rosalina loads), then the old form's folder
    // when the migration changed the process's id - after the new one, never before
    const bool folder = c.Folder(ProcessIdFor(prepared, form), job.game, true);
    c.Step("plugin folder", ProcessIdFor(prepared, form), folder ? 0 : -1);
    if (migrating && folder && ProcessIdFor(prepared, before.form) != ProcessIdFor(prepared, form)) {
        const bool old_folder = c.Folder(ProcessIdFor(prepared, before.form), job.game, false);
        c.Step("old plugin folder removed", ProcessIdFor(prepared, before.form), old_folder ? 0 : -1);
    }
    const bool block = WriteBlock(prepared, smdh, kSmdhSize, in.language, g.transfer, g.required);
    c.Step("block", prepared, block ? 0 : -1);
    note->card_checked = job.card;
    ChildSeen seen;
    seen.game = job.game;
    seen.game_version = job.game_version;
    seen.update_version = job.update_version;
    seen.has_child = true;
    seen.child = g.child;
    seen.child_version = g.child_version;
    std::memcpy(seen.child_hash8, g.child_hash8, sizeof(seen.child_hash8));
    seen.transfer = g.transfer;
    seen.required = g.required;
    std::memcpy(seen.product, g.product, sizeof(seen.product));
    Remember(seen);
    const bool index = RewriteIndex(in.waiting);
    c.Step("record and index", prepared, saved && index ? 0 : -1);
    if (now != Presence::Present || version != g.child_version) {
        return kErrInstallCheck;
    }
    if (!folder) {
        return kErrPlugin;
    }
    if (!saved || !block) {
        return kErrTemp;
    }
    return fell_back ? kErrShownInHome : 0; // (the fallback: the child works, HOME shows it - the sheet of the game says so)
}

// The plugin's copies, the block and the .part go; the record last, and only when the folders went (otherwise it stays, as Removing: the next look
// finishes it). `title_gone` says whether the title is gone too (or was never ours to delete).
int DropRest(Guest g, bool folder, bool title_gone, const RunIn& in, WorkConsole& c) {
    std::remove(BlockPath(g.prepared).c_str());
    std::remove(PartPath(g.prepared).c_str());
    if (!folder || !title_gone) {
        g.state = GuestState::Removing;
        SaveGuest(g);
        return folder ? kErrInstall : kErrPlugin;
    }
    const bool dropped = DropGuest(g.prepared);
    Unregister(g.prepared);
    RewriteIndex(in.waiting);
    c.Step("record removed", g.prepared, dropped ? 0 : -1);
    return dropped ? 0 : kErrTemp;
}

// (the build 2h) the plugin's copies of the two process ids of a child (the system id's, the key's): a removal takes both away
bool BothFoldersOff(WorkConsole& c, const Guest& g) {
    const bool sys = c.Folder(ProcessIdFor(g.prepared, kFormSystem), g.game, false);
    const bool key = c.Folder(g.prepared, g.game, false);
    return sys && key;
}

int Disown(Guest g, const RunIn& in, WorkConsole& c) {
    // A title that is not ours any more: AM is not asked anything about it.
    const bool folder = BothFoldersOff(c, g);
    c.Step("disown: plugin folder", g.prepared, folder ? 0 : -1);
    return DropRest(g, folder, true, in, c);
}

int Remove(const Planned& job, const RunIn& in, WorkConsole& c) {
    const u64 prepared = PreparedFor(job.game);
    Guest g;
    if (!FindRecord(prepared, &g)) {
        return 0;
    }
    g.state = GuestState::Removing;
    if (!SaveGuest(g)) {
        return kErrTemp;
    }
    RewriteIndex(in.waiting); // (the row goes before the title: the plugin never names a title that is going)
    const bool folder = BothFoldersOff(c, g);
    c.Step("remove: plugin folder", ProcessIdFor(prepared, g.form), folder ? 0 : -1);
    const u64 am = AmTitleFor(prepared, g.form); // (the build 2f: the id AM has the record's title by)
    Forms fs;
    fs.at[g.form] = c.Installed(prepared, g.form, &fs.version[g.form]);
    if (fs.at[g.form] == Presence::Unknown) {
        return DropRest(g, folder, false, in, c); // (AM said nothing: the record stays, as Removing)
    }
    bool title_gone = true;
    int refused = 0;
    if (fs.at[g.form] != Presence::Absent) {
        fs.read[g.form] = c.Product(prepared, g.form, fs.product[g.form]);
        if (fs.read[g.form] && OtherProduct(fs.product[g.form], g.product)) {
            c.Step("remove: not ours any more", am, 0); // never deleted: another title holds the id now
        } else if (!OursToDelete(fs, g.form, g)) {
            c.Step("remove: title unverified: kept", am, 0); // (R6: a listed title whose code does not read is never deleted on the record alone)
            title_gone = false;
        } else {
            const RunResult rc = DeleteGuarded(c, in, g, g.form, true, TicketMayGo(g, g.form, true), "remove: title");
            refused = rc >= kErrFirst && rc <= kErrLast ? static_cast<int>(rc) : 0;
            fs.at[g.form] = c.Installed(prepared, g.form, &fs.version[g.form]);
            title_gone = fs.at[g.form] == Presence::Absent;
            if (title_gone && TicketMayGo(g, g.form, true)) {
                // (DPAPP-2h-sec, second pass) the title went and its ticket was asked to go with it: the record goes only once no ticket is left at
                // the id (AM's list not read in the delete, or AM kept it: DeletePrepared's kTicketLeft) - dropped here, or the record stays (Removing)
                title_gone = DropLeftTicket(c, in, g, g.form, "remove: the ticket of the title deleted dropped");
            }
        }
    } else if (TicketWordHere(g) && g.ticket_ours) {
        // (the review of the build 2h, R1) the title is gone (a run given up, deleted by hand): the ticket our record says is ours goes with the
        // record - otherwise the next child of the slot would find "a ticket that is not ours" for ever. Not dropped: the record stays (Removing)
        title_gone = DropLeftTicket(c, in, g, g.form, "remove: the ticket left with no title dropped");
    } else if (TicketWordUnplaced(g)) {
        // (DPAPP-2h-sec, second pass) the word is of the builds before, or this console's stamp is not known: no ticket dropped on it, and the record
        // goes only when no ticket is at the id - a ticket there keeps it (Removing) and the page says why (326)
        const Presence ticket = TicketAtId(c, g, g.form, "remove");
        title_gone = ticket == Presence::Absent;
        refused = ticket == Presence::Present ? kErrIdTicket : 0;
    }
    title_gone = DropOtherForm(c, in, g, "remove: other form") && title_gone; // (the half a migration that was cut left, when it is ours)
    const int dropped = DropRest(g, folder, title_gone, in, c);
    return dropped != 0 && refused != 0 ? refused : dropped; // (the guard's reason, when it kept the title: the page says it)
}

int Adopt(const Planned& job, const RunIn& in, WorkConsole& c) {
    const u64 prepared = PreparedFor(job.game);
    if (prepared == 0 || !SlotIsSound(prepared)) {
        return kErrIdIsApp;
    }
    const std::vector<ChildSeen> cache = LoadChildrenCache();
    const ChildSeen* s = FindChildSeen(cache, job.game, job.game_version, job.update_version);
    // (the build 2h) the form AM lists, in the order of kForms (DPAPP-2j: M3c1 first, then the old one, then the system one)
    u8 form = kPreparedForm;
    u16 version = 0;
    Presence at = Presence::Absent;
    for (const u8 x : kForms) {
        form = x;
        at = c.Installed(prepared, form, &version);
        if (at != Presence::Absent) {
            break;
        }
    }
    char product[kProductBytes] = {};
    if (s == nullptr || !s->has_child || at != Presence::Present || !c.Product(prepared, form, product) || s->product[0] == '\0' ||
        std::strncmp(product, s->product, kProductCompared) != 0 || version != s->child_version) {
        return kErrIdTaken; // (not the child any more: never taken)
    }
    {
        // (the build 2h) adopted only with no id of the slot in the NAND, in a process, on the card (nothing written to AM here: the record only)
        IdCensus live;
        const int rc = ReadLive(c, in, prepared, "the adoption", &live);
        if (rc != 0) {
            return rc;
        }
        const LiveSlot slot = SlotIn(live, prepared);
        for (const u8 x : kForms) {
            if (slot.mask[x] & (SrcBit(kSrcNand) | SrcBit(kSrcPendingNand))) {
                return kErrIdSystem;
            }
            if (slot.mask[x] & SrcBit(kSrcProcess)) {
                return kErrIdRunning;
            }
            if (slot.mask[x] & SrcBit(kSrcCard)) {
                return kErrIdTaken;
            }
        }
    }
    Guest g;
    g.game = job.game;
    g.child = s->child;
    g.prepared = prepared;
    g.where = job.card ? 'C' : 'S';
    g.game_version = job.game_version;
    g.update_version = job.update_version;
    g.child_version = version;
    std::memcpy(g.child_hash8, s->child_hash8, sizeof(g.child_hash8));
    g.transfer = s->transfer;
    g.required = s->required;
    std::memcpy(g.product, s->product, sizeof(g.product));
    g.composer = 0; // (nobody here saw how it was made: the next look prepares it again, in the system form)
    g.language = in.language;
    g.variant = kHide;
    g.form = form;
    g.ticket_known = false; // (whose its ticket is: the legacy rule, TicketMayGo)
    g.state = GuestState::Installed;
    if (!SaveGuest(g)) {
        return kErrTemp;
    }
    if (!Register(prepared, form)) {
        DropGuest(prepared);
        return kErrTooMany;
    }
    c.Step("adopted", AmTitleFor(prepared, form), 0);
    RewriteIndex(in.waiting);
    return 0;
}

int Repair(const Planned& job, const RunIn& in, WorkConsole& c) {
    const u64 prepared = PreparedFor(job.game);
    Guest g;
    if (!FindRecord(prepared, &g)) {
        return 0;
    }
    const bool folder = c.Folder(ProcessIdFor(prepared, g.form), job.game, true); // (the build 2h: the folder of the form's process id)
    c.Step("repair: plugin folder", ProcessIdFor(prepared, g.form), folder ? 0 : -1);
    DropOtherForm(c, in, g, "repair: other form removed"); // (the build 2f: the half a migration that was cut left; never a title that is not ours)
    Found f;
    int rc = OpenChild(c, job, &f);
    bool block = false;
    if (rc == 0) {
        u8 smdh[kSmdhSize];
        const bool read = ReadContentAt(&c, f.pkg.smdh_at, smdh, kSmdhSize);
        c.CloseContent();
        block = read && WriteBlock(prepared, smdh, sizeof(smdh), in.language, g.transfer, g.required);
        c.Step("repair: block", prepared, block ? 0 : -1);
    }
    if (block) {
        g.language = in.language;
    }
    g.where = job.card ? 'C' : 'S';
    const bool saved = SaveGuest(g);
    RewriteIndex(in.waiting);
    if (rc != 0) {
        return rc;
    }
    return !folder ? kErrPlugin : (block && saved ? 0 : kErrTemp);
}

} // namespace

u32 ChildLowOfName(const char* name) {
    if (name == nullptr || std::strlen(name) != 20 || std::strncmp(name, "00040001", 8) != 0) {
        return 0;
    }
    const char* ext = name + 16;
    if (!(ext[0] == '.' && (ext[1] == 'c' || ext[1] == 'C') && (ext[2] == 'i' || ext[2] == 'I') && (ext[3] == 'a' || ext[3] == 'A'))) {
        return 0;
    }
    u32 low = 0;
    for (int i = 8; i < 16; ++i) {
        const char ch = name[i];
        const int d = ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1));
        if (d < 0) {
            return 0;
        }
        low = (low << 4) | static_cast<u32>(d);
    }
    return low;
}

bool RewriteIndex(const std::vector<Waiting>& waiting) {
    return WriteIndex(IndexText(LoadGuests(), waiting, nullptr), false);
}

int RunJob(const Planned& job, const RunIn& in, WorkConsole& console, JobNote* note) {
    JobNote ignored;
    JobNote* n = note != nullptr ? note : &ignored;
    switch (job.act) {
    case Act::Probe: return Probe(job, in, console);
    case Act::Check: return Check(job, in, console, n);
    case Act::Prepare: return Prepare(job, in, console, n);
    case Act::Remove: return Remove(job, in, console);
    case Act::Adopt: return Adopt(job, in, console);
    case Act::Repair: return Repair(job, in, console);
    case Act::Disown: {
        Guest g;
        return FindRecord(PreparedFor(job.game), &g) ? Disown(g, in, console) : 0;
    }
    default: return 0;
    }
}

} // namespace dlpguest
