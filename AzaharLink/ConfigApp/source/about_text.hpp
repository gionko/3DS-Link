// "About 3DS Link" and its credits: the words of the two pages and the few decisions behind them, in plain C++ so that Tests/settings_test/about_test.cpp builds
// this very file on a PC. The pages that draw them are pages_config.cpp (About) and pages_credits.cpp (the credits); the same list of credits is
// AzaharLink/CREDITS.md (the file the GitHub repository carries), and the test reads that file and checks that every name, license and link of the list is in it.
//
// What things are called (the user's words, 2026-10-22): the NAME of the app is "3DS Link"; "Connect, share, play together" is its CLAIM (the line under the name in the
// HOME menu); the VERSION is one number (setup::kVersion, the file AzaharLink/VERSION). The author is Gionko ("Created by Gionko").
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "plugin_install.hpp" // (plain C++: setup::PluginCard, what the card holds against the plugin inside the app)

namespace about {

constexpr const char* kAppName = "3DS Link";
constexpr const char* kClaim = "Connect, share, play together";
constexpr const char* kAuthor = "Gionko";
constexpr const char* kCreatedBy = "Created by Gionko";

// ---------------------------------------------------------------------------------------------
// The rows of the About page
// ---------------------------------------------------------------------------------------------

// The value of the first row ("3DS Link" ... "v1.0.0").
inline std::string VersionLabel(const char* version) {
    return std::string("v") + version;
}

// The note of the first row: the claim comes first (it is what the app says it is for), then the one version.
inline std::string AppNote(const char* version) {
    return std::string(kClaim) + "\n\nVersion " + version +
           ". This app and the AzaharLink plugin share one version number and one settings file, AzaharLink.ini. Changes are saved as you make them.";
}

// The note of "Created by Gionko": who made it, and where the thanks are.
inline std::string CreatedByNote() {
    return std::string("3DS Link was created by ") + kAuthor +
           ". It stands on the work of many others, which it uses or has copied from: the Credits row thanks each one by name, with the license it is under.\n\n"
           "3DS Link is free software (GPL v2 or later, like the Azahar code it ports).";
}

// How this run of the app was started. The row "Installed as" says what is true, never "unknown":
//   Installed  the HOME menu started it: the CIA is installed (title 000400000F83D100) and the launcher gives no path;
//   File       the Homebrew Launcher started it from a .3dsx, and gave the path;
//   NoPath     no path and not the installed title (an emulator running a .3dsx, a launcher that says nothing).
enum class Origin { Installed, File, NoPath };

// `app_path`: argv[0] as the launcher gave it ("" = none). `homebrew`: the Homebrew Launcher's environment is there (libctru's envIsHomebrew). `title_known` and
// `title_id`: the program id the system says this process runs as (APT_GetProgramID), when it said. `app_tid`: the title id of the installed 3DS Link.
// Without a path: the system's answer decides when there is one (it is this app's own title, or it is not); without an answer, a launcher that is there says
// "no path" and anything else is the HOME menu (an installed app is started without a path).
inline Origin OriginOf(const std::string& app_path, bool homebrew, bool title_known, std::uint64_t title_id, std::uint64_t app_tid) {
    if (!app_path.empty()) {
        return Origin::File;
    }
    if (title_known) {
        return title_id == app_tid ? Origin::Installed : Origin::NoPath;
    }
    return homebrew ? Origin::NoPath : Origin::Installed;
}

// "000400000F83D100".
inline std::string TitleIdText(std::uint64_t tid) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(tid));
    return buf;
}

// The last part of a path (the file name of a .3dsx: the folders before it are dropped).
inline std::string FileNameOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct InstalledAs {
    std::string value; // what the row shows at its right
    std::string note;  // what the top screen says
    bool dim = false;  // the value is faint: the app could not tell more
};

// `appdir`: the folder of the app (paths.appdir), `datadir`: the folder of its own files (paths.datadir): the same one for an installed app.
inline InstalledAs DescribeOrigin(Origin origin, const std::string& app_path, const std::string& appdir, const std::string& datadir, std::uint64_t app_tid) {
    InstalledAs out;
    const std::string files = "Its own files (its settings, your rooms, its caches, its logs) are in " + datadir + ".";
    switch (origin) {
    case Origin::Installed:
        out.value = "installed app";
        out.note = "The installed app: 3DS Link was started from the HOME menu (title " + TitleIdText(app_tid) + ").\n" + files +
                   " It puts AzaharLink.3gx there by itself; boot9.bin is looked for there too.";
        break;
    case Origin::File:
        out.value = FileNameOf(app_path);
        out.note = "Started from " + app_path + " (the Homebrew Launcher).\n" + files + " It puts AzaharLink.3gx in its own folder by itself (one already next to the app is used where it is); boot9.bin goes next to the app (" +
                   appdir + ")" + (appdir == datadir ? "." : " or in that folder.");
        break;
    case Origin::NoPath:
        out.value = "no path given";
        out.dim = true;
        out.note = "The launcher did not say where this app was started from (an emulator, or a launcher that passes no path), so the default folder is used.\n" + files;
        break;
    }
    return out;
}

// The plugin file next to the app, against the plugin the app carries (setup::PluginCard says what the card holds, byte for byte).
enum class Tone { Good, Warn, Bad };

struct PluginRow {
    std::string value; // at most kMaxPluginValueChars: what the row shows at its right
    std::string note;
    Tone tone = Tone::Good;
};

constexpr int kMaxPluginValueChars = 20;

// `inside_usable`: the app carries a plugin it can put on the card (setup::EmbeddedPluginUsable). `found_version`: what the file's header says ("" = none). `size_text`: the file's
// size ("1.02 MB"). The words say what the app does about it, which is what plugin_install.hpp does: it puts its own plugin there at every start.
inline PluginRow DescribePluginRow(setup::PluginCard card, bool inside_usable, const std::string& path, const std::string& found_version, const std::string& size_text,
                                   const std::string& app_version) {
    PluginRow out;
    const std::string fix = " The app puts its own plugin there by itself at every start; Settings > Installation check does it now.";
    if (!inside_usable) {
        out.value = "none inside";
        out.tone = Tone::Bad;
        out.note = path + "\nThis copy of 3DS Link has no plugin inside, so the plugin cannot be put on the card: install the 3DS Link CIA again.";
        return out;
    }
    switch (card) {
    case setup::PluginCard::Same:
        out.value = app_version + ", " + size_text;
        out.tone = Tone::Good;
        out.note = path + "\nThe plugin that is inside the app, byte for byte: version " + app_version + ", " + size_text + ".";
        break;
    case setup::PluginCard::Missing:
        out.value = "missing";
        out.tone = Tone::Bad; // (a problem in the Installation check too, and it shuts the 3DS Link switch)
        out.note = path + "\nThere is no plugin there." + fix + " Luma3DS itself loads plugins from sd:/luma/plugins.";
        break;
    case setup::PluginCard::Unreadable:
        out.value = "unreadable";
        out.tone = Tone::Bad;
        out.note = path + "\nThe file is there and could not be read." + fix;
        break;
    case setup::PluginCard::NotPlugin:
        out.value = "not a plugin";
        out.tone = Tone::Bad;
        out.note = path + "\nThe file does not start like a .3gx plugin (" + size_text + ")." + fix;
        break;
    case setup::PluginCard::OtherVersion:
        out.value = found_version.empty() ? std::string("other version") : found_version + ", not " + app_version;
        out.tone = Tone::Bad; // (a problem in the Installation check too, and it shuts the 3DS Link switch)
        out.note = path + "\nVersion " + (found_version.empty() ? std::string("unknown") : found_version) + " in the file; this app is " + app_version + ": the two do not match." + fix;
        break;
    case setup::PluginCard::Altered:
        out.value = "damaged";
        out.tone = Tone::Bad; // (a problem in the Installation check too, and it shuts the 3DS Link switch)
        out.note = path + "\nA plugin of version " + app_version + " whose bytes are not the ones inside the app: cut short, damaged or another build." + fix;
        break;
    }
    return out;
}

// The rows that lead on: the credits.
constexpr const char* kCreditsLabel = "Credits and thanks";
constexpr const char* kCreditsNote =
    "Everything 3DS Link uses or has copied, thanked by name with its author and its license: Azahar, FBI, 3dbrew, devkitPro, Luma3DS and the others. A opens the list.";

// ---------------------------------------------------------------------------------------------
// The credits
// ---------------------------------------------------------------------------------------------

// One credit. `name` and `value` are the row (the name at the left, the short license at the right); the note under the list on the top screen is the rest.
// The same entry is a line of AzaharLink/CREDITS.md. Plain ASCII: the console's font and the quotes of the source are never in doubt.
struct Credit {
    const char* name;         // at most kMaxNameChars
    const char* value;        // the license in a word, at most kMaxValueChars ("MIT", "GPL-3.0", "zlib", "wiki")
    const char* author;       // who
    const char* use;          // what 3DS Link took from it, and where
    const char* license_long; // the license, a little longer
    const char* link;         // where it lives, without "https://"
};

// The room of a row: the label is drawn at 0.55 and the value at 0.48 in 286 pixels (the drawing shortens what does not fit; these keep it from having to).
constexpr int kMaxNameChars = 22;
constexpr int kMaxValueChars = 12;

inline constexpr Credit kCredits[] = {
    {"Azahar Emulator", "GPL-2.0+", "the Azahar and Citra Emulator Projects",
     "The room 3DS Link connects to, and the model for everything it does with one. The plugin's UDS logic (nwm::UDS, frames, beacons, ndm) is a port of Azahar's "
     "nwm_uds, uds and ndm code. Its Download Play code was read to build 3DS Link Delivery.",
     "GPL v2 or later (the files ported from)", "github.com/azahar-emu/azahar"},
    {"FBI", "MIT", "Steveice10",
     "The reference for installing a title: the way Titles Delivery and the installer talk to the AM service (begin, write, finish, seed, clean up what a failed "
     "install leaves), the layout of a CIA, how a title's name and icon are read. 3DS Link follows FBI's sequence.",
     "MIT license", "github.com/Steveice10/FBI"},
    {"3dbrew", "wiki", "the 3dbrew community",
     "The documentation of the 3DS this was built from: the services (nwm::UDS, AM, FS, soc), the formats (CIA, TMD, NCCH, SMDH, 3GX) and the Download Play "
     "protocol. Its tables and notes are cited in the comments of the code.",
     "none stated on the site: used as documentation", "3dbrew.org"},
    {"devkitPro", "GPL", "Dave Murphy, fincs and the devkitPro team",
     "The toolchain that builds the app, the plugin and the installer: devkitARM (compiler, linker), 3dsxtool and smdhtool (the .3dsx and the HOME menu entry), "
     "tex3ds by mtheall (the guide's pictures). Build tools only: none of their code is in 3DS Link.",
     "GPL (3dstools and tex3ds: GPL v3)", "devkitpro.org"},
    {"libctru", "zlib", "smealum, fincs and the libctru contributors",
     "The 3DS system library under everything: services, sockets, UDS, AM, graphics, input.",
     "zlib license", "github.com/devkitPro/libctru"},
    {"citro2d, citro3d", "zlib", "fincs and the devkitPro contributors",
     "All the drawing of the app and of the installer: the lists, the notes, the guide's pictures.",
     "zlib license", "github.com/devkitPro/citro2d"},
    {"Luma3DS", "GPL-3.0", "the LumaTeam (AuroraWright, TuxSH and the contributors)",
     "What loads AzaharLink.3gx into a game: its Plugin Loader (the plg:ldr service the app asks about and switches on) and svcControlService, which the plugin "
     "uses. csvc.h is adapted from Luma3DS's own.",
     "GPL v3 (csvc.h: zlib-style terms in the file)", "github.com/LumaTeam/Luma3DS"},
    {"CTRPluginFramework", "custom", "Nanquitas, The Pixellizer Group; PabloMK7's 0.8.0 build",
     "The framework AzaharLink.3gx is built on (menu, process events, patches, files) and the template the project started from: 3gx.ld, csvc.h, types.h "
     "and Unicode.h are its.",
     "its own: free to use, the source going with its binary", "github.com/PabloMK7/CTRPluginFramework-BlankTemplate"},
    {"ENet", "MIT", "Copyright (c) 2002-2024 Lee Salzman",
     "The reliable UDP library the room speaks (version 1.3.18). It is in the plugin and in the app; enet_3ds.c, the layer on the 3DS sockets, is derived from "
     "ENet's own unix.c.",
     "MIT license", "github.com/lsalzman/enet"},
    {"GodMode9", "GPL-3.0", "d0k3 and the GodMode9 contributors",
     "The guide \"How to get boot9.bin\" shows GodMode9 v2.1.1: its pictures are cropped screenshots with arrows. The CIA code builds the certificate chain the way "
     "GodMode9 does. GM9Megascript, the script the guide has you run, is by annson24.",
     "GPL v3", "github.com/d0k3/GodMode9"},
    {"makerom, ctrtool", "MIT", "3DSGuy, Jakcron and the Project_CTR contributors",
     "Build and check tools (makerom 0.19.0): makerom builds the CIA of the app and the reference ones the tests compare with, ctrtool checks a CIA from outside. "
     "None of their code is in 3DS Link.",
     "makerom: MIT; ctrtool: its folder of the repository states no license", "github.com/3DSGuy/Project_CTR"},
    {"bannertool", "MIT", "Steveice10; stb libraries by Sean Barrett",
     "Makes the banner of the HOME menu (the small 3D model and its tune) from banner.png and jingle.wav. It is fetched by DlpLink/tools/bannertool/build.sh and runs on "
     "the build machine only: none of it is in the app.",
     "MIT license (stb_image, stb_vorbis: public domain or MIT)", "github.com/Mtgxyz/bannertool"},
    {"Pillow", "HPND", "the Pillow contributors",
     "Draws the icon and the guide's pictures: a build script, not part of the app.",
     "HPND (the Pillow license)", "github.com/python-pillow/Pillow"},
};
constexpr int kCreditCount = static_cast<int>(sizeof(kCredits) / sizeof(kCredits[0]));

// The first row of the credits page: the thanks in one line and how to read the list.
constexpr const char* kThanksLabel = "Thank you";
constexpr const char* kThanksNote =
    "3DS Link was created by Gionko and stands on the work of everyone in this list: what it uses, what it has copied, what it learned from. Move up and down; "
    "each note says what was taken, from whom, and under which license.\n\n"
    "The same list is CREDITS.md in the project.";

// The note of a credit.
inline std::string CreditNote(const Credit& credit) {
    return std::string(credit.use) + "\n\nBy " + credit.author + ".\nLicense: " + credit.license_long + ".\n" + credit.link;
}

} // namespace about
