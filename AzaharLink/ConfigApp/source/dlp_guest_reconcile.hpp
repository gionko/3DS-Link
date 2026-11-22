// The reconciliation of the Download Play children the app prepares (1.1.4, piece APP-WORKER: 6-APP-boot9-CRITICO.md 4.3 and 6 steps 5-6, K9 of
// 7-CONTRATTO.md; spec 9 of 2026-11-17). The work is decided by STATE, never hooked on an event: when the Download Play page opens the .guest records
// are looked at against the games there are now (dlp_guest.cpp LookAround fills a Seen per game) and Decide answers for each. This file sorts the
// answers into what runs without a question (quiet: a removal, a look at content 2, a repair, a few KB each) and what the user starts in Download
// Play > Games (heavy: a child to prepare, minutes with HOME off), keeps the app to kMaxGuests children, says how long the heavy part takes and how far
// a run is, which games the reading of the background looks at (ToRead), which failures of a look are remembered (FailureKept) and what a game's row
// of Download Play > Games shows (ShownOf). Pure, header only: Tests/settings_test/dlp_guest_run_test.cpp holds every rule.
#pragma once

#include <cstddef>
#include <vector>

#include "dlp_guest_compose.hpp"
#include "dlp_guest_decide.hpp"
#include "dlp_guest_logic.hpp"

namespace dlpguest {

/// A game as LookAround saw it, and the size of its child's CIA when children.cache knows it (0 = not known).
struct Look {
    Seen seen;
    u32 bytes = 0;
};

/// What the worker is given for a game.
struct Planned {
    u64 game = 0;
    Act act = Act::Nothing;
    bool card = false;
    u16 game_version = 0, update_version = 0;
    u32 bytes = 0;
};

struct Plan {
    std::vector<Planned> quiet; ///< runs at once, no question (HOME and sleep held by its page, DPAPP-2h-sec): Remove and Disown first (they free space and room), Adopt, Repair, Check, Probe
    std::vector<Planned> heavy; ///< Prepare: only ever started by the user, in Download Play > Games ("to update", or a game switched on)
    std::vector<Planned> said;  ///< Refuse and Full: nothing runs, Download Play > Games says why when the game is switched on
};

/// A record's game the scan did not list (dlp_guest.cpp LookAround) is still on the SD card when AM's list of the SD card holds it, or when that list
/// could not be read (`sd` null): the scan leaves installed games out (no readable SMDH when the system refuses the read and there are no keys, a cache
/// that missed after a change of language or an update, a title AM cannot describe). Only a game AM does not list is gone, and its child removed.
inline bool StillOnSd(const std::vector<u64>* sd, u64 game) {
    if (sd == nullptr) {
        return true; // (AM gave no list: nothing is taken for gone on a failure)
    }
    for (u64 t : *sd) {
        if (t == game) {
            return true;
        }
    }
    return false;
}

/// The order of the quiet acts (lower first).
inline int QuietRank(Act a) {
    switch (a) {
    case Act::Remove:
    case Act::Disown: return 0;
    case Act::Adopt: return 1;
    case Act::Repair: return 2;
    case Act::Check: return 3;
    case Act::Probe: return 4;
    default: return 5;
    }
}

/// Decide for every game, sorted into the three lists. `records` is how many .guest records there are: a game with no record that Decide would give a
/// child (Prepare, Adopt) gets one only while records + those already given stay under kMaxGuests; past it, Full.
inline Plan Reconcile(const std::vector<Look>& looks, std::size_t records) {
    Plan plan;
    std::size_t given = 0;
    for (const Look& l : looks) {
        Act act = Decide(l.seen);
        if (l.seen.guest == nullptr && (act == Act::Prepare || act == Act::Adopt)) {
            if (records + given >= kMaxGuests) {
                act = Act::Full;
            } else {
                ++given;
            }
        }
        Planned p;
        p.game = l.seen.game;
        p.act = act;
        p.card = l.seen.card;
        p.game_version = l.seen.game_version;
        p.update_version = l.seen.update_version;
        p.bytes = l.bytes;
        switch (act) {
        case Act::Nothing: break;
        case Act::Prepare: plan.heavy.push_back(p); break;
        case Act::Refuse:
        case Act::Full: plan.said.push_back(p); break;
        default: plan.quiet.push_back(p); break;
        }
    }
    // (a stable sort by rank: the order of the scan stays within a rank)
    for (std::size_t i = 1; i < plan.quiet.size(); ++i) {
        for (std::size_t j = i; j > 0 && QuietRank(plan.quiet[j].act) < QuietRank(plan.quiet[j - 1].act); --j) {
            const Planned t = plan.quiet[j];
            plan.quiet[j] = plan.quiet[j - 1];
            plan.quiet[j - 1] = t;
        }
    }
    return plan;
}

// ---- the reading of the background and Download Play > Games (spec 9 points 4 and 5) ----------------------------------------------------------

/// The background reads (a Probe, no page, no question) a game that may have a child (setup::IsGuestGame), has no record (its child is known), and of
/// which children.cache knows nothing at its current versions: neither a fact (`fact`, FindChildSeen) nor a failure remembered with the stamp of the
/// boot9.bin there is now (`failure`, FindChildFailure: another stamp - boot9.bin came or changed - reads it again). `keys_stamp` is KeysStamp(). A child
/// route F found without the keys is written the same way (kErrNoBoot9, stamp 0): read again when boot9.bin comes, not at every start.
inline bool ToRead(bool guest_game, bool has_record, const ChildSeen* fact, const ChildSeen* failure, u32 keys_stamp) {
    return guest_game && !has_record && fact == nullptr && (failure == nullptr || failure->keys != keys_stamp);
}

/// Whether a failed look at content 2 is remembered in children.cache (spec 9 point 5: then it is not tried again until the version, or boot9.bin,
/// changes, or the user presses A on its row "not read"). What is not the game's is never remembered: no boot9.bin (the game stays "to read", not an
/// error), no movable.sed or the scan's context not there yet, no memory, a stop, a card taken out (`card` and no source). Everything else is.
inline bool FailureKept(int err, bool card) {
    switch (err) {
    case 0:
    case kErrNoChild:     // (not a failure: "no child" is a fact, written as such)
    case kErrNoBoot9:
    case kErrBoot9NoKeys:
    case kErrNoMovable:
    case kErrNoMemory:
    case kErrCancel: return false;
    case kErrNoSource: return !card;
    default: return true;
    }
}

/// What a game's row of Download Play > Games shows.
enum class Shown : u8 {
    Hidden,   ///< not listed: no child, never read yet (the row "Checking your games" counts it), a record being removed, a game that is not there
    Off,      ///< it has a child and the user has not switched it on: A prepares it
    On,       ///< its child is prepared (an Installed record): A removes it
    ToUpdate, ///< its record is there but the child is to be made again (the game was updated, the title taken away, a run that died): A makes it
    NotRead,  ///< the last look at its content 2 failed (FindChildFailure): grey, "not read", A reads it again (with a record, X removes its child)
};
/// `s` as LookAround sees the game (its record in s.guest), `failure` what children.cache remembers of a failed look at its versions, `act_on` what
/// Decide answers for it switched on (the record's next work: Prepare is "to update").
inline Shown ShownOf(const Seen& s, const ChildSeen* failure, Act act_on) {
    if (s.guest != nullptr) {
        if (s.guest->state == GuestState::Removing) {
            return Shown::Hidden;
        }
        if (s.guest->state == GuestState::Preparing || act_on == Act::Prepare) {
            return Shown::ToUpdate;
        }
        // (the review of spec 9) the game changed and the look at its new version failed: not "On" for ever, "not read" with the reason
        return s.present && failure != nullptr ? Shown::NotRead : Shown::On;
    }
    if (!s.present) {
        return Shown::Hidden;
    }
    if (s.child == ChildFact::Has) {
        return Shown::Off;
    }
    if (s.child == ChildFact::Unknown && failure != nullptr && failure->error == kErrNoBoot9) {
        return Shown::Off; // (route F found its child without boot9.bin: it has one; A says that boot9.bin is needed - spec 9 point 4.1)
    }
    return s.child == ChildFact::Unknown && failure != nullptr ? Shown::NotRead : Shown::Hidden;
}

// ---- time ------------------------------------------------------------------------------------------------------------------------------

constexpr u32 kUsualChildBytes = 32u << 20; ///< a child whose size is not known yet (Island Tour's is 30.9 MiB)
/// The low speeds of dlp::Speeds (MiB/s): AES 0.7, SHA-256 2.0, the SD card 3.0.
constexpr double kAesMiBs = 0.7, kShaMiBs = 2.0, kSdMiBs = 3.0;

/// Seconds to prepare a child of `bytes`: the read, the title key (CBC), the NCCH layer (CTR), two SHA-256, the work file written and read again, AM;
/// and the SD layer of route K when `keyed` (6-APP-boot9.md 8: about 2 min 40 s by the system, 3 min 25 s by the keys, for Island Tour's child).
inline u32 SecondsToPrepare(u64 bytes, bool keyed) {
    const double mib = static_cast<double>(bytes == 0 ? kUsualChildBytes : bytes) / 1048576.0;
    double s = mib / kSdMiBs + mib / kAesMiBs + mib / kAesMiBs + 2.0 * mib / kShaMiBs + 2.0 * mib / kSdMiBs + mib / kSdMiBs;
    if (keyed) {
        s += mib / kAesMiBs;
    }
    return static_cast<u32>(s + 0.5);
}

/// The heavy part, in whole minutes, rounded up: a game of the SD card is counted by route K (the longer: the system may refuse route F), a card by F.
inline u32 MinutesFor(const std::vector<Planned>& heavy) {
    u32 s = 0;
    for (const Planned& p : heavy) {
        s += SecondsToPrepare(p.bytes, !p.card);
    }
    return (s + 59) / 60;
}

/// How far one job is (0..1), from the composer's stages: the look before the stream, the stream (most of the time), AM, the end.
inline double JobFraction(int stage, u64 done, u64 total) {
    const double f = total == 0 ? 0.0 : (done >= total ? 1.0 : static_cast<double>(done) / static_cast<double>(total));
    switch (stage) {
    case kStageRead: return 0.02 * f;
    case kStageDecrypt: return 0.02 + 0.73 * f;
    case kStageInstall: return 0.75 + 0.23 * f;
    case kStageFinish: return 0.98 + 0.02 * f;
    default: return 0.0;
    }
}

/// How far a run of `count` jobs is when job `index` (0-based) is at JobFraction `job`: never 1 before the last job is over.
inline double RunFraction(int index, int count, double job) {
    if (count <= 0) {
        return 0.0;
    }
    const double f = (static_cast<double>(index) + (job < 0.0 ? 0.0 : (job > 1.0 ? 1.0 : job))) / static_cast<double>(count);
    return f > 0.99 ? 0.99 : f;
}

} // namespace dlpguest
