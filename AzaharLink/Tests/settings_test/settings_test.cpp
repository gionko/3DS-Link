// The 3DS Link app's reading and writing of AzaharLink.ini and of the list of saved rooms (ConfigApp/source/settings.cpp,
// the very file, built on a PC with a stand-in for <3ds.h>), run by Tests/run.sh. The files live in a directory called
// "sdmc:" under a temporary directory, so the app's own paths work unchanged. What it pins: debug is 0 or 1 whatever an old
// file says, menu_key carries "off" and a custom chord unchanged, the switch of the in-game menu writes "off" and brings the
// chord back, keys the app does not know survive; the saved rooms (the rule of the keyboard, most recently used first, no
// copies, at most 10, the file) and the version word of a 3gx header.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "settings.hpp"

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

static void WriteIni(const std::string& text) {
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3DSLink", 0777);
    FILE* f = std::fopen(setup::kIniPath, "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

static std::string ReadIni() {
    std::string s;
    if (FILE* f = std::fopen(setup::kIniPath, "rb")) {
        char buf[1024];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

static bool Has(const std::string& text, const std::string& line) {
    return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

static setup::Settings LoadFrom(const std::string& ini) {
    WriteIni(ini);
    setup::Settings cfg;
    setup::LoadSettings(cfg);
    return cfg;
}

static void TestDebug() {
    std::printf("debug: 0 or 1, an old 2 is 1 and goes back as 1\n");
    {
        setup::Settings d; // no file at all: the defaults
        fs::remove_all("sdmc:");
        setup::LoadSettings(d);
        CHECK(d.debug == 0);
        CHECK(!setup::MenuOn(d)); // the in-game menu is off until the user switches it on
        CHECK(d.menu_key == "off");
    }
    struct Case {
        const char* value;
        int want;
    };
    const Case cases[] = {{"0", 0}, {"1", 1}, {"2", 1}, {"5", 1}, {"-1", 0}, {"-7", 0}, {"abc", 0}, {"", 0}, {"2 ", 1}};
    for (const Case& c : cases) {
        const setup::Settings cfg = LoadFrom(std::string("debug=") + c.value + "\nhost=h\n");
        CHECK(cfg.debug == c.want);
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), std::string("debug=") + (c.want ? "1" : "0")));
        CHECK(ReadIni().find("debug=2") == std::string::npos);
    }
}

static void TestMenuKey() {
    std::printf("menu_key: off carried, a chord kept, empty ignored, the switch writes off and brings the chord back\n");
    // off / none / OFF travel as written, and are off
    for (const char* v : {"off", "OFF", "none", "None"}) {
        setup::Settings cfg = LoadFrom(std::string("menu_key=") + v + "\nhost=h\n");
        CHECK(!setup::MenuOn(cfg));
        CHECK(cfg.menu_key == v);
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), std::string("menu_key=") + v)); // the user's own spelling survives a save
        setup::Settings again;
        setup::LoadSettings(again);
        CHECK(!setup::MenuOn(again) && again.menu_key == v);
        // switching on: there was never a chord in this file, so the default one
        setup::SetMenuOn(cfg, true);
        CHECK(setup::MenuOn(cfg) && cfg.menu_key == "L+R+Select");
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), "menu_key=L+R+Select"));
    }

    // a custom chord is untouched by a load and a save, and survives off and on
    setup::Settings cfg = LoadFrom("menu_key=L+R+Start\nhost=room\n");
    CHECK(setup::MenuOn(cfg) && cfg.menu_key == "L+R+Start");
    CHECK(setup::SaveSettings(cfg));
    CHECK(Has(ReadIni(), "menu_key=L+R+Start"));
    setup::SetMenuOn(cfg, true); // already on: nothing changes
    CHECK(cfg.menu_key == "L+R+Start");
    setup::SetMenuOn(cfg, false);
    CHECK(!setup::MenuOn(cfg) && cfg.menu_key == "off");
    CHECK(setup::SaveSettings(cfg));
    CHECK(Has(ReadIni(), "menu_key=off"));
    CHECK(ReadIni().find("L+R+Start") == std::string::npos);
    setup::SetMenuOn(cfg, false); // already off: still off, the chord is not lost
    CHECK(cfg.menu_key == "off" && cfg.menu_chord == "L+R+Start");
    // the app is closed and opened again with the file saying off: the chord is gone from the file, so the default
    setup::Settings reopened;
    setup::LoadSettings(reopened);
    setup::SetMenuOn(reopened, true);
    CHECK(reopened.menu_key == "L+R+Select");
    // ... but within one run of the app the custom chord comes back
    setup::SetMenuOn(cfg, true);
    CHECK(cfg.menu_key == "L+R+Start");
    CHECK(setup::SaveSettings(cfg));
    CHECK(Has(ReadIni(), "menu_key=L+R+Start"));

    // an empty menu_key is not a value: the default stays, and is what a save writes
    setup::Settings empty = LoadFrom("menu_key=\nhost=h\n");
    CHECK(!setup::MenuOn(empty) && empty.menu_key == "off");
    CHECK(setup::SaveSettings(empty));
    CHECK(Has(ReadIni(), "menu_key=off"));
    // a line in another case or with spaces around the word still reads as off (the plugin's rule)
    CHECK(!setup::MenuOn(LoadFrom("menu_key= Off \n")));
}

static void TestChordText() {
    std::printf("the chord shown in the notes carries the Select that the plugin always adds\n");
    struct Case {
        const char* chord;
        const char* shown;
    };
    const Case cases[] = {{"L+R+Select", "L+R+Select"},     {"L+R+select", "L+R+select"},
                          {"SELECT+A", "SELECT+A"},         {"L R Select", "L R Select"},
                          {"L,R,Select", "L,R,Select"},     {"L+R+Start", "L+R+Start+Select"},
                          {"A", "A+Select"},                {"Selected", "Selected+Select"}};
    for (const Case& c : cases) {
        CHECK(setup::MenuChordText(c.chord) == c.shown);
    }
}

static void TestUnknownKeysAndRoundTrip() {
    std::printf("the other keys: unknown ones survive, a saved file loads back the same\n");
    const std::string ini =
        "enabled=1\nhost=10.0.0.5\nport=24999\nnickname=Gionko\npassword=secret\ntunnel=1\nautoconnect=0\nradio_like=1\n"
        "socket_buffers=1\nnet_thread_core=-1\nmenu_key=L+R+Start\ngame_name=\ndebug=2\nrcvbuf_kb=32\nsleep_stop=1\n";
    setup::Settings cfg = LoadFrom(ini);
    CHECK(cfg.debug == 1 && cfg.menu_key == "L+R+Start" && cfg.host == "10.0.0.5" && cfg.port == 24999);
    CHECK(cfg.unknown_lines.empty()); // rcvbuf_kb and sleep_stop are fields of their own since 0.1.16
    CHECK(cfg.rcvbuf_kb == 32 && cfg.sleep_stop == 1);
    CHECK(setup::SaveSettings(cfg));
    const std::string saved = ReadIni();
    CHECK(Has(saved, "rcvbuf_kb=32") && Has(saved, "sleep_stop=1"));
    CHECK(Has(saved, "debug=1") && Has(saved, "menu_key=L+R+Start") && Has(saved, "autoconnect=0"));
    // the 15 keys, each once
    int lines = 0;
    for (char c : saved) {
        lines += c == '\n';
    }
    CHECK(lines == 15);
    setup::Settings back;
    setup::LoadSettings(back);
    CHECK(back.debug == cfg.debug && back.menu_key == cfg.menu_key && back.host == cfg.host && back.port == cfg.port &&
          back.nickname == cfg.nickname && back.password == cfg.password && back.autoconnect == cfg.autoconnect &&
          back.unknown_lines == cfg.unknown_lines);
}

// ---- 0.1.16: the ini a fresh install, an old install and a hand edit leave behind ------------------------------------

static const char* const kGoldenDefault =
    "enabled=1\nhost=\nport=24872\nnickname=\npassword=\ntunnel=1\nautoconnect=1\nradio_like=1\nsocket_buffers=1\n"
    "net_thread_core=-1\nmenu_key=off\ngame_name=\ndebug=0\nrcvbuf_kb=64\nsleep_stop=0\n";

// The file of PROVA 1 as it was on the card: the 13 keys of 0.1.14, written by the Setup app (LF), and a last line that
// was typed by hand over FTP (no line end).
static const char* const kProva1Ini =
    "enabled=1\nhost=10.139.0.1\nport=24872\nnickname=\npassword=\ntunnel=1\nautoconnect=1\nradio_like=1\n"
    "socket_buffers=1\nnet_thread_core=-1\nmenu_key=L+R+Select\ngame_name=\ndebug=1\nsleep_stop=1";

static void TestFirstRunAndDefaults() {
    std::printf("first run: no file is Missing, the defaults are the ones of the shared header, a save writes all 15 keys\n");
    fs::remove_all("sdmc:");
    fs::create_directory("sdmc:"); // (the card itself: the app creates /3ds/3DSLink)
    setup::Settings d;
    CHECK(setup::LoadSettings(d) == setup::LoadResult::Missing);
    CHECK(!setup::PluginsDirExists()); // no /luma/plugins on this card: the app does not write a first-run file here
    CHECK(d.sleep_stop == azl::ini::kSleepStopDefault && d.rcvbuf_kb == azl::ini::kRcvbufKbDefault);
    CHECK(!setup::HasAllKeys(d));
    CHECK(setup::SaveSettings(d));
    CHECK(fs::is_directory("sdmc:/3ds/3DSLink")); // a save creates the folder of the file (the plugin's folder, /luma/plugins, is Luma's to have)
    CHECK(!setup::PluginsDirExists());
    CHECK(ReadIni() == kGoldenDefault);                       // 177 bytes, LF, the 15 keys in the order of azl::ini::kKeyNames
    CHECK(ReadIni() == azl::ini::Render(azl::ini::Fields())); // the app's defaults are the shared defaults
    CHECK(ReadIni().size() == 177);
    CHECK(ReadIni().find('\r') == std::string::npos);
    setup::Settings back;
    CHECK(setup::LoadSettings(back) == setup::LoadResult::Loaded);
    CHECK(setup::HasAllKeys(back));
    // the keys are exactly the ones the plugin reads, in the order of the table
    const std::string text = ReadIni();
    std::size_t at = 0;
    for (int i = 0; i < azl::ini::kKeyCount; ++i) {
        const std::string want = std::string(azl::ini::kKeyNames[i]) + "=";
        CHECK(text.compare(at, want.size(), want) == 0);
        at = text.find('\n', at) + 1;
    }
    CHECK(at == text.size());
}

static void TestOldFileGainsTheTwoKeys() {
    std::printf("the file of PROVA 1 (13 keys + a hand-typed sleep_stop=1): read as it is, written back complete\n");
    setup::Settings cfg = LoadFrom(kProva1Ini);
    CHECK(cfg.host == "10.139.0.1" && cfg.debug == 1 && cfg.sleep_stop == 1 && cfg.rcvbuf_kb == 64);
    CHECK(cfg.unknown_lines.empty());
    CHECK(!setup::HasAllKeys(cfg)); // rcvbuf_kb is missing
    CHECK(setup::SaveSettings(cfg));
    const std::string saved = ReadIni();
    CHECK(saved.size() == 194);
    CHECK(Has(saved, "rcvbuf_kb=64") && Has(saved, "sleep_stop=1") && Has(saved, "debug=1") && Has(saved, "host=10.139.0.1"));
    CHECK(saved.find("rcvbuf_kb=64\nsleep_stop=1\n") != std::string::npos); // the two new lines last, in the table's order
    setup::Settings back;
    setup::LoadSettings(back);
    CHECK(setup::HasAllKeys(back) && back.sleep_stop == 1 && back.host == cfg.host);
    // a file of 0.1.14 with no sleep_stop at all: the default
    CHECK(LoadFrom("enabled=1\nhost=h\n").sleep_stop == azl::ini::kSleepStopDefault);
}

static void TestSleepRow() {
    std::printf("sleep_stop: the row cycles 0 -> 1 -> 2 -> 0, each value is written, read back, and survives the other rows\n");
    CHECK(setup::NextSleepStop(0) == 1 && setup::NextSleepStop(1) == 2 && setup::NextSleepStop(2) == 0);
    CHECK(setup::NextSleepStop(7) == 0); // (anything else starts over)
    CHECK(std::string(setup::SleepStopText(0)) == "Off");
    CHECK(std::string(setup::SleepStopText(1)) == "Client only");
    CHECK(std::string(setup::SleepStopText(2)) == "Always");
    setup::Settings cfg = LoadFrom(kGoldenDefault);
    CHECK(cfg.sleep_stop == 0);
    for (int step = 0; step < 4; ++step) {
        cfg.sleep_stop = setup::NextSleepStop(cfg.sleep_stop);
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), "sleep_stop=" + std::to_string(cfg.sleep_stop)));
        cfg.radio_like = setup::NextRadioLike(cfg.radio_like); // another row saved afterwards does not lose it
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), "sleep_stop=" + std::to_string(cfg.sleep_stop)));
        setup::Settings back;
        setup::LoadSettings(back);
        CHECK(back.sleep_stop == cfg.sleep_stop && back.radio_like == cfg.radio_like);
    }
    CHECK(cfg.sleep_stop == 1); // (four steps from 0)
    // D-pad left goes back through the same three values, and undoes a step forward
    CHECK(setup::PrevSleepStop(0) == 2 && setup::PrevSleepStop(1) == 0 && setup::PrevSleepStop(2) == 1);
    // radio_like goes round the same way: Off, Broadcast, All (twice); 2 is written and read back as 2
    CHECK(setup::NextRadioLike(0) == 1 && setup::NextRadioLike(1) == 2 && setup::NextRadioLike(2) == 0);
    CHECK(setup::PrevRadioLike(0) == 2 && setup::PrevRadioLike(1) == 0 && setup::PrevRadioLike(2) == 1);
    CHECK(std::string(setup::RadioLikeText(0)) == "Off" && std::string(setup::RadioLikeText(1)) == "Broadcast" &&
          std::string(setup::RadioLikeText(2)) == "All (twice)");
    cfg.radio_like = 2;
    CHECK(setup::SaveSettings(cfg) && Has(ReadIni(), "radio_like=2"));
    {
        setup::Settings back;
        setup::LoadSettings(back);
        CHECK(back.radio_like == 2);
    }
    CHECK(setup::PrevSleepStop(7) == 1); // (anything else is treated like 2)
    for (int v = 0; v < 3; ++v) {
        CHECK(setup::PrevSleepStop(setup::NextSleepStop(v)) == v && setup::NextSleepStop(setup::PrevSleepStop(v)) == v);
    }
    cfg.sleep_stop = 0;
    CHECK(setup::SaveSettings(cfg));
    CHECK(Has(ReadIni(), "sleep_stop=0")); // written as 0, not left out
    // what the file may say: the plugin's rule (azl::ini::SleepStop)
    CHECK(LoadFrom("sleep_stop=2\n").sleep_stop == 2);
    CHECK(LoadFrom("sleep_stop=on\n").sleep_stop == 1);
    CHECK(LoadFrom("sleep_stop=off\n").sleep_stop == 0);
    CHECK(LoadFrom("sleep_stop=\n").sleep_stop == 0);
    CHECK(LoadFrom("sleep_stop=9\n").sleep_stop == 0);
}

static void TestSwitchWords() {
    std::printf("switches: 1/0 and the words; a typo is the key's default, not 'on'\n");
    struct Case {
        const char* value;
        bool autoconnect; // default on
        bool enabled;     // default on
    };
    const Case cases[] = {{"1", true, true},    {"0", false, false},    {"on", true, true},   {"OFF", false, false},
                          {"true", true, true}, {"False", false, false}, {"yes", true, true}, {"no", false, false},
                          {" 1 ", true, true},  {"", true, true},       {"x", true, true},    {"2", true, true},
                          {"-1", true, true}};
    for (const Case& c : cases) {
        const setup::Settings cfg = LoadFrom(std::string("autoconnect=") + c.value + "\nenabled=" + c.value + "\n");
        CHECK(cfg.autoconnect == c.autoconnect);
        CHECK(cfg.enabled == c.enabled);
        // ... and the other two switches read the same way
        const setup::Settings other = LoadFrom(std::string("tunnel=") + c.value + "\nsocket_buffers=" + c.value + "\n");
        CHECK(other.tunnel == c.enabled && other.socket_buffers == c.enabled);
    }
    // radio_like takes the words of a switch for 0 and 1, "2" for 2, and anything else as 1
    struct RadioCase {
        const char* value;
        int want;
    };
    const RadioCase radio_cases[] = {{"0", 0}, {"off", 0}, {"FALSE", 0}, {"no", 0}, {"1", 1}, {"on", 1}, {"yes", 1}, {"2", 2},
                                     {" 2 ", 2}, {"", 1}, {"x", 1}, {"3", 1}, {"-1", 1}, {"02", 1}};
    for (const RadioCase& c : radio_cases) {
        CHECK(azl::ini::ParseRadioLike(c.value) == c.want);
        CHECK(LoadFrom(std::string("radio_like=") + c.value + "\n").radio_like == c.want);
    }
    // net_thread_core: the plugin's range, -1..3 (2 and 3 are the New 3DS cores of the A/B); out of it the default stays
    CHECK(LoadFrom("net_thread_core=3\n").net_core == 3 && LoadFrom("net_thread_core=2\n").net_core == 2);
    CHECK(LoadFrom("net_thread_core=1\n").net_core == 1 && LoadFrom("net_thread_core=-1\n").net_core == -1);
    CHECK(LoadFrom("net_thread_core=4\n").net_core == -1 && LoadFrom("net_thread_core=-2\n").net_core == -1);
    {
        setup::Settings cores = LoadFrom("net_thread_core=3\n");
        CHECK(setup::SaveSettings(cores) && Has(ReadIni(), "net_thread_core=3")); // (a save from the app keeps the A/B key)
    }
}

static void TestRcvbuf() {
    std::printf("rcvbuf_kb: carried as written (the plugin judges it), text that is no number is 64\n");
    struct Case {
        const char* value;
        int want;
    };
    const Case cases[] = {{"64", 64}, {"32", 32}, {"16", 16},   {"8", 8},  {"128", 128},
                          {"0", 0},   {"-5", -5}, {"abc", 64}, {"", 64}, {"32x", 64}};
    for (const Case& c : cases) {
        const setup::Settings cfg = LoadFrom(std::string("rcvbuf_kb=") + c.value + "\n");
        CHECK(cfg.rcvbuf_kb == c.want);
        CHECK(setup::SaveSettings(cfg));
        CHECK(Has(ReadIni(), "rcvbuf_kb=" + std::to_string(c.want)));
    }
}

static void TestHandEdits() {
    std::printf("hand edits: BOM, CRLF, blanks around '=', comments, lines of other versions\n");
    // a Windows editor: BOM at the start, CRLF line ends, spaces around '='
    setup::Settings cfg =
        LoadFrom("\xEF\xBB\xBF" "enabled=1\r\nhost = 10.0.0.9\r\n  sleep_stop = 1 \r\nrcvbuf_kb\t=\t16\r\n");
    CHECK(cfg.enabled && cfg.host == "10.0.0.9" && cfg.sleep_stop == 1 && cfg.rcvbuf_kb == 16);
    CHECK(cfg.unknown_lines.empty());
    CHECK(setup::SaveSettings(cfg));
    CHECK(ReadIni().find('\r') == std::string::npos && ReadIni().find("\xEF\xBB\xBF") == std::string::npos);
    CHECK(Has(ReadIni(), "host=10.0.0.9") && Has(ReadIni(), "sleep_stop=1") && Has(ReadIni(), "rcvbuf_kb=16"));
    // comments and keys of a newer plugin are kept, after the 15 keys
    setup::Settings other = LoadFrom("# my notes\nhost=h\nfuture_key=7\nsleep_stop=1\n");
    CHECK(other.unknown_lines.size() == 2);
    CHECK(setup::SaveSettings(other));
    const std::string saved = ReadIni();
    CHECK(Has(saved, "# my notes") && Has(saved, "future_key=7") && Has(saved, "sleep_stop=1"));
    CHECK(saved.find("sleep_stop=1\n# my notes\nfuture_key=7\n") != std::string::npos); // after the keys, in order
    // a BOM in front of a comment stays out of the line that is kept
    setup::Settings bom_comment = LoadFrom("\xEF\xBB\xBF" "# my notes\nhost=h\n");
    CHECK(bom_comment.unknown_lines.size() == 1 && bom_comment.unknown_lines[0] == "# my notes");
    CHECK(setup::SaveSettings(bom_comment));
    CHECK(ReadIni().find("\xEF\xBB\xBF") == std::string::npos && Has(ReadIni(), "# my notes"));
    // a key twice: the last one wins and it is written once
    setup::Settings twice = LoadFrom("sleep_stop=1\nsleep_stop=0\n");
    CHECK(twice.sleep_stop == 0);
    CHECK(setup::SaveSettings(twice));
    CHECK(ReadIni() == kGoldenDefault);
}

static void TestBounds() {
    std::printf("bounds: never more than the 4096 bytes the plugin reads, newlines in values cannot split a line\n");
    setup::Settings cfg;
    for (int i = 0; i < 40; ++i) {
        cfg.unknown_lines.push_back(std::string(190, 'x') + "=" + std::to_string(i));
    }
    CHECK(setup::SaveSettings(cfg));
    CHECK(ReadIni().size() <= 3800);
    CHECK(Has(ReadIni(), "sleep_stop=0")); // the keys come first: the extras are what is cut
    setup::Settings nl;
    nl.host = "a\nsleep_stop=1";
    CHECK(setup::SaveSettings(nl));
    setup::Settings back;
    setup::LoadSettings(back);
    CHECK(back.sleep_stop == 0); // the newline in the host did not smuggle a key in
}

static std::string Slurp(const std::string& path);

static void TestUnreadableIsNeverWrittenOver() {
    std::printf("a file that cannot be read is not 'missing': the app never completes it over the user's settings\n");
    // a file where the folder should be: fopen fails with ENOTDIR, which is not "there is no such file"
    fs::remove_all("sdmc:");
    fs::create_directories("sdmc:/luma");
    if (FILE* f = std::fopen("sdmc:/luma/plugins", "wb")) {
        std::fclose(f);
    }
    setup::Settings cfg;
    CHECK(setup::LoadSettings(cfg) == setup::LoadResult::Unreadable); // (the old place: a file that cannot be read there is not lost either)
    CHECK(!setup::PluginsDirExists());
    CHECK(!fs::exists(setup::kIniPath));
    // the same in the new place
    fs::remove_all("sdmc:");
    fs::create_directories("sdmc:/3ds");
    if (FILE* f = std::fopen("sdmc:/3ds/3DSLink", "wb")) {
        std::fclose(f);
    }
    CHECK(setup::LoadSettings(cfg) == setup::LoadResult::Unreadable);
    fs::remove_all("sdmc:");
}

// Until 2026-10-19 the app kept the file in sd:/luma/plugins. A file found there, and none in the new place, is moved whole (the plugin
// reads the old place too while the new file is missing, so nothing is lost either way); afterwards the old place is empty and the
// new file is the one the app and the plugin share. A new file wins over an old one left behind.
static void TestOldPlaceIsMoved() {
    std::printf("a settings file in the old place (sd:/luma/plugins) is moved to sd:/3ds/3DSLink, whole, once\n");
    fs::remove_all("sdmc:");
    fs::create_directories("sdmc:/luma/plugins");
    const std::string old_text = std::string(kGoldenDefault).replace(std::string(kGoldenDefault).find("host="), 5, "host=10.0.0.9\nx=");
    {
        FILE* f = std::fopen(setup::kOldIniPath, "wb");
        std::fwrite(old_text.data(), 1, old_text.size(), f);
        std::fclose(f);
    }
    setup::Settings cfg;
    CHECK(setup::LoadSettings(cfg) == setup::LoadResult::Loaded);
    CHECK(cfg.moved_from_old_place);
    CHECK(cfg.host == "10.0.0.9");
    CHECK(fs::exists(setup::kIniPath) && !fs::exists(setup::kOldIniPath));
    CHECK(ReadIni() == old_text); // (moved as it was: a hand edit, an unknown line, nothing re-rendered)
    // the next load reads the new place and moves nothing
    setup::Settings again;
    CHECK(setup::LoadSettings(again) == setup::LoadResult::Loaded);
    CHECK(!again.moved_from_old_place && again.host == "10.0.0.9");
    // an old file left behind next to a new one is left alone: the new one is read
    {
        FILE* f = std::fopen(setup::kOldIniPath, "wb");
        const std::string stale = std::string(kGoldenDefault).replace(std::string(kGoldenDefault).find("host="), 5, "host=stale\nx=");
        std::fwrite(stale.data(), 1, stale.size(), f);
        std::fclose(f);
    }
    setup::Settings third;
    CHECK(setup::LoadSettings(third) == setup::LoadResult::Loaded);
    CHECK(!third.moved_from_old_place && third.host == "10.0.0.9");
    CHECK(fs::exists(setup::kOldIniPath));
    // a save goes to the new place only
    third.host = "10.0.0.10";
    CHECK(setup::SaveSettings(third));
    CHECK(Has(ReadIni(), "host=10.0.0.10"));
    CHECK(Slurp(setup::kOldIniPath).find("host=stale") != std::string::npos);
    fs::remove_all("sdmc:");
}

static std::string Slurp(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[256];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

static void TestTextFiles() {
    std::printf("the files are written whole or not at all, and a write that was cut off does not lose the old file\n");
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
    const std::string file = "sdmc:/a.txt";
    std::string text;
    CHECK(setup::ReadTextFile(file, 1000, text) == setup::LoadResult::Missing && text.empty());
    CHECK(setup::WriteTextFile(file, "first\n") && Slurp(file) == "first\n");
    CHECK(!fs::exists(file + ".tmp") && !fs::exists(file + ".bak")); // (nothing is left behind)
    CHECK(setup::WriteTextFile(file, "second\n") && Slurp(file) == "second\n" && !fs::exists(file + ".tmp") && !fs::exists(file + ".bak"));
    CHECK(setup::ReadTextFile(file, 1000, text) == setup::LoadResult::Loaded && text == "second\n");
    // only about `max` bytes are read (it stops after the chunk that reaches them)
    CHECK(setup::WriteTextFile(file, std::string(5000, 'x')) && setup::ReadTextFile(file, 600, text) == setup::LoadResult::Loaded && text.size() == 1024);
    // cut off between taking the old file aside and putting the new one in its place: the old file is the file
    fs::remove(file);
    if (FILE* f = std::fopen((file + ".bak").c_str(), "wb")) {
        std::fputs("the old one\n", f);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((file + ".tmp").c_str(), "wb")) {
        std::fputs("half of the new", f);
        std::fclose(f);
    }
    CHECK(setup::ReadTextFile(file, 1000, text) == setup::LoadResult::Loaded && text == "the old one\n" && fs::exists(file) && !fs::exists(file + ".bak"));
    CHECK(setup::WriteTextFile(file, "after\n") && Slurp(file) == "after\n" && !fs::exists(file + ".tmp")); // (the stale .tmp does not matter)
    // a folder where the file should be: it cannot be read (it is not "missing"), and it cannot be replaced
    fs::remove(file);
    fs::create_directory(file);
    CHECK(setup::ReadTextFile(file, 1000, text) == setup::LoadResult::Unreadable && text.empty());
    CHECK(!setup::WriteTextFile(file, "x") && fs::is_directory(file) && !fs::exists(file + ".tmp"));
    // a folder that is not there: nothing is written
    CHECK(!setup::WriteTextFile("sdmc:/nowhere/b.txt", "x"));
    fs::remove_all("sdmc:");
}

static setup::Server Srv(const std::string& host, int port = 24872) {
    setup::Server s;
    s.host = host;
    s.port = static_cast<u16>(port);
    return s;
}

static std::string Order(const setup::ServerList& list) {
    std::string s;
    for (const setup::Server& e : list.entries()) {
        s += (s.empty() ? "" : " ") + e.Text();
    }
    return s;
}

static void TestServerParse() {
    std::printf("servers: what the keyboard may hold is 'host' or 'host:port', as the app always checked it\n");
    setup::Server s;
    CHECK(setup::ParseServer("10.0.0.5", s) == setup::ServerParse::Ok && s.host == "10.0.0.5" && s.port == 24872);
    CHECK(setup::ParseServer("  room.example.org:1234 ", s) == setup::ServerParse::Ok && s.host == "room.example.org" && s.port == 1234);
    CHECK(setup::ParseServer("host : 77", s) == setup::ServerParse::Ok && s.host == "host" && s.port == 77);
    CHECK(setup::ParseServer("a:65535", s) == setup::ServerParse::Ok && s.port == 65535);
    CHECK(setup::ParseServer("", s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer("   ", s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer(":24872", s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer("two words", s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer("tab\there", s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer(std::string(81, 'a'), s) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServer(std::string(80, 'a'), s) == setup::ServerParse::Ok);
    CHECK(setup::ParseServer("host|name", s) == setup::ServerParse::BadHost && setup::ParseServer("a|b:99", s) == setup::ServerParse::BadHost); // (the bar ends the host in the file)
    CHECK(setup::ParseServer("h:", s) == setup::ServerParse::BadPort);
    CHECK(setup::ParseServer("h:0", s) == setup::ServerParse::BadPort);
    CHECK(setup::ParseServer("h:65536", s) == setup::ServerParse::BadPort);
    CHECK(setup::ParseServer("h:-3", s) == setup::ServerParse::BadPort);
    CHECK(setup::ParseServer("h:12ab", s) == setup::ServerParse::BadPort);
    s.host = "keep";
    CHECK(setup::ParseServer("bad host", s) == setup::ServerParse::BadHost && s.host == "keep"); // a refusal leaves the result alone
    CHECK(Srv("Room.EXAMPLE.org", 5).Same(Srv("room.example.org", 5)));
    CHECK(!Srv("a", 5).Same(Srv("a", 6)) && !Srv("a").Same(Srv("b")));
    CHECK(Srv("h").Text() == "h:24872" && Srv("h", 9).Text() == "h:9");
}

static void TestServerList() {
    std::printf("servers: most recently used first, none twice, at most 10, Use makes room, Remove\n");
    setup::ServerList list;
    CHECK(list.size() == 0 && !list.full() && list.Serialise().empty());
    CHECK(list.Add(Srv("a")) == setup::ServerList::AddResult::Added);
    CHECK(list.Add(Srv("b", 5)) == setup::ServerList::AddResult::Added);
    CHECK(list.Add(Srv("c")) == setup::ServerList::AddResult::Added);
    CHECK(Order(list) == "c:24872 b:5 a:24872");
    CHECK(list.Add(Srv("A")) == setup::ServerList::AddResult::Moved); // the same room (case apart): moved, not doubled
    CHECK(Order(list) == "A:24872 c:24872 b:5");
    CHECK(list.Add(Srv("b", 6)) == setup::ServerList::AddResult::Added); // another port is another room
    CHECK(list.size() == 4 && list.Find(Srv("B", 5)) == 3 && list.Find(Srv("zzz")) == -1);
    CHECK(list.Serialise() == "b:6\nA\nc\nb:5\n"); // the default port is not written
    CHECK(list.Remove(1)); // "A"
    CHECK(Order(list) == "b:6 c:24872 b:5");
    CHECK(!list.Remove(3) && !list.Remove(99) && list.size() == 3);

    // full list: Add refuses a new room, still moves a known one; Use gives up the last one
    setup::ServerList full;
    for (int i = 0; i < 10; ++i) {
        CHECK(full.Add(Srv("h" + std::to_string(i))) == setup::ServerList::AddResult::Added);
    }
    CHECK(full.full() && full.size() == 10 && setup::ServerList::kMax == 10);
    CHECK(Order(full).substr(0, 8) == "h9:24872"); // h0 is the oldest, last
    CHECK(full.Add(Srv("new")) == setup::ServerList::AddResult::Full && full.size() == 10 && full.Find(Srv("new")) < 0);
    CHECK(full.Add(Srv("h0")) == setup::ServerList::AddResult::Moved && full.Find(Srv("h0")) == 0 && full.size() == 10);
    CHECK(!full.Use(Srv("h0"))); // already on top: nothing changed
    CHECK(full.Use(Srv("h5")) && full.Find(Srv("h5")) == 0 && full.size() == 10);
    CHECK(full.Use(Srv("new")) && full.Find(Srv("new")) == 0 && full.size() == 10);
    CHECK(full.Find(Srv("h1")) < 0 && full.Find(Srv("h2")) == 9); // h1 was the least recently used: it made room
    CHECK(full.Remove(0) && !full.full() && full.Add(Srv("again")) == setup::ServerList::AddResult::Added);

    setup::ServerList small;
    CHECK(small.Use(Srv("x")) && small.size() == 1 && small.Use(Srv("y")) && Order(small) == "y:24872 x:24872");
    // the page hands over one of the list's own entries (picking a room): the list is rearranged under it
    CHECK(small.Add(Srv("z")) == setup::ServerList::AddResult::Added && Order(small) == "z:24872 y:24872 x:24872");
    CHECK(small.Add(small.entries()[2]) == setup::ServerList::AddResult::Moved && Order(small) == "x:24872 z:24872 y:24872");
    CHECK(small.Use(small.entries()[2]) && Order(small) == "y:24872 x:24872 z:24872");
    CHECK(!small.Use(small.entries()[0]) && Order(small) == "y:24872 x:24872 z:24872");

    // a room used again by a caller that knows only its address (the connection test) keeps the name and the password the list had; a password
    // that is given replaces the old one
    setup::ServerList keep;
    setup::Server full_room = Srv("room.example");
    full_room.name = "Casa";
    full_room.password = "secret";
    CHECK(keep.Add(full_room) == setup::ServerList::AddResult::Added);
    CHECK(keep.Add(Srv("ROOM.example")) == setup::ServerList::AddResult::Moved);
    CHECK(keep.entries()[0].name == "Casa" && keep.entries()[0].password == "secret");
    CHECK(keep.Use(Srv("room.example")) == false && keep.entries()[0].password == "secret"); // (already on top: untouched)
    setup::Server changed = Srv("room.example");
    changed.password = "other";
    CHECK(keep.Add(changed) == setup::ServerList::AddResult::Moved && keep.entries()[0].password == "other" && keep.entries()[0].name == "Casa");
}

static void TestServerFile() {
    std::printf("servers: the file is plain text; bad lines, copies and the 11th line are dropped; a missing file is not an error\n");
    // text -> list
    const setup::ServerList parsed = setup::ServerList::Parse("\xEF\xBB\xBF" "one\r\ntwo:99\r\n\r\nbad host\nONE\ntwo:99\nthree:x\n  four : 5  \n");
    CHECK(Order(parsed) == "one:24872 two:99 four:5");
    std::string many;
    for (int i = 0; i < 14; ++i) {
        many += "h" + std::to_string(i) + "\n";
    }
    const setup::ServerList capped = setup::ServerList::Parse(many);
    CHECK(capped.size() == 10 && capped.entries()[0].host == "h0" && capped.entries()[9].host == "h9"); // the first lines are the newest
    CHECK(setup::ServerList::Parse("").size() == 0 && setup::ServerList::Parse("\n\n").size() == 0);
    CHECK(setup::ServerList::Parse(parsed.Serialise()).Serialise() == parsed.Serialise()); // round trip

    // on disk: the file is wherever the app says (here the data folder of an app started from sdmc:/3ds)
    const std::string file = "sdmc:/3ds/3DSLink/servers.txt";
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    setup::ServerList list;
    CHECK(setup::LoadServers(file, list) == setup::LoadResult::Missing); // no file (not even its folder): the app seeds the list
    CHECK(setup::SaveServers(file, parsed)); // it makes the data folder
    CHECK(fs::exists(file));
    CHECK(setup::LoadServers(file, list) == setup::LoadResult::Loaded && Order(list) == Order(parsed));
    list.Use(Srv("fresh", 7));
    CHECK(setup::SaveServers(file, list));
    setup::ServerList again;
    CHECK(setup::LoadServers(file, again) == setup::LoadResult::Loaded && Order(again) == "fresh:7 one:24872 two:99 four:5");
    CHECK(setup::SaveServers(file, setup::ServerList())); // an empty list is an empty file, which loads as an empty list
    CHECK(setup::LoadServers(file, again) == setup::LoadResult::Loaded && again.size() == 0);
    // another folder, another file: nothing is fixed in the code
    CHECK(setup::SaveServers("sdmc:/3ds/other.txt", parsed) && setup::LoadServers("sdmc:/3ds/other.txt", again) == setup::LoadResult::Loaded &&
          Order(again) == Order(parsed));
    // a file where the folder should be: not "missing", so never written over by the seed
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    if (FILE* f = std::fopen("sdmc:/3ds/3DSLink", "wb")) {
        std::fclose(f);
    }
    CHECK(setup::LoadServers(file, again) == setup::LoadResult::Unreadable);
    fs::remove_all("sdmc:");
}

static std::string NamesOf(const setup::ServerList& list) {
    std::string s;
    bool first = true;
    for (const setup::Server& e : list.entries()) {
        s += (first ? "" : "|") + e.name;
        first = false;
    }
    return s;
}

static void TestServerNames() {
    std::printf("servers: every room has a name (12 characters at most) and an address; old lines read as rooms without a name\n");
    // the name: up to 12 characters of A-Z a-z 0-9 space . _ - (empty = none)
    CHECK(setup::kMaxServerNameChars == 12);
    CHECK(setup::ValidServerName("") && setup::ValidServerName("Home PC") && setup::ValidServerName("a.b_c-d 9") && setup::ValidServerName("123456789012"));
    CHECK(!setup::ValidServerName("1234567890123")); // 13
    CHECK(!setup::ValidServerName("pipe|name") && !setup::ValidServerName("colon:x") && !setup::ValidServerName("tab\tx") &&
          !setup::ValidServerName("caf\xC3\xA9") && !setup::ValidServerName("new\nline"));
    setup::Server named = Srv("192.168.1.20", 24999);
    named.name = "Home PC";
    CHECK(named.Label() == "Home PC" && named.Text() == "192.168.1.20:24999");
    CHECK(Srv("h", 5).Label() == "h:5"); // a room without a name is called by its address
    CHECK(named.Same(Srv("192.168.1.20", 24999)) && Srv("x").Same(named) == false); // the name does not make a room another one

    // the file: "host", "host:port", and "|name" after it when there is one
    setup::ServerList list;
    setup::Server a = Srv("room.example.org");
    a.name = "Friends";
    setup::Server b = Srv("10.0.0.5", 24999);
    b.name = "Lan 2";
    list.Add(Srv("plain"));
    list.Add(b);
    list.Add(a);
    CHECK(list.Serialise() == "room.example.org|Friends\n10.0.0.5:24999|Lan 2\nplain\n");
    const setup::ServerList back = setup::ServerList::Parse(list.Serialise());
    CHECK(Order(back) == Order(list) && NamesOf(back) == "Friends|Lan 2|");
    // a file of the earlier builds (no names) is read as it was, and a name can be added by hand
    const setup::ServerList old = setup::ServerList::Parse("one\ntwo:99\nthree:24872|Mine\n");
    CHECK(Order(old) == "one:24872 two:99 three:24872" && NamesOf(old) == "||Mine");
    CHECK(old.Serialise() == "one\ntwo:99\nthree|Mine\n");
    // a name that is not valid is left out, the room stays; spaces around the parts do not count
    const setup::ServerList odd = setup::ServerList::Parse("h1|123456789012345\nh2|a|b\nh3 | Spaced \nh4|\nh5|caf\xC3\xA9\n");
    CHECK(Order(odd) == "h1:24872 h2:24872 h3:24872 h4:24872 h5:24872" && NamesOf(odd) == "|a|Spaced||"); // ("h2|a|b" is a name and a password since 0.1.17)
    // the password of a room: after a second bar, as typed (it may hold a bar and blanks), written back only when there is one
    const setup::ServerList pw = setup::ServerList::Parse("r1:99|Home|s3cret pass\nr2||only|bars|here\nr3|Plain\nr4\nr5|Name|\n");
    CHECK(Order(pw) == "r1:99 r2:24872 r3:24872 r4:24872 r5:24872");
    CHECK(pw.entries()[0].password == "s3cret pass" && pw.entries()[0].name == "Home");
    CHECK(pw.entries()[1].password == "only|bars|here" && pw.entries()[1].name.empty());
    CHECK(pw.entries()[2].password.empty() && pw.entries()[3].password.empty() && pw.entries()[4].password.empty());
    CHECK(pw.Serialise() == "r1:99|Home|s3cret pass\nr2||only|bars|here\nr3|Plain\nr4\nr5|Name\n");
    CHECK(setup::ServerList::Parse(pw.Serialise()).Serialise() == pw.Serialise()); // round trip
    CHECK(setup::ServerList::Parse("h|n|x\r\n").entries()[0].password == "x"); // (a line that ends with CR LF)
    {   // the same room (host and port) is the same whatever its password; Replace puts a room in the place of another
        setup::ServerList list2 = setup::ServerList::Parse("a:1|A|pa\nb:2|B|pb\nc:3\n");
        setup::Server edited = list2.entries()[1];
        edited.password.clear(); // (a password can be taken away)
        edited.name = "Bee";
        CHECK(list2.Replace(1, edited) == setup::ServerList::ReplaceResult::Ok && list2.Serialise() == "a:1|A|pa\nb:2|Bee\nc:3\n");
        edited.host = "c";
        edited.port = 3; // (the address of another room)
        CHECK(list2.Replace(1, edited) == setup::ServerList::ReplaceResult::Duplicate && list2.Serialise() == "a:1|A|pa\nb:2|Bee\nc:3\n");
        edited.host = "z";
        edited.port = 9;
        CHECK(list2.Replace(1, edited) == setup::ServerList::ReplaceResult::Ok && list2.entries()[1].host == "z" && list2.entries()[0].host == "a");
        CHECK(list2.Replace(7, edited) == setup::ServerList::ReplaceResult::Missing);
        // using a room (it goes to the top) brings its password with it
        CHECK(list2.Use(list2.entries()[0]) == false && list2.Use(setup::ServerList::Parse("c:3|C|pc\n").entries()[0]) && list2.entries()[0].password == "pc");
    }
    setup::Server line;
    CHECK(setup::ParseServerLine("1.2.3.4:55|Name", line) == setup::ServerParse::Ok && line.host == "1.2.3.4" && line.port == 55 && line.name == "Name");
    CHECK(setup::ParseServerLine("bad host|Name", line) == setup::ServerParse::BadHost);
    CHECK(setup::ParseServerLine("h:0|Name", line) == setup::ServerParse::BadPort);
    CHECK(setup::ParseServerLine("plain", line) == setup::ServerParse::Ok && line.name.empty()); // (a name from before is not kept)

    // the same room again: it moves up and takes a new name, but keeps its own when the new one has none (using a room tested by its address)
    setup::ServerList moves;
    moves.Add(a);
    moves.Add(Srv("other"));
    CHECK(moves.Add(Srv("ROOM.example.org")) == setup::ServerList::AddResult::Moved && NamesOf(moves) == "Friends|");
    setup::Server renamed = Srv("room.example.org");
    renamed.name = "New name";
    CHECK(moves.Add(renamed) == setup::ServerList::AddResult::Moved && NamesOf(moves) == "New name|");
    CHECK(moves.Use(Srv("other")) && moves.Use(Srv("room.example.org")) && NamesOf(moves) == "New name|");

    // on disk, names and all
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    CHECK(setup::SaveServers("sdmc:/3ds/3DSLink/servers.txt", list));
    setup::ServerList loaded;
    CHECK(setup::LoadServers("sdmc:/3ds/3DSLink/servers.txt", loaded) == setup::LoadResult::Loaded && NamesOf(loaded) == "Friends|Lan 2|");
    fs::remove_all("sdmc:");
}

static void TestPluginVersionText() {
    std::printf("3gx header: the version word is major << 24 | minor << 16 | revision << 8\n");
    CHECK(setup::PluginVersionText(0x00011100) == "0.1.17"); // the word of 0.1.17 in the README
    CHECK(setup::PluginVersionText(0x00010f00) == "0.1.15");
    CHECK(setup::PluginVersionText(0x01020300) == "1.2.3");
    CHECK(setup::PluginVersionText(0) == "0.0.0");
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_settings_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    TestDebug();
    TestMenuKey();
    TestChordText();
    TestUnknownKeysAndRoundTrip();
    TestFirstRunAndDefaults();
    TestOldFileGainsTheTwoKeys();
    TestSleepRow();
    TestSwitchWords();
    TestRcvbuf();
    TestHandEdits();
    TestBounds();
    TestUnreadableIsNeverWrittenOver();
    TestOldPlaceIsMoved();
    TestTextFiles();
    TestServerParse();
    TestServerList();
    TestServerFile();
    TestServerNames();
    TestPluginVersionText();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
