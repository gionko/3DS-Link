// The log and what it costs when it is off, run by Tests/run.sh (no console needed): the ini values (debug, menu_key),
// the master switch (g_debug_level: Log() and the capture do nothing, allocate nothing and create no file at 0), the
// files of the log on a real directory (the plugin's LogFiles, the very class, on top of a directory instead of the
// SD card), switching 0 -> 1 -> 0 while the writer is running, the rotation, the numbers of the menu's status page,
// the wrap of the messages on the screen, and the counters two threads share.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "azl/ini_values.hpp"
#include "azl/log.hpp"
#include "azl/log_files.hpp"
#include "azl/stats.hpp"
#include "azl/text_wrap.hpp"
#include "support.hpp"

namespace fs = std::filesystem;

extern "C" void azl_c_log(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // (log.cpp: the C sources' sink)

// ---- allocations are counted: "no allocation" at level 0 is a number, not a promise --------------------------------
static std::atomic<unsigned long> g_allocations{0};
void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    return operator new(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
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

// ---- the file layer over a directory -------------------------------------------------------------------------------
class DirFiles : public FileLayer {
public:
    void* OpenAppend(const std::string& path) override {
        ++opens;
        return std::fopen(path.c_str(), "ab");
    }
    u64 Size(void* file) override {
        std::fseek(static_cast<FILE*>(file), 0, SEEK_END);
        return static_cast<u64>(std::ftell(static_cast<FILE*>(file)));
    }
    void Write(void* file, const char* data, std::size_t size) override {
        ++writes;
        std::fwrite(data, 1, size, static_cast<FILE*>(file));
        std::fflush(static_cast<FILE*>(file)); // (readable at once: the tests read the files back; Flush below is only counted)
    }
    void Flush(void* file) override {
        ++flushes;
        std::fflush(static_cast<FILE*>(file));
    }
    void Close(void* file) override {
        last_close_level = g_debug_level; // (switching off sets the level first: nothing can reopen a file while it closes them)
        std::fclose(static_cast<FILE*>(file));
    }
    void Remove(const std::string& path) override {
        ++removes;
        std::remove(path.c_str());
    }
    void Rename(const std::string& from, const std::string& to) override {
        ++renames;
        std::rename(from.c_str(), to.c_str());
    }
    bool Exists(const std::string& path) override {
        ++exists_calls;
        return fs::exists(path);
    }
    int opens = 0, writes = 0, flushes = 0, removes = 0, renames = 0, exists_calls = 0;
    int last_close_level = -1;
};

static std::string ReadAll(const fs::path& p) {
    std::string s;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

/// name -> size of every file in `dir`.
static std::map<std::string, u64> Listing(const fs::path& dir) {
    std::map<std::string, u64> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        files[entry.path().filename().string()] = static_cast<u64>(fs::file_size(entry.path()));
    }
    return files;
}

/// The numbers of the blocks "<prefix>N.log" in a listing, in order (prefix "AzaharLink." or "AzaharLink.debug.").
static std::vector<int> Blocks(const std::map<std::string, u64>& listing, const std::string& prefix) {
    std::vector<int> out;
    for (const auto& entry : listing) {
        const std::string& name = entry.first;
        if (name.size() > prefix.size() + 4 && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - 4, 4, ".log") == 0) {
            const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - 4);
            if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
                out.push_back(std::atoi(digits.c_str()));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// "[%5lu.%03lu] ": a bracket, the seconds right-aligned in at least 5 characters, a dot, three digits, a bracket, a space.
static bool HasStamp(const std::string& prefix) {
    if (prefix.size() < 12 || prefix[0] != '[' || prefix.compare(prefix.size() - 2, 2, "] ") != 0) {
        return false;
    }
    const std::string body = prefix.substr(1, prefix.size() - 3); // "SSSSS.mmm"
    const std::size_t dot = body.rfind('.');
    if (dot == std::string::npos || dot < 5 || body.size() - dot - 1 != 3) {
        return false;
    }
    for (std::size_t i = dot + 1; i < body.size(); ++i) {
        if (body[i] < '0' || body[i] > '9') {
            return false;
        }
    }
    std::size_t i = 0;
    while (i < dot && body[i] == ' ') {
        ++i;
    }
    if (i == dot) {
        return false;
    }
    for (; i < dot; ++i) {
        if (body[i] < '0' || body[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// ---- 1. the ini values ---------------------------------------------------------------------------------------------
static void TestIniValues() {
    std::printf("ini values: debug is 0 or 1, menu_key can be off\n");
    // debug: 0 or 1. 2 (the third level of 0.1.14) is 1; negative and garbage are 0.
    CHECK_EQ(ini::DebugLevel("0"), 0);
    CHECK_EQ(ini::DebugLevel("1"), 1);
    CHECK_EQ(ini::DebugLevel("2"), 1);
    CHECK_EQ(ini::DebugLevel("7"), 1);
    CHECK_EQ(ini::DebugLevel("99999999999999999999"), 1); // (strtol saturates)
    CHECK_EQ(ini::DebugLevel("-1"), 0);
    CHECK_EQ(ini::DebugLevel("-100"), 0);
    CHECK_EQ(ini::DebugLevel(""), 0);
    CHECK_EQ(ini::DebugLevel("abc"), 0);
    CHECK_EQ(ini::DebugLevel("on"), 0); // not a number: off, the safe side
    CHECK_EQ(ini::DebugLevel(" 2"), 1);
    CHECK_EQ(ini::DebugLevel("1\r"), 1);
    CHECK_EQ(ini::DebugLevel("0x1"), 0); // (base 10: reads the 0, stops at the x)

    // menu_key: off and none in any case, spaces around them; a chord is not off; empty is not off.
    for (const char* v : {"off", "OFF", "Off", "oFf", "none", "NONE", "None", " off", "off ", "\toff\t", "off\r"}) {
        CHECK(ini::MenuKeyIsOff(v));
    }
    for (const char* v : {"", " ", "L+R+Select", "L+R+Start", "Select", "offline", "of", "o ff", "nonee", "off+L", "0", "1"}) {
        CHECK(!ini::MenuKeyIsOff(v));
    }
    // sleep_stop: 0 only logs, 1 ends the session of a client, 2 of a host too. The key was on/off before 0.1.16, so 1 and the
    // words of "on" are still 1 and the words of "off" are 0; a value that is none of them (an empty one, a typo, a 3) is the
    // default, 0: the action ends a game session and a slip must not turn it on.
    CHECK_EQ(ini::SleepStop("0"), 0);
    CHECK_EQ(ini::SleepStop("1"), 1);
    CHECK_EQ(ini::SleepStop("2"), 2);
    for (const char* v : {"on", "yes", "true", "ON", "True", " 1 ", "1\r"}) {
        CHECK_EQ(ini::SleepStop(v), 1);
    }
    for (const char* v : {"off", "no", "false", "OFF", "False", "0 ", "00x", "0\r"}) {
        CHECK_EQ(ini::SleepStop(v), 0);
    }
    for (const char* v : {" 2", "2 ", "2\r"}) {
        CHECK_EQ(ini::SleepStop(v), 2);
    }
    for (const char* v : {"", "3", "-1", "01", "02", "00", "x", "1x", "2x", "twice", "11"}) {
        CHECK_EQ(ini::SleepStop(v), ini::kSleepStopDefault);
    }
    CHECK_EQ(ini::kSleepStopDefault, 0);

    // The switches: 1/on/true/yes and 0/off/false/no in any case with blanks around; a slip is the default of the key, never "on"
    // (the rule before was "anything but 0 is on": enabled=off stayed ON, sleep_stop= turned itself ON)
    for (const char* v : {"1", "on", "On", "ON", "true", "TRUE", "yes", "Yes", " 1 ", "\ton\t", "1\r"}) {
        CHECK(ini::ParseBool(v, false));
        CHECK(ini::ParseBool(v, true));
    }
    for (const char* v : {"0", "off", "Off", "OFF", "false", "FALSE", "no", "No", " 0 ", "\toff\t", "0\r"}) {
        CHECK(!ini::ParseBool(v, false));
        CHECK(!ini::ParseBool(v, true));
    }
    for (const char* v : {"", " ", "2", "-1", "x", "tru", "ye", "onn", "01", "1x", "enabled", "1 0"}) {
        CHECK(!ini::ParseBool(v, false));
        CHECK(ini::ParseBool(v, true));
        bool known = true;
        ini::ParseBool(v, true, &known);
        CHECK(!known);
    }
    {
        bool known = false;
        ini::ParseBool("off", true, &known);
        CHECK(known);
    }

    // rcvbuf_kb: the number as written (the plugin judges it); text that is no number from end to end is the default
    CHECK_EQ(ini::RcvbufKb("64"), 64);
    CHECK_EQ(ini::RcvbufKb("32"), 32);
    CHECK_EQ(ini::RcvbufKb(" 16 "), 16);
    CHECK_EQ(ini::RcvbufKb("128"), 128);
    CHECK_EQ(ini::RcvbufKb("0"), 0);
    CHECK_EQ(ini::RcvbufKb("-5"), -5);
    for (const char* v : {"", "abc", "32x", "32 kb", "x32"}) {
        CHECK_EQ(ini::RcvbufKb(v), ini::kRcvbufKbDefault);
    }

    // one line of the file: key and value, blanks / BOM / CR dropped, no '=' or no key is not a line
    {
        ini::KeyValue kv = ini::SplitLine("host=10.0.0.9");
        CHECK(kv.ok && kv.key == "host" && kv.value == "10.0.0.9");
        kv = ini::SplitLine("  sleep_stop = 1 \r");
        CHECK(kv.ok && kv.key == "sleep_stop" && kv.value == "1");
        kv = ini::SplitLine("\xEF\xBB\xBF" "enabled=1");
        CHECK(kv.ok && kv.key == "enabled" && kv.value == "1");
        kv = ini::SplitLine("rcvbuf_kb\t=\t16");
        CHECK(kv.ok && kv.key == "rcvbuf_kb" && kv.value == "16");
        kv = ini::SplitLine("nickname=");
        CHECK(kv.ok && kv.key == "nickname" && kv.value.empty());
        kv = ini::SplitLine("password=a=b c");
        CHECK(kv.ok && kv.key == "password" && kv.value == "a=b c");
        for (const char* v : {"", "   ", "# a note", "garbage", "=value", "  =value"}) {
            CHECK(!ini::SplitLine(v).ok);
        }
    }

    // The whole file: 15 keys in the fixed order, LF, and the lines the writer keeps after them
    {
        const std::string text = ini::Render(ini::Fields());
        CHECK_EQ(text,
                 std::string("enabled=1\nhost=\nport=24872\nnickname=\npassword=\ntunnel=1\nautoconnect=1\nradio_like=1\n"
                             "socket_buffers=1\nnet_thread_core=-1\nmenu_key=off\ngame_name=\ndebug=0\nrcvbuf_kb=64\n"
                             "sleep_stop=0\n"));
        CHECK_EQ(text.size(), 177u);
        CHECK(text.find('\r') == std::string::npos);
        std::size_t at = 0;
        for (int i = 0; i < ini::kKeyCount; ++i) {
            const std::string want = std::string(ini::kKeyNames[i]) + "=";
            CHECK(text.compare(at, want.size(), want) == 0);
            at = text.find('\n', at) + 1;
        }
        CHECK_EQ(at, text.size());
        ini::Fields f;
        f.host = "a\nb";
        f.sleep_stop = 2;
        f.rcvbuf_kb = 32;
        const std::string two = ini::Render(f, {"# note", "future=7"});
        CHECK(two.find("host=a b\n") != std::string::npos); // a stray line break would corrupt the file
        CHECK(two.find("rcvbuf_kb=32\nsleep_stop=2\n# note\nfuture=7\n") != std::string::npos);
        // what does not fit under the limit is left out, whole lines only
        const std::string small = ini::Render(ini::Fields(), {"aaaaaaaaaa", "bbbbbbbbbb"}, text.size() + 12);
        CHECK(small.find("aaaaaaaaaa\n") != std::string::npos && small.find("bbbbbbbbbb") == std::string::npos);
    }
    CHECK(std::string(ini::kMenuKeyOff) == "off");
    CHECK(std::string(ini::kDefaultMenuKey) == "L+R+Select");
    CHECK(ini::MenuKeyIsOff(ini::kMenuKeyOff));
    CHECK(!ini::MenuKeyIsOff(ini::kDefaultMenuKey));
}

// ---- 2. the log at level 0 ------------------------------------------------------------------------------------------
static void TestLevelZeroIsFree() {
    std::printf("debug 0: Log() and the capture format, allocate, lock and record nothing\n");
    CHECK(g_debug_level == 0);
    CHECK(!DebugRingReady()); // nothing has allocated the ring
    int lines = 0;
    SetLogSink([&lines](const char*) { ++lines; });
    const unsigned long before = g_allocations.load();
    for (int i = 0; i < 20000; ++i) {
        Log("uds: command 0x%02x -> 0x%08lx (call %d) %s", i & 0xFF, 0UL, i, "text that would have to be formatted");
        DebugEvent("uds cmd 0x%02x req %08x", i & 0xFF, i);
        DebugFrame(kDirTx, 1, 2, 3, reinterpret_cast<const u8*>("abcd"), 4);
        DebugBeacon(kDirRx, nullptr, 1, reinterpret_cast<const u8*>("abcd"), 4);
        const u32 words[3] = {1, 2, 3};
        DebugCommand("uds cmd 0x14", words, 3, words, 3);
        // the printf-free lines of the code on the game's threads (azl/line.hpp): the same, free
        {
            LogLine line;
            line.Str("srv: the game opened the REAL service '").Str("fs:USER").Str("' -> 0x").Hex(0, 8).Dec(i).SDec(-i);
        }
        {
            DebugLine line;
            line.Str("apt ").Hex(static_cast<u32>(i)).Dec(7);
        }
        DebugIpc(1, 2, 3, 4);
        DebugSvcReal("ndm:u", 1, 2, 3);
        DebugServiceCommand(true, 0x14, words, 3, words, 3);
        DebugPullPacket(1, 2, 3, words, 3, words, 3);
    }
    CHECK_EQ(lines, 0);                                  // the sink never saw a line
    CHECK_EQ(g_allocations.load() - before, 0ul);        // not one allocation
    CHECK(!DebugRingReady());                            // and the calls above did not make a ring
    DebugRecord rec;
    u8 buf[64];
    CHECK(!DebugPop(&rec, buf, sizeof(buf)));
    azl_c_log("soc: socket() failed, errno %d", 5);      // the C sources' sink (ENet's platform layer)
    CHECK_EQ(lines, 0);

    // the same calls with the log on do reach the sink and the ring
    CHECK(DebugInit());
    CHECK(DebugRingReady());
    g_debug_level = 1;
    Log("a line");
    azl_c_log("a line from C %d", 1);
    DebugEvent("an event");
    CHECK_EQ(lines, 2);
    CHECK(DebugPop(&rec, buf, sizeof(buf)));
    g_debug_level = 0;
    while (DebugPop(&rec, buf, sizeof(buf))) {
    }
    // the ring stays allocated after a switch off (the producers may still be inside it), but records nothing
    DebugEvent("an event");
    CHECK(!DebugPop(&rec, buf, sizeof(buf)));
    SetLogSink([](const char*) {});
}

// ---- 3. the files of the log on a directory -------------------------------------------------------------------------
static fs::path MakeTempDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string("azl_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void TestNoFileAtLevelZero() {
    std::printf("debug 0: no file is created, whatever the plugin calls\n");
    const fs::path dir = MakeTempDir("level0");
    DirFiles files;
    {
        LogFiles log(&files, dir.string() + "/", platform::NowMs());
        g_debug_level = 0;
        // what LateInit, the writer, the state callback, the menu and Shutdown call
        log.Open();
        log.Append("a line queued by somebody that did not look at the level"); // (Log() never gets here at 0)
        log.Flush();
        log.DrainDebug(false);
        log.DrainDebug(true);
        log.Flush();
        log.Close();
        CHECK(log.SetLevel(0, false));
        CHECK(!log.log_open() && !log.debug_open());
        CHECK_EQ(files.opens, 0);
        CHECK_EQ(files.writes, 0);
        CHECK_EQ(files.removes + files.renames, 0);
        CHECK(Listing(dir).empty());
        CHECK(g_debug_level == 0);

        // a line that reached the queue with the log off (a race with the switch) is dropped, not kept for the next
        // time the log is switched on
        log.Append("stale line from level 0");
        log.Flush();
        CHECK(log.SetLevel(1, false));
        log.Flush();
        CHECK(!Contains(ReadAll(dir / "AzaharLink.log"), "stale line from level 0"));
        CHECK(log.SetLevel(0, false));
    }
    fs::remove_all(dir);
}

// A rotation that comes due while the log is being switched off must not bring the file back: the reopen after the
// rotation is one of the places that create a file.
static void TestRotationWhileSwitchedOff() {
    std::printf("a rotation that comes due with the log off does not reopen the files\n");
    const fs::path dir = MakeTempDir("rotoff");
    DirFiles files;
    LogFileLimits limits;
    limits.log_rotate_bytes = 100;
    limits.debug_rotate_bytes = 100;
    limits.keep = 2;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    CHECK(log.SetLevel(1, true));
    CHECK(log.log_open() && log.debug_open());
    g_debug_level = 0; // switched off by the menu, the files not closed yet
    log.Append(std::string(300, 'x').c_str());
    DebugEvent("this record is not captured");
    log.Flush(); // writes what was queued (the file is open), the file is over its size: rotation
    CHECK(!log.log_open());
    CHECK(ReadAll(dir / "AzaharLink.1.log").size() > 300); // the text went to the first block
    CHECK(!fs::exists(dir / "AzaharLink.log"));
    // the capture: a record that was in the ring when the level dropped is written, the rotation does not reopen
    g_debug_level = 1;
    DebugEvent("%s", std::string(300, 'y').c_str());
    g_debug_level = 0;
    log.DrainDebug(true);
    CHECK(!log.debug_open());
    CHECK(!fs::exists(dir / "AzaharLink.debug.log"));
    CHECK(fs::exists(dir / "AzaharLink.debug.1.log"));
    log.Close();
    fs::remove_all(dir);
}

// ---- the trail of a sleep: its own file, its own handle, written at once ------------------------------------------------
/// The directory files, but the writes to the main log wait while the gate is closed (the log writer in a long SD write).
class GatedFiles : public DirFiles {
public:
    void* OpenAppend(const std::string& path) override {
        void* file = DirFiles::OpenAppend(path);
        if (file && path.size() >= 14 && path.compare(path.size() - 14, 14, "AzaharLink.log") == 0) {
            main_log = file;
        }
        if (file && path.size() >= 20 && path.compare(path.size() - 20, 20, "AzaharLink.sleep.log") == 0) {
            trail = file;
        }
        return file;
    }
    void Write(void* file, const char* data, std::size_t size) override {
        if (file == main_log && gate_closed.load()) {
            ++blocked;
            while (gate_closed.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (file == trail && trail_gate_closed.load()) { // (a write of the trail that sits in the card)
            ++trail_blocked;
            while (trail_gate_closed.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        DirFiles::Write(file, data, size);
    }
    std::atomic<void*> main_log{nullptr};
    std::atomic<void*> trail{nullptr};
    std::atomic<bool> gate_closed{false};
    std::atomic<bool> trail_gate_closed{false};
    std::atomic<int> blocked{0};
    std::atomic<int> trail_blocked{0};
};

static void TestCrumbFile() {
    std::printf("the trail of a sleep: AzaharLink.sleep.log, its own handle, flushed at once, not held up by the log\n");
    const fs::path dir = MakeTempDir("crumb");
    {
        GatedFiles files;
        LogFiles log(&files, dir.string() + "/", platform::NowMs());
        const fs::path trail = dir / "AzaharLink.sleep.log";

        // level 0: nothing is created, nothing is written
        g_debug_level = 0;
        log.WriteCrumb("sleep: nothing at level 0");
        CHECK(!log.crumb_open());
        CHECK_EQ(files.opens, 0);
        CHECK(Listing(dir).empty());

        // level 1: the first line creates the file, each line is flushed (readable without closing), one handle for all
        CHECK(log.SetLevel(1, false));
        const int opens_before = files.opens;
        log.WriteCrumb("sleep: first");
        CHECK(log.crumb_open());
        CHECK(ReadAll(trail) == "sleep: first\n");
        log.WriteCrumb("sleep: second");
        CHECK(ReadAll(trail) == "sleep: first\nsleep: second\n");
        CHECK_EQ(files.opens - opens_before, 1);

        // the log writer sits in a long write of the main log (the lock of the main log's file is held): the trail is not
        // held up by it - it has its own handle and its own lock
        log.Append("a line for the main log");
        files.gate_closed = true;
        std::thread writer([&] { log.Flush(); });
        for (int i = 0; i < 2000 && files.blocked.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(files.blocked.load() > 0);
        // (the write is made by a thread of its own and waited for at most 2 s: a trail that did wait for the log's lock would
        // otherwise never come back here - the gate is only opened below - and the test would hang instead of failing)
        std::atomic<bool> crumb_done{false};
        const auto t = std::chrono::steady_clock::now();
        std::thread crumb([&] {
            log.WriteCrumb("sleep: written while the log writer sits in the card");
            crumb_done = true;
        });
        for (int i = 0; i < 2000 && !crumb_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        CHECK(crumb_done.load());
        CHECK(ms < 200);
        if (crumb_done.load()) {
            CHECK(Contains(ReadAll(trail), "sleep: written while the log writer sits in the card\n"));
        }
        files.gate_closed = false;
        crumb.join();
        writer.join();

        // the breadcrumb thread sits in a write to the card (the freeze it exists for) when the launch ends: closing the logs does
        // not wait for it - the main log is closed, the trail's handle is left to the process (a helper thread and a wait of at
        // most 2 s, so that a Close() that did wait fails instead of hanging)
        log.Append("a line written at the end of the launch");
        files.trail_gate_closed = true;
        std::thread stuck([&] { log.WriteCrumb("sleep: a write that never comes back"); });
        for (int i = 0; i < 2000 && files.trail_blocked.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(files.trail_blocked.load() > 0);
        std::atomic<bool> closed{false};
        const auto t_close = std::chrono::steady_clock::now();
        std::thread closer([&] {
            log.Close();
            closed = true;
        });
        for (int i = 0; i < 2000 && !closed.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double close_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_close).count();
        CHECK(closed.load());
        CHECK(close_ms < 200);
        CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "a line written at the end of the launch")); // (the main log is whole)
        CHECK(log.crumb_open());                                                                     // (the busy handle was left alone)
        files.trail_gate_closed = false;
        stuck.join();
        closer.join();
        CHECK(Contains(ReadAll(trail), "sleep: a write that never comes back\n"));

        // switching the log off closes it and writes no more; on again, it appends to the same file
        CHECK(log.SetLevel(0, false));
        CHECK(!log.crumb_open());
        log.WriteCrumb("sleep: after the switch off");
        CHECK(!Contains(ReadAll(trail), "after the switch off"));
        CHECK(log.SetLevel(1, false));
        log.WriteCrumb("sleep: after the switch on");
        const std::string all = ReadAll(trail);
        CHECK(Contains(all, "sleep: first\nsleep: second\n"));
        CHECK(Contains(all, "sleep: after the switch on\n"));
        CHECK(log.SetLevel(0, false));
    }
    // a launch that finds the trail of earlier ones over 128 KB starts it afresh; a smaller one is appended to
    {
        DirFiles files;
        {
            FILE* f = std::fopen((dir / "AzaharLink.sleep.log").string().c_str(), "wb");
            const std::string big(200 * 1024, 'x');
            std::fwrite(big.data(), 1, big.size(), f);
            std::fclose(f);
        }
        LogFiles log(&files, dir.string() + "/", platform::NowMs());
        CHECK(log.SetLevel(1, false));
        log.WriteCrumb("sleep: a new launch");
        CHECK(ReadAll(dir / "AzaharLink.sleep.log") == "sleep: a new launch\n");
        CHECK(log.SetLevel(0, false));
    }
    fs::remove_all(dir);
}

static void TestSwitching() {
    std::printf("debug 0 -> 1 -> 0 -> 1: the files open, write, close and open again\n");
    const fs::path dir = MakeTempDir("switch");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    g_debug_level = 0;
    Log("lost at level 0");
    CHECK(Listing(dir).empty());

    // 0 -> 1: both files appear (the ring exists already from the test before: this one asks for it anyway)
    CHECK(log.SetLevel(1, true));
    CHECK(g_debug_level == 1);
    CHECK(log.log_open() && log.debug_open());
    Log("first line, level 1");
    DebugEvent("capture line one");
    log.Flush();
    log.DrainDebug(false);
    CHECK_EQ(Listing(dir).size(), std::size_t(2));
    CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "first line, level 1"));
    CHECK(!Contains(ReadAll(dir / "AzaharLink.log"), "lost at level 0"));
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), "capture line one"));
    CHECK(ReadAll(dir / "AzaharLink.log")[0] == '[');
    // the stamp of a line is "[%5lu.%03lu] " (seconds padded with spaces, made by hand now: line.hpp), then the text
    {
        const std::string text = ReadAll(dir / "AzaharLink.log");
        const std::size_t at = text.find("first line, level 1");
        CHECK(at != std::string::npos && at >= 1);
        const std::size_t start = at == std::string::npos ? 0 : text.rfind('\n', at) + 1; // (npos + 1 = 0: the first line)
        CHECK(at != std::string::npos && HasStamp(text.substr(start, at - start)));
    }

    // 1 -> 0: what is left is written first, then the files are closed, then nothing moves
    Log("last line, level 1");
    DebugEvent("capture line two");
    CHECK(log.SetLevel(0, false));
    CHECK(g_debug_level == 0);
    CHECK_EQ(files.last_close_level, 0); // the level was 0 when the files were closed
    CHECK(!log.log_open() && !log.debug_open());
    const auto closed = Listing(dir);
    CHECK_EQ(closed.size(), std::size_t(2));
    CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "last line, level 1"));
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), "capture line two"));
    const int writes = files.writes;
    const int opens = files.opens;
    for (int i = 0; i < 100; ++i) {
        Log("not written %d", i);
        DebugEvent("not captured %d", i);
        log.Flush();
        log.DrainDebug(false);
        log.Open();
    }
    CHECK_EQ(files.writes, writes);
    CHECK_EQ(files.opens, opens);
    CHECK(Listing(dir) == closed);

    // 0 -> 1 again: the same files are appended to
    CHECK(log.SetLevel(1, true));
    Log("second session");
    DebugEvent("capture line three");
    log.Flush();
    log.DrainDebug(false);
    const std::string text = ReadAll(dir / "AzaharLink.log");
    CHECK(Contains(text, "first line, level 1") && Contains(text, "last line, level 1") && Contains(text, "second session"));
    CHECK(!Contains(text, "not written"));
    CHECK(!Contains(ReadAll(dir / "AzaharLink.debug.log"), "not captured"));
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), "capture line three"));
    CHECK_EQ(Listing(dir).size(), std::size_t(2));
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// A game's thread that logs must not grow the queue (a malloc on a thread with a tiny stack costs what a printf does):
// the room is made when the log is switched on, before any line, not by the first Flush that has something to write.
static void TestQueueHasItsRoomFromTheStart() {
    std::printf("switching the log on gives the queue its room: ~14 KB of lines allocate nothing\n");
    const fs::path dir = MakeTempDir("room");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    g_debug_level = 0;
    CHECK(log.SetLevel(1, false));
    const std::string line(100, 'q'); // (made before the count starts)
    const unsigned long before = g_allocations.load();
    for (int i = 0; i < 130; ++i) { // (130 x 115 bytes with the stamp: under the 16 KB the queue is made for)
        log.Append(line.c_str());
    }
    CHECK_EQ(g_allocations.load() - before, 0ul);
    log.Flush();
    // ... and the Flush leaves the room again for the next lines
    const unsigned long after_flush = g_allocations.load();
    for (int i = 0; i < 130; ++i) {
        log.Append(line.c_str());
    }
    CHECK_EQ(g_allocations.load() - after_flush, 0ul);
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// The writer thread is in the middle of a pass when the menu switches the log off: no file may come back.
static void TestSwitchOffWhileTheWriterRuns() {
    std::printf("debug 1 -> 0 while the writer runs: no file is opened again after the switch\n");
    const fs::path dir = MakeTempDir("race");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    for (int round = 0; round < 60; ++round) {
        CHECK(log.SetLevel(1, true));
        std::atomic<bool> stop{false};
        std::thread writer([&] { // what the plugin's writer does each pass
            while (!stop.load()) {
                log.Flush();
                if (g_debug_level > 0) {
                    log.DrainDebug(false);
                }
                log.Open(); // (the guard in the file opens is what keeps the files closed)
            }
        });
        std::thread logger([&] {
            for (int i = 0; i < 200; ++i) {
                Log("busy line %d", i);
                DebugEvent("busy event %d", i);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1 + round % 4));
        CHECK(log.SetLevel(0, false));
        logger.join();
        const int opens_at_off = files.opens;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        stop.store(true);
        writer.join();
        CHECK(!log.log_open() && !log.debug_open());
        CHECK_EQ(files.opens, opens_at_off); // nothing was opened after the switch
    }
    CHECK_EQ(Listing(dir).size(), std::size_t(2)); // (only the two files of the rounds before)
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// The two logs grow in numbered blocks: the current file becomes AzaharLink[.debug].N.log with N one more than the highest
// block there (1 is the oldest, the blocks read in order), and the lowest blocks go when there are more than kept. A launch
// never starts a file afresh, a block deleted by hand is not renumbered, and a directory with blocks from earlier launches
// goes on from its highest number.
static void TestRotation() {
    std::printf("rotation: numbered blocks in order (1 the oldest), the lowest go past the kept count, numbers never go back\n");
    const fs::path dir = MakeTempDir("rotate");
    DirFiles files;
    LogFileLimits limits;
    limits.log_rotate_bytes = 300;
    limits.debug_rotate_bytes = 400;
    limits.keep = 3;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    for (int i = 0; i < 60; ++i) {
        Log("line number %d of the main log, padded to make it longer", i);
        DebugEvent("capture record %d with some padding to make it longer", i);
        log.Flush();
        log.DrainDebug(false);
    }
    auto listing = Listing(dir);
    CHECK(listing.count("AzaharLink.log") && listing.count("AzaharLink.debug.log"));
    CHECK(!listing.count("AzaharLink.old.log")); // (the name of the old scheme is never made)
    std::vector<int> main_blocks = Blocks(listing, "AzaharLink.");
    std::vector<int> debug_blocks = Blocks(listing, "AzaharLink.debug.");
    CHECK_EQ(main_blocks.size(), std::size_t(3)); // keep: three blocks besides the current file, never more
    CHECK_EQ(debug_blocks.size(), std::size_t(3));
    CHECK_EQ(listing.size(), std::size_t(2 + 3 + 3));
    CHECK(main_blocks.size() == 3 && main_blocks[0] + 2 == main_blocks[2] && main_blocks[2] > 3); // consecutive, well past 1..3
    CHECK(debug_blocks.size() == 3 && debug_blocks[0] + 2 == debug_blocks[2]);
    const int main_hi = main_blocks.empty() ? 0 : main_blocks.back();
    const int debug_hi = debug_blocks.empty() ? 0 : debug_blocks.back();
    // the blocks read in order: a lower block holds earlier lines than a higher one, and the current file the newest
    CHECK(Contains(ReadAll(dir / ("AzaharLink." + std::to_string(main_blocks[0]) + ".log")), "line number"));
    CHECK(Contains(ReadAll(dir / "AzaharLink.log") + ReadAll(dir / ("AzaharLink." + std::to_string(main_hi) + ".log")), "line number 59"));
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log") + ReadAll(dir / ("AzaharLink.debug." + std::to_string(debug_hi) + ".log")),
                   "capture record 59"));
    {
        const std::string lower = ReadAll(dir / ("AzaharLink." + std::to_string(main_blocks[0]) + ".log"));
        const std::string higher = ReadAll(dir / ("AzaharLink." + std::to_string(main_hi) + ".log"));
        int first_low = -1, first_high = -1;
        std::sscanf(lower.c_str() + lower.find("line number "), "line number %d", &first_low);
        std::sscanf(higher.c_str() + higher.find("line number "), "line number %d", &first_high);
        CHECK(first_low >= 0 && first_high > first_low);
    }
    // a marker in the current file says where the part before it went
    CHECK(Contains(ReadAll(dir / "AzaharLink.log"), ("log rotated: the part before this is AzaharLink." + std::to_string(main_hi) + ".log").c_str()));
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});

    // a large current file from earlier launches becomes the next block when the log opens (the numbers go on, 1 is not reused)
    DirFiles files2;
    {
        FILE* f = std::fopen((dir / "AzaharLink.log").string().c_str(), "wb");
        std::string big(500, 'x');
        std::fwrite(big.data(), 1, big.size(), f);
        std::fclose(f);
    }
    LogFiles again(&files2, dir.string() + "/", platform::NowMs(), limits);
    g_debug_level = 1;
    again.Open();
    CHECK(again.log_open());
    listing = Listing(dir);
    main_blocks = Blocks(listing, "AzaharLink.");
    CHECK_EQ(main_blocks.size(), std::size_t(3));
    CHECK(main_blocks.size() == 3 && main_blocks.back() == main_hi + 1);
    CHECK_EQ(ReadAll(dir / ("AzaharLink." + std::to_string(main_hi + 1) + ".log")).size(), std::size_t(500));
    CHECK(ReadAll(dir / "AzaharLink.log").size() < 200);
    CHECK(files2.exists_calls > 0 && files2.exists_calls < 200); // (the blocks are looked for once, near their numbers)
    g_debug_level = 0;
    again.Close();

    // a block deleted by hand leaves a hole: the next block still takes the next number, the lowest still goes
    {
        const int hi = main_hi + 1;
        fs::remove(dir / ("AzaharLink." + std::to_string(hi - 1) + ".log"));
        DirFiles files3;
        LogFiles third(&files3, dir.string() + "/", platform::NowMs(), limits);
        SetLogSink([&third](const char* line) { third.Append(line); });
        CHECK(third.SetLevel(1, false));
        for (int i = 0; i < 6; ++i) {
            Log("line number %d of the third launch, padded to make it longer", i);
            third.Flush();
        }
        listing = Listing(dir);
        main_blocks = Blocks(listing, "AzaharLink.");
        CHECK(!main_blocks.empty() && main_blocks.back() > hi);
        CHECK(main_blocks.size() <= 3);
        CHECK(!listing.count("AzaharLink." + std::to_string(hi - 1) + ".log")); // (the hole is not filled)
        CHECK(third.SetLevel(0, false));
        SetLogSink([](const char*) {});
    }
    fs::remove_all(dir);
}

static void TestCardNotReadyYet() {
    std::printf("a file system that is not ready at the start: the lines wait and are written once it is\n");
    const fs::path dir = MakeTempDir("late") / "missing";
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    g_debug_level = 1;
    Log("written before the card was there");
    log.Open();            // the directory does not exist: nothing opens
    CHECK(!log.log_open());
    log.Flush();           // ... and the text is kept
    fs::create_directories(dir);
    log.Open();
    CHECK(log.log_open());
    CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "written before the card was there"));
    g_debug_level = 0;
    log.Close();
    SetLogSink([](const char*) {});
    fs::remove_all(dir.parent_path());
}

// ---- 4. the numbers of the status page ------------------------------------------------------------------------------
// ---- the capture file: its first line, the bounded passes of the drain, what the diet and the markers leave in it -----------
static u64 FakeConsoleClock() {
    return 3'988'000'000'000ull; // some day of 2026, in milliseconds since 1900
}

/// A card that is slow: every write takes `delay_ms`.
class SlowFiles : public DirFiles {
public:
    explicit SlowFiles(int delay_ms) : delay_ms_(delay_ms) {}
    void Write(void* file, const char* data, std::size_t size) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        DirFiles::Write(file, data, size);
    }

private:
    int delay_ms_;
};

/// Does the text begin with "  12.345 debug capture: this file begins here | console clock 2026-10-11 04:44:44.019\n"? (sscanf, not
/// <regex>: this file replaces operator new / delete, and GCC 12 reads the allocations inside <regex> as a mismatch)
static bool BeginsWithCaptureHeader(const std::string& text) {
    unsigned seconds = 0, millis = 0, year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, milli = 0;
    int used = 0;
    const int fields = std::sscanf(text.c_str(),
                                   " %u.%3u debug capture: this file begins here | console clock %4u-%2u-%2u %2u:%2u:%2u.%3u%n",
                                   &seconds, &millis, &year, &month, &day, &hour, &minute, &second, &milli, &used);
    return fields == 9 && year == 2026 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 && minute < 60 &&
           second < 61 && used > 0 && text[used] == '\n';
}

static void TestCaptureFileHeader() {
    std::printf("every capture file begins with a line that says when, by the console's clock\n");
    const fs::path dir = MakeTempDir("header");
    DirFiles files;
    LogFileLimits limits;
    limits.debug_rotate_bytes = 600;
    limits.keep = 2;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    log.SetConsoleClock(&FakeConsoleClock);
    CHECK(log.SetLevel(1, true));
    {
        const std::string text = ReadAll(dir / "AzaharLink.debug.log");
        CHECK(BeginsWithCaptureHeader(text));
    }
    // a rotation starts the next file with the same line (the lines of a file are read without the others)
    for (int i = 0; i < 12; ++i) {
        DebugEvent("capture record %d with some padding to make it longer than the rotation size", i);
        log.DrainDebug(false);
    }
    const auto listing = Listing(dir);
    const std::vector<int> blocks = Blocks(listing, "AzaharLink.debug.");
    CHECK_EQ(blocks.size(), std::size_t(2)); // (keep: the two newest blocks, numbered in order)
    CHECK(blocks.size() == 2 && blocks[1] == blocks[0] + 1);
    CHECK(BeginsWithCaptureHeader(ReadAll(dir / "AzaharLink.debug.log")));
    for (int n : blocks) {
        CHECK(BeginsWithCaptureHeader(ReadAll(dir / ("AzaharLink.debug." + std::to_string(n) + ".log"))));
    }
    CHECK(log.SetLevel(0, false));
    // a later launch appends to the file and says where it begins
    CHECK(log.SetLevel(1, true));
    std::string again = ReadAll(dir / "AzaharLink.debug.log");
    std::size_t heads = 0;
    for (std::size_t at = again.find("debug capture: this file begins here"); at != std::string::npos;
         at = again.find("debug capture: this file begins here", at + 1)) {
        ++heads;
    }
    CHECK(heads >= 2);
    CHECK(log.SetLevel(0, false));
    // no clock source (the PC): the line has none
    DirFiles files2;
    const fs::path dir2 = MakeTempDir("header2");
    LogFiles bare(&files2, dir2.string() + "/", platform::NowMs());
    CHECK(bare.SetLevel(1, true));
    const std::string bare_text = ReadAll(dir2 / "AzaharLink.debug.log");
    CHECK(Contains(bare_text, "debug capture: this file begins here\n"));
    CHECK(!Contains(bare_text, "console clock"));
    CHECK(bare.SetLevel(0, false));
    fs::remove_all(dir);
    fs::remove_all(dir2);

    // The first file of a launch is opened seconds after the capture began (the card is not ready before): the ring already
    // holds records, and the line stands where the oldest of them does - not at the moment the file was opened, which would
    // send the time column of the file back on its second line
    {
        const fs::path dir3 = MakeTempDir("header3");
        DirFiles files3;
        const u64 began_ms = platform::NowMs() - 10000; // (the plugin started 10 s ago)
        LogFiles early(&files3, dir3.string() + "/", began_ms);
        CHECK(DebugInit());
        DebugReset();
        g_debug_level = 1;
        DebugEvent("a record from before the card was ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        early.Open();
        early.DrainDebug(false);
        const std::string text = ReadAll(dir3 / "AzaharLink.debug.log");
        unsigned s1 = 0, m1 = 0, s2 = 0, m2 = 0;
        const std::size_t second_line = text.find('\n') == std::string::npos ? text.size() : text.find('\n') + 1;
        CHECK(std::sscanf(text.c_str(), " %u.%3u debug capture", &s1, &m1) == 2);
        CHECK(std::sscanf(text.c_str() + second_line, " %u.%3u a record", &s2, &m2) == 2);
        CHECK(Contains(text, "a record from before the card was ready\n"));
        std::printf("  the file begins at %u.%03u, its first record is at %u.%03u\n", s1, m1, s2, m2);
        CHECK_EQ(s1 * 1000 + m1, s2 * 1000 + m2);          // (not 60 ms later: that is when the file was opened)
        CHECK(s2 >= 9 && s2 <= 11);                         // (10 s into the launch)
        CHECK(early.SetLevel(0, false));
        fs::remove_all(dir3);
    }
}

static void TestDrainIsBounded() {
    std::printf("a pass of the drain is bounded (256 KB, 200 ms) and says when it stopped with records left\n");
    const fs::path dir = MakeTempDir("bounds");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    CHECK(log.SetLevel(1, true));
    DebugReset();
    const std::vector<u8> payload(1500, 7);
    // 250 frames of 1500 bytes: 760 KB of text, three passes of 256 KB
    for (int i = 0; i < 250; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    CHECK_EQ(DebugCaptureTotals().lost_full[1], 0u);
    const u64 before = fs::file_size(dir / "AzaharLink.debug.log");
    const int writes_before = files.writes;
    CHECK(log.DrainDebug(false)); // stopped at its bound, records left
    const u64 pass = fs::file_size(dir / "AzaharLink.debug.log") - before;
    CHECK(pass >= 256 * 1024 && pass <= 256 * 1024 + 72 * 1024);
    CHECK(files.writes - writes_before >= 4 && files.writes - writes_before <= 5); // in writes of 64 KB (+ one record)
    CHECK(DebugBytesQueued() > 0);
    int passes = 1;
    while (log.DrainDebug(false)) {
        ++passes;
        CHECK(passes < 6);
    }
    CHECK_EQ(passes, 2); // two passes cut at their bound, the third finds the end of the ring and says so
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(!log.DrainDebug(false)); // nothing left: no reason to come back soon
    // the end of the launch takes everything in one pass
    for (int i = 0; i < 250; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    CHECK(!log.DrainDebug(true));
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);

    // a slow card: the pass stops after 200 ms whatever it has written (each write takes 60 ms here)
    const fs::path dir2 = MakeTempDir("bounds2");
    SlowFiles slow(60);
    LogFiles slow_log(&slow, dir2.string() + "/", platform::NowMs());
    CHECK(slow_log.SetLevel(1, true));
    for (int i = 0; i < 250; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const auto begin = std::chrono::steady_clock::now();
    CHECK(slow_log.DrainDebug(false));
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    CHECK(spent >= 150 && spent < 450); // (about 4 writes: it looks at the clock after each 64 KB)
    CHECK(fs::file_size(dir2 / "AzaharLink.debug.log") < 5 * 64 * 1024); // (four writes of 64 KB, not a whole pass of 256 KB + more)
    CHECK(DebugBytesQueued() > 0);
    CHECK(slow_log.SetLevel(0, false)); // (the final drain empties the ring)
    CHECK_EQ(DebugBytesQueued(), 0u);
    fs::remove_all(dir2);
}

// The card is the game's too: the main log is flushed at every write (its last lines are what a post-mortem needs), the
// capture once a pass of the writer, not at every 64 KB write of it (2026-10-20).
static void TestFlushes() {
    std::printf("the main log is flushed at every write, the capture once a pass\n");
    const fs::path dir = MakeTempDir("flushes");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    DebugReset();
    const int writes0 = files.writes, flushes0 = files.flushes;
    Log("one line");
    log.Flush();
    CHECK_EQ(files.writes - writes0, 1);
    CHECK_EQ(files.flushes - flushes0, 1);
    const std::vector<u8> payload(1500, 7);
    for (int i = 0; i < 60; ++i) { // ~180 KB of text: three writes of 64 KB in one pass
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const int writes1 = files.writes, flushes1 = files.flushes;
    CHECK(!log.DrainDebug(false));
    CHECK(files.writes - writes1 >= 3 && files.writes - writes1 <= 4);
    CHECK_EQ(files.flushes - flushes1, 1);
    // the hex of a frame, formatted in one go, is the same text as before: 1500 bytes of 0x07 after the header
    const std::string text = ReadAll(dir / "AzaharLink.debug.log");
    std::string hex;
    for (int i = 0; i < 1500; ++i) {
        hex += "07";
    }
    CHECK(Contains(text, (hex + "\n").c_str()));
    // nothing to write: nothing flushed
    CHECK(!log.DrainDebug(false));
    CHECK_EQ(files.flushes - flushes1, 1);
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// A thread above the writer never waits for a lock of the log (Tekken stuck at the publisher's screen with the log on,
// 2026-10-20): while a LogLine of another thread holds the log's lock, Log() gives the line up at once and counts it; the
// writer thread (marked) waits and writes; the writer says in the log how many lines were given up.
static void TestNobodyWaitsForTheWriter() {
    std::printf("a thread never waits for the log's lock: the line is given up, counted and reported; only the writer waits\n");
    const fs::path dir = MakeTempDir("nowait");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, false));
    (void)LogTakeBusyDrops();
    std::atomic<bool> hold{true};
    std::atomic<bool> holding{false};
    std::thread holder([&] {
        LogLine line; // (holds the log's lock until it goes out of scope)
        line.Str("held for a while");
        holding = true;
        while (hold.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!holding.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // another thread: a few microseconds of trying, not the holder's time
    std::atomic<bool> probed{false};
    std::thread probe([&] {
        Log("BUSY-DROPPED: the lock is busy");
        probed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(probed.load());
    CHECK_EQ(LogTakeBusyDrops(), 1u);
    // the writer thread waits for it (and stays alive to the end: on the host a new thread may get a dead thread's
    // thread-local block, and would pass for the writer; on the console the writer lives as long as the launch)
    std::atomic<bool> written{false};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        platform::MarkLogWriterThread();
        Log("the writer waited");
        written = true;
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!written.load());
    hold = false;
    holder.join();
    probe.join();
    while (!written.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // the report: one more line given up (the count above was taken), said by the next flush
    {
        LogLine line;
        line.Str("another hold");
        std::thread other([&] { Log("BUSY-DROPPED too"); });
        other.join();
    }
    log.Flush();
    done = true;
    writer.join();
    const std::string text = ReadAll(dir / "AzaharLink.log");
    CHECK(Contains(text, "held for a while") && Contains(text, "the writer waited") && Contains(text, "another hold"));
    CHECK(!Contains(text, "BUSY-DROPPED"));
    CHECK(Contains(text, "log: 1 lines of other threads were given up: a lock of the log was busy"));
    CHECK_EQ(LogTakeBusyDrops(), 0u);
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

static void TestCaptureFileHasTheDietAndTheMarkers() {
    std::printf("the capture file: the counts of the diet come out with the drain, a hole is a LOST line (and the old line)\n");
    const fs::path dir = MakeTempDir("diet");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    CHECK(log.SetLevel(1, true));
    DebugReset();
    for (u32 i = 0; i < 10; ++i) {
        DebugIpc(0x250036, 0x000C0000, i, 0);
    }
    // the second is not over: the drain writes the three whole calls and keeps the count
    log.DrainDebug(false);
    {
        const std::string text = ReadAll(dir / "AzaharLink.debug.log");
        CHECK(Contains(text, "ipc h=0x250036 cmd=0x000c0000 arg=0x2 -> 0x00000000\n"));
        CHECK(!Contains(text, " x7 from "));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    log.DrainDebug(false); // (nobody called again: the writer's pass closes the count)
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), "ipc h=0x250036 cmd=0x000c0000 arg=0x9 -> 0x00000000 x7 from "));
    // at the end of the launch the count of the last second is written whatever its age
    for (u32 i = 0; i < 6; ++i) {
        DebugIpc(0x250036, 0x00080082, i, 0);
    }
    log.DrainDebug(true);
    CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), "cmd=0x00080082 arg=0x5 -> 0x00000000 x3 from "));

    // a hole: frames beyond the ring, drained, one more frame
    const std::vector<u8> payload(1500, 7);
    for (int i = 0; i < 400; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const u32 lost = DebugCaptureTotals().lost_full[1];
    CHECK(lost > 0);
    log.DrainDebug(true);
    DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    log.DrainDebug(true);
    const std::string text = ReadAll(dir / "AzaharLink.debug.log");
    const std::size_t marker = text.find(" LOST ");
    CHECK(marker != std::string::npos);
    CHECK(Contains(text, (" LOST " + std::to_string(lost) + " heavy records from ").c_str()));
    // the line the analysis scripts count holes by is still written (once for the pass that saw them)
    CHECK(Contains(text, ("debug: " + std::to_string(lost) + " records were dropped (").c_str()));
    CHECK(Contains(text, "lock was busy: the LOST lines say which)\n"));
    // the marker comes before the frame that follows the hole, and after the frames before it
    const std::size_t last_frame = text.rfind("FRAME rx");
    CHECK(marker < last_frame);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);
}

static void TestStatusNumbers() {
    std::printf("status numbers: made of the always-on counters, formatted for the menu's box\n");
    Stats s;
    std::memset(&s, 0, sizeof(s));
    // a 5 s window: 300 game frames (60 fps), the longest interval 40 ms (25 fps)
    s.frames = 299;
    s.frame_gap_max = 40 * AZL_TICKS_PER_MS;
    // The network loop ran the whole window (5000 ms of wall time): 300 ms of CPU - ENet 150, frames+engine 100, queue+tick 50,
    // each already without the soc calls and the lock waits that fell in it - and waits that make up the rest: the soc calls
    // (recv 160 ms in 400 calls, send 8 ms in 80) and idle in poll() (4527 ms in 100 calls), 5 ms of the engine's lock.
    s.t_net_wall = 5000 * AZL_TICKS_PER_MS;
    s.t_net_service = 150 * AZL_TICKS_PER_MS;
    s.t_net_events = 100 * AZL_TICKS_PER_MS;
    s.t_net_send = 50 * AZL_TICKS_PER_MS;
    s.soc_calls[kSocPoll] = 100;
    s.soc_ticks[kSocPoll] = 4527 * AZL_TICKS_PER_MS;
    s.soc_calls[kSocRecvData] = 400;
    s.soc_ticks[kSocRecvData] = 160 * AZL_TICKS_PER_MS; // mean 0.4 ms
    s.soc_max_ticks[kSocRecvData] = static_cast<u32>(9.1 * AZL_TICKS_PER_MS);
    s.soc_over20[kSocRecvData] = 2;
    s.soc_over100[kSocRecvData] = 1;
    s.soc_calls[kSocSend] = 80;
    s.soc_ticks[kSocSend] = 8 * AZL_TICKS_PER_MS;       // mean 0.1 ms
    s.soc_max_ticks[kSocSend] = 1 * AZL_TICKS_PER_MS;
    // the game's threads inside the UDS commands: 300 ms of wall time, 15 ms of it waiting for the engine's lock (of the 20 ms
    // all threads waited, 5 ms were the network thread's); 50 ms in the frame callback; 20 ms in the writer (CPU)
    s.ipc_sum_ticks = 300 * AZL_TICKS_PER_MS;
    s.lock_wait_ticks = 20 * AZL_TICKS_PER_MS;
    s.lock_wait_net_ticks = 5 * AZL_TICKS_PER_MS;
    s.t_cb = 50 * AZL_TICKS_PER_MS;
    s.t_writer = 20 * AZL_TICKS_PER_MS;
    s.loop_gap_max = 31;
    s.ring_peak_units = 12;
    s.ring_overwritten = 3;
    s.tx_stale_drops = 4;
    s.tx_cap_drops = 5;
    s.fwd_candidates = 40;
    s.drop_fwd_dup = 38;

    const StatusNumbers n = MakeStatusNumbers(s, 5000, 12, 7, 65536);
    CHECK(n.valid);
    CHECK_EQ(n.window_ms, 5000u);
    CHECK_EQ(n.fps10, 598u);      // 299 frames in 5 s = 59.8
    CHECK_EQ(n.fps_min10, 250u);  // 40 ms = 25.0 fps
    // CPU only: the network thread 300 ms in 5 s = 60.0 ms per second (the soc calls and the poll are its waiting and are not
    // in it); the game threads 300 - 15 ms of lock wait = 285 ms = 57.0; the frame callback 10.0; the writer 4.0
    CHECK(n.net10 >= 600 - 2 && n.net10 <= 600 + 2);
    CHECK(n.uds10 >= 570 - 2 && n.uds10 <= 570 + 2);
    CHECK_EQ(n.frame10, 100u);
    CHECK_EQ(n.writer10, 40u);
    CHECK_EQ(n.plugin10, n.net10 + n.uds10 + n.frame10); // (the writer's figure is wall time of a thread that waits for the core: shown, not added)
    CHECK(n.plugin10 >= 1270 - 4 && n.plugin10 <= 1270 + 4);
    CHECK_EQ(n.loop_pause_ms, 31u);
    CHECK_EQ(n.soc_mean_us, static_cast<u32>((160 + 8) * AZL_TICKS_PER_MS / (400 + 80) / 268)); // recv and send, every call timed
    CHECK(n.soc_max_us >= 9000 && n.soc_max_us <= 9200);
    CHECK_EQ(n.soc_over20, 2u);
    CHECK_EQ(n.soc_over100, 1u);
    CHECK_EQ(n.ring_peak_bytes, 384u); // 12 units of 32 bytes
    CHECK_EQ(n.ring_lost, 3u);
    CHECK_EQ(n.tx_stale, 4u);
    CHECK_EQ(n.tx_cap, 5u);
    CHECK_EQ(n.twin_candidates, 40u);
    CHECK_EQ(n.twin_dropped, 38u);
    CHECK_EQ(n.rtt_ms, 12u);
    CHECK_EQ(n.loss_permille, 7u);
    CHECK_EQ(n.soc_ring_kb, 64);

    const std::string text = FormatStatusNumbers(n);
    CHECK(Contains(text, "Last 5 s: 59.8 fps (slowest 25.0)"));
    CHECK(Contains(text, "Plugin CPU ms/s 127.0: net 60.0, uds 57.0,\n  frame 10.0, log 4.0 wall\n"));
    CHECK(!Contains(text, "Plugin ms/s")); // (it said ms/s of wall clock before 0.1.16)
    CHECK(Contains(text, "Net loop: longest pause 31 ms"));
    CHECK(Contains(text, "soc calls: mean 0.3, max 9.1 ms\n  over 20 ms: 2, over 100 ms: 1\n"));
    CHECK(Contains(text, "Ring: peak 384 B, lost 3, soc 64 KB\n"));
    CHECK(Contains(text, "TX not sent: 4 old, 5 queue full\n"));
    CHECK(Contains(text, "Twins: 40 seen, 38 dropped\n"));
    CHECK(Contains(text, "Room: rtt 12 ms, loss 7/1000"));
    // compact: it goes into the message box of the details, 13 lines of about 45 characters
    int lines = 0;
    std::size_t widest = 0, at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end;
        widest = std::max(widest, end - at);
        ++lines;
        at = end + 1;
    }
    CHECK(lines <= 10);
    CHECK(widest <= 44); // the width of the framework's message box (about 45 characters of its font)

    // no window yet / no frames / the ring not known
    CHECK(!MakeStatusNumbers(s, 0, 0, 0, 0).valid);
    CHECK(Contains(FormatStatusNumbers(StatusNumbers()), "first window"));
    Stats quiet;
    std::memset(&quiet, 0, sizeof(quiet));
    const StatusNumbers q = MakeStatusNumbers(quiet, 5000, 0, 0, 0);
    CHECK(q.valid && q.fps10 == 0 && q.fps_min10 == 0 && q.plugin10 == 0 && q.soc_mean_us == 0);
    CHECK_EQ(q.soc_ring_kb, -1);
    CHECK(Contains(FormatStatusNumbers(q), "soc ?"));
    CHECK(Contains(FormatStatusNumbers(q), ": 0.0 fps"));
    // 3DS-sized windows (the writer wakes late): 6.3 s
    CHECK_EQ(MakeStatusNumbers(s, 6300, 0, 0, 4416).soc_ring_kb, 4);
}

// ---- 4b. what the CPU figure means ------------------------------------------------------------------------------------
// The sums of Stats are CPU (the producers take the waits off their sections: the loop of the room client, the writer);
// MakeCpuNumbers turns them into ms per second and lays the waits next to them. What these tests pin: waiting adds nothing to
// the CPU figure, nothing is counted twice, the pieces of the network loop add up to its wall time, a window longer than the
// 16 s a 32-bit tick sum holds is right, and an idle window closes.
static void TestCpuNumbers() {
    std::printf("the CPU figure: waits are not CPU, nothing twice, the loop closes, a long window does not wrap\n");
    const u64 ms = AZL_TICKS_PER_MS;
    Stats base;
    std::memset(&base, 0, sizeof(base));
    // 10 s window; the net loop: 400 ms CPU (ENet 120, frames 200, queue 80), soc recv+send 900 ms, poll 8,650 ms, pauses 40 ms,
    // 10 ms of lock wait of the network thread (all in all 400 + 900 + 8650 + 40 + 10 = 10,000 ms)
    base.t_net_wall = 10000 * ms;
    base.t_net_service = 120 * ms;
    base.t_net_events = 200 * ms;
    base.t_net_send = 80 * ms;
    base.soc_ticks[kSocRecvData] = 700 * ms;
    base.soc_ticks[kSocRecvEmpty] = 100 * ms;
    base.soc_ticks[kSocSend] = 100 * ms;
    base.soc_ticks[kSocPoll] = 8650 * ms;
    base.t_net_pause = 40 * ms;
    base.lock_wait_ticks = 10 * ms;
    base.lock_wait_net_ticks = 10 * ms;
    base.ipc_sum_ticks = 150 * ms;
    base.t_cb = 30 * ms;
    base.t_writer = 90 * ms;
    base.t_writer_wait = 6400 * ms;
    CpuNumbers c = MakeCpuNumbers(base, 10000);
    CHECK_EQ(c.enet10, 120u);     // 12 ms per second
    CHECK_EQ(c.frames10, 200u);
    CHECK_EQ(c.queue10, 80u);
    CHECK_EQ(c.net10, 400u);
    CHECK_EQ(c.uds10, 150u);      // (the lock wait was the network thread's: nothing of it comes off the game threads)
    CHECK_EQ(c.frame10, 30u);
    CHECK_EQ(c.writer10, 90u);
    CHECK_EQ(c.plugin10, 400u + 150u + 30u); // (the writer apart)
    CHECK_EQ(c.soc10, 900u);
    CHECK_EQ(c.poll10, 8650u);
    CHECK_EQ(c.pause10, 40u);
    CHECK_EQ(c.sd10, 6400u);      // the writer waited for the card 6.4 s of the 10: not CPU
    CHECK_EQ(c.lock10, 10u);
    CHECK_EQ(c.netlock10, 10u);
    CHECK_EQ(c.wall10, 10000u);
    CHECK_EQ(c.other10, 0u);      // cpu 400 + soc 900 + poll 8650 + pauses 40 + lock 10 = the wall: it closes
    CHECK_EQ(c.net10 + c.soc10 + c.poll10 + c.pause10 + c.netlock10 + c.other10, c.wall10);

    // waiting adds nothing to the CPU figure: more soc time, more poll time, more card time, more lock time
    Stats more = base;
    more.soc_ticks[kSocRecvData] += 3000 * ms;
    more.soc_ticks[kSocPoll] += 5000 * ms;
    more.t_writer_wait += 9000 * ms;
    more.t_net_pause += 800 * ms;
    more.t_net_wall += 8800 * ms; // (the wall grows with the waits)
    const CpuNumbers m = MakeCpuNumbers(more, 10000);
    CHECK_EQ(m.plugin10, c.plugin10);
    CHECK_EQ(m.net10, c.net10);
    CHECK_EQ(m.uds10, c.uds10);
    CHECK_EQ(m.writer10, c.writer10);
    CHECK(m.soc10 > c.soc10 && m.poll10 > c.poll10 && m.sd10 > c.sd10 && m.pause10 > c.pause10);

    // The lock wait of the game's threads comes off the time they spent in the commands (they waited, they did not work),
    // never below zero; the network thread's share is not taken off twice
    Stats lock = base;
    lock.lock_wait_ticks = 70 * ms;
    lock.lock_wait_net_ticks = 10 * ms; // 60 ms of the game threads'
    CHECK_EQ(MakeCpuNumbers(lock, 10000).uds10, 90u);
    lock.lock_wait_ticks = 5000 * ms;   // (more than they were in the commands at all)
    CHECK_EQ(MakeCpuNumbers(lock, 10000).uds10, 0u);
    lock.lock_wait_ticks = 3 * ms;
    lock.lock_wait_net_ticks = 10 * ms; // (the network thread's share larger than the whole: a torn read, say; no wrap)
    CHECK_EQ(MakeCpuNumbers(lock, 10000).uds10, 150u);

    // an idle window: the loop sat in poll() the whole time. CPU ~0, and wall = poll + the little that ran
    Stats idle;
    std::memset(&idle, 0, sizeof(idle));
    idle.t_net_wall = 5000 * ms;
    idle.t_net_service = 6 * ms;
    idle.t_net_events = 0;
    idle.t_net_send = 2 * ms;
    idle.soc_ticks[kSocPoll] = 4985 * ms;
    idle.soc_ticks[kSocRecvEmpty] = 7 * ms;
    const CpuNumbers q = MakeCpuNumbers(idle, 5000);
    CHECK_EQ(q.wall10, 10000u);
    CHECK(q.plugin10 <= 20);
    CHECK(q.net10 + q.soc10 + q.poll10 + q.pause10 + q.netlock10 + q.other10 >= 9990 &&
          q.net10 + q.soc10 + q.poll10 + q.pause10 + q.netlock10 + q.other10 <= 10010); // 1000 ms/s +- 1
    CHECK(q.poll10 >= 9960);

    // a window of 20 s - the log writer can be late behind a slow card - holds more ticks than 32 bits do (2^32 = 16 s): a CPU
    // sum of 7 s (and a wall of 20 s) are right
    Stats longw;
    std::memset(&longw, 0, sizeof(longw));
    longw.t_net_wall = 20000 * ms;
    longw.t_net_service = 4000 * ms;
    longw.t_net_events = 2000 * ms;
    longw.t_net_send = 1000 * ms;
    longw.soc_ticks[kSocPoll] = 13000 * ms;
    longw.frame_gap_sum = 19000 * ms;
    longw.ipc_sum_ticks = 3000 * ms;
    CHECK(longw.t_net_wall > 0xFFFFFFFFull && longw.frame_gap_sum > 0xFFFFFFFFull); // (more than a 32-bit sum holds)
    const CpuNumbers w = MakeCpuNumbers(longw, 20000);
    CHECK_EQ(w.net10, 3500u);   // 7 s in 20 s = 350 ms per second
    CHECK_EQ(w.uds10, 1500u);
    CHECK_EQ(w.poll10, 6500u);
    CHECK_EQ(w.wall10, 10000u);
    CHECK_EQ(w.other10, 0u);
    // ... and a window of zero length is no window
    const CpuNumbers z = MakeCpuNumbers(base, 0);
    CHECK(z.plugin10 > 0); // (the division by zero is a division by one: the caller never asks, and nothing crashes)
}

// The window that ends is taken whole: the counters cleared, the 64-bit time sums read and cleared one by one, and what is added
// meanwhile lands in this window or the next - none is lost or torn. Four threads add to the sums while a fifth takes windows.
static void TestTakeStats() {
    std::printf("TakeStats: a window is taken whole, the time sums are never torn or lost\n");
    std::memset(&g_stats, 0, sizeof(g_stats));
    g_stats.frames = 7;
    g_stats.pull_calls = 9;
    g_stats.soc_calls[kSocSend] = 3;
    g_stats.t_net_service = 111;
    g_stats.soc_ticks[kSocPoll] = 222;
    g_stats.frame_gap_sum = 333;
    g_stats.lock_wait_ticks = 444;
    g_stats.t_writer_wait = 555;
    // every other time sum too, each with a value of its own (a sum that TakeStats forgets to read or to clear shows)
    g_stats.t_net_events = 1001;
    g_stats.t_net_send = 1002;
    g_stats.t_cb = 1003;
    g_stats.t_writer = 1004;
    g_stats.ipc_sum_ticks = 1005;
    g_stats.t_net_wall = 1006;
    g_stats.t_net_pause = 1007;
    g_stats.lock_wait_net_ticks = 1008;
    g_stats.lock_hold_ticks = 1012;
    g_stats.soc_ticks[kSocRecvData] = 1009;
    g_stats.soc_ticks[kSocRecvEmpty] = 1010;
    g_stats.soc_ticks[kSocSend] = 1011;
    Stats w = TakeStats();
    CHECK_EQ(w.t_net_events, 1001u);
    CHECK_EQ(w.t_net_send, 1002u);
    CHECK_EQ(w.t_cb, 1003u);
    CHECK_EQ(w.t_writer, 1004u);
    CHECK_EQ(w.ipc_sum_ticks, 1005u);
    CHECK_EQ(w.t_net_wall, 1006u);
    CHECK_EQ(w.t_net_pause, 1007u);
    CHECK_EQ(w.lock_wait_net_ticks, 1008u);
    CHECK_EQ(w.lock_hold_ticks, 1012u);
    CHECK_EQ(w.soc_ticks[kSocRecvData], 1009u);
    CHECK_EQ(w.soc_ticks[kSocRecvEmpty], 1010u);
    CHECK_EQ(w.soc_ticks[kSocSend], 1011u);
    CHECK_EQ(w.frames, 7u);
    CHECK_EQ(w.pull_calls, 9u);
    CHECK_EQ(w.soc_calls[kSocSend], 3u);
    CHECK_EQ(w.t_net_service, 111u);
    CHECK_EQ(w.soc_ticks[kSocPoll], 222u);
    CHECK_EQ(w.frame_gap_sum, 333u);
    CHECK_EQ(w.lock_wait_ticks, 444u);
    CHECK_EQ(w.t_writer_wait, 555u);
    // ... and the next window starts from zero, everywhere
    w = TakeStats();
    CHECK_EQ(w.frames, 0u);
    CHECK_EQ(w.pull_calls, 0u);
    CHECK_EQ(w.t_net_service, 0u);
    CHECK_EQ(w.soc_ticks[kSocPoll], 0u);
    CHECK_EQ(w.frame_gap_sum, 0u);
    CHECK_EQ(w.t_writer_wait, 0u);
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&g_stats);
    bool all_zero = true;
    for (std::size_t i = 0; i < sizeof(g_stats); ++i) {
        all_zero = all_zero && bytes[i] == 0;
    }
    CHECK(all_zero);

    // adds that cross the 32-bit boundary while windows are taken: the grand total of the windows is exactly what was added
    constexpr int kThreads = 4;
    constexpr int kAdds = 60000;
    constexpr u64 kStep = 0x00000000FFFFF123ull; // (every add carries into the high word about every 4096th time... and then some)
    std::atomic<bool> go{true};
    std::vector<std::thread> adders;
    for (int t = 0; t < kThreads; ++t) {
        adders.emplace_back([&, t] {
            for (int i = 0; i < kAdds; ++i) {
                StatsAdd64(g_stats.t_net_service, kStep);
                StatsAdd64(g_stats.soc_ticks[t % kSocKinds], kStep + static_cast<u64>(t));
            }
        });
    }
    u64 service_total = 0;
    u64 soc_total = 0;
    bool plausible = true;
    std::thread taker([&] {
        while (go.load()) {
            const Stats win = TakeStats();
            service_total += win.t_net_service;
            for (int k = 0; k < kSocKinds; ++k) {
                soc_total += win.soc_ticks[k];
            }
            // (a torn read would be off by a multiple of 2^32 from every multiple of the step: the sum is a multiple of kStep)
            plausible = plausible && win.t_net_service % kStep == 0;
        }
    });
    for (auto& th : adders) {
        th.join();
    }
    go = false;
    taker.join();
    const Stats rest = TakeStats();
    service_total += rest.t_net_service;
    for (int k = 0; k < kSocKinds; ++k) {
        soc_total += rest.soc_ticks[k];
    }
    CHECK_EQ(service_total, static_cast<u64>(kThreads) * kAdds * kStep);
    CHECK_EQ(soc_total, static_cast<u64>(kThreads) * kAdds * kStep + static_cast<u64>(kAdds) * (0 + 1 + 2 + 3));
    CHECK(plausible);
    std::memset(&g_stats, 0, sizeof(g_stats));
}

// A slow card: the file layer sleeps in every write. The time a write spends there is what the writer waits for the SD card,
// counted by the LogFiles that makes the calls - the main log's flush, the capture's write, a rotation among them - and not by
// the trail of a sleep, which has a thread of its own.
class CardFiles : public DirFiles {
public:
    void Write(void* file, const char* data, std::size_t size) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        DirFiles::Write(file, data, size);
    }
    int delay_ms = 30;
};

static void TestCardWait() {
    std::printf("the writer's wait for the SD card is counted, apart from its CPU\n");
    const fs::path dir = fs::temp_directory_path() / ("azl_card_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    g_debug_level = 1;
    {
        CardFiles files;
        LogFiles logs(&files, dir.string() + "/", platform::NowMs());
        logs.Open();
        CHECK_EQ(logs.card_wait_ticks(), 0u); // (an empty queue: nothing was written)
        logs.Append("a line");
        const u64 before = platform::NowTicks();
        logs.Flush();
        const u64 flush_ticks = platform::NowTicks() - before;
        const u64 waited = logs.card_wait_ticks();
        CHECK(azl_ticks_to_ms(waited) >= 25 && azl_ticks_to_ms(waited) <= 500);
        CHECK(waited <= flush_ticks);              // (never more than the call took)
        CHECK(flush_ticks - waited < 20 * AZL_TICKS_PER_MS); // ... and the call's own work is small
        // a flush that has nothing to write waits for nothing
        logs.Flush();
        CHECK_EQ(logs.card_wait_ticks(), waited);
        // the trail of a sleep is not the writer's wait
        logs.WriteCrumb("a breadcrumb");
        CHECK_EQ(logs.card_wait_ticks(), waited);
        // a second write adds its own
        logs.Append("another line");
        logs.Flush();
        CHECK(logs.card_wait_ticks() >= waited + 25 * AZL_TICKS_PER_MS);
        logs.Close();
    }
    g_debug_level = 0;
    fs::remove_all(dir);
}

// ---- 5. the wrap of the messages on the screen -----------------------------------------------------------------------
static void TestWrapLines() {
    std::printf("messages on the screen are wrapped, never cut\n");
    const auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& l : v) {
            s += (s.empty() ? "" : "|") + l;
        }
        return s;
    };
    CHECK_EQ(join(WrapLines("short", 58)), std::string("short"));
    CHECK_EQ(join(WrapLines("", 58)), std::string(""));
    CHECK_EQ(join(WrapLines("aaa bbb ccc", 7)), std::string("aaa bbb|ccc"));
    CHECK_EQ(join(WrapLines("aaa bbb ccc", 8)), std::string("aaa bbb|ccc"));
    CHECK_EQ(join(WrapLines("aaa bbb ccc", 11)), std::string("aaa bbb ccc"));
    CHECK_EQ(join(WrapLines("one\ntwo three", 20)), std::string("one|two three"));
    CHECK_EQ(join(WrapLines("abcdefghij", 4)), std::string("abcd|efgh|ij"));
    CHECK_EQ(join(WrapLines("ab   cd", 20)), std::string("ab cd"));
    // every text the plugin puts on the screen at start-up fits whole
    const char* const texts[] = {
        "Socket buffer reduced to 64 KB: not enough free memory for 128 KB (set debug=1 for details)",
        "No memory for the network: AzaharLink is OFF for this launch",
        "AzaharLink: no room address. Set it in the Setup app (the in-game menu is off)",
        "AzaharLink: 105 frames lost, the game is not reading the network (paused?)",
        "Could not start the network layer (0xE0A01BF5): set debug=1 for details",
    };
    for (const char* text : texts) {
        std::string rebuilt;
        for (const std::string& line : WrapLines(text, 58)) {
            CHECK(line.size() <= 58);
            rebuilt += (rebuilt.empty() ? "" : " ") + line;
        }
        CHECK_EQ(rebuilt, std::string(text)); // all of it, in order
    }
}

// ---- 6. counters that two threads write -------------------------------------------------------------------------------
static void TestSharedCounters() {
    std::printf("StatsAdd: two threads adding to one counter lose nothing\n");
    std::memset(&g_stats, 0, sizeof(g_stats));
    constexpr int kThreads = 4;
    constexpr int kEach = 200000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kEach; ++i) {
                StatsAdd(g_stats.tx_cap_drops, 1 + t % 2);
                StatsAdd(g_stats.tx_stale_drops);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK_EQ(g_stats.tx_cap_drops, static_cast<u32>(kEach * (1 + 2 + 1 + 2)));
    CHECK_EQ(g_stats.tx_stale_drops, static_cast<u32>(kEach * kThreads));
    // ... and a 64-bit time sum added to from several threads, past the 32-bit boundary
    std::memset(&g_stats, 0, sizeof(g_stats));
    threads.clear();
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < kEach; ++i) {
                StatsAdd64(g_stats.ipc_sum_ticks, 0x10001ull + 0x100000000ull);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK_EQ(g_stats.ipc_sum_ticks, static_cast<u64>(kEach) * kThreads * (0x10001ull + 0x100000000ull));
    std::memset(&g_stats, 0, sizeof(g_stats));
}

/// The end of a launch on a slow card with a full ring (the Zelda session of 2026-10-14: a full 128 GB card, ~40 KB/s, ring at
/// 465-480 KB): the capture stops writing after BeginShutdown's deadline, the main log - written first - has its last lines and
/// says how much of the capture was not written; a switch-off from the menu (no BeginShutdown) still drains everything.
static void TestTheEndOfALaunchIsBounded() {
    std::printf("the end of a launch: the capture's drain stops at its deadline, the main log's last lines are written first\n");
    const std::vector<u8> payload(1500, 7);
    const fs::path dir = MakeTempDir("exitbound");
    SlowFiles slow(60); // (each write to the card takes 60 ms: 40 writes would take 2.4 s)
    LogFileLimits limits;
    limits.exit_capture_ms = 300;
    LogFiles log(&slow, dir.string() + "/", platform::NowMs(), limits);
    CHECK(log.SetLevel(1, true));
    DebugReset();
    for (int i = 0; i < 300; ++i) { // ~900 KB of text offered: the ring (512 KB) keeps what fits, ~340 frames of text per 512 KB
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const u32 queued_before = DebugBytesQueued();
    CHECK(queued_before > 100 * 1024);
    log.Append("shutdown: begin (test)");
    log.BeginShutdown();
    const auto begin = std::chrono::steady_clock::now();
    log.Close();
    g_debug_level = 0; // (Close() leaves the level alone: the plugin's Shutdown ends the process)
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    CHECK(spent >= 200 && spent < 700); // (the deadline, 300 ms, plus the write in hand: not the seconds a full drain takes)
    CHECK(DebugBytesQueued() > 0);      // (the rest stays in the ring: it is dropped with the process)
    // the main log: its last line came before the capture's drain, and a line says what was not written
    std::string main_log;
    {
        std::FILE* f = std::fopen((dir / "AzaharLink.log").string().c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                main_log.append(buf, n);
            }
            std::fclose(f);
        }
    }
    CHECK(main_log.find("shutdown: begin (test)") != std::string::npos);
    const std::size_t cut_at = main_log.find("shutdown: the capture was cut at the end: ");
    CHECK(cut_at != std::string::npos);
    CHECK(main_log.find(" KB of it were not written (the card was too slow to take them in 300 ms)") != std::string::npos);
    CHECK(main_log.find("shutdown: begin (test)") < cut_at);
    fs::remove_all(dir);

    // a switch-off from the menu has no deadline: everything is written, however slow the card
    const fs::path dir2 = MakeTempDir("exitbound2");
    SlowFiles slow2(20);
    LogFiles menu_log(&slow2, dir2.string() + "/", platform::NowMs(), limits);
    CHECK(menu_log.SetLevel(1, true));
    DebugReset();
    for (int i = 0; i < 100; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    CHECK(menu_log.SetLevel(0, false));
    CHECK_EQ(DebugBytesQueued(), 0u);
    fs::remove_all(dir2);

    // FlushWithin: the lines wait a little for a file lock held by a long write, then go back to the queue in order
    const fs::path dir3 = MakeTempDir("exitbound3");
    GatedFiles gated;
    LogFiles flush_log(&gated, dir3.string() + "/", platform::NowMs());
    CHECK(flush_log.SetLevel(1, false));
    flush_log.Append("first line");
    CHECK(flush_log.FlushWithin(100)); // (the files are free: written at once)
    gated.gate_closed.store(true);
    flush_log.Append("held line");
    std::thread writer([&] { flush_log.Flush(); }); // (the log writer, stuck in a long write to the card with the file lock)
    while (gated.blocked.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    flush_log.Append("late line");
    const auto wait_begin = std::chrono::steady_clock::now();
    CHECK(!flush_log.FlushWithin(120)); // it waits its 120 ms for the lock and gives up
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_begin).count();
    CHECK(waited >= 100 && waited < 400);
    gated.gate_closed.store(false);
    writer.join();
    CHECK(flush_log.FlushWithin(1000)); // the line given back is written now
    flush_log.Close();
    g_debug_level = 0;
    std::string text;
    if (std::FILE* f = std::fopen((dir3 / "AzaharLink.log").string().c_str(), "rb")) {
        char buf[2048];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        std::fclose(f);
    }
    const std::size_t a = text.find("first line"), b = text.find("held line"), c = text.find("late line");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos);
    CHECK(a < b && b < c); // (nothing lost, in order)
    fs::remove_all(dir3);
}

int main() {
    // (nothing may have turned the log on or allocated its ring before the test of level 0)
    TestLevelZeroIsFree();
    TestIniValues();
    TestNoFileAtLevelZero();
    TestRotationWhileSwitchedOff();
    TestCrumbFile();
    TestSwitching();
    TestQueueHasItsRoomFromTheStart();
    TestSwitchOffWhileTheWriterRuns();
    TestRotation();
    TestCardNotReadyYet();
    TestCaptureFileHeader();
    TestDrainIsBounded();
    TestFlushes();
    TestNobodyWaitsForTheWriter();
    TestTheEndOfALaunchIsBounded();
    TestCaptureFileHasTheDietAndTheMarkers();
    TestStatusNumbers();
    TestCpuNumbers();
    TestTakeStats();
    TestCardWait();
    TestWrapLines();
    TestSharedCounters();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
