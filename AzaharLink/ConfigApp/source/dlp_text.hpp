// 3DS Link Delivery (the Download Play host): the words of its screens and the few decisions that need no console, in plain C++ so that
// Tests/settings_test/dlp_text_test.cpp builds this very file on a PC. The page that draws them is pages_dlp.cpp; the library that does the
// work on the air is dlph/ (a copy of DlpLink/host, see dlph/README.md).
#pragma once

#include <cstdio>
#include <string>

namespace dlp {

// The version of the child as its TMD holds it, a u16 of major << 10 | minor << 4 | micro: 1024 is "1.0.0". 0 (no child read) is "".
inline std::string VersionText(unsigned version) {
    if (version == 0) {
        return std::string();
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", (version >> 10) & 0x3F, (version >> 4) & 0x3F, version & 0xF);
    return buf;
}

// What the console in the room is called on the screen: its user name, or "Console" while the library has not read it yet (it does a moment after
// the console comes in, 26-70 ms on the hardware) and the seat it took.
inline std::string ConsoleName(const char* name, int seat) {
    if (name != nullptr && name[0] != '\0') {
        return name;
    }
    return seat > 0 ? "Console (seat " + std::to_string(seat) + ")" : std::string("Console");
}

// What became of a console that was in the room (the library's dlph_result: 0 served, 1 lost, 2 sent away).
enum class Tone { Good, Bad, Warn, Dim };

inline const char* VisitWord(int result, bool already_had) {
    if (already_had) {
        return "Had it already"; // it said it has this version: nothing was sent
    }
    switch (result) {
    case 0:
        return "Delivered"; // (the host sees the download end; the installation is on the other console and it does not see it)
    case 1:
        return "Lost";
    case 2:
        return "Sent away";
    default:
        return "?";
    }
}

inline Tone VisitTone(int result, bool already_had) {
    if (already_had) {
        return Tone::Dim;
    }
    return result == 0 ? Tone::Good : (result == 1 ? Tone::Bad : Tone::Warn);
}

// "about 8 s left", "about 2 min left"; -1 is the library saying it does not know yet (the first second and a half of a download).
inline std::string TimeLeftText(int eta_s) {
    if (eta_s < 0) {
        return "working out the time";
    }
    if (eta_s < 90) {
        return "about " + std::to_string(eta_s < 1 ? 1 : eta_s) + " s left";
    }
    return "about " + std::to_string((eta_s + 30) / 60) + " min left";
}

// "12 s", "1 min 05 s", "2 h 03 min": how long a console has been in the room or took.
inline std::string DurationText(unsigned ms) {
    const unsigned s = (ms + 500) / 1000;
    char buf[32];
    if (s < 60) {
        std::snprintf(buf, sizeof(buf), "%u s", s);
    } else if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%u min %02u s", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%u h %02u min", s / 3600, (s % 3600) / 60);
    }
    return buf;
}

// "0.46 MiB/s".
inline std::string SpeedText(float mibps) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f MiB/s", static_cast<double>(mibps < 0.0f ? 0.0f : mibps));
    return buf;
}

// "3 of 182 packets sent again" (the fragments the console asked for a second time): the share of what the air carried twice.
inline std::string RepeatedText(unsigned repeated, unsigned total) {
    return std::to_string(repeated) + " of " + std::to_string(total) + " packets sent again";
}

// The MAC address of this console is part of the checksum of what is announced: without the right one no console sees the room. All zero (not
// read) and all ones are not addresses of a console.
inline bool MacUsable(const unsigned char mac[6]) {
    bool zero = true, ones = true;
    for (int i = 0; i < 6; ++i) {
        zero = zero && mac[i] == 0x00;
        ones = ones && mac[i] == 0xFF;
    }
    return !zero && !ones;
}

inline std::string MacText(const unsigned char mac[6]) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

// Closing the room cuts off a console that is in it, so the page asks first; with nobody in it closes at once.
inline bool CloseNeedsQuestion(int seat) {
    return seat != 0;
}

constexpr const char* kCloseQuestion = "A console that has not finished will stop receiving.";

// Why the room is not open (or why the package it hands out could not be made first). The title is one line, the text what to do about it.
enum class Failure { None, NoChild, BadChild, NoMac, Wireless, Library, Room, Thread, SwitchedOff, NoBoot9, NoModel, NoPlugin, NoSource, NoKeys, BadAppFile, Prepare };

inline const char* FailureTitle(Failure f) {
    switch (f) {
    case Failure::NoChild:
        return "child.cia not found";
    case Failure::BadChild:
        return "child.cia is damaged";
    case Failure::NoMac:
        return "This console's address is not known";
    case Failure::Wireless:
        return "The wireless is not available";
    case Failure::Library:
        return "The room could not be prepared";
    case Failure::Room:
        return "The room could not be opened";
    case Failure::Thread:
        return "Not enough memory to open the room";
    case Failure::SwitchedOff:
        return "3DS Link Delivery is switched off";
    case Failure::NoBoot9:
        return "boot9.bin is missing";
    case Failure::NoModel:
        return "This build cannot make the package";
    case Failure::NoPlugin:
        return "This build has no plugin";
    case Failure::NoSource:
        return "3DS Link's own package is missing";
    case Failure::NoKeys:
        return "The key of the SD card is missing";
    case Failure::BadAppFile:
        return "3DSLink.cia cannot be used";
    case Failure::Prepare:
        return "The package could not be prepared";
    default:
        return "";
    }
}

// The app's own copy (dlp/3dslink.cia) is there and cannot be used, and the installed 3DS Link cannot be read either (no key, or no installed title): the sentence
// names the file, says what is wrong with it, and gives the two ways out (the right file there; or the installed 3DS Link and boot9.bin). `found` is the version
// its TMD says ("" = none could be read: damaged, or no CIA of 3DS Link; or it is this version and the composer refused it), `wanted` the version of this app.
inline std::string BadAppFileText(const std::string& file, const std::string& found, const std::string& wanted) {
    std::string s = "The file " + file;
    if (found.empty() || found == wanted) {
        s += " is damaged, or it is of another version.";
    } else {
        s += " is of another version (it is " + found + ", this app is " + wanted + ").";
    }
    return s + " Put the 3DSLink.cia of this version there. Or install 3DS Link as a CIA and keep boot9.bin on the card: it then reads its own copy.";
}

// `detail` is what the page knows (the path of the file, the line the library wrote last, the sentence that says what went wrong while the package was
// made); empty is fine. For NoSource and NoKeys it is the folder (the dlp folder of 3DS Link) that the sentence names; for BadAppFile and Prepare it is the
// whole sentence (BadAppFileText, the work's own).
inline std::string FailureText(Failure f, const std::string& detail) {
    std::string text;
    switch (f) {
    case Failure::SwitchedOff:
        text = "It is switched off in Settings (Download Play delivery). Switch it on there to use it.";
        break;
    case Failure::NoBoot9:
        text = "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link works without it: it is only needed to share. "
               "Press Y to see how to get it.";
        break;
    case Failure::NoModel:
        text = "This copy of 3DS Link was built without the installer that other consoles download, so it cannot make the package. "
               "Install the 3DS Link CIA again.";
        break;
    case Failure::NoPlugin:
        text = "The package that other consoles download carries the plugin, and this copy of 3DS Link was built without the plugin inside. "
               "Install the 3DS Link CIA again.";
        break;
    case Failure::NoSource:
        return "Put 3DSLink.cia, the file 3DS Link was installed from, in " + (detail.empty() ? std::string("the dlp folder of 3DS Link") : detail) +
               ". Or install 3DS Link as a CIA and keep boot9.bin on the card: it then reads its own copy.";
    case Failure::NoKeys:
        return "3DS Link reads its own copy from its installation, and for that it needs the key of this console's SD card: boot9.bin and this "
               "console's movable.sed. boot9.bin is there, but the key could not be made. Or put 3DSLink.cia in " +
               (detail.empty() ? std::string("the dlp folder of 3DS Link") : detail) + ".";
    case Failure::NoChild:
        text = "The package that other consoles download is not in the dlp folder of 3DS Link yet.";
        break;
    case Failure::BadChild:
        text = "The package that other consoles download cannot be used: it is not whole, or it is not a Download Play package. "
               "A makes it again.";
        break;
    case Failure::NoMac:
        text = "The wireless address of this console could not be read, and without it no console can see the room.";
        break;
    case Failure::Wireless:
        text = "The local wireless could not be started. Is the Wi-Fi switched on? It is the switch on the side of the console.";
        break;
    case Failure::Library:
        text = "The room could not be prepared.";
        break;
    case Failure::Room:
        text = "The room could not be opened. Switch the Wi-Fi off and on, and try again.";
        break;
    case Failure::Thread:
        text = "Close other applications and try again.";
        break;
    default:
        break;
    }
    if (!detail.empty()) {
        text += (text.empty() ? "" : " ") + detail;
    }
    return text;
}

// What the person at the other console has to do: shown on the top screen while nobody is in the room. `host` is the name the other console
// will see in the list of Download Play (the user name of this console).
constexpr int kStepCount = 5;

inline std::string StepText(int index, const std::string& host) {
    switch (index) {
    case 0:
        return "On the other console open Download Play and choose Nintendo 3DS.";
    case 1:
        return "Pick 3DS Link" + (host.empty() ? std::string() : " from " + host) + ". About 12 s to download, a few more to install.";
    case 2:
        return "The other console needs Luma3DS.";
    case 3:
        return "One console at a time. If it fails, wait a few seconds and try again.";
    case 4:
        return "It needs about 16 MiB free on its SD card and its Wi-Fi on. Keep both consoles open.";
    default:
        return std::string();
    }
}

// ---------------------------------------------------------------------------------------------
// The entry on the main page: when it is open, and when and how it is shut (the same as Titles Delivery's Send Titles without boot9.bin: faint, a word at the
// right of the row, a note that says why, and A only says it again in a line). Sharing needs boot9.bin; 3DS Link works without it, and says so.
// ---------------------------------------------------------------------------------------------

enum class Gate { Open, Off, NoBoot9 };

// `switch_on`: Settings > Download Play delivery (the emergency switch). It comes first: switched off, nothing else about the entry is looked at.
inline Gate GateOf(bool switch_on, bool boot9_found) {
    if (!switch_on) {
        return Gate::Off;
    }
    return boot9_found ? Gate::Open : Gate::NoBoot9;
}

inline const char* GateValue(Gate g) {
    return g == Gate::Off ? "switched off" : (g == Gate::NoBoot9 ? "needs boot9.bin" : "");
}

inline const char* GateNoteTitle(Gate g) {
    return g == Gate::Off ? "3DS Link Delivery is switched off" : (g == Gate::NoBoot9 ? "boot9.bin is missing" : "3DS Link Delivery");
}

inline std::string GateNote(Gate g) {
    switch (g) {
    case Gate::Off:
        return "It is switched off in Settings (Download Play delivery). Nothing of it runs, and nothing is read or written for it, until you switch it on "
               "again.";
    case Gate::NoBoot9:
        return "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link itself works without it.\n\nPress Y to see, step by step, how to get it.";
    default:
        return "Gives 3DS Link to a console that has only Download Play. It installs and starts it by itself. One console at a time.\n\n"
               "The first time it builds the package, and says how long.";
    }
}

// What A says on the shut entry, in a line.
inline const char* GateToast(Gate g) {
    return g == Gate::Off ? "Switched off: Settings > Download Play delivery." : "boot9.bin is missing: press Y to see how to get it.";
}

// The key that opens the guide "How to get boot9.bin", as a footer's hint shows it (short: the room between the buttons is small), and what is said when this
// build has no pictures to show.
constexpr const char* kGuideHint = "Y guide";
constexpr const char* kGuideLabel = "Guide";
constexpr const char* kNoGuideToast = "The guide is not in this build.";

// Settings > Download Play delivery: the one switch of the feature. `on` is what it says now.
constexpr const char* kSwitchLabel = "Download Play delivery";
constexpr const char* kSwitchNoteTitle = "Download Play delivery";
constexpr const char* kSwitchNote =
    "ON: the main page offers 3DS Link Delivery, which lets a console that has only Download Play get 3DS Link from this one.\n"
    "OFF: the whole feature is shut. Its entry is faint and nothing of it runs or is read or written. Use it if that part ever misbehaves: the rest of 3DS "
    "Link is not affected.";

inline const char* SwitchToast(bool on) {
    return on ? "3DS Link Delivery is on." : "3DS Link Delivery is switched off.";
}

// ---------------------------------------------------------------------------------------------
// The one-time work before the room: the package that other consoles download is built from 3DS Link's own CIA and the plugin (dlp_prepare_logic.hpp)
// ---------------------------------------------------------------------------------------------

constexpr const char* kPrepareTitle = "Preparing the package";

// `seconds` is the time of the plan (0 = not known); `measured` says that the pace of the work itself has been seen and `seconds` is the pace's, no longer the plan's.
// The speeds the plan is made with were never measured on a console, so until the first pace is in the time is worded as a first guess.
inline std::string PrepareIntro(int seconds, bool measured) {
    std::string s = "One time only: 3DS Link builds the package that other consoles download.";
    if (seconds > 0) {
        const std::string time = DurationText(static_cast<unsigned>(seconds) * 1000u);
        s += measured ? " It takes about " + time + "." : " It should take about " + time + ", and it can take longer.";
    }
    return s + " It is built again only when 3DS Link is updated.";
}

constexpr const char* kPrepareCopyStage = "Reading 3DS Link's own copy";
constexpr const char* kPrepareWaitScan = "Waiting for the list of your titles";
constexpr const char* kPrepareWaitScanNote = "3DS Link reads its own copy from its installation, and the key for that comes with the list of titles. It goes on by itself.";
constexpr const char* kPrepareKeepAwake = "B stops it. Nothing is left half done. HOME and sleep are off until it ends.";
// (the popup's title is "Stop now?": the text does not ask it again)
constexpr const char* kCancelPrepareQuestion = "Nothing is left half done, and the work starts again the next time you open this page.";

// The question when a run that was building the package did not end (the marker was still there): `died` is how many runs in a row did not.
inline std::string DiedQuestion(int died) {
    if (died >= 2) {
        return "The last " + std::to_string(died) + " preparations in a row did not finish. If it keeps happening, switch Download Play delivery off in Settings: "
               "3DS Link works without it. Try again?";
    }
    return "The last preparation did not finish: the console was switched off, or the app stopped. Try again?";
}

// What the screen says when the work went wrong: the composer's phrase made a sentence, and what to do about it (dlp_prepare_logic.hpp).
inline std::string PrepareFailedText(const std::string& sentence, const std::string& advice) {
    return sentence.empty() ? advice : (advice.empty() ? sentence : sentence + " " + advice);
}

} // namespace dlp
