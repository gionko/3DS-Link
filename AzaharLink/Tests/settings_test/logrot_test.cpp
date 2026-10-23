// What the 3DS Link app tells the title delivery module and how the module names and rotates its log (ConfigApp/titleshare_host/hostcfg.c and
// logrot.c, the very files, plain C built on a PC), run by Tests/run.sh. What it pins: the log's file names (the current file and .1.log ...
// .7.log: eight at most), when a file is due to rotate (64 MiB), what one rotation does to a real folder (the oldest goes, the others move
// up, the current one becomes .1, a gap in the series is fine); the paths the module gets from the app (the data folder, the park folder, the
// catalog cache, the key file, the movable.sed places) and that they are the app's own (ConfigApp/source/paths.cpp builds the same ones), the
// SD-card form of a path, the one log switch, and that a path that does not fit is refused, never cut.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../ConfigApp/source/paths.hpp"
#include "../../ConfigApp/titleshare_host/hostcfg.h"
#include "../../ConfigApp/titleshare_host/logrot.h"

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
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

static std::string Get(const std::string& path) {
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

static std::string Name(const std::string& dir, int index) {
    char out[HC_PATH_MAX];
    return logrot_name(out, sizeof(out), dir.c_str(), index) == 0 ? std::string(out) : std::string("<none>");
}

// What the log layer does with the plan (ConfigApp/titleshare/source/log.c log_rotate).
static void Rotate(const std::string& dir) {
    logrot_op ops[LOGROT_MAX_OPS];
    const int count = logrot_plan(ops, LOGROT_MAX_OPS);
    for (int i = 0; i < count; ++i) {
        const std::string from = Name(dir, ops[i].from);
        if (ops[i].remove) {
            std::remove(from.c_str());
        } else {
            std::rename(from.c_str(), Name(dir, ops[i].to).c_str());
        }
    }
}

static void TestNames() {
    std::printf("the log's file names: the current one, then .1.log ... .7.log (8 files in all)\n");
    CHECK(Name("sdmc:/3ds/3DSLink", 0) == "sdmc:/3ds/3DSLink/3DSLink-TitleDelivery.log");
    CHECK(Name("sdmc:/3ds/3DSLink", 1) == "sdmc:/3ds/3DSLink/3DSLink-TitleDelivery.1.log");
    CHECK(Name("sdmc:/3ds/3DSLink", 7) == "sdmc:/3ds/3DSLink/3DSLink-TitleDelivery.7.log");
    CHECK(Name("sdmc:/3ds/3DSLink", 8) == "<none>" && Name("sdmc:/3ds/3DSLink", -1) == "<none>"); // only 8 files
    CHECK(LOGROT_KEEP == 7 && LOGROT_MAX_OPS == 9);
    char small[16];
    CHECK(logrot_name(small, sizeof(small), "sdmc:/3ds/3DSLink", 0) == -1); // does not fit: refused, not cut
    CHECK(logrot_name(small, sizeof(small), nullptr, 0) == -1);
    // the same names as the app builds for the page that shows the log
    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/games/Link tools");
    CHECK(Name(p.datadir, 0) == p.delivery_log);
    // none of them says what the old experiment was called
    for (int i = 0; i <= LOGROT_KEEP; ++i) {
        CHECK(Name(p.datadir, i).find("TitleShare") == std::string::npos && Name(p.datadir, i).find("log.txt") == std::string::npos);
    }
}

static void TestDue() {
    std::printf("a file is due to rotate at 64 MiB\n");
    CHECK(LOGROT_MAX_BYTES == 64ull * 1024 * 1024);
    CHECK(!logrot_due(0) && !logrot_due(LOGROT_MAX_BYTES - 1));
    CHECK(logrot_due(LOGROT_MAX_BYTES) && logrot_due(LOGROT_MAX_BYTES + 1) && logrot_due(1ull << 40));
}

static void TestPlan() {
    std::printf("one rotation: the oldest goes, the others move up one place, the current one becomes .1\n");
    logrot_op ops[LOGROT_MAX_OPS];
    CHECK(logrot_plan(ops, LOGROT_MAX_OPS - 1) == 0); // room for it is the caller's to give
    const int count = logrot_plan(ops, LOGROT_MAX_OPS);
    CHECK(count == 8 && count <= LOGROT_MAX_OPS);
    CHECK(ops[0].remove == 1 && ops[0].from == 7);
    for (int i = 1; i < count; ++i) {
        CHECK(ops[i].remove == 0 && ops[i].from == 7 - i && ops[i].to == 8 - i); // 6->7, 5->6, ... 0->1
    }

    // on a real folder: eight files, every one with its own content
    fs::remove_all("logs");
    fs::create_directories("logs");
    for (int i = 0; i <= LOGROT_KEEP; ++i) {
        Put(Name("logs", i), "content " + std::to_string(i));
    }
    Rotate("logs");
    CHECK(!fs::exists(Name("logs", 0))); // the caller opens a new current file
    for (int i = 1; i <= LOGROT_KEEP; ++i) {
        CHECK(Get(Name("logs", i)) == "content " + std::to_string(i - 1)); // each one moved up; the old .7 is gone
    }
    // again: a series of 8 stays a series of 8
    Put(Name("logs", 0), "newest");
    Rotate("logs");
    CHECK(Get(Name("logs", 1)) == "newest" && Get(Name("logs", 2)) == "content 0" && Get(Name("logs", 7)) == "content 5");
    int files = 0;
    for (const auto& entry : fs::directory_iterator("logs")) {
        (void) entry;
        ++files;
    }
    CHECK(files == 7); // (no .0: the current one was moved; at most 8 with the new current file)

    // a short series (a first rotation): only what exists moves, the gaps are no error
    fs::remove_all("logs");
    fs::create_directories("logs");
    Put(Name("logs", 0), "only");
    Rotate("logs");
    CHECK(Get(Name("logs", 1)) == "only" && !fs::exists(Name("logs", 0)) && !fs::exists(Name("logs", 2)));
    fs::remove_all("logs");
}

static void TestHostConfig() {
    std::printf("what the app tells the module: folders, key file, log switch\n");
    char out[HC_PATH_MAX];
    // nothing set: nothing is made up
    hc_set_data_dir(nullptr);
    hc_set_app_dir(nullptr);
    hc_set_boot9_path(nullptr);
    CHECK(hc_data_path(out, sizeof(out), "catalog.cache") == -1 && hc_data_dir()[0] == 0 && hc_app_dir()[0] == 0 && hc_boot9_path()[0] == 0);
    CHECK(hc_movable_candidate(0, out, sizeof(out)) == -1); // (no app folder: no such candidate)
    CHECK(hc_movable_candidate(1, out, sizeof(out)) == 0 && std::string(out) == "sdmc:/gm9/out/movable.sed");
    CHECK(hc_movable_candidate(2, out, sizeof(out)) == -1 && hc_movable_candidate(-1, out, sizeof(out)) == -1);

    // set by the app: the module's files are the app's files
    const setup::Paths p = setup::Paths::FromAppDir("sdmc:/games/Link tools");
    hc_set_app_dir(p.appdir.c_str());
    hc_set_data_dir((p.datadir + "/").c_str()); // (a trailing slash is dropped)
    CHECK(std::string(hc_data_dir()) == p.datadir && std::string(hc_app_dir()) == p.appdir);
    CHECK(hc_data_path(out, sizeof(out), "catalog.cache") == 0 && std::string(out) == p.catalog_cache);
    CHECK(hc_data_path(out, sizeof(out), "park") == 0 && std::string(out) == p.park_dir);
    CHECK(hc_data_path(out, 10, "catalog.cache") == -1 && hc_data_path(out, sizeof(out), nullptr) == -1); // does not fit / no name
    CHECK(hc_movable_candidate(0, out, sizeof(out)) == 0 && std::string(out) == "sdmc:/games/Link tools/movable.sed");
    CHECK(setup::kCatalogCacheFileName == std::string("catalog.cache") && setup::kParkDirName == std::string("park"));

    // the key file, as the app found it ("" = none)
    CHECK(hc_boot9_path()[0] == 0);
    hc_set_boot9_path("sdmc:/mine/boot9.bin");
    CHECK(std::string(hc_boot9_path()) == "sdmc:/mine/boot9.bin");
    hc_set_boot9_path("");
    CHECK(hc_boot9_path()[0] == 0);

    // the one log switch
    CHECK(hc_log_enabled() == 0); // off until the app says so
    hc_set_log_enabled(1);
    CHECK(hc_log_enabled() == 1);
    hc_set_log_enabled(5);
    CHECK(hc_log_enabled() == 1);
    hc_set_log_enabled(0);
    CHECK(hc_log_enabled() == 0);

    // a path that does not fit is refused, never cut to another path
    const std::string huge(HC_PATH_MAX, 'a');
    hc_set_data_dir(huge.c_str());
    CHECK(hc_data_dir()[0] == 0 && hc_data_path(out, sizeof(out), "x") == -1);
    hc_set_data_dir(std::string(HC_PATH_MAX - 1, 'a').c_str());
    CHECK(std::strlen(hc_data_dir()) == HC_PATH_MAX - 1 && hc_data_path(out, sizeof(out), "x") == -1);

    // the SD card's form of a path (the archive of the file system service has no device name)
    hc_set_data_dir(p.datadir.c_str());
    CHECK(hc_sd_relative(out, sizeof(out), "sdmc:/3ds/3DSLink/park/0004000000177000.blk") == 0 && std::string(out) == "/3ds/3DSLink/park/0004000000177000.blk");
    CHECK(hc_sd_relative(out, sizeof(out), "/already/relative") == 0 && std::string(out) == "/already/relative");
    CHECK(hc_sd_relative(out, sizeof(out), "no/slash") == 0 && std::string(out) == "/no/slash");
    CHECK(hc_sd_relative(out, sizeof(out), "sdmc:") == 0 && std::string(out) == "/");
    CHECK(hc_sd_relative(out, 8, "sdmc:/3ds/3DSLink/park/x.blk") == -1 && hc_sd_relative(out, sizeof(out), nullptr) == -1);
    hc_set_data_dir(nullptr);
    hc_set_app_dir(nullptr);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_logrot_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    TestNames();
    TestDue();
    TestPlan();
    TestHostConfig();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
