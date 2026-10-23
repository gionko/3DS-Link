// The title delivery module's log layer (ConfigApp/titleshare/source/log.c, the very file with its documented patch, built on a PC against a stand-in for
// libctru's threads and locks) run by Tests/run.sh. What it pins: with the app's Debug log OFF the module creates no file at all and starts no
// watchdog (the one log switch); ON, the log is 3DSLink-TitleDelivery.log in the app's data folder, appended to by every run, with a first line
// that does not name the old experiment; when a file reaches the size limit (here 4 KB instead of 64 MiB: -DLOGROT_MAX_BYTES) the older files move
// up (.1.log ... .7.log) and no more than 8 files ever exist; a file left large by an earlier run is rotated when the log opens; no folder, no log.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "hostcfg.h"
#include "log.h"
#include "logrot.h"
}

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

static std::string Get(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
    }
    return s;
}

static std::string Name(const char* dir, int index) {
    char out[HC_PATH_MAX];
    return logrot_name(out, sizeof(out), dir, index) == 0 ? std::string(out) : std::string();
}

static int CountFiles(const char* dir) {
    int n = 0;
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            (void) entry;
            ++n;
        }
    }
    return n;
}

static void TestSwitchOff() {
    std::printf("Debug log OFF: no file, no thread, no cost\n");
    fs::remove_all("data");
    fs::create_directories("data");
    hc_set_data_dir("data");
    hc_set_log_enabled(0);
    ts_log_open("mittente");
    ts_log("this line goes nowhere %d", 1);
    CHECK(ts_wd_add("loop") == -1); // (no watchdog without a log)
    ts_log_flush();
    ts_log_close();
    CHECK(CountFiles("data") == 0);
    CHECK(!fs::exists("data/log.txt") && !fs::exists("data/log-prev.txt"));
    // the switch on but nowhere to write: nothing either
    hc_set_log_enabled(1);
    hc_set_data_dir(nullptr);
    ts_log_open("ricevente");
    ts_log("no folder");
    ts_log_close();
    CHECK(CountFiles("data") == 0);
    // a data folder that is not there is made by the log (the app made it before; this only covers it going missing)
    hc_set_data_dir("data/not_yet");
    ts_log_open("ricevente");
    ts_log("made");
    ts_log_close();
    CHECK(fs::exists(Name("data/not_yet", 0)));
    hc_set_data_dir(nullptr);
    hc_set_log_enabled(0);
}

static void TestSwitchOn() {
    std::printf("Debug log ON: 3DSLink-TitleDelivery.log in the data folder, appended to by every run\n");
    fs::remove_all("data");
    fs::create_directories("data");
    hc_set_data_dir("data");
    hc_set_log_enabled(1);
    ts_log_open("mittente");
    CHECK(ts_wd_add("loop") >= 0);
    ts_log("first line %d", 7);
    ts_log_flush();
    ts_log_close();
    const std::string path = Name("data", 0);
    CHECK(path == "data/3DSLink-TitleDelivery.log" && fs::exists(path));
    const std::string first = Get(path);
    CHECK(first.find("3DS Link title delivery log (mittente)") == 0);
    CHECK(first.find("first line 7") != std::string::npos);
    CHECK(first.find("TitleShare") == std::string::npos); // the user never sees the old name
    // a second run appends, and the old file is not renamed to anything
    ts_log_open("ricevente");
    ts_log("second run");
    ts_log_close();
    const std::string both = Get(path);
    CHECK(both.compare(0, first.size(), first) == 0 && both.find("3DS Link title delivery log (ricevente)") != std::string::npos &&
          both.find("second run") != std::string::npos);
    CHECK(CountFiles("data") == 1 && !fs::exists("data/log.txt") && !fs::exists("data/log-prev.txt") && !fs::exists(Name("data", 1)));
    // switched off again: the file stays, nothing is added
    hc_set_log_enabled(0);
    ts_log_open("mittente");
    ts_log("while it is off");
    ts_log_close();
    CHECK(Get(path) == both);
    hc_set_log_enabled(0);
}

static void TestRotation() {
    std::printf("rotation: at the limit the older files move up, never more than 8 files\n");
    fs::remove_all("data");
    fs::create_directories("data");
    hc_set_data_dir("data");
    hc_set_log_enabled(1);
    ts_log_open("mittente");
    const std::string filler(560, 'x');
    const int lines = 150; // about 90 KB: more than 20 files of 4 KB if none went
    for (int i = 0; i < lines; ++i) {
        ts_log("line %04d %s", i, filler.c_str());
        ts_log_flush(); // (so that each file is closed soon after the limit, not a whole 32 KB flush later)
    }
    ts_log_close();
    CHECK(fs::exists(Name("data", 0)) && fs::exists(Name("data", 1)) && fs::exists(Name("data", LOGROT_KEEP)));
    CHECK(CountFiles("data") == 8); // the current one and .1.log ... .7.log
    CHECK(!fs::exists("data/3DSLink-TitleDelivery.8.log"));
    // the newest lines are in the newest files, the oldest are gone
    CHECK(Get(Name("data", 0)).find("line 0149") != std::string::npos);
    CHECK(Get(Name("data", 1)).find("line 0149") == std::string::npos);
    bool oldest_gone = true;
    for (int i = 0; i <= LOGROT_KEEP; ++i) {
        oldest_gone = oldest_gone && Get(Name("data", i)).find("line 0000 ") == std::string::npos;
    }
    CHECK(oldest_gone);
    // every file but the current one is about the size of the limit (it is closed by the first write past it)
    for (int i = 1; i <= LOGROT_KEEP; ++i) {
        const auto size = fs::file_size(Name("data", i));
        CHECK(size >= LOGROT_MAX_BYTES && size < LOGROT_MAX_BYTES + 2000);
    }
    // a file after a rotation says what it follows
    CHECK(Get(Name("data", 0)).find("3DS Link title delivery log (il file di prima e' .1.log)") == 0);

    // the series continues across runs
    const std::string newest_before = Get(Name("data", 0));
    ts_log_open("ricevente");
    ts_log("after the rotation");
    ts_log_close();
    CHECK(Get(Name("data", 0)).compare(0, newest_before.size(), newest_before) == 0 && Get(Name("data", 0)).find("after the rotation") != std::string::npos);
    CHECK(CountFiles("data") == 8);
}

static void TestLeftoverIsRotatedAtOpen() {
    std::printf("a file that an earlier run left past the limit is rotated when the log opens\n");
    fs::remove_all("data");
    fs::create_directories("data");
    hc_set_data_dir("data");
    hc_set_log_enabled(1);
    {
        FILE* f = std::fopen(Name("data", 0).c_str(), "wb");
        const std::string big(LOGROT_MAX_BYTES + 100, 'o');
        std::fwrite(big.data(), 1, big.size(), f);
        std::fclose(f);
    }
    ts_log_open("mittente");
    ts_log("fresh");
    ts_log_close();
    CHECK(fs::exists(Name("data", 1)) && fs::file_size(Name("data", 1)) == static_cast<std::size_t>(LOGROT_MAX_BYTES + 100));
    CHECK(fs::file_size(Name("data", 0)) < 600 && Get(Name("data", 0)).find("fresh") != std::string::npos);
    hc_set_data_dir(nullptr);
    hc_set_log_enabled(0);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_logmod_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    CHECK(LOGROT_MAX_BYTES == 4096); // (this test is built with the small limit)
    TestSwitchOff();
    TestSwitchOn();
    TestRotation();
    TestLeftoverIsRotatedAtOpen();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
