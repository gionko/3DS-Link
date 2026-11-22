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

#include "azl/game_card_calls.hpp"
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

    // The whole file: 18 keys in the fixed order, LF, and the lines the writer keeps after them
    {
        const std::string text = ini::Render(ini::Fields());
        CHECK_EQ(text,
                 std::string("enabled=1\nhost=\nport=24872\nnickname=\npassword=\ntunnel=1\nautoconnect=1\nradio_like=1\n"
                             "socket_buffers=1\nnet_thread_core=-1\nmenu_key=off\ngame_name=\ndebug=0\nrcvbuf_kb=64\n"
                             "sleep_stop=0\nchat=0\nchat_key=L+Left+Select\nchat_pos=bottom-left\n")); // (the chat off in a new file: 2026-11-19)
        CHECK_EQ(text.size(), 228u);
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
        f.chat_pos = ini::kChatPosTopRight;
        const std::string two = ini::Render(f, {"# note", "future=7"});
        CHECK(two.find("host=a b\n") != std::string::npos); // a stray line break would corrupt the file
        CHECK(two.find("rcvbuf_kb=32\nsleep_stop=2\nchat=0\nchat_key=L+Left+Select\nchat_pos=top-right\n# note\nfuture=7\n") != std::string::npos);
        // what does not fit under the limit is left out, whole lines only
        const std::string small = ini::Render(ini::Fields(), {"aaaaaaaaaa", "bbbbbbbbbb"}, text.size() + 12);
        CHECK(small.find("aaaaaaaaaa\n") != std::string::npos && small.find("bbbbbbbbbb") == std::string::npos);
    }
    // chat_pos: the four words both ways, a slip is the default (bottom-left), a field out of the table is written as the default
    CHECK(ini::kChatPosDefault == ini::kChatPosBottomLeft && ini::kKeyCount == 18);
    CHECK(std::string(ini::kKeyNames[ini::kKeyCount - 1]) == "chat_pos");
    for (int v = 0; v < ini::kChatPosCount; ++v) {
        CHECK(ini::ParseChatPos(ini::ChatPosWord(v)) == v);
        CHECK(ini::NextChatPos(ini::NextChatPos(v, 0), -1) == v);
    }
    CHECK(ini::ParseChatPos(" TOP-LEFT\r") == ini::kChatPosTopLeft && ini::ParseChatPos("") == ini::kChatPosBottomLeft);
    CHECK(ini::ParseChatPos("3") == ini::kChatPosBottomLeft && ini::ParseChatPos("top left") == ini::kChatPosBottomLeft);
    CHECK(std::string(ini::ChatPosWord(7)) == "bottom-left" && std::string(ini::ChatPosLabel(-3)) == "Bottom left");
    CHECK(std::string(ini::ChatPosLabel(ini::kChatPosBottomRight)) == "Bottom right");
    {
        ini::Fields odd;
        odd.chat_pos = 12;
        CHECK(ini::Render(odd).find("chat_pos=bottom-left\n") != std::string::npos);
    }
    CHECK(std::string(ini::kMenuKeyOff) == "off");
    CHECK(std::string(ini::kDefaultMenuKey) == "L+Right+Select");
    // the old default (any case, blanks around) becomes the new one; a chord the user wrote, and off, stay as they are
    CHECK(ini::MenuKeyFromFile("L+R+Select") == "L+Right+Select" && ini::MenuKeyFromFile(" l+r+select\r") == "L+Right+Select");
    CHECK(ini::MenuKeyFromFile("L+R+Start") == "L+R+Start" && ini::MenuKeyFromFile("off") == "off" && ini::MenuKeyFromFile("L+Right+Select") == "L+Right+Select");
    // (DPAPP-2j, the user: chat, debug and menu off by default) a value with no key the plugin knows besides Select is OFF - the plugin's ParseKeys gave
    // 0 for it and opened the menu on Select | the default chord; "none" stays itself (off), a real chord with Select stays
    CHECK(ini::MenuKeyFromFile("xyz") == "off" && ini::MenuKeyFromFile("Select") == "off" && ini::MenuKeyFromFile(" select \r") == "off" &&
          ini::MenuKeyFromFile("+,") == "off" && ini::MenuKeyFromFile("none") == "none" && ini::MenuKeyFromFile("L+Select") == "L+Select" &&
          ini::MenuKeyFromFile("Start") == "Start");
    CHECK(ini::MenuKeyIsOff(ini::MenuKeyFromFile("xyz")) && !ini::MenuKeyIsOff(ini::MenuKeyFromFile("x+L")));
    CHECK(ini::MenuKeyIsOff(ini::kMenuKeyOff));
    CHECK(!ini::MenuKeyIsOff(ini::kDefaultMenuKey));
    // the menu's chord and the chat's (review of the chat, defect 47): Select always in, the words of ParseKeys in any case, and a pair
    // where one holds the other clashes, not only the same chord (L+Select for the menu took the chat's L+Left+Select)
    CHECK(ini::ChordBits("") == 1u && ini::ChordBits("off") == 1u && ini::ChordBits("Select") == 1u);
    CHECK(ini::ChordBits("L+R") == ini::ChordBits("r, l+SELECT") && ini::ChordBits("L+R") != ini::ChordBits("L"));
    CHECK(ini::ChordBits("Start+A+B+X+Y+L+R+Up+Down+Left+Right+Select") == 0xFFFu);
    CHECK(!ini::ChordsNested(ini::kDefaultMenuKey, ini::kDefaultChatKey) && !ini::ChordsNested(ini::kOldDefaultMenuKey, ini::kDefaultChatKey));
    CHECK(ini::ChordsNested("L+Left+Select", "l+left") && ini::ChordsNested("L+Select", ini::kDefaultChatKey));
    CHECK(ini::ChordsNested(ini::kDefaultChatKey, "L+Select") && ini::ChordsNested("L+Left+Select+A", ini::kDefaultChatKey));
    CHECK(ini::ChordsNested("Left", ini::kDefaultChatKey) && !ini::ChordsNested("R+Select", ini::kDefaultChatKey));
    CHECK(!ini::ChordsNested("L+Up+Select", "L+Down+Select") && ini::ChordsNested("Select", "X+Select"));
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
    // a launch that finds the trail of earlier ones over 128 KB moves it aside as AzaharLink.sleep.1.log (the one before it goes) and
    // starts a new one: the launch before a crash keeps its trail; a smaller one is appended to
    {
        DirFiles files;
        const auto write_file = [](const fs::path& p, const std::string& text) {
            FILE* f = std::fopen(p.string().c_str(), "wb");
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        };
        const std::string big = std::string(200 * 1024 - 18, 'x') + "the launch before\n";
        write_file(dir / "AzaharLink.sleep.log", big);
        write_file(dir / "AzaharLink.sleep.1.log", "an older trail\n");
        {
            LogFiles log(&files, dir.string() + "/", platform::NowMs());
            CHECK(log.SetLevel(1, false));
            log.WriteCrumb("sleep: a new launch");
            CHECK(ReadAll(dir / "AzaharLink.sleep.log") == "sleep: a new launch\n");
            CHECK(ReadAll(dir / "AzaharLink.sleep.1.log") == big); // (moved whole: the older one went)
            CHECK(log.SetLevel(0, false));
        }
        {
            // a smaller file is appended to, nothing moved; the lines of a turn go in one write and one flush
            LogFiles log(&files, dir.string() + "/", platform::NowMs());
            CHECK(log.SetLevel(1, false));
            const std::string turn[3] = {"sleep: one", "sleep: two", "sleep: three"};
            const int writes = files.writes, flushes = files.flushes;
            log.WriteCrumbs(turn, 3);
            CHECK_EQ(files.writes - writes, 1);
            CHECK_EQ(files.flushes - flushes, 1);
            log.WriteCrumbs(turn, 0); // (a turn with no line writes nothing)
            CHECK_EQ(files.writes - writes, 1);
            CHECK(ReadAll(dir / "AzaharLink.sleep.log") == "sleep: a new launch\nsleep: one\nsleep: two\nsleep: three\n");
            CHECK(ReadAll(dir / "AzaharLink.sleep.1.log") == big);
            CHECK(log.SetLevel(0, false));
        }
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

// The queue is full (the card did not keep up): the lines past max_pending are dropped, counted, and the next flush says how many; the
// stats line reads the total since the start.
static void TestQueueFullIsCounted() {
    std::printf("the main log's queue full: the lines dropped are counted, said by the next flush and kept in a total\n");
    const fs::path dir = MakeTempDir("full");
    DirFiles files;
    LogFileLimits limits;
    limits.max_pending = 2 * 1024;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    g_debug_level = 0;
    CHECK(log.SetLevel(1, false));
    const std::string line(100, 'f');
    // (113 bytes a line with its stamp "[    0.xxx] ": a line goes in while the queue holds less than 2 KB, 19 of them, 21 are dropped)
    for (int i = 0; i < 40; ++i) {
        log.Append(line.c_str());
    }
    CHECK_EQ(log.queue_full_total(), 0u); // (counted by the writer's next flush)
    log.Flush();
    CHECK_EQ(log.queue_full_total(), 21u);
    std::string text = ReadAll(dir / "AzaharLink.log");
    // (the report goes in past the full queue: said at once, not dropped and counted as one more)
    CHECK(Contains(text, "log: 21 lines were dropped: the queue of the main log was full (2 KB waiting for the card)"));
    std::size_t kept = 0;
    for (std::size_t at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) {
        ++kept;
    }
    CHECK_EQ(kept, std::size_t{19});
    log.Append("one more");
    log.Flush();
    CHECK_EQ(log.queue_full_total(), 21u); // (nothing dropped since: the total stays, no new report)
    text = ReadAll(dir / "AzaharLink.log");
    CHECK(Contains(text, "one more") && !Contains(text, "log: 1 lines were dropped"));
    CHECK(log.SetLevel(0, false));
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
    std::printf("a pass of the drain that is not the last is ONE write of 64 KB and one flush, and says when it left records\n");
    const fs::path dir = MakeTempDir("bounds");
    DirFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    CHECK(log.SetLevel(1, true));
    DebugReset();
    const std::vector<u8> payload(1500, 7);
    // 250 frames of 1500 bytes: 760 KB of text, twelve writes of 64 KB
    for (int i = 0; i < 250; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    CHECK_EQ(DebugCaptureTotals().lost_full[1], 0u);
    const u64 before = fs::file_size(dir / "AzaharLink.debug.log");
    const int writes_before = files.writes, flushes_before = files.flushes;
    CHECK(log.DrainDebug(false)); // one write, records left
    const u64 pass = fs::file_size(dir / "AzaharLink.debug.log") - before;
    CHECK(pass >= 64 * 1024 && pass <= 64 * 1024 + 4 * 1024); // (64 KB and the record that crossed it)
    CHECK_EQ(files.writes - writes_before, 1);                // ONE call the game's read can be behind, not two and more
    CHECK_EQ(files.flushes - flushes_before, 1);
    CHECK(DebugBytesQueued() > 0);
    int passes = 1;
    while (log.DrainDebug(false)) {
        ++passes;
        CHECK(passes < 16);
    }
    ++passes; // (the one that found the end of the ring)
    std::printf("  760 KB of text in %d passes, %d writes, %d flushes\n", passes, files.writes - writes_before, files.flushes - flushes_before);
    CHECK(passes >= 11 && passes <= 13);
    CHECK_EQ(files.writes - writes_before, passes); // a write a pass
    CHECK_EQ(files.flushes - flushes_before, passes); // ... and a flush
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(!log.DrainDebug(false)); // nothing left: no reason to come back soon
    // the end of the launch takes everything in one pass, write after write, flushed once
    for (int i = 0; i < 250; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const int writes_final = files.writes, flushes_final = files.flushes;
    CHECK(!log.DrainDebug(true));
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(files.writes - writes_final >= 11 && files.writes - writes_final <= 13);
    CHECK_EQ(files.flushes - flushes_final, 1);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);

    // a slow card: the pass is still one write (60 ms here), not write after write for as long as the bound of a pass allowed
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
    CHECK(spent >= 50 && spent < 200); // (one write)
    CHECK(fs::file_size(dir2 / "AzaharLink.debug.log") < 64 * 1024 + 8 * 1024);
    CHECK(DebugBytesQueued() > 0);
    CHECK(slow_log.SetLevel(0, false)); // (the final drain empties the ring)
    CHECK_EQ(DebugBytesQueued(), 0u);
    fs::remove_all(dir2);
}

// The card is the game's too: the main log is flushed at every write (its last lines are what a post-mortem needs), the
// capture once a pass of the writer (2026-10-20), and a pass that is not the last is one write (2026-11-10).
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
    for (int i = 0; i < 60; ++i) { // ~180 KB of text: three passes, a write of 64 KB and a flush each
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const int writes1 = files.writes, flushes1 = files.flushes;
    CHECK(log.DrainDebug(false));
    CHECK_EQ(files.writes - writes1, 1);
    CHECK_EQ(files.flushes - flushes1, 1);
    CHECK(log.DrainDebug(false));
    CHECK(!log.DrainDebug(false));
    CHECK_EQ(files.writes - writes1, 3);
    CHECK_EQ(files.flushes - flushes1, 3);
    // the hex of a frame, formatted in one go, is the same text as before: 1500 bytes of 0x07 after the header
    const std::string text = ReadAll(dir / "AzaharLink.debug.log");
    std::string hex;
    for (int i = 0; i < 1500; ++i) {
        hex += "07";
    }
    CHECK(Contains(text, (hex + "\n").c_str()));
    // nothing to write: nothing flushed
    CHECK(!log.DrainDebug(false));
    CHECK_EQ(files.flushes - flushes1, 3);
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// The capture goes to the card in few, large passes (LogFileLimits::capture_every_ms): a pass is due when the time has gone by, when the ring
// holds capture_ring_high bytes, or when something asks at once (the HOME button); a pass notes itself and takes the ring back to nothing.
static void TestCaptureCadence() {
    std::printf("the capture is written every capture_every_ms, or when the ring is high, or at once on request\n");
    const fs::path dir = MakeTempDir("cadence");
    DirFiles files;
    LogFileLimits limits;
    limits.capture_every_ms = 2000;
    limits.capture_ring_high = 160 * 1024;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    DebugReset();
    // no pass yet: the first is due at once
    CHECK(log.CaptureDue(10000, 0, false));
    log.NoteCapturePass(10000);
    CHECK(!log.CaptureDue(10000, 0, false));
    CHECK(!log.CaptureDue(11999, 0, false)); // 1999 ms later: not yet
    CHECK(log.CaptureDue(12000, 0, false));  // 2000 ms later
    // a ring that holds 160 KB is not left for the rest of the interval; one that holds a little less is
    CHECK(!log.CaptureDue(10500, 160 * 1024 - 1, false));
    CHECK(log.CaptureDue(10500, 160 * 1024, false));
    // a request is at once
    CHECK(log.CaptureDue(10500, 0, true));
    // the ring's fill is readable, and a pass takes it back to nothing and notes itself
    const std::vector<u8> payload(1500, 7);
    for (int i = 0; i < 10; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    CHECK(DebugRingUsedBytes() >= 10 * 1500);
    const int flushes0 = files.flushes;
    CHECK(!log.DrainDebug(false));
    CHECK_EQ(files.flushes - flushes0, 1);
    CHECK_EQ(DebugRingUsedBytes(), 0u);
    CHECK(!log.CaptureDue(platform::NowMs(), 0, false)); // (it noted itself: the next is due in capture_every_ms)
    CHECK(log.CaptureDue(platform::NowMs() + 2000, 0, false));
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

// ---- the capture leaves the card to the game (2026-11-10) -------------------------------------------------------------
// The console has one file-system service: a file read of the game waits behind the call the writer has in it. The pieces below are
// what keeps the writer out of the game's way with the log on: one write a pass, a slice that shrinks on a slow card, a rest as long
// as the pass kept the card, the game's reads first, the beacons that say nothing new counted, the core given up while formatting,
// the main log before the capture - and the numbers that say, call by call, where the card's time went.

/// A card whose calls can be made slow one kind at a time, and which remembers the order of the calls: M / m a write / a flush of the
/// main log, C / c of the capture.
class PaceFiles : public DirFiles {
public:
    void* OpenAppend(const std::string& path) override {
        void* file = DirFiles::OpenAppend(path);
        if (file && path.size() >= 14 && path.compare(path.size() - 14, 14, "AzaharLink.log") == 0) {
            main_log = file;
        }
        if (file && path.size() >= 20 && path.compare(path.size() - 20, 20, "AzaharLink.debug.log") == 0) {
            capture = file;
        }
        return file;
    }
    void Write(void* file, const char* data, std::size_t size) override {
        if (file == capture) {
            trace += 'C';
            last_capture_write = size;
            if (on_capture_write) {
                on_capture_write();
            }
            if (capture_write_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(capture_write_ms));
            }
        } else if (file == main_log) {
            trace += 'M';
        }
        DirFiles::Write(file, data, size);
    }
    void Flush(void* file) override {
        if (file == capture) {
            trace += 'c';
            if (capture_flush_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(capture_flush_ms));
            }
        } else if (file == main_log) {
            trace += 'm';
        }
        DirFiles::Flush(file);
    }
    std::string trace;
    void* main_log = nullptr;
    void* capture = nullptr;
    int capture_write_ms = 0;
    int capture_flush_ms = 0;
    std::size_t last_capture_write = 0;
    void (*on_capture_write)() = nullptr;
};

static void FillFrames(int count) {
    static const std::vector<u8> payload(1500, 7);
    for (int i = 0; i < count; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
}

static void TestCapturePace() {
    std::printf("the pace of the capture: the rest after a pass, the share of the card, the size of the slice, the game's reads\n");
    using namespace pace;
    // the rest: as long as the pass kept the card, 20 ms at least, 5 s at most (250 ms is no longer the ceiling)
    CHECK_EQ(RestAfterPass(0), 20u);
    CHECK_EQ(RestAfterPass(19), 20u);
    CHECK_EQ(RestAfterPass(21), 21u);
    CHECK_EQ(RestAfterPass(250), 250u);
    CHECK_EQ(RestAfterPass(251), 251u);
    CHECK_EQ(RestAfterPass(4800), 4800u);
    CHECK_EQ(RestAfterPass(5000), 5000u);
    CHECK_EQ(RestAfterPass(9300), 5000u);
    CHECK_EQ(RestAfterPass(0xFFFFFFFFFFull), 5000u);
    // the share of the card: over the real length of the window, never more than 100
    CHECK_EQ(CardBusyPercent(2500, 5000), 50u);
    CHECK_EQ(CardBusyPercent(2499, 5000), 49u);
    CHECK_EQ(CardBusyPercent(0, 5000), 0u);
    CHECK_EQ(CardBusyPercent(9300, 9600), 96u);   // (console 63, Kirby Battle Royale: the notice said 185%, by a fixed 5 s)
    CHECK_EQ(CardBusyPercent(43400, 43700), 99u); // (Fantasy Life: "868%")
    CHECK_EQ(CardBusyPercent(2880, 9600), 30u);   // (30% of a long window is not "58%": no notice)
    CHECK_EQ(CardBusyPercent(9300, 5000), 100u);  // (the ceiling)
    CHECK_EQ(CardBusyPercent(1234, 0), 0u);       // (no window: no division)
    // the slice: 64 KB while the card answers, 8 KB after a slow call, 64 KB again after 8 fast ones in a row
    SliceSize slice;
    CHECK_EQ(slice.Bytes(), 64u * 1024);
    slice.NoteCall(250); // (250 ms is not "longer than 250")
    CHECK_EQ(slice.Bytes(), 64u * 1024);
    slice.NoteCall(251);
    CHECK_EQ(slice.Bytes(), 8u * 1024);
    for (int i = 0; i < 7; ++i) {
        slice.NoteCall(10);
        CHECK_EQ(slice.Bytes(), 8u * 1024);
    }
    slice.NoteCall(4810); // (a slow one before the eighth: the count starts again)
    for (int i = 0; i < 7; ++i) {
        slice.NoteCall(10);
        CHECK_EQ(slice.Bytes(), 8u * 1024);
    }
    slice.NoteCall(10);
    CHECK_EQ(slice.Bytes(), 64u * 1024);
    for (int i = 0; i < 20; ++i) { // (fast calls on a card that answers change nothing)
        slice.NoteCall(10);
    }
    CHECK_EQ(slice.Bytes(), 64u * 1024);

    // the game's reads
    typedef GameReadWatch W;
    {
        W w;
        CHECK_EQ(int(w.Look(0, 0, 1000, true)), int(W::kPass)); // (the game reads nothing: the card is the capture's)
        CHECK_EQ(int(w.Look(1, 0, 1050, true)), int(W::kSkip)); // a read under way
        CHECK_EQ(int(w.Look(1, 1, 1100, true)), int(W::kSkip)); // ... which ended since the last look
        CHECK_EQ(int(w.Look(1, 1, 1150, true)), int(W::kPass)); // quiet since
        // a look that wants nothing never skips, and keeps the count fresh: what ended then is not "since the last look" later
        CHECK_EQ(int(w.Look(2, 2, 1200, false)), int(W::kPass));
        CHECK_EQ(int(w.Look(2, 2, 1250, true)), int(W::kPass));
        CHECK_EQ(w.reads, 2u);
        // a read that begins and ends between two looks is seen all the same
        CHECK_EQ(int(w.Look(3, 3, 1300, true)), int(W::kSkip));
        CHECK_EQ(int(w.Look(3, 3, 1350, true)), int(W::kPass));
    }
    {
        // a game that reads without a pause: 10 s of looks that step aside, then ONE forced slice, and the count starts again
        W w;
        u32 n = 0;
        int skips = 0;
        u64 t = 5000;
        for (; t < 5000 + 10000; t += 50) {
            ++n;
            CHECK_EQ(int(w.Look(n, n, t, true)), int(t == 5000 ? W::kPass : W::kSkip)); // (the first look has nothing to compare with)
            ++skips;
        }
        ++n;
        CHECK_EQ(int(w.Look(n, n, t, true)), int(W::kSkip)); // (9950 ms after the first skip: not yet)
        t += 50;
        ++n;
        CHECK_EQ(int(w.Look(n, n, t, true)), int(W::kForced));
        for (int i = 0; i < 199; ++i) {
            t += 50;
            ++n;
            CHECK_EQ(int(w.Look(n, n, t, true)), int(W::kSkip));
        }
        t += 100;
        ++n;
        CHECK_EQ(int(w.Look(n, n, t, true)), int(W::kForced)); // ... every 10 s, not once
        // a pass in between ends the streak: 9 s of skips, a quiet look, 9 s more - no forced slice
        W v;
        u32 m = 1;
        (void)v.Look(m, m, 0, true);
        for (u64 at = 50; at <= 9000; at += 50) {
            ++m;
            CHECK_EQ(int(v.Look(m, m, at, true)), int(W::kSkip));
        }
        CHECK_EQ(int(v.Look(m, m, 9050, true)), int(W::kPass));
        for (u64 at = 9100; at <= 18000; at += 50) {
            ++m;
            CHECK_EQ(int(v.Look(m, m, at, true)), int(W::kSkip));
        }
        // PassedAnyway (the HOME button) ends it too
        v.PassedAnyway();
        ++m;
        CHECK_EQ(int(v.Look(m, m, 18050, true)), int(W::kSkip));
        ++m;
        CHECK_EQ(int(v.Look(m, m, 27000, true)), int(W::kSkip)); // (8950 ms into the new streak, not 18 s into the old)
    }
    {
        // a read whose end is never counted (the post of the call skipped by an Old 3DS's swap): 3 s of "under way", then put right
        W w;
        (void)w.Look(4, 4, 0, true);
        for (u64 at = 50; at < 3050; at += 50) {
            CHECK_EQ(int(w.Look(5, 4, at, true)), int(W::kSkip));
        }
        CHECK_EQ(int(w.Look(5, 4, 3050, true)), int(W::kPass)); // (under way and still for 3 s: it is not coming)
        CHECK_EQ(w.lost_posts, 1u);
        CHECK_EQ(w.realigned, 1u);
        // the reads after it count as before
        CHECK_EQ(int(w.Look(6, 4, 3100, true)), int(W::kSkip)); // one really under way
        CHECK_EQ(int(w.Look(6, 5, 3150, true)), int(W::kSkip)); // ended since
        CHECK_EQ(int(w.Look(6, 5, 3200, true)), int(W::kPass));
        // ... and if the end thought lost comes after all (a read that really lasted that long), the count goes back
        CHECK_EQ(int(w.Look(6, 6, 3250, true)), int(W::kSkip));
        CHECK_EQ(w.lost_posts, 0u);
        CHECK_EQ(int(w.Look(6, 6, 3300, true)), int(W::kPass));
        CHECK_EQ(int(w.Look(7, 6, 3350, true)), int(W::kSkip));
    }
    {
        // a lost end with reads going on around it (music streamed from the card every 400 ms): the count would stay one too many
        // for good. The gate skipped a post (the swap of an Old 3DS): the fewest open over 3 s is what never ended
        W w;
        (void)w.Look(0, 0, 0, true, 7); // (7 posts skipped before the first look: old news)
        u32 begun = 1, done = 0;        // (the read whose end is lost: the gate's count goes to 8)
        int passes = 0;
        u64 at = 50;
        for (; at <= 8000; at += 50) {
            if (at % 400 == 0) {
                ++begun; // a read under way at this look ...
            } else if (at % 400 == 50 && at > 50) {
                ++done; // ... over at the next
            }
            if (w.Look(begun, done, at, true, 8) == W::kPass) {
                ++passes;
            }
            if (at < 3050) {
                CHECK_EQ(w.realigned, 0u); // (3 s from the look that saw the skipped post, not before)
            }
        }
        CHECK_EQ(w.lost_posts, 1u);
        CHECK_EQ(w.realigned, 1u);
        CHECK(passes > 50); // (from 3 s on, the six quiet looks of every eight pass: without the correction none would)
        // one skipped post puts the count right ONCE: the game then reads on end for 10 s (a read under way at every look, around
        // the lost one) and nothing more is taken for lost - no pass in the middle of its reads
        passes = 0;
        for (; at <= 18000; at += 50) {
            if (at % 200 == 0) {
                ++begun;
                ++done; // (one ends and the next begins between two looks)
            }
            if (w.Look(begun + 1, done, at, true, 8) == W::kPass) {
                ++passes;
            }
        }
        CHECK_EQ(w.lost_posts, 1u);
        CHECK_EQ(w.realigned, 1u);
        CHECK_EQ(passes, 0);
    }
    {
        // ... and when no look finds the count right in between: the lost end alone for 3 s (the plain rule puts it right), and from
        // the very next look a load, a read under way at every look for 10 s. The one skipped post was accounted for: no second time
        W w;
        (void)w.Look(0, 0, 0, true, 0);
        for (u64 at = 50; at <= 3050; at += 50) {
            (void)w.Look(1, 0, at, true, 1);
        }
        CHECK_EQ(w.lost_posts, 1u);
        CHECK_EQ(w.realigned, 1u);
        int passes = 0;
        for (u64 at = 3100; at <= 13000; at += 50) {
            const u32 n = static_cast<u32>(at / 200);
            passes += w.Look(n + 2, n, at, true, 1) == W::kPass;
        }
        CHECK_EQ(w.lost_posts, 1u);
        CHECK_EQ(w.realigned, 1u);
        CHECK_EQ(passes, 0);
    }
    {
        // 2026-11-11, the review's case: NO post skipped (a New 3DS never skips one) and a game that reads on end, a read under way
        // at every look - reads of 200 ms one after the other, for a minute. Nothing was lost: the count stays, and the capture never
        // passes (10 s of stepping aside, one forced slice, and again). The rule of 2026-11-10 took the read for lost after 3 s and
        // wrote in the middle of the load.
        for (const u32 read_ms : {200u, 400u, 5u}) {
            W w;
            (void)w.Look(0, 0, 0, true);
            int passes = 0, forced = 0;
            for (u64 at = 50; at <= 60000; at += 50) {
                const u32 n = static_cast<u32>(at / read_ms);
                const W::Verdict v = w.Look(n + 1, n, at, true);
                passes += v == W::kPass;
                forced += v == W::kForced;
            }
            CHECK_EQ(passes, 0);
            CHECK_EQ(forced, 5); // (at 10.05 s, 20.1 s, ...)
            CHECK_EQ(w.lost_posts, 0u);
            CHECK_EQ(w.realigned, 0u);
        }
        // two threads of the game reading at once (300 ms and 120 ms): the same
        {
            W w;
            (void)w.Look(0, 0, 0, true);
            int passes = 0;
            for (u64 at = 50; at <= 60000; at += 50) {
                const u32 n = static_cast<u32>(at / 300 + at / 120);
                passes += w.Look(n + 2, n, at, true) == W::kPass;
            }
            CHECK_EQ(passes, 0);
            CHECK_EQ(w.lost_posts, 0u);
            CHECK_EQ(w.realigned, 0u);
        }
        // a post skipped that was seen while no read was under way lost no end of a read: it proves nothing about the reads after it
        {
            W w;
            (void)w.Look(0, 0, 0, true, 0);
            CHECK_EQ(int(w.Look(0, 0, 50, true, 3)), int(W::kPass)); // (three calls of the game came back during a swap; none a read)
            int passes = 0;
            for (u64 at = 100; at <= 20000; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                passes += w.Look(n + 1, n, at, true, 3) == W::kPass;
            }
            CHECK_EQ(passes, 0);
            CHECK_EQ(w.realigned, 0u);
        }
        // the last resort, an end lost with no skipped post to show for it: reads under way at every look for five minutes
        {
            W w;
            (void)w.Look(0, 0, 0, true);
            u64 at = 50;
            for (; at < 50 + kReadLostMs; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)w.Look(n + 2, n, at, true); // (one that never ends, and reads of 200 ms around it)
            }
            CHECK_EQ(w.realigned, 0u);
            const u32 n = static_cast<u32>(at / 200);
            (void)w.Look(n + 2, n, at, true);
            CHECK_EQ(w.realigned, 1u);
            CHECK_EQ(w.lost_posts, 2u); // (the fewest seen open: at no look was the second thread between two reads)
            // ... and what was not lost after all comes back by itself at the first look that sees fewer open
            CHECK_EQ(int(w.Look(n + 2, n + 1, at + 50, true)), int(W::kSkip));
            CHECK_EQ(w.lost_posts, 1u);
        }
    }
    {
        // 2026-11-11, the second review. (1) The posts the gate skipped BEFORE the first look are no proof (the capture switched on
        // from the menu in a game that went through a swap already): the first look finds a read under way and the game reads on end
        // for 10 s - nothing is taken for lost
        {
            W w;
            CHECK_EQ(int(w.Look(1, 0, 0, true, 7)), int(W::kSkip));
            int passes = 0;
            for (u64 at = 50; at <= 10000; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                passes += w.Look(n + 1, n, at, true, 7) == W::kPass;
            }
            CHECK_EQ(passes, 0);
            CHECK_EQ(w.lost_posts, 0u);
            CHECK_EQ(w.realigned, 0u);
        }
        // (2) a game that reads on end for 10 s, nothing lost; then a swap during which TWO reads lose their end (two threads at the
        // card) and the load goes on. The 3 s run from the look that SEES the skipped posts, and the fewest open is counted from that
        // look - not from the 10 s before it, when one read was under way at every look
        {
            W w;
            (void)w.Look(0, 0, 0, true, 0);
            u64 at = 50;
            for (; at <= 10000; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)w.Look(n + 1, n, at, true, 0);
            }
            CHECK_EQ(w.realigned, 0u);
            const u64 seen = at; // the first look after the swap: two posts skipped, two ends lost, and the load goes on
            for (; at < seen + kReadStuckMs; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)w.Look(n + 3, n, at, true, 2);
                CHECK_EQ(w.realigned, 0u); // (not before 3 s from that look, although reads were under way for 10 s before it)
            }
            const u32 n = static_cast<u32>(at / 200);
            (void)w.Look(n + 3, n, at, true, 2);
            CHECK_EQ(w.realigned, 1u);
            CHECK_EQ(w.lost_posts, 2u); // (the two that were lost: by the fewest open of BEFORE the swap it was one, and the other stayed)
            int passes = 0;
            for (at += 50; at <= seen + 13000; at += 50) { // the load goes on: the game is at the card at every look
                const u32 m = static_cast<u32>(at / 200);
                passes += w.Look(m + 3, m, at, true, 2) == W::kPass;
            }
            CHECK_EQ(passes, 0);
            CHECK_EQ(w.lost_posts, 2u);
            CHECK_EQ(w.realigned, 1u);
        }
        // (3) the review's case: the HOME button during a load. The read under way loses its end (ONE post skipped) and the load goes
        // on at once: reads of 200 ms, one under way at every look, so the fewest seen open in 3 s is two - the lost end and the real
        // read of the moment. One post was skipped: one end is lost, no more, and the capture stays off the card for the rest of the
        // load (with two taken for lost, three looks of every four passed, and the capture wrote in the middle of the load)
        {
            W w;
            (void)w.Look(0, 0, 0, true, 0);
            CHECK_EQ(int(w.Look(1, 0, 50, true, 0)), int(W::kSkip)); // the read the swap interrupts
            int passes = 0;
            for (u64 at = 100; at <= 15000; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                passes += w.Look(n + 2, n, at, true, 1) == W::kPass;
                CHECK_EQ(w.realigned, at < 100 + kReadStuckMs ? 0u : 1u);
            }
            CHECK_EQ(w.lost_posts, 1u);
            CHECK_EQ(passes, 0);
        }
        // (4) "at every look" is no proof across a gap: a read under way at the last look before the HOME menu (or the lid) and one
        // at the first look after it, six minutes later, are two looks - not five minutes of reads on end
        {
            W w;
            (void)w.Look(0, 0, 0, true);
            u64 at = 50;
            for (; at <= 5000; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)w.Look(n + 1, n, at, true);
            }
            at += 360000; // (nobody looked: the writer was stopped with the console)
            for (const u64 end = at + 10000; at <= end; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)w.Look(n + 1, n, at, true);
            }
            CHECK_EQ(w.realigned, 0u);
            CHECK_EQ(w.lost_posts, 0u);
            // ... and the last resort still comes when the looks have gaps (a forced slice that keeps the writer in a slow card for
            // 1.5 s, every 10 s): after five minutes of the time that WAS looked at, the gaps left out
            W v;
            (void)v.Look(0, 0, 0, true);
            u64 t = 0, looked_ms = 0;
            u32 looks = 0;
            while (v.realigned == 0 && t < 2 * u64(kReadLostMs)) {
                const bool gap = ++looks % 200 == 0;
                t += gap ? 1500 : 50;
                looked_ms += gap ? 0 : 50;
                const u32 n = static_cast<u32>(t / 200);
                (void)v.Look(n + 2, n, t, true); // (one that never ends, and reads of 200 ms around it)
            }
            CHECK_EQ(v.realigned, 1u);
            CHECK_EQ(looked_ms, u64(kReadLostMs) + 50); // (the run began at the first look, 50 ms in)
            CHECK(t > looked_ms + 40000);               // (some thirty gaps went by, none of them counted)
            // the plain rule needs no close looks: the same two counts at two looks six minutes apart - nothing began and nothing
            // ended in between, whoever looked
            W u;
            (void)u.Look(4, 4, 0, true);
            CHECK_EQ(int(u.Look(5, 4, 50, true)), int(W::kSkip));
            CHECK_EQ(int(u.Look(5, 4, 360050, true)), int(W::kPass));
            CHECK_EQ(u.lost_posts, 1u);
            // a skipped post seen, then a gap of 5 s: the 3 s of the fewest-open rule are 3 s of looks, from the first one after it
            W x;
            (void)x.Look(0, 0, 0, true, 0);
            (void)x.Look(2, 0, 50, true, 1); // (the look that sees the skipped post: the lost end and a read under way)
            for (at = 5050; at < 5050 + kReadStuckMs; at += 50) {
                const u32 n = static_cast<u32>(at / 200);
                (void)x.Look(n + 2, n, at, true, 1);
                CHECK_EQ(x.realigned, 0u);
            }
            const u32 n = static_cast<u32>(at / 200);
            (void)x.Look(n + 2, n, at, true, 1);
            CHECK_EQ(x.realigned, 1u);
            CHECK_EQ(x.lost_posts, 1u);
        }
    }
    {
        // the plain rule wants BOTH counts still for 3 s: a second read that begins beside the first starts the 3 s again, and what
        // is then put right is everything that is open
        W w;
        (void)w.Look(4, 4, 0, true);
        for (u64 at = 50; at <= 1000; at += 50) {
            CHECK_EQ(int(w.Look(5, 4, at, true)), int(W::kSkip));
        }
        for (u64 at = 1050; at < 4050; at += 50) {
            CHECK_EQ(int(w.Look(6, 4, at, true)), int(W::kSkip)); // (3.05 s is 3 s after the first read began, 2 s after the second)
        }
        CHECK_EQ(int(w.Look(6, 4, 4050, true)), int(W::kPass));
        CHECK_EQ(w.lost_posts, 2u);
        CHECK_EQ(w.realigned, 1u);
    }
}

static void TestCaptureLeavesTheCardToTheGame() {
    std::printf("the capture and the card: the slice shrinks on a slow card, the card rests as long as the pass kept it, the game's reads come first\n");
    const fs::path dir = MakeTempDir("pace");
    PaceFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    CHECK(log.SetLevel(1, true));
    DebugReset();
    FillFrames(300); // (~900 KB of text)
    typedef LogFiles::CaptureTurn Turn;

    // ---- the rest: a pass that kept the card 400 ms (more than the 250 ms that used to be the ceiling of the rest)
    files.capture_write_ms = 400;
    CHECK(log.DrainDebug(false));
    const u64 t1 = platform::NowMs();
    const u64 not_before = log.capture_not_before_ms();
    std::printf("  a pass of 400 ms in the card: the capture is off it for %ld ms more\n", static_cast<long>(not_before - t1));
    // (the upper bound is the ceiling of the rest plus slack, not "about 400": the pass is a real sleep and real calls to a real file, and a
    // PC busy with the other lanes of the tests stretches them; that the rest FOLLOWS the pass is held by RestAfterPass in TestCapturePace)
    CHECK(not_before >= t1 + 380 && not_before <= t1 + 5400);
    CHECK(files.last_capture_write >= 64 * 1024);
    // records are left, but the card rests: nothing now; the writer still wakes within 250 ms (the main log does not wait)
    CHECK(log.NextCaptureTurn(t1, DebugRingUsedBytes(), false) == Turn::kWait);
    CHECK_EQ(log.WriterWaitMs(t1), 250u);
    CHECK_EQ(log.WriterWaitMs(not_before - 100), 100u); // ... and wakes when the rest ends
    CHECK_EQ(log.WriterWaitMs(not_before - 5), 20u);
    CHECK(log.NextCaptureTurn(not_before - 1, DebugRingUsedBytes(), false) == Turn::kWait);
    CHECK(log.NextCaptureTurn(not_before, DebugRingUsedBytes(), false) == Turn::kPass);
    // the HOME button does not wait for the rest
    CHECK(log.NextCaptureTurn(t1, DebugRingUsedBytes(), true) == Turn::kPass);

    // ---- the slice: that write was slow (over 250 ms), so the next ones are 8 KB ...
    CHECK_EQ(log.TakeCardWindow().slice_bytes, 8u * 1024);
    CHECK(log.DrainDebug(false));
    CHECK(files.last_capture_write >= 8 * 1024 && files.last_capture_write < 12 * 1024);
    // ... while the card stays slow, and until eight calls in a row were fast
    files.capture_write_ms = 0;
    int small_passes = 0;
    for (int i = 0; i < 40; ++i) {
        CHECK(log.DrainDebug(false));
        if (files.last_capture_write >= 60 * 1024) {
            break;
        }
        CHECK(files.last_capture_write >= 8 * 1024 && files.last_capture_write < 12 * 1024);
        ++small_passes;
    }
    std::printf("  %d small passes after the card answered again, then 64 KB\n", small_passes);
    // Eight calls: four writes and four flushes, one of them the slow pass's own flush - so never fewer than 3 passes. No tight upper
    // bound here (2026-11-11): these are real calls to a real file, and one of them taking over 250 ms on a busy PC starts the count
    // again, as it should; the exact count of eight is held with made-up calls in TestCapturePace (SliceSize::NoteCall).
    CHECK(small_passes >= 3 && small_passes < 40);
    CHECK(files.last_capture_write >= 64 * 1024);
    CHECK_EQ(log.TakeCardWindow().slice_bytes, 64u * 1024);
    // a slow FLUSH shrinks it too
    files.capture_flush_ms = 300;
    CHECK(log.DrainDebug(false));
    CHECK_EQ(log.TakeCardWindow().slice_bytes, 8u * 1024);
    files.capture_flush_ms = 0;

    // ---- the game's reads: the svc hook's two counts
    u32 begun = 0, done = 0;
    log.SetGameReads(&begun, &done);
    const u64 T = platform::NowMs() + 100000; // (long after any rest: only the reads decide)
    CHECK(log.NextCaptureTurn(T, DebugRingUsedBytes(), false) == Turn::kPass); // no read: write
    begun = 1; // a read under way
    CHECK(log.NextCaptureTurn(T + 50, DebugRingUsedBytes(), false) == Turn::kWait);
    CHECK_EQ(log.WriterWaitMs(T + 50), 50u); // (a look again soon)
    done = 1; // it ended since the last look
    CHECK(log.NextCaptureTurn(T + 100, DebugRingUsedBytes(), false) == Turn::kWait);
    CHECK(log.NextCaptureTurn(T + 150, DebugRingUsedBytes(), false) == Turn::kPass);
    // the HOME button does not wait for the game's reads
    begun = 2;
    CHECK(log.NextCaptureTurn(T + 200, DebugRingUsedBytes(), true) == Turn::kPass);
    done = 2;
    (void)log.TakeCardWindow();
    // a game that reads all the time: 10 s of stepping aside, then one small slice
    int waits = 0;
    Turn turn = Turn::kWait;
    u64 at = T + 1000;
    for (; at < T + 1000 + 20000; at += 50) {
        ++begun;
        ++done;
        turn = log.NextCaptureTurn(at, DebugRingUsedBytes(), false);
        if (turn != Turn::kWait) {
            break;
        }
        ++waits;
    }
    CHECK(turn == Turn::kSmallPass);
    CHECK_EQ(waits, 200); // (10 s of looks 50 ms apart)
    const CardWindow w = log.TakeCardWindow();
    CHECK_EQ(w.forced, 1u);
    CHECK_EQ(w.read_skips, 200u);
    CHECK_EQ(w.game_reads, 202u); // (the 201 of the loop and the one of the HOME button's look)
    // the forced slice is small whatever the card did: with the slice back at 64 KB, it is 8 KB all the same
    for (int i = 0; i < 6 && log.TakeCardWindow().slice_bytes != 64u * 1024; ++i) {
        CHECK(log.DrainDebug(false));
    }
    CHECK_EQ(log.TakeCardWindow().slice_bytes, 64u * 1024);
    FillFrames(60);
    CHECK(log.DrainDebug(false, true));
    CHECK(files.last_capture_write >= 8 * 1024 && files.last_capture_write < 12 * 1024);
    // the gate's count of skipped posts reaches the watch through LogFiles: reads under way at every look for 5 s are a lost end only
    // once the gate has skipped a post (2026-11-11)
    {
        static u32 skipped;
        skipped = 0;
        log.SetGameReads(&begun, &done, [] { return skipped; });
        const u64 T2 = at + 100000;
        (void)log.NextCaptureTurn(T2, DebugRingUsedBytes(), false);
        (void)log.TakeCardWindow();
        ++begun; // one read whose end never comes ...
        for (int i = 1; i <= 100; ++i) {
            if (i % 2) {
                ++begun; // ... and others around it
            } else {
                ++done;
            }
            (void)log.NextCaptureTurn(T2 + i * 50, DebugRingUsedBytes(), false);
        }
        CHECK_EQ(log.TakeCardWindow().realigned, 0u);
        skipped = 1;
        for (int i = 101; i <= 200; ++i) {
            if (i % 2) {
                ++begun;
            } else {
                ++done;
            }
            (void)log.NextCaptureTurn(T2 + i * 50, DebugRingUsedBytes(), false);
        }
        CHECK_EQ(log.TakeCardWindow().realigned, 1u);
        done = begun;
    }
    // 2026-11-11, the second review: the gate's count is read BEFORE the two counts of the reads. A read that begins and loses its end
    // while the writer is between its loads (here: inside the load of the skipped posts itself, before it answers or after it took its
    // value) must not show its skipped post beside counts that do not hold the read yet - the look would find nothing under way and
    // write the post off, and the lost end would stay in the count until the five minutes of the last resort
    for (const bool before_the_load : {true, false}) {
        static u32 skipped;
        static u32* begun_at;
        static int lose; // 1: a read begins and loses its end before this load answers; 2: after it took its value
        skipped = 0;
        begun_at = &begun;
        lose = 0;
        done = begun;
        log.SetGameReads(&begun, &done, [] {
            const u32 was = skipped;
            if (lose != 0) {
                ++*begun_at;
                ++skipped;
            }
            const u32 answer = lose == 1 ? skipped : was;
            lose = 0;
            return answer;
        });
        const u64 T3 = platform::NowMs() + (before_the_load ? 400000 : 600000);
        (void)log.NextCaptureTurn(T3, DebugRingUsedBytes(), false); // (a look with nothing under way: the count is right)
        (void)log.TakeCardWindow();
        lose = before_the_load ? 1 : 2;
        for (int i = 1; i <= 70; ++i) { // 3.5 s of looks, the game reading around the lost end
            if (i > 1) {
                if (i % 2) {
                    ++done;
                } else {
                    ++begun;
                }
            }
            (void)log.NextCaptureTurn(T3 + i * 50, DebugRingUsedBytes(), false);
        }
        CHECK_EQ(lose, 0); // (the load did ask)
        CHECK_EQ(log.TakeCardWindow().realigned, 1u);
        done = begun;
    }
    // 2026-11-11: the two counts are of EVERY call of the game to the file system (azl/game_card_calls.hpp), not of the reads alone.
    // Tekken 3D Prime Edition on a console: the call left waiting 26 s behind the writer was fs:USER OpenFileDirectly. The hook's two
    // lines are played here as Sources/ctr/hook.cpp has them (the policy check pins the lines): an open, a write, a close under way
    // make the capture step aside as a read does, and a call to gsp, dsp or APT never does.
    {
        const auto pre = [&begun](u32 header) {
            if (IsFileSystemCall(header)) {
                ++begun;
            }
        };
        const auto post = [&done](u32 header) {
            if (IsFileSystemCall(header)) {
                ++done;
            }
        };
        done = begun;
        log.SetGameReads(&begun, &done);
        u64 T4 = platform::NowMs() + 800000;
        (void)log.NextCaptureTurn(T4, DebugRingUsedBytes(), false); // (the look that sees the counts of the case above put level)
        CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kPass);
        (void)log.TakeCardWindow();
        // (OpenFileDirectly, OpenFile, FSFILE_Write, FSFILE_Close, FSFILE_GetSize, OpenArchive, ControlArchive, CreateFile, FSDIR_Read)
        const u32 file_calls[] = {0x08030204, 0x080201C2, 0x08030102, 0x08080000, 0x08040000, 0x080C00C2, 0x080D0144, 0x08080202, 0x08010042};
        u32 calls = 0;
        for (const u32 header : file_calls) {
            pre(header); // under way
            CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kWait);
            CHECK_EQ(log.WriterWaitMs(T4), 50u); // (a look again soon, as for a read)
            post(header); // it ended since the last look
            CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kWait);
            CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kPass);
            ++calls;
        }
        const CardWindow opens = log.TakeCardWindow();
        CHECK_EQ(opens.game_reads, calls);
        CHECK_EQ(opens.read_skips, 2 * calls);
        CHECK_EQ(opens.forced, 0u);
        // (gsp TriggerCmdReqQueue and FlushDataCache, dsp, srv, cfg:u, APT InquireNotification, nwm::UDS PullPacket, soc:U poll)
        const u32 other_calls[] = {0x000C0000, 0x00080082, 0x001F0000, 0x00050100, 0x00010082, 0x000B0040, 0x001400C0, 0x00140084};
        for (const u32 header : other_calls) {
            pre(header);
            CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kPass);
            post(header);
            CHECK(log.NextCaptureTurn(T4 += 50, DebugRingUsedBytes(), false) == Turn::kPass);
        }
        const CardWindow others = log.TakeCardWindow();
        CHECK_EQ(others.game_reads, 0u);
        CHECK_EQ(others.read_skips, 0u);
        CHECK_EQ(begun, done);
    }
    log.SetGameReads(nullptr, nullptr);
    // the gate of an Old 3DS's swap closed: the pass does not begin, and the writer comes back for it soon. From a clean state
    // (2026-11-11): the ring emptied and nothing waiting, so that it is the stopped pass that makes the next one due
    for (int i = 0; i < 200 && log.DrainDebug(false); ++i) {
    }
    CHECK_EQ(DebugBytesQueued(), 0u);
    const u64 later = platform::NowMs() + 100000;
    CHECK(log.NextCaptureTurn(later, DebugRingUsedBytes(), false) == Turn::kPass);
    CHECK_EQ(log.WriterWaitMs(later), 250u); // (nothing owed)
    volatile u32 gate = 1;
    log.SetStopCapture(&gate);
    CHECK(log.DrainDebug(false));
    CHECK_EQ(log.WriterWaitMs(later), 20u);
    gate = 0;
    log.SetStopCapture(nullptr);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);
    // a capture file that cannot be opened (the card is not ready): nothing is written, and the writer does not come back every 20 ms
    // for a file that is not there
    {
        const fs::path missing = MakeTempDir("pace2") / "missing";
        DirFiles none;
        LogFiles late(&none, missing.string() + "/", platform::NowMs());
        g_debug_level = 1;
        FillFrames(5);
        CHECK(!late.DrainDebug(false));
        CHECK(!late.debug_open());
        CHECK_EQ(late.WriterWaitMs(platform::NowMs()), 250u);
        g_debug_level = 0;
        late.Close();
        DebugReset();
        fs::remove_all(missing.parent_path());
    }
}

static void TestCardWindow() {
    std::printf("the calls to the card are counted one by one: writes and flushes, the capture and the main log\n");
    const fs::path dir = MakeTempDir("cardwin");
    PaceFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    DebugReset();
    (void)log.TakeCardWindow();
    files.capture_write_ms = 30;
    files.capture_flush_ms = 20;
    Log("a line of the main log");
    log.Flush();
    FillFrames(100);
    CHECK(log.DrainDebug(false));
    CHECK(log.DrainDebug(false));
    const CardWindow w = log.TakeCardWindow();
    CHECK_EQ(w.capture_write.calls, 2u);
    CHECK(w.capture_write.bytes >= 128 * 1024 && w.capture_write.bytes < 136 * 1024);
    CHECK(azl_ticks_to_ms(w.capture_write.ticks) >= 55 && azl_ticks_to_ms(w.capture_write.ticks) < 400);
    CHECK(w.capture_write.longest_ms >= 28 && w.capture_write.longest_ms < 250);
    CHECK(w.capture_write.longest_bytes >= 64 * 1024 && w.capture_write.longest_bytes < 68 * 1024);
    CHECK_EQ(w.capture_flush.calls, 2u);
    CHECK_EQ(w.capture_flush.bytes, 0u);
    CHECK(azl_ticks_to_ms(w.capture_flush.ticks) >= 36 && azl_ticks_to_ms(w.capture_flush.ticks) < 300);
    CHECK(w.capture_flush.longest_ms >= 18);
    CHECK_EQ(w.main_write.calls, 1u);
    CHECK_EQ(w.main_write.bytes, 12u + 22u + 1u); // ("[    0.000] " + the line + the newline)
    CHECK_EQ(w.main_flush.calls, 1u);
    CHECK_EQ(w.other.calls, 0u);
    CHECK_EQ(w.passes, 2u);
    CHECK_EQ(w.forced, 0u);
    CHECK_EQ(w.slice_bytes, 64u * 1024);
    CHECK(w.rest_ms >= 2 * 45 && w.rest_ms < 800); // (each pass kept the card ~50 ms: the capture stayed off it as long)
    char line[512];
    LogFiles::FormatCardLine(w, line, sizeof(line));
    std::printf("  %s\n", line);
    const std::string text = line;
    CHECK(text.compare(0, 30, "stats: card: capture 2 writes ") == 0);
    CHECK(Contains(text, " | main log 1 writes 35 B "));
    CHECK(Contains(text, " | other 0 calls 0 ms | slice 64 KB, 2 passes (0 forced small), 0 skipped for the game's file calls (0 calls, 0 realigned), rest "));
    CHECK(!Contains(text, "game:") && !Contains(text, "longer than")); // (the fields the analysis scripts read are not in it)
    CHECK(text.size() < 440);
    // taken: the next window starts from nothing (the slice stays what it is)
    const CardWindow empty = log.TakeCardWindow();
    CHECK_EQ(empty.capture_write.calls + empty.capture_flush.calls + empty.main_write.calls + empty.main_flush.calls, 0u);
    CHECK_EQ(empty.capture_write.bytes + empty.capture_write.longest_ms + empty.passes + empty.rest_ms, 0u);
    CHECK_EQ(empty.slice_bytes, 64u * 1024);
    // the whole line, from numbers made up
    CardWindow m;
    m.capture_write = {2, 131072, 9310ull * AZL_TICKS_PER_MS, 4810, 65536};
    m.capture_flush = {1, 0, 35ull * AZL_TICKS_PER_MS, 35, 0};
    m.main_write = {5, 13824, 1234ull * AZL_TICKS_PER_MS, 999, 2764};
    m.main_flush = {5, 0, 77ull * AZL_TICKS_PER_MS, 40, 0};
    m.other = {1, 0, 12ull * AZL_TICKS_PER_MS, 12, 0};
    m.slice_bytes = 8 * 1024;
    m.passes = 12;
    m.forced = 1;
    m.read_skips = 37;
    m.game_reads = 512;
    m.realigned = 2;
    m.rest_ms = 9345;
    m.yields = 345;
    m.beacons_same = 1234;
    LogFiles::FormatCardLine(m, line, sizeof(line));
    CHECK_EQ(std::string(line),
             std::string("stats: card: capture 2 writes 131072 B 9310 ms (longest 4810 ms for 65536 B), 1 flushes 35 ms (longest 35 ms)"
                         " | main log 5 writes 13824 B 1234 ms (longest 999 ms for 2764 B), 5 flushes 77 ms (longest 40 ms)"
                         " | other 1 calls 12 ms | slice 8 KB, 12 passes (1 forced small), 37 skipped for the game's file calls"
                         " (512 calls, 2 realigned), rest 9345 ms, 345 yields, 1234 beacons counted not written"));
    // (2026-11-11: the count is of every call of the game to the file system, and the line says "calls" - a number of opens and closes
    // under the name "reads" would be read wrong at the next console test)
    CHECK(!Contains(std::string(line), "reads"));
    // a rotation is counted apart (it is card time too)
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
    {
        const fs::path dir2 = MakeTempDir("cardwin2");
        PaceFiles files2;
        LogFileLimits limits;
        limits.debug_rotate_bytes = 32 * 1024;
        LogFiles rot(&files2, dir2.string() + "/", platform::NowMs(), limits);
        CHECK(rot.SetLevel(1, true));
        DebugReset();
        (void)rot.TakeCardWindow();
        FillFrames(30);
        CHECK(rot.DrainDebug(false));
        CHECK_EQ(rot.TakeCardWindow().other.calls, 1u);
        CHECK(rot.SetLevel(0, false));
        fs::remove_all(dir2);
    }
}

static LogFiles* g_between_log = nullptr;
static int g_between_writes = 0;

static void TestMainLogGoesFirst() {
    std::printf("the main log's waiting lines are written before each write of the capture\n");
    const fs::path dir = MakeTempDir("mainfirst");
    PaceFiles files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    DebugReset();
    FillFrames(100);
    Log("MAIN-FIRST: a line that waits for the writer");
    files.trace.clear();
    CHECK(log.DrainDebug(false));
    CHECK_EQ(files.trace, std::string("MmCc")); // the main log (written and flushed), then the capture's one write and its flush
    CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "MAIN-FIRST: a line that waits for the writer\n"));
    // nothing waiting: the capture alone
    files.trace.clear();
    CHECK(log.DrainDebug(false));
    CHECK_EQ(files.trace, std::string("Cc"));
    // the last pass of a launch writes slice after slice: a line that comes during the first write is on the card before the second
    g_between_log = &log;
    g_between_writes = 0;
    files.on_capture_write = [] {
        if (g_between_writes++ == 0) {
            g_between_log->Append("BETWEEN: a line that came while the capture was in the card");
        }
    };
    FillFrames(100);
    files.trace.clear();
    CHECK(!log.DrainDebug(true));
    CHECK(files.trace.size() > 6);
    CHECK_EQ(files.trace.substr(0, 4), std::string("CMmC"));
    CHECK(files.trace.back() == 'c');
    CHECK_EQ(static_cast<int>(std::count(files.trace.begin(), files.trace.end(), 'c')), 1); // (one flush of the capture for the whole pass)
    CHECK_EQ(static_cast<int>(std::count(files.trace.begin(), files.trace.end(), 'M')), 1);
    files.on_capture_write = nullptr;
    g_between_log = nullptr;
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

static std::size_t CountOf(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

static void TestBeaconsAreCounted() {
    std::printf("a beacon that is the one before of its transmitter is counted, not written again\n");
    const fs::path dir = MakeTempDir("beacons");
    DirFiles files;
    LogFileLimits limits;
    limits.debug_rotate_bytes = 6000;
    limits.keep = 8;
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    CHECK(log.SetLevel(1, true));
    DebugReset();
    (void)log.TakeCardWindow();
    const u8 mac_a[6] = {0x00, 0x1f, 0x32, 0xaa, 0xbb, 0xcc};
    const u8 mac_b[6] = {0x40, 0xf4, 0x07, 0x01, 0x02, 0x03};
    std::vector<u8> body_a(377), body_b(331);
    for (std::size_t i = 0; i < body_a.size(); ++i) {
        body_a[i] = static_cast<u8>(i * 7 + 1);
    }
    for (std::size_t i = 0; i < body_b.size(); ++i) {
        body_b[i] = static_cast<u8>(i * 3 + 2);
    }
    std::string hex_a;
    for (u8 b : body_a) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02x", b);
        hex_a += two;
    }
    const std::string whole_a = " BEACON rx    00:1f:32:aa:bb:cc ch11 377B: " + hex_a + "\n";
    // 20 beacons of a host heard (rx) and 20 of ours (tx), the same bytes every time, with a frame among them
    for (int i = 0; i < 20; ++i) {
        DebugBeacon(kDirRx, mac_a, 11, body_a.data(), 377);
        DebugBeacon(kDirTx, mac_b, 11, body_b.data(), 331);
        if (i == 10) {
            const u8 small[40] = {1, 2, 3};
            DebugFrame(kDirRx, 1, 2, 3, small, sizeof(small));
        }
    }
    CHECK(!log.DrainDebug(false));
    std::string text = ReadAll(dir / "AzaharLink.debug.log");
    CHECK_EQ(CountOf(text, whole_a), std::size_t(1)); // the first one whole, byte for byte the line of before
    CHECK_EQ(CountOf(text, " BEACON rx    00:1f:32:aa:bb:cc ch11 377B: same x19 from "), std::size_t(1));
    CHECK_EQ(CountOf(text, " BEACON tx    40:f4:07:01:02:03 ch11 331B: same x19 from "), std::size_t(1));
    CHECK_EQ(CountOf(text, " BEACON "), std::size_t(4)); // two whole, two counts: not forty lines of 700 characters
    CHECK_EQ(CountOf(text, ", longest gap "), std::size_t(2));
    CHECK_EQ(CountOf(text, " FRAME rx "), std::size_t(1)); // (the other records are what they were)
    CHECK(text.find(whole_a) < text.find("same x19 from")); // the whole beacon first, the count after it
    CHECK_EQ(log.TakeCardWindow().beacons_same, 38u);
    std::printf("  40 beacons in %lu bytes of capture\n", static_cast<unsigned long>(text.size()));
    CHECK(text.size() < 2400); // (whole, the forty lines were 30 KB)

    // the next pass: still the same bytes, so only counts; then the body changes: the count of the old one, then the new one whole
    for (int i = 0; i < 3; ++i) {
        DebugBeacon(kDirRx, mac_a, 11, body_a.data(), 377);
    }
    body_a[40] ^= 0x55;
    DebugBeacon(kDirRx, mac_a, 11, body_a.data(), 377);
    DebugBeacon(kDirRx, mac_a, 11, body_a.data(), 377);
    CHECK(!log.DrainDebug(false));
    text = ReadAll(dir / "AzaharLink.debug.log");
    const std::size_t same3 = text.find("ch11 377B: same x3 from ");
    const std::size_t same1 = text.find("ch11 377B: same x1 from ");
    CHECK(same3 != std::string::npos && same1 != std::string::npos && same3 < same1);
    const std::size_t changed = text.find(" BEACON rx    00:1f:32:aa:bb:cc ch11 377B: 0108", same3); // (the new body, whole, between the two counts)
    CHECK(changed != std::string::npos && changed > same3 && changed < same1);
    CHECK_EQ(CountOf(text, whole_a), std::size_t(1)); // (the old body was not written again)
    // another channel or another length is another beacon
    DebugBeacon(kDirRx, mac_a, 6, body_a.data(), 377);
    DebugBeacon(kDirRx, mac_a, 6, body_a.data(), 376);
    CHECK(!log.DrainDebug(false));
    text = ReadAll(dir / "AzaharLink.debug.log");
    CHECK_EQ(CountOf(text, " BEACON rx    00:1f:32:aa:bb:cc ch6 377B: 0108"), std::size_t(1));
    CHECK_EQ(CountOf(text, " BEACON rx    00:1f:32:aa:bb:cc ch6 376B: 0108"), std::size_t(1));

    // a new file (the rotation at 6000 bytes): its first beacon is whole again, whatever the file before had
    for (int i = 0; i < 40; ++i) {
        DebugEvent("padding %d: a line of text of the capture, long enough to fill the file and make it rotate at its small test size ............", i);
    }
    CHECK(!log.DrainDebug(false));
    CHECK(!Blocks(Listing(dir), "AzaharLink.debug.").empty());
    DebugBeacon(kDirRx, mac_a, 6, body_a.data(), 376);
    DebugBeacon(kDirRx, mac_a, 6, body_a.data(), 376);
    DebugBeacon(kDirRx, mac_a, 6, body_a.data(), 376);
    CHECK(!log.DrainDebug(false));
    text = ReadAll(dir / "AzaharLink.debug.log");
    CHECK(Contains(text, "debug capture: this file begins here"));
    CHECK(text.size() < 2000); // (the file after the rotation: the header, one beacon, one count)
    CHECK_EQ(CountOf(text, " BEACON rx    00:1f:32:aa:bb:cc ch6 376B: 0108"), std::size_t(1));
    CHECK_EQ(CountOf(text, " ch6 376B: same x2 from "), std::size_t(1));

    // more transmitters than the table has places (a search among the public lobbies): the ninth is written whole every time, as before
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);
    const fs::path dir2 = MakeTempDir("beacons2");
    DirFiles files2;
    LogFiles many(&files2, dir2.string() + "/", platform::NowMs());
    CHECK(many.SetLevel(1, true));
    DebugReset();
    for (int round = 0; round < 3; ++round) {
        for (u8 host = 1; host <= 9; ++host) {
            const u8 mac[6] = {0x02, 0, 0, 0, 0, host};
            DebugBeacon(kDirRx, mac, 1, body_b.data(), 331);
        }
    }
    CHECK(!many.DrainDebug(false));
    text = ReadAll(dir2 / "AzaharLink.debug.log");
    CHECK_EQ(CountOf(text, " ch1 331B: same x2 from "), std::size_t(8));
    CHECK_EQ(CountOf(text, " BEACON rx    02:00:00:00:00:09 ch1 331B: 0205"), std::size_t(3));
    CHECK_EQ(CountOf(text, " BEACON rx    02:00:00:00:00:01 ch1 331B: 0205"), std::size_t(1));
    // a switch-off and on again opens the file anew: whole again
    CHECK(many.SetLevel(0, false));
    CHECK(many.SetLevel(1, true));
    const u8 mac1[6] = {0x02, 0, 0, 0, 0, 1};
    DebugBeacon(kDirRx, mac1, 1, body_b.data(), 331);
    CHECK(!many.DrainDebug(false));
    text = ReadAll(dir2 / "AzaharLink.debug.log");
    CHECK_EQ(CountOf(text, " BEACON rx    02:00:00:00:00:01 ch1 331B: 0205"), std::size_t(2));
    CHECK(many.SetLevel(0, false));
    fs::remove_all(dir2);
}

static void TestWriterGivesTheCoreUp() {
    std::printf("a pass that is not the last gives the core up while it formats; the last pass of a launch never does\n");
    const fs::path dir = MakeTempDir("yield");
    DirFiles files;
    LogFileLimits limits;
    limits.capture_yield_ms = 0; // (here: whenever any time at all has gone by, so that a PC as fast as it likes shows it)
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    CHECK(log.SetLevel(1, true));
    DebugReset();
    (void)log.TakeCardWindow();
    CHECK_EQ(log.yielded_ticks(), u64(0));
    FillFrames(60);
    CHECK(log.DrainDebug(false));
    const u32 yields = log.TakeCardWindow().yields;
    std::printf("  %lu pauses in a block of 64 KB\n", static_cast<unsigned long>(yields));
    CHECK(yields >= 10 && yields <= 25); // (one after each of the ~21 frames of the block)
    // the time of the pauses is kept apart: the writer does not charge it as its CPU
    const u64 yielded = log.yielded_ticks();
    CHECK(azl_ticks_to_ms(yielded) >= yields && azl_ticks_to_ms(yielded) < 2000);
    CHECK(!log.DrainDebug(true)); // the rest of the ring, at the end of the launch
    CHECK_EQ(log.TakeCardWindow().yields, 0u);
    CHECK_EQ(log.yielded_ticks(), yielded);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);
    // the limit the plugin runs with
    CHECK_EQ(LogFileLimits().capture_yield_ms, 2u);
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
    // a traced service's command (the lines of "svc REAL") is under the same diet when its session's handle is given: three whole lines,
    // then one count with the service's name; a failure is always whole, and with no handle every call is
    for (u32 i = 0; i < 10; ++i) {
        DebugSvcReal("frd:u", 0x19, i, 0, 0x260042);
    }
    DebugSvcReal("frd:u", 0x19, 0x63, 0xD8E007F7u, 0x260042);
    for (u32 i = 0; i < 5; ++i) {
        DebugSvcReal("ac:u", 0x0D, 0x40 + i, 0);
    }
    log.DrainDebug(true);
    {
        const std::string text = ReadAll(dir / "AzaharLink.debug.log");
        CHECK(Contains(text, "svc REAL frd:u cmd 0x0019 arg 0x2 -> 0x00000000\n"));
        CHECK(!Contains(text, "svc REAL frd:u cmd 0x0019 arg 0x3 -> 0x00000000\n"));
        CHECK(Contains(text, "svc REAL frd:u cmd 0x0019 arg 0x9 -> 0x00000000 x7 from "));
        CHECK(Contains(text, "svc REAL frd:u cmd 0x0019 arg 0x63 -> 0xd8e007f7\n"));
        CHECK(Contains(text, "svc REAL ac:u cmd 0x000d arg 0x44 -> 0x00000000\n"));
        CHECK(!Contains(text, "svc REAL ac:u cmd 0x000d arg 0x44 -> 0x00000000 x"));
    }

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
    // 2026-11-11: a pass that is NOT the last says it too, although it leaves records in the ring. Such a pass is one block, and with a
    // capture the card cannot keep up with no pass ever empties the ring: the count stayed in memory until the end of the launch, and
    // was lost with a game that fell (the review of the capture's writer: 0 passes of 40 wrote the line).
    for (int round = 0; round < 3; ++round) {
        const u32 lost_before = DebugCaptureTotals().lost_full[1];
        for (int i = 0; i < 400; ++i) {
            DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
        }
        const u32 lost_now = DebugCaptureTotals().lost_full[1] - lost_before;
        CHECK(lost_now > 0);
        CHECK(log.DrainDebug(false)); // (one block: records are left)
        CHECK(DebugBytesQueued() > 0);
        CHECK(Contains(ReadAll(dir / "AzaharLink.debug.log"), ("debug: " + std::to_string(lost_now) + " records were dropped (").c_str()));
    }
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
    // all threads waited, 5 ms were the network thread's); 50 ms in the frame callback; 10 ms in the chat's OSD callback (drawn at the
    // present, outside the frame callback: defect 34, the stats line's total did not hold it); 20 ms in the writer (CPU)
    s.ipc_sum_ticks = 300 * AZL_TICKS_PER_MS;
    s.lock_wait_ticks = 20 * AZL_TICKS_PER_MS;
    s.lock_wait_net_ticks = 5 * AZL_TICKS_PER_MS;
    s.t_cb = 50 * AZL_TICKS_PER_MS;
    s.t_chat_osd = 10 * AZL_TICKS_PER_MS;
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
    // in it); the game threads 300 - 15 ms of lock wait = 285 ms = 57.0; the frame callback 10.0 and the chat's OSD callback 2.0 (the
    // status page's "frame" holds both); the writer 4.0
    CHECK(n.net10 >= 600 - 2 && n.net10 <= 600 + 2);
    CHECK(n.uds10 >= 570 - 2 && n.uds10 <= 570 + 2);
    CHECK_EQ(n.frame10, 120u);
    CHECK_EQ(n.writer10, 40u);
    CHECK_EQ(n.plugin10, n.net10 + n.uds10 + n.frame10); // (the writer's figure is wall time of a thread that waits for the core: shown, not added)
    CHECK(n.plugin10 >= 1290 - 4 && n.plugin10 <= 1290 + 4);
    {
        // the stats line's figures: the chat's drawing apart from the frame callback, and in the total
        const CpuNumbers c = MakeCpuNumbers(s, 5000);
        CHECK_EQ(c.frame10, 100u);
        CHECK_EQ(c.chat10, 20u);
        CHECK_EQ(c.plugin10, c.net10 + c.uds10 + c.frame10 + c.chat10);
    }
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
    CHECK(Contains(text, "Plugin CPU ms/s 129.0: net 60.0, uds 57.0,\n  frame 12.0, log 4.0 wall\n"));
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
    g_stats.t_chat_osd = 1013;
    StatsNoteChatOsd(5); // (three draws of the chat's OSD callback: their sum, their count and the longest)
    StatsNoteChatOsd(9);
    StatsNoteChatOsd(3);
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
    CHECK_EQ(w.t_chat_osd, 1013u + 5 + 9 + 3);
    CHECK_EQ(w.chat_osd_draws, 3u);
    CHECK_EQ(w.chat_osd_max_ticks, 9u);
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

/// The swap of the plugin's block on an Old 3DS: the gate closes while the writer is in a pass, and the writer must leave the card. On a slow card the two writes of a pass
/// and the flush outlast what the swap can wait.
class StopAfterWrites : public DirFiles {
public:
    void Write(void* file, const char* data, std::size_t size) override {
        DirFiles::Write(file, data, size);
        if (size >= 32 * 1024) {
            ++big_writes;
            if (stop_after != 0 && big_writes >= stop_after) {
                flag = 1;
            }
        }
    }
    volatile u32 flag = 0;
    int big_writes = 0;
    int stop_after = 0;
};

/// The files of TestGateClosesWhileAPassFormats: what was written, and whether the gate was closed by then (atomics: two threads).
class GateWatchFiles : public DirFiles {
public:
    void* OpenAppend(const std::string& path) override {
        void* file = DirFiles::OpenAppend(path);
        if (file && path.size() >= 14 && path.compare(path.size() - 14, 14, "AzaharLink.log") == 0) {
            main_log = file;
        }
        return file;
    }
    void Write(void* file, const char* data, std::size_t size) override {
        if (file == main_log.load()) {
            ++main_writes;
        } else if (size >= 32 * 1024) {
            ++capture_writes;
            closed_at_capture_write = flag != 0;
        }
        DirFiles::Write(file, data, size);
    }
    volatile u32 flag = 0;
    std::atomic<void*> main_log{nullptr};
    std::atomic<int> main_writes{0};
    std::atomic<int> capture_writes{0};
    std::atomic<bool> closed_at_capture_write{false};
};

/// 2026-11-11 (the review of the capture's writer): the gate of the swap closes while a pass is formatting its block. The swap waits
/// for that pass (CloseSwapGate), so from then on the pass does not sleep any more and does not write the main log's lines first: what
/// is left of it is the formatting, ONE write and its flush, as before 2026-11-10. The gate is closed by this thread a little after the
/// pass began on another; an attempt in which that came too early (the pass never began) or too late is made again, never judged.
static void TestGateClosesWhileAPassFormats() {
    std::printf("the swap of the block: with the gate closing, a pass under way stops sleeping and leaves the main log for later\n");
    const fs::path dir = MakeTempDir("gateformat");
    GateWatchFiles files;
    LogFileLimits limits;
    limits.capture_yield_ms = 0; // (a sleep of 1 ms after every record: a block of small records is most of a second of them)
    LogFiles log(&files, dir.string() + "/", platform::NowMs(), limits);
    log.SetStopCapture(&files.flag);
    SetLogSink([&log](const char* line) { log.Append(line); });
    CHECK(log.SetLevel(1, true));
    const u8 small[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto fill = [&small] {
        DebugReset();
        for (int i = 0; i < 2500; ++i) {
            DebugFrame(kDirRx, 1, 2, 3, small, sizeof(small));
        }
    };
    // the measure: the same block with the gate open
    fill();
    log.Flush();
    (void)log.TakeCardWindow();
    CHECK(log.DrainDebug(false));
    const u32 yields_open = log.TakeCardWindow().yields;
    std::printf("  a block of small records with the gate open: %lu sleeps\n", static_cast<unsigned long>(yields_open));
    CHECK(yields_open >= 400);
    bool judged = false;
    for (int attempt = 0; attempt < 8 && !judged; ++attempt) {
        fill();
        log.Flush();
        (void)log.TakeCardWindow();
        files.flag = 0;
        files.capture_writes = 0;
        files.closed_at_capture_write = false;
        Log("GATE-FORMAT: a line that waits for the writer");
        const int main_before = files.main_writes.load();
        const auto begin = std::chrono::steady_clock::now();
        std::thread pass([&log] { log.DrainDebug(false); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10 + 10 * attempt));
        files.flag = 1; // SWAP_ENTER
        const auto closed_after = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        pass.join();
        const u32 yields = log.TakeCardWindow().yields;
        const int main_in_pass = files.main_writes.load() - main_before;
        files.flag = 0;
        // (began: it wrote its block; in time: it had at most `closed_after` sleeps of 1 ms behind it, a fraction of the block's)
        const bool began = files.capture_writes.load() == 1;
        const bool in_time = closed_after * 4 < static_cast<long long>(yields_open);
        if (began && in_time && files.closed_at_capture_write.load()) {
            judged = true;
            std::printf("  the gate closed %ld ms into the pass: %lu sleeps in all, %d writes of the main log\n", static_cast<long>(closed_after),
                        static_cast<unsigned long>(yields), main_in_pass);
            CHECK(yields * 2 < yields_open); // (no sleep after the gate closed: before, all of the block's)
            CHECK_EQ(main_in_pass, 0);       // (the main log's line was not written in front of the capture's block ...)
            CHECK(!Contains(ReadAll(dir / "AzaharLink.log"), "GATE-FORMAT: a line that waits for the writer\n"));
            log.Flush(); // (... it waited in the queue: the writer's next round, once the gate is open)
            CHECK(Contains(ReadAll(dir / "AzaharLink.log"), "GATE-FORMAT: a line that waits for the writer\n"));
        } else {
            log.Flush();
        }
    }
    CHECK(judged);
    files.flag = 0;
    CHECK(log.SetLevel(0, false));
    SetLogSink([](const char*) {});
    fs::remove_all(dir);
}

static void TestCaptureStopsAtTheGate() {
    std::printf("the swap of the block: the capture's pass does not begin with the gate closed, and stops after the write in hand\n");
    const std::vector<u8> payload(1500, 7);
    const fs::path dir = MakeTempDir("gatestop");
    StopAfterWrites files;
    LogFiles log(&files, dir.string() + "/", platform::NowMs());
    log.SetStopCapture(&files.flag);
    CHECK(log.SetLevel(1, true));
    DebugReset();
    for (int i = 0; i < 300; ++i) { // (more than 64 KB of text: a pass would make several writes)
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    const u32 queued = DebugBytesQueued();
    CHECK(queued > 200 * 1024);

    // closed before the pass: nothing goes to the card, the ring keeps it all, and the writer is told to come back
    files.flag = 1;
    const int writes_before = files.writes;
    CHECK(log.DrainDebug(false));
    CHECK_EQ(files.writes, writes_before);
    CHECK_EQ(DebugBytesQueued(), queued);

    // closed during the first write: no second one
    files.flag = 0;
    files.stop_after = 1;
    CHECK(log.DrainDebug(false));
    CHECK_EQ(files.big_writes, 1);
    const u32 left = DebugBytesQueued();
    CHECK(left > 0 && left < queued);

    // open again: the rest is written
    files.stop_after = 0;
    files.flag = 0;
    for (int i = 0; i < 40 && DebugBytesQueued() != 0; ++i) {
        log.DrainDebug(false);
    }
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(files.big_writes > 1);

    // the end of a launch is not stopped: the last pass takes what it takes
    for (int i = 0; i < 100; ++i) {
        DebugFrame(kDirRx, 1, 2, 3, payload.data(), 1500);
    }
    files.flag = 1;
    log.DrainDebug(true);
    CHECK_EQ(DebugBytesQueued(), 0u);
    CHECK(log.SetLevel(0, false));
    fs::remove_all(dir);
    TestGateClosesWhileAPassFormats();
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

// (2m-diag D2, D3, 2026-11-21) Tetris with eight, console 63, debug=1: six notices "room link silent ... WiFi" with the WiFi at full strength,
// in windows the log writer held up to 17 s (one write of the card took 13.4 s). The notice now takes the datagrams per second over the
// real window and names the room or the network at strength 3 (Plugin::CheckLinkSilence); the stats line marks a stretched window
// (Plugin::LogStatsTiming). The plugin's code around them is pinned by the block "2m-diag" of Tests/policy_check.sh.
static void TestLinkSilenceAndStretchedWindows() {
    std::printf("the silence notice by datagrams per second of the real window, its cause at full strength, the stretched window (2m-diag D2/D3)\n");
    // a normal window of 5 s: the old bound, 100 datagrams and two silences of 150 ms or more
    CHECK(LinkSilenceWorthANotice(100, 2, 5000, 5000));
    CHECK(!LinkSilenceWorthANotice(99, 2, 5000, 5000));
    CHECK(!LinkSilenceWorthANotice(100, 1, 5000, 5000));
    CHECK(LinkSilenceWorthANotice(1500, 6, 5000, 5000));
    // (2m-rev) ... and so is an ordinary window of 5.0-6.0 s (the writer closes it at its first service after the 5 s): 105 in 5.6 s did
    // not pass the bound of the 2m (112), it passes the 100 of before; beyond 6 s the real length less one second of the writer counts
    CHECK(LinkSilenceWorthANotice(105, 2, 5600, 5000));
    CHECK(LinkSilenceWorthANotice(100, 2, 5900, 5000));
    CHECK(LinkSilenceWorthANotice(100, 2, 6000, 5000));
    CHECK(!LinkSilenceWorthANotice(99, 2, 6000, 5000));
    CHECK(!LinkSilenceWorthANotice(100, 2, 6001, 5000));
    CHECK(LinkSilenceWorthANotice(101, 2, 6001, 5000));
    CHECK(!LinkSilenceWorthANotice(99, 2, 4000, 5000)); // (a window shorter than the nominal one: never a lower bound than 100)
    // a window of 17 s the writer held: 101 datagrams were "traffic" until the 2m, they are 5.9 a second now; the same rate as before does it
    CHECK(!LinkSilenceWorthANotice(101, 3, 17000, 5000));
    CHECK(!LinkSilenceWorthANotice(319, 3, 17000, 5000));
    CHECK(LinkSilenceWorthANotice(320, 3, 17000, 5000));
    CHECK(!LinkSilenceWorthANotice(0xFFFFFFFFu, 2, 0, 5000)); // (no window: no notice, no division)
    CHECK(LinkSilenceWorthANotice(0xFFFFFFFFu, 2, 0xFFFFFFFFull, 5000)); // (no overflow in the products)
    // where the notice says the silence is
    CHECK(std::string(LinkSilenceWhere(3)) == "room/network");
    for (int strength = 0; strength < 256; ++strength) {
        if (strength != 3) {
            CHECK(std::string(LinkSilenceWhere(static_cast<u8>(strength))) == "WiFi");
        }
    }
    // the stretched window: twice the nominal 5 s or more (the window of 18.4 s around the write of 13.4 s; an ordinary 5.2 s one is not)
    CHECK(!WindowStretched(5000, 5000));
    CHECK(!WindowStretched(5200, 5000));
    CHECK(!WindowStretched(9999, 5000));
    CHECK(WindowStretched(10000, 5000));
    CHECK(WindowStretched(18400, 5000));
    // (2m-rev) ... and who stretched it: the writer when its SD card time and its latest wake-up cover half of the excess or more, the
    // caller of Tick with no writer thread, something else (HOME, sleep) otherwise; a window that is not stretched has no mark
    CHECK(WhoStretchedTheWindow(18400, 5000, 13400, 0, true) == StretchBy::LogWriter);   // (Tetris with eight: the write of 13.4 s)
    CHECK(WhoStretchedTheWindow(18400, 5000, 6700, 0, true) == StretchBy::LogWriter);    // (half of the 13.4 s of excess)
    CHECK(WhoStretchedTheWindow(18400, 5000, 6000, 699, true) == StretchBy::NotTheWriter);
    CHECK(WhoStretchedTheWindow(18400, 5000, 6000, 700, true) == StretchBy::LogWriter);  // (the late wake-up counts with the card)
    CHECK(WhoStretchedTheWindow(18400, 5000, 50, 3, true) == StretchBy::NotTheWriter);   // (HOME: the writer was not in the card)
    CHECK(WhoStretchedTheWindow(18400, 5000, 13400, 0, false) == StretchBy::TickCaller); // (no writer thread: Tick's thread closed it)
    CHECK(WhoStretchedTheWindow(9999, 5000, 9999, 9999, true) == StretchBy::None);
    CHECK(WhoStretchedTheWindow(9999, 5000, 0, 0, false) == StretchBy::None);
    CHECK(std::string(StretchedByText(StretchBy::LogWriter)) == "by the log writer");
    CHECK(std::string(StretchedByText(StretchBy::NotTheWriter)) == "not by the log writer");
    CHECK(std::string(StretchedByText(StretchBy::TickCaller)) == "by the caller of Tick (no writer thread)");
    CHECK(std::string(StretchedByText(StretchBy::None)).empty());
    // (D1) the count of the rounds long by the pause alone is a window's like the others: TakeStats gives it and clears it
    (void)TakeStats();
    g_stats.net_pause_rounds = 7;
    g_stats.net_slow_rounds = 2;
    const Stats w = TakeStats();
    CHECK_EQ(w.net_pause_rounds, 7u);
    CHECK_EQ(w.net_slow_rounds, 2u);
    CHECK_EQ(g_stats.net_pause_rounds, 0u);
}

int main() {
    // (the frames of this file are the load of the writer's tests - 1 500 bytes each, so many a second: they are whole, as a study
    //  build writes them; the diet of the capture is Tests/capture_test.cpp's)
    g_debug_whole = true;
    // (nothing may have turned the log on or allocated its ring before the test of level 0)
    TestLevelZeroIsFree();
    TestIniValues();
    TestNoFileAtLevelZero();
    TestRotationWhileSwitchedOff();
    TestCrumbFile();
    TestSwitching();
    TestQueueHasItsRoomFromTheStart();
    TestQueueFullIsCounted();
    TestSwitchOffWhileTheWriterRuns();
    TestRotation();
    TestCardNotReadyYet();
    TestCaptureFileHeader();
    TestDrainIsBounded();
    TestFlushes();
    TestCaptureCadence();
    TestCapturePace();
    TestCaptureLeavesTheCardToTheGame();
    TestCardWindow();
    TestMainLogGoesFirst();
    TestBeaconsAreCounted();
    TestWriterGivesTheCoreUp();
    TestNobodyWaitsForTheWriter();
    TestTheEndOfALaunchIsBounded();
    TestCaptureStopsAtTheGate();
    TestCaptureFileHasTheDietAndTheMarkers();
    TestStatusNumbers();
    TestCpuNumbers();
    TestTakeStats();
    TestLinkSilenceAndStretchedWindows();
    TestCardWait();
    TestWrapLines();
    TestSharedCounters();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
