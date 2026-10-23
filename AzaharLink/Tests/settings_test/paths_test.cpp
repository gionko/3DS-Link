// Where the 3DS Link app's files are, and how it finds them (ConfigApp/source/paths.cpp, appcfg.cpp and search.cpp, the very files, built on a PC
// with a stand-in for <3ds.h>), run by Tests/run.sh. The files live in a directory called "sdmc:" under a temporary directory, so the app's own
// paths work unchanged. What it pins: the folder the app was started from, worked out from argv[0] in every form a launcher gives (and what an
// emulator gives: nothing); every file of the app derived from it, none from a fixed folder; the data folder, the first-start state (the app's
// config file); the config file; the order in which boot9.bin is looked for and what counts as one; the files of earlier builds brought over
// once; the search of the whole card (breadth first, what it never enters, a hit that is no good, the budget, cancel) and the folder listing.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

#include "appcfg.hpp"
#include "games.hpp"
#include "paths.hpp"
#include "search.hpp"
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

static void Put(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

static std::string Get(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[1024];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

static bool Exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void Fresh() {
    fs::remove_all("sdmc:");
    fs::create_directories("sdmc:");
}

static std::string Boot9(std::size_t size = 0x10000) {
    return std::string(size, 'k');
}

// ---------------------------------------------------------------------------------------------

static void TestAppDirFromArgv0() {
    std::printf("the folder the app was started from: every form of argv[0]\n");
    struct Case {
        const char* argv0;
        const char* want;
    };
    const Case cases[] = {
        {"sdmc:/3ds/3DSLink.3dsx", "sdmc:/3ds"},                                          // the Homebrew Launcher
        {"/3ds/3DSLink.3dsx", "sdmc:/3ds"},                                               // a leading '/' is the SD card
        {"sdmc:/3DSLink.3dsx", "sdmc:"},                                                  // the root
        {"/3DSLink.3dsx", "sdmc:"},
        {"sdmc:/games/homebrew/3dslink/3DSLink.3dsx", "sdmc:/games/homebrew/3dslink"},    // any folder
        {"sdmc:/my apps/3DS Link/3DSLink.3dsx", "sdmc:/my apps/3DS Link"},                // blanks are part of a name
        {"sdmc://3ds//3DSLink.3dsx", "sdmc:/3ds"},                                        // doubled slashes
        {"sdmc:/a/./b/x.3dsx", "sdmc:/a/b"},
        {"sdmc:/a/b/../c/x.3dsx", "sdmc:/a/c"},
        {"sdmc:/../x.3dsx", "sdmc:"},                                                     // '..' never leaves the card
        {"sdmc:\\3ds\\3DSLink.3dsx", "sdmc:/3ds"},                                        // a launcher that writes backslashes
        {"sdmc:3ds/3DSLink.3dsx", "sdmc:/3ds"},                                           // no slash after the device
        {"sdmc:/3ds/", "sdmc:/3ds"},                                                      // a path that names a folder is that folder
        {"sdmc:", "sdmc:"},
        {"/", "sdmc:"},
        {"sdmc:/3ds/3DSLink", "sdmc:/3ds"},                                               // (no extension: a file all the same)
        {"", "sdmc:/3ds/3DSLink"},                                                         // an app installed as a CIA, an emulator: no argv
        {"3dslink:/3DSLink.3dsx", "sdmc:/3ds/3DSLink"},                                           // netloader: not on the card
        {"romfs:/x.3dsx", "sdmc:/3ds/3DSLink"},
        {"3DSLink.3dsx", "sdmc:/3ds/3DSLink"},                                                    // a bare name says nothing
        {"folder/3DSLink.3dsx", "sdmc:/3ds/3DSLink"},
    };
    for (const Case& c : cases) {
        const std::string got = setup::AppDirFromArgv0(c.argv0);
        if (got != c.want) {
            std::fprintf(stderr, "  argv0 '%s': got '%s', want '%s'\n", c.argv0, got.c_str(), c.want);
        }
        CHECK(got == c.want);
    }
    CHECK(setup::AppDirFromArgv0(nullptr) == "sdmc:/3ds/3DSLink");
    CHECK(std::strcmp(setup::kDefaultAppDir, "sdmc:/3ds/3DSLink") == 0);
}

static void TestPathBuilding() {
    std::printf("path building: join, parent, base name, every file of the app from the app's folder\n");
    CHECK(setup::JoinPath("sdmc:/3ds", "x") == "sdmc:/3ds/x");
    CHECK(setup::JoinPath("sdmc:", "x") == "sdmc:/x");
    CHECK(setup::JoinPath("sdmc:/3ds/", "x") == "sdmc:/3ds/x");
    CHECK(setup::JoinPath("", "x") == "x" && setup::JoinPath("sdmc:/a", "") == "sdmc:/a");
    CHECK(setup::ParentDir("sdmc:/a/b") == "sdmc:/a" && setup::ParentDir("sdmc:/a") == "sdmc:" && setup::ParentDir("sdmc:").empty());
    CHECK(setup::ParentDir("sdmc:/a/b/") == "sdmc:/a" && setup::ParentDir("noslash").empty());
    CHECK(setup::BaseName("sdmc:/a/b.bin") == "b.bin" && setup::BaseName("sdmc:/a") == "a" && setup::BaseName("sdmc:").empty());
    CHECK(setup::BaseName("sdmc:/a/b/") == "b");

    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/3ds");
    CHECK(p.appdir == "sdmc:/3ds");
    CHECK(p.plugin_file == "sdmc:/3ds/AzaharLink.3gx");
    CHECK(p.boot9_beside == "sdmc:/3ds/boot9.bin");
    CHECK(p.boot9_godmode == "sdmc:/gm9/out/boot9.bin");
    CHECK(p.datadir == "sdmc:/3ds/3DSLink");
    CHECK(p.config_file == "sdmc:/3ds/3DSLink/3DSLink.cfg");
    CHECK(p.servers_file == "sdmc:/3ds/3DSLink/servers.txt");
    CHECK(p.games_cache == "sdmc:/3ds/3DSLink/titles.cache");
    CHECK(p.catalog_cache == "sdmc:/3ds/3DSLink/catalog.cache");
    CHECK(p.park_dir == "sdmc:/3ds/3DSLink/park");
    CHECK(p.delivery_log == "sdmc:/3ds/3DSLink/3DSLink-TitleDelivery.log");
    CHECK(p.dlp_dir == "sdmc:/3ds/3DSLink/dlp" && p.dlp_child == "sdmc:/3ds/3DSLink/dlp/child.cia"); // 3DS Link Delivery's child
    CHECK(p.dlp_app_cia == "sdmc:/3ds/3DSLink/dlp/3dslink.cia"); // the app's own CIA, the composer's first source (the installer of the child leaves it there)
    CHECK(p.dlp_marker == "sdmc:/3ds/3DSLink/dlp/child.preparing"); // there while the child is built

    // the app's own folder: the folder 3DSLink.3dsx is in when that is called 3DSLink (one folder, no 3DSLink inside 3DSLink), also when
    // there is no 3dsx at all (the app installed as a CIA)
    const setup::Paths own = setup::Paths::FromAppDir(setup::AppDirFromArgv0("sdmc:/3ds/3DSLink/3DSLink.3dsx"));
    CHECK(own.appdir == "sdmc:/3ds/3DSLink" && own.datadir == "sdmc:/3ds/3DSLink" && own.config_file == "sdmc:/3ds/3DSLink/3DSLink.cfg");
    CHECK(own.plugin_beside == "sdmc:/3ds/3DSLink/AzaharLink.3gx" && own.plugin_in_data == own.plugin_beside && own.plugin_file == own.plugin_beside);
    CHECK(own.boot9_beside == "sdmc:/3ds/3DSLink/boot9.bin" && own.boot9_in_data == own.boot9_beside);
    const setup::Paths cia = setup::Paths::FromAppDir(setup::AppDirFromArgv0(nullptr));
    CHECK(cia.appdir == "sdmc:/3ds/3DSLink" && cia.datadir == "sdmc:/3ds/3DSLink" && cia.plugin_file == "sdmc:/3ds/3DSLink/AzaharLink.3gx");
    CHECK(setup::Paths::FromAppDir("sdmc:/games/3dslink").datadir == "sdmc:/games/3dslink"); // (upper and lower case alike)
    CHECK(setup::Paths::FromAppDir("sdmc:/games/3DS Link").datadir == "sdmc:/games/3DS Link/3DSLink"); // (another name: a folder is made inside)
    CHECK(p.plugin_beside == "sdmc:/3ds/AzaharLink.3gx" && p.plugin_in_data == "sdmc:/3ds/3DSLink/AzaharLink.3gx");
    CHECK(p.boot9_in_data == "sdmc:/3ds/3DSLink/boot9.bin");
    CHECK(setup::SameNameNoCase("3DSLink", "3dslink") && !setup::SameNameNoCase("3DSLink", "3DSLinks") && !setup::SameNameNoCase("a", "b"));

    // another folder: every file follows, none stays behind in sdmc:/3ds
    const setup::Paths q = setup::Paths::FromAppDir(setup::AppDirFromArgv0("sdmc:/games/Link tools/3DSLink.3dsx"));
    const std::string all[] = {q.appdir, q.plugin_file, q.plugin_in_data, q.boot9_beside, q.boot9_in_data, q.datadir, q.config_file, q.servers_file, q.games_cache, q.catalog_cache, q.park_dir, q.delivery_log, q.dlp_dir, q.dlp_child, q.dlp_app_cia, q.dlp_marker};
    for (const std::string& path : all) {
        CHECK(path.compare(0, std::string("sdmc:/games/Link tools").size(), "sdmc:/games/Link tools") == 0);
        CHECK(path.find("/3ds/") == std::string::npos && (path.size() < 4 || path.compare(path.size() - 4, 4, "/3ds") != 0)); // (a folder 3ds; the file 3dslink.cia is not one)
    }
    CHECK(q.plugin_file == "sdmc:/games/Link tools/AzaharLink.3gx" && q.datadir == "sdmc:/games/Link tools/3DSLink");
    CHECK(q.boot9_godmode == "sdmc:/gm9/out/boot9.bin"); // (the one place that is someone else's)

    // the root of the card
    const setup::Paths r = setup::Paths::FromAppDir(setup::AppDirFromArgv0("sdmc:/3DSLink.3dsx"));
    CHECK(r.appdir == "sdmc:" && r.plugin_file == "sdmc:/AzaharLink.3gx" && r.datadir == "sdmc:/3DSLink" && r.config_file == "sdmc:/3DSLink/3DSLink.cfg");

    // nothing the user can read names the old experiment
    for (const setup::Paths* paths : {&p, &q, &r}) {
        for (const std::string& path : {paths->appdir, paths->plugin_file, paths->boot9_beside, paths->boot9_godmode, paths->datadir, paths->config_file,
                                        paths->servers_file, paths->games_cache, paths->catalog_cache, paths->park_dir, paths->delivery_log, paths->dlp_dir, paths->dlp_child, paths->dlp_app_cia, paths->dlp_marker}) {
            CHECK(path.find("TitleShare") == std::string::npos);
        }
    }

    // the places that are not the app's to choose stay as they are
    CHECK(std::strcmp(setup::kPluginsDir, "sdmc:/luma/plugins") == 0 && std::strcmp(setup::kIniPath, "sdmc:/3ds/3DSLink/AzaharLink.ini") == 0);
    CHECK(std::strcmp(setup::kOldIniPath, "sdmc:/luma/plugins/AzaharLink.ini") == 0 && std::strcmp(setup::kPluginDataDir, "sdmc:/3ds/3DSLink") == 0);
    CHECK(setup::GamePluginPath(0x0004000000177000ull) == "sdmc:/luma/plugins/0004000000177000/AzaharLink.3gx");

    // the current paths are the ones the app set
    setup::SetCurrentPaths(q);
    CHECK(setup::CurrentPaths().datadir == q.datadir);
    setup::SetCurrentPaths(p);
}

static void TestMakeDirsAndFirstStart() {
    std::printf("the data folder is made on demand; the first start is 'no config file in it'\n");
    Fresh();
    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/games/Link tools");
    CHECK(setup::IsFirstStart(p)); // nothing there at all
    CHECK(!Exists("sdmc:/games")); // asking made nothing
    CHECK(setup::MakeDirs(p.datadir));
    CHECK(fs::is_directory("sdmc:/games/Link tools/3DSLink"));
    CHECK(setup::MakeDirs(p.datadir)); // again: fine
    CHECK(setup::IsFirstStart(p)); // the folder alone does not end the first start
    setup::AppConfig cfg;
    cfg.first_start_done = true;
    CHECK(setup::SaveAppConfig(p.config_file, cfg));
    CHECK(!setup::IsFirstStart(p));
    CHECK(setup::MakeDirs("sdmc:")); // the root is there
    Put("sdmc:/blocker", "a file");
    CHECK(!setup::MakeDirs("sdmc:/blocker/sub")); // a file in the way
    CHECK(!setup::MakeDirs("sdmc:/blocker"));     // is not a folder
    Fresh();
    // the config file's folder is a file: not a first start either way round, but not readable
    CHECK(setup::IsFirstStart(p));
}

static void TestAppConfig() {
    std::printf("3DSLink.cfg: first start done, the boot9.bin the user chose\n");
    Fresh();
    const std::string path = "sdmc:/3DSLink.cfg";
    setup::AppConfig cfg;
    CHECK(!cfg.first_start_done && cfg.boot9_path.empty());
    CHECK(!setup::LoadAppConfig(path, cfg)); // no file
    cfg.first_start_done = true;
    cfg.boot9_path = "sdmc:/some folder/boot9.bin";
    CHECK(setup::SaveAppConfig(path, cfg));
    CHECK(!Exists(path + ".tmp"));
    setup::AppConfig back;
    CHECK(setup::LoadAppConfig(path, back) && back.first_start_done && back.boot9_path == "sdmc:/some folder/boot9.bin");
    // a path with '=' in it, comments, unknown keys, CRLF, blanks
    const setup::AppConfig odd = setup::ParseAppConfig("# note\r\nfirst_start_done = 1 \r\nboot9=sdmc:/a=b/boot9.bin\r\nfuture_key=7\r\n\r\nnonsense\r\n");
    CHECK(odd.first_start_done && odd.boot9_path == "sdmc:/a=b/boot9.bin");
    CHECK(!setup::ParseAppConfig("first_start_done=0\n").first_start_done && !setup::ParseAppConfig("").first_start_done);
    CHECK(setup::ParseAppConfig(setup::SerialiseAppConfig(cfg)).boot9_path == cfg.boot9_path);
    setup::AppConfig none;
    none.first_start_done = true;
    CHECK(setup::SerialiseAppConfig(none).find("boot9") == std::string::npos); // no path chosen: no line
    CHECK(setup::SerialiseAppConfig(none).find("plugin_stamp") == std::string::npos); // the copies never looked at: no line
    // the stamp of the plugin file the copies were brought up to
    cfg.plugin_stamp = "1000875:1760000000";
    CHECK(setup::ParseAppConfig(setup::SerialiseAppConfig(cfg)).plugin_stamp == "1000875:1760000000");
    CHECK(setup::ParseAppConfig("plugin_stamp = 12:34 \n").plugin_stamp == "12:34");
    CHECK(setup::SaveAppConfig(path, cfg) && setup::LoadAppConfig(path, back) && back.plugin_stamp == "1000875:1760000000");
    cfg.plugin_stamp.clear();
    // rewritten whole
    cfg.boot9_path.clear();
    CHECK(setup::SaveAppConfig(path, cfg) && setup::LoadAppConfig(path, back) && back.boot9_path.empty() && back.first_start_done);
    // a folder that is not there: nothing is written
    CHECK(!setup::SaveAppConfig("sdmc:/nodir/3DSLink.cfg", cfg));
}

static void TestDlpDeliverySwitch() {
    std::printf("3DSLink.cfg: the emergency switch of 3DS Link Delivery (dlp_delivery): on unless the file says off, a slip is on\n");
    Fresh();
    const std::string path = "sdmc:/3DSLink.cfg";
    // on by default: a new config, an empty file, and every file an older app wrote (it has no such line)
    CHECK(setup::AppConfig().dlp_delivery);
    CHECK(setup::ParseAppConfig("").dlp_delivery);
    CHECK(setup::ParseAppConfig("first_start_done=1\nboot9=sdmc:/x/boot9.bin\nplugin_stamp=1:2\n").dlp_delivery);
    // the words of a switch, as the plugin's file knows them (any case, blanks and the CR of a CRLF file around them)
    for (const char* word : {"0", "off", "false", "no", "OFF", " No ", "False", "off\r"}) {
        CHECK(!setup::ParseAppConfig(std::string("dlp_delivery=") + word + "\n").dlp_delivery);
    }
    for (const char* word : {"1", "on", "true", "yes", "ON", " Yes ", "TRUE"}) {
        CHECK(setup::ParseAppConfig(std::string("dlp_delivery=") + word + "\n").dlp_delivery);
    }
    // a value the app cannot read is the default, ON: a typo must not shut the feature (and a damaged file must not leave it shut either)
    for (const char* word : {"", "2", "-1", "maybe", "of", "off off", "0x0", "nul", "\x01\x02\x03", "n0"}) {
        CHECK(setup::ParseAppConfig(std::string("dlp_delivery=") + word + "\n").dlp_delivery);
    }
    // the line is looked for by its key alone: another key that contains it, a comment and a line with no value do not count
    CHECK(setup::ParseAppConfig("my_dlp_delivery=0\n").dlp_delivery);
    CHECK(setup::ParseAppConfig("# dlp_delivery=0\n").dlp_delivery);
    CHECK(setup::ParseAppConfig("dlp_delivery\n").dlp_delivery);
    CHECK(!setup::ParseAppConfig("  dlp_delivery = 0 \r\n").dlp_delivery);
    // a file that is damaged in other lines still says what its own line says
    CHECK(!setup::ParseAppConfig("garbage \x01\n\n=\ndlp_delivery=off\nmore garbage").dlp_delivery);
    CHECK(setup::ParseAppConfig("\xFF\xFE\x01 not text at all").dlp_delivery);
    // written every time, so that a hand edit on a PC only flips the digit
    setup::AppConfig cfg;
    cfg.first_start_done = true;
    CHECK(setup::SerialiseAppConfig(cfg).find("dlp_delivery=1\n") != std::string::npos);
    cfg.dlp_delivery = false;
    CHECK(setup::SerialiseAppConfig(cfg).find("dlp_delivery=0\n") != std::string::npos);
    CHECK(!setup::ParseAppConfig(setup::SerialiseAppConfig(cfg)).dlp_delivery);
    // through the file, and the rest of the file is not disturbed by it
    cfg.boot9_path = "sdmc:/mine/boot9.bin";
    CHECK(setup::SaveAppConfig(path, cfg));
    setup::AppConfig back;
    CHECK(setup::LoadAppConfig(path, back) && !back.dlp_delivery && back.first_start_done && back.boot9_path == "sdmc:/mine/boot9.bin");
    cfg.dlp_delivery = true;
    CHECK(setup::SaveAppConfig(path, cfg) && setup::LoadAppConfig(path, back) && back.dlp_delivery && back.boot9_path == "sdmc:/mine/boot9.bin");
    // the hand edit: the digit flipped in the file as the app wrote it
    std::string text = Get(path);
    const std::size_t at = text.find("dlp_delivery=1");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        text[at + 13] = '0';
        Put(path, text);
        CHECK(setup::LoadAppConfig(path, back) && !back.dlp_delivery && back.first_start_done);
    }
    // a file that cannot be read leaves the config as it was: the app starts on the default, ON (and the Settings page can switch it off)
    setup::AppConfig untouched;
    CHECK(!setup::LoadAppConfig("sdmc:/nothing here.cfg", untouched) && untouched.dlp_delivery);
    // the switch lives in the app's own file: the plugin's ini (15 keys, pinned) is another file, in another place
    CHECK(std::string(setup::kConfigFileName) == "3DSLink.cfg" && std::string(setup::kIniPath).find(setup::kConfigFileName) == std::string::npos);
}

static void TestBoot9Lookup() {
    std::printf("boot9.bin: next to the app, then GodMode9's folder, then where the user pointed to; 64 KB or 32 KB\n");
    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/games/Link tools");
    CHECK(setup::Boot9SizeOk(0x10000) && setup::Boot9SizeOk(0x8000));
    CHECK(!setup::Boot9SizeOk(0) && !setup::Boot9SizeOk(0x10001) && !setup::Boot9SizeOk(0x7FFF) && !setup::Boot9SizeOk(0x20000));
    // the candidates, in order
    const std::vector<std::string> list = setup::Boot9Candidates(p, "sdmc:/mine/boot9.bin");
    CHECK(list.size() == 4 && list[0] == "sdmc:/games/Link tools/boot9.bin" && list[1] == "sdmc:/games/Link tools/3DSLink/boot9.bin" &&
          list[2] == "sdmc:/gm9/out/boot9.bin" && list[3] == "sdmc:/mine/boot9.bin");
    CHECK(setup::Boot9Candidates(p, "").size() == 3);                                   // nothing chosen: three places
    CHECK(setup::Boot9Candidates(p, "sdmc:/gm9/out/boot9.bin").size() == 3);           // a chosen path that is a default one is not listed twice
    CHECK(setup::Boot9Candidates(p, "sdmc:/games/Link tools/boot9.bin").size() == 3);
    CHECK(setup::Boot9Candidates(setup::Paths::FromAppDir("sdmc:/3ds/3DSLink"), "").size() == 2); // (the app in its own folder: next to it IS in it)
    // the first usable one wins (a made-up file system)
    std::map<std::string, u64> files;
    const setup::SizeFn fake = [&files](const std::string& path, u64* size) {
        const auto it = files.find(path);
        if (it == files.end()) {
            return false;
        }
        *size = it->second;
        return true;
    };
    CHECK(!setup::FindBoot9(list, fake).found);
    files["sdmc:/mine/boot9.bin"] = 0x10000;
    CHECK(setup::FindBoot9(list, fake).found && setup::FindBoot9(list, fake).path == "sdmc:/mine/boot9.bin");
    files["sdmc:/gm9/out/boot9.bin"] = 0x8000;
    CHECK(setup::FindBoot9(list, fake).path == "sdmc:/gm9/out/boot9.bin"); // GodMode9's folder before the user's choice
    files["sdmc:/games/Link tools/boot9.bin"] = 0x10000;
    CHECK(setup::FindBoot9(list, fake).path == "sdmc:/games/Link tools/boot9.bin" && setup::FindBoot9(list, fake).folder() == "sdmc:/games/Link tools");
    files["sdmc:/games/Link tools/boot9.bin"] = 1234; // the wrong size does not count: the next place is looked at
    CHECK(setup::FindBoot9(list, fake).path == "sdmc:/gm9/out/boot9.bin");
    files["sdmc:/gm9/out/boot9.bin"] = 0;
    CHECK(setup::FindBoot9(list, fake).path == "sdmc:/mine/boot9.bin");
    CHECK(!setup::FindBoot9(std::vector<std::string>(), fake).found);

    // the real file system
    Fresh();
    CHECK(!setup::FindBoot9(p, "").found && !setup::IsUsableBoot9("sdmc:/games/Link tools/boot9.bin"));
    Put("sdmc:/mine/boot9.bin", Boot9());
    CHECK(!setup::FindBoot9(p, "").found);                                          // not where it looks by itself
    CHECK(setup::FindBoot9(p, "sdmc:/mine/boot9.bin").path == "sdmc:/mine/boot9.bin"); // but where the user pointed to
    Put("sdmc:/gm9/out/boot9.bin", Boot9(0x8000));
    CHECK(setup::FindBoot9(p, "").path == "sdmc:/gm9/out/boot9.bin");
    Put("sdmc:/games/Link tools/boot9.bin", Boot9());
    CHECK(setup::FindBoot9(p, "sdmc:/mine/boot9.bin").path == "sdmc:/games/Link tools/boot9.bin" && setup::IsUsableBoot9("sdmc:/games/Link tools/boot9.bin"));
    fs::remove("sdmc:/games/Link tools/boot9.bin");
    fs::create_directories("sdmc:/games/Link tools/boot9.bin"); // a FOLDER with that name is not a file
    CHECK(setup::FindBoot9(p, "").path == "sdmc:/gm9/out/boot9.bin" && !setup::IsUsableBoot9("sdmc:/games/Link tools/boot9.bin"));
    Put("sdmc:/short/boot9.bin", "tiny");
    CHECK(!setup::IsUsableBoot9("sdmc:/short/boot9.bin") && !setup::IsUsableBoot9("sdmc:/short/none.bin"));

    // GodMode9 2.x names its dump after the console: the name test, and the lookup in the folders the app looks into by itself
    CHECK(setup::IsBoot9Name("boot9.bin") && setup::IsBoot9Name("BOOT9.BIN") && setup::IsBoot9Name("ABC12345678_boot9_00.bin") &&
          setup::IsBoot9Name("boot9_prot.bin") && setup::IsBoot9Name("SPR1234_Boot9_01.BIN"));
    CHECK(!setup::IsBoot9Name("ABC12345678_boot11_00.bin") && !setup::IsBoot9Name("boot11.bin") && !setup::IsBoot9Name("boot9.txt") &&
          !setup::IsBoot9Name("boot9") && !setup::IsBoot9Name("otp.bin") && !setup::IsBoot9Name(".bin"));
    Fresh();
    Put("sdmc:/gm9/out/ABC12345678_boot11_00.bin", Boot9()); // the other bootrom, same size: never taken for boot9
    CHECK(!setup::FindBoot9(p, "").found && setup::FindBoot9InFolder("sdmc:/gm9/out").empty());
    Put("sdmc:/gm9/out/ABC12345678_boot9_00.bin", Boot9());
    CHECK(setup::FindBoot9(p, "").found && setup::FindBoot9(p, "").path == "sdmc:/gm9/out/ABC12345678_boot9_00.bin" &&
          setup::FindBoot9(p, "").folder() == "sdmc:/gm9/out");
    Put("sdmc:/gm9/out/ABC12345678_boot9_01.bin", Boot9()); // a later dump of the same console wins
    CHECK(setup::FindBoot9InFolder("sdmc:/gm9/out") == "sdmc:/gm9/out/ABC12345678_boot9_01.bin");
    Put("sdmc:/gm9/out/ABC12345678_boot9_prot.bin", Boot9(0x8000)); // 64 KB before 32 KB, whatever the name
    CHECK(setup::FindBoot9InFolder("sdmc:/gm9/out") == "sdmc:/gm9/out/ABC12345678_boot9_01.bin");
    Put("sdmc:/gm9/out/boot9.bin", Boot9(0x8000)); // the file called exactly boot9.bin comes first, even when smaller
    CHECK(setup::FindBoot9InFolder("sdmc:/gm9/out") == "sdmc:/gm9/out/boot9.bin" && setup::FindBoot9(p, "").path == "sdmc:/gm9/out/boot9.bin");
    Fresh();
    Put("sdmc:/gm9/out/ABC12345678_boot9_00.bin", "tiny"); // a wrong size is no dump
    CHECK(!setup::FindBoot9(p, "").found);
    fs::create_directories("sdmc:/gm9/out/X_boot9_00.bin"); // a FOLDER with such a name is not a file
    CHECK(!setup::FindBoot9(p, "").found && setup::FindBoot9InFolder("sdmc:/nowhere").empty());
    Put("sdmc:/games/Link tools/Y_boot9_00.bin", Boot9()); // next to the app counts too, and before GodMode9's folder
    Put("sdmc:/gm9/out/Z_boot9_00.bin", Boot9());
    CHECK(setup::FindBoot9(p, "").path == "sdmc:/games/Link tools/Y_boot9_00.bin");

    // a boot9 dump is told by what is IN it, whatever it is called (the bootrom is the same on every console)
    const std::string abc = "abc";
    CHECK(setup::Sha256Hex(reinterpret_cast<const u8*>(abc.data()), abc.size()) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    CHECK(setup::Sha256Hex(nullptr, 0) == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
    const std::string million(1000000, 'a');
    CHECK(setup::Sha256Hex(reinterpret_cast<const u8*>(million.data()), million.size()) == "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0");
    const std::string keys(0x10000, 'K'), lower(0x8000, 'L');
    setup::SetBoot9FingerprintsForTest({{0x10000, setup::Sha256Hex(reinterpret_cast<const u8*>(keys.data()), keys.size()), setup::Boot9Verdict::Good},
                                        {0x8000, setup::Sha256Hex(reinterpret_cast<const u8*>(lower.data()), lower.size()), setup::Boot9Verdict::NoKeys}});
    Fresh();
    Put("sdmc:/gm9/out/mykeys.bin", keys);                 // a name that says nothing: the content does
    Put("sdmc:/gm9/out/other.bin", std::string(0x10000, 'O')); // the right size and no known content, a name that says nothing: not a boot9
    CHECK(setup::ClassifyBoot9File("sdmc:/gm9/out/mykeys.bin") == setup::Boot9Verdict::Good &&
          setup::ClassifyBoot9File("sdmc:/gm9/out/other.bin") == setup::Boot9Verdict::Unknown && setup::ClassifyBoot9File("sdmc:/gm9/out/none.bin") == setup::Boot9Verdict::Unknown);
    CHECK(setup::FindBoot9(p, "").found && setup::FindBoot9(p, "").path == "sdmc:/gm9/out/mykeys.bin");
    Put("sdmc:/gm9/out/NEG1_boot9_00.bin", std::string(0x10000, 'N')); // a name that says boot9, content not known: counts too, but the known content wins
    CHECK(setup::FindBoot9InFolder("sdmc:/gm9/out") == "sdmc:/gm9/out/mykeys.bin");
    fs::remove("sdmc:/gm9/out/mykeys.bin");
    CHECK(setup::FindBoot9InFolder("sdmc:/gm9/out") == "sdmc:/gm9/out/NEG1_boot9_00.bin");
    Fresh();
    Put("sdmc:/gm9/out/boot9_prot.bin", lower); // the lower half alone: the size of a boot9, and no keys in it
    CHECK(setup::ClassifyBoot9File("sdmc:/gm9/out/boot9_prot.bin") == setup::Boot9Verdict::NoKeys && !setup::IsUsableBoot9("sdmc:/gm9/out/boot9_prot.bin") &&
          !setup::FindBoot9(p, "").found && setup::FindBoot9InFolder("sdmc:/gm9/out").empty());
    Put("sdmc:/gm9/out/boot9.bin", lower); // even called exactly boot9.bin
    CHECK(!setup::FindBoot9(p, "").found && !setup::IsUsableBoot9("sdmc:/gm9/out/boot9.bin"));
    Put("sdmc:/gm9/out/boot9.bin", keys); // the file is replaced (the answer is not remembered from the old content)
    CHECK(setup::FindBoot9(p, "").found && setup::IsUsableBoot9("sdmc:/gm9/out/boot9.bin"));
    setup::SetBoot9FingerprintsForTest({}); // the real ones again
}

static void TestResolvePluginFile() {
    std::printf("the plugin file in use: next to the app when it is there, else in the app's own folder (where the app puts the plugin it carries)\n");
    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/games/Link tools");
    Fresh();
    CHECK(setup::ResolvePluginFile(p) == p.plugin_in_data); // none: where the app puts the plugin it carries (its own folder: sdmc:/3ds/3DSLink for a CIA install)
    CHECK(setup::ResolvePluginFile(p) == "sdmc:/games/Link tools/3DSLink/AzaharLink.3gx");
    Put(p.plugin_in_data, "x");
    CHECK(setup::ResolvePluginFile(p) == p.plugin_in_data); // only in the app's own folder
    Put(p.plugin_beside, "x");
    CHECK(setup::ResolvePluginFile(p) == p.plugin_beside); // next to the app wins (a file put there by hand, before the plugin was a resource of the app)
    const setup::Paths threeds = setup::Paths::FromAppDir("sdmc:/3ds"); // (the app is a 3dsx in sdmc:/3ds: the plugin goes to sdmc:/3ds/3DSLink, not into the folder of every other 3dsx)
    Fresh();
    CHECK(setup::ResolvePluginFile(threeds) == "sdmc:/3ds/3DSLink/AzaharLink.3gx");
    const setup::Paths own = setup::Paths::FromAppDir("sdmc:/3ds/3DSLink"); // (the same place twice)
    Fresh();
    CHECK(setup::ResolvePluginFile(own) == "sdmc:/3ds/3DSLink/AzaharLink.3gx");
    Put(own.plugin_file, "x");
    CHECK(setup::ResolvePluginFile(own) == "sdmc:/3ds/3DSLink/AzaharLink.3gx");
}

// ---------------------------------------------------------------------------------------------

// Steps a search to its end, returning how many calls it took.
static int Run(setup::FileSearch& search, int budget) {
    int calls = 0;
    while (!search.finished() && calls < 100000) {
        search.Step(budget);
        ++calls;
    }
    return calls;
}

static void TestSearch() {
    std::printf("the search of the card: breadth first, no Nintendo 3DS folder, only a good file is a hit, B cancels\n");
    Fresh();
    Put("sdmc:/a/b/c/d/boot9.bin", Boot9());             // deep
    Put("sdmc:/x/Boot9.BIN", Boot9(0x8000));             // shallow, other case
    Put("sdmc:/Nintendo 3DS/id0/id1/boot9.bin", Boot9()); // never entered
    Put("sdmc:/System Volume Information/boot9.bin", Boot9());
    Put("sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx", "3GX$ copy");
    Put("sdmc:/luma/plugins/AzaharLink.3gx", "3GX$ loose");
    Put("sdmc:/other/AzaharLink.3gx", "not a plugin");
    fs::create_directories("sdmc:/folder named/boot9.bin"); // a folder called boot9.bin is no file

    const auto usable = [](const std::string& path) { return setup::IsUsableBoot9(path); };
    {
        setup::FileSearch search("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        CHECK(!search.finished() && !search.found());
        const int calls = Run(search, 1000);
        CHECK(search.finished() && search.found() && calls >= 1);
        CHECK(search.result() == "sdmc:/x/Boot9.BIN"); // the shallow one first (breadth first), the card's own spelling of its name
        CHECK(search.folders_done() >= 1 && search.entries_seen() > 0);
    }
    { // one entry at a time: the same answer, however small the slices
        setup::FileSearch search("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        const int calls = Run(search, 1);
        CHECK(search.found() && search.result() == "sdmc:/x/Boot9.BIN" && calls > 3);
    }
    { // without the shallow one: the deep one is found, and the files of Nintendo 3DS and of the PC's leftovers never are
        fs::remove_all("sdmc:/x");
        setup::FileSearch search("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        Run(search, 1000);
        CHECK(search.found() && search.result() == "sdmc:/a/b/c/d/boot9.bin");
        fs::remove_all("sdmc:/a");
        setup::FileSearch none("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        Run(none, 1000);
        CHECK(none.finished() && !none.found() && none.result().empty());
    }
    { // a name test of its own: GodMode9 2.x calls the dump "<serial>_boot9_00.bin", and the boot11 dump next to it is never taken
        Put("sdmc:/gm9/out/ABC12345678_boot11_00.bin", Boot9());
        Put("sdmc:/gm9/out/ABC12345678_boot9_00.bin", Boot9());
        setup::FileSearch::Options options;
        options.name_match = [](const std::string& name) { return setup::IsBoot9Name(name); };
        setup::FileSearch search("sdmc:", "boot9.bin", usable, options);
        Run(search, 1000);
        CHECK(search.found() && search.result() == "sdmc:/gm9/out/ABC12345678_boot9_00.bin");
        fs::remove("sdmc:/gm9/out/ABC12345678_boot9_00.bin");
        setup::FileSearch only11("sdmc:", "boot9.bin", usable, options);
        Run(only11, 1000);
        CHECK(only11.finished() && !only11.found());
    }
    { // a file with the right name that is no good is walked past
        Put("sdmc:/tiny/boot9.bin", "tiny");
        setup::FileSearch search("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        Run(search, 1000);
        CHECK(search.finished() && !search.found());
    }
    { // the plugin: header checked, the copies in the folders of the titles skipped on request
        const auto is_plugin = [](const std::string& path) { return Get(path).compare(0, 4, "3GX$") == 0; };
        setup::FileSearch loose("sdmc:", "AzaharLink.3gx", is_plugin, setup::FileSearch::Options());
        Run(loose, 1000);
        CHECK(loose.found() && loose.result() == "sdmc:/luma/plugins/AzaharLink.3gx"); // (the other one is a folder deeper, and "not a plugin")
        fs::remove("sdmc:/luma/plugins/AzaharLink.3gx");
        setup::FileSearch with_copies("sdmc:", "azaharlink.3gx", is_plugin, setup::FileSearch::Options());
        Run(with_copies, 1000);
        CHECK(with_copies.found() && with_copies.result() == "sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx");
        setup::FileSearch::Options skip;
        skip.skip_plugin_title_folders = true;
        setup::FileSearch without_copies("sdmc:", "AzaharLink.3gx", is_plugin, skip);
        Run(without_copies, 1000);
        CHECK(without_copies.finished() && !without_copies.found()); // only the title's copy and the file that is no plugin: nothing
        // a folder under plugins that is not a title id is searched
        Put("sdmc:/luma/plugins/mine/AzaharLink.3gx", "3GX$ in my folder");
        setup::FileSearch mine("sdmc:", "AzaharLink.3gx", is_plugin, skip);
        Run(mine, 1000);
        CHECK(mine.found() && mine.result() == "sdmc:/luma/plugins/mine/AzaharLink.3gx");
    }
    { // cancel: finished at once, nothing found, and nothing more is read
        setup::FileSearch search("sdmc:", "boot9.bin", usable, setup::FileSearch::Options());
        search.Step(2);
        search.Cancel();
        CHECK(search.finished() && !search.found());
        const long seen = search.entries_seen();
        CHECK(search.Step(100) && !search.found() && search.entries_seen() == seen); // (still over: nothing more is read)
    }
    { // the depth limit
        Put("sdmc:/l1/l2/l3/l4/deep.txt", "x");
        setup::FileSearch::Options shallow;
        shallow.max_depth = 2;
        setup::FileSearch search("sdmc:", "deep.txt", [](const std::string&) { return true; }, shallow);
        Run(search, 1000);
        CHECK(search.finished() && !search.found());
        setup::FileSearch deeper("sdmc:", "deep.txt", [](const std::string&) { return true; }, setup::FileSearch::Options());
        Run(deeper, 1000);
        CHECK(deeper.found() && deeper.result() == "sdmc:/l1/l2/l3/l4/deep.txt");
    }
    { // a root that cannot be opened: the search ends with nothing
        setup::FileSearch search("sdmc:/nowhere", "boot9.bin", usable, setup::FileSearch::Options());
        Run(search, 10);
        CHECK(search.finished() && !search.found());
    }
    CHECK(setup::IsTitleIdFolderName("000400000016E700") && setup::IsTitleIdFolderName("000400000016e700"));
    CHECK(!setup::IsTitleIdFolderName("000400000016E70") && !setup::IsTitleIdFolderName("000400000016E70G") && !setup::IsTitleIdFolderName("mine"));
}

static void TestFolderListing() {
    std::printf("the folder browser: the sub-folders of a folder, sorted, files left out\n");
    Fresh();
    fs::create_directories("sdmc:/Zeta");
    fs::create_directories("sdmc:/alpha");
    fs::create_directories("sdmc:/Beta");
    fs::create_directories("sdmc:/alpha/inner");
    Put("sdmc:/file.txt", "x");
    bool ok = false;
    std::vector<std::string> names = setup::ListSubfolders("sdmc:", &ok);
    CHECK(ok && names == std::vector<std::string>({"alpha", "Beta", "Zeta"}));
    names = setup::ListSubfolders("sdmc:/alpha", &ok);
    CHECK(ok && names == std::vector<std::string>({"inner"}));
    names = setup::ListSubfolders("sdmc:/Beta", &ok);
    CHECK(ok && names.empty()); // an empty folder is fine
    names = setup::ListSubfolders("sdmc:/nowhere", &ok);
    CHECK(!ok && names.empty());
    CHECK(setup::ListSubfolders("sdmc:/file.txt", &ok).empty() && !ok); // a file is not a folder
    // walking up and down the way the browser does
    std::string path = "sdmc:";
    path = setup::JoinPath(path, "alpha");
    path = setup::JoinPath(path, "inner");
    CHECK(path == "sdmc:/alpha/inner" && setup::ParentDir(path) == "sdmc:/alpha" && setup::ParentDir(setup::ParentDir(path)) == "sdmc:" &&
          setup::ParentDir("sdmc:").empty());
}

static void TestFileStampAndPark() {
    std::printf("the stamp of a file (size and date), and the block files a receiving left in park/\n");
    Fresh();
    CHECK(setup::FileStamp("sdmc:/none.3gx").empty() && setup::FileStamp("").empty());
    Put("sdmc:/AzaharLink.3gx", std::string(1000, 'p'));
    const std::string stamp = setup::FileStamp("sdmc:/AzaharLink.3gx");
    CHECK(stamp.size() > 5 && stamp.find("1000:") == 0 && stamp.find(':') == 4); // "size:mtime"
    CHECK(setup::FileStamp("sdmc:/AzaharLink.3gx") == stamp);               // the same file, the same stamp
    Put("sdmc:/AzaharLink.3gx", std::string(1001, 'p'));
    CHECK(setup::FileStamp("sdmc:/AzaharLink.3gx").find("1001:") == 0);      // another size, another stamp
    fs::create_directories("sdmc:/dir");
    CHECK(setup::FileStamp("sdmc:/dir").empty());                           // a folder is not a file
    // park/: only *.blk go, anything else stays; no folder is no error
    CHECK(setup::RemoveParkFiles("sdmc:/3ds/3DSLink/park") == 0);
    Put("sdmc:/3ds/3DSLink/park/0.blk", "x");
    Put("sdmc:/3ds/3DSLink/park/1.blk", "y");
    Put("sdmc:/3ds/3DSLink/park/note.txt", "z");
    Put("sdmc:/3ds/3DSLink/park/blk", "w");
    CHECK(setup::RemoveParkFiles("sdmc:/3ds/3DSLink/park") == 2);
    CHECK(!Exists("sdmc:/3ds/3DSLink/park/0.blk") && !Exists("sdmc:/3ds/3DSLink/park/1.blk"));
    CHECK(Exists("sdmc:/3ds/3DSLink/park/note.txt") && Exists("sdmc:/3ds/3DSLink/park/blk") && Exists("sdmc:/3ds/3DSLink/park"));
    CHECK(setup::RemoveParkFiles("sdmc:/3ds/3DSLink/park") == 0);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_paths_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    TestAppDirFromArgv0();
    TestPathBuilding();
    TestMakeDirsAndFirstStart();
    TestAppConfig();
    TestDlpDeliverySwitch();
    TestBoot9Lookup();
    TestResolvePluginFile();
    TestSearch();
    TestFolderListing();
    TestFileStampAndPark();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
