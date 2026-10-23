// What the plugin says on the screen about the game's memory and about frames lost (azl/screen_text.hpp), run by Tests/run.sh:
// the texts, the limits, the moment of the first reading and of every one after it, when a warning is told and when it is told
// again, and the choice between "the game reads too slowly" and "the game is not reading the network". Pure functions: nothing
// here needs a console. The numbers of the memory readings are the ones of the hardware logs (Zelda Tri Force Heroes and Final
// Fantasy Explorers on a New 3DS: 70,656 KB of 126,976 KB used, 56,320 KB free, from 10 s to the end of a 2,000 s session).
#include <cstdio>
#include <cstring>
#include <string>

#include "azl/screen_text.hpp"
#include "azl/soc_plan.hpp"
#include "azl/text_wrap.hpp"

using namespace azl;

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
#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const std::string got_ = (a);                                                              \
        if (got_ != (b)) {                                                                         \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: '%s' is not '%s'\n", __FILE__, __LINE__, got_.c_str(), (b)); \
        }                                                                                          \
    } while (0)

static gamemem::Reading Kb(long region_kb, long used_kb) {
    return gamemem::MakeReading(region_kb * 1024ll, used_kb * 1024ll);
}

static void TestReading() {
    std::printf("memory: the reading, the mode, the texts\n");
    // the hardware: both games, New 3DS
    const gamemem::Reading tfh = Kb(126976, 70656);
    CHECK(tfh.valid && tfh.new_layout);
    CHECK(tfh.region_kb == 126976 && tfh.used_kb == 70656 && tfh.free_kb == 56320);
    CHECK(!gamemem::IsLow(tfh));
    CHECK_STR(gamemem::InfoText(tfh), "New 3DS memory mode: 55 MB free"); // 56,320 KB = 55 MB (1 MB = 1024 KB, rounded down)
    CHECK_STR(gamemem::StatusText(tfh), "Game memory: 55 MB free (New 3DS mode)");
    CHECK(gamemem::StatusText(tfh).size() <= 44);
    // an Old 3DS: 64 MiB region, the plugin lives in the system region
    const gamemem::Reading old3ds = Kb(65536, 37888);
    CHECK(old3ds.valid && !old3ds.new_layout && old3ds.free_kb == 27648);
    CHECK_STR(gamemem::InfoText(old3ds), "Old 3DS memory mode: 27 MB free");
    CHECK_STR(gamemem::StatusText(old3ds), "Game memory: 27 MB free (Old 3DS mode)");
    // an Old 3DS in an extended mode (80 MiB) is still the Old layout; the New layout starts at 100 MiB
    CHECK(!Kb(81920, 0).new_layout);
    CHECK(!gamemem::MakeReading(gamemem::kNewLayoutRegionBytes - 1, 0).new_layout);
    CHECK(gamemem::MakeReading(gamemem::kNewLayoutRegionBytes, 0).new_layout);
    CHECK(gamemem::kNewLayoutRegionBytes == static_cast<long long>(ctr::socplan::kNewConsoleRegion)); // (one threshold, two files)
    // unusable numbers are no reading
    CHECK(!gamemem::MakeReading(0, 0).valid);
    CHECK(!gamemem::MakeReading(-1, 0).valid);
    CHECK(!gamemem::MakeReading(1 << 20, -5).valid);
    CHECK(!gamemem::MakeReading(1 << 20, (1 << 20) + 1).valid); // (more used than there is)
    CHECK(gamemem::MakeReading(1 << 20, 1 << 20).valid && gamemem::MakeReading(1 << 20, 1 << 20).free_kb == 0);
    CHECK_STR(gamemem::StatusText(gamemem::Reading()), "Game memory: read 20 s after the start");

    // the figure: MB rounded down, KB below one MB, never "5 MB" for 5119 KB
    CHECK_STR(gamemem::FreeText(56320), "55 MB");
    CHECK_STR(gamemem::FreeText(1024), "1 MB");
    CHECK_STR(gamemem::FreeText(2047), "1 MB");
    CHECK_STR(gamemem::FreeText(1023), "1023 KB");
    CHECK_STR(gamemem::FreeText(640), "640 KB");
    CHECK_STR(gamemem::FreeText(0), "0 KB");
    CHECK_STR(gamemem::FreeText(-12), "0 KB");
    CHECK_STR(gamemem::FreeText(5119), "4 MB");
    CHECK_STR(gamemem::FreeText(5120), "5 MB");
    // every text fits the screen as it is or wraps without losing a word
    for (const gamemem::Reading& r : {tfh, old3ds, Kb(126976, 126900), Kb(65536, 65536)}) {
        for (const std::string& text : {gamemem::InfoText(r), gamemem::LowText(r)}) {
            std::string rebuilt;
            for (const std::string& line : WrapLines(text, 58)) {
                CHECK(line.size() <= 58);
                rebuilt += (rebuilt.empty() ? "" : " ") + line;
            }
            CHECK(rebuilt == text);
            CHECK(text.size() <= 58); // one line of the overlay
        }
    }
}

static void TestLimits() {
    std::printf("memory: the limit (one, 5 MB, on both layouts)\n");
    CHECK(gamemem::kLowKb == 5 * 1024 && gamemem::kLowKb == 5120);
    // New 3DS: the limit is the plugin's own block
    CHECK(!gamemem::IsLow(Kb(126976, 126976 - 5120)));
    CHECK(gamemem::IsLow(Kb(126976, 126976 - 5119)));
    CHECK(gamemem::IsLow(Kb(126976, 126976)));
    // Old 3DS: the same limit, to the kilobyte (no other number is known for it)
    CHECK(!gamemem::IsLow(Kb(65536, 65536 - 5120)));
    CHECK(gamemem::IsLow(Kb(65536, 65536 - 5119)));
    CHECK(gamemem::IsLow(Kb(65536, 65536)));
    // 6 MB free is not low on either layout (an Old 3DS is not held to a higher limit than a New one)
    CHECK(!gamemem::IsLow(Kb(126976, 126976 - 6144)));
    CHECK(!gamemem::IsLow(Kb(65536, 65536 - 6144)));
    CHECK(!gamemem::IsLow(Kb(65536, 65536 - 8191)));
    CHECK(!gamemem::IsLow(Kb(81920, 81920 - 8191))); // (an Old 3DS in an extended mode, 80 MiB)
    // no reading is not low
    CHECK(!gamemem::IsLow(gamemem::Reading()));
    CHECK_STR(gamemem::LowText(Kb(126976, 126976 - 3500)), "Game memory almost full: 3 MB free");
    CHECK_STR(gamemem::LowText(Kb(65536, 65536 - 300)), "Game memory almost full: 300 KB free");
    // the games on record are nowhere near
    CHECK(Kb(126976, 70656).free_kb > 10 * gamemem::kLowKb);
}

static void TestWatch() {
    std::printf("memory: when to read, what to say, when to say it again\n");
    const gamemem::Reading good = Kb(126976, 70656);
    const gamemem::Reading low = Kb(126976, 126976 - 3000);
    const gamemem::Reading just_above = Kb(126976, 126976 - 5120 - 100);   // above the limit by less than the re-arm margin
    const gamemem::Reading well_above = Kb(126976, 126976 - 5120 - 1024); // above by the margin
    using Say = gamemem::Watch::Say;

    // the first reading is at 20 s, not before
    gamemem::Watch w;
    CHECK(!w.Due(0) && !w.Due(10000) && !w.Due(19999));
    CHECK(w.Due(20000) && w.Due(25000));
    // ... says the memory mode once (the Info toast of the start) ...
    CHECK(w.Update(good, 20500) == Say::Info);
    // ... and the next reading is 30 s after that one, and says nothing while all is well, however often it is read
    CHECK(!w.Due(20500) && !w.Due(50499) && w.Due(50500));
    CHECK(w.Update(good, 50500) == Say::Nothing);
    CHECK(w.Update(good, 80500) == Say::Nothing);
    CHECK(w.Update(good, 110500) == Say::Nothing);
    // it falls below the limit: the warning, once
    CHECK(w.Update(low, 140500) == Say::Low);
    CHECK(w.Update(low, 170500) == Say::Nothing);
    CHECK(w.Update(low, 200500) == Say::Nothing);
    // ... not told again for a figure that wobbles around the limit
    CHECK(w.Update(just_above, 230500) == Say::Nothing);
    CHECK(w.Update(low, 260500) == Say::Nothing);
    // ... but told again when it came back well above and falls again
    CHECK(w.Update(well_above, 290500) == Say::Nothing);
    CHECK(w.Update(low, 320500) == Say::Low);
    CHECK(w.Update(good, 350500) == Say::Nothing);
    CHECK(w.Update(low, 380500) == Say::Low);

    // a game that fills its region from the start (the first reading is already low: Final Fantasy Explorers and Tri Force Heroes
    // on the Old 3DS memory layout of the emulator leave 4 and 0 KB, by design): ONE toast, the memory mode and the figure - no
    // warning, now or while it stays so - and a warning only for a fall after it has been above the limit
    gamemem::Watch full;
    CHECK(full.Update(low, 21000) == Say::Info);
    CHECK(full.Update(low, 51000) == Say::Nothing);
    CHECK(full.Update(low, 81000) == Say::Nothing);
    CHECK(full.Update(well_above, 111000) == Say::Nothing);
    CHECK(full.Update(low, 141000) == Say::Low);
    CHECK(full.Update(low, 171000) == Say::Nothing);
    // ... the Old layout of the emulator, to the byte
    gamemem::Watch emulated_old;
    CHECK(emulated_old.Update(Kb(65536, 65532), 20679) == Say::Info);   // 4 KB free
    CHECK_STR(gamemem::InfoText(Kb(65536, 65532)), "Old 3DS memory mode: all memory in use"); // (4 KB free: the whole region)
    CHECK_STR(gamemem::StatusText(Kb(65536, 65532)), "Game memory: all in use (Old 3DS mode)");
    // Mario Party: The Top 100 on a New 3DS (hardware, 2026-10-14): 126,976 of 126,976 KB, 0 KB free all session
    CHECK_STR(gamemem::InfoText(Kb(126976, 126976)), "New 3DS memory mode: all memory in use");
    CHECK_STR(gamemem::StatusText(Kb(126976, 126976)), "Game memory: all in use (New 3DS mode)");
    CHECK(gamemem::StatusText(Kb(126976, 126976)).size() <= 44);
    // ... but a few hundred KB is a figure, not "all": the limit is 256 KB
    CHECK_STR(gamemem::InfoText(Kb(126976, 126976 - 255)), "New 3DS memory mode: all memory in use");
    CHECK_STR(gamemem::InfoText(Kb(126976, 126976 - 256)), "New 3DS memory mode: 256 KB free");
    CHECK_STR(gamemem::InfoText(Kb(126976, 126976 - 640)), "New 3DS memory mode: 640 KB free");
    CHECK(emulated_old.Update(Kb(65536, 65532), 50679) == Say::Nothing);
    CHECK(emulated_old.Update(Kb(65536, 65536), 80679) == Say::Nothing); // 0 KB free

    // a reading that failed (the system call said no): nothing to say, tried again 30 s later, and the Info toast still
    // comes with the first reading that works
    gamemem::Watch retry;
    CHECK(retry.Update(gamemem::Reading(), 20000) == Say::Nothing);
    CHECK(!retry.Due(49999) && retry.Due(50000));
    CHECK(retry.Update(good, 50000) == Say::Info);
    CHECK(retry.Update(good, 80000) == Say::Nothing);

    // an Old 3DS: the same machine and the same limit as the New one
    gamemem::Watch oldw;
    const gamemem::Reading old_ok = Kb(65536, 65536 - 9000);
    const gamemem::Reading old_mid = Kb(65536, 65536 - 7000);  // (fewer than 8 MB free is no news here: the limit is 5 MB)
    const gamemem::Reading old_low = Kb(65536, 65536 - 4536);  // 4,536 KB free
    CHECK(oldw.Update(old_ok, 20000) == Say::Info);
    CHECK(oldw.Update(old_mid, 50000) == Say::Nothing);
    CHECK(oldw.Update(old_low, 80000) == Say::Low);  // (it fell: 9,000 KB free at 20 s, 7,000 KB at 50 s, 4,536 KB at 80 s)
    CHECK(oldw.Update(old_low, 110000) == Say::Nothing);

    // the false alarm of 0.1.15: Tri Force Heroes (extended memory) is told it may commit 119,476 KB with 114,228 KB free. What
    // matters now is what it really uses: the reading at 20 s is the Info toast, whatever it may still commit
    gamemem::Watch tfh;
    CHECK(tfh.Update(good, 20000) == Say::Info);
    CHECK(tfh.Update(Kb(126976, 70656), 50000) == Say::Nothing);
    for (u32 t = 80000; t < 2000000; t += 30000) {
        CHECK(tfh.Update(good, t) == Say::Nothing); // (a whole session at 56,320 KB free: not one warning)
    }
}

static void TestFramesLost() {
    std::printf("frames lost: \"reads too slowly\" or \"not reading the network\", by the game's UDS calls\n");
    using frameslost::NotReading;
    // no UDS call in the window: not reading, whatever the idle time says
    CHECK(NotReading(0, 0));
    CHECK(NotReading(0, 5000));
    // calls in the window: reading, unless none came in the last second
    CHECK(!NotReading(42, 0));
    CHECK(!NotReading(300, 999));
    CHECK(NotReading(300, 1000));          // the pause began in the middle of the window: its calls are still in the count
    CHECK(NotReading(51, 5000));
    CHECK(!NotReading(1, 0));
    CHECK(NotReading(2000000000ul, 60000)); // (a pause of a minute after a busy window)
    CHECK(frameslost::kNotReadingMs == 1000 && frameslost::kWarnAbove == 50);

    CHECK_STR(frameslost::Text(51, false), "AzaharLink: 51 frames lost, the game reads too slowly");
    CHECK_STR(frameslost::Text(105, true), "AzaharLink: 105 frames lost, the game is not reading the network (paused?)");
    CHECK_STR(frameslost::Text(501, true), "AzaharLink: 501 frames lost, the game is not reading the network (paused?)");
    for (const bool not_reading : {false, true}) {
        for (const unsigned long lost : {51ul, 105ul, 501ul, 99999ul}) {
            const std::string text = frameslost::Text(lost, not_reading);
            std::string rebuilt;
            for (const std::string& line : WrapLines(text, 58)) { // (the overlay wraps at 58 characters: all of it, in order)
                CHECK(line.size() <= 58);
                rebuilt += (rebuilt.empty() ? "" : " ") + line;
            }
            CHECK(rebuilt == text);
            CHECK(text.find("debug=1") == std::string::npos); // (nothing to switch on: the log line says what the text was chosen from)
        }
    }

    // The looks at the counter: "idle" is the time since a look found it changed
    frameslost::Idle idle;
    idle.Look(0, 1000);                    // the first look is activity
    CHECK(idle.IdleMs(1000) == 0);
    idle.Look(0, 2000);                    // nothing came
    idle.Look(0, 3000);
    CHECK(idle.IdleMs(3000) == 2000);
    idle.Look(120, 4000);                  // the game read
    CHECK(idle.IdleMs(4000) == 0);
    idle.Look(310, 5000);
    idle.Look(310, 6000);                  // ... and paused
    idle.Look(310, 7000);
    CHECK(idle.IdleMs(7000) == 2000 && NotReading(310, idle.IdleMs(7000)));
    idle.CounterCleared();                 // the window ended: the counter starts again from 0
    idle.Look(0, 8000);                    // (0 is the same as the cleared counter: still no activity)
    CHECK(idle.IdleMs(8000) == 3000);
    idle.Look(15, 9000);                   // ... and 15 new calls are activity
    CHECK(idle.IdleMs(9000) == 0);
    idle.Look(15, 9500);
    CHECK(idle.IdleMs(9500) == 500 && !NotReading(15, idle.IdleMs(9500)));
    CHECK(idle.IdleMs(100) == 0);          // (a clock that reads earlier: no wrap)
    // a counter that is cleared and has already counted as many calls as before is a change
    frameslost::Idle same;
    same.Look(50, 1000);
    same.Look(50, 2000);
    same.CounterCleared();
    same.Look(50, 3000);
    CHECK(same.IdleMs(3000) == 0);

    // the cases of the pause that started the work (PROVA 2: 105 frames lost while the game made no UDS call)
    CHECK(NotReading(0, 42000) && frameslost::Text(105, NotReading(0, 42000)).find("not reading") != std::string::npos);
    CHECK(!NotReading(300, 0) && frameslost::Text(51, NotReading(300, 0)).find("too slowly") != std::string::npos);
}

static void TestHint() {
    std::printf("the \"set debug=1 for details\" hint is for a log that is off\n");
    CHECK_STR(DebugHint(false), " (set debug=1 for details)");
    CHECK_STR(DebugHint(true), "");
    CHECK_STR(ctr::socplan::StartupIssue(0x10000, true), "Socket buffer reduced to 64 KB: not enough free memory for 128 KB");
    CHECK_STR(ctr::socplan::StartupIssue(0x10000, false),
              "Socket buffer reduced to 64 KB: not enough free memory for 128 KB (set debug=1 for details)");
    CHECK_STR(ctr::socplan::StartupIssue(ctr::socplan::kSocBlockSize, true), "");
    CHECK_STR(ctr::socplan::StartupIssue(ctr::socplan::kSocBlockSize, false), "");
}

int main() {
    TestReading();
    TestLimits();
    TestWatch();
    TestFramesLost();
    TestHint();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
