// The printf-free text of the code that runs on the game's threads (azl/line.hpp, LogLine, DebugLine, the raw records
// of DebugIpc / DebugSvcReal, the console clock), run by Tests/run.sh. Every piece is compared with the snprintf
// format it replaces, on corner values and on random ones: the analysis scripts parse these lines, so "the same
// text" is a number here, not a promise.
#include <cinttypes>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
// libstdc++'s <regex> trips -Wmaybe-uninitialized inside its own headers at -O1 (see test_main.cpp).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include <regex>
#pragma GCC diagnostic pop
#include <set>
#include <string>
#include <vector>

#include "azl/line.hpp"
#include "azl/log.hpp"
#include "azl/log_files.hpp"
#include "azl/printf_lite.hpp"
#include "azl/stats.hpp"
#include "azl/wallclock.hpp"
#include "support.hpp"

static std::string Fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
static std::string Fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// The reference of the capture lines: the text newlib's vsnprintf makes of the 0.1.15 format, stored in the ring as a text
// record. (DebugEvent() itself now writes the conversions of the plugin without newlib - azl/printf_lite.hpp - and is
// compared with newlib in TestPrintfLite; the lines of the hook are compared with THIS, not with another run of the code
// under test.)
static void RefEvent(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void RefEvent(const char* format, ...) {
    char text[200];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    DebugEvent("%s", text);
}

static const u32 kCorners[] = {0,          1,          9,          10,         11,         15,         16,         99,
                               100,        255,        256,        999,        1000,       4095,       4096,       65535,
                               65536,      99999,      100000,     999999,     1000000,    0x7FFFFFFFu, 0x80000000u,
                               999999999u, 1000000000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x12345678u, 0x00140021u, 0x000B0000u};

static std::vector<u32> Values() {
    std::vector<u32> v(std::begin(kCorners), std::end(kCorners));
    std::mt19937 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        // all magnitudes: a random number of random bits
        const u32 bits = rng() % 33;
        v.push_back(bits == 0 ? 0 : (bits == 32 ? rng() : rng() & ((1u << bits) - 1)));
    }
    return v;
}

// ---- 1. the pieces against printf ------------------------------------------------------------------------------------
static void TestPieces() {
    std::printf("LineBuf pieces: the same bytes as %%0*lx, %%lu, %%d, %%*lu, %%s\n");
    for (u32 v : Values()) {
        char buf[64];
        for (u32 digits = 1; digits <= 10; ++digits) {
            LineBuf b(buf, sizeof(buf) - 1);
            b.Hex(v, digits);
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%0*lx", static_cast<int>(digits), static_cast<unsigned long>(v)));
        }
        {
            LineBuf b(buf, sizeof(buf) - 1);
            b.Hex(v);
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%lx", static_cast<unsigned long>(v)));
        }
        for (u32 digits : {1u, 2u, 8u}) { // capitals: "%0*lX" (the MACs of the wifi capture lines)
            LineBuf b(buf, sizeof(buf) - 1);
            b.Hex(v, digits, true);
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%0*lX", static_cast<int>(digits), static_cast<unsigned long>(v)));
        }
        {
            LineBuf b(buf, sizeof(buf) - 1);
            b.Dec(v);
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%lu", static_cast<unsigned long>(v)));
        }
        {
            LineBuf b(buf, sizeof(buf) - 1);
            b.SDec(static_cast<s32>(v));
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%d", static_cast<int>(static_cast<s32>(v))));
        }
        for (u32 width : {1u, 2u, 3u, 5u, 11u}) {
            LineBuf b(buf, sizeof(buf) - 1);
            b.DecPad(v, width, ' ');
            buf[b.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%*lu", static_cast<int>(width), static_cast<unsigned long>(v)));
            LineBuf z(buf, sizeof(buf) - 1);
            z.DecPad(v, width, '0');
            buf[z.size()] = 0;
            CHECK_EQ(std::string(buf), Fmt("%0*lu", static_cast<int>(width), static_cast<unsigned long>(v)));
        }
    }
    // strings: whole, limited, empty, null
    char buf[64];
    {
        LineBuf b(buf, sizeof(buf) - 1);
        b.Str("nwm::UDS").Str("").Str(nullptr).Str("abcdef", 3).Str("xy", 0).Ch('!');
        buf[b.size()] = 0;
        CHECK_EQ(std::string(buf), std::string("nwm::UDSabc!"));
    }
    // a line that does not fit is cut like snprintf's (never past the buffer), and later pieces add nothing
    {
        char small[8];
        LineBuf b(small, 5);
        b.Str("abc").Hex(0xABCDEF, 8).Dec(12345).Ch('x');
        CHECK_EQ(b.size(), 5u);
        CHECK(std::memcmp(small, "abc00", 5) == 0);
        char guard[4] = {'g', 'u', 'a', 'r'};
        LineBuf c(guard, 0); // capacity 0: an inactive line
        c.Str("zzz").Hex(1).Dec(2);
        CHECK_EQ(c.size(), 0u);
        CHECK(guard[0] == 'g' && guard[3] == 'r');
    }
}

// ---- 2. the log lines of the hook, built as hook.cpp / uds_hle.cpp build them, against the printf formats they replaced
// (the formats are copied from 0.1.15's hook.cpp and uds_hle.cpp; Tests/chain_sim runs the real compiled lines too) -------
static void TestHookLines() {
    std::printf("the lines of the hook: LogLine / DebugLine against the 0.1.15 printf formats\n");
    LogOn on;
    std::vector<std::string> sink_lines;
    SetLogSink([&sink_lines](const char* line) { sink_lines.push_back(line); });
    CHECK(DebugInit());
    std::mt19937 rng(777);
    const char* kServices[] = {"fs:USER", "APT:U", "gsp::Gpu", "hid:USER", "ndm:u", "nwm::UDS", "dlp:CLNT", "ptm:u", "cfg:u"};
    const char* kSignals[] = {"none", "home button", "sleep query", "wake-up", "?"};
    for (int i = 0; i < 4000; ++i) {
        const u32 a = rng(), b = rng() >> (rng() % 32), c = rng() >> (rng() % 32);
        const char* service = kServices[i % 9];
        const char* signal = kSignals[i % 5];
        sink_lines.clear();
        // srv GetServiceHandle
        {
            LogLine line;
            line.Str("srv: the game opened the REAL service '").Str(service).Str("' -> 0x").Hex(b, 8).Str(" (handle 0x").Hex(c).Str(")");
        }
        CHECK_EQ(sink_lines.size(), 1u);
        CHECK_EQ(sink_lines[0], Fmt("srv: the game opened the REAL service '%s' -> 0x%08lx (handle 0x%lx)", service,
                                    static_cast<unsigned long>(b), static_cast<unsigned long>(c)));
        // apt notification
        sink_lines.clear();
        {
            LogLine line;
            line.Str("apt: notification ").Dec(a % 12).Str(" (").Str(signal).Str(") for app 0x").Hex(b).Str(" -> 0x").Hex(c, 8);
        }
        CHECK_EQ(sink_lines[0], Fmt("apt: notification %lu (%s) for app 0x%lx -> 0x%08lx", static_cast<unsigned long>(a % 12),
                                    signal, static_cast<unsigned long>(b), static_cast<unsigned long>(c)));
        // svc: REAL
        sink_lines.clear();
        {
            LogLine line;
            line.Str("svc: REAL ").Str(service).Str(" command 0x").Hex(a & 0xFFFF, 4).Str(" (arg 0x").Hex(b).Str(") -> 0x").Hex(c, 8);
        }
        CHECK_EQ(sink_lines[0], Fmt("svc: REAL %s command 0x%04lx (arg 0x%lx) -> 0x%08lx", service,
                                    static_cast<unsigned long>(a & 0xFFFF), static_cast<unsigned long>(b),
                                    static_cast<unsigned long>(c)));
        // fake session lines of uds_hle.cpp
        const int ndm_session = static_cast<int>(a % 6);
        sink_lines.clear();
        {
            LogLine line;
            line.Str("uds: game opened ").Str("ndm:u").Str(" -> fake session 0x").Hex(b);
            line.Str(" (ndm session ").SDec(ndm_session).Str(")");
        }
        CHECK_EQ(sink_lines[0], Fmt("uds: game opened %s -> fake session 0x%lx%s", "ndm:u", static_cast<unsigned long>(b),
                                    Fmt(" (ndm session %d)", ndm_session).c_str()));
        sink_lines.clear();
        {
            LogLine line;
            line.Str("uds: game opened ").Str("ndm:u").Str(" -> fake session 0x").Hex(b);
            line.Str(" (all ").SDec(6).Str(" ndm sessions are in use: shares session ").SDec(ndm_session).Str(")");
        }
        CHECK_EQ(sink_lines[0], Fmt("uds: game opened %s -> fake session 0x%lx%s", "ndm:u", static_cast<unsigned long>(b),
                                    Fmt(" (all %d ndm sessions are in use: shares session %d)", 6, ndm_session).c_str()));
    }
    // the capture lines (DebugLine) come out of the ring as the DebugEvent lines of 0.1.15 did: pushed in pairs (new,
    // old), drained after each round
    DebugRecord rec;
    u8 buf[1700];
    for (int i = 0; i < 4000; ++i) {
        const u32 a = rng(), b = rng() >> (rng() % 32), c = rng() >> (rng() % 32);
        const char* service = kServices[i % 9];
        {
            DebugLine line;
            line.Str("srv GetServiceHandle '").Str(service).Str("' -> 0x").Hex(a, 8).Str(" (handle 0x").Hex(b).Str(")");
        }
        RefEvent("srv GetServiceHandle '%s' -> 0x%08lx (handle 0x%lx)", service, static_cast<unsigned long>(a),
                   static_cast<unsigned long>(b));
        {
            DebugLine line;
            line.Str("apt InquireNotification app 0x").Hex(a).Str(" -> ").Dec(c % 12).Str(" (").Str("sleep query").Str(")");
        }
        RefEvent("apt InquireNotification app 0x%lx -> %lu (%s)", static_cast<unsigned long>(a),
                   static_cast<unsigned long>(c % 12), "sleep query");
        {
            DebugLine line;
            line.Str("apt ").Str("ReplySleepQuery").Str(" app 0x").Hex(a).Str(" answer ").Dec(c % 3);
        }
        RefEvent("apt %s app 0x%lx answer %lu", "ReplySleepQuery", static_cast<unsigned long>(a), static_cast<unsigned long>(c % 3));
        {
            DebugLine line;
            line.Str("uds game closed its fake ").Str("nwm::UDS").Str(" session 0x").Hex(a);
        }
        RefEvent("uds game closed its fake %s session 0x%lx%s", "nwm::UDS", static_cast<unsigned long>(a), "");
        for (int pair = 0; pair < 4; ++pair) {
            std::string mine, printed;
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            DebugFormat(rec, buf, rec.ms, &mine);
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            DebugFormat(rec, buf, rec.ms, &printed);
            // (the time column is the same on both: compare the text after it)
            CHECK_EQ(mine.substr(mine.find(' ', 4)), printed.substr(printed.find(' ', 4)));
            if (mine.substr(mine.find(' ', 4)) != printed.substr(printed.find(' ', 4))) {
                std::fprintf(stderr, "    new: %s    old: %s", mine.c_str(), printed.c_str());
                SetLogSink(nullptr);
                return;
            }
        }
    }
    SetLogSink(nullptr);
}

// ---- 3. the raw records of the hook's pass-through lines against the DebugEvent lines they replaced ----------------------
static void TestRawRecords() {
    std::printf("DebugIpc / DebugSvcReal: raw words, the same lines as the 0.1.15 DebugEvent formats\n");
    LogOn on;
    CHECK(DebugInit());
    std::mt19937 rng(4242);
    DebugRecord rec;
    u8 buf[1700];
    const char* kNames[] = {"", "a", "fs:USER", "gsp::Gpu", "nwm::UDS", "12345678", "ELEVENCHARS", "dlp:CLNT", "boss:U"};
    std::vector<u32> hs = Values();
    for (int i = 0; i < 6000; ++i) {
        const u32 handle = hs[rng() % hs.size()], header = hs[rng() % hs.size()], arg = hs[rng() % hs.size()],
                  result = hs[rng() % hs.size()];
        DebugIpc(handle, header, arg, result);
        RefEvent("ipc h=0x%lx cmd=0x%08lx arg=0x%lx -> 0x%08lx", static_cast<unsigned long>(handle),
                   static_cast<unsigned long>(header), static_cast<unsigned long>(arg), static_cast<unsigned long>(result));
        const char* name = kNames[i % 9];
        const u32 command = hs[rng() % hs.size()] & 0xFFFF;
        DebugSvcReal(name, command, arg, result);
        RefEvent("svc REAL %s cmd 0x%04lx arg 0x%lx -> 0x%08lx", name, static_cast<unsigned long>(command),
                   static_cast<unsigned long>(arg), static_cast<unsigned long>(result));
        for (int pair = 0; pair < 2; ++pair) {
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            std::string raw;
            DebugFormat(rec, buf, rec.ms, &raw);
            CHECK_EQ(static_cast<int>(rec.kind), pair == 0 ? static_cast<int>(kDbgIpc) : static_cast<int>(kDbgSvcReal));
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            CHECK_EQ(static_cast<int>(rec.kind), static_cast<int>(kDbgText));
            std::string old;
            DebugFormat(rec, buf, rec.ms, &old);
            // (compare from the first character after the time column)
            CHECK_EQ(raw.substr(raw.find(' ', 5)), old.substr(old.find(' ', 5)));
            if (raw.substr(raw.find(' ', 5)) != old.substr(old.find(' ', 5))) {
                std::fprintf(stderr, "    new: %s    old: %s", raw.c_str(), old.c_str());
                return;
            }
            if (pair == 0) {
                // the order in the ring is ipc(new) ipc(old) svc(new) svc(old): the loop reads them in that order
            }
        }
    }
    // the records are small: 32 bytes for an ipc line, 40 for a svc REAL line (the text lines were ~16 + 55)
    DebugIpc(1, 2, 3, 4);
    CHECK(DebugPop(&rec, buf, sizeof(buf)));
    CHECK_EQ(static_cast<u32>(rec.stored), 16u);
    DebugSvcReal("srv", 1, 2, 3);
    CHECK(DebugPop(&rec, buf, sizeof(buf)));
    CHECK_EQ(static_cast<u32>(rec.stored), 24u);
}

// ---- 4. the commands of the UDS/ndm capture, heads made without printf ---------------------------------------------------
static void TestCommandHeads() {
    std::printf("DebugServiceCommand / DebugPullPacket: the same lines as DebugCommand with the head printf made\n");
    LogOn on;
    CHECK(DebugInit());
    std::mt19937 rng(99);
    DebugRecord rec;
    u8 buf[1700];
    for (int i = 0; i < 3000; ++i) {
        u32 req[64], rep[64];
        for (auto& w : req) w = rng();
        for (auto& w : rep) w = rng();
        const u32 id = rng() & (i % 2 ? 0xFF : 0xFFFF);
        const u32 rw = rng() % 64, pw = rng() % 64;
        const bool uds = i % 3 != 0;
        DebugServiceCommand(uds, id, req, rw, rep, pw);
        DebugCommand(Fmt("%s cmd 0x%02lx", uds ? "uds" : "ndm", static_cast<unsigned long>(id)).c_str(), req, rw, rep, pw);
        const u32 bind = rng(), bytes = rng(), node = rng() >> (rng() % 32);
        DebugPullPacket(bind, bytes, node, req, rw, rep, pw);
        DebugCommand(Fmt("uds PullPacket bind %lu: %lu bytes from node %lu", static_cast<unsigned long>(bind),
                         static_cast<unsigned long>(bytes), static_cast<unsigned long>(node)).c_str(),
                     req, rw, rep, pw);
        for (int pair = 0; pair < 2; ++pair) {
            std::string a, b;
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            DebugFormat(rec, buf, rec.ms, &a);
            CHECK(DebugPop(&rec, buf, sizeof(buf)));
            DebugFormat(rec, buf, rec.ms, &b);
            CHECK_EQ(a.substr(a.find(' ', 5)), b.substr(b.find(' ', 5)));
        }
    }
}

// ---- 4b. the capture line of a frame that is not game data (sent from the thread that ends a session) --------------------
static void TestWifiLine() {
    std::printf("DebugWifiLine: the same lines as the 0.1.15 DebugEvent format (MACs in capitals)\n");
    LogOn on;
    CHECK(DebugInit());
    std::mt19937 rng(31337);
    DebugRecord rec;
    u8 buf[1700];
    for (int i = 0; i < 6000; ++i) {
        u8 from[6], to[6];
        for (auto& b : from) b = static_cast<u8>(rng());
        for (auto& b : to) b = static_cast<u8>(rng());
        if (i % 7 == 0) {
            std::memset(from, 0, 6); // (the corners: all zero, all FF)
            std::memset(to, 0xFF, 6);
        }
        const char* dir = i % 2 ? "tx" : "rx";
        const u32 type = rng() % 8, channel = rng() % 256, size = i % 11 == 0 ? rng() : rng() % 3000;
        DebugWifiLine(dir, type, size, channel, from, to);
        RefEvent("wifi %s type %u %luB ch%u from %02X:%02X:%02X:%02X:%02X:%02X to %02X:%02X:%02X:%02X:%02X:%02X", dir, type,
                   static_cast<unsigned long>(size), channel, from[0], from[1], from[2], from[3], from[4], from[5], to[0], to[1],
                   to[2], to[3], to[4], to[5]);
        std::string a, b;
        CHECK(DebugPop(&rec, buf, sizeof(buf)));
        DebugFormat(rec, buf, rec.ms, &a);
        CHECK(DebugPop(&rec, buf, sizeof(buf)));
        DebugFormat(rec, buf, rec.ms, &b);
        CHECK_EQ(a.substr(a.find(' ', 5)), b.substr(b.find(' ', 5)));
    }
    // no record at level 0 and no ring is needed to call it
    g_debug_level = 0;
    const u8 mac[6] = {1, 2, 3, 4, 5, 6};
    DebugWifiLine("tx", 1, 2, 3, mac, mac);
    CHECK(!DebugPop(&rec, buf, sizeof(buf)));
}

// ---- 5. the console clock ----------------------------------------------------------------------------------------------
static void TestConsoleClock() {
    std::printf("FormatConsoleClock: the same dates as gmtime, 1900 to 2200\n");
    constexpr u64 kSecondsFrom1900To1970 = 2208988800ull;
    auto reference = [](u64 os_ms) {
        const std::time_t t = static_cast<std::time_t>(os_ms / 1000) - static_cast<std::time_t>(kSecondsFrom1900To1970);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return Fmt("%s.%03u", buf, static_cast<unsigned>(os_ms % 1000));
    };
    auto check = [&](u64 os_ms) {
        char out[24];
        FormatConsoleClock(os_ms, out);
        CHECK_EQ(std::string(out), reference(os_ms));
        CHECK_EQ(std::strlen(out), 23u);
    };
    check(0);
    check(1);
    check(86400ull * 1000 - 1);
    check(kSecondsFrom1900To1970 * 1000);          // 1970-01-01
    check(kSecondsFrom1900To1970 * 1000 - 1);      // 1969-12-31 23:59:59.999
    // the leap days: 1900 is not a leap year, 2000 is, 2100 is not
    for (const char* date : {"1900-02-28", "1900-03-01", "1904-02-29", "1999-12-31", "2000-02-28", "2000-02-29",
                             "2000-03-01", "2024-02-29", "2026-10-10", "2038-01-19", "2100-02-28", "2100-03-01",
                             "2199-12-31"}) {
        std::tm tm{};
        std::sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        // days since 1900 by counting (independent of the code under test): timegm for years >= 1970, a loop before
        std::time_t t = timegm(&tm);
        const u64 secs = static_cast<u64>(static_cast<long long>(t) + static_cast<long long>(kSecondsFrom1900To1970));
        for (u64 extra : {0ull, 3600ull * 23 + 59 * 60 + 59, 43200ull}) {
            check((secs + extra) * 1000 + 123);
        }
    }
    std::mt19937_64 rng(2026);
    for (int i = 0; i < 300000; ++i) {
        check((rng() % (300ull * 365 * 86400)) * 1000 + rng() % 1000); // 1900 .. ~2200
    }
    // the plugin's stamp: the one Log() line that has it, with a real date in it
    char out[24];
    FormatConsoleClock(3'988'000'000'000ull, out); // some day in 2026
    CHECK_EQ(std::string(out).substr(0, 4), std::string("2026"));
}

// ---- 6. the formatter of Log() / DebugEvent() against newlib's -----------------------------------------------------------
// (azl/printf_lite.hpp: the conversions of the plugin's own formats, written without newlib's vsnprintf chain, which needs
// ~1.4 KB of stack on the game's UDS thread)
static std::string Lite(const char* format, ...) __attribute__((format(printf, 1, 2)));
static std::string Lite(const char* format, ...) {
    char out[512];
    va_list args;
    va_start(args, format);
    CHECK(printf_lite::Supported(format));
    const u32 n = printf_lite::Supported(format) ? printf_lite::Format(out, sizeof(out) - 1, format, args) : 0;
    va_end(args);
    return std::string(out, n);
}

static void TestPrintfLite() {
    std::printf("printf_lite: the same bytes as newlib for every flag, width, length and conversion the plugin uses\n");
    std::mt19937_64 rng(60606);
    std::vector<unsigned long long> values;
    for (unsigned long long v : {0ull, 1ull, 9ull, 10ull, 15ull, 16ull, 99ull, 100ull, 255ull, 256ull, 65535ull, 0x7FFFFFFFull,
                                 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                                 0xFFFFFFFFFFFFFFFFull, 12345678901234ull}) {
        values.push_back(v);
    }
    for (int i = 0; i < 40; ++i) {
        values.push_back(rng() >> (rng() % 64));
    }
    const char* flags[] = {"", "-", "0", "-0"};
    const char* widths[] = {"", "1", "2", "3", "5", "8", "11", "20"};
    for (const char* f : flags) {
        for (const char* w : widths) {
            const std::string spec = std::string("%") + f + w;
            for (unsigned long long v : values) {
                const int i32 = static_cast<int>(static_cast<std::uint32_t>(v));
                const unsigned u32v = static_cast<unsigned>(v);
                const long lv = static_cast<long>(v);
                const unsigned long ulv = static_cast<unsigned long>(v);
                const long long llv = static_cast<long long>(v);
                const unsigned long long ullv = v;
                const std::size_t zv = static_cast<std::size_t>(v);
                auto check = [&](const std::string& format, auto value) {
                    const std::string with_text = "<" + format + ">";
                    // (the format is a variable here on purpose, and the argument is of the type the conversion reads)
                    char ref[256];
                    std::snprintf(ref, sizeof(ref), with_text.c_str(), value);
                    CHECK_EQ(Lite(with_text.c_str(), value), std::string(ref));
                };
                check(spec + "d", i32);
                check(spec + "i", i32);
                check(spec + "ld", lv);
                check(spec + "lld", llv);
                check(spec + "u", u32v);
                check(spec + "lu", ulv);
                check(spec + "llu", ullv);
                check(spec + "zu", zv);
                check(spec + "x", u32v);
                check(spec + "lx", ulv);
                check(spec + "llx", ullv);
                check(spec + "X", u32v);
                check(spec + "lX", ulv);
                check(spec + "zx", zv);
            }
        }
    }
    // characters and strings: width, '-', a precision, null
    const char* strings[] = {"", "a", "abc", "nwm::UDS", "a longer text than any width above, 0123456789"};
    for (const char* f : {"", "-"}) {
        for (const char* w : widths) {
            for (const char* pr : {"", ".0", ".1", ".3", ".10", ".100"}) {
                const std::string format = std::string("[%") + f + w + pr + "s]";
                for (const char* text : strings) {
                    char ref[512];
                    std::snprintf(ref, sizeof(ref), format.c_str(), text);
                    CHECK_EQ(Lite(format.c_str(), text), std::string(ref));
                }
                if (pr[0] == 0) { // (a null with a precision is glibc's "" and newlib's "(nu": the plugin passes none)
                    char ref[512];
                    std::snprintf(ref, sizeof(ref), format.c_str(), static_cast<const char*>(nullptr));
                    CHECK_EQ(Lite(format.c_str(), static_cast<const char*>(nullptr)), std::string(ref));
                }
            }
            const std::string cformat = std::string("[%") + f + w + "c]";
            for (int c : {int('a'), int(' '), int('Z'), 0x7F}) {
                char ref[64];
                std::snprintf(ref, sizeof(ref), cformat.c_str(), c);
                CHECK_EQ(Lite(cformat.c_str(), c), std::string(ref));
            }
        }
    }
    CHECK_EQ(Lite("%%"), Fmt("%%"));
    CHECK_EQ(Lite("100%% of %d%%", 7), Fmt("100%% of %d%%", 7));
    CHECK_EQ(Lite("[%*d][%-*d][%0*u][%.*s][%*.*s]", 6, 42, 6, 42, 6, 42u, 2, "abcdef", 8, 3, "abcdef"),
             Fmt("[%*d][%-*d][%0*u][%.*s][%*.*s]", 6, 42, 6, 42, 6, 42u, 2, "abcdef", 8, 3, "abcdef"));
    CHECK_EQ(Lite("[%*d]", -6, 42), Fmt("[%*d]", -6, 42));            // a negative width is '-' and its size
    CHECK_EQ(Lite("[%.*s]", -1, "abc"), Fmt("[%.*s]", -1, "abc"));    // a negative precision is none
    // the formats of the sources, as the lines read them
    CHECK_EQ(Lite("uds: Initialize (cmd 0x%02lx, version 0x%x) -> 0x%08lx", 0x1BUL, 0x400, 0xC8A113EAUL),
             Fmt("uds: Initialize (cmd 0x%02lx, version 0x%x) -> 0x%08lx", 0x1BUL, 0x400, 0xC8A113EAUL));
    CHECK_EQ(Lite("uds: connecting to host %02X:%02X:%02X:%02X:%02X:%02X", 0, 1, 0xAB, 0xCD, 0xEF, 0xFF),
             Fmt("uds: connecting to host %02X:%02X:%02X:%02X:%02X:%02X", 0, 1, 0xAB, 0xCD, 0xEF, 0xFF));
    CHECK_EQ(Lite("console: %lld MiB (%s)", 124LL, "New 3DS"), Fmt("console: %lld MiB (%s)", 124LL, "New 3DS"));
    CHECK_EQ(Lite("%s %s %d %u", "a", "b", -5, 7u), Fmt("%s %s %d %u", "a", "b", -5, 7u));

    // the line is cut where snprintf cuts it (and never past the buffer)
    for (unsigned cap = 0; cap < 40; ++cap) {
        char out[64];
        std::memset(out, '#', sizeof(out));
        const char* format = "abc %d def %s ghi %08lx jkl";
        const auto call = [&](unsigned c, ...) {
            va_list args;
            va_start(args, c);
            const u32 n = printf_lite::Format(out, c, format, args);
            va_end(args);
            return n;
        };
        const u32 n = call(cap, -12345, "text", 0xABCDUL);
        char ref[128];
        std::snprintf(ref, sizeof(ref), format, -12345, "text", 0xABCDUL);
        CHECK_EQ(n, std::min<u32>(cap, static_cast<u32>(std::strlen(ref))));
        CHECK(std::memcmp(out, ref, n) == 0);
        CHECK(out[n] == '#'); // nothing written past the cap
    }

    // what is not supported is said so (it goes to newlib, whole)
    for (const char* bad : {"%p", "%f", "%5.2f", "%e", "%g", "%a", "%+d", "% d", "%#x", "%#o", "%hd", "%hhx", "%jd", "%td",
                            "%Lf", "%.3d", "%.0u", "%.2x", "%05s", "%0c", "%-%", "%5%", "%l", "%lc", "%ls", "%n", "%o", "%",
                            "100%", "abc %5", "%llc", "%zs", "%lls", "%hs", "%4096d", "%99999d", "%.99999s", "%lo"}) {
        CHECK(!printf_lite::Supported(bad));
    }
    for (const char* good : {"", "plain", "%d", "%i", "%u", "%x", "%X", "%c", "%s", "%%", "%ld", "%lld", "%zu", "%08lx", "%-5s",
                             "%*d", "%.*s", "%5.3s", "%02X:%02X", "%-*s", "%0*lu", "a%sb%dc%%"}) {
        CHECK(printf_lite::Supported(good));
    }
}

// Log() and DebugEvent() themselves: a supported format and an exotic one end up as newlib prints them
static void TestLogFormats() {
    std::printf("Log() / DebugEvent(): the line as newlib prints it, also for a format newlib has to print\n");
    LogOn on;
    CHECK(DebugInit());
    DebugReset();
    std::vector<std::string> lines;
    SetLogSink([&lines](const char* line) { lines.push_back(line); });
    Log("uds: Bind node %lu channel %u from node 0x%x (recv buffer %lu)", 2UL, 243u, 0xFFFFu, 11824UL);
    Log("exotic: %5.1f %p %s", 3.14159, static_cast<void*>(nullptr), "x");
    Log("%s", std::string(600, 'z').c_str()); // cut at 399 characters
    CHECK_EQ(lines.size(), std::size_t(3));
    if (lines.size() == 3) {
        CHECK_EQ(lines[0], Fmt("uds: Bind node %lu channel %u from node 0x%x (recv buffer %lu)", 2UL, 243u, 0xFFFFu, 11824UL));
        CHECK_EQ(lines[1], Fmt("exotic: %5.1f %p %s", 3.14159, static_cast<void*>(nullptr), "x"));
        CHECK_EQ(lines[2], std::string(399, 'z'));
    }
    DebugEvent("uds twin: node %u>%u ch%u %uB came through", 3u, 2u, 243u, 140u);
    DebugEvent("exotic: %5.1f", 2.5);
    DebugEvent("%s", std::string(500, 'q').c_str()); // cut at 199
    DebugRecord rec;
    u8 buf[1700];
    std::vector<std::string> texts;
    while (DebugPop(&rec, buf, sizeof(buf))) {
        CHECK_EQ(static_cast<int>(rec.kind), static_cast<int>(kDbgText));
        CHECK_EQ(static_cast<int>(rec.pad), static_cast<int>(kLaneControl));
        texts.push_back(std::string(reinterpret_cast<char*>(buf), rec.stored));
    }
    CHECK_EQ(texts.size(), std::size_t(3));
    if (texts.size() == 3) {
        CHECK_EQ(texts[0], std::string("uds twin: node 3>2 ch243 140B came through"));
        CHECK_EQ(texts[1], std::string("exotic:   2.5"));
        CHECK_EQ(texts[2], std::string(199, 'q'));
    }
    SetLogSink(nullptr);
}

// Every format of every Log() / DebugEvent() call in the sources is one that printf_lite writes (a call with an exotic
// format would take newlib's 1.4 KB of stack on the game's UDS thread again, unnoticed), and every conversion in them is
// written as newlib writes it.
static std::string Unescape(const std::string& literal_body) {
    std::string out;
    for (std::size_t i = 0; i < literal_body.size(); ++i) {
        if (literal_body[i] == '\\' && i + 1 < literal_body.size()) {
            const char c = literal_body[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += literal_body[i];
        }
    }
    return out;
}

static void TestSourceFormats() {
    std::printf("the formats of the sources: every Log() / DebugEvent() call uses conversions printf_lite writes\n");
    namespace fs = std::filesystem;
    std::set<std::string> specs;
    int calls = 0;
    int files = 0;
    const std::regex call(R"re(\b(?:Log|DebugEvent)\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+))re");
    const std::regex literal(R"re("((?:[^"\\]|\\.)*)")re");
    for (const char* dir : {"Sources", "Includes"}) {
        if (!fs::exists(dir)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            const std::string ext = entry.path().extension().string();
            const std::string path = entry.path().generic_string();
            if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".hpp") || path.find("Sources/enet/") != std::string::npos ||
                path.find("printf_lite") != std::string::npos) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ++files;
            for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it) {
                std::string format;
                const std::string joined = (*it)[1].str();
                for (std::sregex_iterator lit(joined.begin(), joined.end(), literal); lit != std::sregex_iterator(); ++lit) {
                    format += Unescape((*lit)[1].str());
                }
                ++calls;
                if (!printf_lite::Supported(format.c_str())) {
                    std::fprintf(stderr, "  not a format printf_lite writes (%s): %s\n", path.c_str(), format.c_str());
                }
                CHECK(printf_lite::Supported(format.c_str()));
                for (const char* p = format.c_str(); *p; ++p) {
                    if (*p == '%') {
                        printf_lite::Spec s;
                        const char* after = printf_lite::ParseSpec(p + 1, &s);
                        if (after) {
                            specs.insert(std::string(p, after));
                            p = after - 1;
                        }
                    }
                }
            }
        }
    }
    CHECK(files > 10);
    CHECK(calls > 100); // (the sources have about 200: a pattern that stopped finding them must fail here)
    CHECK(specs.size() > 10);
    // each conversion that occurs, against newlib, with the type its length asks for
    int tested = 0;
    for (const std::string& spec : specs) {
        printf_lite::Spec s;
        printf_lite::ParseSpec(spec.c_str() + 1, &s);
        if (s.width_star || s.prec_star || s.conv == '%') {
            continue;
        }
        for (unsigned long long v : {0ull, 1ull, 7ull, 255ull, 4096ull, 0x7FFFFFFFull, 0xFFFFFFFFull, 0x123456789ABCull}) {
            char ref[128];
            std::string mine;
            switch (s.conv) {
            case 'c':
                std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<int>('a' + v % 26));
                mine = Lite(spec.c_str(), static_cast<int>('a' + v % 26));
                break;
            case 's':
                std::snprintf(ref, sizeof(ref), spec.c_str(), "nwm::UDS");
                mine = Lite(spec.c_str(), "nwm::UDS");
                break;
            case 'd':
            case 'i':
                if (s.length == 'L') {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<long long>(v));
                    mine = Lite(spec.c_str(), static_cast<long long>(v));
                } else if (s.length == 'l') {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<long>(v));
                    mine = Lite(spec.c_str(), static_cast<long>(v));
                } else {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<int>(v));
                    mine = Lite(spec.c_str(), static_cast<int>(v));
                }
                break;
            default:
                if (s.length == 'L') {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<unsigned long long>(v));
                    mine = Lite(spec.c_str(), static_cast<unsigned long long>(v));
                } else if (s.length == 'l') {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<unsigned long>(v));
                    mine = Lite(spec.c_str(), static_cast<unsigned long>(v));
                } else if (s.length == 'z') {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<std::size_t>(v));
                    mine = Lite(spec.c_str(), static_cast<std::size_t>(v));
                } else {
                    std::snprintf(ref, sizeof(ref), spec.c_str(), static_cast<unsigned>(v));
                    mine = Lite(spec.c_str(), static_cast<unsigned>(v));
                }
                break;
            }
            CHECK_EQ(mine, std::string(ref));
            ++tested;
        }
    }
    CHECK(tested > 80);
}

int main() {
    TestPieces();
    TestConsoleClock();
    TestHookLines();
    TestRawRecords();
    TestCommandHeads();
    TestWifiLine();
    TestPrintfLite();
    TestLogFormats();
    TestSourceFormats();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
