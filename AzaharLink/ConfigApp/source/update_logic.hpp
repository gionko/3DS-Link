// The update of 3DS Link from GitHub: the decisions and the words (plain C++, no libctru and no curl: Tests/settings_test/update_test.cpp builds it on a PC).
//
// The question is one request: GitHub answers .../releases/latest with a redirect to .../releases/tag/vX.Y.Z, and the tag is the version that is out. No API (its limit is
// 60 requests an hour per address) and no JSON. The file comes over plain HTTPS: libcurl checks GitHub's certificate against the public list of roots the app carries
// (certs/cacert.pem, Mozilla's, the one browsers use), dates included, so a console whose clock is wrong cannot check: it is told so (ClockTooOld, NetResult::Clock), once.
//
// update_http.cpp is the HTTPS (libcurl), update_worker.cpp the thread and the install, pages_update.cpp the pages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "about_text.hpp" // (about::Origin: how this run was started)

namespace update {

constexpr const char* kLatestUrl = "https://github.com/gionko/3DS-Link/releases/latest";
constexpr const char* kTagPrefix = "https://github.com/gionko/3DS-Link/releases/tag/v";
constexpr const char* kDownloadPrefix = "https://github.com/gionko/3DS-Link/releases/download/v";
constexpr const char* kChangelogPrefix = "https://raw.githubusercontent.com/gionko/3DS-Link/v";

// More than either file will ever be: an answer of this size is not ours.
constexpr std::size_t kMaxFileBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxChangelogBytes = 512u * 1024u;

// ---------------------------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------------------------

struct Version {
    int major = 0, minor = 0, micro = 0;
};

// "X.Y.Z" and nothing else: 1 to 3 digits each. A tag like "1.0.14-test" is not a version that is offered.
inline bool ParseVersion(const std::string& text, Version& out) {
    int parts[3] = {0, 0, 0};
    int part = 0, digits = 0;
    for (const char c : text) {
        if (c == '.') {
            if (digits == 0 || ++part > 2) {
                return false;
            }
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 3) {
                return false;
            }
            parts[part] = parts[part] * 10 + (c - '0');
        } else {
            return false;
        }
    }
    if (part != 2 || digits == 0) {
        return false;
    }
    out.major = parts[0];
    out.minor = parts[1];
    out.micro = parts[2];
    return true;
}

inline int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.micro != b.micro) {
        return a.micro < b.micro ? -1 : 1;
    }
    return 0;
}

// True only when both can be read and `latest` is the later one.
inline bool IsNewer(const std::string& latest, const std::string& current) {
    Version l, c;
    return ParseVersion(latest, l) && ParseVersion(current, c) && CompareVersions(l, c) > 0;
}

// The version in the redirect of kLatestUrl ("" when the answer is anything else).
inline std::string VersionFromLocation(const std::string& location) {
    const std::string prefix = kTagPrefix;
    if (location.compare(0, prefix.size(), prefix) != 0) {
        return std::string();
    }
    const std::string version = location.substr(prefix.size());
    Version parsed;
    return ParseVersion(version, parsed) ? version : std::string();
}

// The title version a version has in the CIA's TMD (DlpLink/tools/version.py).
inline std::uint16_t TmdVersion(const Version& v) {
    return static_cast<std::uint16_t>((v.major << 10) | (v.minor << 4) | v.micro);
}

// ---------------------------------------------------------------------------------------------
// What is updated: how this run was started says which file
// ---------------------------------------------------------------------------------------------

enum class Target {
    Cia,      // the installed title: 3DSLink.cia through AM
    File3dsx, // the file the Homebrew Launcher started: 3DSLink.3dsx in its place
    None      // no file on the SD card is known (a network load, a launcher that gives no path): nothing can be replaced
};

inline Target ChooseTarget(about::Origin origin, const std::string& app_path) {
    if (origin == about::Origin::Installed) {
        return Target::Cia;
    }
    const std::string device = "sdmc:/", ending = ".3dsx";
    const bool on_card = app_path.compare(0, device.size(), device) == 0;
    const bool is_3dsx = app_path.size() > ending.size() && app_path.compare(app_path.size() - ending.size(), ending.size(), ending) == 0;
    return origin == about::Origin::File && on_card && is_3dsx ? Target::File3dsx : Target::None;
}

inline std::string AssetUrl(const std::string& version, Target target) {
    return kDownloadPrefix + version + (target == Target::Cia ? "/3DSLink.cia" : "/3DSLink.3dsx");
}

inline std::string ChangelogUrl(const std::string& version) {
    return kChangelogPrefix + version + "/CHANGELOG.md";
}

// ---------------------------------------------------------------------------------------------
// The file that came: is it what was asked for?
// ---------------------------------------------------------------------------------------------

struct CiaIdentity {
    std::uint64_t title_id = 0;
    std::uint16_t version = 0;
};

// The title id and the version in the CIA's TMD. False when the bytes are not a CIA with a whole TMD header.
inline bool ReadCiaIdentity(const std::uint8_t* data, std::size_t size, CiaIdentity& out) {
    const auto le32 = [&](std::size_t at) {
        return static_cast<std::uint64_t>(data[at]) | static_cast<std::uint64_t>(data[at + 1]) << 8 | static_cast<std::uint64_t>(data[at + 2]) << 16 |
               static_cast<std::uint64_t>(data[at + 3]) << 24;
    };
    const auto be = [&](std::size_t at, int bytes) {
        std::uint64_t value = 0;
        for (int i = 0; i < bytes; ++i) {
            value = value << 8 | data[at + static_cast<std::size_t>(i)];
        }
        return value;
    };
    const auto align64 = [](std::uint64_t value) { return (value + 63) / 64 * 64; };
    if (!data || size < 0x20 || le32(0) != 0x2020) {
        return false;
    }
    const std::uint64_t certs_at = align64(le32(0));
    const std::uint64_t ticket_at = align64(certs_at + le32(8));
    const std::uint64_t tmd_at = align64(ticket_at + le32(0xC));
    const std::uint64_t tmd_size = le32(0x10);
    if (tmd_at + 4 > size) {
        return false;
    }
    std::uint64_t header = 0; // the signature's size, by its type, as 3dbrew's "Title metadata" lists them
    switch (be(static_cast<std::size_t>(tmd_at), 4)) {
    case 0x00010003:
        header = 0x240;
        break;
    case 0x00010004:
        header = 0x140;
        break;
    case 0x00010005:
        header = 0x80;
        break;
    default:
        return false;
    }
    if (header + 0xC4 > tmd_size || tmd_at + header + 0xC4 > size) {
        return false;
    }
    out.title_id = be(static_cast<std::size_t>(tmd_at + header + 0x4C), 8);
    out.version = static_cast<std::uint16_t>(be(static_cast<std::size_t>(tmd_at + header + 0x9C), 2));
    return true;
}

inline bool Looks3dsx(const std::uint8_t* data, std::size_t size) {
    return data && size > 4 && data[0] == '3' && data[1] == 'D' && data[2] == 'S' && data[3] == 'X';
}

// ---------------------------------------------------------------------------------------------
// What's new: the sentence that opens the version's section in CHANGELOG.md
// ---------------------------------------------------------------------------------------------

// The section is "## X.Y.Z - date" and its first line of text is the summary in bold: that line, without the marks of the format ("" when there is none). The tables below it
// are for a browser, not for this screen.
inline std::string WhatsNew(const std::string& changelog, const std::string& version) {
    constexpr std::size_t kMaxLetters = 280;
    const std::string heading = "## " + version;
    std::size_t pos = 0;
    bool in_section = false;
    while (pos < changelog.size()) {
        std::size_t end = changelog.find('\n', pos);
        if (end == std::string::npos) {
            end = changelog.size();
        }
        std::string line = changelog.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!in_section) {
            const bool same = line.compare(0, heading.size(), heading) == 0 && (line.size() == heading.size() || line[heading.size()] == ' ');
            in_section = same;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#' || line[0] == '|' || line[0] == '>') {
            return std::string(); // (this section does not open with a sentence)
        }
        std::string clean;
        for (const char c : line) {
            if (c != '*' && c != '`') {
                clean += c;
            }
        }
        if (clean.size() > kMaxLetters) {
            std::size_t cut = clean.rfind(' ', kMaxLetters);
            clean = clean.substr(0, cut == std::string::npos ? kMaxLetters : cut) + "...";
        }
        return clean;
    }
    return std::string();
}

// ---------------------------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------------------------

// GitHub's certificates cannot be checked with a clock that is years off, and no certificate of today is valid before this year. This code did not exist before it either.
constexpr int kOldestBelievableYear = 2026;

inline bool ClockTooOld(int year) {
    return year < kOldestBelievableYear;
}

// The popup about the clock is shown once: it comes back only after a check has gone through (so the clock was right) and the clock went wrong again.
inline bool ClockPopupDue(bool clock_wrong, bool already_warned) {
    return clock_wrong && !already_warned;
}

// ---------------------------------------------------------------------------------------------
// How a request went
// ---------------------------------------------------------------------------------------------

enum class NetResult {
    Ok,
    Cancelled,   // the app asked it to stop
    Offline,     // no name, no connection, no answer: no internet, or GitHub cannot be reached
    Clock,       // the certificate is out of its dates: for GitHub's that means the console's clock
    Certificate, // the certificate is not GitHub's: somebody is in the way, nothing is taken from them
    Server,      // GitHub answered something else than what was asked (a 404, a redirect to another place)
    TooBig,      // more bytes than the limit
    NoMemory,    // no room for the file in memory
    Failed       // anything else: a connection cut in the middle, a TLS error
};

// What to tell of a certificate curl refused, from its error text.
inline NetResult CertificateFailure(const std::string& curl_error) {
    return curl_error.find("BADCERT_FUTURE") != std::string::npos || curl_error.find("BADCERT_EXPIRED") != std::string::npos ? NetResult::Clock : NetResult::Certificate;
}

// Should the question come by itself at this start? Only for a version the user has not already said "not now" to (the row in Settings asks whatever the answer was).
inline bool AskAtStart(const std::string& latest, const std::string& current, const std::string& not_now) {
    return IsNewer(latest, current) && latest != not_now;
}

// ---------------------------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------------------------

constexpr const char* kCheckRowLabel = "Check for updates";
constexpr const char* kCheckRowNote =
    "Asks GitHub whether a newer 3DS Link is out and offers to install it. It needs the WiFi, internet and the right date on the console.";
constexpr const char* kAtStartRowLabel = "Check at start";
constexpr const char* kAtStartRowNote =
    "ON: each time 3DS Link starts it asks GitHub, in the background. You see nothing unless a new version is out, and nothing is installed without your OK. OFF: only from the row above.";

constexpr const char* kClockTitle = "Check the clock";
constexpr const char* kClockText =
    "The clock of this console looks wrong. It is not serious and 3DS Link works as usual, but automatic updates cannot work until the date is right (System Settings).";

constexpr const char* kPromptTitle = "Update available";

inline std::string PromptText(const std::string& latest, const std::string& current) {
    return "3DS Link v" + latest + " is out.\nYou have v" + current + ".\nUpdate now?";
}

// The top screen of the question.
inline std::string PromptTopText(const std::string& whats_new) {
    std::string text;
    if (!whats_new.empty()) {
        text = whats_new + "\n\n";
    }
    return text +
           "3DS Link downloads and installs the new version, then starts again and brings the plugin up to date in all your games by itself. "
           "Wait for its message before you close it, or some games would keep the old plugin.";
}

constexpr const char* kUpToDateTitle = "No update";

inline std::string UpToDateText(const std::string& current) {
    return "You have the latest version: v" + current + ".";
}

inline std::string NoFileText(const std::string& latest) {
    return "3DS Link v" + latest + " is out, but this copy was started without the path of its file, so it cannot replace itself. Get it from github.com/gionko/3DS-Link.";
}

// Why a check or a download did not work (nothing has changed on the console when this is shown).
inline const char* NetFailureText(NetResult r) {
    switch (r) {
    case NetResult::Offline:
        return "GitHub could not be reached. Is this console connected to the internet?";
    case NetResult::Clock:
        return kClockText;
    case NetResult::Certificate:
        return "This connection did not prove to be GitHub, so nothing was taken from it. Try again on another network.";
    case NetResult::Server:
        return "GitHub did not give the expected answer. Try again later.";
    case NetResult::TooBig:
        return "The file on GitHub is far bigger than it should be: it was not taken.";
    case NetResult::NoMemory:
        return "There is not enough memory for the file. Close 3DS Link, open it again and retry.";
    case NetResult::Failed:
        return "The connection to GitHub was cut. Try again.";
    default:
        return "";
    }
}

constexpr const char* kWrongFileText = "What GitHub gave is not the file of this version of 3DS Link: it was not installed.";
constexpr const char* kBusyText = "A check is still running. Try again in a moment.";

inline std::string InstallFailureText(std::uint32_t rc) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(rc));
    return std::string("The console refused the update (") + code + "). You still have the version you had.";
}

// `phrase` is setup::WriteFailurePhrase.
inline std::string FileFailureText(const std::string& phrase) {
    return "The file of 3DS Link could not be replaced: " + phrase + ". You still have the version you had.";
}

inline std::string DoneText(Target target, const std::string& version) {
    if (target == Target::Cia) {
        return "3DS Link v" + version + " is installed.\nPress A: 3DS Link closes and opens again. If it does not, open it from the HOME Menu.\n"
               "Then wait for its message: it updates the plugin in all your games.";
    }
    return "The file of 3DS Link is now v" + version + ".\nPress A to close this app, then open it again from the Homebrew Launcher.\n"
           "Then wait for its message: it updates the plugin in all your games.";
}

} // namespace update
