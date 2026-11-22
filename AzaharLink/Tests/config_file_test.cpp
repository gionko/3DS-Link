// config_file_test (F7-rev, 2026-11-21): the plugin's OWN code of AzaharLink.ini - Plugin::LoadConfig, Plugin::SaveConfig, Plugin::ReadDataFile and
// the card of azl/whole_file.hpp (struct CtrWhole), cut out of Sources/ctr/plugin.cpp unchanged by Tests/config_file_extract.py, with the paths and
// struct Config - in front of a stand-in of CTRPF 0.8.0's File that answers as the real one does (File.hpp and File.o of pablomk7/libctrpf:0.8.0,
// disassembled for F7-rev):
//   - Exists: one open of the card in read, then its close: 1 when it opens, 0 whenever the open fails (a missing file and a passing error alike);
//   - Open: one command (the file made when missing with CREATE), then FSFILE_SetSize(0) as a command of its own with TRUNCATE; ~File closes;
//   - Rename: FSUSER_RenameFile alone, which never replaces a file that is there (an FS error); Remove: one command;
//   - GetSize: the FS error sign-extended (a huge size) when FSFILE_GetSize fails; Read: SUCCESS or the FS error.
// Every command is one step; the power may go before any of them (a write it meets lets a prefix down) and one command may fail with the power on.
// What it holds (the points of the review of F7):
//   - a save that found only the .bak of a cut save leaves the ini and no .bak (points 1 and 5);
//   - a first look at the ini that fails does not hand LoadConfig a stale .bak (point 2);
//   - an ini (or a .bak) that is there and cannot be read whole - it does not open, it is over 4096 bytes, its size or its read fails - is never
//     written over by SaveConfig, an empty one is (point 4); a second LoadConfig that reads it (LateInit) lets the saves through again;
//   - the adapter and the reader's choice of ini_path (point 6): the ini, the .bak, the old place - never written; LoadConfig never changes the
//     card; SaveConfig cut at every step or with one command failing leaves the old settings or the new ones, never the defaults after a file;
//   - the cost of a save in commands of the card (point 3): 14 with an ini, 11 without (4 before F7).
//   python3 Tests/config_file_extract.py Sources/ctr/plugin.cpp Includes/ctr/plugin.hpp "$OUT/config_file_slice.inc"
//   g++ $CXXFLAGS -I"$OUT" Tests/config_file_test.cpp -o "$OUT/config_file_test"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "azl/common.hpp"
#include "azl/ini_values.hpp"
#include "azl/room_proto.hpp"
#include "azl/whole_file.hpp"

// ---- the card
namespace sd {
using Files = std::map<std::string, std::string>;
constexpr int kFsError = static_cast<int>(0xC8804464u);  // (a passing error of the card)
constexpr int kNotFound = static_cast<int>(0xC8804478u); // (no such file)
constexpr int kExists = static_cast<int>(0xC82044BEu);   // (RenameFile onto a file that is there)
enum class Step { Done, Failed, CutNow, Dead };
struct Card {
    Files files;
    long budget = -1;       // commands before the power goes (-1: never)
    std::size_t prefix = 0; // bytes of the write the cut meets that reach the card
    long fail_step = -1;    // one command that fails alone, the power on
    bool cut = false;
    long steps = 0;
    long mutations = 0;                      // commands that changed the card
    std::map<std::string, int> exists_fails; // the next N looks of File::Exists at this path fail (a passing error)
    std::map<std::string, int> open_fails;   // the next N File::Open of this path fail
    std::set<std::string> never_opens;       // every open of this path fails (File::Exists too: it is an open)
    std::set<std::string> size_fails;        // FSFILE_GetSize fails on this path
    std::set<std::string> read_fails;        // FSFILE_Read fails on this path
    Step Next() {
        if (cut) {
            return Step::Dead;
        }
        if (budget == 0) {
            cut = true;
            ++steps;
            return Step::CutNow;
        }
        if (budget > 0) {
            --budget;
        }
        return steps++ == fail_step ? Step::Failed : Step::Done;
    }
    bool Has(const std::string& p) const { return files.count(p) != 0; }
};
Card g;
} // namespace sd

// ---- CTRPF's File, as 0.8.0 answers
namespace CTRPluginFramework {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
class File {
public:
    enum Mode { READ = 1, WRITE = 1 << 1, CREATE = 1 << 2, APPEND = 1 << 3, TRUNCATE = 1 << 4, SYNC = 1 << 5, RW = READ | WRITE, RWC = READ | WRITE | CREATE };
    enum OPResult { SUCCESS = 0, INVALID_PATH = -1, NOT_OPEN = -2, INVALID_MODE = -3, INVALID_ARG = -4 };
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (open_) {
            sd::g.Next(); // FSFILE_Close
        }
    }
    static int Exists(const std::string& p) {
        if (sd::g.Next() != sd::Step::Done) {
            return 0;
        }
        int& fails = sd::g.exists_fails[p];
        if (fails > 0) {
            --fails;
            return 0;
        }
        if (!sd::g.Has(p) || sd::g.never_opens.count(p)) {
            return 0;
        }
        sd::g.Next(); // FSFILE_Close
        return 1;
    }
    static int Open(File& f, const std::string& p, int mode = READ | WRITE | SYNC) {
        if (sd::g.Next() != sd::Step::Done) {
            return sd::kFsError;
        }
        int& fails = sd::g.open_fails[p];
        if (fails > 0) {
            --fails;
            return sd::kFsError;
        }
        if (sd::g.never_opens.count(p)) {
            return sd::kFsError;
        }
        if (!sd::g.Has(p)) {
            if (!(mode & CREATE)) {
                return sd::kNotFound;
            }
            sd::g.files[p] = "";
            ++sd::g.mutations;
        }
        f.open_ = true;
        f.path_ = p;
        f.mode_ = mode;
        f.pos_ = 0;
        if (mode & TRUNCATE) {
            if (sd::g.Next() != sd::Step::Done) { // FSFILE_SetSize(0)
                return sd::kFsError;
            }
            sd::g.files[p].clear();
            ++sd::g.mutations;
        }
        return SUCCESS;
    }
    static int Rename(const std::string& from, const std::string& to) {
        if (sd::g.Next() != sd::Step::Done || !sd::g.Has(from)) {
            return sd::kNotFound;
        }
        if (sd::g.Has(to)) {
            return sd::kExists;
        }
        const std::string text = sd::g.files[from];
        sd::g.files.erase(from);
        sd::g.files[to] = text;
        ++sd::g.mutations;
        return SUCCESS;
    }
    static int Remove(const std::string& p) {
        if (sd::g.Next() != sd::Step::Done || !sd::g.Has(p)) {
            return sd::kNotFound;
        }
        sd::g.files.erase(p);
        ++sd::g.mutations;
        return SUCCESS;
    }
    int Write(const void* data, u32 length) {
        if (!open_) {
            return NOT_OPEN;
        }
        if (!(mode_ & WRITE)) {
            return INVALID_MODE;
        }
        const sd::Step s = sd::g.Next();
        if (s != sd::Step::Done && s != sd::Step::CutNow) {
            return sd::kFsError;
        }
        std::string& text = sd::g.files[path_];
        const std::string bytes(static_cast<const char*>(data), length);
        text = text.substr(0, pos_) + (s == sd::Step::CutNow ? bytes.substr(0, std::min<std::size_t>(sd::g.prefix, length)) : bytes);
        ++sd::g.mutations;
        if (s == sd::Step::CutNow) {
            return sd::kFsError;
        }
        pos_ += length;
        return SUCCESS;
    }
    int Read(void* buffer, u32 length) const {
        if (!open_) {
            return NOT_OPEN;
        }
        if (sd::g.Next() != sd::Step::Done || sd::g.read_fails.count(path_)) {
            return sd::kFsError;
        }
        const std::string& text = sd::g.files[path_];
        const std::size_t n = pos_ >= text.size() ? 0 : std::min<std::size_t>(length, text.size() - pos_);
        std::memcpy(buffer, text.data() + pos_, n);
        pos_ += n;
        return SUCCESS;
    }
    u64 GetSize() const {
        if (!open_) {
            return static_cast<u64>(static_cast<s64>(NOT_OPEN));
        }
        if (sd::g.Next() != sd::Step::Done || sd::g.size_fails.count(path_)) {
            return static_cast<u64>(static_cast<s64>(sd::kFsError)); // (File.o: the Result, sign-extended)
        }
        return sd::g.files[path_].size();
    }
    int Flush() const { return SUCCESS; }

private:
    bool open_ = false;
    std::string path_;
    int mode_ = 0;
    mutable std::size_t pos_ = 0;
};
} // namespace CTRPluginFramework

// ---- the plugin's code, in a stand-in of the Plugin class with the members it uses (by their names in plugin.hpp)
namespace azl {
namespace ctr {
using CTRPluginFramework::File;

std::vector<std::string> g_log;
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_log.push_back(line);
}
void MakeDataDir() {} // (the folder is there: Directory is not part of this test)

#include "config_file_head.inc"

class Plugin {
public:
    static bool ReadDataFile(const char* path, std::size_t max_bytes, std::string& out);
    bool LoadConfig();
    bool SaveConfig();
    Config config_;
    bool config_from_old_place_ = false;
    bool config_unreadable_ = false;
    std::atomic<bool> chat_on_{ini::kChatDefault};
    std::atomic<int> chat_pos_{ini::kChatPosDefault};
};

#include "config_file_slice.inc"
} // namespace ctr
} // namespace azl

using azl::ctr::Config;
using azl::ctr::kConfigBakPath;
using azl::ctr::kConfigPath;
using azl::ctr::kOldConfigPath;
using azl::ctr::Plugin;
using sd::Files;

static int g_checks = 0, g_failures = 0, g_shown = 0;
#define CHECK_AT(cond, what)                                                                       \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            if (g_shown++ < 40) {                                                                  \
                std::fprintf(stderr, "  FAIL line %d: %s (%s)\n", __LINE__, #cond, (what));       \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#define CHECK(cond) CHECK_AT(cond, "")

static const std::string kIni = kConfigPath;
static const std::string kBak = kConfigBakPath;
static const std::string kTmp = std::string(kConfigPath) + ".tmp";
static const std::string kOld = kOldConfigPath;
static const std::string kTextA = "enabled=1\nhost=a.example\nport=24900\nnickname=Ann\npassword=pw-a\nchat=1\nchat_pos=top-right\n";
static const std::string kTextB = "enabled=1\nhost=b.example\nport=24901\nnickname=Bob\npassword=pw-b\n";
static const std::string kStale = "host=stale.example\nnickname=Stale\n";
static const std::string kOldPlace = "host=oldplace.example\nnickname=Olda\n";

// the settings as one line (what a launch would use)
static std::string Snap(const Config& c) {
    char port[16];
    std::snprintf(port, sizeof(port), "%u", static_cast<unsigned>(c.port));
    return std::string(c.enabled ? "1" : "0") + "|" + c.host + "|" + port + "|" + c.nickname + "|" + c.password + "|" + (c.chat ? "1" : "0") + "|" +
           std::to_string(c.chat_pos) + "|" + std::to_string(c.debug) + "|" + c.menu_key + "|" + c.game_name;
}
static std::string Defaults() { return Snap(Config()); }

static void Fresh(Files files) { // (by value: a caller may hand the card's own files)
    sd::g = sd::Card();
    sd::g.files = files;
    azl::ctr::g_log.clear();
}

struct Launch {
    bool loaded = false;
    std::string snap;
};
// a launch on the card as it is: LoadConfig only (nothing failing)
static Launch Reload(Files files) {
    Fresh(files);
    Plugin p;
    Launch l;
    l.loaded = p.LoadConfig();
    l.snap = Snap(p.config_);
    CHECK_AT(sd::g.mutations == 0, "LoadConfig never changes the card");
    return l;
}
static bool Logged(const char* part) {
    for (const std::string& line : azl::ctr::g_log) {
        if (line.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static void TestNormal() {
    std::printf("the ini read, a change of the menu saved, the cost\n");
    Fresh({{kIni, kTextA}});
    Plugin p;
    CHECK(p.LoadConfig() && !p.config_unreadable_ && p.config_.host == "a.example" && p.config_.chat && p.config_.port == 24900);
    CHECK(sd::g.mutations == 0);
    const long load_steps = sd::g.steps;
    p.config_.host = "menu.example"; // (as Pick of the in-game menu)
    const long before = sd::g.steps;
    CHECK(p.SaveConfig());
    const long save_steps = sd::g.steps - before;
    CHECK(sd::g.files.size() == 1 && sd::g.files.count(kIni));
    CHECK(save_steps == 14);
    Fresh({});
    Plugin q;
    q.LoadConfig();
    const long before_q = sd::g.steps;
    CHECK(q.SaveConfig() && sd::g.files.size() == 1 && sd::g.files.count(kIni));
    const long save_none = sd::g.steps - before_q;
    CHECK(save_none == 11);
    std::printf("  LoadConfig %ld commands of the card; SaveConfig %ld with an ini, %ld without (4 before F7: open, SetSize, write, close)\n", load_steps,
                save_steps, save_none);
    // the next launch reads what the menu saved
    Fresh({{kIni, kTextA}});
    Plugin r;
    r.LoadConfig();
    r.config_.host = "menu.example";
    r.SaveConfig();
    const Files after = sd::g.files;
    const Launch l = Reload(after);
    Config want = r.config_;
    CHECK(l.loaded && l.snap == Snap(want));
}

static void TestBakAlone() {
    std::printf("only the .bak of a cut save: read, then the save leaves the ini and no .bak (points 1 and 5)\n");
    Fresh({{kBak, kTextB}});
    Plugin p;
    CHECK(p.LoadConfig() && p.config_.host == "b.example" && !p.config_unreadable_);
    CHECK(Logged("AzaharLink.ini.bak"));
    CHECK(sd::g.mutations == 0 && sd::g.files.size() == 1 && sd::g.files.count(kBak)); // (only read: LoadConfig puts nothing in order)
    p.config_.chat = true;
    CHECK(p.SaveConfig());
    CHECK(sd::g.files.size() == 1 && sd::g.files.count(kIni) && !sd::g.files.count(kBak) && !sd::g.files.count(kTmp));
    const Launch l = Reload(sd::g.files);
    CHECK(l.loaded && l.snap == Snap(p.config_));
}

static void TestPassingErrorOfTheLook() {
    std::printf("a first look at the ini that fails, a stale .bak beside it: the ini is read (point 2)\n");
    Fresh({{kIni, kTextA}, {kBak, kStale}});
    sd::g.exists_fails[kIni] = 1;
    Plugin p;
    CHECK(p.LoadConfig() && p.config_.host == "a.example" && !Logged("AzaharLink.ini.bak"));
    // two looks that fail in a row: the stale copy (one save behind), never the defaults
    Fresh({{kIni, kTextA}, {kBak, kStale}});
    sd::g.exists_fails[kIni] = 2;
    Plugin q;
    CHECK(q.LoadConfig() && q.config_.host == "stale.example");
    // ... and its save, the looks good again, puts that copy with the menu's change over the ini: what is left open (two failing looks in a row
    // after a save cut at its very last step), whole all the same
    q.config_.host = "menu.example";
    CHECK(q.SaveConfig() && sd::g.files.size() == 1 && Reload(sd::g.files).snap == Snap(q.config_));
}

static void TestUnreadable() {
    std::printf("an ini or a .bak that is there and cannot be read whole is never written over (point 4)\n");
    std::string big = "host=big.example\nnickname=Big\n";
    while (big.size() <= 4096) {
        big += "# a line of a file grown by hand, kept as it is: 0123456789\n";
    }
    struct Case {
        const char* what;
        Files files;
        std::string path;
        int how; // 0 its open fails, 1 over 4096 bytes, 2 GetSize fails, 3 Read fails
    };
    const std::vector<Case> cases = {
        {"the ini's open fails", {{kIni, kTextA}}, kIni, 0},
        {"the ini over 4096 bytes", {{kIni, big}}, kIni, 1},
        {"the ini's size fails", {{kIni, kTextA}}, kIni, 2},
        {"the ini's read fails", {{kIni, kTextA}}, kIni, 3},
        {"the .bak's open fails", {{kBak, kTextB}}, kBak, 0},
        {"the .bak over 4096 bytes", {{kBak, big}}, kBak, 1},
        {"the .bak's read fails", {{kBak, kTextB}}, kBak, 3},
    };
    for (const Case& c : cases) {
        Fresh(c.files);
        if (c.how == 0) {
            sd::g.open_fails[c.path] = 1;
        } else if (c.how == 2) {
            sd::g.size_fails.insert(c.path);
        } else if (c.how == 3) {
            sd::g.read_fails.insert(c.path);
        }
        Plugin p;
        CHECK_AT(!p.LoadConfig() && p.config_unreadable_ && Snap(p.config_) == Defaults(), c.what);
        p.config_.host = "menu.example";
        sd::g.size_fails.clear();
        sd::g.read_fails.clear();
        const long m = sd::g.mutations;
        CHECK_AT(!p.SaveConfig(), c.what);
        CHECK_AT(sd::g.mutations == m && sd::g.files == c.files, c.what);
        CHECK_AT(Logged("could not be read: not written over"), c.what);
        // LateInit reads it again; when that read is whole, the menu's changes are saved again
        CHECK_AT(p.LoadConfig() == (c.how != 1) && p.config_unreadable_ == (c.how == 1), c.what);
        if (c.how != 1) {
            p.config_.host = "menu.example";
            CHECK_AT(p.SaveConfig() && sd::g.files.size() == 1 && sd::g.files.count(kIni), c.what);
        }
    }
    // an ini that never opens: File::Exists says 0 too, so the reader takes it for missing - and the save, finding no file, renames onto it:
    // the card refuses (RenameFile never replaces), the save says false, the ini is untouched
    Fresh({{kIni, kTextA}});
    sd::g.never_opens.insert(kIni);
    Plugin n;
    CHECK(!n.LoadConfig() && !n.config_unreadable_);
    CHECK(!n.SaveConfig() && sd::g.files.size() == 1 && sd::g.files.at(kIni) == kTextA);
    // an empty file (a cut of 1.1.3) holds nothing to keep: the save writes it
    Fresh({{kIni, ""}});
    Plugin e;
    CHECK(!e.LoadConfig() && !e.config_unreadable_);
    e.config_.host = "menu.example";
    CHECK(e.SaveConfig() && sd::g.files.size() == 1 && sd::g.files.at(kIni).find("host=menu.example\n") != std::string::npos);
    // no file at all: the defaults, and the save writes the ini
    Fresh({});
    Plugin z;
    CHECK(!z.LoadConfig() && !z.config_unreadable_ && Snap(z.config_) == Defaults());
    CHECK(z.SaveConfig() && sd::g.files.size() == 1 && sd::g.files.count(kIni));
}

static void TestOldPlace() {
    std::printf("the old place: read while the ini is missing, never written\n");
    Fresh({{kOld, kOldPlace}});
    Plugin p;
    CHECK(p.LoadConfig() && p.config_from_old_place_ && p.config_.host == "oldplace.example");
    CHECK(p.SaveConfig() && sd::g.files.count(kIni) && sd::g.files.at(kOld) == kOldPlace && sd::g.files.size() == 2);
    // the ini wins over the old place; a .bak wins over it too
    CHECK(Reload({{kIni, kTextA}, {kOld, kOldPlace}}).snap.find("|a.example|") != std::string::npos);
    CHECK(Reload({{kBak, kTextB}, {kOld, kOldPlace}}).snap.find("|b.example|") != std::string::npos);
    // an old place that does not open: nothing of ours is there, a save writes the ini and leaves the old place alone
    Fresh({{kOld, kOldPlace}});
    sd::g.open_fails[kOld] = 1;
    Plugin q;
    CHECK(!q.LoadConfig() && !q.config_unreadable_ && !q.config_from_old_place_);
    CHECK(q.SaveConfig() && sd::g.files.at(kOld) == kOldPlace);
}

// the states a save of the menu may meet
static std::vector<std::pair<const char*, Files>> States() {
    return {
        {"the ini", {{kIni, kTextA}}},
        {"the ini + a stale .bak", {{kIni, kTextA}, {kBak, kStale}}},
        {"the ini + half a .tmp", {{kIni, kTextA}, {kTmp, "enabled=1\nho"}}},
        {"the .bak alone", {{kBak, kTextB}}},
        {"nothing", {}},
        {"an empty ini", {{kIni, ""}}},
        {"the old place", {{kOld, kOldPlace}}},
    };
}

static void TestCuts() {
    std::printf("SaveConfig cut at every step, every prefix of its writes; one command failing, the power on (point 6)\n");
    long cuts = 0, failures = 0;
    for (const auto& st : States()) {
        const Launch old = Reload(st.second);
        // the save with nothing in its way: its steps and what it leaves
        Fresh(st.second);
        Plugin ref;
        ref.LoadConfig();
        ref.config_.host = "menu.example";
        ref.config_.chat_pos = azl::ini::kChatPosDefault == 0 ? 1 : 0;
        const long start = sd::g.steps;
        CHECK_AT(ref.SaveConfig(), st.first);
        const long total = sd::g.steps - start;
        const std::string want_new = Snap(ref.config_);
        CHECK_AT(Reload(sd::g.files).snap == want_new && !sd::g.files.count(kBak) && !sd::g.files.count(kTmp), st.first);
        for (long k = 0; k <= total; ++k) {
            for (std::size_t prefix : {std::size_t(0), std::size_t(1), std::size_t(40), std::size_t(4000)}) {
                ++cuts;
                Fresh(st.second);
                Plugin p;
                p.LoadConfig();
                p.config_.host = "menu.example";
                p.config_.chat_pos = ref.config_.chat_pos;
                sd::g.budget = k;
                sd::g.prefix = prefix;
                p.SaveConfig();
                const Launch got = Reload(sd::g.files);
                CHECK_AT(got.snap == old.snap || got.snap == want_new, st.first);
                CHECK_AT(!old.loaded || got.loaded, st.first); // (never the defaults after a whole file)
                CHECK_AT(st.second.count(kOld) == sd::g.files.count(kOld) && (!st.second.count(kOld) || sd::g.files.at(kOld) == kOldPlace), st.first);
            }
        }
        for (long f = 0; f < total; ++f) {
            ++failures;
            Fresh(st.second);
            Plugin p;
            p.LoadConfig();
            p.config_.host = "menu.example";
            p.config_.chat_pos = ref.config_.chat_pos;
            sd::g.fail_step = sd::g.steps + f;
            const bool ok = p.SaveConfig();
            const Launch got = Reload(sd::g.files);
            CHECK_AT(ok ? got.snap == want_new : got.snap == old.snap, st.first);
            if (!ok && st.second.count(kIni)) {
                CHECK_AT(sd::g.files.count(kIni) && sd::g.files.at(kIni) == st.second.at(kIni), st.first); // (the old ini at its place)
            }
            CHECK_AT(!ok || !sd::g.files.count(kTmp), st.first);
        }
    }
    std::printf("  %ld cut saves, %ld saves with one failing command\n", cuts, failures);
}

static void TestAdapter() {
    std::printf("the card of whole::Replace (CtrWhole) as CTRPF answers\n");
    azl::ctr::CtrWhole card;
    Fresh({{kIni, kTextA}, {kBak, kStale}});
    CHECK(card.Exists(kIni) && card.Exists(kBak) && !card.Exists(kTmp));
    CHECK(!card.Rename(kIni, kBak)); // (RenameFile never replaces)
    CHECK(sd::g.files.at(kIni) == kTextA && sd::g.files.at(kBak) == kStale);
    CHECK(card.WriteNew(kTmp, kTextB) && sd::g.files.at(kTmp) == kTextB);
    CHECK(card.WriteNew(kTmp, "short\n") && sd::g.files.at(kTmp) == "short\n"); // (TRUNCATE: nothing of the longer text left)
    // the read back fails: WriteNew says false
    Fresh({});
    sd::g.read_fails.insert(kTmp);
    CHECK(!card.WriteNew(kTmp, kTextB));
    // the write fails: false
    Fresh({});
    sd::g.fail_step = 2; // (open 0, SetSize 1, write 2)
    CHECK(!card.WriteNew(kTmp, kTextB));
    card.Remove(kTmp);
    CHECK(!sd::g.files.count(kTmp));
}

int main() {
    TestAdapter();
    TestNormal();
    TestBakAlone();
    TestPassingErrorOfTheLook();
    TestUnreadable();
    TestOldPlace();
    TestCuts();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
