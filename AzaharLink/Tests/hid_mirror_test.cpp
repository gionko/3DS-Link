// The input mirror page, its pure parts (azl/hid_mirror.hpp; the console's side is Sources/ctr/input_mirror.cpp), run by Tests/run.sh with
// ASan/UBSan. What it holds:
// - the page: the mirror made from a real page byte by byte - the pad's raw state and raw Circle Pad zero, the 8 pad entries from 0x28 every
//   0x10 {held 0, pressed 0, released = the keys the game saw held, Circle Pad 0}, the raw touch and the 8 touch entries from 0xC8 every 8 zero
//   (the flag at +4 too), everything else (the headers with their ticks and indices at 0x10 and 0xB8, the accelerometer from 0x108, the
//   gyroscope from 0x158, the rest of the page) the real page's; a refresh copies the live parts again and nothing past 0x2B0; "released"
//   for 2 samples, then 0; the keys held now from the entry at the page's index (clamped to 7);
// - the look that says the real page is quiet (no key and the Circle Pad at rest in any entry, no finger; the pad's two raw words at 0x1C
//   and 0x20 NOT looked at, whatever they hold) and which condition is not met, and the wait for the release: 50 ms of quiet looked at
//   every frame, at most 1 s, a clock that wraps, a clock a little older than the wait's start (never "over the cap"), and whether it
//   ended at the cap or by the quiet;
// - what is still down when the wait ends at its cap (HeldMask): each key masked until the 8 entries no longer mention it, the Circle Pad
//   centred with its direction keys until it is at rest (10 s at most), the touch absent until the pen lifts, everything else the real
//   page's; and the plugin's frame over it against a hid module of 234 Hz: the console's case (B held 3 s past the close, the pad pushed,
//   a key pressed meanwhile), the normal path byte for byte as before, the pen, the keyboard opened again, HOME and the exit with the
//   masks on, a pad that never rests, a way back that has to wait; and a hid module that adds its samples from another thread WHILE the
//   mirror is written: the mirror's index never points at an entry older than itself (the headers taken first and written last); and
//   the keyboard asked for again with the masks on, never shown and closed (a close with no opening of the mirror before it);
// - the search of the game's mappings by physical address over a simulated address space (the framework's own excluded and seen; a page in
//   the middle of a shared block; two mappings; a third one too many);
// - the switch and the way back over a simulated kernel that folds neighbours of the same state and permissions as the real one does:
//   the threads held across every change and let go on every path, both passes of the hold checked (an emulator's predicate that is never
//   called, a failed svc), a writable shared neighbour below or above refuses, a read-only one does not, the mirror exactly one page or
//   the real page goes back, the address free after an unmap, a map of the mirror that fails, two mappings with the second failing (the first
//   given back), the real page mapped back on the second try, the mirror again when it cannot be (and never at the swap: the address left
//   empty), a mirror that cannot be unmapped stays known, a game address that is no longer shared;
// - the hold's predicate (HoldThread) inside a transcription of Luma3DS's PROCESSOP_SCHEDULE_THREADS loop over KThreads laid out as the
//   kernel lays them out (+0x94 the TLS's user address, +0x98 its kernel alias, a different number for the same page): the caller - a
//   game's thread from the hook, the plugin's thread, the keep-alive thread - is never held, every other game's thread is, no plugin's
//   thread is, the let-go frees them all, and under the framework's pause nothing is touched.
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include "azl/hid_mirror.hpp"

using namespace azl;
using namespace azl::hidmirror;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

// ---- the page ---------------------------------------------------------------------------------------------------------------

static bool InNeutral(u32 off) {
    if (off >= kPadRaw && off < kPadRaw + 8) {
        return true; // raw state and raw Circle Pad
    }
    if (off >= kPadEntries && off < kTouchHeader) {
        return true; // the pad's entries
    }
    return off >= kTouchRaw && off < kAccel; // raw touch and the touch's entries
}

static void FillReal(u8* real, u8 seed) {
    for (u32 i = 0; i < kPageBytes; ++i) {
        real[i] = static_cast<u8>(i * 7 + seed);
    }
    Store32(real, kPadIndex, 3);
    Store32(real, kTouchIndex, 5);
    for (u32 i = 0; i < kEntries; ++i) {
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, 0x100u + i);
        Store32(real, kTouchEntries + i * kTouchEntryStep + kTouchFlag, 1);
    }
}

static void TestLayout() {
    // The offsets as libctru's hidScanInput reads them (words 4, 10 + 4 * id, 42 + 4, 42 + 8 + 2 * id, 66, 86).
    CHECK(kPadIndex == 4 * 4 && kPadEntries == 10 * 4 && kPadEntryStep == 4 * 4);
    CHECK(kTouchHeader == 42 * 4 && kTouchIndex == (42 + 4) * 4 && kTouchEntries == (42 + 8) * 4 && kTouchEntryStep == 2 * 4);
    CHECK(kAccel == 66 * 4 && kGyro == 86 * 4 && kHidBytes == 0x2B0 && kTouchFlag == 4);

    static u8 real[kPageBytes], m[kPageBytes];
    FillReal(real, 3);
    PrepareMirror(m, real, 0xABCu);
    bool same_elsewhere = true;
    for (u32 off = 0; off < kPageBytes; ++off) {
        if (!InNeutral(off) && m[off] != real[off]) {
            same_elsewhere = false;
        }
    }
    CHECK(same_elsewhere);
    CHECK(Load32(m, kPadRaw) == 0 && Load32(m, kPadRawCircle) == 0);
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 e = kPadEntries + i * kPadEntryStep;
        CHECK(Load32(m, e + kPadHeld) == 0 && Load32(m, e + kPadPressed) == 0 && Load32(m, e + kPadReleased) == 0xABCu && Load32(m, e + kPadCircle) == 0);
        const u32 t = kTouchEntries + i * kTouchEntryStep;
        CHECK(Load32(m, t) == 0 && Load32(m, t + kTouchFlag) == 0); // (the flag too: zero entries with the flag set are a finger at 0,0)
    }
    CHECK(Load32(m, kTouchRaw) == 0 && Load32(m, kTouchRaw + 4) == 0);
    CHECK(Load32(m, kPadIndex) == 3 && Load32(m, kTouchIndex) == 5);
    CHECK(std::memcmp(m + kAccel, real + kAccel, kHidBytes - kAccel) == 0);
    CHECK(std::memcmp(m + kGyro, real + kGyro, 0x10) == 0);

    // A frame later: new ticks, indices, accelerometer, gyroscope, keys and touches in the real page; past 0x2B0 too.
    static u8 real2[kPageBytes];
    FillReal(real2, 101);
    Store32(real2, kPadIndex, 6);
    Store32(real2, kTouchIndex, 1);
    RefreshMirror(m, real2, 0);
    bool live = true, kept = true;
    for (u32 off = 0; off < kHidBytes; ++off) {
        if (!InNeutral(off) && m[off] != real2[off]) {
            live = false;
        }
    }
    for (u32 off = kHidBytes; off < kPageBytes; ++off) {
        if (m[off] != real[off]) {
            kept = false; // (a refresh copies the hid module's part only)
        }
    }
    CHECK(live);
    CHECK(kept);
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 e = kPadEntries + i * kPadEntryStep;
        CHECK(Load32(m, e + kPadHeld) == 0 && Load32(m, e + kPadReleased) == 0);
        CHECK(Load32(m, kTouchEntries + i * kTouchEntryStep + kTouchFlag) == 0);
    }
    CHECK(Load32(m, kPadIndex) == 6 && Load32(m, kTouchIndex) == 1);

    // The keys the game saw held: the entry at the index, clamped to 7 as libctru does.
    CHECK(HeldNow(real) == 0x103u);
    Store32(real, kPadIndex, 9);
    CHECK(HeldNow(real) == 0x107u);

    Released r;
    CHECK(r.Next() == 0);
    r.Start(0x40Cu);
    CHECK(r.Next() == 0x40Cu); // (the prepared page)
    CHECK(r.Next() == 0x40Cu); // (the first refresh)
    CHECK(r.Next() == 0);
    CHECK(r.Next() == 0);
}

static void TestQuiet() {
    static u8 p[kPageBytes];
    std::memset(p, 0, sizeof(p));
    CHECK(PageQuiet(p));
    const auto with = [](u32 off, u32 v) {
        static u8 q[kPageBytes];
        std::memset(q, 0, sizeof(q));
        Store32(q, off, v);
        return PageQuiet(q);
    };
    const auto circle = [](u32 off, std::int16_t x, std::int16_t y) {
        static u8 q[kPageBytes];
        std::memset(q, 0, sizeof(q));
        std::memcpy(q + off, &x, 2);
        std::memcpy(q + off + 2, &y, 2);
        return PageQuiet(q);
    };
    for (u32 i = 0; i < kEntries; ++i) {
        CHECK(!with(kPadEntries + i * kPadEntryStep + kPadHeld, 0x8)); // a key in any entry
        CHECK(!with(kTouchEntries + i * kTouchEntryStep + kTouchFlag, 1)); // a finger in any entry
        CHECK(with(kTouchEntries + i * kTouchEntryStep, 0x00500040u));     // a position with no finger is none
        CHECK(!circle(kPadEntries + i * kPadEntryStep + kPadCircle, 120, 0));
        CHECK(!circle(kPadEntries + i * kPadEntryStep + kPadCircle, 0, -120));
        CHECK(circle(kPadEntries + i * kPadEntryStep + kPadCircle, 12, -kCircleRest)); // at rest: noise
        CHECK(with(kPadEntries + i * kPadEntryStep + kPadReleased, 0x8)); // released is no key held
    }
    CHECK(!with(kTouchRaw + kTouchFlag, 1));
    // The pad's raw words (0x1C the raw state, 0x20 the Circle Pad before its calibration) are NOT looked at (2026-11-10): on a console
    // they kept the page from ever being quiet (121 closes of 144 gave the input back at the 1 s cap, none by the quiet). Whatever they
    // hold, a page whose 8 pad entries and 8 touch entries are at rest is quiet.
    CHECK(with(kPadRaw, 1));
    CHECK(with(kPadRaw, 0xFFFFFFFFu));
    CHECK(circle(kPadRawCircle, kCircleRest + 1, 0));
    CHECK(circle(kPadRawCircle, 2047, -2048));
    {
        static u8 q[kPageBytes];
        FillReal(q, 0x5A); // (a page full of numbers, as a console's: every word not 0)
        for (u32 i = 0; i < kEntries; ++i) {
            const u32 e = kPadEntries + i * kPadEntryStep;
            Store32(q, e + kPadHeld, 0);
            const std::int16_t x = static_cast<std::int16_t>(i % 2 ? 9 : -14), y = static_cast<std::int16_t>(kCircleRest);
            std::memcpy(q + e + kPadCircle, &x, 2);
            std::memcpy(q + e + kPadCircle + 2, &y, 2);
            Store32(q, kTouchEntries + i * kTouchEntryStep + kTouchFlag, 0);
        }
        Store32(q, kTouchRaw + kTouchFlag, 0);
        CHECK(Load32(q, kPadRaw) != 0 && Load32(q, kPadRawCircle) != 0); // (the raw words say "held" and "pushed")
        CHECK(PageQuiet(q) && Unquiet(q) == 0);
        Store32(q, kPadEntries + 5 * kPadEntryStep + kPadHeld, 0x2); // ... and one entry with a key is enough
        CHECK(!PageQuiet(q) && Unquiet(q) == kUnquietKey);
    }
    // Which condition is not met (the debug log's line of a wait that ended at its cap): one bit each, and quiet is "none".
    const auto why = [](u32 off, u32 v) {
        static u8 q[kPageBytes];
        std::memset(q, 0, sizeof(q));
        Store32(q, off, v);
        return Unquiet(q);
    };
    CHECK(Unquiet(p) == 0);
    CHECK(why(kPadEntries + 7 * kPadEntryStep + kPadHeld, 0x1) == kUnquietKey);
    CHECK(why(kPadEntries + 0 * kPadEntryStep + kPadCircle, 0x0000FF00u) == kUnquietCircle);                 // x = -256
    CHECK(why(kPadEntries + 3 * kPadEntryStep + kPadCircle, static_cast<u32>(kCircleRest + 1) << 16) == kUnquietCircle); // y
    CHECK(why(kTouchEntries + 2 * kTouchEntryStep + kTouchFlag, 1) == kUnquietTouch);
    CHECK(why(kTouchRaw + kTouchFlag, 1) == kUnquietRawTouch);
    CHECK(why(kPadRaw, 0x40C) == 0 && why(kPadRawCircle, 0x07FF0800u) == 0);
    {
        static u8 q[kPageBytes];
        std::memset(q, 0, sizeof(q));
        Store32(q, kPadEntries + kPadHeld, 0x8);
        Store32(q, kTouchEntries + kTouchFlag, 1);
        Store32(q, kTouchRaw + kTouchFlag, 1);
        CHECK(Unquiet(q) == (kUnquietKey | kUnquietTouch | kUnquietRawTouch) && !PageQuiet(q));
    }
    CHECK(kUnquietKey == 1 && kUnquietCircle == 2 && kUnquietTouch == 4 && kUnquietRawTouch == 8); // (the log's line prints the bits)

    // The wait: every frame (16 or 17 ms), 50 ms of quiet; any input starts it again; at most 1 s.
    ReleaseWait w;
    CHECK(!w.Update(true, 0)); // (not waiting)
    w.Start(1000);
    CHECK(w.waiting());
    CHECK(!w.Update(true, 1000) && !w.Update(true, 1016) && !w.Update(true, 1033));
    CHECK(w.Update(true, 1050) && !w.waiting());
    CHECK(!w.capped()); // (ended by the quiet)
    // A clock OLDER than the start (2026-11-10: the close read the clock again, 1-2 ms after the frame's own, and the frame then looked at
    // the wait with its own): the difference is -1, not 4 billion - the wait goes on, and it ends when it should, counted from the start.
    w.Start(1001);
    CHECK(!w.Update(false, 1000) && w.waiting()); // (held, 1 ms "before" the start: not over the cap)
    CHECK(!w.Update(false, 1017) && !w.Update(false, 2000));
    CHECK(w.Update(false, 2001) && w.capped()); // (1 s after the start, a key held all along: the cap)
    w.Start(1002);
    CHECK(!w.Update(true, 1000)); // (quiet, 2 ms "before" the start: the 50 ms begin at this look)
    CHECK(!w.Update(true, 999));  // (a clock that steps back between two looks: -1 ms of quiet, not 4 billion)
    CHECK(!w.Update(true, 1049));
    CHECK(w.Update(true, 1050) && !w.capped());
    w.Start(0x00000002u); // (the same across the wrap: the frame's clock 0xFFFFFFFF, the start 3 ms later)
    CHECK(!w.Update(false, 0xFFFFFFFFu) && w.waiting());
    CHECK(!w.Update(false, 0x000003E9u) && w.Update(false, 0x000003EAu) && w.capped());
    w.Start(0);
    u32 t = 0, done_at = 0;
    for (; t < 2000; t += 17) {
        if (w.Update(t >= 300, t)) {
            done_at = t;
            break;
        }
    }
    CHECK(done_at >= 350 && done_at < 350 + 17 + 17);
    CHECK(!w.capped());
    w.Start(5);
    done_at = 0;
    for (t = 5; t < 3000; t += 16) {
        if (w.Update(false, t)) {
            done_at = t;
            break;
        }
    }
    CHECK(done_at >= 1005 && done_at < 1005 + 16); // (held all along: 1 s)
    CHECK(w.capped());
    // quiet for 50 ms exactly when the cap comes: the quiet ended it (the cap's line of the log is for a page that never was)
    w.Start(0);
    CHECK(!w.Update(false, 940) && !w.Update(true, 950) && w.Update(true, 1000) && !w.capped());
    // quiet only at the last look: the cap ended it
    w.Start(0);
    CHECK(!w.Update(false, 990) && w.Update(true, 1000) && w.capped());
    w.Start(0xFFFFFFF0u); // (a clock that wraps)
    CHECK(!w.Update(true, 0xFFFFFFF0u) && !w.Update(true, 0x10u));
    CHECK(w.Update(true, 0x22u));
    w.Start(0);
    w.Cancel();
    CHECK(!w.waiting() && !w.Update(true, 5000));

    CHECK(FreezeSeenAll(10, 10, 64) && FreezeSeenAll(20, 10, 64));
    CHECK(!FreezeSeenAll(0, 10, 64) && !FreezeSeenAll(11, 10, 64) && !FreezeSeenAll(9, 10, 64) && !FreezeSeenAll(0, 0, 64));
    CHECK(!FreezeSeenAll(5, -1, 64));
    CHECK(FreezeSeenAll(70, 64, 64) && !FreezeSeenAll(63, 64, 64)); // (a full list: at least that many)

    TryMutex m;
    CHECK(m.TryLock() && !m.TryLock());
    m.Unlock();
    CHECK(m.TryLock());
}

// ---- what is still down when the wait ends at its cap (HeldMask) --------------------------------------------------------------------

constexpr u32 kKeyA = 0x1, kKeyB = 0x2, kKeyX = 0x400;
constexpr u32 kPadRight = 0x10000000u, kPadLeft = 0x20000000u, kPadUp = 0x40000000u, kPadDown = 0x80000000u;

static void PutCircle(u8* p, u32 off, int x, int y) {
    const std::int16_t sx = static_cast<std::int16_t>(x), sy = static_cast<std::int16_t>(y);
    std::memcpy(p + off, &sx, 2);
    std::memcpy(p + off + 2, &sy, 2);
}

// Everything a reader of the page could take for input, whatever entry it reads: the keys of the 8 entries (held, pressed, released) and
// of the raw word, the Circle Pad of the 8 entries and the raw one, the touch of the 8 entries and the raw one (positions too).
struct Shown {
    u32 keys = 0;
    bool circle = false;
    bool touch = false;
};
static Shown ShownBy(const u8* p) {
    Shown s;
    s.keys = Load32(p, kPadRaw);
    s.circle = Load32(p, kPadRawCircle) != 0;
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 e = kPadEntries + i * kPadEntryStep;
        s.keys |= Load32(p, e + kPadHeld) | Load32(p, e + kPadPressed) | Load32(p, e + kPadReleased);
        s.circle = s.circle || Load32(p, e + kPadCircle) != 0;
    }
    for (u32 off = kTouchRaw; off < kTouchEnd; ++off) {
        s.touch = s.touch || p[off] != 0;
    }
    return s;
}
// What a game that reads the entry at the page's index holds.
static u32 HeldAtIndex(const u8* p) {
    return HeldNow(p);
}

static void TestHeldMask() {
    static u8 real[kPageBytes], m[kPageBytes];
    const auto clean = [&]() {
        std::memset(real, 0, sizeof(real));
        std::memset(m, 0xEE, sizeof(m));
    };
    HeldMask h;
    CHECK(!h.on() && !h.any() && h.keys() == 0 && h.what() == 0);

    // Nothing in the entries: no mask, and the mirror is not written.
    clean();
    Store32(real, kPadRaw, 0x40C); // (the raw word is not looked at, as in Unquiet)
    CHECK(!h.Start(m, real, 100) && !h.on() && m[kPadEntries] == 0xEE && m[0] == 0xEE);

    // A key in an entry that is NOT the one at the index, and only as "released" in another: both are masked - in every word of every entry
    // and in the raw word - and a key that was not there at the cap goes through.
    clean();
    Store32(real, kPadIndex, 5);
    Store32(real, kPadEntries + 2 * kPadEntryStep + kPadHeld, kKeyB);
    Store32(real, kPadEntries + 2 * kPadEntryStep + kPadPressed, kKeyB);
    Store32(real, kPadEntries + 4 * kPadEntryStep + kPadReleased, kKeyX);
    Store32(real, kPadRaw, kKeyB);
    CHECK(h.Start(m, real, 100) && h.on() && h.keys() == (kKeyB | kKeyX) && h.what() == 0);
    CHECK(ShownBy(m).keys == 0);
    CHECK(Load32(m, kPadIndex) == 5); // (the header is the real page's)
    for (u32 i = 0; i < kEntries; ++i) {
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, kKeyB | kKeyA); // (B still down, A new)
    }
    Store32(real, kPadEntries + 6 * kPadEntryStep + kPadPressed, kKeyA);
    Store32(real, kPadEntries + 4 * kPadEntryStep + kPadReleased, 0); // (X is out of the entries)
    Store32(real, kPadRaw, kKeyB | kKeyA);
    CHECK(h.Frame(m, real, 117) && h.keys() == kKeyB); // (X left the mask; A never entered it: a mask only shrinks)
    CHECK(ShownBy(m).keys == kKeyA && HeldAtIndex(m) == kKeyA && Load32(m, kPadEntries + 6 * kPadEntryStep + kPadPressed) == kKeyA);
    CHECK(Load32(m, kPadRaw) == kKeyA);
    // B let go: its "released" is still in an entry, so B stays masked (no release of a key the game never saw pressed) ...
    for (u32 i = 0; i < kEntries; ++i) {
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, kKeyA);
    }
    Store32(real, kPadEntries + 2 * kPadEntryStep + kPadPressed, 0);
    Store32(real, kPadEntries + 7 * kPadEntryStep + kPadReleased, kKeyB);
    Store32(real, kPadRaw, kKeyA);
    CHECK(h.Frame(m, real, 134) && h.keys() == kKeyB && ShownBy(m).keys == kKeyA);
    // ... and once no entry mentions it, nothing is masked: the mirror is the real page, byte for byte, up to the end of what hid writes.
    Store32(real, kPadEntries + 7 * kPadEntryStep + kPadReleased, 0);
    CHECK(!h.Frame(m, real, 151) && !h.any() && h.on()); // (on until Clear: the copy goes on while the page cannot go back)
    CHECK(std::memcmp(m, real, kHidBytes) == 0 && m[kHidBytes] == 0xEE && m[kPageBytes - 1] == 0xEE);
    Store32(real, kPadEntries + kPadHeld, kKeyB | kKeyA); // (B pressed again: a new press, it goes through)
    CHECK(!h.Frame(m, real, 168) && std::memcmp(m, real, kHidBytes) == 0);
    h.Clear();
    CHECK(!h.on() && !h.any());

    // The Circle Pad off its rest in one entry: centred in all of them and in the raw word, and its four direction keys masked with it -
    // the ones that come later too (the stick turned without passing through the centre).
    clean();
    PutCircle(real, kPadEntries + 3 * kPadEntryStep + kPadCircle, -120, 95);
    Store32(real, kPadEntries + 3 * kPadEntryStep + kPadHeld, kPadLeft | kPadUp);
    Store32(real, kPadRawCircle, 0x09890D0Du); // (what console 63 printed)
    CHECK(h.Start(m, real, 1000) && h.what() == kMaskCircle && h.keys() == 0); // (the direction keys are the pad's mask's)
    CHECK(ShownBy(m).keys == 0 && !ShownBy(m).circle);
    for (u32 i = 0; i < kEntries; ++i) {
        PutCircle(real, kPadEntries + i * kPadEntryStep + kPadCircle, 140, -30);
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, kPadRight | kKeyA);
    }
    CHECK(h.Frame(m, real, 1017) && h.what() == kMaskCircle && h.keys() == 0);
    CHECK(ShownBy(m).keys == kKeyA && !ShownBy(m).circle); // (right is new, and masked with the pad; A goes through)
    // At rest in every entry but one: still centred. At rest in all: the mask falls, and the direction key an entry still mentions (its
    // release) waits as any key does.
    for (u32 i = 0; i < kEntries; ++i) {
        PutCircle(real, kPadEntries + i * kPadEntryStep + kPadCircle, 12, -kCircleRest);
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, 0);
    }
    PutCircle(real, kPadEntries + 1 * kPadEntryStep + kPadCircle, kCircleRest + 1, 0);
    Store32(real, kPadEntries + 2 * kPadEntryStep + kPadReleased, kPadRight);
    CHECK(h.Frame(m, real, 1034) && h.what() == kMaskCircle && !ShownBy(m).circle && ShownBy(m).keys == 0);
    PutCircle(real, kPadEntries + 1 * kPadEntryStep + kPadCircle, kCircleRest, 0);
    CHECK(h.Frame(m, real, 1051) && h.what() == 0 && h.keys() == kPadRight && !h.circle_capped());
    CHECK(ShownBy(m).keys == 0 && ShownBy(m).circle); // (the pad's own small numbers at rest are the real page's again)
    CHECK(LoadS16(m, kPadEntries + kPadCircle) == 12 && LoadS16(m, kPadEntries + kPadCircle + 2) == -kCircleRest);
    CHECK(Load32(m, kPadRawCircle) == 0x09890D0Du);
    Store32(real, kPadEntries + 2 * kPadEntryStep + kPadReleased, 0);
    CHECK(!h.Frame(m, real, 1068) && std::memcmp(m, real, kHidBytes) == 0);
    h.Clear();

    // A Circle Pad that never comes to rest: centred for kCircleMaskMaxMs, counted with its sign across the clock's wrap, then shown as it
    // is (with its direction keys: the pad is given up, not half of it); a key held all along is still masked.
    clean();
    for (u32 i = 0; i < kEntries; ++i) {
        PutCircle(real, kPadEntries + i * kPadEntryStep + kPadCircle, 0, 60);
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, kPadUp | kKeyB);
    }
    CHECK(h.Start(m, real, 0xFFFFF000u) && h.what() == kMaskCircle);
    CHECK(h.Frame(m, real, 0xFFFFEFFFu) && h.what() == kMaskCircle); // (a clock 1 ms older than the start: not "over the cap")
    CHECK(h.Frame(m, real, 0xFFFFF000u + kCircleMaskMaxMs - 1) && h.what() == kMaskCircle && !ShownBy(m).circle && ShownBy(m).keys == 0);
    CHECK(h.Frame(m, real, 0xFFFFF000u + kCircleMaskMaxMs) && h.what() == 0 && h.circle_capped() && h.keys() == kKeyB);
    CHECK(ShownBy(m).circle && ShownBy(m).keys == kPadUp && LoadS16(m, kPadEntries + kPadCircle + 2) == 60);
    CHECK(h.since(0xFFFFF000u + kCircleMaskMaxMs) == kCircleMaskMaxMs);
    CHECK(kCircleMaskMaxMs == 10000 && kCirclePadKeys == (kPadRight | kPadLeft | kPadUp | kPadDown));
    h.Clear();
    CHECK(!h.on() && !h.any() && h.keys() == 0 && h.what() == 0); // (B was still masked: Clear forgets it)
    CHECK(h.Start(m, real, 77) && h.what() == kMaskCircle && !h.circle_capped() && h.since(80) == 3); // (a new start: its own 10 s)
    CHECK(h.frames() == 0 && h.longest_gap_ms() == 0); // (... and its own count of the frames that wrote the mirror)
    CHECK(h.Frame(m, real, 94) && h.Frame(m, real, 160) && h.Frame(m, real, 159) && h.Frame(m, real, 176));
    CHECK(h.frames() == 4 && h.longest_gap_ms() == 66); // the longest time between two frames: what a key waited for the plugin's thread
    h.Clear();
    // A key an entry mentions only as "pressed" (the module never writes that, held goes with it: whatever is there is masked).
    clean();
    Store32(real, kPadEntries + 1 * kPadEntryStep + kPadPressed, kKeyX);
    CHECK(h.Start(m, real, 0) && h.keys() == kKeyX && ShownBy(m).keys == 0);
    h.Clear();

    // The pen down in one entry, or only in the raw touch: the touch is absent - flags and positions, raw and entries - until neither has it.
    clean();
    Store32(real, kTouchEntries + 6 * kTouchEntryStep, 0x00500040u);
    Store32(real, kTouchEntries + 6 * kTouchEntryStep + kTouchFlag, 1);
    Store32(real, kTouchIndex, 6);
    CHECK(h.Start(m, real, 50) && h.what() == kMaskTouch && h.keys() == 0);
    CHECK(!ShownBy(m).touch && Load32(m, kTouchIndex) == 6);
    Store32(real, kTouchEntries + 6 * kTouchEntryStep + kTouchFlag, 0);
    Store32(real, kTouchRaw, 0x00500040u);
    Store32(real, kTouchRaw + kTouchFlag, 1);
    Store32(real, kPadEntries + kPadHeld, kKeyX);
    CHECK(h.Frame(m, real, 67) && h.what() == kMaskTouch && !ShownBy(m).touch && ShownBy(m).keys == kKeyX); // (a key meanwhile goes through)
    Store32(real, kTouchRaw + kTouchFlag, 0);
    CHECK(!h.Frame(m, real, 84) && std::memcmp(m, real, kHidBytes) == 0);
    Store32(real, kTouchEntries + 7 * kTouchEntryStep + kTouchFlag, 1); // (the pen down again: a new touch, the game's)
    CHECK(!h.Frame(m, real, 101) && ShownBy(m).touch);
    h.Clear();

    // The live parts in a frame with the masks on, as in the neutral mirror's refresh: the headers, the accelerometer and the gyroscope.
    clean();
    FillReal(real, 0x11);
    CHECK(h.Start(m, real, 0) && h.what() == (kMaskCircle | kMaskTouch));
    FillReal(real, 0x77);
    CHECK(h.Frame(m, real, 17));
    CHECK(std::memcmp(m, real, kPadRaw) == 0 && std::memcmp(m + kTouchHeader, real + kTouchHeader, kTouchRaw - kTouchHeader) == 0);
    CHECK(std::memcmp(m + kAccel, real + kAccel, kHidBytes - kAccel) == 0 && Load32(m, kPadRawCircle + 4) == Load32(real, kPadRawCircle + 4));
    CHECK(!ShownBy(m).circle && !ShownBy(m).touch && m[kHidBytes] == 0xEE);
    h.Clear();
}

// The hid module, as far as these tests need it: every 4274 us (234 Hz) the pad's ring and the touch's take a sample of what the user
// holds. The Circle Pad's direction keys as the module sets them, past 40 on an axis.
struct Hid {
    u8 real[kPageBytes];
    u32 keys = 0;
    int cx = 0, cy = 0;
    bool pen = false;
    u32 prev = 0, index = kEntries - 1, samples = 0;
    Hid() {
        std::memset(real, 0, sizeof(real));
    }
    u32 Held() const {
        return keys | (cx > 40 ? kPadRight : 0u) | (cx < -40 ? kPadLeft : 0u) | (cy > 40 ? kPadUp : 0u) | (cy < -40 ? kPadDown : 0u);
    }
    void Sample() {
        index = (index + 1) % kEntries;
        ++samples;
        const u32 held = Held(), e = kPadEntries + index * kPadEntryStep, t = kTouchEntries + index * kTouchEntryStep;
        Store32(real, e + kPadHeld, held);
        Store32(real, e + kPadPressed, held & ~prev);
        Store32(real, e + kPadReleased, prev & ~held);
        PutCircle(real, e + kPadCircle, cx, cy);
        prev = held;
        Store32(real, kPadRaw, keys);
        Store32(real, kPadRawCircle, 0x08000800u + static_cast<u32>(cx & 0xFF));
        Store32(real, t, pen ? 0x00780050u : 0u);
        Store32(real, t + kTouchFlag, pen ? 1u : 0u);
        Store32(real, kTouchRaw, pen ? 0x07000600u : 0u);
        Store32(real, kTouchRaw + kTouchFlag, pen ? 1u : 0u);
        Store32(real, 0, samples); // (the tick)
        Store32(real, kPadIndex, index);
        Store32(real, kTouchHeader, samples);
        Store32(real, kTouchIndex, index);
        Store32(real, kAccel, samples * 3); // (something live past the touch)
    }
};

// The plugin's thread's frame over the pure parts, as Sources/ctr/input_mirror.cpp makes it (MirrorWant, MirrorRelease, MirrorFrame,
// MaskFrame: the same calls in the same order), the kernel's switch reduced to `active`: the game reads the mirror while it is true and
// the real page otherwise. `masks` false is the flow of 1.1.4, with no HeldMask: what the normal path is compared with.
struct Flow {
    u8 page[kPageBytes];
    bool have_page = false, active = false, want = false, tried = false, dead = false, want_restore = false;
    bool masks = true;
    bool lock_busy = false;    // the way back finds the mirror's lock taken
    bool capped_quiet = false; // a wait ended at its cap with the page quiet at that look
    Released released;
    ReleaseWait wait;
    HeldMask held;
    std::vector<const char*> said;
    std::vector<u32> said_ms;
    u32 back_ms = 0, backs = 0, mask_on_ms = 0;

    Flow() {
        std::memset(page, 0, sizeof(page));
    }
    void Say(const char* what, u32 now_ms) {
        said.push_back(what);
        said_ms.push_back(now_ms);
    }
    bool RestoreHere(u32 now_ms) {
        if (!active) {
            want_restore = false;
            return true;
        }
        if (lock_busy) {
            return false;
        }
        want_restore = false;
        active = false;
        back_ms = now_ms;
        ++backs;
        return true;
    }
    void Want(u32 now_ms) { // MirrorWant
        want = true;
        tried = false;
        wait.Cancel();
        if (held.on()) {
            Say("opened again", now_ms);
            if (have_page && active) {
                released.Start(HeldNow(page));
            }
            held.Clear();
        }
    }
    void Release(u32 now_ms) { // MirrorRelease
        want = false;
        if (active) {
            wait.Start(now_ms);
        }
    }
    // Another thread gives the page back: the keep-alive at HOME_ENTER, SLEEP_ENTER, SWAP_ENTER or EXIT, a game's thread at a notification.
    void GivenBackByAnother(bool exit) {
        active = false;
        dead = dead || exit;
    }
    void MaskFrame(const u8* real, u32 now_ms) {
        const u32 keys = held.keys(), what = held.what();
        const bool left = held.Frame(page, real, now_ms);
        if (left) {
            if (held.keys() != keys || held.what() != what) {
                Say((what & ~held.what() & kMaskCircle) != 0 && held.circle_capped() ? "pad capped" : "let go", now_ms);
            }
            return;
        }
        if (keys != 0 || what != 0) {
            Say((what & ~held.what() & kMaskCircle) != 0 && held.circle_capped() ? "last, pad capped" : "last", now_ms);
        }
        if (RestoreHere(now_ms) && !active) {
            held.Clear();
        }
    }
    void Frame(const u8* real, u32 now_ms) { // MirrorFrame
        if (want_restore) {
            want = false;
            wait.Cancel();
            RestoreHere(now_ms);
        }
        if (want && !tried && !active) { // (Engage)
            tried = true;
            if (!dead) {
                const u32 keys = HeldNow(real);
                PrepareMirror(page, real, keys);
                released.Start(keys);
                released.Next();
                have_page = true;
                active = true;
            }
        }
        if (active) {
            if (held.on()) {
                MaskFrame(real, now_ms);
                return;
            }
            RefreshMirror(page, real, released.Next());
            if (!want) {
                if (wait.waiting()) {
                    const u32 unquiet = Unquiet(real);
                    if (wait.Update(unquiet == 0, now_ms)) {
                        capped_quiet = capped_quiet || (wait.capped() && unquiet == 0);
                        if (masks && unquiet != 0 && held.Start(page, real, now_ms)) {
                            mask_on_ms = now_ms;
                            Say("masks on", now_ms);
                            return;
                        }
                        RestoreHere(now_ms);
                    }
                } else {
                    RestoreHere(now_ms);
                }
            }
        } else if (!want && have_page) {
            have_page = false;
            if (held.on()) {
                Say("went with the page", now_ms);
                held.Clear();
            }
        }
    }
    const u8* GamePage(const u8* real) const {
        return active ? page : real;
    }
};

// A run: the hid module's samples and the plugin's frames (every 16667 us) on one clock of microseconds; `script(ms)` is called before each
// of them to move the user's fingers and the keyboard; `look(ms)` after each, to see what the game's page shows.
template <class Script, class LookFn>
static void Run(Hid& hid, Flow& flow, u32 until_ms, Script script, LookFn look, u32 clock0_ms = 5000) {
    std::uint64_t next_sample = 0, next_frame = 9000; // (the frame's phase against the samples is arbitrary)
    while (true) {
        const bool frame = next_frame < next_sample;
        const std::uint64_t t = frame ? next_frame : next_sample;
        const u32 ms = static_cast<u32>(t / 1000);
        if (ms >= until_ms) {
            break;
        }
        script(ms, frame);
        if (frame) {
            flow.Frame(hid.real, clock0_ms + ms);
            next_frame += 16667;
        } else {
            hid.Sample();
            next_sample += 4274;
        }
        look(ms);
    }
}

static bool Said(const Flow& f, const char* what) {
    for (const char* s : f.said) {
        if (std::strcmp(s, what) == 0) {
            return true;
        }
    }
    return false;
}

// The console's case (63, Final Fantasy Explorers, 2026-11-13): B closes the keyboard and stays down 3 s more, the Circle Pad is pushed
// all along; A is pressed for 100 ms in the middle. The keyboard opens at 0 ms and closes at 200.
static void TestMaskConsoleCase() {
    Hid hid;
    Flow flow;
    u32 seen_b_ms = 0, seen_pad_ms = 0, a_first_ms = 0, a_last_ms = 0, b_new_ms = 0, wait_keys = 0;
    bool opened = false, closed = false;
    Run(
        hid, flow, 5000,
        [&](u32 ms, bool frame) {
            hid.keys = (ms >= 150 && ms < 3200 ? kKeyB : 0u) | (ms >= 600 && ms < 650 ? kKeyX : 0u) | (ms >= 1500 && ms < 1600 ? kKeyA : 0u) |
                       (ms >= 4000 && ms < 4100 ? kKeyB : 0u);
            hid.cx = ms >= 100 && ms < 3400 ? -120 : 0;
            hid.cy = ms >= 100 && ms < 3400 ? 95 : 0;
            if (frame && !opened) {
                opened = true;
                flow.Want(5000 + ms);
            }
            if (frame && !closed && ms >= 200) {
                closed = true;
                flow.Release(5000 + ms);
            }
        },
        [&](u32 ms) {
            if (ms < 30) {
                return; // (the keyboard's first frame: the game's page is its own until the switch)
            }
            const Shown s = ShownBy(flow.GamePage(hid.real));
            if (ms < 3990) {
                if ((s.keys & (kKeyB | kCirclePadKeys)) != 0 && seen_b_ms == 0) {
                    seen_b_ms = ms;
                }
                if (s.circle && ms < 3400 && seen_pad_ms == 0) {
                    seen_pad_ms = ms;
                }
            }
            if (ms < 1200) {
                wait_keys |= s.keys; // (the keyboard open, then the wait for the release: the neutral mirror)
            }
            if ((HeldAtIndex(flow.GamePage(hid.real)) & kKeyA) != 0) {
                a_first_ms = a_first_ms ? a_first_ms : ms;
                a_last_ms = ms;
            }
            if (ms >= 3990 && (HeldAtIndex(flow.GamePage(hid.real)) & kKeyB) != 0 && b_new_ms == 0) {
                b_new_ms = ms;
            }
        });
    CHECK(seen_b_ms == 0);   // the game never sees the B that closed the keyboard, nor a direction key of the pushed pad, in any entry
    CHECK(seen_pad_ms == 0); // ... nor the Circle Pad off its centre, until it is let go
    CHECK(wait_keys == 0);   // (X, pressed and let go inside the wait, is the neutral mirror's: never shown)
    CHECK(flow.mask_on_ms >= 5000 + 1200 && flow.mask_on_ms < 5000 + 1200 + 34); // (the masks start at the cap, 1 s after the close)
    CHECK(a_first_ms >= 1500 && a_first_ms < 1500 + 5 + 17 + 1); // a key pressed with the masks on reaches the game at the next frame
    CHECK(a_last_ms >= 1600 - 17 && a_last_ms < 1600 + 5 + 17 + 1);
    // (B out of the entries 35 ms after it is let go; the pad at rest in all of them 30 ms after it is, its direction keys' release out of
    // them 5 ms later: one line or two, as the frame falls)
    CHECK(flow.said.size() >= 3 && flow.said.size() <= 4 && std::strcmp(flow.said[0], "masks on") == 0 && std::strcmp(flow.said.back(), "last") == 0);
    CHECK(flow.said.size() >= 3 && std::strcmp(flow.said[1], "let go") == 0 && flow.said_ms[1] >= 5000 + 3200 + 30 && flow.said_ms[1] < 5000 + 3200 + 60);
    CHECK(flow.backs == 1 && flow.back_ms >= 5000 + 3400 + 30 && flow.back_ms < 5000 + 3400 + 75); // the page back once, when the last mask fell
    CHECK(!flow.held.on() && !flow.active && !flow.have_page);
    CHECK(flow.held.frames() > 120 && flow.held.longest_gap_ms() >= 16 && flow.held.longest_gap_ms() <= 17); // (2.2 s of masks, a frame each 16.7 ms)
    CHECK(b_new_ms >= 4000 && b_new_ms < 4000 + 6); // (B pressed again after that is the game's, on its own page)
}

// The normal path - everything let go inside the wait - is 1.1.4's, byte for byte and frame for frame: the two flows side by side, the one
// with the masks and the one without, on the same fingers. Also the wait that ends at its cap with the page quiet at that look.
static void TestMaskNormalPath() {
    for (u32 let_go_ms : {230u, 420u, 700u, 1140u}) {
        Hid hid;
        Flow now, before;
        before.masks = false;
        bool opened = false, closed = false, same = true;
        Run(
            hid, now, 2500,
            [&](u32 ms, bool frame) {
                hid.keys = ms >= 150 && ms < let_go_ms ? kKeyB : 0u;
                hid.cx = ms >= 100 && ms < let_go_ms ? 110 : 0;
                hid.pen = ms >= 120 && ms < let_go_ms - 20;
                if (frame && !opened) {
                    opened = true;
                    now.Want(5000 + ms);
                    before.Want(5000 + ms);
                }
                if (frame && !closed && ms >= 200) {
                    closed = true;
                    now.Release(5000 + ms);
                    before.Release(5000 + ms);
                }
                if (frame) {
                    before.Frame(hid.real, 5000 + ms); // (Run makes `now`'s frame right after this, on the same page and clock)
                }
            },
            [&](u32) {
                same = same && now.active == before.active && now.have_page == before.have_page &&
                       (!now.have_page || std::memcmp(now.page, before.page, kPageBytes) == 0);
            });
        CHECK(same);
        CHECK(now.backs == 1 && before.backs == 1 && now.back_ms == before.back_ms);
        CHECK(now.said.empty() && !now.held.on()); // no mask was ever on
        if (let_go_ms == 1140) {
            CHECK(now.capped_quiet && before.capped_quiet); // (let go 60 ms before the cap: quiet at the cap's look, not yet for 50 ms)
            CHECK(now.back_ms >= 5000 + 1200 && now.back_ms < 5000 + 1200 + 34);
        } else {
            CHECK(!now.capped_quiet && now.back_ms >= 5000 + let_go_ms + 50 && now.back_ms < 5000 + let_go_ms + 50 + 35 + 17 + 17);
        }
    }
}

// The pen on the keyboard through the close and 2 s more: the touch is absent for the game until it is lifted, a key meanwhile goes
// through, and the pen put down again later is the game's.
static void TestMaskTouch() {
    Hid hid;
    Flow flow;
    u32 touch_ms = 0, a_ms = 0, late_touch_ms = 0;
    bool opened = false, closed = false;
    Run(
        hid, flow, 4000,
        [&](u32 ms, bool frame) {
            hid.pen = (ms >= 150 && ms < 2200) || ms >= 3000;
            hid.keys = ms >= 1500 && ms < 1560 ? kKeyA : 0u;
            if (frame && !opened) {
                opened = true;
                flow.Want(5000 + ms);
            }
            if (frame && !closed && ms >= 200) {
                closed = true;
                flow.Release(5000 + ms);
            }
        },
        [&](u32 ms) {
            if (ms < 30) {
                return;
            }
            const Shown s = ShownBy(flow.GamePage(hid.real));
            if (s.touch && ms < 2990 && touch_ms == 0) {
                touch_ms = ms;
            }
            if (s.touch && ms >= 2990 && late_touch_ms == 0) {
                late_touch_ms = ms;
            }
            if ((HeldAtIndex(flow.GamePage(hid.real)) & kKeyA) != 0 && a_ms == 0) {
                a_ms = ms;
            }
        });
    CHECK(touch_ms == 0); // the game never sees the pen that was on the keyboard
    CHECK(a_ms >= 1500 && a_ms < 1500 + 5 + 17 + 1);
    CHECK(flow.said.size() == 2 && Said(flow, "masks on") && Said(flow, "last"));
    CHECK(flow.backs == 1 && flow.back_ms >= 5000 + 2200 + 25 && flow.back_ms < 5000 + 2200 + 60);
    CHECK(late_touch_ms >= 3000 && late_touch_ms < 3000 + 6 && !flow.held.on());
}

// The keyboard opened again with the masks on (B still held, A held by then and seen by the game): the mirror is neutral again at that
// frame - A "released" for two refreshes - the masks are dropped, and the second close has its own wait and, B still held, its own masks.
static void TestMaskReopen() {
    Hid hid;
    Flow flow;
    u32 shown_while_open = 0, released_a = 0, seen_b_ms = 0, a_ms = 0;
    bool opened = false, closed = false, opened2 = false, closed2 = false;
    Run(
        hid, flow, 5000,
        [&](u32 ms, bool frame) {
            hid.keys = (ms >= 150 && ms < 3500 ? kKeyB : 0u) | (ms >= 1300 && ms < 1800 ? kKeyA : 0u);
            if (frame && !opened) {
                opened = true;
                flow.Want(5000 + ms);
            }
            if (frame && !closed && ms >= 200) {
                closed = true;
                flow.Release(5000 + ms);
            }
            if (frame && !opened2 && ms >= 1500) {
                opened2 = true;
                CHECK(flow.held.on() && flow.held.keys() == kKeyB);
                flow.Want(5000 + ms);
            }
            if (frame && !closed2 && ms >= 2000) {
                closed2 = true;
                CHECK(!flow.held.on() && flow.active);
                flow.Release(5000 + ms);
            }
        },
        [&](u32 ms) {
            if (ms < 30) {
                return;
            }
            const u8* page = flow.GamePage(hid.real);
            const Shown s = ShownBy(page);
            if ((s.keys & kKeyB) != 0 && seen_b_ms == 0) {
                seen_b_ms = ms;
            }
            if ((HeldAtIndex(page) & kKeyA) != 0 && a_ms == 0) {
                a_ms = ms;
            }
            if (opened2 && !closed2 && flow.active) {
                for (u32 i = 0; i < kEntries; ++i) {
                    const u32 e = kPadEntries + i * kPadEntryStep;
                    shown_while_open |= Load32(page, e + kPadHeld) | Load32(page, e + kPadPressed);
                    released_a |= Load32(page, e + kPadReleased);
                }
            }
        });
    CHECK(a_ms >= 1300 && a_ms < 1300 + 5 + 17 + 1); // (with the first masks on, A reached the game)
    CHECK(shown_while_open == 0 && released_a == kKeyA); // the keyboard open again: nothing held, and the A the game saw is released
    CHECK(seen_b_ms == 0);
    CHECK(flow.said.size() == 4 && std::strcmp(flow.said[0], "masks on") == 0 && std::strcmp(flow.said[1], "opened again") == 0 &&
          std::strcmp(flow.said[2], "masks on") == 0 && std::strcmp(flow.said[3], "last") == 0);
    CHECK(flow.said.size() == 4 && flow.said_ms[2] >= 5000 + 3000 && flow.said_ms[2] < 5000 + 3000 + 34); // (1 s after the second close)
    CHECK(flow.backs == 1 && flow.back_ms >= 5000 + 3500 + 30 && flow.back_ms < 5000 + 3500 + 60 && !flow.held.on());
}

// HOME, sleep or the in-game menu with the masks on: another thread gives the page back (or cannot - the lock - and the plugin's thread
// does at its next frame), the masks go with it, and the next keyboard starts from nothing: neutral, B - still held - released once.
static void TestMaskHomeAndExit() {
    for (int kind = 0; kind < 3; ++kind) { // 0: given back by the other thread; 1: left to the plugin's thread (want_restore); 2: EXIT
        Hid hid;
        Flow flow;
        u32 held_while_open2 = 0, released_while_open2 = 0;
        bool opened = false, closed = false, home = false, opened2 = false;
        Run(
            hid, flow, 4000,
            [&](u32 ms, bool frame) {
                hid.keys = ms >= 150 ? kKeyB : 0u;
                hid.cy = ms >= 150 ? -130 : 0;
                if (frame && !opened) {
                    opened = true;
                    flow.Want(5000 + ms);
                }
                if (frame && !closed && ms >= 200) {
                    closed = true;
                    flow.Release(5000 + ms);
                }
                if (!frame && !home && ms >= 1700) {
                    home = true;
                    CHECK(flow.held.on() && flow.held.keys() == kKeyB && flow.held.what() == kMaskCircle && flow.active);
                    if (kind == 1) {
                        flow.want_restore = true;
                    } else {
                        flow.GivenBackByAnother(kind == 2);
                    }
                }
                if (frame && home && !opened2 && ms >= 2500) {
                    opened2 = true;
                    CHECK(!flow.held.on() && !flow.active && !flow.have_page); // (nothing left of the masks, the page back in the heap)
                    flow.Want(5000 + ms);
                }
            },
            [&](u32) {
                if (opened2 && flow.active) { // (from the switch itself: the "released" are in the prepared page and in one refresh)
                    for (u32 i = 0; i < kEntries; ++i) {
                        const u32 e = kPadEntries + i * kPadEntryStep;
                        held_while_open2 |= Load32(flow.page, e + kPadHeld) | Load32(flow.page, e + kPadPressed) | Load32(flow.page, e + kPadCircle);
                        released_while_open2 |= Load32(flow.page, e + kPadReleased);
                    }
                }
            });
        CHECK(flow.said.size() == 2 && std::strcmp(flow.said[0], "masks on") == 0 && std::strcmp(flow.said[1], "went with the page") == 0);
        if (kind == 1) {
            CHECK(flow.backs == 1 && flow.back_ms >= 5000 + 1700 && flow.back_ms < 5000 + 1700 + 18); // (the plugin's thread, its next frame)
        } else {
            CHECK(flow.backs == 0);
        }
        if (kind == 2) {
            CHECK(!flow.active && !flow.have_page && !flow.held.on() && held_while_open2 == 0); // the game ended: no mirror ever again
        } else {
            CHECK(flow.active && !flow.held.on() && held_while_open2 == 0 && released_while_open2 == (kKeyB | kPadDown));
        }
    }
}

// A Circle Pad that never comes back to its centre (worn, off its calibration) is centred for 10 s, not for ever; and a way back that
// cannot be made when the last mask falls (the lock) leaves the mirror as the real page's copy - a key pressed then goes through - until
// it can.
static void TestMaskLimits() {
    {
        Hid hid;
        Flow flow;
        u32 pad_ms = 0;
        bool opened = false, closed = false;
        Run(
            hid, flow, 13000,
            [&](u32 ms, bool frame) {
                hid.cx = 55;
                hid.keys = ms >= 150 && ms < 2000 ? kKeyB : 0u;
                if (frame && !opened) {
                    opened = true;
                    flow.Want(5000 + ms);
                }
                if (frame && !closed && ms >= 200) {
                    closed = true;
                    flow.Release(5000 + ms);
                }
            },
            [&](u32 ms) {
                // (from 60 ms: the neutral mirror's first two refreshes say "released" for the direction key the game saw held)
                if (ms >= 60 && pad_ms == 0 && (ShownBy(flow.GamePage(hid.real)).circle || (ShownBy(flow.GamePage(hid.real)).keys & kCirclePadKeys) != 0)) {
                    pad_ms = ms;
                }
            });
        CHECK(pad_ms >= 1200 + 10000 && pad_ms < 1200 + 10000 + 34); // (10 s after the cap, 11 s after the close)
        CHECK(flow.said.size() == 3 && std::strcmp(flow.said[1], "let go") == 0 && std::strcmp(flow.said[2], "last, pad capped") == 0);
        CHECK(flow.backs == 1 && !flow.held.on() && flow.back_ms == 5000 + pad_ms);
    }
    {
        Hid hid;
        Flow flow;
        u32 b_ms = 0, neutral_again = 0;
        bool opened = false, closed = false;
        Run(
            hid, flow, 4000,
            [&](u32 ms, bool frame) {
                hid.keys = (ms >= 150 && ms < 1500 ? kKeyB : 0u) | (ms >= 1700 && ms < 2600 ? kKeyB : 0u);
                flow.lock_busy = ms >= 1400 && ms < 2000;
                if (frame && !opened) {
                    opened = true;
                    flow.Want(5000 + ms);
                }
                if (frame && !closed && ms >= 200) {
                    closed = true;
                    flow.Release(5000 + ms);
                }
            },
            [&](u32 ms) {
                if (ms >= 1700 && b_ms == 0 && (HeldAtIndex(flow.GamePage(hid.real)) & kKeyB) != 0) {
                    b_ms = ms;
                }
                if (ms >= 1700 + 40 && ms < 2000 && flow.active && (HeldAtIndex(flow.page) & kKeyB) == 0) {
                    ++neutral_again; // (the mirror that stays is never the neutral one again)
                }
            });
        CHECK(Said(flow, "last") && flow.said.size() == 2);
        CHECK(b_ms >= 1700 && b_ms < 1700 + 5 + 17 + 1 && neutral_again == 0); // B pressed again while the page cannot go back: the game's
        CHECK(flow.backs == 1 && flow.back_ms >= 5000 + 2000 && flow.back_ms < 5000 + 2000 + 18 && !flow.held.on());
    }
}

// A sample of the hid module in the middle of a frame of the masks (the review, 2026-11-13). The module is another process: it adds a
// sample every 4.3 ms whenever it likes - the entry first, then the ring's index - and the plugin's thread can lose the CPU anywhere in
// HeldMask::Write. Here the module is a thread that never stops adding samples, each with its number where nothing is masked (the Circle
// Pad's word of the pad's entry; the touch's entry, with its complement beside it to tell a word read while it was written), and the
// frames run flat out against it. What must hold after EVERY frame: the entry at the mirror's index is not older than the last sample
// that was whole when the frame began. With the headers read from the real page after the entries (how the masks were first written,
// in the neutral refresh's order) an index that moved in between pointed at the mirror's oldest entry - 7 samples before the frame's
// start, 34 ms on a console - for 28% of the samples that came during a frame. Taken first and written last it cannot: the check never
// fails by chance, and a wrong order is caught within a few hundred samples.
static void TestMaskSampleMidWrite() {
    static u8 real[kPageBytes], m[kPageBytes];
    std::memset(real, 0, sizeof(real));
    std::memset(m, 0, sizeof(m));
    for (u32 i = 0; i < kEntries; ++i) {
        Store32(real, kPadEntries + i * kPadEntryStep + kPadHeld, kKeyB); // (B down since the close: the one mask, on for the whole run)
    }
    HeldMask h;
    CHECK(h.Start(m, real, 0) && h.keys() == kKeyB && h.what() == 0);
    const u32 kFirst = 16, kSamples = 20000;
    std::atomic<u32> whole{0};
    std::atomic<bool> stop{false};
    std::thread hid([&] {
        for (u32 seq = 1; !stop.load(std::memory_order_relaxed); ++seq) {
            const u32 i = seq % kEntries, e = kPadEntries + i * kPadEntryStep, t = kTouchEntries + i * kTouchEntryStep;
            Store32(real, e + kPadCircle, seq);
            Store32(real, e + kPadHeld, kKeyB | (seq % 2 != 0 ? kKeyA : 0u)); // (A comes and goes: a key the game must see as it is)
            Store32(real, t, seq);
            Store32(real, t + kTouchFlag, ~seq);
            Store32(real, 0, seq); // (the headers after the entry, as a ring is written)
            Store32(real, kPadIndex, i);
            Store32(real, kTouchHeader, seq);
            Store32(real, kTouchIndex, i);
            whole.store(seq, std::memory_order_release);
            for (volatile u32 spin = 0; spin < 3000; spin = spin + 1) {
            }
        }
    });
    while (whole.load(std::memory_order_acquire) < kFirst) {
    }
    u32 frames = 0, stale_pad = 0, stale_touch = 0, shown_b = 0, a_wrong = 0;
    while (whole.load(std::memory_order_acquire) < kFirst + kSamples) {
        const u32 before = whole.load(std::memory_order_acquire);
        h.Frame(m, real, ++frames);
        const u32 pe = kPadEntries + (Load32(m, kPadIndex) % kEntries) * kPadEntryStep;
        const u32 te = kTouchEntries + (Load32(m, kTouchIndex) % kEntries) * kTouchEntryStep;
        const u32 pad_seq = Load32(m, pe + kPadCircle), held = Load32(m, pe + kPadHeld);
        const u32 touch_seq = Load32(m, te), touch_check = Load32(m, te + kTouchFlag);
        if (static_cast<s32>(pad_seq - before) < 0) {
            ++stale_pad;
        }
        if (touch_check == ~touch_seq && static_cast<s32>(touch_seq - before) < 0) {
            ++stale_touch;
        }
        shown_b |= held & kKeyB;
        if (((held & kKeyA) != 0) != (pad_seq % 2 != 0)) {
            ++a_wrong;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    hid.join();
    CHECK(stale_pad == 0);   // the pad's index never points at an entry of the mirror older than the frame's start
    CHECK(stale_touch == 0); // ... nor the touch's
    CHECK(shown_b == 0 && h.keys() == kKeyB && frames > 0); // (and B, down all along, was never in the entry the game reads)
    CHECK(a_wrong == 0); // (... while A, never masked, was in it exactly when its sample has it)
}

// The keyboard asked for again with the masks on and never shown - the game is not presenting its bottom screen - then closed as "not
// shown" (the review, 2026-11-13): MirrorRelease with the masks on and no MirrorWant before it (Sources/ctr/chat_overlay.cpp, Type: the
// switch is wanted only once the picture is published). The masks go on as if nothing had happened - B, down since the first close, never
// reaches the game, a key pressed meanwhile does - the page goes back once, when B is let go, and the wait that close started, which no
// frame ever looked at, is cancelled by the next opening: its close has its own 50 ms of quiet.
static void TestMaskClosedUnshown() {
    Hid hid;
    Flow flow;
    u32 seen_b_ms = 0, a_ms = 0, b_new_ms = 0, backs_at_third = 0, back1_ms = 0;
    bool opened = false, closed = false, closed_unshown = false, opened3 = false, closed3 = false, masks_at_unshown = false, stale_wait = false;
    Run(
        hid, flow, 6000,
        [&](u32 ms, bool frame) {
            hid.keys = (ms >= 150 && ms < 3000 ? kKeyB : 0u) | (ms >= 2200 && ms < 2300 ? kKeyA : 0u) | (ms >= 4600 && ms < 4700 ? kKeyB : 0u);
            if (frame && !opened) {
                opened = true;
                flow.Want(5000 + ms);
            }
            if (frame && !closed && ms >= 200) {
                closed = true;
                flow.Release(5000 + ms);
            }
            if (frame && !closed_unshown && ms >= 2000) {
                closed_unshown = true;
                masks_at_unshown = flow.held.on() && flow.held.keys() == kKeyB;
                flow.Release(5000 + ms); // (the keyboard of 1990 ms, never shown: closed with no Want)
            }
            if (frame && !opened3 && ms >= 4000) {
                opened3 = true;
                backs_at_third = flow.backs;
                back1_ms = flow.back_ms;
                stale_wait = flow.wait.waiting() && !flow.active && !flow.held.on();
                flow.Want(5000 + ms);
            }
            if (frame && !closed3 && ms >= 4500) {
                closed3 = true;
                flow.Release(5000 + ms);
            }
        },
        [&](u32 ms) {
            if (ms < 30) {
                return;
            }
            const u8* page = flow.GamePage(hid.real);
            if ((ShownBy(page).keys & kKeyB) != 0) {
                if (ms < 4590) {
                    seen_b_ms = seen_b_ms ? seen_b_ms : ms;
                } else {
                    b_new_ms = b_new_ms ? b_new_ms : ms;
                }
            }
            if ((HeldAtIndex(page) & kKeyA) != 0 && a_ms == 0) {
                a_ms = ms;
            }
        });
    CHECK(masks_at_unshown && seen_b_ms == 0); // the B of the first close never reaches the game through the close of a keyboard never shown
    CHECK(a_ms >= 2200 && a_ms < 2200 + 5 + 17 + 1); // (a key pressed after that close is the game's at the next frame, as before it)
    CHECK(flow.said.size() == 2 && Said(flow, "masks on") && Said(flow, "last")); // (one start of the masks, one end: the close restarted nothing)
    CHECK(backs_at_third == 1 && back1_ms >= 5000 + 3000 + 30 && back1_ms < 5000 + 3000 + 60); // the page back once, when B was let go
    CHECK(stale_wait); // (the wait of the unshown close was never looked at: still "waiting", with no mirror ...)
    CHECK(flow.backs == 2 && flow.back_ms >= 5000 + 4500 + 50 && flow.back_ms < 5000 + 4500 + 50 + 17 + 17); // (... and the next close has its own 50 ms)
    CHECK(b_new_ms >= 4600 && b_new_ms < 4600 + 6 && !flow.held.on() && !flow.active);
}

static void TestMaskFlow() {
    TestMaskSampleMidWrite();
    TestMaskConsoleCase();
    TestMaskNormalPath();
    TestMaskTouch();
    TestMaskReopen();
    TestMaskClosedUnshown();
    TestMaskHomeAndExit();
    TestMaskLimits();
}

// ---- a simulated kernel -----------------------------------------------------------------------------------------------------

constexpr u32 kHidPa = 0x1FF6A000;
constexpr u32 kMirrorPa = 0x27F00000;
enum Owner { kReal, kMirror, kOther };

struct Page {
    u32 state, perm, pa;
    Owner owner;
};

// The address space a page at a time; svcQueryMemory's answer is the run of pages around the address with the same state and permissions
// (the kernel keeps neighbours of the same state and permissions as one block).
struct Kernel {
    std::map<u32, Page> pages;
    bool fail_query = false;
    bool lie_size = false; // the mirror's block comes out larger than the page

    void Put(u32 va, u32 state, u32 perm, u32 pa, Owner o) {
        pages[va] = Page{state, perm, pa, o};
    }
    const Page* At(u32 va) const {
        auto it = pages.find(va & ~0xFFFu);
        return it == pages.end() ? nullptr : &it->second;
    }
    bool Query(u32 va, Region* r) const {
        if (fail_query) {
            return false;
        }
        const u32 page = va & ~0xFFFu;
        const Page* p = At(page);
        if (!p) {
            auto next = pages.lower_bound(page);
            r->base = 0;
            if (next != pages.begin()) {
                auto prev = next;
                --prev;
                r->base = prev->first + kPageBytes;
            }
            r->size = (next == pages.end() ? 0xF0000000u : next->first) - r->base;
            r->state = kStateFree;
            r->perm = 0;
            return true;
        }
        u32 lo = page, hi = page + kPageBytes;
        for (const Page* q; (q = At(lo - kPageBytes)) && q->state == p->state && q->perm == p->perm;) {
            lo -= kPageBytes;
        }
        for (const Page* q; (q = At(hi)) && q->state == p->state && q->perm == p->perm;) {
            hi += kPageBytes;
        }
        r->base = lo;
        r->size = hi - lo;
        if (lie_size && p->owner == kMirror) {
            r->size += kPageBytes;
        }
        r->state = p->state;
        r->perm = p->perm;
        return true;
    }
    u32 PaOf(u32 va) const {
        const Page* p = At(va);
        return p ? p->pa : 0;
    }
};

struct Ops {
    Kernel& k;
    explicit Ops(Kernel& kernel) : k(kernel) {}
    bool frozen = false;
    int freezes = 0, unfreezes = 0, settles = 0;
    int changes_unfrozen = 0; // a change of the address space with the game's threads running: never
    s32 threads = 12;
    u32 calls = 12;
    s32 freeze_rc = 0;
    int fail_unmap_real = 0, fail_map_mirror = 0, fail_map_real = 0, fail_unmap_mirror = 0;
    bool unmap_real_noop = false;
    u32 fail_map_mirror_at = 0; // only at this address (0: any)
    int folded_unmaps = 0;      // a mirror unmapped out of a block larger than its page
    int panics = 0;             // the kernel's unmap ran past the block: kernelpanic() on the console
    int foreign_unmapped = 0;   // pages of another block freed by that unmap
    void (*before_map_mirror)(Kernel&, u32) = nullptr; // a block that appears between the neighbour check and the map

    void Change() {
        if (!frozen) {
            ++changes_unfrozen;
        }
    }
    int queries = 0, fail_query_at = -1; // (the fail_query_at-th query fails)
    bool Query(u32 va, Region* r) {
        return queries++ != fail_query_at && k.Query(va, r);
    }
    s32 Freeze(u32* c) {
        ++freezes;
        frozen = true;
        *c = calls;
        return freeze_rc;
    }
    void Unfreeze() {
        ++unfreezes;
        frozen = false;
    }
    void Settle() {
        ++settles;
    }
    s32 Threads() {
        return threads;
    }
    s32 Cap() const {
        return 64;
    }
    s32 UnmapReal(u32 va) {
        Change();
        if (fail_unmap_real > 0) {
            --fail_unmap_real;
            return -1;
        }
        const Page* p = k.At(va);
        if (!p || p->owner != kReal) {
            return -2;
        }
        if (!unmap_real_noop) {
            k.pages.erase(va);
        }
        return 0;
    }
    s32 MapReal(u32 va, u32 perm) {
        Change();
        if (fail_map_real > 0) {
            --fail_map_real;
            return -1;
        }
        if (k.At(va)) {
            return -2;
        }
        k.Put(va, kStateShared, perm, kHidPa, kReal);
        return 0;
    }
    s32 MapMirror(u32 va) {
        Change();
        if (before_map_mirror) {
            before_map_mirror(k, va);
        }
        if (fail_map_mirror > 0 && (fail_map_mirror_at == 0 || fail_map_mirror_at == va)) {
            --fail_map_mirror;
            return -1;
        }
        if (k.At(va)) {
            return -2;
        }
        k.Put(va, kStateShared, kPermR | kPermW, kMirrorPa, kMirror);
        return 0;
    }
    // svcUnmapProcessMemoryEx(CUR, va, 0x1000) as the kernel's KProcessPageTable::UnmapProcessMemory does it (11.14+): QueryInfo(va); a
    // block that ends past va + 0x1000 (folded with one above) ends the loop with success and nothing unmapped; a shared writable one is
    // freed from va (not from its base) over its whole size, so folded with one below past its end: kernelpanic().
    s32 UnmapMirror(u32 va) {
        Change();
        Region r;
        if (k.Query(va, &r) && (r.base != va || r.size != kPageBytes)) {
            ++folded_unmaps; // (one page of a block the kernel folded: what panics on the console)
        }
        if (fail_unmap_mirror > 0) {
            --fail_unmap_mirror;
            return -1;
        }
        if (!k.Query(va, &r) || r.base + r.size > va + kPageBytes) {
            return 0;
        }
        if (r.state == kStateShared && r.perm == (kPermR | kPermW)) {
            for (u32 a = va; a < va + r.size; a += kPageBytes) {
                const Page* p = k.At(a);
                if (!p || a >= r.base + r.size) {
                    ++panics;
                    return 0;
                }
                foreign_unmapped += p->owner != kMirror ? 1 : 0;
                k.pages.erase(a);
            }
        }
        return 0;
    }
};

constexpr u32 kGameVa = 0x10002000;
constexpr u32 kOwnVa = 0x11000000;

// A game's address space: its HID page, the framework's, some other shared blocks and private memory around.
static void Layout(Kernel& k) {
    k.pages.clear();
    k.Put(0x10000000, kStateShared, kPermR, 0x1F000000, kOther);         // another read-only shared block (GSP, say)
    k.Put(kGameVa, kStateShared, kPermR, kHidPa, kReal);                  // the game's HID page
    k.Put(0x10004000, kStateShared, kPermR | kPermW, 0x1F001000, kOther); // a writable one, not a neighbour
    k.Put(kOwnVa, kStateShared, kPermR, kHidPa, kReal);                   // the framework's own
    k.Put(0x08000000, 5, kPermR | kPermW, 0x20000000, kOther);            // the game's heap
}

static bool IsReal(const Kernel& k, u32 va) {
    const Page* p = k.At(va);
    Region r;
    return p && p->owner == kReal && k.Query(va, &r) && r.perm == kPermR;
}
static bool IsMirror(const Kernel& k, u32 va) {
    const Page* p = k.At(va);
    return p && p->owner == kMirror;
}
static bool Balanced(const Ops& o) {
    return !o.frozen && o.changes_unfrozen == 0 && o.unfreezes >= 1;
}

static void PutWritableBelow(Kernel& k, u32 va) {
    k.Put(va - kPageBytes, kStateShared, kPermR | kPermW, 0x1F300000, kOther);
}
static void PutWritableAbove(Kernel& k, u32 va) {
    k.Put(va + kPageBytes, kStateShared, kPermR | kPermW, 0x1F300000, kOther);
}

static Found FindIn(const Kernel& k) {
    return FindHidVas([&k](u32 va, Region* r) { return k.Query(va, r); }, [&k](u32 va) { return k.PaOf(va); }, kHidPa, kOwnVa);
}

static void TestFind() {
    Kernel k;
    Layout(k);
    Found f = FindIn(k);
    CHECK(f.own_seen && f.n == 1 && f.va[0] == kGameVa && f.perm[0] == kPermR && !f.too_many);
    // A second mapping in the middle of a three-page read-only shared block.
    k.Put(0x10010000, kStateShared, kPermR, 0x1F100000, kOther);
    k.Put(0x10011000, kStateShared, kPermR, kHidPa, kReal);
    k.Put(0x10012000, kStateShared, kPermR, 0x1F102000, kOther);
    f = FindIn(k);
    CHECK(f.own_seen && f.n == 2 && f.va[0] == kGameVa && f.va[1] == 0x10011000);
    k.Put(0x12000000, kStateShared, kPermR, kHidPa, kReal);
    f = FindIn(k);
    CHECK(f.n == 2 && f.too_many);
    // The framework's own mapping not found: the caller refuses (its address is not where the search says the page is).
    Layout(k);
    k.pages.erase(kOwnVa);
    f = FindIn(k);
    CHECK(!f.own_seen && f.n == 1);
    // No game mapping at all (a framework that uses the game's own page: only its address is there, and it is excluded).
    Layout(k);
    k.pages.erase(kGameVa);
    f = FindIn(k);
    CHECK(f.own_seen && f.n == 0);
    // Nothing at the edges is taken for the page: private memory and the address space past the search.
    Layout(k);
    k.Put(0x14000000, kStateShared, kPermR, kHidPa, kReal);
    k.Put(0x0FFFF000, kStateShared, kPermR, kHidPa, kReal);
    f = FindIn(k);
    CHECK(f.n == 1 && f.va[0] == kGameVa);
}

static void TestSwitch() {
    Kernel k;
    // Happy path: held across the change, the mirror exactly one page, the way back.
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        CHECK(s.active() && s.count() == 1 && s.va(0) == kGameVa);
        CHECK(IsMirror(k, kGameVa) && IsReal(k, kOwnVa)); // (the framework's own untouched)
        CHECK(Balanced(o) && o.freezes == 2 && o.settles >= 2);
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone && o.freezes == 2); // (already: nothing)
        CHECK(s.Restore(o, true) == Outcome::kDone);
        CHECK(!s.active() && IsReal(k, kGameVa) && Balanced(o));
        // (the zombie probe's threads line: a hold is two Freeze passes and one let-go, so a balanced session reads passes == 2 x let-gos)
        CHECK(o.freezes == 2 * o.unfreezes && o.unfreezes == 2);
        CHECK(s.Restore(o, false) == Outcome::kDone); // (nothing to give back)
    }
    // The hold: the svc failed, an emulator's predicate never called, a thread missed; twice each is fine.
    for (int c = 0; c < 4; ++c) {
        Layout(k);
        Ops o(k);
        Switcher s;
        if (c == 0) {
            o.freeze_rc = static_cast<s32>(0xF8C007F4);
        } else if (c == 1) {
            o.calls = 0;
        } else if (c == 2) {
            o.calls = 11;
        } else {
            o.calls = 24;
        }
        const Outcome out = s.Engage(o, FindIn(k));
        if (c == 3) {
            CHECK(out == Outcome::kDone && IsMirror(k, kGameVa));
        } else {
            CHECK(out == (c == 0 ? Outcome::kFreezeFailed : Outcome::kFreezeUnverified));
            CHECK(!s.active() && IsReal(k, kGameVa) && Balanced(o));
            CHECK(o.freezes == 2 * o.unfreezes); // (a refused hold is still two passes and one let-go)
        }
    }
    // A writable shared neighbour, below or above: no switch. A read-only one: the switch.
    for (int c = 0; c < 3; ++c) {
        Layout(k);
        const u32 at = c == 1 ? kGameVa + kPageBytes : kGameVa - kPageBytes;
        k.Put(at, kStateShared, c == 2 ? kPermR : (kPermR | kPermW), 0x1F200000, kOther);
        Ops o(k);
        Switcher s;
        const Outcome out = s.Engage(o, FindIn(k));
        if (c == 2) {
            CHECK(out == Outcome::kDone && IsMirror(k, kGameVa));
        } else {
            CHECK(out == Outcome::kNeighbour && IsReal(k, kGameVa) && !s.active() && Balanced(o));
        }
    }
    // The mirror not exactly one page: never unmapped as it is (the kernel's unmap of one page of a larger block), kept and known; once
    // it is one page again the way back gives the real page.
    {
        Layout(k);
        k.lie_size = true;
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kNotExact);
        CHECK(IsMirror(k, kGameVa) && s.active() && Balanced(o) && o.folded_unmaps == 0);
        CHECK(s.Restore(o, true) == Outcome::kStuck && IsMirror(k, kGameVa) && s.active() && o.folded_unmaps == 0);
        k.lie_size = false;
        CHECK(s.Restore(o, true) == Outcome::kDone && IsReal(k, kGameVa) && !s.active() && Balanced(o));
    }
    // A writable shared block the game maps right below or above the mirror while it is there (the kernel folds the two): the way back
    // never calls the kernel's unmap on the folded block (below: kernelpanic(), above: nothing done), the mirror stays known, at the swap
    // too; once the block is gone the real page goes back. The same block appearing between the check and the map: kNotExact, kept.
    for (int c = 0; c < 6; ++c) {
        Layout(k);
        const u32 at = (c & 1) ? kGameVa + kPageBytes : kGameVa - kPageBytes;
        Ops o(k);
        Switcher s;
        if (c >= 4) {
            o.before_map_mirror = (c & 1) ? &PutWritableAbove : &PutWritableBelow;
            CHECK(s.Engage(o, FindIn(k)) == Outcome::kNotExact);
            o.before_map_mirror = nullptr;
        } else {
            CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
            k.Put(at, kStateShared, kPermR | kPermW, 0x1F300000, kOther);
            CHECK(s.Restore(o, c < 2) == Outcome::kStuck);
        }
        CHECK(IsMirror(k, kGameVa) && s.active() && k.At(at) && Balanced(o));
        CHECK(o.panics == 0 && o.foreign_unmapped == 0 && o.folded_unmaps == 0);
        k.pages.erase(at);
        CHECK(s.Restore(o, c < 2) == Outcome::kDone && IsReal(k, kGameVa) && !s.active() && Balanced(o) && o.panics == 0);
    }
    // The query after the unmap fails: the address is free but kept as the mirror's; the next way back finds it free and maps the real page
    // (it never stays empty for the guard above).
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.queries = 0;
        o.fail_query_at = 1;
        CHECK(s.Restore(o, true) == Outcome::kStuck && !k.At(kGameVa) && s.active() && Balanced(o));
        o.fail_query_at = -1;
        CHECK(s.Restore(o, true) == Outcome::kDone && IsReal(k, kGameVa) && !s.active() && Balanced(o));
    }
    // The real page could not be unmapped: nothing changed. "Unmapped" but still there: nothing changed either.
    {
        Layout(k);
        Ops o(k);
        o.fail_unmap_real = 1;
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kUnmapFailed && IsReal(k, kGameVa) && !s.active() && Balanced(o));
        Ops o2(k);
        o2.unmap_real_noop = true;
        CHECK(s.Engage(o2, FindIn(k)) == Outcome::kNotFree && IsReal(k, kGameVa) && !s.active() && Balanced(o2));
    }
    // The mirror could not be mapped: the real page back.
    {
        Layout(k);
        Ops o(k);
        o.fail_map_mirror = 1;
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kMapFailed && IsReal(k, kGameVa) && !s.active() && Balanced(o));
    }
    // Two mappings, the second fails: the first is given back.
    {
        Layout(k);
        k.Put(0x10011000, kStateShared, kPermR, kHidPa, kReal);
        Ops o(k);
        o.fail_map_mirror = 1;
        o.fail_map_mirror_at = 0x10011000;
        Switcher s;
        const Found f = FindIn(k);
        CHECK(f.n == 2);
        CHECK(s.Engage(o, f) == Outcome::kMapFailed);
        CHECK(IsReal(k, kGameVa) && IsReal(k, 0x10011000) && !s.active() && Balanced(o));
        Ops o2(k);
        CHECK(s.Engage(o2, f) == Outcome::kDone && IsMirror(k, kGameVa) && IsMirror(k, 0x10011000) && s.count() == 2);
        CHECK(s.Restore(o2, true) == Outcome::kDone && IsReal(k, kGameVa) && IsReal(k, 0x10011000) && Balanced(o2));
    }
    // Two mappings side by side: the second mirror would be folded into the first (both shared and writable), and unmapping one page of
    // the folded block is the kernel panic the neighbour check is there for: no switch, nothing changed.
    {
        Layout(k);
        k.Put(kGameVa - kPageBytes, kStateShared, kPermR, kHidPa, kReal);
        Ops o(k);
        Switcher s;
        const Found f = FindIn(k);
        CHECK(f.n == 2 && f.va[0] == kGameVa - kPageBytes && f.va[1] == kGameVa);
        CHECK(s.Engage(o, f) == Outcome::kNeighbour);
        CHECK(IsReal(k, kGameVa) && IsReal(k, kGameVa - kPageBytes) && !s.active() && Balanced(o) && o.folded_unmaps == 0);
        CHECK(o.freezes == 2 && o.unfreezes == 1); // (refused under the hold, before anything changed)
    }
    // The game's address no longer a shared block (the game unmapped it): no switch. A query that fails: no switch.
    {
        Layout(k);
        const Found f = FindIn(k);
        k.pages.erase(kGameVa);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, f) == Outcome::kNotShared && !s.active() && Balanced(o) && !k.At(kGameVa));
        Layout(k);
        k.fail_query = true;
        Ops o2(k);
        CHECK(s.Engage(o2, f) == Outcome::kQueryFailed && !s.active());
        k.fail_query = false;
        CHECK(IsReal(k, kGameVa) && Balanced(o2));
    }
    // The way back: the real page on the second try.
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.fail_map_real = 1;
        CHECK(s.Restore(o, true) == Outcome::kDone && IsReal(k, kGameVa) && !s.active() && Balanced(o));
    }
    // ... never: the mirror again (outside a swap), then given back later.
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.fail_map_real = 2;
        CHECK(s.Restore(o, true) == Outcome::kFellBack);
        CHECK(IsMirror(k, kGameVa) && s.active() && s.va(0) == kGameVa && Balanced(o));
        CHECK(s.Restore(o, true) == Outcome::kDone && IsReal(k, kGameVa) && !s.active());
    }
    // ... never, at the swap or the exit: the address left EMPTY, never the mirror (its memory goes with the plugin's block).
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.fail_map_real = 5;
        CHECK(s.Restore(o, false) == Outcome::kLeftEmpty);
        CHECK(!k.At(kGameVa) && !s.active() && s.left_empty() == 1 && Balanced(o));
    }
    // The mirror that cannot be unmapped stays known (the next way back tries again).
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.fail_unmap_mirror = 1;
        CHECK(s.Restore(o, false) == Outcome::kStuck && IsMirror(k, kGameVa) && s.active() && Balanced(o));
        CHECK(s.Restore(o, false) == Outcome::kDone && IsReal(k, kGameVa) && !s.active());
    }
    // The way back holds the threads even when the hold fails (the page must go back), and lets them go.
    {
        Layout(k);
        Ops o(k);
        Switcher s;
        CHECK(s.Engage(o, FindIn(k)) == Outcome::kDone);
        o.freeze_rc = -1;
        o.calls = 0;
        CHECK(s.Restore(o, false) == Outcome::kDone && IsReal(k, kGameVa) && !o.frozen);
    }
}

// ---- the hold's predicate ---------------------------------------------------------------------------------------------------

struct KThreadSim {
    u32 words[0x2C] = {};  // the KThread (0xB0 bytes): +0x34 the scheduling mask (bit 0x20: held), +0x94 user TLS, +0x98 kernel alias
    bool current = false;  // running on a core when the svc is called (the caller among them)
    bool held() const { return (words[0x34 / 4] & 0x20u) != 0; }
    void hold(bool on) { words[0x34 / 4] = on ? (words[0x34 / 4] | 0x20u) : (words[0x34 / 4] & ~0x20u); }
};

static u32 g_pred_caller = 0;
static u32 g_pred_off = 0;
static bool Pred(const u32* kthread) { return HoldThread(kthread, g_pred_caller, g_pred_off); }

// Luma3DS 13.x k11_extension/source/svc/ControlProcess.c, PROCESSOP_SCHEDULE_THREADS: lock holds every thread the predicate says, the
// current ones in a second pass (so the caller too, if the predicate says it); unlock lets go of every thread the predicate says.
static void LumaScheduleThreads(std::vector<KThreadSim>& ts, bool lock) {
    if (!lock) {
        for (auto& t : ts) {
            if (Pred(t.words) && t.held()) {
                t.hold(false);
            }
        }
        return;
    }
    bool current_found = false;
    for (auto& t : ts) {
        if (!Pred(t.words)) {
            continue;
        }
        if (t.current) {
            current_found = true;
        } else {
            t.hold(true);
        }
    }
    if (current_found) {
        for (auto& t : ts) {
            if (Pred(t.words) && !t.held()) {
                t.hold(true);
            }
        }
    }
}

static void TestPredicate() {
    // The TLS pages, below 4 GB so that a word holds their address as the kernel's alias does (the 3DS is 32-bit).
    void* mem = mmap(nullptr, 0x1000 * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(mem != MAP_FAILED);
    if (mem == MAP_FAILED) {
        return;
    }
    // 0-3: the game's threads (no mark; 0 is the thread of the APT notifications, 1 runs on the other core), 4: the plugin's thread,
    // 5: the keep-alive thread, 6: the network thread (the mark), 7: a thread without a TLS.
    auto make = [&](int caller) {
        std::vector<KThreadSim> ts(8);
        for (int i = 0; i < 8; ++i) {
            u32* page = reinterpret_cast<u32*>(static_cast<u8*>(mem) + 0x1000 * i);
            page[0] = (i >= 4 && i <= 6) ? kThreadVarsMagic : 0u;
            ts[i].words[kKThreadUserTlsWord] = 0x1FF82000u + 0x200u * static_cast<u32>(i);
            ts[i].words[kKThreadTlsWord] = (i == 7) ? 0u : static_cast<u32>(reinterpret_cast<uintptr_t>(page));
            ts[i].current = (i == caller) || i == 1;
        }
        g_pred_caller = ts[caller].words[kKThreadUserTlsWord]; // getThreadLocalStorage() on the caller: TPIDRURO, the user address
        return ts;
    };
    for (int caller : {0, 4, 5}) {
        std::vector<KThreadSim> ts = make(caller);
        CHECK(ts[caller].words[kKThreadTlsWord] != g_pred_caller); // (the kernel's alias is another number for the same page)
        g_pred_off = 0;
        LumaScheduleThreads(ts, true);
        CHECK(!ts[caller].held()); // the caller never holds itself (from the hook it would stay held with the whole game)
        for (int i = 0; i < 4; ++i) {
            CHECK(i == caller || ts[i].held());
        }
        CHECK(!ts[4].held() && !ts[5].held() && !ts[6].held() && !ts[7].held());
        LumaScheduleThreads(ts, false);
        for (int i = 0; i < 8; ++i) {
            CHECK(!ts[i].held());
        }
    }
    {
        // The framework's pause: nothing held, and a let-go wakes nobody it holds.
        std::vector<KThreadSim> ts = make(4);
        g_pred_off = 1;
        LumaScheduleThreads(ts, true);
        for (int i = 0; i < 8; ++i) {
            CHECK(!ts[i].held());
        }
        ts[2].hold(true);
        LumaScheduleThreads(ts, false);
        CHECK(ts[2].held());
        g_pred_off = 0;
    }
    munmap(mem, 0x1000 * 8);
}

int main() {
    TestLayout();
    TestQuiet();
    TestHeldMask();
    TestMaskFlow();
    TestFind();
    TestSwitch();
    TestPredicate();
    std::printf("hid_mirror_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
