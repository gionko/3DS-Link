// What the app does about a game's Download Play child (1.1.4, piece APP-STORE: 6-APP-boot9.md 4.2 with its critic 6-APP-boot9-CRITICO.md 3 and 4, and
// K9 of 7-CONTRATTO.md; spec 9 of 2026-11-17). The work is decided by RECONCILIATION - the .guest records against the games there are now - when the
// Download Play page opens, and for the one game the user switches on in Download Play > Games (a game is "on" when the user switched it on there: its
// record; Enable Titles has nothing to do with it, spec 9 point 7): Decide answers for one game what is to be done, from what the caller read (the scan,
// AM, the records, children.cache); DecideAtStart answers for one record at the start. Pure: no file, no system call;
// Tests/settings_test/dlp_guest_store_test.cpp runs every branch.
#pragma once

#include <cstring>

#include "dlp_guest_store.hpp"

namespace dlpguest {

/// What the app knows of a game's content 2 at its current versions (children.cache).
enum class ChildFact : u8 {
    Unknown, ///< never looked at, at these versions
    None,    ///< looked at: no child in it
    Has,     ///< looked at: this child (Seen::child_*)
};

/// One game as the caller sees it now.
struct Seen {
    u64 game = 0;
    bool on = false;      ///< setup::IsGuestGame, and the user switched it on in Download Play > Games: its record is there (or it is the game the
                          ///< user is switching on now). Enable Titles has nothing to do with it (spec 9 point 7)
    bool present = false; ///< installed on the SD card, or its card is in the slot
    bool still_listed = false; ///< (not present, a game of the SD card) AM's list of the SD card still holds the game, or could not be read: the scan
                               ///< only did not recognise it (no readable SMDH, a cache that missed, a title AM cannot describe), it is not gone
    bool card = false;    ///< (present) it is the game card's
    bool card_checked = false; ///< (card) its child's TMD was read again this session (a card's version reads 0: the scan cannot tell revisions)
    u16 game_version = 0, update_version = 0;
    ChildFact child = ChildFact::Unknown;
    u64 child_tid = 0;
    u16 child_version = 0;
    char product[kProductBytes] = {}; ///< (Has) the child's product code
    const Guest* guest = nullptr;     ///< its record, if there is one that is ours (IsOwnedGuestTid: a record no registry slot was left for is not)
    bool prepared_installed = false;  ///< PreparedFor(game) is on the SD card in `prepared_form` (ours or not: Presence Present or Broken)
    u16 prepared_version = 0;
    char prepared_product[kProductBytes] = {}; ///< (installed) its product code (AM_GetTitleProductCode)
    /// (the build 2f) the form the three above are of: the record's; with no record the first one AM lists (kForms: the system form first since the
    /// build 2h), or kPreparedForm (dlp_guest.cpp)
    u8 prepared_form = kFormApplication;
    bool other_form_present = false;  ///< the id of ANOTHER form is on the SD card too (Present or Broken; the build 2h: three forms)
    /// ... and every such one is ours: its product code read, and the record's (no record: the child's, at the child's version) - the half of a
    /// migration that was cut, or of a fallback. Only then is it taken away (Repair); another title there is never touched (K9.1)
    bool other_form_ours = false;
    /// (the build 2h) the kErr* of an id of the slot that is somebody else's - a title of the NAND, a ticket, a process, the card, a title of the SD
    /// card (dlp_guest_ids.hpp JudgeInstall on ids.cache, for an install in kPreparedForm); 0: none. A game with no record is refused (never
    /// prepared, never adopted); the run judges the console again right before it writes, whatever this says
    int id_taken = 0;
    bool plugin_on = false;           ///< (guest) the prepared title's folder holds our plugin and nothing else
    bool block_ok = false;            ///< (guest) its block is there, whole
    u8 language = 0;                  ///< the console's language now
    bool keys = false;                ///< boot9.bin is there with its keys: without it the feature is off and nothing new is done
    bool am_known = true;             ///< AM answered about PreparedFor(game) (Presence is not Unknown): otherwise prepared_* say nothing
    bool look_failed = false;         ///< children.cache remembers a failed look at the game's current versions, made with the boot9.bin there is now
                                      ///< (FindChildFailure, its stamp): a Check is not made again at every opening of the page (spec 9 point 5)
    std::size_t guests = 0;           ///< the records there are now (LoadGuests): the app makes kMaxGuests at most
};

enum class Act : u8 {
    Nothing,
    Probe,   ///< look at content 2 (FindChildPackage) and write children.cache: no HOME held, no title written
    Prepare, ///< compose and install the child (a title that is ours: made again)
    Check,   ///< read the child's TMD again (another version of the game or its update, a card): the same child is only recorded, another is prepared
    Repair,  ///< the title is right: the plugin's folder, the block or the index is not
    Remove,  ///< take the title away (the game is off or gone): only ever a title of a .guest
    Adopt,   ///< a title of the band with no .guest that IS this child (same product code, same version): its .guest is written again
    Refuse,  ///< the id is taken by a title that is not ours: said with a message, NEVER deleted
    Disown,  ///< a .guest whose id now holds a title that is NOT ours (another product code): our record, block and plugin's copy go, the title
             ///< is NEVER touched (the game is then one with no record: Refuse)
    Full,    ///< a game that would get a child, but kMaxGuests records are there already: nothing is made (the page says so)
};

/// AM gives a product code of 15 characters at most (libctru writes it into 16 bytes with its NUL); the NCCH's has 16.
constexpr std::size_t kProductCompared = 15;
/// The title at a prepared id is another one than the record's: both product codes read (AM_GetTitleProductCode; the composer never changes the
/// NCCH's), and different. A code that could not be read proves nothing either way: the record stays the proof.
inline bool OtherProduct(const char* installed, const char* recorded) {
    return installed[0] != '\0' && recorded[0] != '\0' && std::strncmp(installed, recorded, kProductCompared) != 0;
}

/// The one decision about a game.
inline Act Decide(const Seen& s) {
    if (!s.am_known) {
        return Act::Nothing; // (AM said nothing of the prepared id: no title is installed over, deleted or adopted on a guess)
    }
    const Guest* g = s.guest;
    if (g != nullptr) {
        if (g->state == GuestState::Preparing) {
            return Act::Nothing; // (the worker is on it: the start settles a Preparing that died)
        }
        if (s.prepared_installed && OtherProduct(s.prepared_product, g->product)) {
            return Act::Disown; // the id holds a title that is not ours any more: never installed over, never deleted
        }
        if (g->state == GuestState::Removing) {
            return Act::Remove;
        }
        if (!s.on) {
            return Act::Remove; // no longer a game that can have a child (setup::IsGuestGame): its child goes
        }
        if (!s.present) {
            // a card that is out comes back; a game the scan did not recognise is still there (AM lists it); a game gone from the SD card is not
            return g->where == 'C' || s.still_listed ? Act::Nothing : Act::Remove;
        }
        if (!s.keys) {
            return Act::Nothing; // (no boot9.bin: what is there stays, nothing is made)
        }
        if (!s.prepared_installed || s.prepared_version != g->child_version) {
            return Act::Prepare; // the title was taken away (the Installation check, by hand) or replaced: made again, not an error
        }
        // (the build 2f: the other form's id of our child, left by a migration that was cut, goes too)
        const bool repair = (s.card ? 'C' : 'S') != g->where || !s.plugin_on || !s.block_ok || s.language != g->language || s.other_form_ours;
        if (g->composer < kComposerVersion) {
            // made by an older composer: made again when the user says so (Prepare, "to update"); until then the child that works is kept right
            // quietly - its folder, its block (the review of the build 2f: the children of composer 1 lost their Repairs). Not composer 0: the
            // mark of a child to make again (Revert, Adopt, a Check that found another child), whose block would be another child's
            return repair && g->composer != 0 ? Act::Repair : Act::Prepare;
        }
        if (s.game_version != g->game_version || s.update_version != g->update_version || (s.card && !s.card_checked)) {
            // (a Check that failed at these versions with this boot9.bin is not made again by itself: the row says "not read", A reads it again - the
            // review of spec 9: it was made again, quietly, at every opening of the page)
            return s.look_failed ? Act::Nothing : Act::Check;
        }
        if (repair) {
            return Act::Repair;
        }
        return Act::Nothing;
    }
    if (!s.on || !s.present || !s.keys) {
        return Act::Nothing;
    }
    if (s.child == ChildFact::Unknown) {
        return Act::Probe; // a game never looked at (at these versions)
    }
    if (s.child == ChildFact::None) {
        return Act::Nothing;
    }
    if (s.guests >= kMaxGuests) {
        return Act::Full; // (one record more would be a child the index has no row for)
    }
    if (s.id_taken != 0) {
        return Act::Refuse; // (the build 2h: an id of the slot is somebody else's - the NAND, a ticket, a process, the card: never written, never adopted)
    }
    if (s.prepared_installed) {
        const bool same = s.product[0] != '\0' && std::strncmp(s.product, s.prepared_product, kProductCompared) == 0 && s.prepared_version == s.child_version;
        // (the build 2f) the two ids: one that is not this child, on either, is never adopted over (Refuse)
        return same && (!s.other_form_present || s.other_form_ours) ? Act::Adopt : Act::Refuse;
    }
    if (s.other_form_present) {
        return Act::Refuse; // (LookAround fills prepared_* with the form it found: this is a title it could not take for the child)
    }
    return Act::Prepare;
}

/// What the start does with a record.
enum class StartAct : u8 {
    Keep,     ///< Installed: as it is (a title gone since is the next look's Prepare)
    Later,    ///< AM could not say whether the title is there: as it is (a title is never taken for gone on a failure of AM)
    Disown,   ///< the id holds a title that is not ours any more (another product code): folder, block and record go, the title is never touched
    Revert,   ///< Preparing over an Installed child that AM kept (the old version is there): Installed again, made again by the next look
    TakeAway, ///< a Preparing that died (or whose old child is gone), a Removing: folder, block, pending title, title and record go
    /// (the review of the build 2f) a migration that died (Guest::prev_other): its old child, in the other form, is there whole at its version and
    /// ours - Installed again in that form, made again by the next look; what the run made of the new form goes
    RevertOther,
    /// (the review of the build 2h, R2) a migration from the form M3c1 (never installed again, never jumped to) cut after AM's Finish: the new child,
    /// in the record's form, is there and ours - it is kept (Installed, composer 0: made again by the next look), never deleted to go back to M3c1
    KeepNew,
};
/// `at` and `version` are AM's (StartConsole::Installed), `product` the product code of the title at the id ("" when it could not be read).
/// `other_*`: the same of the id of the other form, asked only for a migration (Guest::prev_other); Absent otherwise.
inline StartAct DecideAtStart(const Guest& g, Presence at, u16 version, const char* product, Presence other_at = Presence::Absent, u16 other_version = 0,
                              const char* other_product = "") {
    if (g.state == GuestState::Installed) {
        return StartAct::Keep;
    }
    if (at == Presence::Unknown) {
        return StartAct::Later;
    }
    if (at != Presence::Absent && OtherProduct(product, g.product)) {
        return StartAct::Disown;
    }
    if (g.state == GuestState::Preparing && g.prev_other) {
        if (!Installable(g.prev_other_form) && at == Presence::Present && product[0] != '\0' && g.product[0] != '\0' && !OtherProduct(product, g.product)) {
            return StartAct::KeepNew; // (the review of the build 2h, R2: going back to M3c1 would leave the slot with no child that works)
        }
        if (other_at == Presence::Unknown) {
            return StartAct::Later; // (AM said nothing of the old child: nothing is taken away on a guess)
        }
        if (other_at == Presence::Present && other_version == g.prev_other_version && !OtherProduct(other_product, g.product)) {
            return StartAct::RevertOther; // (the review of the build 2f: a migration cut in the middle never takes the child that worked away)
        }
    }
    if (g.state == GuestState::Preparing && !g.prev_installed && g.product[0] == '\0' && at != Presence::Absent) {
        // (a first run cut before its record had a product code: whose title AM lists at the id - described or not (Broken) - nothing proves: never
        // deleted (K9.1). Ours, the next look adopts it (same code and version); another one is refused. The final review of 1.1.4, and its review)
        return StartAct::Disown;
    }
    if (g.state == GuestState::Preparing && g.prev_installed && at == Presence::Present && version == g.prev_version) {
        return StartAct::Revert;
    }
    return StartAct::TakeAway;
}

} // namespace dlpguest
