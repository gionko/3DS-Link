// The plugin of 3DS Link: ONE package. AzaharLink.3gx is a resource of the app (data/AzaharLink.3gx, linked in by the Makefile; plugin_resource.hpp), and the app puts it on
// the SD card by itself: sdmc:/3ds/3DSLink/AzaharLink.3gx with a CIA install, the master file every title's copy (sdmc:/luma/plugins/<title id>/) is made from. At every start
// the file on the card is compared, BYTE FOR BYTE, with the plugin inside the app (a header that looks good says nothing: a file cut short, damaged or of another build has
// one too); a file that is missing or is not the same is written again, through a .part file and a rename, and read back. The app and its plugin are one version, so a
// different file is replaced, never kept.
//
// This file holds the decisions and the words (plain C++, no libctru: Tests/settings_test/plugin_install_test.cpp builds it on a PC) and, in plugin_install.cpp, the file
// work on bytes that are handed in (so the same code runs on the console, where the bytes are the resource, and in the test, where they are made up).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace setup {

// ---------------------------------------------------------------------------------------------
// What the card holds, against the plugin inside the app
// ---------------------------------------------------------------------------------------------

enum class PluginCard {
    Missing,      // there is no file (or there is a folder of that name: StatFile does not count a folder as a file)
    Unreadable,   // the file is there and could not be read
    NotPlugin,    // it does not start like a .3gx plugin (or it is shorter than the header)
    OtherVersion, // a plugin, of another version than the one inside the app (an older 3DS Link left it)
    Altered,      // a plugin of the version of the app whose bytes are not the ones inside it: cut short, damaged, or another build of the same version
    Same          // the plugin inside the app, byte for byte
};

// The facts InspectPluginFile measures.
struct PluginProbe {
    bool exists = false;
    bool readable = false;
    std::uint64_t size = 0;
    bool header_ok = false;            // starts with the 3GX magic and holds the version word
    std::uint32_t version_word = 0;    // bytes 8..11 of the header: major << 24 | minor << 16 | revision << 8
    bool same_bytes = false;           // size and every byte equal to the plugin inside the app
};

inline PluginCard ClassifyPluginCard(const PluginProbe& p, std::uint32_t wanted_version_word) {
    if (!p.exists) {
        return PluginCard::Missing;
    }
    if (!p.readable) {
        return PluginCard::Unreadable;
    }
    if (p.same_bytes) {
        return PluginCard::Same; // (the bytes decide, not the header: this is the only way to Same)
    }
    if (!p.header_ok) {
        return PluginCard::NotPlugin;
    }
    return p.version_word != wanted_version_word ? PluginCard::OtherVersion : PluginCard::Altered;
}

// ---------------------------------------------------------------------------------------------
// What to do about it, and what came of it
// ---------------------------------------------------------------------------------------------

enum class PluginStep {
    Leave,   // the file is the plugin of the app (or this build has no plugin to put: nothing can be done)
    Put,     // there is none: write it
    Replace  // there is a file that is not the plugin of the app: write it over (the old one stays until the new one is whole and read back)
};

inline PluginStep DecideInstall(PluginCard card, bool embedded_usable) {
    if (!embedded_usable || card == PluginCard::Same) {
        return PluginStep::Leave;
    }
    return card == PluginCard::Missing ? PluginStep::Put : PluginStep::Replace;
}

// Why the file could not be written (Ok: it could).
enum class WriteResult {
    Ok,
    NoFolder,       // the folder could not be made
    FolderInTheWay, // a folder is where the file goes
    CannotWrite,    // the card refused the bytes (full, locked, or it opened the file and would not take it)
    NotVerified,    // what was written did not read back the same
    CannotPlace     // the new file could not take the place of the old one
};

// A phrase that ends a sentence ("... could not be put there: the card refused the file (is it full or locked?)."): no capital, no full stop.
inline const char* WriteFailurePhrase(WriteResult r) {
    switch (r) {
    case WriteResult::NoFolder:
        return "its folder could not be made";
    case WriteResult::FolderInTheWay:
        return "a folder with that name is in the way";
    case WriteResult::CannotWrite:
        return "the SD card refused the file (is it full or locked?)";
    case WriteResult::NotVerified:
        return "what was written did not read back the same (the card may be failing)";
    case WriteResult::CannotPlace:
        return "the old file could not be replaced";
    default:
        return "";
    }
}

// One look-and-fix of the file on the card.
struct PluginEnsure {
    PluginCard before = PluginCard::Missing; // what the card held
    PluginStep step = PluginStep::Leave;     // what was decided
    WriteResult write = WriteResult::Ok;     // how the write went (Ok when nothing was written)
    PluginCard after = PluginCard::Missing;  // what the card holds now (read again after a write)
    bool header_ok = false;                  // the file as it is now: it starts like a .3gx ...
    std::uint32_t version_word = 0;          // ... and says this version (bytes 8..11 of its header) ...
    std::uint64_t size = 0;                  // ... and is this long (0 when there is no file)

    bool wrote() const {
        return step != PluginStep::Leave && write == WriteResult::Ok;
    }
    bool failed() const {
        return step != PluginStep::Leave && write != WriteResult::Ok;
    }
};

// The words for what an ensure did, as a toast (the app tells it once): "" when nothing was done. `folder` is the folder the file is in (the data folder of the app).
inline std::string PluginInstallToast(const PluginEnsure& e, const std::string& folder) {
    if (e.failed()) {
        return "Could not put the plugin in " + folder + ": " + WriteFailurePhrase(e.write) + ".";
    }
    if (!e.wrote()) {
        return std::string();
    }
    if (e.step == PluginStep::Put) {
        return "The plugin was put in " + folder + ".";
    }
    return e.before == PluginCard::OtherVersion ? "The plugin in " + folder + " was updated to this version."
                                                : "The plugin in " + folder + " was damaged: it was replaced.";
}

// The few words of the first start's step "Plugin file" (the list of what the check found, one short line each): `put_now` says that this start put the file there.
inline std::string PluginStepDetail(bool embedded_usable, PluginCard card, bool put_now, const std::string& found_version) {
    if (!embedded_usable) {
        return "none in this app";
    }
    switch (card) {
    case PluginCard::Same:
        return put_now ? std::string("put there by 3DS Link") : "v" + found_version;
    case PluginCard::Missing:
        return "could not be put there";
    case PluginCard::OtherVersion:
        return "v" + found_version + ", not this app's";
    default:
        return "damaged";
    }
}

// ---------------------------------------------------------------------------------------------
// The 3DS Link switch of the main page
// ---------------------------------------------------------------------------------------------

// A Luma3DS that is too old shuts the whole Azahar side; a plugin that is not the plugin of the app (missing, damaged, another version: AFTER the install step had its
// chance) shuts the switch alone. Luma comes first: its reason is the one nothing in the app can mend.
enum class SwitchLock { None, Luma, Plugin };

inline SwitchLock LockOfSwitch(bool luma_too_old, bool plugin_ready) {
    return luma_too_old ? SwitchLock::Luma : (plugin_ready ? SwitchLock::None : SwitchLock::Plugin);
}

// What the switch shows: ON only when it is switched on in the settings file and nothing shuts it.
inline bool SwitchIsOn(bool enabled, bool tunnel, SwitchLock lock) {
    return enabled && tunnel && lock == SwitchLock::None;
}

// Why the switch is shut because of the plugin, in one sentence: it is the toast of A and of the D-pad on the shut row, and the start of the row's note.
// `found_version` is the version the file says ("" = none could be read); `tried` says that the app tried to put the plugin and `why` how that went.
inline std::string PluginLockText(bool embedded_usable, PluginCard card, const std::string& folder, const std::string& found_version, const std::string& app_version,
                                  bool tried, WriteResult why) {
    if (!embedded_usable) {
        return "This copy of 3DS Link has no plugin inside, so 3DS Link cannot be switched on. Install the 3DS Link CIA again.";
    }
    if (card == PluginCard::Same) {
        return std::string();
    }
    std::string s;
    switch (card) {
    case PluginCard::Missing:
        s = "AzaharLink.3gx is missing in " + folder;
        break;
    case PluginCard::OtherVersion:
        s = "AzaharLink.3gx in " + folder + " is " + (found_version.empty() ? std::string("of another version") : "version " + found_version) + " (this app is " + app_version + ")";
        break;
    default:
        s = "AzaharLink.3gx in " + folder + " is damaged";
        break;
    }
    if (tried && why != WriteResult::Ok) {
        s += std::string(card == PluginCard::Missing ? " and could not be put there: " : " and could not be replaced: ") + WriteFailurePhrase(why);
    }
    return s + ".";
}

// What the note of the shut row adds after that sentence (only when the app has a plugin to put: a build with none inside says to install the CIA again, and nothing "puts it back").
constexpr const char* kPluginLockMore =
    "3DS Link carries the plugin inside and puts it back by itself at every start; Settings > Installation check does it now. Until then the switch stays OFF.";

// How long a toast stays up. A sentence too long for its place on the main page runs by (gfx::MarqueeText: a rest, 150 ms a letter, a rest) and the sentences about a plugin that
// could not be put are 100 to 150 letters: the time it was given is raised to what the run needs (two seconds and 150 ms a letter) for a text of more than 40 letters; a short one keeps
// its time. (F, the review of 1.0.1: a 7 s toast showed 82 letters and the reason was cut off.)
inline std::uint64_t ToastMsToRead(std::size_t letters, std::uint64_t given_ms) {
    const std::uint64_t run = 2000 + 150 * static_cast<std::uint64_t>(letters);
    return letters > 40 && run > given_ms ? run : given_ms;
}

// ---------------------------------------------------------------------------------------------
// The file work (plugin_install.cpp: POSIX calls on the paths and the bytes handed in)
// ---------------------------------------------------------------------------------------------

// Measures the file at `path` against `data` (the plugin inside the app): exists, readable, size, header and, when the size is the same, every byte.
PluginProbe InspectPluginFile(const std::string& path, const std::uint8_t* data, std::size_t size);

// Writes `data` as the file `dest`: its folder is made, the bytes go to "<dest>.part", the .part file is read back against `data`, the file that is there is put aside as
// "<dest>.old" (the name the installer of 3DS Link uses), the .part file takes its place and only then the old one goes; a failure puts the old one back. So the file on the card is
// whole, or it is the old one. A swap that a power cut left half done (no `dest`, a ".old" file) is put back first.
WriteResult WritePluginFile(const std::string& dest, const std::uint8_t* data, std::size_t size);

// Looks at the file at `dest` and, when it is not the plugin `data` (and `usable` says that there is a plugin to put), writes it; then looks again. `wanted_version_word`
// is the version word of the plugin inside the app.
PluginEnsure EnsurePluginFile(const std::string& dest, const std::uint8_t* data, std::size_t size, bool usable, std::uint32_t wanted_version_word);

} // namespace setup
