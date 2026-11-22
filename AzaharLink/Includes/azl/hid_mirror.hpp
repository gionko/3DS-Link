// The input mirror page: while the chat's keyboard is open over the RUNNING game, the game's HID page (the one physical page of the hid
// module, 0x2B0 bytes used, mapped read-only in every process) is replaced, at the game's own address, by a page of the plugin's heap that
// says "no key, no Circle Pad, no touch" with the live headers, accelerometer and gyroscope of the real one. The plugin still reads the real
// input through the framework's own mapping of the page (CTRPluginFramework calls hidInit itself: another address, the same page).
// The pure parts (Tests/hid_mirror_test.cpp drives them on the PC): the page's layout and its neutral copy, the look at the real page that
// says whether keys are still held, the wait for the release, the masks of what is still down when that wait ends at its cap, the search
// for the game's addresses of the page, and the switch and its way
// back over an abstract kernel (`Ops`): the console's side, with the svcs, is Sources/ctr/input_mirror.cpp.
//
// Why each step (AzaharLink/analysis/chat-in-game-2026-11-04: STUDIO-CHAT sections 2, 11 and 12, DECISIONI "Pagina specchio"):
// - only the game's addresses: the framework's own (hidSharedMem, the same physical page) is excluded, or the plugin would read the
//   neutral page too; its address must be among the pages found, or the search is not what it thinks it is (no switch);
// - the kernel coalesces a block with a neighbour of the same state and permissions: the mirror is mapped shared (0x5806) read-write, so
//   a neighbour that is shared and writable refuses the switch, the mirror must come out as exactly one page at the address (base, size),
//   and the address must be free after the real page is unmapped (and after the mirror is, on the way back);
// - the game's threads are held (Luma3DS's PROCESSOP_SCHEDULE_THREADS) while the address is empty: a read of it then is a data abort.
//   The hold is asynchronous on the other core and skips the thread that owns the kernel's synchronization mutex, so it is asked twice
//   with a pause after each, and the predicate counts the threads it was asked about: every thread of the process, once or twice (the
//   second pass of the svc for the threads running on another core). A predicate that was never called (an emulator) refuses the switch;
// - the way back never fails into the void when it can help it: the real page is mapped twice before giving up, and then, except at the
//   swap of the plugin's block (Old 3DS) and at the exit, the mirror goes back (neutral input rather than a data abort). At the swap the
//   mirror's memory is freed right after the answer, so the address is left empty instead;
// - the mirror is an alias of the plugin's memory (svc 0xA0, state 0x5806), and such an alias left mapped OUTLIVES the game's process: its
//   page stays taken in the APPLICATION region until a reboot (proved on console 63, 2026-11-06: AzaharLink/analysis/
//   friend_a-crash-uscita-gioco-2026-11-06/ANALISI.md, "Prove decisive"; the reading of the decompiled kernel that said KProcess::Finalize
//   lets it go was wrong). Every change is still made only by the lock's holder: at the game's last event the keep-alive thread waits for
//   the lock (Sources/ctr/input_mirror.cpp, MirrorProcessEvent), and a lock not had there leaves the mirror mapped, said in the log.
// No heap, no wait, no line of the log here: the way back runs on a game's thread too (the hook, when a notification takes the game away).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "azl/common.hpp"

namespace azl {
namespace hidmirror {

// >>> game-thread code (Tests/policy_check.sh: the way back runs on the game's thread that got a notification: no heap, no line)

// ---- The page (libctru hid.c, hidScanInput; 3dbrew "HID Shared Memory") -------------------------------------------------------
constexpr u32 kPageBytes = 0x1000;     ///< the page the kernel maps
constexpr u32 kHidBytes = 0x2B0;       ///< what the hid module writes in it
constexpr u32 kEntries = 8;            ///< every ring of the page
constexpr u32 kPadIndex = 0x10;        ///< the pad's last entry (its two ticks before it, at 0x00 and 0x08)
constexpr u32 kPadRaw = 0x1C;          ///< the pad's raw state ...
constexpr u32 kPadRawCircle = 0x20;    ///< ... and the Circle Pad's raw position (s16 x, s16 y): neutral in the mirror, not looked at by PageQuiet
constexpr u32 kPadEntries = 0x28;      ///< 8 entries of 0x10: held, pressed, released, Circle Pad (s16 x, s16 y)
constexpr u32 kPadEntryStep = 0x10;
constexpr u32 kPadHeld = 0x0, kPadPressed = 0x4, kPadReleased = 0x8, kPadCircle = 0xC;
constexpr u32 kTouchHeader = 0xA8;     ///< the touch screen's two ticks and its index (at 0xB8)
constexpr u32 kTouchIndex = 0xB8;
constexpr u32 kTouchRaw = 0xC0;        ///< raw touch (u16 x, u16 y, u32 flag)
constexpr u32 kTouchEntries = 0xC8;    ///< 8 entries of 8: u16 x, u16 y, u32 flag at +4
constexpr u32 kTouchEntryStep = 8;
constexpr u32 kTouchFlag = 4;
constexpr u32 kAccel = 0x108;          ///< the accelerometer's section (then the gyroscope's at 0x158 and the rest to 0x2B0): copied live
constexpr u32 kGyro = 0x158;
constexpr u32 kTouchEnd = kTouchEntries + kEntries * kTouchEntryStep; ///< = kAccel
static_assert(kTouchEnd == kAccel, "the touch's entries end where the accelerometer begins");
static_assert(kPadEntries + kEntries * kPadEntryStep == kTouchHeader, "the pad's entries end where the touch's header begins");

constexpr u32 kReleasedSamples = 2; ///< the mirror's entries say "released" for the keys the game saw held, this many refreshes
constexpr u32 kQuietMs = 50;        ///< the game gets its page back after this long with no key, no touch and the Circle Pad at rest ...
constexpr u32 kMaxWaitMs = 1000;    ///< ... or after this long whatever is held
constexpr s32 kCircleRest = 40;     ///< a Circle Pad within this of the centre is at rest (a pushed one reads 100-156)

inline u32 Load32(const u8* p, u32 off) {
    u32 v;
    std::memcpy(&v, p + off, 4);
    return v;
}
inline void Store32(u8* p, u32 off, u32 v) {
    std::memcpy(p + off, &v, 4);
}
inline s32 LoadS16(const u8* p, u32 off) {
    std::int16_t v;
    std::memcpy(&v, p + off, 2);
    return v;
}

/// The keys the game last saw held: the pad's entry at the page's index (clamped to 7, as libctru does).
inline u32 HeldNow(const u8* real) {
    u32 i = Load32(real, kPadIndex);
    if (i > kEntries - 1) {
        i = kEntries - 1;
    }
    return Load32(real, kPadEntries + i * kPadEntryStep + kPadHeld);
}

/// The neutral parts of the mirror: the pad's raw state and raw Circle Pad, every pad entry {held 0, pressed 0, released `released`,
/// Circle Pad 0}, the raw touch and every touch entry (the flag too: a zero entry with the flag set is a finger at 0,0).
inline void Neutralize(u8* m, u32 released) {
    Store32(m, kPadRaw, 0);
    Store32(m, kPadRawCircle, 0);
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 e = kPadEntries + i * kPadEntryStep;
        Store32(m, e + kPadHeld, 0);
        Store32(m, e + kPadPressed, 0);
        Store32(m, e + kPadReleased, released);
        Store32(m, e + kPadCircle, 0);
    }
    std::memset(m + kTouchRaw, 0, kTouchEnd - kTouchRaw);
}

/// The mirror made from the real page: all of it, then the neutral parts.
inline void PrepareMirror(u8* m, const u8* real, u32 released) {
    std::memcpy(m, real, kPageBytes);
    Neutralize(m, released);
}

/// A frame's refresh: the live parts copied again - the accelerometer, the gyroscope and the rest of the page, the word between the raw
/// Circle Pad and the entries, then the headers (ticks and index of the pad and of the touch screen) last, so that an index never points
/// at an entry older than itself - and the neutral entries written again with `released`.
inline void RefreshMirror(u8* m, const u8* real, u32 released) {
    std::memcpy(m + kAccel, real + kAccel, kHidBytes - kAccel);
    std::memcpy(m + kPadRawCircle + 4, real + kPadRawCircle + 4, kPadEntries - (kPadRawCircle + 4));
    Neutralize(m, released);
    std::memcpy(m, real, kPadRaw);
    std::memcpy(m + kTouchHeader, real + kTouchHeader, kTouchRaw - kTouchHeader);
}

/// The "released" of the mirror's entries, refresh after refresh: the keys the game saw held at the switch for kReleasedSamples, then 0
/// (a reader that takes the release from the entry, not from its own edge, sees the keys go up once).
class Released {
public:
    void Start(u32 keys) {
        keys_ = keys;
        left_ = kReleasedSamples;
    }
    /// What the first refresh writes is what PrepareMirror wrote: Start's keys.
    u32 Next() {
        if (left_ == 0) {
            return 0;
        }
        --left_;
        return keys_;
    }

private:
    u32 keys_ = 0;
    u32 left_ = 0;
};

/// What keeps the real page from being quiet (Unquiet), one bit a condition: PageQuiet is "none of them", and the debug log's line of a
/// wait for the release that ended at its cap says which (Sources/ctr/input_mirror.cpp).
constexpr u32 kUnquietKey = 1;      ///< a key held in one of the 8 pad entries
constexpr u32 kUnquietCircle = 2;   ///< the Circle Pad off its rest in one of them
constexpr u32 kUnquietTouch = 4;    ///< a finger in one of the 8 touch entries
constexpr u32 kUnquietRawTouch = 8; ///< the raw touch's flag (at 0xC4)

/// The conditions of PageQuiet that the real page does not meet (0: quiet): the 8 pad entries (the last ~34 ms at the hid module's 234 Hz)
/// for a key or a Circle Pad off its rest, the 8 touch entries and the raw touch for a finger.
/// The pad's raw words at 0x1C and 0x20 are NOT looked at (2026-11-10): with them the page was never quiet on a console - of 144 closes of the
/// keyboard in the logs of console 63, 121 gave the game its input back only at the 1 s cap and none by the 50 ms of quiet - and what those
/// two words hold at rest on hardware is not known (0x20 is the Circle Pad before its calibration; an emulator leaves both at 0, which is
/// why the PC never saw it). The entries are the hid module's own decoded state, the one the game reads.
inline u32 Unquiet(const u8* real) {
    const auto rest = [](s32 v) { return v <= kCircleRest && v >= -kCircleRest; };
    u32 why = 0;
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 e = kPadEntries + i * kPadEntryStep;
        if (Load32(real, e + kPadHeld) != 0) {
            why |= kUnquietKey;
        }
        if (!rest(LoadS16(real, e + kPadCircle)) || !rest(LoadS16(real, e + kPadCircle + 2))) {
            why |= kUnquietCircle;
        }
        if (Load32(real, kTouchEntries + i * kTouchEntryStep + kTouchFlag) != 0) {
            why |= kUnquietTouch;
        }
    }
    if (Load32(real, kTouchRaw + kTouchFlag) != 0) {
        why |= kUnquietRawTouch;
    }
    return why;
}

/// The real page says nothing is held: no key and the Circle Pad at rest in any of the 8 pad entries, and no finger in any touch entry
/// nor in the raw touch.
inline bool PageQuiet(const u8* real) {
    return Unquiet(real) == 0;
}

/// The keyboard closed: the game gets its page back once the real page was quiet for kQuietMs (looked at every frame), or kMaxWaitMs
/// after the close whatever is held. A clock that wraps is fine (differences only), and so is a `now_ms` a little OLDER than the one of
/// Start or of the look before (2026-11-10: the close took the clock again while the frame's look still had its own, read 1-2 ms earlier:
/// the difference without a sign was 0xFFFFFFFF, "over the cap", and the page went back in the same frame with the key that closed the
/// keyboard still held - 23 closes of 144 on console 63). Both differences are compared WITH their sign, on both sides: kMaxWaitMs and
/// kQuietMs are u32, and a signed difference against an unsigned constant is an unsigned comparison again.
class ReleaseWait {
public:
    void Start(u32 now_ms) {
        on_ = true;
        start_ = now_ms;
        quiet_ = false;
        capped_ = false;
    }
    void Cancel() {
        on_ = false;
    }
    bool waiting() const {
        return on_;
    }
    /// True once: the page goes back now.
    bool Update(bool quiet, u32 now_ms) {
        if (!on_) {
            return false;
        }
        if (!quiet) {
            quiet_ = false;
        } else if (!quiet_) {
            quiet_ = true;
            quiet_since_ = now_ms;
        }
        const bool rested = quiet_ && static_cast<s32>(now_ms - quiet_since_) >= static_cast<s32>(kQuietMs);
        const bool capped = static_cast<s32>(now_ms - start_) >= static_cast<s32>(kMaxWaitMs);
        if (rested || capped) {
            on_ = false;
            capped_ = !rested;
            return true;
        }
        return false;
    }
    /// The wait that just ended (Update said true) ended at its cap, the page never quiet for kQuietMs: what the debug log tells apart.
    bool capped() const {
        return capped_;
    }

private:
    bool on_ = false;
    bool quiet_ = false;
    bool capped_ = false;
    u32 start_ = 0;
    u32 quiet_since_ = 0;
};

// ---- What is still down when the wait ends at its cap (2026-11-13) --------------------------------------------------------------
// Console 63, Final Fantasy Explorers with four: the keyboard closed with B, and 1 s later - B still held, the Circle Pad pushed - the
// page went back at the cap; the game took that B and left the mission. A wait that ends at its cap with something down no longer gives
// the page back: the mirror stays, and from then on it is the REAL page without what is still down from the close -
// - each key (held, pressed and released, in the 8 entries and in the raw word) until the 8 entries no longer mention it;
// - the Circle Pad, centred, and its four direction keys, until it is at rest in the 8 entries (or kCircleMaskMaxMs: a pad that never
//   comes back to its centre is not centred for ever);
// - the touch, absent, until no entry and no raw flag has the pen down.
// Everything else is the real page's: a key pressed AFTER the cap is in the mirror at the plugin's next frame. A mask only shrinks. The
// page goes back when the last one falls (Sources/ctr/input_mirror.cpp, MaskFrame); until then the mirror is written with the masked
// values directly, never with the real ones first: the game may read it between any two stores.
constexpr u32 kCirclePadKeys = 0xF0000000u; ///< the Circle Pad as four keys (bits 28-31: right, left, up, down): masked with it
constexpr u32 kCircleMaskMaxMs = 10000;     ///< the Circle Pad's mask falls after this long even off its rest
constexpr u32 kMaskCircle = 1;              ///< HeldMask::what(): the Circle Pad is centred for the game
constexpr u32 kMaskTouch = 2;               ///< ... the touch is absent

class HeldMask {
public:
    /// The wait ended at its cap: everything the real page's entries still mention is masked, and the mirror `m` is written without it.
    /// False (and off, `m` untouched) when they mention nothing.
    bool Start(u8* m, const u8* real, u32 now_ms) {
        const Seen s = Look(real);
        keys_ = s.circle ? s.keys & ~kCirclePadKeys : s.keys; // (the pad's direction keys are its mask's while it is on)
        what_ = (s.circle ? kMaskCircle : 0u) | (s.touch ? kMaskTouch : 0u);
        circle_capped_ = false;
        start_ = now_ms;
        last_ms_ = now_ms;
        frames_ = 0;
        gap_max_ = 0;
        on_ = any();
        if (on_) {
            Write(m, real);
        }
        return on_;
    }
    /// The keyboard opened again, or the page went back by another way (HOME, sleep, the menu, the exit): nothing is masked.
    void Clear() {
        on_ = false;
        keys_ = 0;
        what_ = 0;
    }
    /// On from Start to Clear - also with nothing left to mask, while the page could not go back yet: the mirror is then the real
    /// page's copy, frame after frame.
    bool on() const {
        return on_;
    }
    u32 keys() const {
        return keys_;
    }
    u32 what() const {
        return what_;
    }
    bool any() const {
        return keys_ != 0 || what_ != 0;
    }
    /// The Circle Pad's mask fell at kCircleMaskMaxMs, the pad still off its rest (the log's line says so).
    bool circle_capped() const {
        return circle_capped_;
    }
    u32 since(u32 now_ms) const {
        return now_ms - start_;
    }
    /// How the mirror was kept since Start (the debug log's line): the frames that wrote it, and the longest time between two - what a
    /// key pressed or let go meanwhile waited for the plugin's thread before the game could see it.
    u32 frames() const {
        return frames_;
    }
    u32 longest_gap_ms() const {
        return gap_max_;
    }
    /// A frame: what was let go since the last look leaves the mask, then the mirror is written. False: nothing is masked any more.
    bool Frame(u8* m, const u8* real, u32 now_ms) {
        const s32 gap = static_cast<s32>(now_ms - last_ms_);
        if (gap > static_cast<s32>(gap_max_)) {
            gap_max_ = static_cast<u32>(gap);
        }
        last_ms_ = now_ms;
        ++frames_;
        const Seen s = Look(real);
        if ((what_ & kMaskCircle) != 0) {
            if (!s.circle) {
                what_ &= ~kMaskCircle;
                keys_ |= s.keys & kCirclePadKeys; // (a direction key the entries still mention - its release - waits for them as any key)
            } else if (static_cast<s32>(now_ms - start_) >= static_cast<s32>(kCircleMaskMaxMs)) {
                what_ &= ~kMaskCircle; // (given up whole: the pad and its direction keys are the real page's from here)
                circle_capped_ = true;
            }
        }
        if (!s.touch) {
            what_ &= ~kMaskTouch;
        }
        keys_ &= s.keys;
        Write(m, real);
        return any();
    }

private:
    struct Seen {
        u32 keys = 0;        ///< every key an entry mentions: held, pressed or released
        bool circle = false; ///< the Circle Pad off its rest in an entry
        bool touch = false;  ///< the pen down in an entry or in the raw touch
    };
    static Seen Look(const u8* real) {
        const auto rest = [](s32 v) { return v <= kCircleRest && v >= -kCircleRest; };
        Seen s;
        for (u32 i = 0; i < kEntries; ++i) {
            const u32 e = kPadEntries + i * kPadEntryStep;
            s.keys |= Load32(real, e + kPadHeld) | Load32(real, e + kPadPressed) | Load32(real, e + kPadReleased);
            if (!rest(LoadS16(real, e + kPadCircle)) || !rest(LoadS16(real, e + kPadCircle + 2))) {
                s.circle = true;
            }
            if (Load32(real, kTouchEntries + i * kTouchEntryStep + kTouchFlag) != 0) {
                s.touch = true;
            }
        }
        if (Load32(real, kTouchRaw + kTouchFlag) != 0) {
            s.touch = true;
        }
        return s;
    }
    // The mirror from the real page. The keys, the Circle Pad and the touch are written a word at a time, already without what is masked;
    // `live` copies what is never masked. The two headers (the ticks and the index of the pad's ring and of the touch's) are TAKEN first
    // and written last (the review, 2026-11-13): the hid module adds a sample every 4.3 ms whenever it likes, and this thread - the lowest
    // of the process - can be taken off the CPU for longer than that anywhere in here. Read from the real page AFTER the entries, as the
    // neutral refresh does (there every entry says the same), an index that moved meanwhile points at an entry of the mirror copied
    // before its sample: the oldest of the ring, 34 ms old, shown to the game as the newest until the next frame - a key let go 30 ms ago
    // held again, one pressed since up again (Tests/hid_mirror_test.cpp, TestMaskSampleMidWrite). Taken first, the index is never newer
    // than the entries copied after it; written last, the game never reads a new index over the entries of the frame before. The barrier
    // keeps the two reads where they are written, whatever the compiler knows about `real`.
    void Write(u8* m, const u8* real) const {
        const auto live = [m, real](u32 from, u32 to) { std::memcpy(m + from, real + from, to - from); };
        u8 pad_head[kPadRaw], touch_head[kTouchRaw - kTouchHeader];
        std::memcpy(pad_head, real, sizeof(pad_head));
        std::memcpy(touch_head, real + kTouchHeader, sizeof(touch_head));
        __asm__ __volatile__("" ::: "memory");
        const bool circle = (what_ & kMaskCircle) != 0;
        const u32 keep = ~(keys_ | (circle ? kCirclePadKeys : 0u));
        const u32 touch_bytes = kTouchEnd - kTouchRaw;
        live(kAccel, kHidBytes);
        live(kPadRawCircle + 4, kPadEntries);
        Store32(m, kPadRaw, Load32(real, kPadRaw) & keep);
        Store32(m, kPadRawCircle, circle ? 0u : Load32(real, kPadRawCircle));
        for (u32 i = 0; i < kEntries; ++i) {
            const u32 e = kPadEntries + i * kPadEntryStep;
            Store32(m, e + kPadHeld, Load32(real, e + kPadHeld) & keep);
            Store32(m, e + kPadPressed, Load32(real, e + kPadPressed) & keep);
            Store32(m, e + kPadReleased, Load32(real, e + kPadReleased) & keep);
            Store32(m, e + kPadCircle, circle ? 0u : Load32(real, e + kPadCircle));
        }
        if ((what_ & kMaskTouch) != 0) {
            std::memset(m + kTouchRaw, 0, touch_bytes);
        } else {
            live(kTouchRaw, kTouchEnd);
        }
        std::memcpy(m, pad_head, sizeof(pad_head));
        std::memcpy(m + kTouchHeader, touch_head, sizeof(touch_head));
    }

    bool on_ = false;
    bool circle_capped_ = false;
    u32 keys_ = 0;
    u32 what_ = 0;
    u32 start_ = 0;
    u32 last_ms_ = 0;
    u32 frames_ = 0;
    u32 gap_max_ = 0;
};

// ---- The address space (svcQueryMemory's MemInfo; libctru's MemState and MemPerm values) --------------------------------------
constexpr u32 kStateFree = 0;
constexpr u32 kStateShared = 6; ///< what svcQueryMemory says of a block of state 0x5806 (the hid page, and the mirror once mapped)
constexpr u32 kPermR = 1, kPermW = 2;
constexpr u32 kSearchBegin = 0x10000000; ///< where shared memory blocks are mapped (svcMapMemoryBlock takes 0x08000000-0x14000000)
constexpr u32 kSearchEnd = 0x14000000;
constexpr u32 kMaxVas = 2;               ///< the game's mappings of the page this switches (a game that maps it twice)
constexpr u32 kMaxPagesLooked = 2048;    ///< the search asks the physical address of at most this many pages

struct Region {
    u32 base = 0, size = 0, perm = 0, state = 0;
};

inline bool Contains(const Region& r, u32 va) {
    return va >= r.base && va - r.base < r.size;
}
inline bool IsFreeAt(const Region& r, u32 va) {
    return r.state == kStateFree && Contains(r, va);
}
inline bool IsSharedAt(const Region& r, u32 va) {
    return r.state == kStateShared && Contains(r, va);
}
/// A neighbour the kernel could fold the mirror into (shared and writable, as the mirror is).
inline bool NeighbourMerges(const Region& r) {
    return r.state == kStateShared && (r.perm & kPermW) != 0;
}
/// Two of the game's addresses side by side: read-only now, but the second mirror would be folded into the first (both shared and
/// writable), and the way back would unmap one page of the folded block.
template <class FoundT>
inline bool NextToAnother(const FoundT& f, u32 i) {
    for (u32 j = 0; j < f.n; ++j) {
        if (j != i && (f.va[j] + kPageBytes == f.va[i] || f.va[i] + kPageBytes == f.va[j])) {
            return true;
        }
    }
    return false;
}
/// The mirror as it must be after the map: one block, exactly the page, shared and writable.
inline bool MappedExactly(const Region& r, u32 va) {
    return r.base == va && r.size == kPageBytes && r.state == kStateShared && (r.perm & (kPermR | kPermW)) == (kPermR | kPermW);
}

/// The hold of the game's threads saw them all: `calls` of the predicate in one pass of the svc against the process's `threads` (once each,
/// or twice when a thread was running on another core). With a list that was full (`threads` == `cap`) at least that many.
inline bool FreezeSeenAll(u32 calls, s32 threads, s32 cap) {
    if (threads <= 0) {
        return false;
    }
    const u32 n = static_cast<u32>(threads);
    if (threads >= cap) {
        return calls >= n;
    }
    return calls == n || calls == 2 * n;
}

// The hold's predicate: Sources/ctr/input_mirror.cpp's azl_mirror_thread_predicate is this, inlined. It runs in the KERNEL, inside Luma3DS's
// PROCESSOP_SCHEDULE_THREADS (ControlProcess.c), once per thread of the process and pass; true: hold this thread (lock), let it go (unlock).
// Two words of the kernel's thread, both of Luma3DS's own KThread (k11_extension/include/kernel.h: one layout for the Old and the New 3DS):
// - +0x94 threadLocalStorage, the TLS's USER address: what the scheduler writes to TPIDRURO, so what getThreadLocalStorage() returned on
//   the thread that calls the svc (the caller's value). Luma3DS's lock holds the current thread too (its second pass), so the caller is
//   left out by this word; the other one (+0x98) is never equal to it, and the game's thread that gives the page back from the hook (no
//   mark) would hold itself and stay held with every other thread of the game;
// - +0x98 threadLocalStorageKernel, the kernel's alias of the same page (PA + 0xC0000000): the word Luma3DS's own ThreadPredicate reads
//   for its swap (plgloader.c, kthread[0x26]). The mark is read through it: libctru's "!TV$" (THREADVARS_MAGIC) at the start of the TLS,
//   which the plugin's threads carry (Luma3DS holds them by it at the Old 3DS's swap) and the game's (the SDK's) do not.
// `off` not 0 (the framework's pause holds the game, Process::Pause): no thread is ours to hold or let go.
constexpr u32 kThreadVarsMagic = 0x21545624;
constexpr u32 kKThreadUserTlsWord = 0x25; ///< +0x94: the TLS's user address (TPIDRURO)
constexpr u32 kKThreadTlsWord = 0x26;     ///< +0x98: the kernel's alias of the TLS page

__attribute__((always_inline)) inline bool HoldThread(const u32* kthread, u32 caller_tls, u32 off) {
    const u32 tls = kthread[kKThreadTlsWord];
    if (tls == 0 || kthread[kKThreadUserTlsWord] == caller_tls) {
        return false;
    }
    if (off != 0) {
        return false;
    }
    return *reinterpret_cast<const volatile u32*>(static_cast<uintptr_t>(tls)) != kThreadVarsMagic;
}

enum class Outcome : u32 {
    kDone = 0,
    kBusy,             ///< another thread is switching or giving back
    kUnsupported,      ///< an emulator, or a system without Luma3DS's svcs
    kNoMemory,         ///< no page for the mirror
    kNotFound,         ///< no mapping of the page in the game
    kOwnNotFound,      ///< the framework's own mapping is not among the pages of its physical address: the search is wrong
    kTooMany,          ///< more mappings than kMaxVas
    kFreezeFailed,     ///< the svc that holds the threads failed
    kFreezeUnverified, ///< its predicate did not see every thread of the process
    kNotShared,        ///< the game's address is no longer a shared block
    kNeighbour,        ///< a neighbour is shared and writable: the kernel could fold the mirror into it
    kQueryFailed,
    kUnmapFailed,      ///< the real page could not be unmapped (nothing changed)
    kNotFree,          ///< the address was not free after an unmap
    kMapFailed,        ///< the mirror could not be mapped (the real page went back)
    kNotExact,         ///< the mirror did not come out as exactly one page (it stays, known: never unmapped folded)
    kCancelled,        ///< the keyboard closed (HOME, power, sleep, exit) before the switch
    kFellBack,         ///< the real page could not be mapped back: the mirror is there again (the next try later)
    kLeftEmpty,        ///< the real page could not be mapped back and the mirror may not stay (swap, exit): the address is empty
    kStuck,            ///< the mirror could not be unmapped: it is still there
};

inline const char* OutcomeText(Outcome o) {
    static const char* const kText[] = {"done",
                                        "busy",
                                        "not on this system",
                                        "no memory",
                                        "the game has no mapping of the page",
                                        "the framework's own mapping is not among the page's",
                                        "more mappings than expected",
                                        "the game's threads could not be held",
                                        "the hold did not see every thread",
                                        "the game's address is not a shared block",
                                        "a writable shared neighbour",
                                        "a query failed",
                                        "the real page could not be unmapped",
                                        "the address was not free after the unmap",
                                        "the mirror could not be mapped",
                                        "the mirror is not exactly one page",
                                        "the keyboard closed first",
                                        "the real page could not be mapped back: the mirror stays",
                                        "the real page could not be mapped back: the address is EMPTY",
                                        "the mirror could not be unmapped"};
    const u32 i = static_cast<u32>(o);
    return i < sizeof(kText) / sizeof(kText[0]) ? kText[i] : "?";
}

/// The game's mappings of the page: every page of the shared blocks of [kSearchBegin, kSearchEnd) whose physical address is `hid_pa`, the
/// framework's own (`own_va`) left out and noted. `query(va, Region*)` and `pa_of(va)` are the kernel's (svcQueryMemory, svcConvertVAToPA).
struct Found {
    u32 va[kMaxVas] = {};
    u32 perm[kMaxVas] = {};
    u32 n = 0;
    bool own_seen = false;
    bool too_many = false;
};
template <class Query, class PaOf>
inline Found FindHidVas(Query query, PaOf pa_of, u32 hid_pa, u32 own_va) {
    Found f;
    u32 looked = 0;
    u32 va = kSearchBegin;
    while (va < kSearchEnd && looked < kMaxPagesLooked) {
        Region r;
        if (!query(va, &r) || r.size == 0 || !Contains(r, va)) {
            break;
        }
        if (r.state == kStateShared) {
            // (from the address the walk is at, never below the search: a block may begin under kSearchBegin, folded with one above it)
            for (u32 p = va & ~(kPageBytes - 1); p - r.base < r.size && p < kSearchEnd && looked < kMaxPagesLooked; p += kPageBytes) {
                ++looked;
                if (pa_of(p) != hid_pa) {
                    continue;
                }
                if (p == own_va) {
                    f.own_seen = true;
                } else if (f.n < kMaxVas) {
                    f.va[f.n] = p;
                    f.perm[f.n] = r.perm;
                    ++f.n;
                } else {
                    f.too_many = true;
                }
            }
        }
        const u32 next = r.base + r.size;
        if (next <= va) {
            break; // (a block that ends at the top of the address space)
        }
        va = next;
    }
    return f;
}

/// The lock between the switch (the plugin's thread), the way back on a game's thread (a try only: it never waits) and the way back at the
/// framework's events (a bounded wait). No kernel object: a word.
class TryMutex {
public:
    bool TryLock() {
        u32 expected = 0;
        return word_.compare_exchange_strong(expected, 1u, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void Unlock() {
        word_.store(0, std::memory_order_release);
    }

private:
    std::atomic<u32> word_{0};
};

/// The switch and the way back, over the kernel's calls (`Ops`):
///   bool Query(u32 va, Region* out)      svcQueryMemory
///   s32 Freeze(u32* calls)               one pass of the hold of the game's threads; `calls`: the predicate's calls in it
///   void Settle()                        the pause after a pass (the interrupt to the other core)
///   s32 Threads()                        the number of the process's threads (GetProcessInfo 7, never svcGetThreadList; Cap() the most trusted)
///   void Unfreeze()                      the game's threads go again
///   s32 UnmapReal(u32 va), MapReal(u32 va, u32 perm)       the hid module's block at the game's address
///   s32 MapMirror(u32 va), UnmapMirror(u32 va)             the mirror at the game's address
/// The caller holds the TryMutex. Not a thread of its own: whoever holds the lock runs it.
class Switcher {
public:
    bool active() const {
        return n_ != 0;
    }
    u32 count() const {
        return n_;
    }
    u32 va(u32 i) const {
        return va_[i];
    }

    template <class Ops>
    Outcome Engage(Ops& ops, const Found& f) {
        if (active()) {
            return Outcome::kDone;
        }
        if (f.n == 0 || f.n > kMaxVas) {
            return Outcome::kNotFound;
        }
        if (!Hold(ops, true)) {
            ops.Unfreeze();
            return last_hold_;
        }
        Outcome out = Outcome::kDone;
        // Every address checked before anything changes: its block, and the neighbours the mirror could be folded into.
        for (u32 i = 0; i < f.n && out == Outcome::kDone; ++i) {
            Region r, below, above;
            if (!ops.Query(f.va[i], &r) || !ops.Query(f.va[i] - 1, &below) || !ops.Query(f.va[i] + kPageBytes, &above)) {
                out = Outcome::kQueryFailed;
            } else if (!IsSharedAt(r, f.va[i])) {
                out = Outcome::kNotShared;
            } else if (NeighbourMerges(below) || NeighbourMerges(above) || NextToAnother(f, i)) {
                out = Outcome::kNeighbour;
            }
        }
        for (u32 i = 0; i < f.n && out == Outcome::kDone; ++i) {
            const u32 va = f.va[i], perm = f.perm[i];
            if (ops.UnmapReal(va) != 0) {
                out = Outcome::kUnmapFailed;
                break;
            }
            Region r;
            if (!ops.Query(va, &r) || !IsFreeAt(r, va)) {
                out = Outcome::kNotFree;
                if (!(ops.Query(va, &r) && IsSharedAt(r, va))) {
                    Track(PutBack(ops, va, perm, false, true), va, perm); // (whatever is there, the real page again if it can)
                }
                break;
            }
            if (ops.MapMirror(va) != 0) {
                out = Outcome::kMapFailed;
                Track(PutBack(ops, va, perm, false, true), va, perm);
                break;
            }
            if (!ops.Query(va, &r) || !MappedExactly(r, va)) {
                out = Outcome::kNotExact;
                Track(PutBack(ops, va, perm, true, true), va, perm); // (folded: stuck, kept in the list)
                break;
            }
            va_[n_] = va;
            perm_[n_] = perm;
            ++n_;
        }
        if (out != Outcome::kDone) {
            GiveBack(ops, true); // (the ones switched already; one that keeps the mirror stays in the list: active() says so)
        }
        ops.Unfreeze();
        return out;
    }

    /// The real page back at every address. `may_fall_back`: when it cannot be mapped, the mirror goes back (false at the swap and at the
    /// exit: the address is left empty). The game's threads are held for it even when the hold fails (the page must go back).
    template <class Ops>
    Outcome Restore(Ops& ops, bool may_fall_back) {
        if (!active()) {
            return Outcome::kDone;
        }
        Hold(ops, false);
        const Outcome out = GiveBack(ops, may_fall_back);
        ops.Unfreeze();
        return out;
    }

    u32 left_empty() const {
        return left_empty_;
    }

private:
    // Two passes of the hold, a pause after each. Checked (`verify`): the svc said yes and its predicate saw every thread.
    template <class Ops>
    bool Hold(Ops& ops, bool verify) {
        u32 calls = 0;
        const s32 a = ops.Freeze(&calls);
        ops.Settle();
        calls = 0;
        const s32 b = ops.Freeze(&calls);
        ops.Settle();
        if (!verify) {
            return true;
        }
        if (a != 0 || b != 0) {
            last_hold_ = Outcome::kFreezeFailed;
            return false;
        }
        if (!FreezeSeenAll(calls, ops.Threads(), ops.Cap())) {
            last_hold_ = Outcome::kFreezeUnverified;
            return false;
        }
        return true;
    }

    // One address: the mirror off it (`mirror_there`), the address free, the real page on it (twice), else the mirror again or nothing.
    template <class Ops>
    Outcome PutBack(Ops& ops, u32 va, u32 perm, bool mirror_there, bool may_fall_back) {
        Region r;
        if (mirror_there) {
            // Only a block that is exactly the mirror's page is unmapped: the kernel's unmap of one page of a block folded with a writable
            // shared one the game mapped beside it frees from the address over the whole folded size (folded below: past the block,
            // kernelpanic()) or does nothing (folded above). Such a mirror stays, known: the next way back looks again. An address found
            // free (unmapped by a way back whose query after it failed) gets its real page.
            if (!ops.Query(va, &r)) {
                return Outcome::kStuck;
            }
            if (!IsFreeAt(r, va)) {
                if (!MappedExactly(r, va) || ops.UnmapMirror(va) != 0) {
                    return Outcome::kStuck;
                }
                if (!ops.Query(va, &r) || !IsFreeAt(r, va)) {
                    return Outcome::kStuck; // (not free: nothing more is mapped over it)
                }
            }
        }
        for (u32 attempt = 0; attempt < 2; ++attempt) {
            if (ops.MapReal(va, perm) == 0 && ops.Query(va, &r) && IsSharedAt(r, va)) {
                return Outcome::kDone;
            }
            ops.Settle();
        }
        if (may_fall_back && ops.MapMirror(va) == 0) {
            return Outcome::kFellBack;
        }
        ++left_empty_;
        return Outcome::kLeftEmpty;
    }

    // An address the way back left with the mirror (fell back, stuck) is one to give back later.
    void Track(Outcome o, u32 va, u32 perm) {
        if ((o == Outcome::kFellBack || o == Outcome::kStuck) && n_ < kMaxVas) {
            va_[n_] = va;
            perm_[n_] = perm;
            ++n_;
        }
    }

    // Every address switched, in turn; the ones that stay mirrored (fell back, stuck) stay in the list.
    template <class Ops>
    Outcome GiveBack(Ops& ops, bool may_fall_back) {
        Outcome worst = Outcome::kDone;
        u32 kept = 0;
        for (u32 i = 0; i < n_; ++i) {
            const Outcome o = PutBack(ops, va_[i], perm_[i], true, may_fall_back);
            if (o == Outcome::kFellBack || o == Outcome::kStuck) {
                va_[kept] = va_[i];
                perm_[kept] = perm_[i];
                ++kept;
            }
            if (o != Outcome::kDone && (worst == Outcome::kDone || o == Outcome::kLeftEmpty)) {
                worst = o;
            }
        }
        n_ = kept;
        return worst;
    }

    u32 va_[kMaxVas] = {};
    u32 perm_[kMaxVas] = {};
    u32 n_ = 0;
    u32 left_empty_ = 0;
    Outcome last_hold_ = Outcome::kDone;
};

// <<< game-thread code

} // namespace hidmirror
} // namespace azl
