// The input mirror page, its pure parts (azl/hid_mirror.hpp; the console's side is Sources/ctr/input_mirror.cpp), run by Tests/run.sh with
// ASan/UBSan. What it holds:
// - the page: the mirror made from a real page byte by byte - the pad's raw state and raw Circle Pad zero, the 8 pad entries from 0x28 every
//   0x10 {held 0, pressed 0, released = the keys the game saw held, Circle Pad 0}, the raw touch and the 8 touch entries from 0xC8 every 8 zero
//   (the flag at +4 too), everything else (the headers with their ticks and indices at 0x10 and 0xB8, the accelerometer from 0x108, the
//   gyroscope from 0x158, the rest of the page) the real page's; a refresh copies the live parts again and nothing past 0x2B0; "released"
//   for 2 samples, then 0; the keys held now from the entry at the page's index (clamped to 7);
// - the look that says the real page is quiet (no key and the Circle Pad at rest in any entry, no finger), and the wait for the release:
//   50 ms of quiet looked at every frame, at most 1 s, a clock that wraps;
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
    CHECK(!with(kPadRaw, 1));
    CHECK(!circle(kPadRawCircle, kCircleRest + 1, 0));
    CHECK(!with(kTouchRaw + kTouchFlag, 1));

    // The wait: every frame (16 or 17 ms), 50 ms of quiet; any input starts it again; at most 1 s.
    ReleaseWait w;
    CHECK(!w.Update(true, 0)); // (not waiting)
    w.Start(1000);
    CHECK(w.waiting());
    CHECK(!w.Update(true, 1000) && !w.Update(true, 1016) && !w.Update(true, 1033));
    CHECK(w.Update(true, 1050) && !w.waiting());
    w.Start(0);
    u32 t = 0, done_at = 0;
    for (; t < 2000; t += 17) {
        if (w.Update(t >= 300, t)) {
            done_at = t;
            break;
        }
    }
    CHECK(done_at >= 350 && done_at < 350 + 17 + 17);
    w.Start(5);
    done_at = 0;
    for (t = 5; t < 3000; t += 16) {
        if (w.Update(false, t)) {
            done_at = t;
            break;
        }
    }
    CHECK(done_at >= 1005 && done_at < 1005 + 16); // (held all along: 1 s)
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
    TestFind();
    TestSwitch();
    TestPredicate();
    std::printf("hid_mirror_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
