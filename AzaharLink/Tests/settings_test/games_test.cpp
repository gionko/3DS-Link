// The rules of the "Enable titles for Azahar" page and of the installation check of the 3DS Link app (ConfigApp/source/games.cpp and
// status.cpp, the very files, built on a PC with a stand-in for <3ds.h>), run by Tests/run.sh. The files live in a directory
// called "sdmc:" under a temporary directory, so the app's own paths work unchanged. What it pins: the title id as Luma spells it
// and which titles are games at all; the SMDH (the name in the console's language, the fallbacks, line breaks, surrogates, a title
// without a name is left out) and the layout of its icon for the GPU; the cache of the games (written whole, read back whole, a
// changed, cut, longer or foreign file is no cache); the order of the list; what a game's folder says (off, on,
// another plugin); enabling (the folder and the copy, the same size as the source, never a half copy), disabling (the copy and
// the empty folder go, another file is never touched), bringing a stale copy up to date; the installation check (every file
// that is missing says what to copy where, what is only optional is never counted); the Plugin Loader question; the short texts.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "games.hpp"
#include "settings.hpp"
#include "status.hpp"

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
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/luma", 0777);
    mkdir("sdmc:/luma/plugins", 0777);
}

// A plugin-like file of `size` bytes whose content depends on `seed`.
static std::string Plugin(std::size_t size, char seed) {
    std::string s = "3GX$";
    while (s.size() < size) {
        s += static_cast<char>(seed + static_cast<char>(s.size() % 7));
    }
    return s;
}

// ---------------------------------------------------------------------------------------------

static void TestTitleIds() {
    CHECK(setup::TidText(0x000400000016E700ull) == "000400000016E700");
    CHECK(setup::TidText(0x0004000000177000ull) == "0004000000177000");
    CHECK(setup::TidText(0x00040000FFFFFFFFull) == "00040000FFFFFFFF");
    CHECK(setup::TidText(0) == "0000000000000000");
    CHECK(setup::TidText(0x0004000e00175e00ull) == "0004000E00175E00"); // upper case, whatever the number
    CHECK(setup::TidText(0x000400000016E700ull).size() == 16);

    CHECK(setup::IsApplicationTid(0x000400000016E700ull));
    CHECK(!setup::IsApplicationTid(0x0004000E0016E700ull)); // update
    CHECK(!setup::IsApplicationTid(0x0004008C0016E700ull)); // DLC
    CHECK(!setup::IsApplicationTid(0x0004001B00010002ull)); // system applet
    CHECK(!setup::IsApplicationTid(0x0004001000020000ull)); // system application
    CHECK(!setup::IsApplicationTid(0x0004800400000000ull)); // DSiWare
    CHECK(!setup::IsApplicationTid(0x0004013000001702ull)); // a NAND module
    CHECK(!setup::IsApplicationTid(0x0000000000000000ull));

    CHECK(setup::GamePluginDir(0x000400000016E700ull) == "sdmc:/luma/plugins/000400000016E700");
    CHECK(setup::GamePluginPath(0x000400000016E700ull) == "sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx");
}

// ---------------------------------------------------------------------------------------------

// An SMDH with the given short titles by language (index -> UTF-16 text given as UTF-16 code units).
struct Smdh {
    std::string bytes;
    Smdh() : bytes(setup::kSmdhSize, '\0') {
        std::memcpy(&bytes[0], "SMDH", 4);
    }
    void Title(int lang, const std::u16string& text) {
        Put16(8 + 0x200 * static_cast<std::size_t>(lang), text);
    }
    // The long title of the language (0x80 bytes after the short one): the whole name of the title.
    void LongTitle(int lang, const std::u16string& text) {
        Put16(8 + 0x200 * static_cast<std::size_t>(lang) + 0x80, text);
    }
    void Put16(std::size_t at, const std::u16string& text) {
        for (char16_t c : text) {
            bytes[at++] = static_cast<char>(c & 0xFF);
            bytes[at++] = static_cast<char>(c >> 8);
        }
    }
    void Icons() {
        for (std::size_t i = 0; i < 24 * 24; ++i) { // the small icon: not kept by the app
            bytes[0x2040 + 2 * i] = static_cast<char>(i & 0xFF);
            bytes[0x2040 + 2 * i + 1] = static_cast<char>((i >> 8) | 0x80);
        }
        for (std::size_t i = 0; i < 48 * 48; ++i) {
            bytes[0x24C0 + 2 * i] = static_cast<char>((i * 3) & 0xFF);
            bytes[0x24C0 + 2 * i + 1] = static_cast<char>((i >> 8) | 0x40);
        }
    }
    bool Parse(int lang, setup::GameInfo& info) const {
        return setup::ParseSmdh(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), lang, info);
    }
};

static void TestSmdh() {
    setup::GameInfo info;
    {
        Smdh s;
        s.Title(1, u"Final Fantasy Explorers");
        s.Icons();
        CHECK(s.Parse(1, info) && info.name == "Final Fantasy Explorers");
        // the large icon is copied as it is, whole
        bool same = true;
        for (int i = 0; i < 48 * 48; ++i) {
            u16 want;
            std::memcpy(&want, &s.bytes[0x24C0 + 2 * static_cast<std::size_t>(i)], 2);
            same = same && info.icon[i] == want;
        }
        CHECK(same);
    }
    { // the console's language first, then the fallbacks
        Smdh s;
        s.Title(1, u"English");
        s.Title(3, u"Deutsch");
        CHECK(s.Parse(3, info) && info.name == "Deutsch");
        CHECK(s.Parse(1, info) && info.name == "English");
        CHECK(s.Parse(2, info) && info.name == "English"); // no French title: English
        CHECK(s.Parse(99, info) && info.name == "English"); // not a language: English
        CHECK(s.Parse(-1, info) && info.name == "English");
    }
    { // only a Japanese title (index 0), or only a Dutch one (8)
        Smdh s;
        s.Title(0, u"ゼルダ"); // three kana
        CHECK(s.Parse(1, info) && info.name == "\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80");
        Smdh d;
        d.Title(8, u"Nederlands");
        CHECK(d.Parse(1, info) && info.name == "Nederlands");
    }
    { // a title on two lines, runs of spaces, an accent
        Smdh s;
        s.Title(1, u"  Pokémon\nUltra  Sun \r");
        CHECK(s.Parse(1, info));
        CHECK(info.name == "Pok\xC3\xA9mon Ultra Sun");
    }
    { // a pair of surrogates (an emoji-like character) and a lone one
        Smdh s;
        s.Title(1, std::u16string(u"A") + u"\xD83C\xDFAE" + u"B");
        CHECK(s.Parse(1, info) && info.name == "A\xF0\x9F\x8E\xAE" "B");
        Smdh l;
        l.Title(1, std::u16string(u"A") + u"\xD83C" + u"B");
        CHECK(l.Parse(1, info) && info.name == "A\xEF\xBF\xBD" "B");
    }
    { // not recognised: no name at all, no magic, too short
        Smdh none;
        CHECK(!none.Parse(1, info));
        Smdh blank;
        blank.Title(1, u"   \n ");
        CHECK(!blank.Parse(1, info));
        Smdh bad;
        bad.Title(1, u"Name");
        bad.bytes[0] = 'X';
        CHECK(!bad.Parse(1, info));
        Smdh good;
        good.Title(1, u"Name");
        CHECK(!setup::ParseSmdh(reinterpret_cast<const u8*>(good.bytes.data()), setup::kSmdhSize - 1, 1, info));
        CHECK(!setup::ParseSmdh(nullptr, 0, 1, info));
    }
    { // the short name is read up to its 64 characters, whatever follows
        Smdh s;
        s.Title(1, std::u16string(64, u'x'));
        CHECK(s.Parse(1, info) && info.name.size() == 64);
    }
    { // the list shows the LONG title (the whole name); the short one only when there is no long one
        Fresh();
        Smdh s;
        s.Title(1, u"Tri Force Heroes");
        s.LongTitle(1, u"The Legend of Zelda: Tri Force Heroes");
        CHECK(s.Parse(1, info) && info.name == "The Legend of Zelda: Tri Force Heroes");
        Smdh only_short;
        only_short.Title(1, u"Short Only");
        CHECK(only_short.Parse(1, info) && info.name == "Short Only");
        Smdh only_long;
        only_long.LongTitle(1, u"Only A Long Title");
        CHECK(only_long.Parse(1, info) && info.name == "Only A Long Title");
        // a language with a short title but no long one is still that language's title (the language comes first)
        Smdh mixed;
        mixed.Title(3, u"Deutsch kurz");
        mixed.LongTitle(1, u"English long title");
        CHECK(mixed.Parse(3, info) && info.name == "Deutsch kurz");
        CHECK(mixed.Parse(1, info) && info.name == "English long title");
        // the long title is read up to its 128 characters
        Smdh long128;
        long128.LongTitle(1, std::u16string(128, u'y'));
        CHECK(long128.Parse(1, info) && info.name.size() == 128);
        // and it goes through the cache whole
        std::vector<setup::GameInfo> in(1), out;
        in[0].tid = 0x0004000000177000ull;
        in[0].version = 3;
        in[0].name = std::string(128, 'y');
        CHECK(setup::SaveGameCache("sdmc:/3ds/long.cache", 1, in));
        CHECK(setup::LoadGameCache("sdmc:/3ds/long.cache", 1, out) && out.size() == 1 && out[0].name == in[0].name);
        std::string three_byte;
        for (int i = 0; i < 128; ++i) {
            three_byte += "\xE3\x82\xBC"; // the longest name the SMDH can hold in UTF-8: 128 characters of 3 bytes
        }
        in[0].name = three_byte;
        CHECK(three_byte.size() == 384 && setup::SaveGameCache("sdmc:/3ds/long.cache", 1, in));
        CHECK(setup::LoadGameCache("sdmc:/3ds/long.cache", 1, out) && out.size() == 1 && out[0].name == three_byte);
    }
}

static void TestOrder() {
    CHECK(setup::NameLess("apple", "Banana"));
    CHECK(!setup::NameLess("Banana", "apple"));
    CHECK(setup::NameLess("Zelda", "zora")); // upper and lower case alike
    CHECK(!setup::NameLess("zora", "Zelda"));
    CHECK(!setup::NameLess("same", "SAME") && !setup::NameLess("SAME", "same")); // equal
    CHECK(setup::NameLess("Pokemon", "Pok\xC3\xA9mon Y") && !setup::NameLess("Pok\xC3\xA9mon Y", "Pokemon")); // the accent is the plain letter
    CHECK(setup::NameLess("\xC3\x89" "tude", "Fable") && setup::NameLess("Eagle", "\xC3\x89" "tude")); // E-acute sorts with E
    CHECK(setup::NameLess("a", "ab") && !setup::NameLess("ab", "a"));
    CHECK(setup::NameLess("Mario Kart 7", "Mario Party")); // 'K' < 'P'
    CHECK(setup::NameLess("1-2-Switch", "A")); // digits before letters
}

static void TestIconLayout() {
    // 24x24 into 32x32: every tile of the file lands in the same row and column of tiles of the texture (4 tiles per row there)
    u16 tiled[24 * 24];
    for (int i = 0; i < 24 * 24; ++i) {
        tiled[i] = static_cast<u16>(i + 1);
    }
    u16 out[32 * 32];
    setup::LayoutIcon(tiled, 24, 32, out);
    for (int tile_row = 0; tile_row < 3; ++tile_row) {
        for (int tile = 0; tile < 3; ++tile) {
            for (int k = 0; k < 64; ++k) {
                const u16 want = tiled[(tile_row * 3 + tile) * 64 + k];
                const u16 got = out[(tile_row * 4 + tile) * 64 + k];
                CHECK(got == want);
            }
        }
    }
    for (int i = 3 * 4 * 64; i < 4 * 4 * 64; ++i) {
        CHECK(out[i] == 0); // the last row of tiles is padding
    }
    for (int tile_row = 0; tile_row < 4; ++tile_row) {
        for (int k = 0; k < 64; ++k) {
            CHECK(out[(tile_row * 4 + 3) * 64 + k] == 0); // the fourth column of tiles is padding too
        }
    }
    // 48 into 64
    static u16 big[48 * 48];
    static u16 big_out[64 * 64];
    for (int i = 0; i < 48 * 48; ++i) {
        big[i] = static_cast<u16>(i * 7 + 1);
    }
    setup::LayoutIcon(big, 48, 64, big_out);
    int mismatches = 0;
    for (int tile_row = 0; tile_row < 6; ++tile_row) {
        for (int tile = 0; tile < 6; ++tile) {
            for (int k = 0; k < 64; ++k) {
                if (big_out[(tile_row * 8 + tile) * 64 + k] != big[(tile_row * 6 + tile) * 64 + k]) {
                    ++mismatches;
                }
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(big_out[6 * 8 * 64] == 0 && big_out[8 * 8 * 64 - 1] == 0); // two rows of tiles of padding at the end
    CHECK(big_out[6 * 64] == 0 && big_out[7 * 64 + 63] == 0);      // two columns of tiles of padding after the sixth
}

// ---------------------------------------------------------------------------------------------

static setup::GameInfo MakeGame(u64 tid, u16 version, const std::string& name, int seed) {
    setup::GameInfo g;
    g.tid = tid;
    g.version = version;
    g.name = name;
    for (int i = 0; i < 48 * 48; ++i) {
        g.icon[i] = static_cast<u16>(seed * 31 + i * 7);
    }
    return g;
}

static bool SameGame(const setup::GameInfo& a, const setup::GameInfo& b) {
    return a.tid == b.tid && a.version == b.version && a.name == b.name && std::memcmp(a.icon, b.icon, sizeof(a.icon)) == 0;
}

static void TestGameCache() {
    Fresh();
    const std::string path = "sdmc:/3ds/3DSLink.games";
    std::vector<setup::GameInfo> in;
    in.push_back(MakeGame(0x000400000016E700ull, 3, "Final Fantasy Explorers", 1));
    in.push_back(MakeGame(0x0004000000177000ull, 0x1234, "Pok\xC3\xA9mon X", 2));
    in.push_back(MakeGame(0x00040000FFFFFFFFull, 0xFFFF, std::string(64 * 3, 'x'), 3)); // the longest name an SMDH can give
    CHECK(setup::SaveGameCache(path.c_str(), 1, in));
    CHECK(!Exists(path + ".tmp"));
    std::vector<setup::GameInfo> out;
    CHECK(setup::LoadGameCache(path.c_str(), 1, out));
    CHECK(out.size() == 3 && SameGame(out[0], in[0]) && SameGame(out[1], in[1]) && SameGame(out[2], in[2]));
    CHECK(!setup::LoadGameCache(path.c_str(), 2, out) && out.empty()); // the names depend on the language: another language is not this cache
    // replaced by a newer one
    in.pop_back();
    CHECK(setup::SaveGameCache(path.c_str(), 1, in));
    CHECK(setup::LoadGameCache(path.c_str(), 1, out) && out.size() == 2);
    // the empty cache is a cache
    CHECK(setup::SaveGameCache(path.c_str(), 1, {}));
    CHECK(setup::LoadGameCache(path.c_str(), 1, out) && out.empty());

    // a file that is not whole, or not ours, is no cache at all (and gives no half list)
    CHECK(setup::SaveGameCache(path.c_str(), 1, in));
    const std::string raw = Get(path);
    CHECK(raw.size() == 20 + 2 * (8 + 2 + setup::kCachedNameBytes + 2 * 48 * 48));
    Put(path, raw.substr(0, raw.size() - 1));
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty()); // cut
    Put(path, raw + "x");
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty()); // longer than it was written
    std::string flipped = raw;
    flipped[raw.size() / 2] = static_cast<char>(flipped[raw.size() / 2] ^ 0x10);
    Put(path, flipped);
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty()); // a changed byte (the check value)
    std::string alien = raw;
    alien[0] = 'X';
    Put(path, alien);
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty()); // not the magic
    std::string huge = raw;
    huge[12] = static_cast<char>(0xFF);
    huge[13] = static_cast<char>(0xFF);
    Put(path, huge);
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty()); // a count no card holds
    std::string lying = raw;
    lying[12] = static_cast<char>(0xF0); // a count the card could hold (4080) that the file's length does not bear out: refused before memory is asked for it
    lying[13] = 0;
    Put(path, lying);
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out) && out.empty());
    Put(path, "");
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out));
    remove(path.c_str());
    CHECK(!setup::LoadGameCache(path.c_str(), 1, out)); // no file
    // a card that cannot be written: false, no leftover
    CHECK(!setup::SaveGameCache("sdmc:/nodir/3DSLink.games", 1, in));
}

// ---------------------------------------------------------------------------------------------

static void TestClassifyFolder() {
    using setup::Slot;
    CHECK(setup::ClassifyFolder({}) == Slot::Off);
    CHECK(setup::ClassifyFolder({"AzaharLink.3gx"}) == Slot::On);
    CHECK(setup::ClassifyFolder({"azaharlink.3GX"}) == Slot::On); // the card does not care about case
    CHECK(setup::ClassifyFolder({"AZAHARLINK.3GX"}) == Slot::On);
    CHECK(setup::ClassifyFolder({"Other.3gx"}) == Slot::Other);
    CHECK(setup::ClassifyFolder({"AzaharLink.3gx", "Other.3gx"}) == Slot::Other); // two plugins: not ours to say which loads
    CHECK(setup::ClassifyFolder({"AzaharLink.3gx.part"}) == Slot::Off); // a copy that was cut short is not a plugin
    CHECK(setup::ClassifyFolder({"readme.txt", "cheats.txt"}) == Slot::Off);
    CHECK(setup::ClassifyFolder({"AzaharLink.3gx", "readme.txt"}) == Slot::On);
    CHECK(setup::ClassifyFolder({".3gx"}) == Slot::Off); // no name at all
    CHECK(setup::ClassifyFolder({"x.3gx"}) == Slot::Other);
    CHECK(setup::ClassifyFolder({"AzaharLink.3gx2"}) == Slot::Off);
    CHECK(setup::ClassifyFolder({"AzaharLink-0.1.16.3gx"}) == Slot::Other); // an older copy under another name is another plugin to us
}

static void TestEnableDisable() {
    Fresh();
    const u64 ffe = 0x000400000016E700ull;
    const u64 tfh = 0x0004000000177000ull;
    const std::string source = "sdmc:/3ds/AzaharLink.3gx";
    const std::string content = Plugin(70000, 'a'); // more than one 32 KB block, not a multiple of one

    CHECK(setup::ReadSlot(ffe).slot == setup::Slot::Off);
    // without the source nothing is made
    CHECK(setup::EnableGame(ffe, source.c_str()) == setup::EnableResult::SourceMissing);
    CHECK(!Exists(setup::GamePluginDir(ffe)));

    Put(source, content);
    CHECK(setup::EnableGame(ffe, source.c_str()) == setup::EnableResult::Ok);
    CHECK(Exists("sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx"));
    CHECK(Get("sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx") == content); // the same bytes, so the same size
    CHECK(!Exists("sdmc:/luma/plugins/000400000016E700/AzaharLink.3gx.part"));
    const setup::SlotInfo on = setup::ReadSlot(ffe);
    CHECK(on.slot == setup::Slot::On && on.copy_size == content.size());
    CHECK(setup::ReadSlot(tfh).slot == setup::Slot::Off); // the other game is untouched
    CHECK(!Exists(setup::GamePluginDir(tfh)));

    // enabled twice is the same
    CHECK(setup::EnableGame(ffe, source.c_str()) == setup::EnableResult::Ok);
    CHECK(Get(setup::GamePluginPath(ffe)) == content);

    // the list of folders names the games that have one
    const std::set<std::string> folders = setup::ListPluginFolders();
    CHECK(folders.count("000400000016E700") == 1 && folders.size() == 1);

    // the plugin file of the app is replaced: the copy is stale until it is made again
    const std::string newer = Plugin(70500, 'k');
    Put(source, newer);
    CHECK(setup::CopyIsStale(setup::ReadSlot(ffe).copy_size, newer.size()));
    CHECK(!setup::CopyIsStale(setup::ReadSlot(ffe).copy_size, content.size()));
    CHECK(setup::EnableGame(ffe, source.c_str()) == setup::EnableResult::Ok);
    CHECK(Get(setup::GamePluginPath(ffe)) == newer);
    CHECK(!setup::CopyIsStale(setup::ReadSlot(ffe).copy_size, newer.size()));

    // disabling removes the copy and then the empty folder
    CHECK(setup::DisableGame(ffe));
    CHECK(!Exists(setup::GamePluginPath(ffe)));
    CHECK(!Exists(setup::GamePluginDir(ffe)));
    CHECK(setup::ReadSlot(ffe).slot == setup::Slot::Off);
    CHECK(setup::DisableGame(ffe)); // again: nothing to do, fine
    CHECK(setup::ListPluginFolders().empty());

    // a folder with something else in it keeps the folder (and the something else)
    mkdir(setup::GamePluginDir(tfh).c_str(), 0777);
    Put(setup::GamePluginDir(tfh) + "/notes.txt", "mine");
    CHECK(setup::EnableGame(tfh, source.c_str()) == setup::EnableResult::Ok);
    CHECK(setup::DisableGame(tfh));
    CHECK(Exists(setup::GamePluginDir(tfh)) && Get(setup::GamePluginDir(tfh) + "/notes.txt") == "mine");
    CHECK(!Exists(setup::GamePluginPath(tfh)));

    // a cut copy is cleaned up with the folder
    Put(setup::GamePluginPath(tfh) + ".part", "cut");
    Put(setup::GamePluginDir(tfh) + "/notes.txt", "mine");
    remove((setup::GamePluginDir(tfh) + "/notes.txt").c_str());
    CHECK(setup::ReadSlot(tfh).slot == setup::Slot::Off);
    CHECK(setup::DisableGame(tfh));
    CHECK(!Exists(setup::GamePluginDir(tfh)));
}

static void TestOtherPlugin() {
    Fresh();
    const u64 id = 0x0004000000055D00ull;
    const std::string source = "sdmc:/3ds/AzaharLink.3gx";
    Put(source, Plugin(5000, 'a'));
    mkdir(setup::GamePluginDir(id).c_str(), 0777);
    Put(setup::GamePluginDir(id) + "/Cheats.3gx", Plugin(300, 'q'));
    CHECK(setup::ReadSlot(id).slot == setup::Slot::Other);
    CHECK(setup::EnableGame(id, source.c_str()) == setup::EnableResult::OtherPlugin);
    CHECK(!Exists(setup::GamePluginPath(id)));
    CHECK(Get(setup::GamePluginDir(id) + "/Cheats.3gx") == Plugin(300, 'q')); // untouched
    // a sub-folder called x.3gx is not a plugin
    Fresh();
    mkdir(setup::GamePluginDir(id).c_str(), 0777);
    mkdir((setup::GamePluginDir(id) + "/x.3gx").c_str(), 0777);
    CHECK(setup::ReadSlot(id).slot == setup::Slot::Off);
}

static void TestCopyIsNeverHalf() {
    Fresh();
    // the source cannot be read: no destination, no leftover
    CHECK(!setup::CopyPluginFile("sdmc:/3ds/none.3gx", "sdmc:/luma/plugins/x.3gx"));
    CHECK(!Exists("sdmc:/luma/plugins/x.3gx") && !Exists("sdmc:/luma/plugins/x.3gx.part"));
    // an empty source is not a plugin
    Put("sdmc:/3ds/empty.3gx", "");
    CHECK(!setup::CopyPluginFile("sdmc:/3ds/empty.3gx", "sdmc:/luma/plugins/x.3gx"));
    CHECK(!Exists("sdmc:/luma/plugins/x.3gx") && !Exists("sdmc:/luma/plugins/x.3gx.part"));
    // a destination that cannot be made leaves the old copy as it was
    Put("sdmc:/3ds/p.3gx", Plugin(100, 'a'));
    Put("sdmc:/luma/plugins/old.3gx", "old");
    CHECK(!setup::CopyPluginFile("sdmc:/3ds/p.3gx", "sdmc:/luma/plugins/nodir/new.3gx"));
    CHECK(Get("sdmc:/luma/plugins/old.3gx") == "old");
    // a copy over an old one replaces it whole
    CHECK(setup::CopyPluginFile("sdmc:/3ds/p.3gx", "sdmc:/luma/plugins/old.3gx"));
    CHECK(Get("sdmc:/luma/plugins/old.3gx") == Plugin(100, 'a'));
    CHECK(!Exists("sdmc:/luma/plugins/old.3gx.part"));
}

// ---------------------------------------------------------------------------------------------

static setup::InstallFacts GoodFacts() {
    setup::InstallFacts f;
    f.plugin_path = "sdmc:/games/3ds/AzaharLink.3gx"; // (the app need not be in sdmc:/3ds: the check says where its own file is)
    f.plugin_card = setup::PluginCard::Same; // (the file on the card is, byte for byte, the plugin inside the app)
    f.embedded_ok = true;
    f.plugin_size = 1048576 + 20000;
    f.plugin_version = (0u << 24) | (1u << 16) | (17u << 8);
    f.plugins_dir = true;
    f.default_plugin = false;
    f.boot9 = true;
    f.boot9_path = "sdmc:/gm9/out/boot9.bin";
    f.sd_writable = true;
    return f;
}

static const setup::CheckItem* Find(const setup::InstallReport& r, const char* label) {
    for (const setup::CheckItem& item : r.items) {
        if (item.label == label) {
            return &item;
        }
    }
    return nullptr;
}

static void TestInstallCheck() {
    using setup::CheckAction;
    using setup::CheckLevel;
    {
        const setup::InstallReport r = setup::EvaluateInstall(GoodFacts(), "0.1.17");
        CHECK(r.problems == 0 && setup::InstallSummary(r) == "All OK");
        CHECK(r.items.size() == 4); // plugin file, plugin folder, SD card, boot9.bin: no settings-file row, no default.3gx row, no titles row before the scan
        for (const setup::CheckItem& item : r.items) {
            CHECK(item.level == CheckLevel::Ok && item.action == CheckAction::None);
        }
        CHECK(Find(r, "Plugin file") && Find(r, "Plugin file")->value == "v0.1.17, 1.02 MB");
        CHECK(Find(r, "Plugin file")->note.find("sdmc:/games/3ds/AzaharLink.3gx") == 0); // the note names the file where it is
        CHECK(Find(r, "Plugin file")->note.find("byte for byte the plugin inside this app") != std::string::npos);
        CHECK(!Find(r, "Settings file") && !Find(r, "Plugin copy") && !Find(r, "default.3gx"));
        // boot9.bin says where it is
        CHECK(Find(r, "boot9.bin")->value == "sdmc:/gm9/out" && Find(r, "boot9.bin")->note.find("sdmc:/gm9/out/boot9.bin") == 0);
        const setup::InstallLine line = setup::InstallLineText(r);
        CHECK(line.full == "All OK" && line.compact == "All OK");
    }
    { // no plugin file: says where it is expected and that the app puts it there by itself, and A puts it from the app (nothing is searched)
        setup::InstallFacts f = GoodFacts();
        f.plugin_card = setup::PluginCard::Missing;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && setup::InstallSummary(r) == "1 problem");
        const setup::CheckItem* item = Find(r, "Plugin file");
        CHECK(item && item->level == CheckLevel::Problem && item->value == "missing" && item->action == CheckAction::RestorePlugin);
        CHECK(item && item->note.find("sdmc:/games/3ds/AzaharLink.3gx") != std::string::npos && item->note.find("inside") != std::string::npos);
        CHECK(item && item->note.find("by itself at every start") != std::string::npos && item->note.find("A: put it there now") != std::string::npos);
        CHECK(item && item->note.find("search") == std::string::npos && item->note.find("sd:/3ds") == std::string::npos); // no search, and no folder is fixed in the text
        CHECK(item && item->note.find("could not") == std::string::npos); // (the app has not tried and failed: nothing is blamed)
        CHECK(item && item->short_problem == "plugin file missing");
        // the app tried to put it and the card refused: the note says why, in the app's words
        f.plugin_tried = true;
        f.plugin_write = setup::WriteResult::CannotWrite;
        const setup::InstallReport tried = setup::EvaluateInstall(f, "0.1.17");
        CHECK(Find(tried, "Plugin file")->note.find("tried to put it there and could not: the SD card refused the file (is it full or locked?)") != std::string::npos);
        CHECK(Find(tried, "Plugin file")->action == CheckAction::RestorePlugin);
    }
    { // a file that is not a plugin, one that cannot be read, a header that looks good over other bytes, another version: each is replaced from the app, and says what it is
        setup::InstallFacts f = GoodFacts();
        f.plugin_card = setup::PluginCard::NotPlugin;
        setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->value == "not a plugin" && Find(r, "Plugin file")->action == CheckAction::RestorePlugin);
        CHECK(Find(r, "Plugin file")->note.find("does not start like a .3gx plugin") != std::string::npos);
        f.plugin_card = setup::PluginCard::Unreadable;
        r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->value == "unreadable" && Find(r, "Plugin file")->action == CheckAction::RestorePlugin);
        f.plugin_card = setup::PluginCard::Altered;
        r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->value == "damaged" && Find(r, "Plugin file")->action == CheckAction::RestorePlugin);
        CHECK(Find(r, "Plugin file")->note.find("0.1.17") != std::string::npos && Find(r, "Plugin file")->note.find("byte for byte") != std::string::npos);
        CHECK(Find(r, "Plugin file")->short_problem == "plugin file damaged");
        f = GoodFacts();
        f.plugin_card = setup::PluginCard::OtherVersion;
        f.plugin_version = (0u << 24) | (1u << 16) | (16u << 8);
        r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->level == CheckLevel::Problem && Find(r, "Plugin file")->action == CheckAction::RestorePlugin);
        CHECK(Find(r, "Plugin file")->value == "v0.1.16 (app v0.1.17)" && Find(r, "Plugin file")->short_problem == "plugin file is another version");
        CHECK(Find(r, "Plugin file")->note.find("0.1.16") != std::string::npos && Find(r, "Plugin file")->note.find("0.1.17") != std::string::npos);
        CHECK(Find(r, "Plugin file")->note.find("one version") != std::string::npos);
    }
    { // a build with no plugin inside: a problem with nothing to press (it cannot put a plugin it does not have), whatever the card holds
        setup::InstallFacts f = GoodFacts();
        f.embedded_ok = false;
        setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->value == "none in this app" && Find(r, "Plugin file")->action == CheckAction::None);
        CHECK(Find(r, "Plugin file")->short_problem == "no plugin in the app" && Find(r, "Plugin file")->note.find("Install the 3DS Link CIA again") != std::string::npos);
        f.plugin_card = setup::PluginCard::Missing;
        r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Plugin file")->action == CheckAction::None);
    }
    { // no plugin folder: ONE problem, and A makes it
        setup::InstallFacts f = GoodFacts();
        f.plugins_dir = false;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "Settings file") == nullptr); // (the settings file is made by the app itself: it has no row)
        const setup::CheckItem* item = Find(r, "Plugin folder");
        CHECK(item && item->level == CheckLevel::Problem && item->action == CheckAction::CreatePluginsDir && item->short_problem == "plugin folder missing");
        CHECK(item && item->note.find("sdmc:/luma/plugins") != std::string::npos);
    }
    { // boot9.bin missing is a WARNING (3DS Link works without it: only sending needs it): the main page names it, A finds it
        setup::InstallFacts f = GoodFacts();
        f.boot9 = false;
        f.boot9_path.clear();
        f.app_dir = "sdmc:/games/3ds";
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 0 && r.warnings == 1 && setup::InstallSummary(r) == "1 warning");
        const setup::CheckItem* item = Find(r, "boot9.bin");
        CHECK(item && item->level == CheckLevel::Warning && item->value == "missing" && item->action == CheckAction::FindBoot9);
        CHECK(item && item->note_title == "boot9.bin is missing");
        CHECK(item && item->note.find("receiving works without it") != std::string::npos);
        CHECK(item && item->note.find("Not found next to 3DS Link (sdmc:/games/3ds)") != std::string::npos && item->note.find("sdmc:/gm9/out/") != std::string::npos);
        CHECK(item && item->note.find("TitleShare") == std::string::npos);
        // what it is for comes before where it was looked for, and what A does comes last
        CHECK(item && item->note.find("key file") < item->note.find("Not found") && item->note.find("Not found") < item->note.find("A: search"));
        CHECK(item && item->short_problem == "boot9.bin missing");
        const setup::InstallLine line = setup::InstallLineText(r);
        CHECK(line.full == "boot9.bin missing - A on Installation check" && line.compact == "1 warning - A on Installation check");
        // the rows of the check: the warning first (nothing is red), then what is fine, in their own order
        const std::vector<std::size_t> order = setup::InstallRowOrder(r);
        CHECK(order.size() == r.items.size() && r.items[order[0]].label == "boot9.bin" && r.items[order[1]].label == "Plugin file");
    }
    { // the main page's line: the problems first, then the warnings, each in a few words, and a count when they are many
        setup::InstallFacts f = GoodFacts();
        f.plugin_card = setup::PluginCard::Missing;
        f.plugins_dir = false;
        f.boot9 = false;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 2 && r.warnings == 1 && setup::InstallSummary(r) == "2 problems, 1 warning");
        const setup::InstallLine line = setup::InstallLineText(r);
        CHECK(line.full == "plugin file missing, plugin folder missing, boot9.bin missing - A on Installation check");
        CHECK(line.compact == "2 problems, 1 warning - A on Installation check");
        // the order of the rows: both problems (in the order of the check), then the warning, then what is fine
        const std::vector<std::size_t> order = setup::InstallRowOrder(r);
        CHECK(order.size() == 4);
        CHECK(r.items[order[0]].label == "Plugin file" && r.items[order[1]].label == "Plugin folder" && r.items[order[2]].label == "boot9.bin" &&
              r.items[order[3]].label == "SD card");
        // the words of the summary: the singular and the plural, problems before warnings
        setup::InstallReport counted;
        counted.problems = 1;
        CHECK(setup::InstallSummary(counted) == "1 problem");
        counted.warnings = 2;
        CHECK(setup::InstallSummary(counted) == "1 problem, 2 warnings");
        counted.problems = 0;
        counted.warnings = 0;
        CHECK(setup::InstallSummary(counted) == "All OK");
        CHECK(setup::SeverityRank(CheckLevel::Problem) < setup::SeverityRank(CheckLevel::Warning) &&
              setup::SeverityRank(CheckLevel::Warning) < setup::SeverityRank(CheckLevel::Ok) &&
              setup::SeverityRank(CheckLevel::Ok) < setup::SeverityRank(CheckLevel::Optional));
    }
    { // default.3gx: information, never counted, and it says that 3DS Link does not use it
        setup::InstallFacts f = GoodFacts();
        f.default_plugin = true;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 0 && r.items.size() == 5);
        const setup::CheckItem* item = Find(r, "default.3gx");
        CHECK(item && item->level == CheckLevel::Optional && item->action == CheckAction::None);
        CHECK(item && item->note.find("default.3gx would apply to every title; 3DS Link does not use it") != std::string::npos);
    }
    { // the seed database: readable is fine, not readable is a warning (nothing to fix from the app), not asked is no row
        setup::InstallFacts f = GoodFacts();
        f.seed = 1;
        f.seed_size = 31 * 1024;
        setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 0 && r.warnings == 0 && r.items.size() == 5);
        CHECK(Find(r, "Seed database") && Find(r, "Seed database")->level == CheckLevel::Ok && Find(r, "Seed database")->value == "31.0 KB");
        f.seed = 2;
        f.seed_error = 0xC8804478u;
        r = setup::EvaluateInstall(f, "0.1.17");
        const setup::CheckItem* seed = Find(r, "Seed database");
        CHECK(r.problems == 0 && r.warnings == 1 && seed && seed->level == CheckLevel::Warning && seed->action == CheckAction::None);
        CHECK(seed && seed->value == "not readable" && seed->note.find("0xC8804478") != std::string::npos && seed->short_problem == "seed database not readable");
        CHECK(!Find(setup::EvaluateInstall(GoodFacts(), "0.1.17"), "Seed database"));
    }
    { // Rosalina's Plugin Loader: on is fine, off is a problem that A fixes, unknown (no service) is no row
        setup::InstallFacts f = GoodFacts();
        f.loader = setup::LoaderState::Enabled;
        setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 0 && Find(r, "Plugin Loader") && Find(r, "Plugin Loader")->level == CheckLevel::Ok && Find(r, "Plugin Loader")->value == "enabled");
        f.loader = setup::LoaderState::Disabled;
        r = setup::EvaluateInstall(f, "0.1.17");
        const setup::CheckItem* loader = Find(r, "Plugin Loader");
        CHECK(r.problems == 1 && loader && loader->level == CheckLevel::Problem && loader->action == CheckAction::EnableLoader);
        CHECK(loader && loader->short_problem == "Plugin Loader off");
        f.loader = setup::LoaderState::Unknown;
        CHECK(!Find(setup::EvaluateInstall(f, "0.1.17"), "Plugin Loader"));
    }
    { // a card that cannot be written
        setup::InstallFacts f = GoodFacts();
        f.sd_writable = false;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 1 && Find(r, "SD card")->value == "read-only" && Find(r, "SD card")->action == CheckAction::None);
        CHECK(setup::InstallLineText(r).full == "SD card read-only - A on Installation check");
    }
    { // everything wrong: N problems, in the plural
        setup::InstallFacts f;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 3 && r.warnings == 1 && setup::InstallSummary(r) == "3 problems, 1 warning"); // plugin file, plugin folder, SD card; boot9.bin
    }
    // the probe: a folder that can be written, one that cannot be made
    Fresh();
    CHECK(setup::ProbeWritable("sdmc:/3ds"));
    CHECK(!Exists("sdmc:/3ds/.3dslink_write_test"));
    Put("sdmc:/blocker", "file");
    CHECK(!setup::ProbeWritable("sdmc:/blocker/sub"));
}

static void TestLoader() {
    using setup::LoaderState;
    CHECK(std::strcmp(setup::LoaderStateText(LoaderState::Enabled), "Enabled") == 0);
    CHECK(std::strcmp(setup::LoaderStateText(LoaderState::Disabled), "Disabled") == 0);
    CHECK(std::strcmp(setup::LoaderStateText(LoaderState::Unknown), "unknown") == 0);
    // asked only for a loader that is known to be off, and only when 3DS Link is on
    CHECK(setup::ShouldOfferLoader(LoaderState::Disabled, true));
    CHECK(!setup::ShouldOfferLoader(LoaderState::Disabled, false));
    CHECK(!setup::ShouldOfferLoader(LoaderState::Enabled, true));
    CHECK(!setup::ShouldOfferLoader(LoaderState::Unknown, true)); // no service (an emulator): no popup
    CHECK(!setup::ShouldOfferLoader(LoaderState::Unknown, false));
    bool good = false;
    CHECK(setup::LoaderEnableMessage(0, LoaderState::Enabled, &good) == "Plugin Loader enabled." && good);
    CHECK(setup::LoaderEnableMessage(0, LoaderState::Disabled, &good) == "The Plugin Loader is still not enabled." && !good);
    CHECK(setup::LoaderEnableMessage(0, LoaderState::Unknown, &good) == "The Plugin Loader is still not enabled." && !good);
    CHECK(setup::LoaderEnableMessage(0xD9001830u, LoaderState::Disabled, &good) == "Could not enable the Plugin Loader (0xD9001830)." && !good);
    CHECK(setup::LoaderEnableMessage(0xD9001830u, LoaderState::Enabled, nullptr) == "Could not enable the Plugin Loader (0xD9001830)."); // a failed request is never good news
}

// The copies of the plugin in the games' folders are looked at once a start: the one that is not the plugin file next to the app (an older version, or
// another build of the same size) is made again; ours and nothing else; a missing source or another plugin leaves the folder as it is.
static void TestRefreshCopies() {
    Fresh();
    const u64 ffe = 0x000400000016E700ull;
    const u64 tfh = 0x0004000000177000ull;
    const std::string source = "sdmc:/3ds/AzaharLink.3gx";
    const std::string build_a = Plugin(70000, 'a');
    Put(source, build_a);

    // FilesDiffer: the size first, then every byte; a file that is not there is not a difference
    Put("sdmc:/3ds/same.3gx", build_a);
    Put("sdmc:/3ds/other_bytes.3gx", Plugin(70000, 'b')); // the same size, other bytes
    Put("sdmc:/3ds/longer.3gx", Plugin(70001, 'a'));
    Put("sdmc:/3ds/shorter.3gx", Plugin(30000, 'a'));
    CHECK(!setup::FilesDiffer(source.c_str(), source.c_str()));
    CHECK(!setup::FilesDiffer(source.c_str(), "sdmc:/3ds/same.3gx"));
    CHECK(setup::FilesDiffer(source.c_str(), "sdmc:/3ds/other_bytes.3gx"));
    CHECK(setup::FilesDiffer(source.c_str(), "sdmc:/3ds/longer.3gx"));
    CHECK(setup::FilesDiffer(source.c_str(), "sdmc:/3ds/shorter.3gx"));
    CHECK(setup::FilesDiffer("sdmc:/3ds/shorter.3gx", source.c_str()));
    CHECK(!setup::FilesDiffer(source.c_str(), "sdmc:/3ds/not_there.3gx"));
    CHECK(!setup::FilesDiffer("sdmc:/3ds/not_there.3gx", source.c_str()));

    // a game with no folder: nothing is made
    bool copied = true;
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::Ok && !copied);
    CHECK(!Exists(setup::GamePluginDir(ffe)));

    // a copy that is the plugin file: nothing is written (the file is not even touched)
    CHECK(setup::EnableGame(ffe, source.c_str()) == setup::EnableResult::Ok);
    copied = true;
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::Ok && !copied);
    CHECK(Get(setup::GamePluginPath(ffe)) == build_a);

    // the plugin file of the app is another build of the SAME size: the size check (the old rule) sees nothing, this one makes the copy again
    const std::string build_b = Plugin(70000, 'b');
    Put(source, build_b);
    CHECK(!setup::CopyIsStale(setup::ReadSlot(ffe).copy_size, build_b.size()));
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::Ok && copied);
    CHECK(Get(setup::GamePluginPath(ffe)) == build_b);
    CHECK(!Exists(setup::GamePluginPath(ffe) + ".part"));
    copied = true;
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::Ok && !copied); // and then it is up to date

    // an older and a newer file are both "another": the plugin file of the app is the one that counts
    const std::string build_c = Plugin(70500, 'c');
    Put(source, build_c);
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::Ok && copied);
    CHECK(Get(setup::GamePluginPath(ffe)) == build_c);

    // the plugin file of the app is not there: the copy is kept as it is
    remove(source.c_str());
    copied = true;
    CHECK(setup::RefreshGameCopy(ffe, source.c_str(), &copied) == setup::EnableResult::SourceMissing && !copied);
    CHECK(Get(setup::GamePluginPath(ffe)) == build_c);

    // a folder with another plugin in it is never touched
    Put(source, build_a);
    mkdir(setup::GamePluginDir(tfh).c_str(), 0777);
    Put(setup::GamePluginDir(tfh) + "/Cheats.3gx", "theirs");
    copied = true;
    CHECK(setup::RefreshGameCopy(tfh, source.c_str(), &copied) == setup::EnableResult::Ok && !copied);
    CHECK(Get(setup::GamePluginDir(tfh) + "/Cheats.3gx") == "theirs" && !Exists(setup::GamePluginPath(tfh)));
    // ours and another one together: Luma may load either, so the folder is left alone too
    Put(setup::GamePluginPath(tfh), Plugin(100, 'z'));
    CHECK(setup::RefreshGameCopy(tfh, source.c_str(), &copied) == setup::EnableResult::Ok && !copied);
    CHECK(Get(setup::GamePluginPath(tfh)) == Plugin(100, 'z'));
}

static void TestLuma() {
    using setup::LoaderState;
    using setup::LumaSupport;
    CHECK(setup::LumaVersionText(0x0d030300) == "13.3.3" && setup::LumaVersionText(0x0a020100) == "10.2.1" && setup::LumaVersionText(0x0a010100) == "10.1.1");
    CHECK(setup::LumaVersionText(0).empty());
    CHECK(setup::ClassifyLuma(0, LoaderState::Unknown) == LumaSupport::Unknown);   // an emulator: no row
    CHECK(setup::ClassifyLuma(0, LoaderState::Enabled) == LumaSupport::Unknown);
    CHECK(setup::ClassifyLuma(0x0a010100, LoaderState::Unknown) == LumaSupport::NotWork); // the two old consoles of the test: 10.1.1 and 10.2.1, no Plugin Loader
    CHECK(setup::ClassifyLuma(0x0a020100, LoaderState::Unknown) == LumaSupport::NotWork);
    CHECK(setup::ClassifyLuma(0x0c000100, LoaderState::Unknown) == LumaSupport::NotWork); // 12.0.1: the last one without the loader
    CHECK(setup::ClassifyLuma(0x0a020100, LoaderState::Disabled) == LumaSupport::Maybe);  // an unofficial Luma with an old number that does answer the loader
    CHECK(setup::ClassifyLuma(0x0d000000, LoaderState::Enabled) == LumaSupport::Maybe);   // 13.0
    CHECK(setup::ClassifyLuma(0x0d010000, LoaderState::Disabled) == LumaSupport::Maybe);  // 13.1
    CHECK(setup::ClassifyLuma(0x0d020000, LoaderState::Unknown) == LumaSupport::Maybe);   // 13.2 (the loader busy: still a Luma with the loader)
    CHECK(setup::ClassifyLuma(0x0d030000, LoaderState::Enabled) == LumaSupport::Tested);  // 13.3
    CHECK(setup::ClassifyLuma(0x0d030300, LoaderState::Enabled) == LumaSupport::Tested);  // 13.3.3: the one it was tried on
    CHECK(setup::ClassifyLuma(0x0d040000, LoaderState::Disabled) == LumaSupport::Tested); // 13.4
    CHECK(setup::ClassifyLuma(0x0e000000, LoaderState::Enabled) == LumaSupport::Tested);  // a later major version
    // the row: red, yellow, green, and nothing when the version is not known
    auto luma_row = [](u32 word, LoaderState loader, setup::CheckLevel* level, std::string* value) {
        setup::InstallFacts f;
        f.plugin_card = setup::PluginCard::Same;
        f.plugins_dir = true;
        f.boot9 = true;
        f.sd_writable = true;
        f.luma = word;
        f.loader = loader;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        for (const setup::CheckItem& item : r.items) {
            if (item.label == "Luma3DS") {
                *level = item.level;
                *value = item.value;
                return true;
            }
        }
        return false;
    };
    setup::CheckLevel level = setup::CheckLevel::Optional;
    std::string value;
    CHECK(luma_row(0x0a020100, LoaderState::Unknown, &level, &value) && level == setup::CheckLevel::Problem && value == "10.2.1 not work");
    CHECK(luma_row(0x0d010000, LoaderState::Disabled, &level, &value) && level == setup::CheckLevel::Warning && value == "13.1.0 maybe");
    CHECK(luma_row(0x0d030300, LoaderState::Enabled, &level, &value) && level == setup::CheckLevel::Ok && value == "13.3.3 tested");
    CHECK(!luma_row(0, LoaderState::Unknown, &level, &value));
}

// The row "Installed titles" of the check: none until the scan has looked; all usable is fine; what is of no use is a warning that lists it, with A
// to delete it; the send page's words and the main page's line come from the same texts.
static void TestTitlesRow() {
    using setup::CheckAction;
    using setup::CheckLevel;
    using setup::UnusableTitle;
    std::printf("the installation check: the titles that are of no use\n");
    {
        setup::InstallFacts f = GoodFacts();
        f.titles.checked = true;
        f.titles.titles = 221;
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        const setup::CheckItem* item = Find(r, "Installed titles");
        CHECK(r.problems == 0 && r.warnings == 0 && item && item->level == CheckLevel::Ok && item->value == "221 titles, all usable");
        CHECK(item && item->action == CheckAction::None && item->note.find("221 titles") != std::string::npos);
    }
    {
        setup::InstallFacts f = GoodFacts();
        f.titles.checked = true;
        f.titles.titles = 84;
        UnusableTitle a;
        a.kind = UnusableTitle::Kind::Unfinished;
        a.tid = 0x0004000E00179500ull;
        UnusableTitle b;
        b.kind = UnusableTitle::Kind::PartialDlc;
        b.tid = 0x0004008C001B4100ull;
        b.name = "Fire Emblem Fates";
        b.installed = 23;
        b.missing = 11;
        UnusableTitle c;
        c.kind = UnusableTitle::Kind::Broken;
        c.tid = 0x00040000000ABCDEull;
        f.titles.unusable = {a, b, c};
        CHECK(f.titles.Count(UnusableTitle::Kind::Unfinished) == 1 && f.titles.Count(UnusableTitle::Kind::PartialDlc) == 1 && f.titles.Count(UnusableTitle::Kind::Broken) == 1);
        CHECK(setup::UnusableTitleLine(a) == "- 0004000E00179500: begun, never finished");
        CHECK(setup::UnusableTitleLine(b) == "- Fire Emblem Fates (DLC): 11 of 34 contents missing");
        CHECK(setup::UnusableTitleLine(c) == "- 00040000000ABCDE: the system cannot describe it");
        const setup::InstallReport r = setup::EvaluateInstall(f, "0.1.17");
        CHECK(r.problems == 0 && r.warnings == 1 && setup::InstallSummary(r) == "1 warning");
        const setup::CheckItem* item = Find(r, "Installed titles");
        CHECK(item && item->level == CheckLevel::Warning && item->value == "3 unusable titles" && item->action == CheckAction::CleanTitles);
        CHECK(item && item->note_title == "Titles of no use" && item->short_problem == "unusable titles");
        CHECK(item && item->note.find("84 titles") != std::string::npos && item->note.find(setup::UnusableTitleLine(b)) != std::string::npos);
        CHECK(item && item->note.find("A: delete them") != std::string::npos && item->note.find("still works for what is there") != std::string::npos);
        const setup::InstallLine line = setup::InstallLineText(r);
        CHECK(line.full == "unusable titles - A on Installation check");
        // the warning is listed before what is fine
        const std::vector<std::size_t> order = setup::InstallRowOrder(r);
        CHECK(order.size() == r.items.size() && r.items[order[0]].label == "Installed titles");
        // one of them is enough for the row, and the long list is cut
        f.titles.unusable.assign(12, a);
        const setup::InstallReport many = setup::EvaluateInstall(f, "0.1.17");
        CHECK(Find(many, "Installed titles")->value == "12 unusable titles" && Find(many, "Installed titles")->note.find("- and 4 more") != std::string::npos);
    }
}

static void TestTexts() {
    setup::ScanProgress p;
    p.titles = 221;
    p.done = 12;
    p.total = 221;
    CHECK(setup::ScanProgressText(p) == "names: 12 of 221 titles");
    p.done = 300;
    CHECK(setup::ScanProgressText(p) == "names: 221 of 221 titles"); // never above the total
    p.done = -4;
    CHECK(setup::ScanProgressText(p) == "names: 0 of 221 titles");
    p.phase = 1;
    p.done = 30;
    p.total = 172;
    CHECK(setup::ScanProgressText(p) == "games: 30 of 172");
    p.phase = 2;
    CHECK(setup::ScanProgressText(p) == "checking the titles");
    p.phase = 3;
    p.games = 172;
    CHECK(setup::ScanProgressText(p) == "221 titles, 172 games");
    CHECK(setup::ProgressFraction(0, 0) == 0.0f);
    CHECK(setup::ProgressFraction(50, 100) == 0.5f);
    CHECK(setup::ProgressFraction(150, 100) == 1.0f);
    CHECK(setup::ProgressFraction(-5, 100) == 0.0f);
    CHECK(setup::FormatSize(900) == "900 B" && setup::FormatSize(2048) == "2.0 KB" && setup::FormatSize(1048576) == "1.00 MB");
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_games_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    TestTitleIds();
    TestSmdh();
    TestOrder();
    TestIconLayout();
    TestGameCache();
    TestClassifyFolder();
    TestEnableDisable();
    TestOtherPlugin();
    TestCopyIsNeverHalf();
    TestInstallCheck();
    TestTitlesRow();
    TestLoader();
    TestRefreshCopies();
    TestLuma();
    TestTexts();
    if (chdir("/") != 0) {
        return 2;
    }
    fs::remove_all(dir);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
