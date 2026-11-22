#include "dlp_guest_store.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "azl/dlp_children.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_decide.hpp"
#include "dlp_guest_ids.hpp"
#include "dlp_guest_paths.hpp"
#include "fileio.hpp"
#include "paths.hpp"

namespace dlpguest {

namespace {

constexpr const char* kGuestHeader = "#dlp-guest 1";
constexpr const char* kCacheHeader = "#dlp-children-cache 1";
constexpr const char* kCacheFileName = "children.cache";
constexpr std::size_t kGuestMaxBytes = 2048;
constexpr std::size_t kCacheMaxBytes = 64 * 1024;
static_assert(kMaxGuests == azl::dlp_children::kMaxRows, "the app makes one child per row of the index, no more");
static_assert(kRegistrySlots >= kMaxGuests, "every record the app makes has a slot");

// ---- text (no printf family here: the pins keep it out of every dlp_guest file) ----
std::string Hex16(u64 v) {
    char p[16];
    azl::dlp_children::PutHex16(p, v);
    return std::string(p, 16);
}

std::string Dec(u32 v) {
    char d[10];
    int n = 0;
    do {
        d[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    std::string s;
    while (n > 0) {
        s += d[--n];
    }
    return s;
}

std::string HexBytes(const u8* p, std::size_t n) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += kDigits[p[i] >> 4];
        s += kDigits[p[i] & 0xF];
    }
    return s;
}

bool ReadId(const std::string& v, u64* out) {
    return v.size() == 16 && azl::dlp_children::ReadHex16(v.data(), out);
}

bool ReadDec(const std::string& v, u32 top, u32* out) {
    if (v.empty() || v.size() > 10) {
        return false;
    }
    u64 n = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + static_cast<u64>(c - '0');
    }
    if (n > top) {
        return false;
    }
    *out = static_cast<u32>(n);
    return true;
}

int HexDigit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1));
}

bool ReadBytes(const std::string& v, u8* out, std::size_t n) {
    if (v.size() != 2 * n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = HexDigit(v[2 * i]), lo = HexDigit(v[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

// A product code: letters, digits and '-' only (it sits in a line of key=value and of the cache's ':' fields), at most 16.
bool ReadProduct(const std::string& v, char (&out)[kProductBytes]) {
    if (v.size() >= kProductBytes) {
        return false;
    }
    for (const char c : v) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
        if (!ok) {
            return false;
        }
    }
    std::memset(out, 0, kProductBytes);
    std::memcpy(out, v.data(), v.size());
    return true;
}

std::string ProductText(const char (&p)[kProductBytes]) {
    std::size_t n = 0;
    while (n < kProductBytes - 1 && p[n] != '\0') {
        ++n;
    }
    return std::string(p, n);
}

// The lines of a text without their '\n' and the '\r' before it.
std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        at = end + 1;
    }
    return lines;
}

std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> f;
    std::size_t at = 0;
    for (;;) {
        const std::size_t c = line.find(':', at);
        f.push_back(line.substr(at, c == std::string::npos ? std::string::npos : c - at));
        if (c == std::string::npos) {
            return f;
        }
        at = c + 1;
    }
}

const char* StateText(GuestState s) {
    return s == GuestState::Installed ? "installed" : (s == GuestState::Removing ? "removing" : "preparing");
}

bool IsSoundGuest(const Guest& g) {
    return azl::title_rules::PreparedOf(g.game) != 0 && g.prepared == azl::title_rules::PreparedOf(g.game) &&
           static_cast<u32>(g.child >> 32) == azl::title_rules::kTitleHighDlpChild && static_cast<u32>(g.child) != 0 && (g.where == 'S' || g.where == 'C');
}

// ---- the folder ----
bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> FileNames(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return names;
    }
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != ".." && IsFile(dir + "/" + name)) {
            names.push_back(name);
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

// (DPAPP-2j) the folders of `dir` (the ID0 folders of dlp-guests)
std::vector<std::string> DirNames(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return names;
    }
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != ".." && IsDir(dir + "/" + name)) {
            names.push_back(name);
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

bool EndsWith(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// "<16 hex digits><tail>": the prepared title it names (upper case only: the names the app writes).
bool PreparedOfName(const std::string& name, const char* tail, u64* prepared) {
    const std::size_t n = std::strlen(tail);
    if (name.size() != 16 + n || name.compare(16, n, tail) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return azl::dlp_children::ReadHex16(name.data(), prepared);
}

const Guest* FindGuest(const std::vector<Guest>& guests, u64 prepared) {
    for (const Guest& g : guests) {
        if (g.prepared == prepared) {
            return &g;
        }
    }
    return nullptr;
}

std::atomic<u64> g_owned[kRegistrySlots];
std::atomic<u32> g_this_console{0}; // (DPAPP-2h-sec: SetThisConsole)
std::atomic<u8> g_form[kRegistrySlots]; // (the build 2f) the form of the title of the same slot: AmTitleOf, the jump

} // namespace

// ---- the record ----------------------------------------------------------------------------------------------------------------------
// (the build 2h) the word of a form in the record: app, dlp, sys
static const char* FormWord(u8 form) {
    return form == kFormSystem ? "sys" : (form == kFormDlpChild ? "dlp" : "app");
}

static bool ReadForm(const std::string& v, u8* form) {
    if (v != "app" && v != "dlp" && v != "sys") {
        return false;
    }
    *form = v == "sys" ? kFormSystem : (v == "dlp" ? kFormDlpChild : kFormApplication);
    return true;
}

std::string GuestText(const Guest& g) {
    std::string t = std::string(kGuestHeader) + "\n";
    t += "game=" + Hex16(g.game) + "\n";
    t += "child=" + Hex16(g.child) + "\n";
    t += "prepared=" + Hex16(g.prepared) + "\n";
    t += std::string("where=") + (g.where == 'C' ? "C" : "S") + "\n";
    t += "game_version=" + Dec(g.game_version) + "\n";
    t += "update_version=" + Dec(g.update_version) + "\n";
    t += "child_version=" + Dec(g.child_version) + "\n";
    t += "child_hash8=" + HexBytes(g.child_hash8, sizeof(g.child_hash8)) + "\n";
    t += "transfer=" + Dec(g.transfer) + "\n";
    t += "required=" + Dec(g.required) + "\n";
    t += "product=" + ProductText(g.product) + "\n";
    t += "composer=" + Dec(g.composer) + "\n";
    t += "language=" + Dec(g.language) + "\n";
    t += "variant=" + Dec(g.variant) + "\n";
    t += std::string("form=") + FormWord(g.form) + "\n"; // (the build 2f: the id AM has the title by, AmTitleFor; the build 2h: sys)
    t += std::string("state=") + StateText(g.state) + "\n";
    t += std::string("prev_state=") + (g.prev_installed ? "installed" : "none") + "\n";
    t += "prev_version=" + Dec(g.prev_version) + "\n";
    // (the review of the build 2f) the migration's old child, in the other form: two keys an older app skips
    t += std::string("prev_other=") + (g.prev_other ? "installed" : "none") + "\n";
    t += "prev_other_version=" + Dec(g.prev_other_version) + "\n";
    // (the build 2h) the form of that old child, and whose the ticket of the record's id of AM is (written only when known: the builds before
    // knew nothing of it, and their records stay "unknown" - TicketMayGo)
    t += std::string("prev_other_form=") + FormWord(g.prev_other_form) + "\n";
    if (g.ticket_known) {
        t += std::string("ticket=") + (g.ticket_ours ? "ours" : "other") + "\n";
    }
    if (g.console != 0) {
        t += "console=" + Dec(g.console) + "\n"; // (DPAPP-2h-sec: the console the ticket word holds on; an older app skips it)
    }
    return t;
}

bool ParseGuest(const std::string& text, Guest* out) {
    const std::vector<std::string> lines = Lines(text);
    if (lines.empty() || lines[0] != kGuestHeader) {
        return false;
    }
    Guest g;
    bool have_game = false, have_child = false, have_prepared = false, have_where = false, have_state = false, have_prev_other_form = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        const std::string key = line.substr(0, eq), v = line.substr(eq + 1);
        u32 n = 0;
        bool ok = true;
        if (key == "game") {
            ok = have_game = ReadId(v, &g.game);
        } else if (key == "child") {
            ok = have_child = ReadId(v, &g.child);
        } else if (key == "prepared") {
            ok = have_prepared = ReadId(v, &g.prepared);
        } else if (key == "where") {
            ok = have_where = v == "S" || v == "C";
            g.where = v == "C" ? 'C' : 'S';
        } else if (key == "game_version" || key == "update_version" || key == "child_version" || key == "prev_version") {
            ok = ReadDec(v, 0xFFFF, &n);
            u16& field = key == "game_version" ? g.game_version : (key == "update_version" ? g.update_version : (key == "child_version" ? g.child_version : g.prev_version));
            field = static_cast<u16>(n);
        } else if (key == "prev_other_version") {
            ok = ReadDec(v, 0xFFFF, &n);
            g.prev_other_version = static_cast<u16>(n);
        } else if (key == "prev_other") {
            ok = v == "none" || v == "installed";
            g.prev_other = v == "installed";
        } else if (key == "child_hash8") {
            ok = ReadBytes(v, g.child_hash8, sizeof(g.child_hash8));
        } else if (key == "transfer" || key == "required") {
            ok = ReadDec(v, 0xFFFFFFFFu, &n);
            (key == "transfer" ? g.transfer : g.required) = n;
        } else if (key == "product") {
            ok = ReadProduct(v, g.product);
        } else if (key == "composer" || key == "language" || key == "variant") {
            ok = ReadDec(v, 0xFF, &n);
            (key == "composer" ? g.composer : (key == "language" ? g.language : g.variant)) = static_cast<u8>(n);
        } else if (key == "state") {
            ok = have_state = v == "preparing" || v == "installed" || v == "removing";
            g.state = v == "installed" ? GuestState::Installed : (v == "removing" ? GuestState::Removing : GuestState::Preparing);
        } else if (key == "prev_state") {
            ok = v == "none" || v == "installed";
            g.prev_installed = v == "installed";
        } else if (key == "form") {
            // (the build 2f) a record without the key is of the builds before: the form kFormApplication. A value this version does not know
            // is a record refused - its title would be looked for under the wrong id of AM (the build 2h: sys, the system form)
            ok = ReadForm(v, &g.form);
        } else if (key == "prev_other_form") {
            ok = have_prev_other_form = ReadForm(v, &g.prev_other_form);
        } else if (key == "ticket") {
            // (the build 2h) whose the ticket of the record's id of AM is: a value this version does not know is a record refused (never taken for ours)
            ok = v == "ours" || v == "other";
            g.ticket_known = ok;
            g.ticket_ours = v == "ours";
        } else if (key == "console") {
            // (DPAPP-2h-sec) the console the ticket word was written on: a value that does not read is a record refused (never taken for this one)
            ok = ReadDec(v, 0xFFFFFFFFu, &n) && n != 0;
            g.console = n;
        } // (another key: a later version's, skipped)
        if (!ok) {
            return false;
        }
    }
    if (!have_game || !have_child || !have_prepared || !have_where || !have_state || !IsSoundGuest(g)) {
        return false;
    }
    if (!have_prev_other_form) {
        // (a record of the builds 2f/2g knew two forms: the old child of a migration was in the other one)
        g.prev_other_form = g.form == kFormDlpChild ? kFormApplication : kFormDlpChild;
    }
    if (g.prev_other && g.prev_other_form == g.form) {
        return false; // (an old child in the form of the run itself is no migration: a broken record)
    }
    *out = g;
    return true;
}

// ---- (DPAPP-2j, R-B) the records of THIS console --------------------------------------------------------------------------------------
namespace {
std::string g_this_id0; // (set once at the start, before any worker runs)

bool IsHexName(const std::string& s, std::size_t n) {
    if (s.size() != n) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}
} // namespace

std::string Id0OfRoot(const std::string& root) {
    static const std::string kHead = "/Nintendo 3DS/";
    if (root.compare(0, kHead.size(), kHead) != 0) {
        return "";
    }
    std::string rest = root.substr(kHead.size());
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || !IsHexName(rest.substr(0, slash), 32) || !IsHexName(rest.substr(slash + 1), 32)) {
        return "";
    }
    return Lower(rest.substr(0, slash));
}

std::string Id0OfFolders(const std::vector<std::string>& names) {
    std::string one;
    for (const std::string& name : names) {
        if (IsHexName(name, 32)) {
            if (!one.empty()) {
                return ""; // (two ID0 folders: never a guess)
            }
            one = Lower(name);
        }
    }
    return one;
}

std::string Id0OfSdCard(const std::string& dir) {
    return Id0OfFolders(DirNames(dir));
}

void SetThisId0(const std::string& id0) {
    g_this_id0 = IsHexName(id0, 32) ? Lower(id0) : std::string();
}

std::string ThisId0() {
    return g_this_id0;
}

std::string RecordsDir() {
    return g_this_id0.empty() ? std::string() : GuestsDir() + "/" + g_this_id0;
}

std::vector<u64> OtherConsolesKeys() {
    std::vector<u64> keys;
    const auto add = [&keys](const std::string& dir) {
        for (const std::string& name : FileNames(dir)) {
            u64 p = 0;
            if ((PreparedOfName(name, ".guest", &p) || PreparedOfName(name, ".guest.bak", &p)) && std::find(keys.begin(), keys.end(), p) == keys.end()) {
                keys.push_back(p);
            }
        }
    };
    add(GuestsDir()); // (the shared folder: records of the builds before DPAPP-2j that were not moved)
    for (const std::string& name : DirNames(GuestsDir())) {
        if (IsHexName(name, 32) && Lower(name) != g_this_id0) {
            add(GuestsDir() + "/" + name);
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::string GuestPath(u64 prepared) {
    const std::string dir = RecordsDir();
    return dir.empty() ? std::string() : dir + "/" + Hex16(prepared) + ".guest";
}

std::string PartPath(u64 prepared) {
    const std::string dir = RecordsDir();
    return dir.empty() ? std::string() : dir + "/" + Hex16(prepared) + ".part";
}

std::vector<Guest> LoadGuests() {
    std::vector<u64> named;
    if (RecordsDir().empty()) {
        return {}; // (DPAPP-2j: the ID0 not known - no record is this console's)
    }
    for (const std::string& name : FileNames(RecordsDir())) {
        u64 p = 0;
        if (PreparedOfName(name, ".guest", &p) || PreparedOfName(name, ".guest.bak", &p)) {
            if (std::find(named.begin(), named.end(), p) == named.end()) {
                named.push_back(p);
            }
        }
    }
    std::sort(named.begin(), named.end());
    std::vector<Guest> guests;
    for (const u64 p : named) {
        std::string text;
        Guest g;
        if (setup::ReadTextFile(GuestPath(p), kGuestMaxBytes, text) == setup::LoadResult::Loaded && ParseGuest(text, &g) && g.prepared == p) {
            guests.push_back(g);
        }
    }
    return guests;
}

bool SaveGuest(const Guest& g) {
    if (!IsSoundGuest(g) || RecordsDir().empty()) {
        return false; // (DPAPP-2j: the ID0 not known - no record, so nothing of AM after it)
    }
    setup::MakeDirs(RecordsDir());
    return setup::WriteTextFile(GuestPath(g.prepared), GuestText(g));
}

bool DropGuest(u64 prepared) {
    const std::string path = GuestPath(prepared);
    if (path.empty()) {
        return false;
    }
    std::remove(path.c_str());
    std::remove((path + ".bak").c_str());
    std::remove((path + ".tmp").c_str());
    return !IsFile(path) && !IsFile(path + ".bak");
}

// ---- the registry ------------------------------------------------------------------------------------------------------------------
// The slot of a registered prepared title, -1 for another.
static int SlotOf(u64 prepared) {
    if (prepared == 0) {
        return -1;
    }
    for (std::size_t i = 0; i < kRegistrySlots; ++i) {
        if (g_owned[i].load(std::memory_order_acquire) == prepared) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SetThisConsole(u32 stamp) {
    g_this_console.store(stamp, std::memory_order_release);
}

u32 ThisConsole() {
    return g_this_console.load(std::memory_order_acquire);
}

bool IsOwnedGuestTid(u64 tid) {
    // (the build 2f) any id of a child of ours: the key, its id of AM in the form M3c1 (000400010F8...), its system id (the build 2h: 000400100F8...)
    return SlotOf(SlotKeyOf(tid)) >= 0;
}

bool Register(u64 prepared, u8 form) {
    if (!azl::title_rules::IsPreparedId(prepared) || prepared == azl::title_rules::kAppTid) {
        return false;
    }
    const int at = SlotOf(prepared);
    if (at >= 0) {
        g_form[at].store(form, std::memory_order_release);
        return true;
    }
    for (std::size_t i = 0; i < kRegistrySlots; ++i) {
        u64 empty = 0;
        if (g_owned[i].load(std::memory_order_acquire) == 0 && g_owned[i].compare_exchange_strong(empty, prepared, std::memory_order_acq_rel)) {
            g_form[i].store(form, std::memory_order_release); // (the worker registers, the main thread reads: one writer of a slot at a time)
            return true;
        }
    }
    return false; // every slot is taken: NOT ours to this run (the caller counts it; nothing deletes or hides its title)
}

void SetForm(u64 prepared, u8 form) {
    const int at = SlotOf(prepared);
    if (at >= 0) {
        g_form[at].store(form, std::memory_order_release);
    }
}

u8 FormOf(u64 prepared) {
    const int at = SlotOf(prepared);
    return at >= 0 ? g_form[at].load(std::memory_order_acquire) : static_cast<u8>(kFormApplication);
}

u64 AmTitleOf(u64 prepared) {
    return AmTitleFor(prepared, FormOf(prepared));
}

u64 ProcessIdOf(u64 prepared) {
    return ProcessIdFor(prepared, FormOf(prepared));
}

void Unregister(u64 prepared) {
    for (std::size_t i = 0; i < kRegistrySlots; ++i) {
        u64 was = prepared;
        if (g_owned[i].compare_exchange_strong(was, 0, std::memory_order_acq_rel)) {
            g_form[i].store(kFormApplication, std::memory_order_release);
        }
    }
}

void ClearRegistry() {
    for (std::size_t i = 0; i < kRegistrySlots; ++i) {
        g_owned[i].store(0, std::memory_order_release);
        g_form[i].store(kFormApplication, std::memory_order_release);
    }
}

std::vector<Guest> LoadRegistry(int* unowned) {
    std::vector<Guest> guests = LoadGuests();
    ClearRegistry();
    int left = 0;
    for (const Guest& g : guests) {
        left += Register(g.prepared, g.form) ? 0 : 1;
    }
    if (unowned) {
        *unowned = left;
    }
    return guests;
}

// ---- children.cache ------------------------------------------------------------------------------------------------------------------
std::string CachePath() {
    return GuestsDir() + "/" + kCacheFileName;
}

std::string CacheText(const std::vector<ChildSeen>& seen) {
    std::string t = std::string(kCacheHeader) + "\n";
    for (const ChildSeen& c : seen) {
        t += Hex16(c.game) + ":" + Dec(c.game_version) + ":" + Dec(c.update_version) + ":";
        if (c.error != 0) {
            t += "failed:" + Dec(static_cast<u32>(c.error)) + ":" + Dec(c.keys) + "\n"; // (a line an older app skips as broken)
            continue;
        }
        if (!c.has_child) {
            t += "none\n";
            continue;
        }
        t += Hex16(c.child) + ":" + Dec(c.child_version) + ":" + HexBytes(c.child_hash8, sizeof(c.child_hash8)) + ":" + Dec(c.transfer) + ":" +
             Dec(c.required) + ":" + ProductText(c.product) + "\n";
    }
    return t;
}

bool ParseCache(const std::string& text, std::vector<ChildSeen>* out) {
    const std::vector<std::string> lines = Lines(text);
    if (lines.empty() || lines[0] != kCacheHeader) {
        return false;
    }
    out->clear();
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty() || lines[i][0] == '#') {
            continue;
        }
        const std::vector<std::string> f = Fields(lines[i]);
        ChildSeen c;
        u32 gv = 0, uv = 0;
        bool ok = (f.size() == 4 || f.size() == 6 || f.size() == 9) && ReadId(f[0], &c.game) && ReadDec(f[1], 0xFFFF, &gv) && ReadDec(f[2], 0xFFFF, &uv) &&
                  PreparedFor(c.game) != 0;
        if (ok && f.size() == 4) {
            ok = f[3] == "none";
        } else if (ok && f.size() == 6) {
            u32 err = 0;
            ok = f[3] == "failed" && ReadDec(f[4], 0x7FFFFFFFu, &err) && err != 0 && ReadDec(f[5], 0xFFFFFFFFu, &c.keys);
            c.error = static_cast<int>(err);
        } else if (ok) {
            u32 cv = 0;
            ok = ReadId(f[3], &c.child) && static_cast<u32>(c.child >> 32) == azl::title_rules::kTitleHighDlpChild && ReadDec(f[4], 0xFFFF, &cv) &&
                 ReadBytes(f[5], c.child_hash8, sizeof(c.child_hash8)) && ReadDec(f[6], 0xFFFFFFFFu, &c.transfer) && ReadDec(f[7], 0xFFFFFFFFu, &c.required) &&
                 ReadProduct(f[8], c.product);
            c.child_version = static_cast<u16>(cv);
            c.has_child = true;
        }
        c.game_version = static_cast<u16>(gv);
        c.update_version = static_cast<u16>(uv);
        bool again = false; // (one line per game and versions: the first one stays)
        for (const ChildSeen& o : *out) {
            again = again || (o.game == c.game && o.game_version == c.game_version && o.update_version == c.update_version);
        }
        if (ok && !again) {
            out->push_back(c);
        }
    }
    return true;
}

std::vector<ChildSeen> LoadChildrenCache() {
    std::string text;
    std::vector<ChildSeen> seen;
    if (setup::ReadTextFile(CachePath(), kCacheMaxBytes, text) != setup::LoadResult::Loaded || !ParseCache(text, &seen)) {
        seen.clear();
    }
    return seen;
}

bool SaveChildrenCache(const std::vector<ChildSeen>& seen) {
    setup::MakeDirs(GuestsDir());
    return setup::WriteTextFile(CachePath(), CacheText(seen));
}

const ChildSeen* FindChildSeen(const std::vector<ChildSeen>& seen, u64 game, u16 game_version, u16 update_version) {
    for (const ChildSeen& c : seen) {
        if (c.error == 0 && c.game == game && c.game_version == game_version && c.update_version == update_version) {
            return &c;
        }
    }
    return nullptr;
}

const ChildSeen* FindChildFailure(const std::vector<ChildSeen>& seen, u64 game, u16 game_version, u16 update_version) {
    for (const ChildSeen& c : seen) {
        if (c.error != 0 && c.game == game && c.game_version == game_version && c.update_version == update_version) {
            return &c;
        }
    }
    return nullptr;
}

// ---- what the plugin reads ---------------------------------------------------------------------------------------------------------
std::string IndexText(const std::vector<Guest>& guests, const std::vector<Waiting>& waiting, int* dropped) {
    namespace dc = azl::dlp_children;
    std::string t = std::string(dc::kIndexHeader) + "\n";
    std::vector<u64> children;
    int lost = 0;
    const auto add = [&](const dc::Row& r) {
        if (std::find(children.begin(), children.end(), r.child) != children.end()) {
            return; // one row per child (the plugin refuses a second): the first one, the prepared one, stays
        }
        char line[dc::kRowBytes + 1];
        if (children.size() >= dc::kMaxRows || dc::FormatRow(r, line) == 0) {
            ++lost;
            return;
        }
        children.push_back(r.child);
        t += line;
    };
    for (const Guest& g : guests) {
        if (g.state == GuestState::Installed) {
            add(dc::Row{g.child, g.game, g.prepared, g.child_version, g.where});
        }
    }
    for (const Waiting& w : waiting) {
        add(dc::Row{w.child, w.game, 0, w.child_version, w.where});
    }
    if (dropped) {
        *dropped = lost;
    }
    return t;
}

bool WriteIndex(const std::string& text, bool always) {
    const std::string path = IndexPath();
    std::string now;
    if (!always && setup::ReadTextFile(path, azl::dlp_children::kIndexMaxBytes * 4, now) == setup::LoadResult::Loaded && now == text) {
        return true;
    }
    setup::MakeDirs(GuestsDir());
    return setup::WriteTextFile(path, text);
}

bool WriteBlock(u64 prepared, const u8* smdh, std::size_t smdh_size, u8 language, u32 transfer, u32 required) {
    std::string block(azl::dlp_children::kBlockBytes, '\0');
    if (!azl::title_rules::IsPreparedId(prepared) ||
        !azl::dlp_children::MakeBlock(reinterpret_cast<u8*>(&block[0]), smdh, smdh_size, language, transfer, required)) {
        return false;
    }
    setup::MakeDirs(GuestsDir());
    return setup::WriteTextFile(BlockPath(prepared), block);
}

// ---- the leftovers ----------------------------------------------------------------------------------------------------------------
Sweep SweepOrphans(const std::vector<Guest>& alive) {
    Sweep s;
    const std::string dir = GuestsDir();
    const std::string mine = RecordsDir(); // (DPAPP-2j: this console's records; "" when the ID0 is not known)
    // First the halves of WriteTextFile: a .bak is the whole old file of a write that was cut. With its file there it is dropped; alone it IS the
    // file (setup::ReadTextFile would put it back too) and is put back, so nothing is lost before the file is written again. (DPAPP-2j) In the
    // shared folder (the plugin's files, the records of the builds before) and in this console's: never in another console's folder.
    for (const std::string& folder : {dir, mine}) {
        if (folder.empty()) {
            continue;
        }
        for (const std::string& name : FileNames(folder)) {
            const std::string path = folder + "/" + name;
            if (EndsWith(name, ".tmp")) {
                std::remove(path.c_str());
                ++s.temps;
            } else if (EndsWith(name, ".bak")) {
                const std::string file = path.substr(0, path.size() - 4);
                if (IsFile(file)) {
                    std::remove(path.c_str());
                    ++s.baks;
                } else if (std::rename(path.c_str(), file.c_str()) == 0) {
                    ++s.restored;
                }
            }
        }
    }
    // Then the work files of this console's folder and the blocks of the titles that are not (or no longer) there. (DPAPP-2j) A block another
    // console's record names is kept: the card may go back to it
    if (!mine.empty()) {
        for (const std::string& name : FileNames(mine)) {
            u64 p = 0;
            if (PreparedOfName(name, ".part", &p)) {
                const Guest* g = FindGuest(alive, p);
                if (g == nullptr || g->state != GuestState::Preparing) {
                    std::remove((mine + "/" + name).c_str());
                    ++s.parts;
                }
            }
        }
    }
    const std::vector<u64> others = OtherConsolesKeys();
    for (const std::string& name : FileNames(dir)) {
        const std::string path = dir + "/" + name;
        u64 p = 0;
        if (PreparedOfName(name, ".bin", &p)) {
            const Guest* g = FindGuest(alive, p);
            const bool has_child = g != nullptr && (g->state == GuestState::Installed || (g->state == GuestState::Preparing && (g->prev_installed || g->prev_other)));
            if (!has_child && !std::binary_search(others.begin(), others.end(), p)) {
                std::remove(path.c_str());
                ++s.blocks;
            }
        }
    }
    return s;
}

// ---- the start ----------------------------------------------------------------------------------------------------------------------
// (DPAPP-2j, R-B) the records of the shared folder (the builds before DPAPP-2j wrote them there) that are THIS console's go into its folder: written
// by this console (console= is ThisConsole), or without the key (the builds up to 2h) and AM lists their title in their form with their product
// code. Every other one - another console's, unproved, unreadable - stays where it is, intact: never read as ours, never deleted. A copy already in
// this console's folder (a start cut between the write and the remove) is the record; the shared one goes only when it is the same text.
void MoveSharedRecords(StartConsole& console, StartReport* r) {
    const std::string dir = GuestsDir();
    const std::string mine = RecordsDir();
    if (mine.empty()) {
        return;
    }
    for (const std::string& name : FileNames(dir)) {
        u64 p = 0;
        if (!PreparedOfName(name, ".guest", &p)) {
            continue;
        }
        const std::string from = dir + "/" + name;
        std::string text;
        Guest g;
        if (setup::ReadTextFile(from, kGuestMaxBytes, text) != setup::LoadResult::Loaded || !ParseGuest(text, &g) || g.prepared != p) {
            ++r->other;
            continue;
        }
        bool ours = g.console != 0 && ThisConsole() != 0 && g.console == ThisConsole();
        if (!ours && g.console == 0) {
            u16 version = 0;
            char product[kProductBytes] = {};
            ours = console.Installed(p, g.form, &version) == Presence::Present && console.Product(p, g.form, product) && product[0] != '\0' &&
                   g.product[0] != '\0' && std::strncmp(product, g.product, 15) == 0;
        }
        if (!ours) {
            ++r->other;
            continue;
        }
        const std::string to = mine + "/" + name;
        std::string there;
        if (setup::ReadTextFile(to, kGuestMaxBytes, there) != setup::LoadResult::Loaded) {
            setup::MakeDirs(mine);
            if (!setup::WriteTextFile(to, text) || setup::ReadTextFile(to, kGuestMaxBytes, there) != setup::LoadResult::Loaded) {
                ++r->other; // (not written: the shared one stays, and is moved at the next start)
                continue;
            }
        }
        if (there == text) {
            std::remove(from.c_str());
            std::remove((from + ".bak").c_str());
        }
        ++r->moved;
    }
}

StartReport SweepAtStart(StartConsole& console) {
    StartReport r;
    MoveSharedRecords(console, &r); // (DPAPP-2j, R-B: before the records are read - the moved ones are this console's from now)
    std::vector<Guest> guests = LoadRegistry(&r.unowned);
    r.guests = static_cast<int>(guests.size());
    if (guests.empty() && !IsDir(GuestsDir())) {
        return r; // (never prepared anything: no folder is made)
    }
    std::vector<Guest> alive;
    for (Guest& g : guests) {
        if (!IsOwnedGuestTid(g.prepared)) {
            alive.push_back(g); // (no registry slot was left for it: its title is not ours to this run, nothing of it is touched)
            continue;
        }
        u16 version = 0;
        const Presence at = console.Installed(g.prepared, g.form, &version); // (the form of the record: the id AM has the title by)
        char product[kProductBytes] = {};
        if (at == Presence::Present || at == Presence::Broken) {
            console.Product(g.prepared, g.form, product);
        }
        // (the review of the build 2f) a migration that was cut: the old child, in the other form - whole, at its version, ours - is gone back to
        Presence other_at = Presence::Absent;
        u16 other_version = 0;
        char other_product[kProductBytes] = {};
        if (g.state == GuestState::Preparing && g.prev_other) {
            // (the build 2h: the form the record says the old child was in, prev_other_form)
            other_at = console.Installed(g.prepared, g.prev_other_form, &other_version);
            if (other_at == Presence::Present || other_at == Presence::Broken) {
                console.Product(g.prepared, g.prev_other_form, other_product);
            }
        }
        // (the build 2h, R7) the title of the record's form is ours NOW: listed, its product code read and the record's (the legacy rule of the ticket)
        const bool title_ours_now = (at == Presence::Present || at == Presence::Broken) && product[0] != '\0' && g.product[0] != '\0' &&
                                    !OtherProduct(product, g.product);
        const StartAct act = DecideAtStart(g, at, version, product, other_at, other_version, other_product);
        if (act == StartAct::Keep || act == StartAct::Later) {
            ++(act == StartAct::Keep ? r.kept : r.later);
            alive.push_back(g);
            continue;
        }
        if (act == StartAct::Revert) {
            // AM puts the new title in place only at its Finish: a run that died before it left the old child whole. What AM had begun of the new
            // one is cancelled (THAT title's pending installation: the installed one is not touched). The record's child was the NEW run's (its
            // version, hash, sizes and composer): it names the old child's version now, and composer 0 has the next look make it again (Decide)
            console.DeletePending(g.prepared, g.form); // (the new run's, cut before its Finish; Revert is of the same form only: prev_installed)
            g.state = GuestState::Installed;
            g.prev_installed = false;
            g.child_version = g.prev_version;
            g.composer = 0;
            SaveGuest(g);
            std::remove(PartPath(g.prepared).c_str());
            ++r.reverted;
            alive.push_back(g);
            continue;
        }
        if (act == StartAct::KeepNew) {
            // (the review of the build 2h, R2) a migration from M3c1 cut after AM's Finish: the new child is there and ours - kept in its form,
            // composer 0 (the next look makes it again: the record's child and block are the old one's); the old form's title is a Repair of the
            // next look (DropOtherForm), never deleted here. What AM may have begun of the new id is cancelled (THAT title's)
            console.DeletePending(g.prepared, g.form);
            g.state = GuestState::Installed;
            g.child_version = version;
            g.composer = 0;
            g.prev_installed = false;
            g.prev_version = 0;
            g.prev_other = false;
            g.prev_other_version = 0;
            g.prev_other_form = kFormApplication;
            SaveGuest(g);
            SetForm(g.prepared, g.form);
            std::remove(PartPath(g.prepared).c_str());
            ++r.reverted;
            alive.push_back(g);
            continue;
        }
        if (act == StartAct::RevertOther) {
            // (the review of the build 2f) a migration cut before its end: the old child, in the other form, is there whole and ours. The record goes
            // back to it - Installed in that form, with the composer it had (the run's record kept it: "to update" again) - and what the run made of
            // the new form goes (THAT pending title,
            // and the title when AM lists it: the record's form, the same product code or none read - Disown came first). A delete that fails
            // leaves it to the next look (the other form's id of our child: Repair)
            console.DeletePending(g.prepared, g.form);
            if (at == Presence::Broken || title_ours_now) {
                // (the review of the build 2h, R3: R6 - a listed title whose code does not read is never deleted on the record alone; it stays, and
                // the next migration installs over it)
                console.DeleteTitle(g.prepared, g.form, TicketMayGo(g, g.form, title_ours_now));
            } else if (at == Presence::Absent && TicketWordHere(g) && g.ticket_ours) {
                console.DropTicket(g.prepared, g.form); // (the review of the build 2h, R1: the ticket the run cut after its Begin left, ours by the record)
            }
            g.form = g.prev_other_form;
            g.ticket_known = false; // (the old child's ticket: the builds before did not say whose it is - TicketMayGo's legacy rule)
            g.ticket_ours = false;
            g.state = GuestState::Installed;
            g.child_version = g.prev_other_version;
            g.prev_other = false;
            g.prev_other_version = 0;
            g.prev_installed = false;
            g.prev_version = 0;
            SaveGuest(g);
            SetForm(g.prepared, g.form);
            std::remove(PartPath(g.prepared).c_str());
            ++r.reverted;
            alive.push_back(g);
            continue;
        }
        // Taken away (a run that died, a removal) or disowned (the id holds a title that is not ours any more: another product code, so ours was
        // deleted by hand and something else was installed there), in this order: the plugin's copy and its folder (a folder left behind would be
        // brought up to date forever), the block, the installation AM may have begun (THAT title's only) and the title - both only for a title
        // of ours: a disowned one, and whatever AM has of it, is NEVER touched - the work file, and the record last, so a cut anywhere leaves a
        // record that says what is still to do. A title AM lists is deleted (a Broken one too); only one AM does not list is gone.
        // (the build 2h) the folders of both process ids: the key's (the fallback, the builds before) and the system id's
        const bool folder_gone = console.DisableFolder(ProcessIdFor(g.prepared, kFormSystem), g.game) & console.DisableFolder(g.prepared, g.game);
        std::remove(BlockPath(g.prepared).c_str());
        bool title_gone = true;
        if (act == StartAct::TakeAway) {
            console.DeletePending(g.prepared, g.form);
            // (the build 2h, R6) the record's title is deleted only when it is ours by its product code read, or a Broken one (an install of ours cut in
            // its commit): a listed title whose code does not read is kept, and the record with it (Removing: the next look says it again)
            const bool ours_to_delete = at == Presence::Broken || title_ours_now;
            title_gone = at == Presence::Absent || (ours_to_delete && console.DeleteTitle(g.prepared, g.form, TicketMayGo(g, g.form, title_ours_now)));
            if (at == Presence::Absent && TicketWordHere(g) && g.ticket_ours) {
                // (the review of the build 2h, R1) a run cut after its Begin left the ticket the record says is ours, with no title: it goes with the
                // record - or the record stays (Removing), never the next child of the slot refused for "a ticket that is not ours"
                title_gone = console.DropTicket(g.prepared, g.form);
            }
            if (title_gone && TicketWordUnplaced(g)) {
                // (DPAPP-2h-sec, second pass) the record's word on the ticket is not known to be this console's: nothing is dropped on it, and the
                // record goes only when the console holds no ticket at the id - otherwise it stays (Removing), never a ticket of the slot unnamed
                title_gone = console.TicketAt(g.prepared, g.form) == Presence::Absent;
            }
            // (the build 2f) the ids of the other forms too - what a migration cut in the middle left - but only when AM lists one AND its product
            // code reads and is the record's: another one, or one that does not read, is never touched (K9.1). The build 2h: two other forms
            for (const u8 other : kForms) {
                if (other == g.form) {
                    continue;
                }
                u16 other_version = 0;
                const Presence other_at = console.Installed(g.prepared, other, &other_version);
                char other_product[kProductBytes] = {};
                if ((other_at == Presence::Present || other_at == Presence::Broken) && console.Product(g.prepared, other, other_product) &&
                    other_product[0] != '\0' && g.product[0] != '\0' && !OtherProduct(other_product, g.product)) {
                    console.DeletePending(g.prepared, other);
                    // (ours now in that form: its ticket too, R7). DPAPP-2h-sec, second pass: a delete that took the title and not its ticket (AM's list
                    // not read, AM kept it) - the ticket alone at once, behind its guard; the record stays while it is there
                    const bool other_gone = console.DeleteTitle(g.prepared, other, true) ||
                                            (console.Installed(g.prepared, other, &other_version) == Presence::Absent && console.DropTicket(g.prepared, other));
                    title_gone = other_gone && title_gone;
                }
            }
        }
        std::remove(PartPath(g.prepared).c_str());
        if (folder_gone && title_gone) {
            DropGuest(g.prepared);
            Unregister(g.prepared);
            ++(act == StartAct::TakeAway ? r.taken : r.disowned);
        } else {
            g.state = GuestState::Removing;
            SaveGuest(g);
            ++r.failed;
            alive.push_back(g);
        }
    }
    r.sweep = SweepOrphans(alive);
    r.index_written = WriteIndex(IndexText(alive, {}, nullptr), true);
    return r;
}

} // namespace dlpguest
