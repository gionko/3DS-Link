// The chat over the running game, its pure parts (azl/chat_host.hpp; the console's side is Sources/ctr/chat_overlay.cpp), run by Tests/run.sh
// with ASan/UBSan and again with ThreadSanitizer. What it holds:
// - the notifications that close the keyboard, read in the LOW BYTE (every value of the byte, 0x608 = the power button of an Old 3DS, Mario
//   Party's 0x1BC00001 = HOME), only when answered, and the framework's events that close it;
// - the rules with notifications and events simulated: what closes, what holds the opening for 2 s, nothing between SWAP_ENTER and
//   SWAP_EXIT (and both between two looks), never after EXIT, a clock that wraps;
// - the chord's own edge, the pace (no window of a second with more than 30 jobs, at a 1 us step and at the 3DS's 59.83 Hz; every other frame
//   at 60 fps, every frame at 30), and the latch: a one-frame press of a button and a one-frame tap between two jobs reach the keyboard
//   (azl/osk.hpp), which a host that looks only at its jobs loses, and a lift of one or two frames between two keys or two A's is not merged
//   (the keyboard types what it types fed every frame, also over 600 random typings);
// - the picture's handshake: an Image is freed only when no copy can read it (sequential, then two real threads under ASan / TSan: the reader
//   checks every Image it gets, the writer poisons and frees);
// - the copy: in the five formats and with a column longer than the screen, exactly the visible pixels the plugin's drawing puts on the
//   framebuffer itself, nothing of the gap rows or of the guards, 32-bit words only and as many as the screen has (the cost of a frame);
// - what the callback draws: nothing with the chat off (not a store), the lines while they are recent or the keyboard is open, the keyboard
//   only in the format it was drawn in (the format told to the plugin's thread first); and no allocation in any of it (malloc wrapped);
// - the keyboard's Send into the chat's box: posted, busy (the draft kept), nothing left of it; and never "nothing" for a real text while
//   the network thread empties the box on another thread (the draft that the first version could throw away);
// - the framework's screen with the high bits of the game's format byte (0x40, 0x20, 0x80341...): the format masked, the bytes a pixel from it
//   (gfx::ScreenSurface), the lines and the keyboard drawn in every format; the framework's own BytesPerPixel would lose 24 of those screens;
// - the plugin's own notifications (NoticeBoard): in the corner opposite the chat for every corner, for 5 s, cleaned and cut at 128, and the
//   top screen's state told to the plugin's thread (top_seen: 0 for a screen the layer refuses, so the framework shows them);
// - our message refused after Send, told once, with why (not in the room, or the room's connection did not take it).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

static unsigned long g_stores[5]; // [1], [2], [4]: stores of that many bytes
#define AZL_GFX_COUNT_STORE(bytes) (++g_stores[(bytes)])

#include "azl/chat_host.hpp"
#include "azl/osk_draw.hpp"

using namespace azl;

namespace azl {
namespace chat {
Signals g_signals;
}
} // namespace azl

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

// ---- every allocation of the process is counted (the link wraps malloc; operator new is replaced to go through it) ----
static std::atomic<unsigned> g_allocs{0};
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(n);
}
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t n) {
    return operator new(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

static void ResetStores() {
    std::memset(g_stores, 0, sizeof(g_stores));
}

// ---------------------------------------------------------------------------------------------
// The signals
// ---------------------------------------------------------------------------------------------

static void TestSignals() {
    for (u32 b = 0; b < 256; ++b) {
        const bool closes = b == 1 || b == 2 || b == 3 || b == 5 || b == 7 || b == 8 || b == 9 || b == 10 || b == 11;
        const bool holds = b == 1 || b == 2 || b == 8 || b == 9;
        CHECK(chat::AptClosesKeyboard(b) == closes);
        CHECK(chat::AptHoldsKeyboard(b) == holds);
        CHECK(!holds || closes); // (what holds the opening closes too)
        // the byte above the signal is left over (NS writes one byte): whatever it holds, the low byte decides
        CHECK(chat::AptLowSignal(0x5A00u | b) == b);
        CHECK(chat::AptLowSignal(0xFFFFFF00u | b) == b);
    }
    CHECK(chat::AptLowSignal(0x608) == 8);      // the power button of an Old 3DS (issue 5: 16 bits read it as 0x608, which closed nothing)
    CHECK(chat::AptLowSignal(0x1BC00001) == 1); // HOME in Mario Party: The Top 100
    CHECK(chat::AptLowSignal(0x100) == 0);

    chat::Signals s;
    // answered only: a failed call, or a service that said no, is no notification
    chat::NoteAptNotification(s, -1, 0, 1);
    chat::NoteAptNotification(s, 0, 0xC8A0CFF0u, 1);
    CHECK(s.apt_close.load() == 0 && s.apt_hold.load() == 0);
    // the ones that do not close: none, sleep cancelled, wake-up, and a byte past 11
    for (u32 raw : {0u, 4u, 6u, 12u, 0x100u, 0xFF00u}) {
        chat::NoteAptNotification(s, 0, 0, raw);
    }
    CHECK(s.apt_close.load() == 0 && s.apt_hold.load() == 0);
    chat::NoteAptNotification(s, 0, 0, 0x608); // power, Old 3DS
    CHECK(s.apt_close.load() == 1 && s.apt_hold.load() == 1);
    chat::NoteAptNotification(s, 0, 0, 3); // sleep query: closes, no hold
    CHECK(s.apt_close.load() == 2 && s.apt_hold.load() == 1);
    chat::NoteAptNotification(s, 0, 0, 0x1BC00001); // HOME
    CHECK(s.apt_close.load() == 3 && s.apt_hold.load() == 2);
    chat::NoteAptNotification(s, 0, 0, 11);
    CHECK(s.apt_close.load() == 4 && s.apt_hold.load() == 2);

    // The closing events, one list for the counters and the input mirror's way back (HOME_ENTER, SWAP_ENTER, SLEEP_ENTER, EXIT)
    static_assert(chat::IsClosingEvent(chat::kEventHomeEnter) && chat::IsClosingEvent(chat::kEventSwapEnter), "the closing events");
    CHECK(chat::IsClosingEvent(3) && chat::IsClosingEvent(5) && chat::IsClosingEvent(1) && chat::IsClosingEvent(0));
    CHECK(!chat::IsClosingEvent(2) && !chat::IsClosingEvent(4) && !chat::IsClosingEvent(6) && !chat::IsClosingEvent(7) && !chat::IsClosingEvent(-1));

    chat::Signals e;
    chat::NoteProcessEvent(e, 2); // SLEEP_EXIT
    chat::NoteProcessEvent(e, 4); // HOME_EXIT
    CHECK(e.fw_close.load() == 0 && e.swap_enter.load() == 0 && e.swap_exit.load() == 0 && e.exits.load() == 0);
    chat::NoteProcessEvent(e, chat::kEventHomeEnter);
    chat::NoteProcessEvent(e, chat::kEventSleepEnter);
    CHECK(e.fw_close.load() == 2);
    chat::NoteProcessEvent(e, chat::kEventSwapEnter);
    CHECK(e.fw_close.load() == 3 && e.swap_enter.load() == 1);
    chat::NoteProcessEvent(e, chat::kEventSwapExit); // (SWAP_EXIT closes nothing: the keyboard is closed already)
    CHECK(e.fw_close.load() == 3 && e.swap_exit.load() == 1);
    chat::NoteProcessEvent(e, chat::kEventExit);
    CHECK(e.fw_close.load() == 4 && e.exits.load() == 1);
}

// ---------------------------------------------------------------------------------------------
// The rules, with notifications and events simulated
// ---------------------------------------------------------------------------------------------

struct Rig {
    chat::Signals s;
    chat::CloseRules rules;
    u32 now = 100000;
    bool Look() {
        return rules.Update(chat::Look(s), now);
    }
    void Apt(u32 raw) {
        chat::NoteAptNotification(s, 0, 0, raw);
    }
    void Event(int e) {
        chat::NoteProcessEvent(s, e);
    }
};

static void TestRules() {
    {
        Rig r;
        CHECK(!r.rules.MayOpen(r.now)); // (no look yet)
        r.Apt(1);                       // before the first look: closes nothing, holds nothing (no keyboard was open)
        r.Event(chat::kEventHomeEnter);
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        CHECK(!r.Look()); // nothing new
        // a sleep query: closes, may open again at once
        r.Apt(3);
        CHECK(r.Look());
        CHECK(r.rules.MayOpen(r.now));
        CHECK(!r.Look());
        // HOME: closes, and no opening for 2 s
        r.Apt(1);
        r.now += 16;
        CHECK(r.Look());
        CHECK(!r.rules.MayOpen(r.now));
        r.now += chat::kReopenHoldMs - 1;
        CHECK(!r.Look());
        CHECK(!r.rules.MayOpen(r.now));
        r.now += 1;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        // the power button of an Old 3DS (0x608): the same
        r.Apt(0x608);
        CHECK(r.Look());
        CHECK(!r.rules.MayOpen(r.now + 1000));
        CHECK(r.rules.MayOpen(r.now + chat::kReopenHoldMs));
        // a notification the service refused, or one the game was not given: nothing
        chat::NoteAptNotification(r.s, -5, 0, 1);
        chat::NoteAptNotification(r.s, 0, 0x80000000u, 9);
        r.now += 5000;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        // a second HOME during the hold makes it longer
        r.Apt(2);
        CHECK(r.Look());
        r.now += 1500;
        r.Apt(9);
        CHECK(r.Look());
        r.now += 1000;
        CHECK(!r.Look());
        CHECK(!r.rules.MayOpen(r.now)); // (2.5 s after the first, 1 s after the second)
        r.now += 1000;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        // the framework's HOME_ENTER and SLEEP_ENTER close (on a New 3DS HOME_ENTER may come with no notification seen)
        r.Event(chat::kEventHomeEnter);
        CHECK(r.Look());
        CHECK(r.rules.MayOpen(r.now));
        r.Event(chat::kEventSleepEnter);
        CHECK(r.Look());
        // SWAP_ENTER: closed, and nothing opens until SWAP_EXIT and 2 s after it
        r.Event(chat::kEventSwapEnter);
        CHECK(r.Look());
        for (int i = 0; i < 100; ++i) {
            r.now += 100;
            CHECK(r.Look()); // (still in the swap: whatever was opened would close)
            CHECK(!r.rules.MayOpen(r.now));
        }
        r.Event(chat::kEventSwapExit);
        r.now += 10;
        CHECK(!r.Look());
        CHECK(!r.rules.MayOpen(r.now));
        r.now += chat::kReopenHoldMs;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        // the whole swap between two looks (the plugin's thread was stopped through it): closes and holds
        r.Event(chat::kEventSwapEnter);
        r.Event(chat::kEventSwapExit);
        r.now += 3000;
        CHECK(r.Look());
        CHECK(!r.rules.MayOpen(r.now));
        r.now += chat::kReopenHoldMs;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        // HOME and power in the HOME menu of an Old 3DS whose notifications the hook no longer sees: SWAP_ENTER alone is the net
        r.Event(chat::kEventSwapEnter);
        CHECK(r.Look());
        CHECK(!r.rules.MayOpen(r.now + 60000));
        r.Event(chat::kEventSwapExit);
        CHECK(!r.Look());
        // EXIT: closes, and never opens again
        r.now += 10000;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        r.Event(chat::kEventExit);
        CHECK(r.Look());
        for (int i = 0; i < 10; ++i) {
            r.now += 100000;
            CHECK(r.Look());
            CHECK(!r.rules.MayOpen(r.now));
        }
    }
    {
        // the first look inside a swap: no opening
        Rig r;
        r.Event(chat::kEventSwapEnter);
        r.Look();
        CHECK(!r.rules.MayOpen(r.now));
        r.Event(chat::kEventSwapExit);
        r.Look();
        CHECK(!r.rules.MayOpen(r.now));
        CHECK(r.rules.MayOpen(r.now + chat::kReopenHoldMs));
    }
    {
        // a clock that wraps: the hold ends 2 s later all the same, and does not come back 49 days later
        Rig r;
        r.now = 0xFFFFFFFFu - 500;
        r.Look();
        r.Apt(1);
        CHECK(r.Look());
        r.now += 1000; // (past the wrap)
        CHECK(!r.Look());
        CHECK(!r.rules.MayOpen(r.now));
        r.now += 1000;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
        r.now += 0x80000000u;
        CHECK(!r.Look());
        CHECK(r.rules.MayOpen(r.now));
    }
}

// ---------------------------------------------------------------------------------------------
// The chord, the pace, the latch
// ---------------------------------------------------------------------------------------------

static void TestChord() {
    const u32 chord = osk::kKeyL | osk::kKeyLeft | osk::kKeySelect;
    chat::ChordEdge e;
    CHECK(!e.Update(chord, chord)); // held when first looked at (the game started with it held)
    CHECK(!e.Update(chord, chord));
    CHECK(!e.Update(osk::kKeyL | osk::kKeySelect, chord));
    CHECK(e.Update(chord | osk::kKeyA, chord)); // (other keys with it do not matter)
    CHECK(!e.Update(chord, chord));            // once
    CHECK(!e.Update(0, chord));
    CHECK(!e.Update(osk::kKeyLeft, chord));
    CHECK(e.Update(chord, chord));
    chat::ChordEdge none;
    CHECK(!none.Update(0, 0));
    CHECK(!none.Update(0xFFFFFFFFu, 0)); // (an empty chord never opens)
}

static void TestPace() {
    // a host that looks every microsecond for 3 s: no window of a second holds more than 30 jobs, and every job is 1/30 s after the last
    // (the first one 2 ms sooner: its place 2 ms before it)
    const u64 gap = chat::WorkGap(1000000); // (us: 33400, 1/30 s and 1/500 s rounded up as the console's ticks are)
    const u64 early = chat::WorkEarly(1000000);
    CHECK(gap == 33400 && early == 2000);
    CHECK(chat::WorkGap(268111856) == 8954936); // (the console's: two frames of 59.83 Hz, 8962306 ticks, are more)
    CHECK(chat::WorkEarly(268111856) == 536223);
    {
        chat::WorkPacer p(gap, early);
        std::vector<u64> jobs;
        for (u64 t = 0; t < 3000000; ++t) {
            if (p.Due(t)) {
                jobs.push_back(t);
            }
        }
        CHECK(jobs.size() >= 89 && jobs.size() <= 91);
        bool spaced = true;
        for (std::size_t i = 1; i < jobs.size(); ++i) {
            spaced = spaced && jobs[i] - jobs[i - 1] >= (i == 1 ? gap - early : gap);
        }
        CHECK(spaced);
        std::size_t worst = 0;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            std::size_t n = 0;
            for (std::size_t j = i; j < jobs.size() && jobs[j] < jobs[i] + 1000000; ++j) {
                ++n;
            }
            worst = n > worst ? n : worst;
        }
        CHECK(worst <= chat::kWorkPerSecond);
        std::printf("pace: %zu jobs in 3 s at a 1 us step, at most %zu in a second\n", jobs.size(), worst);
    }
    {
        // the 3DS's frames (59.83 Hz, 16714 us): a job every other frame
        chat::WorkPacer p(gap, early);
        u32 jobs = 0, every_other = 0;
        int last = -2;
        for (int f = 0; f < 600; ++f) {
            if (p.Due(static_cast<u64>(f) * 16714)) {
                ++jobs;
                every_other += f - last == 2 ? 1 : 0;
                last = f;
            }
        }
        CHECK(jobs == 300);
        CHECK(every_other == 300);
    }
    {
        // a game at 30 fps: every frame; one that stalls: the next frame, then the pace again (no burst after the stall)
        chat::WorkPacer p(gap, early);
        u32 jobs = 0;
        for (int f = 0; f < 300; ++f) {
            jobs += p.Due(static_cast<u64>(f) * 33428) ? 1 : 0;
        }
        CHECK(jobs == 300);
        CHECK(p.Due(10000000 + 5000000));
        CHECK(!p.Due(10000000 + 5000000 + 16714));
        CHECK(p.Due(10000000 + 5000000 + 33428));
        p.Reset();
        CHECK(p.Due(10000000 + 5000000 + 33429)); // (a new keyboard: its first look at once)
    }
}

// The frames as the plugin's thread sees them: the time of the frame callback moves by tenths of a millisecond (the game's present, the
// network's work before the keyboard's). At 59.83 Hz and at 30 fps, with a jitter of up to +-1 ms, for 60 s: a job every other frame (every
// frame at 30 fps), 29 or 30 in every second, never more than 30 in any window of a second. The first pace (no place, the step from the last
// job) took 22-26 a second at 60 fps with +-0.5 ms, 18-23 at 30 fps.
static u64 PaceRand(u64& seed) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return seed >> 33;
}

static std::size_t PaceWorst(const std::vector<u64>& jobs, u64 window) {
    std::size_t worst = 0, j = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        while (j < jobs.size() && jobs[j] < jobs[i] + window) {
            ++j;
        }
        worst = j - i > worst ? j - i : worst;
    }
    return worst;
}

static void TestPaceJitter() {
    const u64 gap = chat::WorkGap(1000000), early = chat::WorkEarly(1000000);
    struct Case {
        u64 frame_ns; // (a frame, in ns: 59.83 Hz or 29.92)
        u64 jitter_ns;
        u64 per_job;  // frames a job
    };
    const Case cases[] = {{16714000, 0, 2}, {16714000, 100000, 2}, {16714000, 500000, 2}, {16714000, 1000000, 2},
                          {33428000, 500000, 1}, {33428000, 1000000, 1}};
    u64 seed = 37;
    for (const Case& c : cases) {
        chat::WorkPacer p(gap, early);
        std::vector<u64> jobs;
        u32 per_second[61] = {};
        u64 last_frame = 0;
        bool every = true;
        const u64 frames = 60000000000ull / c.frame_ns;
        for (u64 f = 0; f < frames; ++f) {
            const u64 jit = c.jitter_ns ? PaceRand(seed) % (2 * c.jitter_ns + 1) : 0;
            const u64 t = (2000000 + f * c.frame_ns + jit - c.jitter_ns) / 1000; // (us)
            if (p.Due(t)) {
                if (!jobs.empty()) {
                    every = every && f - last_frame == c.per_job;
                }
                last_frame = f;
                jobs.push_back(t);
                per_second[f * c.frame_ns / 1000000000ull]++; // (the second of the frame)
            }
        }
        u32 lo = 1000, hi = 0;
        for (u32 s = 0; s < 59; ++s) { // (the last second is cut by the end of the frames)
            lo = per_second[s] < lo ? per_second[s] : lo;
            hi = per_second[s] > hi ? per_second[s] : hi;
        }
        const std::size_t worst = PaceWorst(jobs, 1000000);
        CHECK(every);
        CHECK(worst <= chat::kWorkPerSecond);
        CHECK(lo >= 29 && hi <= chat::kWorkPerSecond);
        std::printf("  pace: %llu us frames, +-%llu us: %zu jobs in 60 s, %u-%u a second, at most %zu in a window of a second\n",
                    static_cast<unsigned long long>(c.frame_ns / 1000), static_cast<unsigned long long>(c.jitter_ns / 1000), jobs.size(), lo, hi,
                    worst);
    }
    // looks at random times (steps of 0-40 ms, bursts of steps of 0-3 ms, stalls of 0.1-2 s): never more than 30 in a window of a second
    for (int trial = 0; trial < 50; ++trial) {
        chat::WorkPacer p(gap, early);
        std::vector<u64> jobs;
        u64 t = PaceRand(seed) % 1000;
        for (int i = 0; i < 20000; ++i) {
            const u64 r = PaceRand(seed);
            t += r % 97 == 0 ? 100000 + r % 1900000 : (r & 0x100) ? r % 3001 : r % 40001;
            if (p.Due(t)) {
                jobs.push_back(t);
            }
        }
        CHECK(PaceWorst(jobs, 1000000) <= chat::kWorkPerSecond);
        bool spaced = true;
        for (std::size_t i = 1; i < jobs.size(); ++i) {
            spaced = spaced && jobs[i] - jobs[i - 1] >= gap - early;
        }
        CHECK(spaced);
    }
}

// A one-frame press and a one-frame tap, at 59.83 Hz, between two jobs of the pace: the keyboard gets them through the latch.
static void TestLatch() {
    const u64 gap = chat::WorkGap(1000000);
    const osk::KeyDef& q = osk::kKeys[osk::FindKey(osk::kAbc, osk::kChar, 'q')];
    struct Frame {
        u32 held;
        bool touch;
    };
    // frames 0..: idle, a tap on 'q' on frame 7 only (an odd frame: no job on it), idle, B on frame 13 only (deletes the 'q'), idle, B on
    // frame 19 only (no text: closes)
    constexpr std::size_t kFrames = 26;
    Frame frames[kFrames] = {};
    frames[7] = Frame{osk::kKeyTouch, true};
    frames[13] = Frame{osk::kKeyB, false};
    frames[19] = Frame{osk::kKeyB, false};
    for (int latched = 1; latched >= 0; --latched) {
        osk::Osk k(osk::Options{});
        chat::WorkPacer pacer(gap);
        chat::PadLatch latch;
        osk::Result last = osk::Result::kNone;
        std::size_t len_after_tap = 0;
        bool jobs_on_odd = false;
        for (std::size_t f = 0; f < kFrames; ++f) {
            const u64 t = static_cast<u64>(f) * 16714;
            const Frame& fr = frames[f];
            const u16 x = static_cast<u16>(q.x + q.w / 2), y = static_cast<u16>(q.y + q.h / 2);
            latch.Sample(fr.held, fr.touch, x, y);
            if (!pacer.Due(t)) {
                continue;
            }
            jobs_on_odd = jobs_on_odd || (f & 1);
            osk::Input in = latch.Take(static_cast<u32>(t / 1000));
            if (!latched) {
                in.held = fr.held; // (a host that looks only at its jobs)
                in.touch = fr.touch;
                in.tx = x;
                in.ty = y;
            }
            const osk::Result r = k.Feed(in);
            if (r != osk::Result::kNone) {
                last = r;
                break;
            }
            if (f == 10) {
                len_after_tap = k.length();
            }
        }
        CHECK(!jobs_on_odd);
        if (latched) {
            CHECK(len_after_tap == 1);
            CHECK(last == osk::Result::kCancel);
        } else {
            CHECK(len_after_tap == 0); // (lost: the reason for the latch)
            CHECK(last == osk::Result::kNone);
        }
    }
    // a key held across many frames is held at every job, and its release is seen
    chat::PadLatch latch;
    latch.Sample(osk::kKeyA, false, 0, 0);
    latch.Sample(osk::kKeyA | osk::kKeyR, false, 0, 0);
    osk::Input in = latch.Take(5);
    CHECK(in.held == (osk::kKeyA | osk::kKeyR) && !in.touch && in.now_ms == 5);
    latch.Sample(osk::kKeyA, true, 10, 20);
    latch.Sample(osk::kKeyA, true, 11, 21);
    in = latch.Take(6);
    CHECK(in.held == osk::kKeyA && in.touch && in.tx == 11 && in.ty == 21);
    latch.Sample(0, false, 0, 0);
    in = latch.Take(7);
    CHECK(in.held == 0 && !in.touch);
}

// A release shorter than the pace (the stylus lifted for one or two frames between two keys, A pressed twice in a hurry), at 59.83 Hz with
// the jobs on the even or on the odd frames: the keyboard types what it types when it is fed every frame. The first latch merged the lift
// (the touch OR-ed over the window): "q, one frame up, w" typed nothing at all, "q, one frame up, q" and two A's one q.
struct LiftFrame {
    u32 held;
    bool touch;
    char key; // (the key under the finger)
};

static u32 LiftRun(const LiftFrame* fr, std::size_t n, bool latched, std::size_t phase, char* text) {
    osk::Osk k(osk::Options{});
    chat::WorkPacer pacer(chat::WorkGap(1000000));
    chat::PadLatch latch;
    for (std::size_t f = 0; f < n; ++f) {
        u16 x = 0, y = 0;
        if (fr[f].touch) {
            const osk::KeyDef& kd = osk::kKeys[osk::FindKey(osk::kAbc, osk::kChar, fr[f].key)];
            x = static_cast<u16>(kd.x + kd.w / 2);
            y = static_cast<u16>(kd.y + kd.h / 2);
        }
        const u64 t = static_cast<u64>(f) * 16714;
        if (!latched) {
            osk::Input in;
            in.held = fr[f].held;
            in.touch = fr[f].touch;
            in.tx = x;
            in.ty = y;
            in.now_ms = static_cast<u32>(t / 1000);
            k.Feed(in);
            continue;
        }
        latch.Sample(fr[f].held, fr[f].touch, x, y);
        if (f < phase || !pacer.Due(t)) {
            continue;
        }
        k.Feed(latch.Take(static_cast<u32>(t / 1000)));
    }
    std::memcpy(text, k.text(), k.length());
    text[k.length()] = 0;
    return k.length();
}

static std::size_t LiftPush(LiftFrame* fr, std::size_t n, std::size_t frames, u32 held, bool touch, char key) {
    for (std::size_t i = 0; i < frames; ++i) {
        fr[n++] = LiftFrame{held | (touch ? osk::kKeyTouch : 0u), touch, key};
    }
    return n;
}

static void TestLatchLifts() {
    constexpr std::size_t kMax = 400;
    static LiftFrame fr[kMax];
    char got[osk::kTextMax + 1], want[osk::kTextMax + 1];
    struct Case {
        std::size_t a;
        char ka;
        std::size_t lift;
        std::size_t b;
        char kb;
        const char* text;
    };
    // (A: the first press only shows the selection, 'q' at the opening; the next two type it. Not held: a one-frame tap, a one-frame lift and
    // a one-frame tap, three edges in one job's two frames, where a bit keeps two: 50 ms for two keys is no stylus)
    const Case cases[] = {{3, 'q', 1, 3, 'w', "qw"}, {5, 'q', 1, 5, 'w', "qw"}, {3, 'q', 2, 3, 'w', "qw"}, {3, 'q', 1, 3, 'q', "qq"},
                          {3, 'q', 2, 3, 'q', "qq"}, {1, 'q', 2, 1, 'w', "qw"}, {3, 0, 1, 3, 0, "q"}};
    for (const Case& c : cases) {
        for (std::size_t phase = 0; phase < 2; ++phase) {
            std::size_t n = LiftPush(fr, 0, 10, 0, false, 0);
            if (!c.ka) {
                n = LiftPush(fr, n, 3, osk::kKeyA, false, 0);
                n = LiftPush(fr, n, 4, 0, false, 0);
            }
            n = LiftPush(fr, n, c.a, c.ka ? 0 : osk::kKeyA, c.ka != 0, c.ka);
            n = LiftPush(fr, n, c.lift, 0, false, 0);
            n = LiftPush(fr, n, c.b, c.kb ? 0 : osk::kKeyA, c.kb != 0, c.kb);
            n = LiftPush(fr, n, 10, 0, false, 0);
            LiftRun(fr, n, true, phase, got);
            const std::size_t len = std::strlen(c.text) + (c.ka ? 0 : 1);
            if (c.ka) {
                CHECK(std::strcmp(got, c.text) == 0);
            } else {
                CHECK(std::strlen(got) == len && got[0] == 'q' && got[1] == 'q');
            }
            LiftRun(fr, n, false, 0, want);
            CHECK(std::strcmp(got, want) == 0);
        }
    }
    // a frame of no touch while the stylus rests on a key is a lift as it is fed every frame: two taps, no more (and no fewer)
    {
        std::size_t n = LiftPush(fr, 0, 10, 0, false, 0);
        n = LiftPush(fr, n, 6, 0, true, 'e');
        n = LiftPush(fr, n, 1, 0, false, 0);
        n = LiftPush(fr, n, 6, 0, true, 'e');
        n = LiftPush(fr, n, 10, 0, false, 0);
        for (std::size_t phase = 0; phase < 2; ++phase) {
            LiftRun(fr, n, true, phase, got);
            CHECK(std::strcmp(got, "ee") == 0);
        }
    }
    // random typing, presses and touches of 3-8 frames, lifts of 1-4: the same text as an every-frame keyboard, in both phases
    const char keys[] = "qwertyuiopasdfghjklzxcvbnm";
    u32 seed = 12345;
    int same = 0, runs = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::size_t n = LiftPush(fr, 0, 10, 0, false, 0);
        while (n + 8 + 4 + 10 < kMax) {
            seed = seed * 1103515245u + 12345u;
            const u32 r = seed >> 8;
            const bool touch = (r & 3) != 0;
            n = LiftPush(fr, n, 3 + (r >> 2) % 6, touch ? 0 : osk::kKeyA, touch, keys[(r >> 5) % 26]);
            n = LiftPush(fr, n, 1 + (r >> 10) % 4, 0, false, 0);
        }
        n = LiftPush(fr, n, 10, 0, false, 0);
        LiftRun(fr, n, false, 0, want);
        for (std::size_t phase = 0; phase < 2; ++phase) {
            LiftRun(fr, n, true, phase, got);
            ++runs;
            same += std::strcmp(got, want) == 0 ? 1 : 0;
        }
    }
    CHECK(same == runs);
    std::printf("  latch: %d of %d random typings as an every-frame keyboard\n", same, runs);
}

// ---------------------------------------------------------------------------------------------
// The picture's handshake
// ---------------------------------------------------------------------------------------------

static void TestSlotSequential() {
    chat::ImageSlot slot;
    chat::ImageKeeper keeper;
    CHECK(slot.Enter() == nullptr);
    slot.Leave();
    CHECK(keeper.Freeable(slot) == nullptr && keeper.Empty());
    chat::Image a, b;
    slot.Publish(&a);
    CHECK(slot.Enter() == &a); // a copy under way ...
    CHECK(keeper.Retire(slot));
    CHECK(!keeper.Empty());
    CHECK(slot.shown() == nullptr);
    CHECK(keeper.Freeable(slot) == nullptr); // ... holds it
    CHECK(keeper.Freeable(slot) == nullptr);
    // a copy that starts now finds nothing
    CHECK(slot.Enter() == nullptr);
    slot.Leave();
    CHECK(keeper.Freeable(slot) == nullptr);
    // a second one cannot be retired while the first waits
    slot.Publish(&b);
    CHECK(!keeper.Retire(slot));
    CHECK(slot.shown() == &b);
    slot.Leave(); // the first copy ends
    CHECK(keeper.Freeable(slot) == &a);
    CHECK(keeper.Empty());
    CHECK(keeper.Freeable(slot) == nullptr);
    CHECK(keeper.Retire(slot));
    CHECK(keeper.Freeable(slot) == &b); // (no copy under way: at once)
    CHECK(keeper.Retire(slot));         // (nothing shown: nothing to wait for)
    CHECK(keeper.Freeable(slot) == nullptr && keeper.Empty());
}

// Two real threads: the game's copies whatever Image it finds and checks every byte of it; the plugin's publishes, retires, and poisons and
// frees an Image as soon as the keeper gives it back. A copy that read a freed Image is a use-after-free (ASan), or a byte that is not the
// Image's (the check), or a race (TSan).
static void TestSlotThreads() {
    constexpr std::size_t kBytes = 2048;
    chat::ImageSlot slot;
    std::atomic<bool> stop{false};
    std::atomic<u32> bad{0};
    std::atomic<u64> copies{0}, empties{0};
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const chat::Image* im = slot.Enter();
            if (im) {
                const u8 want = im->format;
                u32 wrong = 0;
                for (std::size_t i = 0; i < kBytes; ++i) {
                    wrong += im->pixels[i] != want;
                }
                if (wrong) {
                    bad.fetch_add(1);
                }
                copies.fetch_add(1, std::memory_order_relaxed);
            } else {
                empties.fetch_add(1, std::memory_order_relaxed);
            }
            slot.Leave();
        }
    });
    chat::ImageKeeper keeper;
    u32 freed = 0;
    for (u32 round = 0; round < 3000; ++round) {
        chat::Image* im = new chat::Image();
        im->pixels = new u8[kBytes];
        im->format = static_cast<u8>(round % 250 + 1);
        std::memset(im->pixels, im->format, kBytes);
        slot.Publish(im);
        if (round % 500 == 0) {
            // (every 500 rounds the writer waits, 2 s at most, for one copy of the picture shown: on a loaded machine the reader may not
            // run at all within the three yields of every round, and copies stayed 0 with the handshake right; a copy counted here is of
            // this picture, the one before was freed only once no copy could still count it)
            const u64 before = copies.load();
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (copies.load() == before && std::chrono::steady_clock::now() < until) {
                std::this_thread::yield();
            }
        }
        for (int i = 0; i < 3; ++i) {
            std::this_thread::yield();
        }
        CHECK(keeper.Retire(slot));
        for (;;) {
            chat::Image* done = keeper.Freeable(slot);
            if (done) {
                CHECK(done == im);
                std::memset(done->pixels, 0, kBytes); // (poison: a copy still reading would see it)
                delete[] done->pixels;
                delete done;
                ++freed;
                break;
            }
            std::this_thread::yield();
        }
    }
    stop.store(true);
    reader.join();
    CHECK(bad.load() == 0);
    CHECK(freed == 3000);
    CHECK(copies.load() > 0);
    std::printf("handshake: 3000 pictures freed, %llu copies of them, %llu frames with none\n", static_cast<unsigned long long>(copies.load()),
                static_cast<unsigned long long>(empties.load()));
}

// ---------------------------------------------------------------------------------------------
// The copy and the callback
// ---------------------------------------------------------------------------------------------

// A bottom framebuffer as the framework hands it: 320 columns of `rows` pixels (240 visible, then the gap of a longer column), guards around.
struct Fb {
    static constexpr std::size_t kGuard = 64;
    std::vector<u8> mem;
    gfx::Surface s;
    Fb(u32 format, u32 rows, u32 w = 320) {
        const u32 bpp = gfx::BytesPerPixel(format);
        mem.assign(kGuard * 2 + static_cast<std::size_t>(w) * rows * bpp, 0);
        for (std::size_t i = 0; i < mem.size(); ++i) {
            mem[i] = static_cast<u8>(i * 7 + 3); // (a "game" in every byte, the guards too)
        }
        s.left = mem.data() + kGuard;
        s.stride = rows * bpp;
        s.bpp = static_cast<u8>(bpp);
        s.format = static_cast<u8>(format);
        s.w = static_cast<u16>(w);
        s.h = 240;
    }
    bool Visible(std::size_t off) const { // (an offset from s.left)
        return off % s.stride < 240u * s.bpp;
    }
};

struct OwnedImage {
    std::vector<u8> pixels;
    chat::Image im;
    explicit OwnedImage(u32 format, u32 col_bytes = 0) {
        im.format = static_cast<u8>(format);
        im.bpp = static_cast<u8>(gfx::BytesPerPixel(format));
        im.col_bytes = col_bytes ? col_bytes : chat::kImageH * im.bpp;
        pixels.assign(chat::ImageBytes(format) + 64, 0);
        im.pixels = pixels.data();
    }
};

static void TestCopy() {
    osk::Osk k(osk::Options{}, "the draft of a message");
    osk::View v;
    k.Snapshot(&v);
    for (u32 format = 0; format < gfx::kFormats; ++format) {
        for (u32 rows : {240u, 256u}) {
            OwnedImage img(format);
            osk::DrawOsk(chat::ImageSurface(img.im), v);
            // the plugin's drawing put straight on a framebuffer (the paused menu's way) ...
            Fb direct(format, rows);
            osk::DrawOsk(direct.s, v);
            // ... and the Image copied by the game's thread
            Fb copied(format, rows);
            const std::vector<u8> before = copied.mem;
            CHECK(chat::ImageFits(img.im, copied.s));
            ResetStores();
            const unsigned allocs = g_allocs.load();
            chat::CopyImage(img.im, copied.s);
            CHECK(g_allocs.load() == allocs);
            const u32 bpp = gfx::BytesPerPixel(format);
            CHECK(g_stores[4] == 320ul * 240 * bpp / 4); // (the cost of a frame: the screen's bytes, in words)
            CHECK(g_stores[1] == 0 && g_stores[2] == 0);
            u32 visible_diff = 0, outside_changed = 0;
            for (std::size_t i = 0; i < copied.mem.size(); ++i) {
                const bool inside = i >= Fb::kGuard && i < Fb::kGuard + 320u * copied.s.stride && copied.Visible(i - Fb::kGuard);
                if (inside) {
                    visible_diff += copied.mem[i] != direct.mem[i];
                } else {
                    outside_changed += copied.mem[i] != before[i];
                }
            }
            CHECK(visible_diff == 0);
            CHECK(outside_changed == 0);
        }
    }
    // what does not fit is not copied: another format, another pixel, a column too short, a picture of another layout
    OwnedImage img(gfx::kBGR8);
    Fb other(gfx::kRGB565, 240);
    CHECK(!chat::ImageFits(img.im, other.s));
    Fb top(gfx::kBGR8, 240, 400);
    CHECK(!chat::ImageFits(img.im, top.s));
    Fb ok(gfx::kBGR8, 240);
    CHECK(chat::ImageFits(img.im, ok.s));
    ok.s.stride = 239 * 3 + 0; // (shorter than a column: and not a word apart)
    CHECK(!chat::ImageFits(img.im, ok.s));
    OwnedImage odd(gfx::kBGR8, 240 * 3 + 12);
    Fb ok2(gfx::kBGR8, 256);
    CHECK(!chat::ImageFits(odd.im, ok2.s));
    // RGB565, RGB5A1 and RGBA4 are all 2 bytes a pixel: only the format tells them apart (a game that turns its bottom screen from one to
    // another would get one or two frames of the keyboard in the wrong colours)
    const u32 sixteen[] = {gfx::kRGB565, gfx::kRGB5A1, gfx::kRGBA4};
    for (const u32 fi : sixteen) {
        for (const u32 ff : sixteen) {
            OwnedImage im16(fi);
            Fb fb16(ff, 240);
            CHECK(chat::ImageFits(im16.im, fb16.s) == (fi == ff));
        }
    }
}

static std::size_t PacketOf(u8* out, const char* nick, const char* msg) {
    std::size_t p = 0;
    out[p++] = chat::kIdChatMessage;
    const char* fields[3] = {nick, "user", msg};
    for (const char* f : fields) {
        const u32 n = static_cast<u32>(std::strlen(f));
        StoreBE32(out + p, n);
        std::memcpy(out + p + 4, f, n);
        p += 4 + n;
    }
    return p;
}

static u32 g_draw_allocs = 0; // allocations made inside DrawOverlay (the game's thread): must stay 0

static u32 Draw(chat::Overlay& o, const gfx::Surface& s, bool top, u32 now_ms) {
    const unsigned before = g_allocs.load();
    const u32 drew = chat::DrawOverlay(o, s, top, now_ms);
    g_draw_allocs += g_allocs.load() - before;
    return drew;
}

static void TestOverlay() {
    chat::Overlay o;
    static ChatSink g_sink; // (the plugin's is never freed either)
    ChatSink* sink = &g_sink;
    u8 packet[256];
    const u32 t0 = 50000;
    CHECK(sink->OnPacket(packet, PacketOf(packet, "Gionko", "hello from the room"), t0));
    Fb top(gfx::kBGR8, 240, 400);
    Fb bottom(gfx::kRGB565, 240);

    // off: nothing, not a store, the format of the bottom screen not even looked at
    o.sink.store(sink);
    o.keyboard.store(1);
    ResetStores();
    CHECK(Draw(o, top.s, true, t0 + 10) == chat::kDrewNothing);
    CHECK(Draw(o, bottom.s, false, t0 + 10) == chat::kDrewNothing);
    CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
    CHECK(o.bottom_geometry.load() == 0);
    o.keyboard.store(0);

    // on: the lines while they are recent (10 s after the last), then nothing
    o.on.store(1);
    CHECK(Draw(o, top.s, true, t0 + 10) == chat::kDrewLines);
    CHECK(g_stores[4] > 0);
    CHECK(Draw(o, top.s, true, t0 + chat::kShowMs - 1) == chat::kDrewLines);
    ResetStores();
    CHECK(Draw(o, top.s, true, t0 + chat::kShowMs) == chat::kDrewNothing);
    CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
    // the keyboard closed: the bottom screen is not touched and its format not told
    CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewNothing);
    CHECK(o.bottom_geometry.load() == 0);
    // the keyboard open: the lines stay, whatever their age
    o.keyboard.store(1);
    CHECK(Draw(o, top.s, true, t0 + 600000) == chat::kDrewLines);
    // ... and the bottom screen: first its format is told (no Image yet: nothing drawn)
    ResetStores();
    CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewNothing);
    CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
    const u32 geometry = o.bottom_geometry.load();
    CHECK(geometry == chat::GeometryOf(bottom.s));
    CHECK(chat::GeometryFormat(geometry) == gfx::kRGB565 && chat::GeometryBpp(geometry) == 2);
    // an Image of another format is not copied
    OwnedImage wrong(gfx::kBGR8);
    o.image.Publish(&wrong.im);
    CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewNothing);
    CHECK(o.image.Quiet());
    // ... nor one of another 16-bit format (the same bytes a pixel as the screen's RGB565)
    OwnedImage alpha1(gfx::kRGB5A1), alpha4(gfx::kRGBA4);
    for (chat::Image* im16 : {&alpha1.im, &alpha4.im}) {
        o.image.Publish(im16);
        ResetStores();
        CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewNothing);
        CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
        CHECK(o.image.Quiet());
    }
    // the plugin's thread draws one in the format told: copied, every frame
    OwnedImage right(chat::GeometryFormat(geometry));
    osk::Osk k(osk::Options{});
    osk::View v;
    k.Snapshot(&v);
    osk::DrawOsk(chat::ImageSurface(right.im), v);
    o.image.Publish(&right.im);
    ResetStores();
    CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewKeyboard);
    CHECK(g_stores[4] == 320ul * 240 * 2 / 4 && g_stores[1] == 0 && g_stores[2] == 0);
    CHECK(o.image.Quiet()); // (the copy has left the slot)
    // the top screen with the keyboard open never gets the Image
    ResetStores();
    CHECK(Draw(o, top.s, true, t0) == chat::kDrewLines);
    // the bottom-right corner climbs above the notifications on screen (the plugin's thread notes them)
    o.corner.store(chat::kBottomRight);
    o.notices.Note(t0);
    CHECK(Draw(o, top.s, true, t0 + 100) == chat::kDrewLines);
    // closed again: the bottom screen is the game's at once, the lines go after 10 s
    o.keyboard.store(0);
    CHECK(Draw(o, bottom.s, false, t0) == chat::kDrewNothing);
    CHECK(Draw(o, top.s, true, t0 + chat::kShowMs + 5) == chat::kDrewNothing);
    // no chat block yet (the chat on, its block not made): nothing
    chat::Overlay empty;
    empty.on.store(1);
    CHECK(Draw(empty, top.s, true, t0) == chat::kDrewNothing);
    CHECK(g_draw_allocs == 0); // (the game's thread allocates nothing)
    o.image.Withdraw();
}

// The framework's screen as the callback takes it (gfx::ScreenSurface, what Sources/ctr/chat_overlay.cpp's SurfaceOf calls): the framework
// hands the game's format byte unmasked (0x40 the top screen in 2D, 0x20 the 3D; libctrpf 0.8.0's OSDImpl::CallbackCommon) and works its
// Screen::BytesPerPixel out of it unmasked (GetBPP: 0 -> 4, 2..4 -> 2, anything else 3). In every format with every high bit the surface is the
// masked format's, Usable, the lines drawn on the top screen and the keyboard told and copied on the bottom one; the framework's bytes a pixel
// would not be (the defect: no line and no keyboard in RGBA8, RGB565, RGB5A1 and RGBA4 with a high bit). 5-7 are no format: never drawn.
static u32 FrameworkBpp(u32 raw) {
    const u32 f = raw & 0xFF; // (uxtb: the framework keeps the low byte)
    return f == 0 ? 4 : (f >= 2 && f <= 4) ? 2 : 3;
}

static void TestScreenFormats() {
    static ChatSink sink;
    u8 packet[256];
    const u32 t0 = 70000;
    CHECK(sink.OnPacket(packet, PacketOf(packet, "Gionko", "a format with a high bit"), t0));
    const u32 highs[] = {0x0, 0x20, 0x40, 0x60, 0x300, 0x320, 0x340, 0x80300, 0x80340};
    u32 framework_lost = 0;
    for (u32 f = 0; f < gfx::kFormats; ++f) {
        for (const u32 high : highs) {
            const u32 raw = high | f;
            Fb top(f, 240, 400), bottom(f, 240);
            const gfx::Surface ts = gfx::ScreenSurface(top.s.left, nullptr, top.s.stride, raw, 400, 240);
            const gfx::Surface bs = gfx::ScreenSurface(bottom.s.left, nullptr, bottom.s.stride, raw, 320, 240);
            CHECK(ts.format == f && ts.bpp == gfx::BytesPerPixel(f) && ts.w == 400 && ts.h == 240 && ts.stride == top.s.stride);
            CHECK(gfx::Usable(ts) && gfx::Usable(bs));
            gfx::Surface fw = ts;
            fw.bpp = static_cast<u8>(FrameworkBpp(raw));
            framework_lost += gfx::Usable(fw) ? 0 : 1;
            chat::Overlay o;
            o.sink.store(&sink);
            o.on.store(1);
            o.keyboard.store(1);
            CHECK(Draw(o, ts, true, t0 + 10) == chat::kDrewLines);
            CHECK(Draw(o, bs, false, t0) == chat::kDrewNothing); // (the format told first)
            const u32 geometry = o.bottom_geometry.load();
            CHECK(chat::GeometryFormat(geometry) == f && chat::GeometryBpp(geometry) == gfx::BytesPerPixel(f));
            OwnedImage im(chat::GeometryFormat(geometry));
            o.image.Publish(&im.im);
            CHECK(Draw(o, bs, false, t0) == chat::kDrewKeyboard);
            o.image.Withdraw();
        }
    }
    // (bit 5 or 6 in the low byte - 0x20, 0x40, 0x60, 0x320, 0x340, 0x80340 - in RGBA8 and the three 16-bit formats: BGR8's 3 is right by chance)
    CHECK(framework_lost == 6 * 4);
    for (u32 raw = 5; raw < 8; ++raw) {
        Fb top(gfx::kRGBA8, 240, 400);
        CHECK(!gfx::Usable(gfx::ScreenSurface(top.s.left, nullptr, top.s.stride, raw | 0x40, 400, 240)));
    }
    CHECK(g_draw_allocs == 0);
}

// The keyboard's Send into the chat's box (SubmitDraft): posted, busy (the draft kept) or nothing left of it (spaces only). Then the race
// that lost a draft: the network thread empties the box while Send is deciding; a text with something in it is never "nothing to send".
static void TestSubmit() {
    u8 packet[chat::kPacketMax];
    {
        ChatSink sink(0);
        CHECK(chat::SubmitDraft(sink, "   ", 3) == chat::Sent::kNothing);
        CHECK(!sink.OutboxBusy());
        CHECK(chat::SubmitDraft(sink, "hello", 5) == chat::Sent::kPosted);
        CHECK(sink.OutboxBusy());
        CHECK(chat::SubmitDraft(sink, "again", 5) == chat::Sent::kBusy); // (the box full: kept, not lost)
        CHECK(chat::SubmitDraft(sink, "   ", 3) == chat::Sent::kBusy);   // (busy wins: the text is not looked at)
        CHECK(sink.TakeOutgoing(1000, true, packet) == 5 + 5);
        sink.SentOutgoing("me", 2, 1000, true);
        CHECK(chat::SubmitDraft(sink, "again", 5) == chat::Sent::kPosted);
    }
    {
        // two threads: the network thread empties the box as fast as it can, Send posts a real text over and over
        ChatSink sink(0);
        std::atomic<bool> stop{false};
        std::thread net([&]() {
            u8 p[chat::kPacketMax];
            u32 now = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (sink.TakeOutgoing(++now, true, p) != 0) {
                    sink.SentOutgoing("me", 2, now, true);
                }
            }
        });
        u32 posted = 0, busy = 0, nothing = 0;
        for (int i = 0; i < 200000 && posted < 2000; ++i) {
            switch (chat::SubmitDraft(sink, "hi there", 8)) {
            case chat::Sent::kPosted:
                ++posted;
                break;
            case chat::Sent::kBusy:
                ++busy;
                break;
            case chat::Sent::kNothing:
                ++nothing;
                break;
            }
        }
        stop.store(true);
        net.join();
        CHECK(nothing == 0);
        CHECK(posted > 0);
        std::printf("  submit: %u posted, %u busy, %u lost\n", posted, busy, nothing);
    }
}

// Our message after Send (PostWatch): the keyboard closed and the draft forgotten, the network thread may still refuse it (the room left
// before its round, or ENet not taking the packet). Refused tells it once, when the box is free again, and the text is still in the box;
// a message that went, one still waiting and a refusal of a message not watched tell nothing.
static void TestPostWatch() {
    u8 packet[chat::kPacketMax];
    ChatSink sink(0);
    chat::PostWatch watch;
    CHECK(!watch.Refused(sink)); // (nothing posted)
    const auto post = [&](const char* text) {
        const u32 left = sink.counters.refused_left.load();
        const u32 refused = sink.counters.refused.load();
        CHECK(chat::SubmitDraft(sink, text, static_cast<u32>(std::strlen(text))) == chat::Sent::kPosted);
        watch.Posted(refused, left);
    };
    // the room left before the network thread's round
    post("hello there");
    CHECK(!watch.Refused(sink)); // (still in the box)
    CHECK(!watch.Refused(sink));
    CHECK(sink.TakeOutgoing(1000, false, packet) == 0);
    CHECK(watch.Refused(sink));
    CHECK(watch.NotInRoom()); // (the reason the chat gives: not in the room)
    CHECK(std::strcmp(sink.Outbox(), "hello there") == 0);
    CHECK(!watch.Refused(sink)); // (once)
    // sent: nothing to tell
    post("second");
    CHECK(sink.TakeOutgoing(2000, true, packet) == 5 + 6);
    CHECK(!watch.Refused(sink)); // (taken but not given to ENet yet: the box is still full)
    sink.SentOutgoing("me", 2, 2000, true);
    CHECK(!watch.Refused(sink));
    // ENet does not take the packet
    post("third");
    CHECK(sink.TakeOutgoing(3000, true, packet) == 5 + 5);
    sink.SentOutgoing("me", 2, 3000, false);
    CHECK(watch.Refused(sink));
    CHECK(!watch.NotInRoom()); // (the room's connection did not take it)
    CHECK(std::strcmp(sink.Outbox(), "third") == 0);
    // a refusal counted before the watch was armed (the snapshot is taken before the Post) is not this message's
    post("fourth");
    CHECK(sink.TakeOutgoing(4000, true, packet) == 5 + 6);
    sink.SentOutgoing("me", 2, 4000, true);
    CHECK(!watch.Refused(sink));
    CHECK(sink.counters.sent.load() == 2 && sink.counters.refused.load() == 2 && sink.counters.refused_left.load() == 1);
}

// A pixel of a fake framebuffer is `c` in its format.
static bool IsColour(const Fb& fb, int x, int y, gfx::Rgb c) {
    const u8* p = fb.s.left + static_cast<std::size_t>(x) * fb.s.stride + static_cast<std::size_t>(239 - y) * fb.s.bpp;
    const u32 raw = gfx::Raw(c, fb.s.format);
    for (u32 i = 0; i < fb.s.bpp; ++i) {
        if (p[i] != static_cast<u8>(raw >> (8 * i))) {
            return false;
        }
    }
    return true;
}

// The plugin's own notifications (NoticeBoard, the user's request of 2026-11-08): drawn by the callback in the corner OPPOSITE the chat
// (bottom-left -> top-right, top-left -> bottom-right, bottom-right -> top-left, top-right -> bottom-left), never over it, for 5 s; cleaned like
// a message and cut at 128 with "..."; a top screen the layer refuses draws nothing and says so (top_seen 0: the plugin's thread then hands
// its notifications to the framework). With the chat off the callback draws nothing at all, notifications included.
static void TestNotices() {
    static ChatSink sink;
    u8 packet[256];
    const u32 t0 = 90000;
    CHECK(sink.OnPacket(packet, PacketOf(packet, "Gionko", "the chat's own line"), t0));
    const char* note = "Joined room \"Test\" (2 of 4 players)";
    const int note_w = chat::BlockWidth(static_cast<u32>(std::strlen(note)));
    const int chat_w = chat::BlockWidth(6 + 2 + 19);
    const gfx::Rgb nick = chat::kNick[chat::NickColour("Gionko", 6)];
    const auto x_of = [](int corner, int width) { return corner == chat::kTopRight || corner == chat::kBottomRight ? 390 - width : 6; };
    const auto y_of = [](int corner) { return corner == chat::kTopLeft || corner == chat::kTopRight ? 8 : 236 - 12; };
    const int across_of[4] = {chat::kBottomRight, chat::kBottomLeft, chat::kTopRight, chat::kTopLeft};
    for (int corner = 0; corner < 4; ++corner) {
        chat::Overlay o;
        o.sink.store(&sink);
        o.on.store(1);
        o.corner.store(static_cast<u32>(corner));
        CHECK(o.board.Show(note, t0));
        const int across = chat::Opposite(corner);
        CHECK(across == across_of[corner]);
        Fb top(gfx::kBGR8, 240, 400);
        CHECK(Draw(o, top.s, true, t0 + 10) == chat::kDrewLines);
        CHECK(o.top_seen.load() == ((t0 + 10) | 1u));
        // the notification's bar in the opposite corner, the chat's in its own, and neither where the other is
        CHECK(IsColour(top, x_of(across, note_w), y_of(across), chat::kNoticeBar));
        CHECK(IsColour(top, x_of(across, note_w) + 1, y_of(across) + 11, chat::kNoticeBar));
        CHECK(IsColour(top, x_of(corner, chat_w), y_of(corner), nick));
        CHECK(!IsColour(top, x_of(corner, note_w), y_of(corner), chat::kNoticeBar));
        CHECK(!IsColour(top, x_of(across, chat_w), y_of(across), nick));
        // 5 s later the notification is gone, the chat's line (10 s) is still there
        Fb later(gfx::kBGR8, 240, 400);
        CHECK(Draw(o, later.s, true, t0 + chat::kNoticeShowMs + 1) == chat::kDrewLines);
        CHECK(!IsColour(later, x_of(across, note_w), y_of(across), chat::kNoticeBar));
        CHECK(IsColour(later, x_of(corner, chat_w), y_of(corner), nick));
        // the chat off: nothing at all, the notification neither
        o.on.store(0);
        Fb off(gfx::kBGR8, 240, 400);
        ResetStores();
        CHECK(Draw(o, off.s, true, t0 + 20) == chat::kDrewNothing);
        CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
    }
    // a notification alone (no chat line young enough, or no chat block yet) is drawn all the same
    {
        chat::Overlay o;
        o.on.store(1);
        o.corner.store(chat::kBottomLeft);
        CHECK(o.board.Show(note, t0));
        Fb top(gfx::kBGR8, 240, 400);
        CHECK(Draw(o, top.s, true, t0 + 10) == chat::kDrewLines);
        CHECK(IsColour(top, x_of(chat::kTopRight, note_w), y_of(chat::kTopRight), chat::kNoticeBar));
    }
    // a top screen the layer refuses (here one narrower than 400): nothing drawn, and the plugin's thread is told
    {
        chat::Overlay o;
        o.sink.store(&sink);
        o.on.store(1);
        o.top_seen.store(12345);
        CHECK(o.board.Show(note, t0));
        Fb narrow(gfx::kBGR8, 240, 320);
        ResetStores();
        CHECK(Draw(o, narrow.s, true, t0 + 10) == chat::kDrewNothing);
        CHECK(g_stores[1] + g_stores[2] + g_stores[4] == 0);
        CHECK(o.top_seen.load() == 0);
    }
    // cleaned and cut like a message: nothing left is not shown; longer than 128 ends with "..."
    {
        chat::NoticeBoard board;
        CHECK(!board.Show("\x01\x02  ", t0));
        CHECK(board.Refresh().count == 0);
        const std::string longer(200, 'n');
        CHECK(board.Show(longer.c_str(), t0));
        const chat::View& v = board.Refresh();
        CHECK(v.count == 1 && v.lines[0].nick_len == 0 && v.lines[0].text_len == chat::kTextMax);
        CHECK(std::strcmp(v.lines[0].text + chat::kTextMax - 3, "...") == 0);
        CHECK(chat::ShownNotices(v, t0 + 100) == 1 && chat::ShownNotices(v, t0 + chat::kNoticeShowMs) == 0);
    }
    CHECK(g_draw_allocs == 0);
}

int main() {
    TestSignals();
    TestRules();
    TestChord();
    TestPace();
    TestPaceJitter();
    TestLatch();
    TestLatchLifts();
    TestSlotSequential();
    TestSlotThreads();
    TestCopy();
    TestOverlay();
    TestScreenFormats();
    TestSubmit();
    TestPostWatch();
    TestNotices();
    std::printf("chat_host_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
