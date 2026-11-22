// H5 (1.1.4): the wait of the host of the real Download Play for the guests that went to start their child (azl/dlp_child_wait.hpp), on a PC,
// pure: a clock of the test's, the room's presence a table of the test's. The caps are the user's (analysis/piano-1.1.4-2026-11-15/specifiche/
// REGOLE-IMPLEMENTAZIONE.md): 15 s at most, 10 s with an Azahar among the guests (one that never leaves the room: it holds nothing after 5 s), 6 s
// after the first one back. The times below are written out, not the constants of the header.
//   1 off: nothing noted, nothing awaited            7 two consoles, A back at 4 s, B never: the end at 10 s (4 + 6)
//   2 armed with nobody noted: nothing               8 a guest the room does not list at its final: not noted
//   3 out at 2.5 s and back at 8 s: the end at 8 s   9 out and back with the room never seeing it out: as one that stayed, 5 s
//   4 out and never back: the end at 15 s           10 Arm twice, Reset, a slot past the seven, Note after Arm
//   5 an Azahar that stays: the end at 5 s          11 a clock that goes back
//   6 an Azahar and a slow console: the end at 10 s 12 the end is given once; a wait that is over awaits nobody
#include <cstdio>
#include <cstring>

#include "azl/dlp_child_wait.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                        \
    do {                                                                                                      \
        ++g_checks;                                                                                           \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                                    \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                                    \
        if (va_ != vb_) {                                                                                     \
            ++g_failures;                                                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                                     \
    } while (0)

using azl::Mac;
using azl::u32;
using azl::u8;
using Wait = azl::dlp_real::ChildWait;

Mac M(u8 i) {
    return Mac{0x40, 0xF4, 0x07, 0x00, 0x00, i};
}

/// The room as the test says it: who is listed now.
struct Room {
    bool in[8] = {};
    bool operator()(const Mac& m) const {
        return m[5] < 8 && in[m[5]];
    }
};

/// Steps the wait every 10 ms from `from` up to `to` (included) with the room `room` (changed by `at` before each step); the time of the end
/// (0xFFFFFFFF = not ended) and the end itself. The mask after every step is checked against `want_mask(t)` when given.
template <typename At>
u32 Run(Wait& w, Room& room, u32 from, u32 to, At at, Wait::End* end) {
    for (u32 t = from; t <= to; t += 10) {
        at(t, room);
        if (w.Step(t, room, end)) {
            return t;
        }
    }
    return 0xFFFFFFFFu;
}
void NoChange(u32, Room&) {}

/// A wait on, `n` guests noted in slots 0..n-1, all listed, armed at `t0`.
void Begin(Wait& w, Room& room, u32 n, u32 t0) {
    w.SetOn(true);
    w.Reset();
    for (u32 s = 0; s < n; ++s) {
        room.in[s] = true;
        w.Note(s, M(static_cast<u8>(s)), true);
    }
    w.Arm(t0);
}

void T1_Off() {
    std::printf("1 off\n");
    Wait w;
    CHECK(!w.on());
    w.Note(0, M(0), true);
    w.Arm(1000);
    CHECK_EQ(w.Awaited(), 0);
    Room room;
    Wait::End e;
    CHECK(!w.Step(2000, room, &e));
    CHECK(!w.Step(40000, room, &e));
    w.SetOn(true);
    w.Arm(1000);
    CHECK_EQ(w.Awaited(), 0);  // (nothing was noted while it was off)
}

void T2_NobodyNoted() {
    std::printf("2 armed with nobody noted\n");
    Wait w;
    w.SetOn(true);
    w.Arm(500);
    CHECK_EQ(w.Awaited(), 0);
    Room room;
    Wait::End e;
    CHECK(!w.Step(600, room, &e));
}

void T3_OutAndBack() {
    std::printf("3 out at +2.5 s, back at +8 s\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 1000);
    CHECK_EQ(w.Awaited(), 0x01);
    Wait::End e = {};
    const u32 t = Run(w, room, 1000, 30000, [](u32 now, Room& r) { r.in[0] = !(now >= 3500 && now < 9000); }, &e);
    CHECK_EQ(t, 9000u);  // the first step that sees it back
    CHECK_EQ(e.waited_ms, 8000);
    CHECK_EQ(e.back, 1);
    CHECK_EQ(e.stayed, 0);
    CHECK_EQ(e.missing, 0);
    CHECK_EQ(w.Awaited(), 0);
    // the mask is 1 until then (a second run, stepped by hand)
    Wait w2;
    Room r2;
    Begin(w2, r2, 1, 0);
    r2.in[0] = false;
    CHECK(!w2.Step(2500, r2, &e));
    CHECK_EQ(w2.Awaited(), 0x01);
    CHECK(!w2.Step(7990, r2, &e));
    CHECK_EQ(w2.Awaited(), 0x01);
    r2.in[0] = true;
    CHECK(w2.Step(8000, r2, &e));
    CHECK_EQ(w2.Awaited(), 0);
    CHECK_EQ(e.waited_ms, 8000);
}

void T4_NeverBack() {
    std::printf("4 out and never back\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 0);
    Wait::End e = {};
    const u32 t = Run(w, room, 0, 40000, [](u32 now, Room& r) { r.in[0] = now < 2500; }, &e);
    CHECK_EQ(t, 15000u);
    CHECK_EQ(e.waited_ms, 15000);
    CHECK_EQ(e.missing, 1);
    CHECK_EQ(e.back, 0);
    CHECK_EQ(e.stayed, 0);
    // and the mask was 1 at 14990
    Wait w2;
    Room r2;
    Begin(w2, r2, 1, 0);
    r2.in[0] = false;
    CHECK(!w2.Step(14990, r2, &e));
    CHECK_EQ(w2.Awaited(), 0x01);
    CHECK(w2.Step(15000, r2, &e));
}

void T5_AzaharStays() {
    std::printf("5 an Azahar stays in the room\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 100);
    Wait::End e = {};
    const u32 t = Run(w, room, 100, 40000, NoChange, &e);
    CHECK_EQ(t, 5100u);
    CHECK_EQ(e.waited_ms, 5000);
    CHECK_EQ(e.stayed, 1);
    CHECK_EQ(e.back, 0);
    CHECK_EQ(e.missing, 0);
}

void T6_AzaharAndSlowConsole() {
    std::printf("6 an Azahar that stays and a console that is slow\n");
    Wait w;
    Room room;
    Begin(w, room, 2, 0);
    Wait::End e = {};
    const u32 t = Run(w, room, 0, 40000, [](u32 now, Room& r) { r.in[1] = now < 1000 || now >= 12000; }, &e);
    CHECK_EQ(t, 10000u);  // the cap with an Azahar: before the console's child is back
    CHECK_EQ(e.stayed, 1);
    CHECK_EQ(e.missing, 1);
    CHECK_EQ(e.back, 0);
    // without the Azahar the same console would be awaited up to 12 s
    Wait w2;
    Room r2;
    Begin(w2, r2, 1, 0);
    Wait::End e2 = {};
    CHECK_EQ(Run(w2, r2, 0, 40000, [](u32 now, Room& r) { r.in[0] = now < 1000 || now >= 12000; }, &e2), 12000u);
    CHECK_EQ(e2.back, 1);
}

void T7_OneBackOneNot() {
    std::printf("7 two consoles, A back at 4 s and B never\n");
    Wait w;
    Room room;
    Begin(w, room, 2, 0);
    Wait::End e = {};
    const u32 t = Run(w, room, 0, 40000,
                      [](u32 now, Room& r) {
                          r.in[0] = now < 1000 || now >= 4000;
                          r.in[1] = now < 1000;
                      },
                      &e);
    CHECK_EQ(t, 10000u);  // 4 s + the 6 s of grace
    CHECK_EQ(e.back, 1);
    CHECK_EQ(e.missing, 1);
    CHECK_EQ(e.stayed, 0);
    // both back: the end with the second
    Wait w2;
    Room r2;
    Begin(w2, r2, 2, 0);
    Wait::End e2 = {};
    CHECK_EQ(Run(w2, r2, 0, 40000,
                 [](u32 now, Room& r) {
                     r.in[0] = now < 1000 || now >= 4000;
                     r.in[1] = now < 1000 || now >= 7000;
                 },
                 &e2),
             7000u);
    CHECK_EQ(e2.back, 2);
    CHECK_EQ(e2.missing, 0);
    // three: A back at 2 s, B at 7 s, C never - the grace counts from the FIRST one back: the end at 8 s, not 6 s after B
    Wait w3;
    Room r3;
    Begin(w3, r3, 3, 0);
    Wait::End e3 = {};
    CHECK_EQ(Run(w3, r3, 0, 40000,
                 [](u32 now, Room& r) {
                     r.in[0] = now < 1000 || now >= 2000;
                     r.in[1] = now < 1000 || now >= 7000;
                     r.in[2] = now < 1000;
                 },
                 &e3),
             8000u);
    CHECK_EQ(e3.back, 2);
    CHECK_EQ(e3.missing, 1);
}

void T8_NotListed() {
    std::printf("8 a guest the room does not list at its final\n");
    Wait w;
    w.SetOn(true);
    w.Note(0, M(0), false);
    w.Note(1, M(1), true);
    CHECK_EQ(w.Awaited(), 0);  // (noted, not yet counted: nobody is awaited before the Download Play's network ends)
    w.Arm(0);
    CHECK_EQ(w.Awaited(), 0x02);
    Wait w2;
    w2.SetOn(true);
    w2.Note(3, M(3), false);
    w2.Arm(0);
    CHECK_EQ(w2.Awaited(), 0);
}

void T9_NeverSeenOut() {
    std::printf("9 out and back without the room ever showing it out\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 0);
    Wait::End e = {};
    CHECK_EQ(Run(w, room, 0, 40000, NoChange, &e), 5000u);
    CHECK_EQ(e.stayed, 1);
    CHECK_EQ(e.back, 0);
}

void T10_Edges() {
    std::printf("10 Arm twice, Reset, slot 7, Note after Arm\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 1000);
    w.Arm(4000);  // (already armed: the count is the first one's)
    Wait::End e = {};
    CHECK_EQ(Run(w, room, 1000, 40000, NoChange, &e), 6000u);
    // Reset voids a wait and keeps it on
    Begin(w, room, 2, 0);
    CHECK_EQ(w.Awaited(), 0x03);
    w.Reset();
    CHECK_EQ(w.Awaited(), 0);
    CHECK(w.on());
    CHECK(!w.Step(100, room, &e));
    // slot 7 and above: ignored
    w.Note(7, M(7), true);
    w.Note(200, M(1), true);
    w.Arm(0);
    CHECK_EQ(w.Awaited(), 0);
    // the seventh slot is one
    w.Note(6, M(6), true);
    w.Arm(0);
    CHECK_EQ(w.Awaited(), 0x40);
    // a Note after the count started is not taken
    w.Note(2, M(2), true);
    CHECK_EQ(w.Awaited(), 0x40);
    // SetOn(false) stops new notes, not a wait under way
    w.SetOn(false);
    CHECK_EQ(w.Awaited(), 0x40);
}

void T11_ClockBack() {
    std::printf("11 a clock that goes back\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 10000);
    Wait::End e = {};
    CHECK(!w.Step(9000, room, &e));  // (before the start: no time, nothing ends)
    CHECK(!w.Step(2000, room, &e));
    CHECK_EQ(w.Awaited(), 0x01);
    CHECK(w.Step(15000, room, &e));
    CHECK_EQ(e.waited_ms, 5000);
    // the clock wraps around 32 bits
    Wait w2;
    Room r2;
    Begin(w2, r2, 1, 0xFFFFF000u);
    r2.in[0] = false;
    CHECK(!w2.Step(0xFFFFF100u, r2, &e));
    CHECK(!w2.Step(0x00001000u, r2, &e));  // 8.2 s later
    CHECK(w2.Step(0x00002A98u, r2, &e));   // 15000 ms after the start
    CHECK_EQ(e.waited_ms, 15000);
}

void T12_Once() {
    std::printf("12 the end is given once\n");
    Wait w;
    Room room;
    Begin(w, room, 1, 0);
    Wait::End e = {};
    CHECK(w.Step(5000, room, &e));
    for (u32 t = 5000; t < 60000; t += 1000) {
        CHECK(!w.Step(t, room, &e));
    }
    CHECK_EQ(w.Awaited(), 0);
    // a new wait after it
    w.Note(0, M(0), true);
    w.Arm(70000);
    CHECK_EQ(w.Awaited(), 0x01);
}

}  // namespace

int main() {
    std::printf("dlp_child_wait_test: the host's wait for the children (H5)\n");
    T1_Off();
    T2_NobodyNoted();
    T3_OutAndBack();
    T4_NeverBack();
    T5_AzaharStays();
    T6_AzaharAndSlowConsole();
    T7_OneBackOneNot();
    T8_NotListed();
    T9_NeverSeenOut();
    T10_Edges();
    T11_ClockBack();
    T12_Once();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
