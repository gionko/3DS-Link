// whole_file_states_test (F7, 2026-11-21): the app's own ConfigApp/source/fileio.cpp in front of every card that a cut save of the plugin
// (Includes/azl/whole_file.hpp, cut at every step in Tests/whole_file_model.hpp) can leave, written for real in a temporary directory ("sdmc:"
// under it, as settings_test). What it holds, for each of those cards:
//   - ReadTextFile reads what the plugin's reader (whole::Choose) reads - the file, else its .bak, else Missing - and when that was the .bak
//     it is back at its place afterwards, with no .bak left;
//   - WriteTextFile leaves the new file, no .tmp and no .bak (F7-rev: also on a card where only the .bak of a cut save was), and the very same
//     files as whole::Replace on the same card: app and plugin are one order;
//   - the empty .new boxes of the lists (an older build left them) are never read nor touched.
//   whole_file_states_test
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "../whole_file_model.hpp"
#include "fileio.hpp"

namespace fs = std::filesystem;
using namespace wfm;

static int g_checks = 0, g_failures = 0, g_shown = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            if (g_shown++ < 40) {                                                                  \
                std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                    \
            }                                                                                      \
        }                                                                                          \
    } while (0)

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

static void Put(const Files& files) {
    fs::remove_all("sdmc:");
    fs::create_directories("sdmc:/3ds/3DSLink");
    for (const auto& kv : files) {
        FILE* f = std::fopen(kv.first.c_str(), "wb");
        std::fwrite(kv.second.data(), 1, kv.second.size(), f);
        std::fclose(f);
    }
}

// the card as it is now: every file of the folder with its text
static Files Take() {
    Files files;
    for (const auto& e : fs::directory_iterator("sdmc:/3ds/3DSLink")) {
        const std::string p = "sdmc:/3ds/3DSLink/" + e.path().filename().string();
        files[p] = Slurp(p);
    }
    return files;
}

// whole::Replace on the PC's file system (a rename here replaces: the Replace way of the model)
struct PosixFs {
    bool Exists(const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    bool WriteNew(const std::string& p, const std::string& text) {
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) {
            return false;
        }
        const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (std::fclose(f) != 0 || !wrote) {
            return false;
        }
        return Slurp(p) == text;
    }
    bool Rename(const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; }
    void Remove(const std::string& p) { std::remove(p.c_str()); }
};

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_whole_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    // every card the model's cut saves leave, and the states they start from
    std::set<Files> cards;
    for (const State& st : States()) {
        cards.insert(st.files);
    }
    ForEachCut([&](const State&, RenameMode, const Run& r) { cards.insert(r.files); });
    std::printf("the app's fileio.cpp in front of %zu cards a cut save can leave\n", cards.size());
    const std::string bak = azl::whole::BakOf(kPath);
    const std::string tmp = azl::whole::TmpOf(kPath);
    int from_bak = 0;
    for (const Files& card : cards) {
        const Read want = PluginRead(card, kPath);
        // the app's reader
        Put(card);
        std::string text;
        const setup::LoadResult got = setup::ReadTextFile(kPath, 4096, text);
        CHECK(got == (want.present ? setup::LoadResult::Loaded : setup::LoadResult::Missing));
        CHECK(!want.present || text == want.text);
        if (want.present && !card.count(kPath)) {
            ++from_bak;
            CHECK(fs::exists(kPath) && Slurp(kPath) == want.text && !fs::exists(bak));
        }
        for (const std::string& box : {kBoxTitles, kBoxIni}) {
            CHECK(card.count(box) == static_cast<std::size_t>(fs::exists(box)) && (!card.count(box) || Slurp(box) == card.at(box)));
        }
        // the app's writer and the plugin's order on the same card: the same files after them
        Put(card);
        CHECK(setup::WriteTextFile(kPath, kNew2));
        const Files by_app = Take();
        CHECK(by_app.count(kPath) && by_app.at(kPath) == kNew2 && !by_app.count(tmp));
        CHECK(!by_app.count(bak)); // (F7-rev: the writer drops the .bak whether or not it found a file at its place)
        Put(card);
        PosixFs pc;
        CHECK(azl::whole::Replace(pc, kPath, kNew2));
        const Files by_plugin = Take();
        CHECK(by_plugin == by_app);
        for (const std::string& box : {kBoxTitles, kBoxIni}) {
            CHECK(by_app.count(box) == card.count(box) && (!card.count(box) || by_app.at(box) == card.at(box)));
        }
    }
    std::printf("  %d of them read from the .bak\n", from_bak);
    CHECK(from_bak > 0);
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
