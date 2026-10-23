// The debug capture (azl/log.cpp, azl/stats.hpp), run by Tests/run.sh: the two lanes of the ring and what the lane of control
// records is for, the LOST markers of a hole, a producer that never waits for the ring's lock, the diet of the IPC lines, the
// raw records of the HLE proper's commands against the printf formats they replaced, and the header-only frame records.
// Time is the real clock (the diet works in seconds): the test takes about 6 s.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
// libstdc++'s <regex> trips -Wmaybe-uninitialized inside its own headers at -O1 (see test_main.cpp).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include <regex>
#pragma GCC diagnostic pop
#include <string>
#include <thread>
#include <vector>

#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "support.hpp"

namespace {

constexpr u32 kRing = 512 * 1024;
constexpr u32 kReserve = 32 * 1024;

std::string Fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

struct Popped {
    DebugRecord rec;
    std::vector<u8> payload;
    std::string line; ///< as the writer prints it, with the time column of `start`
};

/// Everything the ring holds, oldest first.
std::vector<Popped> Drain(u32 start_ms) {
    std::vector<Popped> all;
    Popped p;
    u8 buf[1700];
    while (DebugPop(&p.rec, buf, sizeof(buf))) {
        p.payload.assign(buf, buf + p.rec.stored);
        p.line.clear();
        DebugFormat(p.rec, buf, start_ms, &p.line);
        all.push_back(p);
    }
    return all;
}

u32 NowMs() {
    return static_cast<u32>(platform::NowMs());
}

/// A fresh capture: a ring, the level on, nothing in it.
struct Fresh {
    Fresh() {
        CHECK(DebugInit());
        DebugReset();
        g_debug_level = 1;
    }
    ~Fresh() {
        g_debug_level = 0;
        DebugReset();
    }
};

int Count(const std::vector<Popped>& all, u8 kind) {
    int n = 0;
    for (const Popped& p : all) {
        n += p.rec.kind == kind ? 1 : 0;
    }
    return n;
}

// ---- 1. the lanes ----------------------------------------------------------------------------------------------------------
void TestLanes() {
    std::printf("the ring has a lane for control records: a flood of frames fills the rest and nothing more\n");
    Fresh fresh;
    const u32 start = NowMs();
    const std::vector<u8> payload(1000, 0x5A);
    constexpr u32 kFrameRecord = 16 + 1000;
    const int attempts = 1000;
    for (int i = 0; i < attempts; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1000);
    }
    CaptureTotals t = DebugCaptureTotals();
    const u32 stored = t.records[kLaneHeavy];
    CHECK(stored * kFrameRecord <= kRing - kReserve);
    CHECK((stored + 1) * kFrameRecord > kRing - kReserve); // it went in until the next one would not fit, and no further
    CHECK_EQ(t.lost_full[kLaneHeavy], static_cast<u32>(attempts) - stored);
    CHECK_EQ(t.lost_busy[kLaneHeavy], 0u);
    CHECK(DebugBytesQueued() <= kRing - kReserve);

    // ... the control records still go in: the 32 KB are theirs
    for (int i = 0; i < 300; ++i) {
        DebugEvent("control record number %d of the test", i);
    }
    t = DebugCaptureTotals();
    CHECK_EQ(t.records[kLaneControl], 300u);
    CHECK_EQ(t.lost_full[kLaneControl], 0u);
    // a role line, a command that changes state, a Deauthentication line
    {
        DebugLine line;
        line.Str("uds role idle -> host node 1 nodes 1/4 reason 1");
    }
    DebugServiceCommand(true, 0x1D, nullptr, 0, nullptr, 0);   // BeginHostingNetwork: control
    DebugServiceCommand(true, 0x14, nullptr, 0, nullptr, 0);   // PullPacket: heavy, and the heavy lane is full
    DebugServiceCommand(true, 0x17, nullptr, 0, nullptr, 0);   // SendTo: heavy
    DebugServiceCommand(false, 0x14, nullptr, 0, nullptr, 0);  // an ndm command: control, whatever its id
    t = DebugCaptureTotals();
    CHECK_EQ(t.records[kLaneControl], 300u + 1 + 1 + 1);
    CHECK_EQ(t.lost_full[kLaneHeavy], static_cast<u32>(attempts) - stored + 2);
    CHECK(DebugBytesQueued() <= kRing);

    // the writer empties the ring: first the frames, then the control lines, and the hole of the frames is announced by
    // the marker ahead of the next frame
    const std::vector<Popped> all = Drain(start);
    CHECK_EQ(static_cast<u32>(Count(all, kDbgFrame)), stored);
    CHECK_EQ(Count(all, kDbgText), 301);
    CHECK_EQ(Count(all, kDbgCommand), 2);
    CHECK_EQ(Count(all, kDbgLost), 0);
    CHECK_EQ(DebugBytesQueued(), 0u);
    DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1000);
    const std::vector<Popped> after = Drain(start);
    CHECK_EQ(after.size(), std::size_t(2));
    if (after.size() == 2) {
        CHECK_EQ(static_cast<int>(after[0].rec.kind), static_cast<int>(kDbgLost));
        CHECK_EQ(static_cast<int>(after[0].rec.dir), static_cast<int>(kLaneHeavy));
        CHECK_EQ(static_cast<int>(after[1].rec.kind), static_cast<int>(kDbgFrame));
        const std::regex lost(R"(^\s*\d+\.\d{3} LOST (\d+) heavy records from (\d+)\.(\d{3}) to (\d+)\.(\d{3}) \(ring full (\d+), lock busy 0\)\n$)");
        std::smatch m;
        CHECK(std::regex_match(after[0].line, m, lost));
        if (m.size() == 7) {
            CHECK_EQ(std::stoul(m[1]), static_cast<unsigned long>(attempts - stored + 2));
            CHECK_EQ(std::stoul(m[6]), static_cast<unsigned long>(attempts - stored + 2));
            const double from = std::stod(m[2].str() + "." + m[3].str()), to = std::stod(m[4].str() + "." + m[5].str());
            CHECK(from <= to);
            CHECK(to < 5.0); // (the test started a moment ago)
        }
    }
    // the marker is written once
    DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1000);
    const std::vector<Popped> again = Drain(start);
    CHECK_EQ(again.size(), std::size_t(1));
    CHECK_EQ(DebugTakeDropped(), static_cast<u32>(attempts - stored + 2));
    CHECK_EQ(DebugTakeDropped(), 0u);
}

void TestControlLaneFull() {
    std::printf("the control lane can fill too: then it loses records and says so, the ring never overflows\n");
    Fresh fresh;
    const u32 start = NowMs();
    const std::string text(180, 'c');
    constexpr u32 kLine = 16 + 180;
    int attempts = 0;
    for (; DebugCaptureTotals().lost_full[kLaneControl] == 0; ++attempts) {
        DebugEvent("%s", text.c_str());
        CHECK(attempts < 4000);
        CHECK(DebugBytesQueued() <= kRing);
    }
    const CaptureTotals t = DebugCaptureTotals();
    CHECK_EQ(t.records[kLaneControl] + 1, static_cast<u32>(attempts)); // (the last attempt is the first one that did not fit)
    CHECK((t.records[kLaneControl] + 1) * kLine > kRing);
    CHECK(t.records[kLaneControl] * kLine <= kRing);
    // heavy records had no room long before
    DebugFrame(kDirRx, 1, 2, 3, reinterpret_cast<const u8*>("abc"), 3);
    CHECK_EQ(DebugCaptureTotals().lost_full[kLaneHeavy], 1u);
    for (int i = 0; i < 10; ++i) {
        DebugEvent("%s", text.c_str());
    }
    CHECK_EQ(DebugCaptureTotals().lost_full[kLaneControl], 11u);
    Drain(start);
    DebugEvent("after the hole");
    const std::vector<Popped> after = Drain(start);
    CHECK_EQ(after.size(), std::size_t(2));
    if (after.size() == 2) {
        CHECK_EQ(static_cast<int>(after[0].rec.kind), static_cast<int>(kDbgLost));
        CHECK_EQ(static_cast<int>(after[0].rec.dir), static_cast<int>(kLaneControl));
        CHECK(after[0].line.find(" LOST 11 control records from ") != std::string::npos);
        CHECK(after[0].line.find("(ring full 11, lock busy 0)") != std::string::npos);
        CHECK(after[1].line.find("after the hole") != std::string::npos);
    }
    // the heavy lane had its own hole, announced ahead of its next record
    DebugFrame(kDirRx, 1, 2, 3, reinterpret_cast<const u8*>("abc"), 3);
    const std::vector<Popped> heavy = Drain(start);
    CHECK_EQ(heavy.size(), std::size_t(2));
    if (heavy.size() == 2) {
        CHECK(heavy[0].line.find(" LOST 1 heavy records from ") != std::string::npos);
    }
}

// ---- 2. the ring's lock is never waited for ----------------------------------------------------------------------------------
void TestLockBusy() {
    std::printf("a producer that finds the ring's lock taken gives the record up and counts it (never waits)\n");
    Fresh fresh;
    const u32 start = NowMs();
    std::atomic<int> state{0}; // 0 not yet, 1 holding the lock, 2 release
    // A DebugLine holds the ring's lock for as long as it lives: another thread's, here the log writer's, in a long copy.
    std::thread holder([&] {
        DebugLine line;
        line.Str("a line under construction");
        state = 1;
        while (state.load() != 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (state.load() != 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::vector<u8> payload(100, 1);
    const u32 words[3] = {1, 2, 3};
    // (a producer that waited for the lock would wait for ever, the holder being let go only after the producers are done: the
    // watchdog lets it go after 3 s, so that such a producer fails the check below instead of hanging the test)
    std::atomic<bool> producers_done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !producers_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        state = 2;
    });
    const auto begin = std::chrono::steady_clock::now();
    constexpr int kRounds = 40;
    for (int i = 0; i < kRounds; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 100);       // heavy
        DebugIpc(0x1234, 0x5678, static_cast<u32>(i), 0);       // heavy
        DebugSvcReal("ac:u", 0x29, 0, 0);                       // heavy
        DebugBeacon(kDirRx, nullptr, 11, payload.data(), 100);  // heavy
        DebugPullPacket(2, 140, 3, words, 3, words, 3);         // heavy
        DebugEvent("a control line %d", i);                     // control
        DebugServiceCommand(true, 3, words, 3, words, 3);       // control
        {
            DebugLine line;                                     // control: an inactive line, appends do nothing
            line.Str("never written").Dec(5);
            CHECK_EQ(line.size(), 0u);
        }
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    CHECK(spent < 250); // 320 calls that each tried for a few microseconds: nowhere near a wait for the holder
    producers_done = true;
    watchdog.join();
    state = 2;
    holder.join();
    CaptureTotals t = DebugCaptureTotals();
    CHECK_EQ(t.lost_busy[kLaneHeavy], 5u * kRounds);
    CHECK_EQ(t.lost_busy[kLaneControl], 3u * kRounds);
    CHECK_EQ(t.lost_full[kLaneHeavy] + t.lost_full[kLaneControl], 0u);
    CHECK_EQ(DebugTakeDropped(), 8u * kRounds);

    // The next record of each lane that does go in is preceded by the marker: the count, the interval and the reason.
    DebugFrame(kDirRx, 1, 2, 3, payload.data(), 100);
    DebugEvent("first control line after the hole");
    const std::vector<Popped> all = Drain(start);
    // (the holder's own line goes in when it ends, behind the marker of the control lane's misses)
    std::vector<std::string> lost_lines;
    for (const Popped& p : all) {
        if (p.rec.kind == kDbgLost) {
            lost_lines.push_back(p.line);
        }
    }
    CHECK_EQ(lost_lines.size(), std::size_t(2));
    bool saw_heavy = false, saw_control = false;
    for (const std::string& l : lost_lines) {
        saw_heavy |= l.find(Fmt(" LOST %d heavy records from ", 5 * kRounds)) != std::string::npos &&
                     l.find(Fmt("(ring full 0, lock busy %d)", 5 * kRounds)) != std::string::npos;
        saw_control |= l.find(Fmt(" LOST %d control records from ", 3 * kRounds)) != std::string::npos &&
                       l.find(Fmt("(ring full 0, lock busy %d)", 3 * kRounds)) != std::string::npos;
    }
    CHECK(saw_heavy);
    CHECK(saw_control);
    // and each marker comes right before a record of its lane
    for (std::size_t i = 0; i + 1 < all.size(); ++i) {
        if (all[i].rec.kind == kDbgLost) {
            CHECK_EQ(static_cast<int>(all[i + 1].rec.pad), static_cast<int>(all[i].rec.dir));
        }
    }
    // with nobody holding it nothing is given up
    for (int i = 0; i < 100; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 100);
    }
    CHECK_EQ(DebugCaptureTotals().lost_busy[kLaneHeavy], 5u * kRounds);
}

// ---- 3. the diet --------------------------------------------------------------------------------------------------------------
void TestDiet() {
    std::printf("the diet: an IPC call that succeeds is written whole three times a second, then counted\n");
    Fresh fresh;
    const u32 start = NowMs();
    const u32 gsp = 0x250036, flush = 0x000C0000;
    const u32 t0 = NowMs();
    for (u32 i = 0; i < 100; ++i) {
        DebugIpc(gsp, flush, 0x1000 + i, 0);
    }
    const u32 t1 = NowMs();
    CHECK(t1 - t0 < 900); // (the second of the test: if the machine stalled, the rest of this test is not about the diet)
    DebugFlushAggregates(false);
    std::vector<Popped> all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpc), 3);            // the first three
    CHECK_EQ(Count(all, kDbgIpcAgg), 0);         // the count is not written before its second is over
    if (all.size() >= 3) {
        CHECK(all[0].line.find(" ipc h=0x250036 cmd=0x000c0000 arg=0x1000 -> 0x00000000") != std::string::npos);
        CHECK(all[2].line.find(" arg=0x1002 -> 0x00000000") != std::string::npos);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    DebugFlushAggregates(false); // (the writer's turn: nothing else calls, the count has to come out)
    all = Drain(start);
    CHECK_EQ(all.size(), std::size_t(1));
    if (all.size() == 1) {
        CHECK_EQ(static_cast<int>(all[0].rec.kind), static_cast<int>(kDbgIpcAgg));
        const std::regex line(R"(^\s*\d+\.\d{3} ipc h=0x250036 cmd=0x000c0000 arg=0x1063 -> 0x00000000 x97 from (\d+)\.(\d{3}) to (\d+)\.(\d{3})\n$)");
        std::smatch m;
        CHECK(std::regex_match(all[0].line, m, line));
        if (m.size() == 5) {
            const double from = std::stod(m[1].str() + "." + m[2].str()) * 1000, to = std::stod(m[3].str() + "." + m[4].str()) * 1000;
            CHECK(from >= static_cast<double>(t0) - start - 1 && to <= static_cast<double>(t1) - start + 1 && from <= to);
        }
        // the old pattern of the analysis scripts reads it as an ipc line
        const std::regex scripts(R"(^\s*(\d+\.\d+) ipc h=(0x[0-9a-f]+) cmd=(0x[0-9a-f]+) arg=(0x[0-9a-f]+) -> (0x[0-9a-f]+))");
        CHECK(std::regex_search(all[0].line, scripts));
    }
    CHECK_EQ(DebugCaptureTotals().agg_lines, 1u);
    CHECK_EQ(DebugCaptureTotals().agg_calls, 97u);
    DebugFlushAggregates(true);
    CHECK(Drain(start).empty()); // (written once)

    // every call with a result other than 0 is whole, and a failing call does not use up the three of its kind
    for (u32 i = 0; i < 50; ++i) {
        DebugIpc(gsp, flush, i, 0xD8E007F7);
    }
    all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpc), 50);
    CHECK_EQ(Count(all, kDbgIpcAgg), 0);
    // another command of the same session, another session with the same command: other kinds, three whole calls each
    for (u32 i = 0; i < 5; ++i) {
        DebugIpc(gsp, 0x00080082, i, 0);
        DebugIpc(0x250037, flush, i, 0);
    }
    DebugFlushAggregates(true);
    all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpc), 6);
    CHECK_EQ(Count(all, kDbgIpcAgg), 2);
    for (const Popped& p : all) {
        if (p.rec.kind == kDbgIpcAgg) {
            CHECK(p.line.find(" x2 from ") != std::string::npos); // 5 calls, 3 whole: 2 counted
            CHECK(p.line.find(" arg=0x4 -> ") != std::string::npos); // the last of them
        }
    }

    // a kind that is called three times a second or less is never counted (nothing is lost to the diet below that rate)
    DebugReset();
    for (int second = 0; second < 2; ++second) {
        for (u32 i = 0; i < 3; ++i) {
            DebugIpc(gsp, flush, i, 0);
        }
        if (second == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        }
    }
    DebugFlushAggregates(true);
    all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpc), 6);
    CHECK_EQ(Count(all, kDbgIpcAgg), 0);

    // a call in the next second only hands the count of the last one over (a game thread pushes no record for it): the writer
    // writes it, with the time of its own turn, and "from" and "to" say when the calls really were
    DebugReset();
    for (u32 i = 0; i < 10; ++i) {
        DebugIpc(gsp, flush, i, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    DebugIpc(gsp, flush, 99, 0);
    all = Drain(start);
    CHECK_EQ(all.size(), std::size_t(4)); // 3 whole and the new call: no count yet
    if (all.size() == 4) {
        CHECK_EQ(static_cast<int>(all[3].rec.kind), static_cast<int>(kDbgIpc));
        CHECK(all[3].line.find(" arg=0x63 -> ") != std::string::npos);
    }
    DebugFlushAggregates(false);
    all = Drain(start);
    CHECK_EQ(all.size(), std::size_t(1));
    if (all.size() == 1) {
        CHECK_EQ(static_cast<int>(all[0].rec.kind), static_cast<int>(kDbgIpcAgg));
        CHECK(all[0].line.find(" arg=0x9 -> 0x00000000 x7 from ") != std::string::npos);
    }
    DebugFlushAggregates(true);
    CHECK(Drain(start).empty()); // (the new window has 1 call, whole: nothing to count)

    // the count of an earlier second and the open one are one line when the writer comes: the calls of both
    DebugReset();
    for (u32 i = 0; i < 10; ++i) {
        DebugIpc(gsp, flush, i, 0);                 // 3 whole, 7 counted (the last arg 9)
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    for (u32 i = 0; i < 8; ++i) {
        DebugIpc(gsp, flush, 100 + i, 0);           // a second second: 3 whole, 5 counted (the last arg 107), the first 7 handed over
    }
    DebugFlushAggregates(true);
    all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpc), 6);
    CHECK_EQ(Count(all, kDbgIpcAgg), 1);
    if (!all.empty()) {
        CHECK(all.back().line.find(" arg=0x6b -> 0x00000000 x12 from ") != std::string::npos); // 0x6b = 107
    }
    CHECK_EQ(DebugCaptureTotals().agg_calls, 12u);

    // the count line is a control record: it goes in when the heavy lane has no room left, and its record says so
    DebugReset();
    for (u32 i = 0; i < 10; ++i) {
        DebugIpc(gsp, flush, i, 0); // 3 whole, 7 counted
    }
    const std::vector<u8> filler(1000, 1);
    for (int i = 0; i < 600; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, filler.data(), 1000); // the heavy lane fills up
    }
    CHECK(DebugCaptureTotals().lost_full[kLaneHeavy] > 0);
    DebugFlushAggregates(true);
    all = Drain(start);
    CHECK_EQ(Count(all, kDbgIpcAgg), 1);
    for (const Popped& p : all) {
        if (p.rec.kind == kDbgIpcAgg) {
            CHECK_EQ(static_cast<int>(p.rec.pad), static_cast<int>(kLaneControl));
            CHECK(p.line.find(" x7 from ") != std::string::npos);
        }
    }
    CHECK_EQ(DebugCaptureTotals().lost_full[kLaneControl], 0u);

    // a count line that finds the control lane full is lost, and so are the calls it stood for: the LOST marker says 97, not 1
    // (the diet must not turn a stalled writer into calls that vanish without a mark)
    DebugReset();
    for (u32 i = 0; i < 100; ++i) {
        DebugIpc(gsp, flush, i, 0); // 3 whole, 97 counted
    }
    const std::string text(180, 'c');
    for (int i = 0; i < 4000 && DebugCaptureTotals().lost_full[kLaneControl] == 0; ++i) {
        DebugEvent("%s", text.c_str()); // the control lane fills up ...
    }
    const u32 lost_at_first = DebugCaptureTotals().lost_full[kLaneControl];
    CHECK(lost_at_first > 0);
    for (int i = 0; i < 20000 && DebugCaptureTotals().lost_full[kLaneControl] == lost_at_first; ++i) {
        DebugEvent("x"); // ... to the last bytes (a short record fits where a long one did not; the count line and its marker do not)
    }
    const CaptureTotals before = DebugCaptureTotals();
    CHECK(before.lost_full[kLaneControl] > lost_at_first);
    DebugFlushAggregates(true);
    const CaptureTotals after = DebugCaptureTotals();
    CHECK_EQ(after.agg_lines, before.agg_lines);                                         // the line did not go in ...
    CHECK_EQ(after.lost_full[kLaneControl], before.lost_full[kLaneControl] + 97u);         // ... and its 97 calls are in the lost count
    CHECK_EQ(after.agg_calls, before.agg_calls);
    DebugFlushAggregates(true);
    CHECK_EQ(DebugCaptureTotals().lost_full[kLaneControl], after.lost_full[kLaneControl]); // (counted once)
    Drain(start);                                                                            // the writer catches up ...
    DebugEvent("after the hole");
    all = Drain(start);                                                                      // ... and the hole is announced
    CHECK_EQ(all.size(), std::size_t(2));
    if (all.size() == 2) {
        CHECK_EQ(static_cast<int>(all[0].rec.kind), static_cast<int>(kDbgLost));
        const std::regex lost_line(R"( LOST (\d+) control records from )");
        std::smatch m;
        CHECK(std::regex_search(all[0].line, m, lost_line));
        if (m.size() == 2) {
            CHECK(std::stoul(m[1]) >= 97); // (the calls of the count line are in the number, not one record for them)
        }
    }
}

void TestDietConserves() {
    std::printf("the diet loses no call: every one is written whole or is in a count (table full, threads, errors)\n");
    Fresh fresh;
    const u32 start = NowMs();
    // 40 kinds in one second: the table (32 slots) cannot hold them all; the ones without a slot are written whole
    u32 calls = 0;
    for (u32 kind = 0; kind < 40; ++kind) {
        for (u32 i = 0; i < 5; ++i) {
            DebugIpc(0x300000 + kind * 7, 0x00010000 * kind + 0x42, i, 0);
            ++calls;
        }
    }
    DebugFlushAggregates(true);
    std::vector<Popped> all = Drain(start);
    u64 counted = 0;
    for (const Popped& p : all) {
        if (p.rec.kind == kDbgIpcAgg) {
            u32 w[7];
            std::memcpy(w, p.payload.data(), sizeof(w));
            counted += w[4];
        }
    }
    CHECK_EQ(static_cast<u64>(Count(all, kDbgIpc)) + counted, static_cast<u64>(calls));
    CHECK(Count(all, kDbgIpcAgg) > 0 && Count(all, kDbgIpcAgg) <= 32);
    CHECK(Count(all, kDbgIpc) > 3 * Count(all, kDbgIpcAgg)); // (the kinds without a slot: all 5 of their calls)

    // random calls of a few kinds, some failing, from three threads, with a flusher: nothing is lost or counted twice
    DebugReset();
    std::atomic<u32> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(100 + t);
            for (int i = 0; i < 20000; ++i) {
                DebugIpc(0x400000 + rng() % 5, 0x00020000 * (rng() % 3), i, rng() % 20 == 0 ? 0xD8E007F7 : 0);
                total.fetch_add(1);
                if (i % 5000 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            }
        });
    }
    std::thread flusher([&] {
        while (!stop.load()) {
            DebugFlushAggregates(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    std::vector<Popped> drained;
    for (auto& t : threads) {
        t.join();
    }
    stop = true;
    flusher.join();
    DebugFlushAggregates(true);
    const CaptureTotals totals = DebugCaptureTotals();
    all = Drain(start);
    counted = 0;
    for (const Popped& p : all) {
        if (p.rec.kind == kDbgIpcAgg) {
            u32 w[7];
            std::memcpy(w, p.payload.data(), sizeof(w));
            counted += w[4];
        }
    }
    // (the ring is large enough for the whole run here: 60000 calls, a few thousand whole. A producer that was preempted with
    // the ring's lock in its hand makes the others give their call up after a few microseconds: those are counted too)
    CHECK_EQ(totals.lost_full[kLaneHeavy], 0u);
    CHECK_EQ(static_cast<u64>(Count(all, kDbgIpc)) + counted + totals.lost_busy[kLaneHeavy], static_cast<u64>(total.load()));
    CHECK_EQ(totals.agg_calls, static_cast<u32>(counted));
}

// ---- 4. the commands of the HLE proper, as raw words ---------------------------------------------------------------------------
std::string Words(const u32* w, u32 n) {
    std::string s;
    for (u32 i = 0; i < n; ++i) {
        s += Fmt(" %08lx", static_cast<unsigned long>(w[i]));
    }
    return s;
}

void TestCommandRecords() {
    std::printf("the commands of the HLE proper are raw words: the writer makes the lines the printf formats made\n");
    Fresh fresh;
    const u32 start = NowMs();
    std::mt19937 rng(2468);
    for (int i = 0; i < 4000; ++i) {
        u32 req[64], rep[64];
        for (auto& w : req) w = rng();
        for (auto& w : rep) w = rng();
        const u32 rw = rng() % 80, pw = rng() % 80; // (more than the IPC buffer holds: cut at 63)
        const u32 cut_rw = rw > 63 ? 63 : rw, cut_pw = pw > 63 ? 63 : pw;
        const bool uds = i % 3 != 0;
        const u32 id = rng() & (i % 2 ? 0xFF : 0xFFFF);
        DebugServiceCommand(uds, id, req, rw > 64 ? 64 : rw, rep, pw > 64 ? 64 : pw);
        const u32 bind = rng(), bytes = rng(), node = rng() >> (rng() % 32);
        DebugPullPacket(bind, bytes, node, req, rw > 64 ? 64 : rw, rep, pw > 64 ? 64 : pw);
        const std::string head = Fmt("%s cmd 0x%02lx", uds ? "uds" : "ndm", static_cast<unsigned long>(id));
        const std::string pull = Fmt("uds PullPacket bind %lu: %lu bytes from node %lu", static_cast<unsigned long>(bind),
                                     static_cast<unsigned long>(bytes), static_cast<unsigned long>(node));
        const std::vector<Popped> two = Drain(start);
        CHECK_EQ(two.size(), std::size_t(2));
        if (two.size() != 2) {
            return;
        }
        const std::string tail = " req" + Words(req, cut_rw) + " -> rep" + Words(rep, cut_pw) + "\n";
        // (the time column is "%6lu.%03lu " of the record's own time)
        CHECK_EQ(two[0].line.substr(two[0].line.find(' ', 6) + 1), head + tail);
        CHECK_EQ(two[1].line.substr(two[1].line.find(' ', 6) + 1), pull + tail);
        CHECK_EQ(static_cast<int>(two[0].rec.kind), static_cast<int>(kDbgCommand));
        CHECK_EQ(static_cast<int>(two[1].rec.kind), static_cast<int>(kDbgCommand));
    }
    // a head of any text, cut at 90 characters
    {
        const u32 w[2] = {0xAABBCCDD, 1};
        DebugCommand("uds cmd 0x1b", w, 2, w, 1);
        DebugCommand(std::string(200, 'h').c_str(), nullptr, 0, nullptr, 0);
        DebugCommand("", nullptr, 0, w, 2);
        const std::vector<Popped> three = Drain(start);
        CHECK_EQ(three.size(), std::size_t(3));
        if (three.size() == 3) {
            CHECK(three[0].line.find(" uds cmd 0x1b req aabbccdd 00000001 -> rep aabbccdd\n") != std::string::npos);
            CHECK(three[1].line.find(" " + std::string(90, 'h') + " req -> rep\n") != std::string::npos);
            CHECK(three[2].line.find(" ") != std::string::npos && three[2].line.find("  req -> rep aabbccdd 00000001\n") != std::string::npos);
        }
    }
    // the size of the record: what the game's thread copies is the words, no text (a SendTo: 8 request words, 1 reply word)
    {
        const u32 w[9] = {};
        DebugServiceCommand(true, 0x17, w, 8, w, 1);
        DebugRecord rec;
        u8 buf[1700];
        CHECK(DebugPop(&rec, buf, sizeof(buf)));
        CHECK_EQ(static_cast<u32>(rec.stored), 16u + 4 * 9);
    }
    // the lane of a command: the ones a game polls are heavy, the ones that change the session are control
    DebugReset();
    const std::vector<u8> payload(1000, 1);
    for (int i = 0; i < 600; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1000); // the heavy lane is full ...
    }
    for (int i = 0; i < 100; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1); // ... down to the last bytes: no command record (32 bytes) fits
    }
    DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1);
    CHECK(DebugCaptureTotals().lost_full[kLaneHeavy] > 0);
    for (u32 id = 0; id < 0x24; ++id) {
        const bool hot = id == 0x0B || id == 0x0C || id == 0x0D || id == 0x0F || id == 0x14 || id == 0x17 || id == 0x22;
        const u32 before = DebugCaptureTotals().records[kLaneControl];
        DebugServiceCommand(true, id, nullptr, 0, nullptr, 0);
        const u32 added = DebugCaptureTotals().records[kLaneControl] - before;
        if (added != (hot ? 0u : 1u)) {
            std::fprintf(stderr, "    uds command 0x%02x: %u control records added\n", id, added);
        }
        CHECK_EQ(added, hot ? 0u : 1u);
        const u32 ndm_before = DebugCaptureTotals().records[kLaneControl];
        DebugServiceCommand(false, id, nullptr, 0, nullptr, 0);
        CHECK_EQ(DebugCaptureTotals().records[kLaneControl] - ndm_before, 1u); // (all of ndm:u is control)
    }
    // a PullPacket line is a heavy record (a game polls it), the line with a head of free text a control record
    {
        const CaptureTotals before = DebugCaptureTotals();
        DebugPullPacket(2, 140, 3, nullptr, 0, nullptr, 0);
        CHECK_EQ(DebugCaptureTotals().lost_full[kLaneHeavy], before.lost_full[kLaneHeavy] + 1);
        CHECK_EQ(DebugCaptureTotals().records[kLaneControl], before.records[kLaneControl]);
        DebugCommand("uds cmd 0x1b", nullptr, 0, nullptr, 0);
        CHECK_EQ(DebugCaptureTotals().records[kLaneControl], before.records[kLaneControl] + 1);
        CHECK_EQ(DebugCaptureTotals().lost_full[kLaneHeavy], before.lost_full[kLaneHeavy] + 1);
    }
}

// ---- 5. the lines of the frames ----------------------------------------------------------------------------------------------
void TestFrameLines() {
    std::printf("frames: whole for this node and ours, header only for the others, the echoes and the second copies\n");
    Fresh fresh;
    const u32 start = NowMs();
    const u8 data[5] = {1, 2, 3, 4, 5};
    DebugFrame(kDirRx, 3, 2, 243, data, 5);
    DebugFrame(kDirTx, 2, 0xFFFF, 243, data, 5);
    DebugFrame(kDirOther, 3, 4, 243, nullptr, 140);
    DebugFrame(kDirEcho, 2, 3, 243, nullptr, 140);
    DebugFrame(kDirDup, 3, 2, 243, nullptr, 140);
    const std::vector<Popped> all = Drain(start);
    CHECK_EQ(all.size(), std::size_t(5));
    if (all.size() == 5) {
        auto body = [](const Popped& p) { return p.line.substr(p.line.find(' ', 6) + 1); };
        CHECK_EQ(body(all[0]), std::string("FRAME rx    3>2 ch243 5B: 0102030405\n"));
        CHECK_EQ(body(all[1]), std::string("FRAME tx    2>all ch243 5B: 0102030405\n"));
        CHECK_EQ(body(all[2]), std::string("FRAME other 3>4 ch243 140B: \n"));
        CHECK_EQ(body(all[3]), std::string("FRAME echo  2>3 ch243 140B: \n"));
        CHECK_EQ(body(all[4]), std::string("FRAME dup   3>2 ch243 140B: \n"));
        // the pattern of the analysis scripts (load.js) reads all five
        const std::regex frame(R"(^\s*([\d.]+) FRAME (\S+)\s+(\S+)>(\S+) ch(\d+) (\d+)B(\s\(cut\))?: ([0-9a-f]*)$)");
        for (const Popped& p : all) {
            std::smatch m;
            const std::string one = p.line.substr(0, p.line.size() - 1);
            CHECK(std::regex_match(one, m, frame));
        }
        // a frame whose bytes were cut still says so; a header-only one does not
        CHECK_EQ(all[2].line.find("(cut)"), std::string::npos);
    }
    std::vector<u8> big(2000, 0xAB);
    DebugFrame(kDirRx, 2, 3, 7, big.data(), 2000);
    const std::vector<Popped> cut = Drain(start);
    CHECK_EQ(cut.size(), std::size_t(1));
    if (cut.size() == 1) {
        CHECK(cut[0].line.find("FRAME rx    2>3 ch7 2000B (cut): ab") != std::string::npos);
    }
}

// ---- 6. the writer pops while the producers push ---------------------------------------------------------------------------------
// DebugPop copies the payload without the lock (nobody writes between the tail and the head): every record that comes out
// must be whole, in order per producer, and none may be lost or repeated.
void TestPopWhilePushing() {
    std::printf("the writer pops without the lock while three producers push: every record whole, none lost or repeated\n");
    Fresh fresh;
    constexpr int kThreads = 3;
    constexpr int kPerThread = 15000;
    std::atomic<int> attempts{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&, t] {
            std::mt19937 rng(900 + t);
            std::vector<u8> payload(1700);
            for (int i = 0; i < kPerThread; ++i) {
                const u16 sequence = static_cast<u16>(i);
                const u32 size = 1 + rng() % 1600;
                for (u32 j = 0; j < size; ++j) {
                    payload[j] = static_cast<u8>(t * 131 + sequence * 7 + j);
                }
                DebugFrame(kDirRx, static_cast<u16>(t), sequence, static_cast<u8>(t), payload.data(), size);
                attempts.fetch_add(1);
                if (i % 2000 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });
    }
    long popped = 0;
    long broken = 0;
    long out_of_order = 0;
    int last_sequence[kThreads];
    for (int& v : last_sequence) v = -1;
    std::thread consumer([&] {
        DebugRecord rec;
        u8 buf[1700];
        for (;;) {
            if (!DebugPop(&rec, buf, sizeof(buf))) {
                if (done.load()) {
                    // one more look: the last records were pushed before `done` was set
                    if (!DebugPop(&rec, buf, sizeof(buf))) {
                        break;
                    }
                } else {
                    std::this_thread::yield();
                    continue;
                }
            }
            if (rec.kind == kDbgLost) {
                continue; // (a hole: a marker precedes the next record of the lane)
            }
            ++popped;
            const int t = rec.src;
            const u16 sequence = rec.dest;
            bool whole = t < kThreads && rec.stored == std::min<u32>(rec.size, 1600) && rec.stored >= 1;
            for (u32 j = 0; whole && j < rec.stored; ++j) {
                whole = buf[j] == static_cast<u8>(t * 131 + sequence * 7 + j);
            }
            broken += whole ? 0 : 1;
            if (t < kThreads) {
                // (sequences are 16 bits and wrap after 65536: 15000 per thread never do)
                out_of_order += static_cast<int>(sequence) <= last_sequence[t] ? 1 : 0;
                last_sequence[t] = sequence;
            }
        }
    });
    for (auto& th : producers) {
        th.join();
    }
    done = true;
    consumer.join();
    const CaptureTotals totals = DebugCaptureTotals();
    CHECK_EQ(broken, 0L);
    CHECK_EQ(out_of_order, 0L);
    CHECK_EQ(attempts.load(), kThreads * kPerThread);
    // every attempt is a record that was popped or one that was counted as lost (no room, or the lock busy for longer than a producer waits)
    CHECK_EQ(static_cast<long>(totals.lost_full[kLaneHeavy] + totals.lost_busy[kLaneHeavy]) + popped, static_cast<long>(attempts.load()));
    CHECK_EQ(DebugBytesQueued(), 0u);
    std::printf("    %ld popped, %u lost (ring full %u, lock busy %u)\n", popped, totals.lost_full[1] + totals.lost_busy[1],
                totals.lost_full[1], totals.lost_busy[1]);
}

} // namespace

int main() {
    TestLanes();
    TestControlLaneFull();
    TestLockBusy();
    TestDiet();
    TestDietConserves();
    TestCommandRecords();
    TestFrameLines();
    TestPopWhilePushing();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
