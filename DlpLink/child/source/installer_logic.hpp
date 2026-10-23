// The decisions of the 3DS Link Installer (the Download Play child) that need no console: how a CIA is read, which result code means what,
// whether 3DS Link has to be installed, updated or left alone, which files the job writes (the plugin, the copy of the app's CIA) and what they cost on the card,
// what the manifest of the package says, when the result screens speak of boot9.bin and which key does what there. Plain C++ with no libctru in it, so
// DlpLink/tests/child_logic_test.cpp builds this very file on a PC against real CIAs (the fixture and the ones tools/make_*.sh produce).
// main.cpp is the part that talks to the console (AM, the SD card, the screens).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace inst {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---------------------------------------------------------------------------------------------
// Our own result codes. They follow the convention of the title delivery receiver (0xC8604C01 = out of memory): level fatal, summary 3,
// module 19 (0xC8604Cxx decodes to RM_HID, descriptions 1..24: they are told apart from the system's own codes by their exact value, which ErrorText
// matches BEFORE it reads any field; what the user sees is the text next to the number, the number is for the log and for us).
// ---------------------------------------------------------------------------------------------
constexpr u32 kErrNoMemory = 0xC8604C01;
constexpr u32 kErrPayload = 0xC8604C10;     // the package is damaged: a file of the RomFS is missing or the wrong size
constexpr u32 kErrNoCia = 0xC8604C11;       // romfs:/3dslink.cia could not be opened
constexpr u32 kErrNotCia = 0xC8604C12;      // ... and it is not a CIA we understand
constexpr u32 kErrShortRead = 0xC8604C13;   // the RomFS ended early
constexpr u32 kErrShortWrite = 0xC8604C14;  // AM or the SD card took fewer bytes than given
constexpr u32 kErrCancelled = 0xC8604C15;   // the system asked the app to close in the middle of the work
constexpr u32 kErrNotVerified = 0xC8604C16; // AM finished but the title is not there with the right version
constexpr u32 kErrNoPlugin = 0xC8604C17;    // romfs:/AzaharLink.3gx is missing or is not a plugin
constexpr u32 kErrPluginWrite = 0xC8604C18; // the plugin could not be written to the SD card
constexpr u32 kErrSdFull = 0xC86044CD;      // FS, description 205: what the file system answers when the card is full
constexpr u32 kErrNotThis = 0xC8604C19;     // the CIA in the package is not the title the manifest names, or the manifest names a title that is not 3DS Link's

// The one title this installer installs: 3DS Link (unique id 0x0F83D1, DlpLink/tools/common.sh; the app's own copy of the constant is kAppTid of
// AzaharLink/ConfigApp/source/dlp_prepare_logic.hpp, and Tests/policy_check.sh holds the two together). The job deletes the title it is about to install, and its ticket,
// before it installs the new one; the manifest and the CIA of the package are read from the child's own RomFS, so a package that names another title (damaged, or made
// by another tool) must not be allowed to delete a game: it is refused whole, whatever the CIA inside says.
constexpr u64 kAppTid = 0x000400000F83D100ull;

inline u32 Be32(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
inline u32 Le32(const u8* p) {
    return p[0] | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}
inline u64 Le64(const u8* p) {
    return static_cast<u64>(Le32(p)) | static_cast<u64>(Le32(p + 4)) << 32;
}
inline u64 Align64(u64 x) {
    return (x + 63) & ~static_cast<u64>(63);
}

// ---------------------------------------------------------------------------------------------
// A CIA: the title and the version its TMD holds. Offsets are the ones of FBI (core/data/cia.c) and of tools/finalize_child.py.
// ---------------------------------------------------------------------------------------------
struct CiaInfo {
    u64 tid = 0;
    u16 version = 0;
    u16 content_count = 0;
    u64 total_size = 0;     // what the header says the whole file adds up to (header, certificates, ticket, TMD, contents, meta)
    u64 tmd_offset = 0;     // where the TMD begins: the caller must have read that far plus kTmdNeeded bytes
};

constexpr u64 kHeaderBytes = 0x2020;
// How much of the TMD the parser looks at: the signature, the header (0xC4 bytes; the title version at +0x9C and the content count at +0x9E).
constexpr u64 kTmdNeeded = 0x400;

// Where the TMD of a CIA starts, from its first 0x20 bytes (0 when they are not a CIA header).
inline u64 TmdOffset(const u8* header, size_t len) {
    if (len < 0x20 || Le32(header) != kHeaderBytes) {
        return 0;
    }
    const u32 cert = Le32(header + 8), ticket = Le32(header + 12), tmd = Le32(header + 16);
    if (cert == 0 || ticket == 0 || tmd == 0 || cert > 0x10000 || ticket > 0x10000 || tmd > 0x100000) {
        return 0;
    }
    return Align64(kHeaderBytes) + Align64(cert) + Align64(ticket);
}

// `buf` holds the first `len` bytes of the file and `file_size` is its size. False when it is not a CIA, or when `buf` is too short to show the TMD.
inline bool ParseCia(const u8* buf, size_t len, u64 file_size, CiaInfo* out) {
    const u64 tmd = TmdOffset(buf, len);
    if (tmd == 0 || tmd + kTmdNeeded > len || tmd + kTmdNeeded > file_size) {
        return false;
    }
    const u32 sig = Be32(buf + tmd);
    u32 sig_size = 0, pad = 0x3C; // RSA signatures are followed by 0x3C bytes of padding, ECDSA by 0x40
    switch (sig) {
    case 0x10000:
    case 0x10003:
        sig_size = 0x200;
        break;
    case 0x10001:
    case 0x10004:
        sig_size = 0x100;
        break;
    case 0x10002:
    case 0x10005:
        sig_size = 0x3C;
        pad = 0x40;
        break;
    default:
        return false;
    }
    const u8* body = buf + tmd + 4 + sig_size + pad;
    CiaInfo info;
    info.tid = static_cast<u64>(Be32(body + 0x4C)) << 32 | Be32(body + 0x50);
    info.version = static_cast<u16>(body[0x9C] << 8 | body[0x9D]);
    info.content_count = static_cast<u16>(body[0x9E] << 8 | body[0x9F]);
    info.tmd_offset = tmd;
    const u32 tmd_size = Le32(buf + 16), meta = Le32(buf + 20);
    const u64 content = Le64(buf + 0x18);
    info.total_size = tmd + Align64(tmd_size) + Align64(content) + meta;
    if (info.content_count == 0 || info.tid == 0 || info.total_size > file_size + 0x40) {
        return false;
    }
    if (out != nullptr) {
        *out = info;
    }
    return true;
}

// "1.4.2" for a version u16 of major << 10 | minor << 4 | micro (the TMD's own notation).
inline std::string VersionText(unsigned version) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", (version >> 10) & 0x3F, (version >> 4) & 0x3F, version & 0xF);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// What to do with 3DS Link on this console.
// ---------------------------------------------------------------------------------------------
enum class Action {
    Install, // not there: install it
    Update,  // an older version is there: install over it
    Keep,    // the same version is there: do not touch the app (it may be a complete build with more in it); the plugin and the copy of the CIA are brought up to date
    Skip     // a newer version is there: touch nothing
};

inline Action Decide(bool installed, unsigned installed_version, unsigned package_version) {
    if (!installed) {
        return Action::Install;
    }
    if (installed_version < package_version) {
        return Action::Update;
    }
    return installed_version == package_version ? Action::Keep : Action::Skip;
}

// How a package differs from what is installed, from the two version numbers (major << 10 | minor << 4 | micro): the decision screen says it in words.
enum class Change { Same, Patch, Minor, Major, Older };

inline Change Compare(unsigned installed, unsigned package) {
    if (package == installed) {
        return Change::Same;
    }
    if (package < installed) {
        return Change::Older;
    }
    if ((package >> 10) != (installed >> 10)) {
        return Change::Major;
    }
    return ((package >> 4) & 0x3F) != ((installed >> 4) & 0x3F) ? Change::Minor : Change::Patch;
}

inline const char* ChangeText(Change c) {
    switch (c) {
    case Change::Same:
        return "the same version";
    case Change::Patch:
        return "a patch";
    case Change::Minor:
        return "a feature update";
    case Change::Major:
        return "a major update";
    default:
        return "an older release";
    }
}

// The copy of the app's CIA that the installer leaves at sdmc:/3ds/3DSLink/dlp/3dslink.cia, for the app to compose the Download Play child from on a console that
// has only Luma (DlpLink/docs/COMPOSITORE.md, source (a)). It is the CIA of the package, byte for byte, so it takes cia_size on the card. Every job that gets as far
// as writing (the app installed, updated or replaced, and the same version kept) leaves it, unless the file there is already the right one. It is written AFTER
// the app is installed and its failure never fails the job: the app is there.
inline bool WritesCopy(Action action, bool copy_same) {
    return action != Action::Skip && !copy_same;
}

// "Nothing to do": the same version is there, the plugin is the right one and so is the copy. No question, no write, no space needed. (A copy that is missing or
// different alone is NOT a question: it is not the user's file, the job just leaves the right one.)
inline bool NothingToWrite(Action action, bool plugin_same, bool copy_same) {
    return action == Action::Keep && plugin_same && copy_same;
}

// The only question the job asks about files that are already there: the same version is installed but its plugin file is missing or is another one. The copy never asks.
inline bool AsksRepair(Action action, bool plugin_same) {
    return action == Action::Keep && !plugin_same;
}

// What the SD card needs for the plugin, the CIA and (write_copy) the copy of the CIA, with a margin for AM (it reserves the space of the contents with its first
// write). `write_copy` is WritesCopy(...): a copy that is already right is not written and not counted.
constexpr u64 kSpareBytes = 16ull << 20;
inline u64 NeedBytes(u64 cia_size, u64 plugin_size, Action action, bool write_copy) {
    const u64 copy = write_copy ? cia_size : 0;
    switch (action) {
    case Action::Install:
    case Action::Update:
        return cia_size + plugin_size + copy + kSpareBytes;
    case Action::Keep:
        return plugin_size + copy + (1ull << 20);
    default:
        return 0;
    }
}

// Does this action put the app through AM (a first install, an update, or the older version over a newer one)? Keep and Skip never touch the app.
inline bool InstallsApp(Action action) {
    return action == Action::Install || action == Action::Update;
}

// The bytes of work the progress bar counts: the plugin (counted even when the card already has the right one: the worker advances over it, so the bar still
// ends at 100%), the app's CIA through AM when it is installed, and the copy of the CIA when it is written (`write_copy` is WritesCopy(...)).
inline u64 WorkBytes(u64 cia_size, u64 plugin_size, Action action, bool write_copy) {
    return plugin_size + (InstallsApp(action) ? cia_size : 0) + (write_copy ? cia_size : 0);
}

// ---------------------------------------------------------------------------------------------
// The keys of the screens (main.cpp feeds them with what the buttons say; the PC test runs the same code)
// ---------------------------------------------------------------------------------------------
// The first answer to a question stands: the worker wakes up to 20 ms after the key, and in those frames a second key (B, then X one frame later) must not turn
// "Leave" into "Install older". 0 = not answered, 1 = go ahead, 2 = leave it as it is; B wins over the yes key when both come in the same frame.
inline int NextDecision(int current, bool leave_pressed, bool yes_pressed) {
    if (current != 0) {
        return current;
    }
    return leave_pressed ? 2 : (yes_pressed ? 1 : 0);
}

// A key pressed in the first moments of a screen was meant for something else: it must not answer a question or take a result off the screen. ANY change of
// what the screen shows starts the count again (a question that is answered with B goes to its result in microseconds: nothing runs between the two).
constexpr unsigned long long kKeyLockMs = 800;
struct Lockout {
    int shown = -1;
    unsigned long long since = 0;
    bool changed = false; // true for the call that saw a new screen
    bool Open(int screen, unsigned long long now_ms) {
        changed = screen != shown;
        if (changed) {
            shown = screen;
            since = now_ms;
        }
        return now_ms - since >= kKeyLockMs;
    }
    void Forget() { shown = -1; } // the next call is a new screen whatever it shows ("Try again": the worker may reach a question or a failure at once)
};

// ---------------------------------------------------------------------------------------------
// The notice about boot9.bin on the result screens, and the key that opens the guide (AzaharLink/ConfigApp/guide, DlpLink/docs/GUIDA-BOOT9.md).
// 3DS Link works without boot9.bin; the file is only needed to SHARE (3DS Link Delivery, Titles Delivery). So the installer never insists: when the job
// went well and the SD card has no boot9.bin, the result screens say so and offer the guide with a key of its own; nothing waits for it.
// ---------------------------------------------------------------------------------------------
// How a job that is over ended, as the screens word it (main.cpp keeps the number in its job record).
enum Outcome { OUT_NONE = 0, OUT_INSTALLED, OUT_UPDATED, OUT_CURRENT, OUT_NEWER, OUT_LEFT, OUT_DOWNGRADED, OUT_REPAIRED };

// The outcomes that leave a 3DS Link working on this console because of this run: installed, updated, replaced by the older one, the plugin restored, or the
// same version that was already there (checked and brought up to date). NOT "a newer one stays" and "left as it is" (nothing was done: the screen says so and
// stops there), and not a failure (that is another screen altogether: the caller passes `done` false).
inline bool LeavesWorkingApp(int outcome) {
    return outcome == OUT_INSTALLED || outcome == OUT_UPDATED || outcome == OUT_DOWNGRADED || outcome == OUT_REPAIRED || outcome == OUT_CURRENT;
}

// What the result screen says about boot9.bin: the top screen gets one line, the bottom screen the explanation, and the footer the key of the guide.
struct Boot9Notice {
    bool top = false, bottom = false, key = false;
    bool any() const { return top || bottom || key; }
};

// `done`: the screen is the finished one (not a question, not the work in progress, not the failure screen). `boot9_found`: guide::Boot9Found() on this card.
// With boot9.bin there, or on any other screen, the installer says nothing about it.
inline Boot9Notice Boot9NoticeFor(bool done, int outcome, bool boot9_found) {
    Boot9Notice n;
    if (done && !boot9_found && LeavesWorkingApp(outcome)) {
        n.top = n.bottom = n.key = true;
    }
    return n;
}

// The buttons, as libctru names them (the bits of hidKeysDown(); main.cpp static_asserts that they are the same numbers, so this file needs no libctru).
constexpr u32 kPadA = 1u << 0, kPadB = 1u << 1, kPadSelect = 1u << 2, kPadStart = 1u << 3, kPadRight = 1u << 4, kPadLeft = 1u << 5, kPadUp = 1u << 6,
              kPadDown = 1u << 7, kPadR = 1u << 8, kPadL = 1u << 9, kPadX = 1u << 10, kPadY = 1u << 11;
// The key of the guide on the result screen. (The footer shows it: "Y boot9.bin guide".)
constexpr u32 kGuideKey = kPadY;
// The screen the guide is, for the key lockout: not a phase of the job.
constexpr int kScreenGuide = 100;

enum class ResultKey { None, Leave, Guide };
// The buttons that take the user off a finished screen: A ("Go to HOME" in the footer) and B (back). Every other button of the pad does nothing there: up to
// 1.0.1 EVERY button left ("press any button"), and a user who pressed X for Y was thrown to the HOME menu (the console test of 2026-10-22).
constexpr u32 kLeavePads = kPadA | kPadB;

// What a press does on a screen that waits for the user without asking anything: the finished screen (`done`: A and B leave) or the failure screen of a job
// that cannot be tried again (only B leaves: A is not on its footer). `guide_offered`: the footer shows the guide key (a notice is up and the guide can be
// opened): then that key opens the guide, whatever else was pressed in the same frame (a real console reports a touch of a neighbouring button; the
// guide is harmless and B closes it).
inline ResultKey ResultKeyFor(u32 down, bool done, bool guide_offered) {
    if (done && guide_offered && (down & kGuideKey) != 0) {
        return ResultKey::Guide;
    }
    const u32 leave = done ? kLeavePads : kPadB;
    return (down & leave) != 0 ? ResultKey::Leave : ResultKey::None;
}

// A .3gx plugin starts with the four bytes "3GX$" (the same test the app makes in system.cpp).
inline bool PluginHeaderOk(const u8* first, size_t len) {
    return len >= 4 && std::memcmp(first, "3GX$", 4) == 0;
}

// ---------------------------------------------------------------------------------------------
// manifest.txt, written by tools/make_dlp_child.sh:
//   version=1.4.2
//   version16=1090
//   tid=000400000F83D100
//   3dslink.cia=<bytes> <sha256>
//   AzaharLink.3gx=<bytes> <sha256>
// ---------------------------------------------------------------------------------------------
struct Manifest {
    std::string version;
    u32 version16 = 0;
    u64 tid = 0;        // the title of 3dslink.cia: the installer installs this one and nothing else
    u64 cia_size = 0;
    u64 plugin_size = 0;
    bool has_version16 = false, has_tid = false, has_cia = false, has_plugin = false;
    bool complete() const {
        return has_version16 && has_tid && tid != 0 && has_cia && has_plugin && cia_size > 0 && plugin_size > 0;
    }
};

// The manifest names 3DS Link's own title and nothing else (what kAppTid says): the first thing the job checks, before it looks at the CIA.
inline bool NamesApp(const Manifest& m) {
    return m.tid == kAppTid;
}

inline Manifest ParseManifest(const std::string& text) {
    Manifest m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "version") {
            m.version = value;
        } else if (key == "version16") {
            m.version16 = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
            m.has_version16 = !value.empty();
        } else if (key == "tid") {
            m.tid = std::strtoull(value.c_str(), nullptr, 16);
            m.has_tid = !value.empty();
        } else if (key == "3dslink.cia") {
            m.cia_size = std::strtoull(value.c_str(), nullptr, 10);
            m.has_cia = true;
        } else if (key == "AzaharLink.3gx") {
            m.plugin_size = std::strtoull(value.c_str(), nullptr, 10);
            m.has_plugin = true;
        }
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// What a result code means, in a sentence for the screen. The number is always shown next to it (0x%08X); this only chooses the words from the
// fields of the code (level << 27 | summary << 21 | module << 10 | description), never from the whole value.
// ---------------------------------------------------------------------------------------------
constexpr u32 kModuleFs = 17;
constexpr u32 kModuleAm = 32;
inline u32 RcModule(u32 rc) {
    return (rc >> 10) & 0xFF;
}
inline u32 RcDescription(u32 rc) {
    return rc & 0x3FF;
}

// What AM answers for a title that is not installed: 0xD8A083FA, AM description 1018. The project's hardware logs show it for AM_DeleteTitle and
// AM_DeletePendingTitle of a title that is not there, Azahar answers it to AM_GetTitleInfo too, and 3dbrew gives the same module and description; for
// AM_GetTitleInfo on hardware it is not in a log yet (the installer's own log will say). Any OTHER failure of that call means the state of the console
// is not known, and then nothing is deleted (if the answer on hardware were another one, the cost is a '(could not be read)' row and no delete).
inline bool IsTitleNotFound(u32 rc) {
    return rc == 0xD8A083FAu || (RcModule(rc) == kModuleAm && RcDescription(rc) == 1018);
}

inline std::string ErrorText(u32 rc) {
    switch (rc) {
    case kErrNoMemory:
        return "Not enough memory.";
    case kErrPayload:
    case kErrNoCia:
    case kErrNotCia:
    case kErrShortRead:
    case kErrNoPlugin:
    case kErrNotThis:
        return "The package is damaged. Download it again.";
    case kErrShortWrite:
    case kErrPluginWrite:
        return "The SD card could not be written.";
    case kErrCancelled:
        return "The installation was interrupted.";
    case kErrNotVerified:
        return "The installation could not be verified.";
    case 0xD8E06406u: // sm: access denied
    case 0xD0406401u: // sm: service not registered
        return "This console refused access to the installer service.";
    default:
        break;
    }
    const u32 module = RcModule(rc), desc = RcDescription(rc);
    if (module == kModuleFs) {
        if (desc >= 200 && desc <= 229) { // 3dbrew: the descriptions of a card that is full; an AM write that meets it answers at the top of the range
            return "Not enough space on the SD card. Free some space and try again.";
        }
        if (desc >= 130 && desc <= 179) {
            return "The SD card is not available.";
        }
    }
    if (module == kModuleAm) {
        if (desc == 43 || desc == 393) {
            return "SD title database not ready. Open HOME once with the card in, then try again.";
        }
        if (desc == 106 || desc == 37 || desc == 4 || desc == 32 || desc == 39) {
            return "The package is damaged. Download it again.";
        }
    }
    return "Something went wrong.";
}

} // namespace inst
