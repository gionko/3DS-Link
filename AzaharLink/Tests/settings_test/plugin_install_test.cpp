// The plugin of 3DS Link is ONE package with the app: the app carries AzaharLink.3gx and puts it on the SD card by itself (ConfigApp/source/plugin_install.hpp and .cpp, the very
// files, plain C++, built on a PC with a stand-in for <3ds.h> and run on a temporary folder), run by Tests/run.sh. What it pins: how the file on the card is told from the plugin
// inside the app (missing, unreadable, not a plugin, another version, a header that looks good over bytes that are not the app's, the same: the BYTES decide), what the app does
// about each (puts it, replaces it, leaves it alone, and does nothing when this build has no plugin to put), that a write is whole or leaves the old file (a folder in the way,
// a folder that cannot be made, a card that refuses the file, a swap that a power cut cut in half), no .part or .old file left behind, the words that tell it (the toast, the
// sentence of the shut switch, the step of the first start) and the rule of the switch (a Luma3DS that is too old first, then a plugin that is not on the card).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_install.hpp"

namespace fs = std::filesystem;
using namespace setup;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

// A plugin that is made up: "3GX$", the format, the version word (1.0.1), then bytes that are all different (a flipped one is found at any place).
static std::vector<std::uint8_t> MakePlugin(std::size_t size, std::uint32_t version_word = 0x01000100u) {
    std::vector<std::uint8_t> p(size);
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        p[i] = static_cast<std::uint8_t>(x >> 24);
    }
    std::memcpy(p.data(), "3GX$0002", 8);
    std::memcpy(p.data() + 8, &version_word, 4);
    return p;
}

static void Put(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        if (!bytes.empty()) {
            std::fwrite(bytes.data(), 1, bytes.size(), f); // (an empty file: nothing is written, and data() of an empty vector is null)
        }
        std::fclose(f);
    }
}

static std::vector<std::uint8_t> Get(const std::string& path) {
    std::vector<std::uint8_t> v;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            v.insert(v.end(), buf, buf + n);
        }
        std::fclose(f);
    }
    return v;
}

static bool Exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static const std::uint32_t kWanted = 0x01000100u; // 1.0.1

// ---------------------------------------------------------------------------------------------
// The decisions
// ---------------------------------------------------------------------------------------------

static void TestClassify() {
    PluginProbe p;
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::Missing);
    p.exists = true;
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::Unreadable); // (there, not readable)
    p.readable = true;
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::NotPlugin);  // (no header)
    p.header_ok = true;
    p.version_word = 0x01000000u; // 1.0.0
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::OtherVersion);
    p.version_word = kWanted;
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::Altered);    // (a good header of the right version, and bytes that are not the app's)
    p.same_bytes = true;
    CHECK(ClassifyPluginCard(p, kWanted) == PluginCard::Same);
    // the bytes decide, never the header: the same bytes with a header that this check does not like cannot happen, but "Same" needs nothing else
    PluginProbe q;
    q.exists = true;
    q.readable = true;
    q.same_bytes = true;
    CHECK(ClassifyPluginCard(q, kWanted) == PluginCard::Same);
    // a file that is not there is missing whatever else the probe says
    PluginProbe r;
    r.same_bytes = true;
    r.header_ok = true;
    CHECK(ClassifyPluginCard(r, kWanted) == PluginCard::Missing);
}

static void TestDecide() {
    for (const PluginCard c : {PluginCard::Missing, PluginCard::Unreadable, PluginCard::NotPlugin, PluginCard::OtherVersion, PluginCard::Altered, PluginCard::Same}) {
        CHECK(DecideInstall(c, false) == PluginStep::Leave); // (a build with no plugin inside has nothing to put, whatever the card holds)
    }
    CHECK(DecideInstall(PluginCard::Same, true) == PluginStep::Leave);
    CHECK(DecideInstall(PluginCard::Missing, true) == PluginStep::Put);
    for (const PluginCard c : {PluginCard::Unreadable, PluginCard::NotPlugin, PluginCard::OtherVersion, PluginCard::Altered}) {
        CHECK(DecideInstall(c, true) == PluginStep::Replace); // (one version: a different file is replaced, never kept)
    }
}

static void TestToastTime() {
    // a short toast keeps its time, a long one (it runs by on the main page) gets what the run needs: two seconds and 150 ms a letter; never less than it was given
    CHECK(ToastMsToRead(0, 1800) == 1800);
    CHECK(ToastMsToRead(6, 1800) == 1800);
    CHECK(ToastMsToRead(40, 6000) == 6000);                 // 40 letters or fewer: as given
    CHECK(ToastMsToRead(41, 4500) == 2000 + 150 * 41);      // 8150 ms
    CHECK(ToastMsToRead(100, 7000) == 2000 + 150 * 100);    // the sentence of a plugin that could not be put: 17 s, not the 7 s that cut its reason off
    CHECK(ToastMsToRead(153, 12000) == 2000 + 150 * 153);
    CHECK(ToastMsToRead(60, 12000) == 12000);               // a time already longer than the run stays
    for (std::size_t n = 0; n < 400; ++n) {
        CHECK(ToastMsToRead(n, 5000) >= 5000);
        CHECK(n <= 40 || ToastMsToRead(n, 0) >= 2000 + 150 * n);
    }
}

static void TestWords() {
    CHECK(std::string(WriteFailurePhrase(WriteResult::Ok)).empty());
    for (const WriteResult w : {WriteResult::NoFolder, WriteResult::FolderInTheWay, WriteResult::CannotWrite, WriteResult::NotVerified, WriteResult::CannotPlace}) {
        const std::string phrase = WriteFailurePhrase(w);
        CHECK(phrase.size() > 10 && phrase.back() != '.' && phrase[0] >= 'a' && phrase[0] <= 'z'); // (it ends a sentence: no capital, no full stop)
    }
    const std::string folder = "sdmc:/3ds/3DSLink";
    PluginEnsure put;
    put.before = PluginCard::Missing;
    put.step = PluginStep::Put;
    put.after = PluginCard::Same;
    CHECK(put.wrote() && !put.failed());
    CHECK(PluginInstallToast(put, folder) == "The plugin was put in sdmc:/3ds/3DSLink.");
    PluginEnsure old = put;
    old.before = PluginCard::OtherVersion;
    old.step = PluginStep::Replace;
    CHECK(PluginInstallToast(old, folder) == "The plugin in sdmc:/3ds/3DSLink was updated to this version.");
    PluginEnsure bad = old;
    bad.before = PluginCard::Altered;
    CHECK(PluginInstallToast(bad, folder) == "The plugin in sdmc:/3ds/3DSLink was damaged: it was replaced.");
    PluginEnsure fail = put;
    fail.write = WriteResult::CannotWrite;
    fail.after = PluginCard::Missing;
    CHECK(fail.failed() && !fail.wrote());
    CHECK(PluginInstallToast(fail, folder) == "Could not put the plugin in sdmc:/3ds/3DSLink: the SD card refused the file (is it full or locked?).");
    const PluginEnsure nothing;
    CHECK(!nothing.wrote() && !nothing.failed() && PluginInstallToast(nothing, folder).empty()); // (nothing was needed: nothing is said)

    // the sentence of the shut switch
    CHECK(PluginLockText(true, PluginCard::Same, folder, "1.0.1", "1.0.1", false, WriteResult::Ok).empty());
    CHECK(PluginLockText(true, PluginCard::Missing, folder, "", "1.0.1", true, WriteResult::CannotWrite) ==
          "AzaharLink.3gx is missing in sdmc:/3ds/3DSLink and could not be put there: the SD card refused the file (is it full or locked?).");
    CHECK(PluginLockText(true, PluginCard::Missing, folder, "", "1.0.1", false, WriteResult::Ok) == "AzaharLink.3gx is missing in sdmc:/3ds/3DSLink."); // (no try: nothing to blame)
    CHECK(PluginLockText(true, PluginCard::OtherVersion, folder, "1.0.0", "1.0.1", true, WriteResult::CannotPlace) ==
          "AzaharLink.3gx in sdmc:/3ds/3DSLink is version 1.0.0 (this app is 1.0.1) and could not be replaced: the old file could not be replaced.");
    CHECK(PluginLockText(true, PluginCard::OtherVersion, folder, "", "1.0.1", false, WriteResult::Ok).find("of another version") != std::string::npos);
    for (const PluginCard c : {PluginCard::Unreadable, PluginCard::NotPlugin, PluginCard::Altered}) {
        const std::string s = PluginLockText(true, c, folder, "", "1.0.1", true, WriteResult::FolderInTheWay);
        CHECK(s.find("is damaged and could not be replaced: a folder with that name is in the way.") != std::string::npos);
    }
    CHECK(PluginLockText(false, PluginCard::Same, folder, "", "1.0.1", false, WriteResult::Ok).find("no plugin inside") != std::string::npos); // (a build with no plugin says so first)
    CHECK(PluginLockText(false, PluginCard::Missing, folder, "", "1.0.1", true, WriteResult::CannotWrite).find("Install the 3DS Link CIA again") != std::string::npos);
    CHECK(std::string(kPluginLockMore).find("Installation check") != std::string::npos && std::string(kPluginLockMore).find("every start") != std::string::npos);

    // the step of the first start
    CHECK(PluginStepDetail(true, PluginCard::Same, true, "1.0.1") == "put there by 3DS Link");
    CHECK(PluginStepDetail(true, PluginCard::Same, false, "1.0.1") == "v1.0.1");
    CHECK(PluginStepDetail(true, PluginCard::Missing, false, "") == "could not be put there");
    CHECK(PluginStepDetail(true, PluginCard::OtherVersion, false, "1.0.0") == "v1.0.0, not this app's");
    CHECK(PluginStepDetail(true, PluginCard::Altered, false, "1.0.1") == "damaged" && PluginStepDetail(true, PluginCard::NotPlugin, false, "") == "damaged");
    CHECK(PluginStepDetail(false, PluginCard::Same, false, "1.0.1") == "none in this app");
}

static void TestSwitch() {
    // Luma first: its reason is the one nothing in the app can mend
    CHECK(LockOfSwitch(true, true) == SwitchLock::Luma);
    CHECK(LockOfSwitch(true, false) == SwitchLock::Luma);
    CHECK(LockOfSwitch(false, false) == SwitchLock::Plugin);
    CHECK(LockOfSwitch(false, true) == SwitchLock::None);
    // what it shows: ON only when it is on in the settings file and nothing shuts it
    CHECK(SwitchIsOn(true, true, SwitchLock::None));
    CHECK(!SwitchIsOn(true, true, SwitchLock::Plugin));
    CHECK(!SwitchIsOn(true, true, SwitchLock::Luma));
    CHECK(!SwitchIsOn(false, true, SwitchLock::None));
    CHECK(!SwitchIsOn(true, false, SwitchLock::None)); // (tunnel=0 keeps the plugin off for a launch)
    // when the file comes back the switch is free again (it shows what the file says: OFF, since it went OFF when it was shut)
    const SwitchLock shut = LockOfSwitch(false, false);
    const SwitchLock unlocked = LockOfSwitch(false, true);
    CHECK(shut == SwitchLock::Plugin && unlocked == SwitchLock::None);
    CHECK(!SwitchIsOn(false, true, unlocked));
}

// ---------------------------------------------------------------------------------------------
// The file work
// ---------------------------------------------------------------------------------------------

static void NothingLeftBehind(const std::string& dest) {
    CHECK(!Exists(dest + ".part"));
    CHECK(!Exists(dest + ".old"));
}

static void TestEnsure(const std::string& root) {
    const std::vector<std::uint8_t> plugin = MakePlugin(300 * 1024 + 123); // (a size that is not a multiple of a block: the last block is a short one)
    const std::string dir = root + "/ensure/3ds/3DSLink"; // (a folder that is not there: the app makes it)
    const std::string dest = dir + "/AzaharLink.3gx";

    // missing: put (the folders are made), read back, and the same the next time
    PluginEnsure e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Missing && e.step == PluginStep::Put && e.write == WriteResult::Ok && e.after == PluginCard::Same && e.wrote());
    CHECK(Get(dest) == plugin);
    CHECK(e.header_ok && e.version_word == kWanted && e.size == plugin.size()); // (the file as the ensure saw it last: the app does not read the megabyte a second time)
    NothingLeftBehind(dest);
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Same && e.step == PluginStep::Leave && !e.wrote() && !e.failed() && e.after == PluginCard::Same);
    CHECK(e.header_ok && e.version_word == kWanted && e.size == plugin.size());
    CHECK(PluginInstallToast(e, dir).empty());

    // the file is cut short, its header good: the header says nothing, the bytes do
    std::vector<std::uint8_t> shorter(plugin.begin(), plugin.begin() + 100 * 1024);
    Put(dest, shorter);
    PluginProbe probe = InspectPluginFile(dest, plugin.data(), plugin.size());
    CHECK(probe.exists && probe.readable && probe.header_ok && probe.version_word == kWanted && !probe.same_bytes && probe.size == shorter.size());
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Altered && e.step == PluginStep::Replace && e.write == WriteResult::Ok && e.after == PluginCard::Same);
    CHECK(Get(dest) == plugin);
    NothingLeftBehind(dest);

    // the same size, ONE byte different in the middle (another build of the same version): found, replaced
    std::vector<std::uint8_t> flipped = plugin;
    flipped[flipped.size() / 2] ^= 0x01;
    Put(dest, flipped);
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Altered && e.step == PluginStep::Replace && e.after == PluginCard::Same && Get(dest) == plugin);
    // ... and the last byte
    flipped = plugin;
    flipped.back() ^= 0x80;
    Put(dest, flipped);
    CHECK(EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted).before == PluginCard::Altered);
    CHECK(Get(dest) == plugin);
    // a file that is longer by one byte
    flipped = plugin;
    flipped.push_back(0);
    Put(dest, flipped);
    CHECK(EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted).before == PluginCard::Altered && Get(dest) == plugin);

    // an older 3DS Link left its plugin (another version in the header), and the toast says it was updated
    std::vector<std::uint8_t> older = MakePlugin(plugin.size(), 0x01000000u);
    Put(dest, older);
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::OtherVersion && e.step == PluginStep::Replace && e.after == PluginCard::Same && Get(dest) == plugin);
    CHECK(e.version_word == kWanted); // (what the file says NOW, not what it said before the write)
    CHECK(PluginInstallToast(e, dir) == "The plugin in " + dir + " was updated to this version.");

    // not a plugin at all; and an empty file
    Put(dest, std::vector<std::uint8_t>(5000, 'x'));
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::NotPlugin && e.after == PluginCard::Same && Get(dest) == plugin);
    Put(dest, std::vector<std::uint8_t>());
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::NotPlugin && e.after == PluginCard::Same && Get(dest) == plugin);
    NothingLeftBehind(dest);

    // a swap that a power cut cut in half: there is no file, the old one is ".old": it comes back first (and is then the file that is looked at)
    std::remove(dest.c_str());
    Put(dest + ".old", older);
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::OtherVersion && e.after == PluginCard::Same && Get(dest) == plugin);
    NothingLeftBehind(dest);

    // a power cut between the rename of the new file and the removal of the old one: the card holds the plugin and a stray ".old" is left; the look takes it away
    Put(dest, plugin);
    Put(dest + ".old", older);
    e = EnsurePluginFile(dest, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Same && e.step == PluginStep::Leave && e.after == PluginCard::Same && Get(dest) == plugin);
    NothingLeftBehind(dest);

    // a build with no plugin inside puts nothing, whatever is (or is not) on the card
    const std::string other = root + "/ensure/none/AzaharLink.3gx";
    e = EnsurePluginFile(other, nullptr, 0, false, 0);
    CHECK(e.before == PluginCard::Missing && e.step == PluginStep::Leave && e.after == PluginCard::Missing && !Exists(other) && !Exists(root + "/ensure/none"));
}

static void TestWriteFailures(const std::string& root) {
    const std::vector<std::uint8_t> plugin = MakePlugin(70 * 1024);
    const std::vector<std::uint8_t> older = MakePlugin(plugin.size(), 0x01000000u);

    // a folder is where the file goes: nothing is written, the folder stays, and the check says what it is (a folder is not a file: "missing")
    const std::string in_the_way = root + "/fail/folder/AzaharLink.3gx";
    fs::create_directories(in_the_way);
    PluginEnsure e = EnsurePluginFile(in_the_way, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::Missing && e.step == PluginStep::Put && e.write == WriteResult::FolderInTheWay && e.failed() && e.after == PluginCard::Missing);
    CHECK(fs::is_directory(in_the_way) && !Exists(in_the_way + ".part") && !Exists(in_the_way + ".old"));
    CHECK(PluginInstallToast(e, root + "/fail/folder") == "Could not put the plugin in " + root + "/fail/folder: a folder with that name is in the way.");

    // the folder of the file cannot be made (a file is where the folder goes)
    Put(root + "/fail/blocked", older);
    e = EnsurePluginFile(root + "/fail/blocked/sub/AzaharLink.3gx", plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.step == PluginStep::Put && e.write == WriteResult::NoFolder && e.failed() && e.after == PluginCard::Missing);
    CHECK(!e.header_ok && e.size == 0 && e.version_word == 0); // (no file: nothing to say of it)
    CHECK(Get(root + "/fail/blocked") == older); // (what was there is as it was)

    // the card refuses the file (the .part file cannot be opened: a folder with something in it is there): the old file is still there, whole
    const std::string kept = root + "/fail/refused/AzaharLink.3gx";
    Put(kept, older);
    Put(kept + ".part/stay", older);
    e = EnsurePluginFile(kept, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.before == PluginCard::OtherVersion && e.step == PluginStep::Replace && e.write == WriteResult::CannotWrite && e.failed());
    CHECK(e.after == PluginCard::OtherVersion && Get(kept) == older);
    CHECK(e.header_ok && e.version_word == 0x01000000u && e.size == older.size()); // (the old file as it is: the app tells its version in the lock sentence)
    CHECK(PluginInstallToast(e, root + "/fail/refused").find("the SD card refused the file") != std::string::npos);
    CHECK(!Exists(kept + ".old"));
    // WritePluginFile alone, on a file that is not there
    const std::string fresh = root + "/fail/refused2/AzaharLink.3gx";
    Put(fresh + ".part/stay", older);
    CHECK(WritePluginFile(fresh, plugin.data(), plugin.size()) == WriteResult::CannotWrite && !Exists(fresh));

    // and once the way is clear it is written
    fs::remove_all(kept + ".part");
    e = EnsurePluginFile(kept, plugin.data(), plugin.size(), true, kWanted);
    CHECK(e.write == WriteResult::Ok && e.after == PluginCard::Same && Get(kept) == plugin);
    NothingLeftBehind(kept);
}

static void TestInspect(const std::string& root) {
    const std::vector<std::uint8_t> plugin = MakePlugin(4096);
    // not there, a folder, a file
    PluginProbe p = InspectPluginFile(root + "/inspect/none", plugin.data(), plugin.size());
    CHECK(!p.exists && !p.readable && !p.same_bytes);
    fs::create_directories(root + "/inspect/dir");
    p = InspectPluginFile(root + "/inspect/dir", plugin.data(), plugin.size());
    CHECK(!p.exists);
    Put(root + "/inspect/short", std::vector<std::uint8_t>{'3', 'G', 'X', '$'}); // (the magic and nothing else: shorter than a header)
    p = InspectPluginFile(root + "/inspect/short", plugin.data(), plugin.size());
    CHECK(p.exists && p.readable && !p.header_ok && !p.same_bytes && p.size == 4);
    Put(root + "/inspect/good", plugin);
    p = InspectPluginFile(root + "/inspect/good", plugin.data(), plugin.size());
    CHECK(p.exists && p.readable && p.header_ok && p.version_word == kWanted && p.same_bytes && p.size == plugin.size());
    // the same file against bytes that differ at the very end, and against a shorter plugin (the card's file is longer)
    std::vector<std::uint8_t> other = plugin;
    other.back() ^= 1;
    CHECK(!InspectPluginFile(root + "/inspect/good", other.data(), other.size()).same_bytes);
    CHECK(!InspectPluginFile(root + "/inspect/good", plugin.data(), plugin.size() - 1).same_bytes);
    // an empty plugin is not found in an empty file by accident: size 0 against size 0 is the same bytes, and it is not a plugin (the app never has one: EmbeddedPluginUsable)
    Put(root + "/inspect/empty", std::vector<std::uint8_t>());
    p = InspectPluginFile(root + "/inspect/empty", plugin.data(), 0);
    CHECK(p.exists && p.readable && !p.header_ok && p.size == 0);
}

// The stamp the copies in the titles' folders are brought up to (3DSLink.cfg plugin_stamp): it is made from the BYTES. Two plugins of the same size differ (the stamp of the build is a
// string of fixed length), a plugin that is cut short or longer differs, the same bytes give the same stamp, and an app that wrote the old "<size>:<date>" stamp never equals it.
static void TestContentStamp() {
    std::vector<std::uint8_t> plugin(1014783);
    for (std::size_t i = 0; i < plugin.size(); ++i) {
        plugin[i] = static_cast<std::uint8_t>((i * 31 + i / 7) & 0xFF);
    }
    const std::string a = PluginContentStamp(plugin.data(), plugin.size());
    CHECK(a.size() == 7 + 2 + 16); // "1014783:h" + 16 hex digits
    CHECK(a.compare(0, 9, "1014783:h") == 0);
    CHECK(a.find_first_not_of("0123456789abcdef", 9) == std::string::npos);
    CHECK(PluginContentStamp(plugin.data(), plugin.size()) == a); // (the same bytes, the same stamp)
    std::vector<std::uint8_t> copy = plugin; // (a different file with the same bytes)
    CHECK(PluginContentStamp(copy.data(), copy.size()) == a);
    // one byte different, anywhere (the first, the middle, the last): another stamp, though the size is the same
    for (const std::size_t at : {std::size_t(0), plugin.size() / 2, plugin.size() - 1}) {
        std::vector<std::uint8_t> other = plugin;
        other[at] ^= 1;
        const std::string b = PluginContentStamp(other.data(), other.size());
        CHECK(b != a);
        CHECK(b.compare(0, 9, "1014783:h") == 0);
    }
    // another length: another stamp (and the size is in front)
    CHECK(PluginContentStamp(plugin.data(), plugin.size() - 1) != a);
    CHECK(PluginContentStamp(plugin.data(), plugin.size() - 1).compare(0, 9, "1014782:h") == 0);
    // nothing to stamp: nothing (the copies are then never skipped)
    CHECK(PluginContentStamp(nullptr, 0).empty());
    CHECK(PluginContentStamp(plugin.data(), 0).empty());
    // the stamp an app of the days before wrote ("<size>:<date>", the date 0 on the card) is not this one
    CHECK(a != "1014783:0");
}

int main() {
    char tmpl[] = "/tmp/plugin_install_test_XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::printf("no temporary folder\n");
        return 1;
    }
    const std::string root = tmpl;
    TestClassify();
    TestDecide();
    TestWords();
    TestToastTime();
    TestSwitch();
    TestContentStamp();
    TestInspect(root);
    TestEnsure(root);
    TestWriteFailures(root);
    fs::remove_all(root);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
