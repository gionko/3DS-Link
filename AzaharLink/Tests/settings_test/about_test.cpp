// "About 3DS Link" and the credits (ConfigApp/source/about_text.hpp, the very file, plain C++), run by Tests/run.sh from AzaharLink/. What it pins: the NAME of the app is "3DS Link" and
// "Connect, share, play together" is its claim (the first line of the note of the first row), the version is the one number the caller hands in, "Created by Gionko", what the row "Installed as"
// says for each way the app can be started (the installed CIA by its title id, the file the Homebrew Launcher gave, or no path at all, which is never called "unknown"), what the
// plugin row says for every state of the file on the card against the plugin inside the app, and the credits: the list that the app shows, AzaharLink/CREDITS.md (the file the
// GitHub repository carries) saying every word of it, every entry fitting a row and a note card, and every entry being something the repository really has (ENet's own license and
// version, Luma3DS's csvc.h terms, the template's linker script, FBI's sequence in the installing code, GodMode9 in the CIA code, Pillow in the scripts, makerom's version).
//
// The test is run from AzaharLink/ (Tests/run.sh does that): `about_test [AzaharLink folder]` (default the current folder).
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "about_text.hpp"

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

static std::string g_dir = ".";

static bool Contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}
static bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

static bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// A rough model of gfx::WrapText (the same as dlp_text_test's): the paragraphs are cut at every '\n' (an empty one is a blank line) and the words are packed greedily into lines
// of `cols` characters. How many lines the text takes.
static int WrapLines(const std::string& text, std::size_t cols) {
    int lines = 0;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        const std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (para.empty()) {
            ++lines;
        } else {
            std::size_t used = 0;
            bool open = false;
            std::size_t at = 0;
            while (at < para.size()) {
                const std::size_t sp = para.find(' ', at);
                const std::size_t len = (sp == std::string::npos ? para.size() : sp) - at;
                if (len > 0) {
                    if (!open) {
                        ++lines;
                        open = true;
                        used = len;
                    } else if (used + 1 + len <= cols) {
                        used += 1 + len;
                    } else {
                        ++lines;
                        used = len;
                    }
                }
                at = sp == std::string::npos ? para.size() : sp + 1;
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return lines;
}

// The note card of the top screen (ui.cpp DrawNote: 380x168, text at 0.45 in 352 pixels) has room for nine lines; the emulator's stand-in font takes 69-70 characters of a line and a
// console's font fewer than that, so 60 is a margin that holds on both.
constexpr std::size_t kNoteCols = 60;
constexpr int kNoteLines = 9;

static bool PlainAscii(const std::string& s) {
    for (char c : s) {
        if (c != '\n' && (c < 0x20 || c >= 0x7F)) {
            return false;
        }
    }
    return true;
}

static void CheckWords(const std::string& text) {
    CHECK(PlainAscii(text));
    CHECK(!Contains(text, "  "));
    CHECK(!text.empty() && text.front() != ' ' && text.front() != '\n' && text.back() != ' ' && text.back() != '\n');
    CHECK(!Contains(text, " \n") && !Contains(text, "\n "));
}

// ---------------------------------------------------------------------------------------------

static void TestNames() {
    // The NAME is "3DS Link" and not the claim; the claim is the line under the name in the HOME menu (the Makefile's APP_DESCRIPTION: "Connect, share, play together . v<version>").
    CHECK(std::strcmp(about::kAppName, "3DS Link") == 0);
    CHECK(std::strcmp(about::kClaim, "Connect, share, play together") == 0);
    CHECK(std::strcmp(about::kAuthor, "Gionko") == 0);
    CHECK(std::strcmp(about::kCreatedBy, "Created by Gionko") == 0);
    CHECK(std::string(about::kCreatedBy) == std::string("Created by ") + about::kAuthor);
    CHECK(!Contains(about::kAppName, "Connect") && !Contains(about::kAppName, "Nintendo"));

    // The first row: the name at the left and the version at the right, one number, a "v" before it.
    CHECK(about::VersionLabel("1.0.0") == "v1.0.0");
    CHECK(about::VersionLabel("1.2.3") == "v1.2.3");
    const std::string note = about::AppNote("1.0.1");
    CHECK(StartsWith(note, about::kClaim)); // the claim is what the top screen shows first
    CHECK(Contains(note, "Version 1.0.1"));
    CHECK(Contains(note, "AzaharLink.ini"));
    CHECK(!Contains(note, "1.0.0"));
    CheckWords(note);
    CHECK(WrapLines(note, kNoteCols) <= kNoteLines);

    // Created by Gionko: who, and where the thanks are.
    const std::string created = about::CreatedByNote();
    CHECK(Contains(created, "created by Gionko"));
    CHECK(Contains(created, "Credits"));
    CHECK(Contains(created, "GPL"));
    CheckWords(created);
    CHECK(WrapLines(created, kNoteCols) <= kNoteLines);

    // The row that leads to the credits.
    CHECK(std::strcmp(about::kCreditsLabel, "Credits and thanks") == 0);
    CHECK(static_cast<int>(std::strlen(about::kCreditsLabel)) <= about::kMaxNameChars);
    CheckWords(about::kCreditsNote);
    CHECK(WrapLines(about::kCreditsNote, kNoteCols) <= kNoteLines);
    CHECK(Contains(about::kCreditsNote, "Azahar") && Contains(about::kCreditsNote, "FBI") && Contains(about::kCreditsNote, "3dbrew"));
}

static void TestOrigin() {
    const std::uint64_t app = 0x000400000F83D100ull, hbmenu = 0x000400000D921E00ull;
    using about::Origin;
    // a path from the launcher: it is a file, whatever else is known
    CHECK(about::OriginOf("sdmc:/3ds/3DSLink.3dsx", true, true, hbmenu, app) == Origin::File);
    CHECK(about::OriginOf("sdmc:/3ds/3DSLink.3dsx", false, false, 0, app) == Origin::File);
    CHECK(about::OriginOf("sdmc:/3ds/3DSLink.3dsx", false, true, app, app) == Origin::File);
    // no path: the system's answer decides when there is one
    CHECK(about::OriginOf("", false, true, app, app) == Origin::Installed);
    CHECK(about::OriginOf("", true, true, app, app) == Origin::Installed);
    CHECK(about::OriginOf("", false, true, hbmenu, app) == Origin::NoPath);
    CHECK(about::OriginOf("", true, true, hbmenu, app) == Origin::NoPath);
    CHECK(about::OriginOf("", false, true, 0, app) == Origin::NoPath);
    // no path and no answer: a launcher that is there says "no path"; without one it is the HOME menu (an installed app is started without a path)
    CHECK(about::OriginOf("", false, false, 0, app) == Origin::Installed);
    CHECK(about::OriginOf("", true, false, 0, app) == Origin::NoPath);

    CHECK(about::TitleIdText(app) == "000400000F83D100");
    CHECK(about::TitleIdText(0) == "0000000000000000");
    CHECK(about::TitleIdText(0x0004000E00123456ull) == "0004000E00123456");
    CHECK(about::FileNameOf("sdmc:/3ds/3DSLink.3dsx") == "3DSLink.3dsx");
    CHECK(about::FileNameOf("sdmc:/3DSLink.3dsx") == "3DSLink.3dsx");
    CHECK(about::FileNameOf("3DSLink.3dsx") == "3DSLink.3dsx");
    CHECK(about::FileNameOf("").empty());

    // The installed app: its title, where its files are; not a file name, not "unknown", not dim.
    const about::InstalledAs installed = about::DescribeOrigin(Origin::Installed, "", "sdmc:/3ds/3DSLink", "sdmc:/3ds/3DSLink", app);
    CHECK(installed.value == "installed app");
    CHECK(!installed.dim);
    CHECK(Contains(installed.note, "000400000F83D100"));
    CHECK(Contains(installed.note, "sdmc:/3ds/3DSLink"));
    CHECK(Contains(installed.note, "HOME menu"));
    CHECK(Contains(installed.note, "AzaharLink.3gx") && Contains(installed.note, "boot9.bin"));
    CHECK(Contains(installed.note, "puts AzaharLink.3gx there by itself")); // (the plugin is a resource of the app: it is put there, not looked for)
    CHECK(!Contains(installed.note, "unknown") && !Contains(installed.value, "unknown"));

    // A .3dsx: the path, its file name, where the files go (the app's folder, and its own when that is another).
    const about::InstalledAs file = about::DescribeOrigin(Origin::File, "sdmc:/3ds/3DSLink.3dsx", "sdmc:/3ds", "sdmc:/3ds/3DSLink", app);
    CHECK(file.value == "3DSLink.3dsx");
    CHECK(!file.dim);
    CHECK(Contains(file.note, "sdmc:/3ds/3DSLink.3dsx"));
    CHECK(Contains(file.note, "Homebrew Launcher"));
    CHECK(Contains(file.note, "next to the app (sdmc:/3ds) or in that folder"));
    CHECK(Contains(file.note, "puts AzaharLink.3gx in its own folder by itself"));
    CHECK(Contains(file.note, "are in sdmc:/3ds/3DSLink."));
    CHECK(!Contains(file.note, "000400000F83D100"));
    const about::InstalledAs own = about::DescribeOrigin(Origin::File, "sdmc:/3ds/3DSLink/3DSLink.3dsx", "sdmc:/3ds/3DSLink", "sdmc:/3ds/3DSLink", app);
    CHECK(Contains(own.note, "next to the app (sdmc:/3ds/3DSLink).")); // the app's folder is its own: no "or in that folder"
    CHECK(!Contains(own.note, "or in that folder"));

    // No path: it says so in words, faint, and what folder it uses; never the word "unknown".
    const about::InstalledAs nopath = about::DescribeOrigin(Origin::NoPath, "", "sdmc:/3ds/3DSLink", "sdmc:/3ds/3DSLink", app);
    CHECK(nopath.value == "no path given");
    CHECK(nopath.dim);
    CHECK(Contains(nopath.note, "sdmc:/3ds/3DSLink"));
    CHECK(Contains(nopath.note, "emulator"));
    CHECK(!Contains(nopath.note, "unknown") && !Contains(nopath.value, "unknown"));

    for (const about::InstalledAs* shown : {&installed, &file, &own, &nopath}) {
        CHECK(static_cast<int>(shown->value.size()) <= 20); // the value takes at most 45% of a row
        CheckWords(shown->note);
        CHECK(WrapLines(shown->note, kNoteCols) <= kNoteLines);
    }
}

static void TestPluginRow() {
    using setup::PluginCard;
    const std::string path = "sdmc:/3ds/3DSLink/AzaharLink.3gx";
    const PluginCard all[] = {PluginCard::Missing, PluginCard::Unreadable, PluginCard::NotPlugin, PluginCard::OtherVersion, PluginCard::Altered, PluginCard::Same};

    // The file is the plugin inside the app: good, with the version and the size.
    const about::PluginRow same = about::DescribePluginRow(PluginCard::Same, true, path, "1.0.0", "1.02 MB", "1.0.0");
    CHECK(same.tone == about::Tone::Good);
    CHECK(same.value == "1.0.0, 1.02 MB");
    CHECK(Contains(same.note, path) && Contains(same.note, "byte for byte"));

    // Every other state: not good, a short word, the path, and what the app does (it puts its own plugin there).
    for (PluginCard card : all) {
        if (card == PluginCard::Same) {
            continue;
        }
        const about::PluginRow row = about::DescribePluginRow(card, true, path, card == PluginCard::OtherVersion ? "0.9.0" : "", "1.02 MB", "1.0.0");
        CHECK(row.tone == about::Tone::Bad); // (every state but the good one is a problem in the Installation check, and shuts the 3DS Link switch)
        CHECK(!row.value.empty());
        CHECK(Contains(row.note, path));
        CHECK(Contains(row.note, "Installation check"));
        CHECK(Contains(row.note, "by itself at every start"));
    }
    CHECK(about::DescribePluginRow(PluginCard::Missing, true, path, "", "", "1.0.0").value == "missing");
    CHECK(about::DescribePluginRow(PluginCard::Missing, true, path, "", "", "1.0.0").tone == about::Tone::Bad); // (red like the Installation check: it shuts the switch)
    CHECK(about::DescribePluginRow(PluginCard::Unreadable, true, path, "", "", "1.0.0").tone == about::Tone::Bad);
    CHECK(about::DescribePluginRow(PluginCard::NotPlugin, true, path, "", "100 B", "1.0.0").value == "not a plugin");
    CHECK(about::DescribePluginRow(PluginCard::NotPlugin, true, path, "", "100 B", "1.0.0").tone == about::Tone::Bad);
    CHECK(about::DescribePluginRow(PluginCard::Altered, true, path, "1.0.0", "900 KB", "1.0.0").value == "damaged");
    const about::PluginRow other = about::DescribePluginRow(PluginCard::OtherVersion, true, path, "0.9.0", "1.02 MB", "1.0.0");
    CHECK(other.value == "0.9.0, not 1.0.0");
    CHECK(Contains(other.note, "Version 0.9.0 in the file; this app is 1.0.0"));
    CHECK(about::DescribePluginRow(PluginCard::OtherVersion, true, path, "", "1.02 MB", "1.0.0").value == "other version");

    // A copy of the app with no plugin inside cannot put one on the card, whatever the card holds: it says so.
    for (PluginCard card : all) {
        const about::PluginRow row = about::DescribePluginRow(card, false, path, "1.0.0", "1.02 MB", "1.0.0");
        CHECK(row.tone == about::Tone::Bad);
        CHECK(row.value == "none inside");
        CHECK(Contains(row.note, "no plugin inside") && Contains(row.note, "install the 3DS Link CIA again"));
    }

    // They all fit a row and the note card.
    for (PluginCard card : all) {
        for (bool usable : {true, false}) {
            const about::PluginRow row = about::DescribePluginRow(card, usable, path, "0.9.0", "1.02 MB", "1.0.0");
            CHECK(static_cast<int>(row.value.size()) <= about::kMaxPluginValueChars);
            CheckWords(row.note);
            CHECK(WrapLines(row.note, kNoteCols) <= kNoteLines);
        }
    }
}

// ---------------------------------------------------------------------------------------------

static void TestCreditsList() {
    CHECK(about::kCreditCount == static_cast<int>(sizeof(about::kCredits) / sizeof(about::kCredits[0])));
    CHECK(about::kCreditCount >= 12);
    std::set<std::string> names;
    for (int i = 0; i < about::kCreditCount; ++i) {
        const about::Credit& c = about::kCredits[i];
        CHECK(c.name && c.value && c.author && c.use && c.license_long && c.link);
        const std::string name = c.name, value = c.value, author = c.author, use = c.use, license = c.license_long, link = c.link;
        CHECK(!name.empty() && static_cast<int>(name.size()) <= about::kMaxNameChars);
        CHECK(!value.empty() && static_cast<int>(value.size()) <= about::kMaxValueChars);
        CHECK(!author.empty() && use.size() > 20 && !license.empty() && link.size() > 5);
        CHECK(names.insert(name).second); // no name twice
        for (const std::string& field : {name, value, author, use, license, link}) {
            CHECK(PlainAscii(field) && field.find('\n') == std::string::npos);
            CHECK(!field.empty() && field.front() != ' ' && field.back() != ' ');
            CHECK(!Contains(field, "  "));
        }
        // a link is an address, without the scheme (the page shows it as it is; CREDITS.md adds https://), with no spaces
        CHECK(link.find(' ') == std::string::npos && link.find("://") == std::string::npos && link.find('.') != std::string::npos);
        CHECK(use.back() == '.');
        // the note on the top screen fits its card
        const std::string note = about::CreditNote(c);
        CheckWords(note);
        CHECK(Contains(note, use) && Contains(note, author) && Contains(note, license) && Contains(note, link));
        const int lines = WrapLines(note, kNoteCols);
        CHECK(lines <= kNoteLines);
        if (lines > kNoteLines) {
            std::printf("  (%s: %d lines of %d)\n", c.name, lines, kNoteLines);
        }
    }
    // thanks (the first row of the credits page)
    CHECK(std::strcmp(about::kThanksLabel, "Thank you") == 0);
    CheckWords(about::kThanksNote);
    CHECK(Contains(about::kThanksNote, "Gionko") && Contains(about::kThanksNote, "CREDITS.md"));
    CHECK(WrapLines(about::kThanksNote, kNoteCols) <= kNoteLines);

    // Whom the user asked to be thanked, by name, and the rest of what the audit found (README, third_party, titleshare, the Makefiles, the tools, the guide).
    for (const char* wanted : {"Azahar Emulator", "FBI", "3dbrew", "devkitPro", "libctru", "citro2d, citro3d", "Luma3DS", "CTRPluginFramework", "ENet", "GodMode9", "makerom, ctrtool", "bannertool", "Pillow"}) {
        CHECK(names.count(wanted) == 1);
    }
    // the tools that build the app are named where they are (devkitPro's three, and tex3ds), GodMode9's script and the Citra project in the notes of the entries that own them
    std::string devkit, azahar, godmode, ctrpf;
    for (int i = 0; i < about::kCreditCount; ++i) {
        const std::string name = about::kCredits[i].name, note = about::CreditNote(about::kCredits[i]);
        if (name == "devkitPro") {
            devkit = note;
        } else if (name == "Azahar Emulator") {
            azahar = note;
        } else if (name == "GodMode9") {
            godmode = note;
        } else if (name == "CTRPluginFramework") {
            ctrpf = note;
        }
    }
    CHECK(Contains(devkit, "devkitARM") && Contains(devkit, "3dsxtool") && Contains(devkit, "smdhtool") && Contains(devkit, "tex3ds"));
    CHECK(Contains(azahar, "Citra Emulator Project"));
    CHECK(Contains(godmode, "GM9Megascript") && Contains(godmode, "annson24") && Contains(godmode, "d0k3"));
    CHECK(Contains(ctrpf, "PabloMK7") && Contains(ctrpf, "Nanquitas"));
}

// AzaharLink/CREDITS.md says what the page says: the same entries in the same order, and every word of each.
static void TestCreditsFile() {
    std::string md;
    if (!ReadFile(g_dir + "/CREDITS.md", &md)) {
        std::printf("FAIL: %s/CREDITS.md is not there (the file the GitHub repository carries; it is the list of about_text.hpp)\n", g_dir.c_str());
        ++g_failures;
        return;
    }
    CHECK(Contains(md, "Gionko"));
    CHECK(Contains(md, "GPL v2"));
    CHECK(Contains(md, "Nintendo"));
    CHECK(Contains(md, about::kClaim) && Contains(md, about::kAppName));
    // the headings of the list, in order: "### name"
    const std::size_t list_at = md.find("## The list");
    CHECK(list_at != std::string::npos);
    std::vector<std::string> headings;
    std::size_t pos = list_at == std::string::npos ? 0 : list_at;
    for (;;) {
        const std::size_t at = md.find("\n### ", pos);
        if (at == std::string::npos) {
            break;
        }
        const std::size_t end = md.find('\n', at + 1 + 4);
        headings.push_back(md.substr(at + 5, (end == std::string::npos ? md.size() : end) - (at + 5)));
        pos = at + 5;
    }
    CHECK(static_cast<int>(headings.size()) == about::kCreditCount);
    for (int i = 0; i < about::kCreditCount && i < static_cast<int>(headings.size()); ++i) {
        const about::Credit& c = about::kCredits[i];
        CHECK(headings[i] == c.name);
        // the section of this credit: from its heading to the next one (or the end of the list)
        const std::size_t from = md.find(std::string("\n### ") + c.name + "\n");
        CHECK(from != std::string::npos);
        const std::size_t next = md.find("\n## ", from == std::string::npos ? 0 : from);
        const std::size_t next_credit = md.find("\n### ", from == std::string::npos ? 0 : from + 5);
        std::size_t to = md.size();
        if (next != std::string::npos && next < to) {
            to = next;
        }
        if (next_credit != std::string::npos && next_credit < to) {
            to = next_credit;
        }
        const std::string section = from == std::string::npos ? std::string() : md.substr(from, to - from);
        CHECK(Contains(section, std::string("- What for: ") + c.use + "\n"));
        CHECK(Contains(section, std::string("- By: ") + c.author + "\n"));
        CHECK(Contains(section, std::string("- License: ") + c.value + " - " + c.license_long + "\n"));
        CHECK(Contains(section, std::string("- Link: https://") + c.link + "\n"));
    }
}

// ---------------------------------------------------------------------------------------------

static const about::Credit* Find(const char* name) {
    for (int i = 0; i < about::kCreditCount; ++i) {
        if (std::strcmp(about::kCredits[i].name, name) == 0) {
            return &about::kCredits[i];
        }
    }
    return nullptr;
}

// The credits say nothing the repository does not show: what each entry claims about the tree is looked up in it (a file that is not here, because the test is run from another
// folder of the repository, is told and skipped, as guide_test does).
static void TestCreditsAreReal() {
    std::string text;
    auto read = [&](const std::string& rel, bool* here) {
        *here = ReadFile(g_dir + "/" + rel, &text);
        if (!*here) {
            std::printf("SKIP the check of %s (not here: the test is run from AzaharLink/ of the whole repository)\n", rel.c_str());
        }
        return *here;
    };
    bool here = false;

    // ENet: Lee Salzman's MIT license is in the tree and the version the entry names is the one in the headers.
    if (read("third_party/enet/LICENSE", &here)) {
        CHECK(Contains(text, "Lee Salzman"));
        const about::Credit* enet_credit = Find("ENet");
        CHECK(enet_credit && Contains(text, enet_credit->author)); // (the MIT notice the license asks to carry is the author line of the entry, word for word)
        CHECK(Contains(text, "Permission is hereby granted, free of charge"));
    }
    if (read("Includes/enet/enet.h", &here)) {
        const std::string major = "#define ENET_VERSION_MAJOR 1", minor = "#define ENET_VERSION_MINOR 3", patch = "#define ENET_VERSION_PATCH 18";
        CHECK(Contains(text, major) && Contains(text, minor) && Contains(text, patch));
        const about::Credit* enet = Find("ENet");
        CHECK(enet && Contains(enet->use, "version 1.3.18"));
    }
    if (read("Sources/enet/enet_3ds.c", &here)) {
        CHECK(Contains(text, "Derived from ENet's unix.c"));
    }
    // Luma3DS's csvc.h: in the plugin's sources, with the zlib-style terms the entry names; the template's own files, byte for byte, are the ones the CTRPluginFramework entry names.
    if (read("Includes/csvc.h", &here)) {
        CHECK(Contains(text, "provided 'as-is'"));
        CHECK(Contains(text, "svcControlService"));
    }
    for (const char* file : {"3gx.ld", "Includes/types.h", "Includes/Unicode.h"}) {
        read(file, &here);
        CHECK(here);
    }
    // Azahar: the UDS port says whose it is, and under which license.
    if (read("Includes/azl/common.hpp", &here)) {
        CHECK(Contains(text, "GPLv2 or any later version") && Contains(text, "Azahar"));
    }
    // FBI: the installing code follows its sequence; GodMode9: the CIA code builds the chain as it does.
    if (read("ConfigApp/titleshare/source/amsink.c", &here)) {
        CHECK(Contains(text, "FBI"));
    }
    if (read("ConfigApp/titleshare/common/cia_front.c", &here)) {
        CHECK(Contains(text, "GodMode9"));
    }
    // bannertool: its source and its license are in the repository (it makes the banner of the CIA on the build machine); the stb libraries are in it.
    if (read("../DlpLink/tools/bannertool/LICENSE.txt", &here)) {
        CHECK(Contains(text, "Steveice10") && Contains(text, "Permission is hereby granted"));
    }
    if (read("../DlpLink/tools/bannertool/ORIGIN.md", &here)) {
        CHECK(Contains(text, "Mtgxyz") && Contains(text, "stb_image") && Contains(text, "stb_vorbis"));
    }
    // Pillow: the scripts that draw the icon and the guide use it.
    if (read("ConfigApp/tools/make_icon.py", &here)) {
        CHECK(Contains(text, "from PIL import"));
    }
    if (read("../DlpLink/tools/make_guide.py", &here)) {
        CHECK(Contains(text, "from PIL import"));
    }
    // makerom: the version the entry names is the one of the binaries kept in the repository.
    if (read("../tools_ctr/LEGGIMI.txt", &here)) {
        CHECK(Contains(text, "makerom v0.19.0"));
        const about::Credit* tools = Find("makerom, ctrtool");
        CHECK(tools && Contains(tools->use, "makerom 0.19.0"));
    }
    if (read("../DlpLink/tools/make_3dslink_cia.sh", &here)) {
        CHECK(Contains(text, "\"$MAKEROM\" -f cia")); // the CIA of the app is made by makerom
    }
    // the guide's GodMode9 version, and its script
    if (read("../DlpLink/docs/GUIDA-BOOT9.md", &here)) {
        const about::Credit* gm9 = Find("GodMode9");
        CHECK(gm9 && Contains(gm9->use, "v2.1.1") && Contains(text, "v2.1.1"));
    }
    if (read("ConfigApp/guide/guide_logic.hpp", &here)) {
        CHECK(Contains(text, "annson24"));
    }
    // the plugin that is built with CTRPluginFramework: its framework header is the one the sources include
    if (read("Sources/main.cpp", &here)) {
        CHECK(Contains(text, "#include <CTRPluginFramework.hpp>"));
    }
}

// ---------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    struct stat st;
    if (stat((g_dir + "/CREDITS.md").c_str(), &st) != 0 && stat((g_dir + "/VERSION").c_str(), &st) != 0) {
        std::printf("about_test: %s/CREDITS.md and %s/VERSION are not here: run it from AzaharLink/ (or give that folder)\n", g_dir.c_str(), g_dir.c_str());
        return 2;
    }
    TestNames();
    TestOrigin();
    TestPluginRow();
    TestCreditsList();
    TestCreditsFile();
    TestCreditsAreReal();
    std::printf("about_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
