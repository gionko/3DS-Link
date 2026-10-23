// The keyed reading of a game's icon file (ConfigApp/source/ncch_icon.cpp, the very file, built on a PC with TitleShare's own aes.c,
// sdcrypt.c, sha256.c, contentpath.c and ncchname.c, which are the vendored copies in ConfigApp/titleshare/common), run by
// Tests/run.sh. The title is made up under a directory called "sdmc:" in a temporary directory: its TMD and its main content are
// encrypted with a made-up SD key and a made-up NCCH key, exactly as a console stores them (the SD layer over the whole file, the
// NCCH layer over the ExeFS), and the reader has to give back the planted icon file. What it pins: the whole SMDH comes out (the
// name and the icon), the same name TitleShare's own ncch_read_name finds, the highest TMD wins, a content in a sub-folder is found,
// an NCCH without the crypto flag, and every way a title can be unreadable (no folder, no TMD, a TMD of no known type, a wrong key,
// a title without an icon, a DLC without an ExeFS, an icon file that is too short) ends with a negative number and no crash.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "games.hpp"
#include "ncch_icon.hpp"

// (TitleShare's headers by path: a -I on its folder would also hide the system's <sched.h>, which it has a header of its own named like)
extern "C" {
#include "../../ConfigApp/titleshare/common/aes.h"
#include "../../ConfigApp/titleshare/common/ncchname.h"
#include "../../ConfigApp/titleshare/common/sdcrypt.h"
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

typedef std::vector<uint8_t> Bytes;

static const char* const kRoot = "sdmc:/Nintendo 3DS/00112233445566778899aabbccddeeff/ffeeddccbbaa99887766554433221100";

static uint32_t g_sd_rk[44];
static uint8_t g_keyx[16];

static void Put(const std::string& path, const Bytes& data) {
    fs::create_directories(fs::path(path).parent_path());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
}

static void Be32(Bytes& b, std::size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 24);
    b[at + 1] = static_cast<uint8_t>(v >> 16);
    b[at + 2] = static_cast<uint8_t>(v >> 8);
    b[at + 3] = static_cast<uint8_t>(v);
}

static void Le32(Bytes& b, std::size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }
}

// A made-up SMDH: the English title and a recognisable icon.
static Bytes MakeSmdh(const char16_t* title, int seed) {
    Bytes smdh(setup::kSmdhSize, 0);
    std::memcpy(smdh.data(), "SMDH", 4);
    std::size_t at = 8 + 0x200 * 1;
    for (const char16_t* c = title; *c; ++c) {
        smdh[at++] = static_cast<uint8_t>(*c & 0xFF);
        smdh[at++] = static_cast<uint8_t>(*c >> 8);
    }
    for (std::size_t i = 0x24C0; i < setup::kSmdhSize; ++i) {
        smdh[i] = static_cast<uint8_t>(i * 13 + static_cast<std::size_t>(seed));
    }
    return smdh;
}

static std::string TitleDir(uint64_t tid) {
    char dir[160];
    std::snprintf(dir, sizeof(dir), "%s/title/%08x/%08x/content", kRoot, static_cast<unsigned>(tid >> 32), static_cast<unsigned>(tid));
    return dir;
}

static std::string RelOf(const std::string& path) {
    return path.substr(std::strlen(kRoot)); // the path under ID1, as the SD layer's counter wants it
}

// Takes/puts the SD layer on a file's bytes (AES-CTR: the same operation both ways).
static void SdLayer(const std::string& path, Bytes& data, const uint32_t rk[44]) {
    uint8_t ctr[16];
    sd_path_ctr(RelOf(path).c_str(), ctr);
    aes_ctr_crypt(rk, ctr, 0, data.data(), data.size());
}

// A TMD of a title whose main content has id `content_id`; `sig_type` is the type of its signature.
static Bytes MakeTmd(uint32_t content_id, uint32_t sig_type = 0x10004) {
    Bytes tmd(0xB34, 0); // 0x140 + 0xC4 + 0x24 * 64 = 0xB04 where the chunk records start, 0x30 for the one chunk
    Be32(tmd, 0, sig_type);
    const std::size_t header = 0x140; // (4 + 0x100 + 0x3F) & ~0x3F for a 0x100-byte signature
    tmd[header + 0x9E] = 0;
    tmd[header + 0x9F] = 1; // one content
    const std::size_t chunk = header + 0xC4 + 0x24 * 64;
    Be32(tmd, chunk, content_id);
    tmd[chunk + 4] = 0;
    tmd[chunk + 5] = 0; // index 0
    return tmd;
}

struct MadeTitle {
    uint64_t tid = 0x000400000016E700ull;
    std::string tmd_name = "00000001.tmd";
    std::string sub;               // a sub-folder of content/ the .app is in (a DLC-like layout)
    bool no_crypto = false;
    bool with_icon = true;
    bool no_exefs = false;         // a DLC: no ExeFS at all
    std::size_t icon_size = setup::kSmdhSize;
    const uint32_t* tmd_key = nullptr; // the key the TMD is encrypted with (default: the SD key)
    uint32_t content_id = 0x2A;
};

// Writes the title as a console would keep it. Returns the plain SMDH planted in it.
static Bytes MakeTitle(const MadeTitle& t, const char16_t* name, int seed) {
    const uint32_t* tmd_rk = t.tmd_key ? t.tmd_key : g_sd_rk;
    const std::string dir = TitleDir(t.tid);
    Bytes tmd = MakeTmd(t.content_id);
    const std::string tmd_path = dir + "/" + t.tmd_name;
    SdLayer(tmd_path, tmd, tmd_rk);
    Put(tmd_path, tmd);

    // The NCCH: header (0x200), then the ExeFS at 0x200: its header (0x200) and the icon file.
    const Bytes smdh = MakeSmdh(name, seed);
    const uint32_t exefs_units = t.no_exefs ? 0 : (1 + (static_cast<uint32_t>(t.icon_size) + 0x1FF) / 0x200);
    Bytes app(0x200 + static_cast<std::size_t>(exefs_units) * 0x200, 0);
    for (int i = 0; i < 16; ++i) {
        app[static_cast<std::size_t>(i)] = static_cast<uint8_t>(0xA0 + i); // the signature: its first 16 bytes are the NCCH key's KeyY
    }
    std::memcpy(&app[0x100], "NCCH", 4);
    const uint8_t part_id[8] = {0x00, 0xE7, 0x16, 0x00, 0x00, 0x00, 0x04, 0x00};
    std::memcpy(&app[0x108], part_id, 8);
    if (t.no_crypto) {
        app[0x18F] = 0x04;
    }
    Le32(app, 0x1A0, 1);
    Le32(app, 0x1A4, exefs_units);
    if (!t.no_exefs) {
        Bytes exefs(static_cast<std::size_t>(exefs_units) * 0x200, 0);
        if (t.with_icon) {
            std::memcpy(&exefs[0], "icon\0\0\0\0", 8);
            Le32(exefs, 8, 0);
            Le32(exefs, 12, static_cast<uint32_t>(t.icon_size));
            std::memcpy(&exefs[0x200], smdh.data(), std::min(t.icon_size, smdh.size()));
        } else {
            std::memcpy(&exefs[0], "icn\0\0\0\0\0", 8); // some other file
            Le32(exefs, 8, 0);
            Le32(exefs, 12, 0x100);
        }
        if (!t.no_crypto) { // the NCCH layer over the ExeFS
            uint8_t key[16];
            ncch_scramble(g_keyx, app.data(), key);
            uint32_t nrk[44];
            aes_set_key(nrk, key);
            uint8_t nctr[16];
            for (int i = 0; i < 8; ++i) {
                nctr[i] = app[0x108 + 7 - static_cast<std::size_t>(i)];
            }
            nctr[8] = 2;
            std::memset(nctr + 9, 0, 7);
            aes_ctr_crypt(nrk, nctr, 0, exefs.data(), exefs.size());
        }
        std::memcpy(&app[0x200], exefs.data(), exefs.size());
    }
    const std::string app_path = dir + (t.sub.empty() ? "" : "/" + t.sub) + "/0000002a.app";
    SdLayer(app_path, app, g_sd_rk);
    Put(app_path, app);
    return smdh;
}

static setup::SdKeys Keys() {
    setup::SdKeys keys;
    keys.sd_round_keys = g_sd_rk;
    keys.ncch_keyx = g_keyx;
    keys.ctr_root = kRoot;
    return keys;
}

static void TestReadsTheWholeFile() {
    fs::remove_all("sdmc:");
    MadeTitle t;
    const Bytes planted = MakeTitle(t, u"Final Fantasy Explorers", 5);
    Bytes out(setup::kSmdhSize, 0);
    CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) == 0);
    CHECK(out == planted); // the whole file, icon included
    setup::GameInfo info;
    CHECK(setup::ParseSmdh(out.data(), out.size(), 1, info) && info.name == "Final Fantasy Explorers");
    CHECK(std::memcmp(info.icon, &planted[0x24C0], sizeof(info.icon)) == 0);
    // the name TitleShare's own reader finds is the same
    char name[64];
    CHECK(ncch_read_name(g_sd_rk, g_keyx, kRoot, t.tid, 1, name, sizeof(name)) == 0);
    CHECK(info.name == name);
    // a buffer that is too small is refused, not overrun
    CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), setup::kSmdhSize - 1) < 0);
    // no keys, no reading
    setup::SdKeys none;
    CHECK(setup::ReadSmdhKeyed(none, t.tid, out.data(), out.size()) < 0);
}

static void TestLayouts() {
    fs::remove_all("sdmc:");
    // the highest TMD wins
    {
        MadeTitle old_tmd;
        old_tmd.tmd_name = "00000000.tmd";
        old_tmd.content_id = 0x99; // points at a content that is not there
        MakeTitle(old_tmd, u"Old", 1);
        MadeTitle t;
        t.tmd_name = "00000007.tmd";
        t.content_id = 0x2A;
        const Bytes planted = MakeTitle(t, u"New", 2);
        Bytes out(setup::kSmdhSize, 0);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) == 0 && out == planted);
    }
    // the content in a sub-folder (as a DLC's are)
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        t.sub = "00000000";
        const Bytes planted = MakeTitle(t, u"Sub", 3);
        Bytes out(setup::kSmdhSize, 0);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) == 0 && out == planted);
    }
    // an NCCH with the "no crypto" flag has nothing to take off but the SD layer
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        t.no_crypto = true;
        const Bytes planted = MakeTitle(t, u"Plain", 4);
        Bytes out(setup::kSmdhSize, 0);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) == 0 && out == planted);
    }
    // two titles side by side do not mix
    {
        fs::remove_all("sdmc:");
        MadeTitle a;
        MadeTitle b;
        b.tid = 0x0004000000177000ull;
        const Bytes pa = MakeTitle(a, u"A game", 6);
        const Bytes pb = MakeTitle(b, u"B game", 7);
        Bytes oa(setup::kSmdhSize, 0), ob(setup::kSmdhSize, 0);
        CHECK(setup::ReadSmdhKeyed(Keys(), a.tid, oa.data(), oa.size()) == 0 && oa == pa);
        CHECK(setup::ReadSmdhKeyed(Keys(), b.tid, ob.data(), ob.size()) == 0 && ob == pb);
        CHECK(oa != ob);
    }
}

static void TestUnreadable() {
    Bytes out(setup::kSmdhSize, 0);
    // no folder for the title
    fs::remove_all("sdmc:");
    CHECK(setup::ReadSmdhKeyed(Keys(), 0x0004000000123400ull, out.data(), out.size()) < 0);
    // a content folder with no TMD
    fs::create_directories(TitleDir(0x0004000000123400ull));
    CHECK(setup::ReadSmdhKeyed(Keys(), 0x0004000000123400ull, out.data(), out.size()) < 0);
    // a TMD of a type nobody knows (what a wrong SD key looks like too)
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        uint8_t wrong[16];
        std::memset(wrong, 0x77, sizeof(wrong));
        uint32_t wrong_rk[44];
        aes_set_key(wrong_rk, wrong);
        t.tmd_key = wrong_rk;
        MakeTitle(t, u"Wrong key", 1);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) < 0);
    }
    // the main content is not there
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        t.content_id = 0x2B; // the .app on the card is 0000002a
        MakeTitle(t, u"Missing content", 1);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) < 0);
    }
    // the NCCH layer's key is not the one the title was made with: the icon does not decode to an SMDH
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        MakeTitle(t, u"Other NCCH key", 1);
        uint8_t other_x[16];
        std::memset(other_x, 0x55, sizeof(other_x));
        setup::SdKeys keys = Keys();
        keys.ncch_keyx = other_x;
        CHECK(setup::ReadSmdhKeyed(keys, t.tid, out.data(), out.size()) < 0);
    }
    // a title without an icon file, a DLC with no ExeFS, an icon file that is too short
    {
        fs::remove_all("sdmc:");
        MadeTitle t;
        t.with_icon = false;
        MakeTitle(t, u"No icon", 1);
        CHECK(setup::ReadSmdhKeyed(Keys(), t.tid, out.data(), out.size()) < 0);
        fs::remove_all("sdmc:");
        MadeTitle dlc;
        dlc.no_exefs = true;
        MakeTitle(dlc, u"DLC", 1);
        CHECK(setup::ReadSmdhKeyed(Keys(), dlc.tid, out.data(), out.size()) == -8);
        fs::remove_all("sdmc:");
        MadeTitle shorty;
        shorty.icon_size = 0x2008; // only the titles, not the icons
        MakeTitle(shorty, u"Short", 1);
        CHECK(setup::ReadSmdhKeyed(Keys(), shorty.tid, out.data(), out.size()) < 0);
    }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_ncch_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    aes_init();
    uint8_t sd_key[16];
    for (int i = 0; i < 16; ++i) {
        sd_key[i] = static_cast<uint8_t>(0x11 + i * 3);
        g_keyx[i] = static_cast<uint8_t>(0x80 ^ (i * 5));
    }
    aes_set_key(g_sd_rk, sd_key);
    TestReadsTheWholeFile();
    TestLayouts();
    TestUnreadable();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
